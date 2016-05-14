/*
      This file is part of Smoothie (http://smoothieware.org/). The motion control part is heavily based on Grbl (https://github.com/simen/grbl).
      Smoothie is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
      Smoothie is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
      You should have received a copy of the GNU General Public License along with Smoothie. If not, see <http://www.gnu.org/licenses/>.
*/


#include "StepTicker.h"

#include "libs/nuts_bolts.h"
#include "libs/Module.h"
#include "libs/Kernel.h"
#include "StepperMotor.h"
#include "StreamOutputPool.h"
#include "Block.h"

#include "system_LPC17xx.h" // mbed.h lib
#include <math.h>
#include <mri.h>

#ifdef STEPTICKER_DEBUG_PIN
#include "gpio.h"
extern GPIO stepticker_debug_pin;
#endif

StepTicker *StepTicker::instance;

StepTicker::StepTicker()
{
    instance = this; // setup the Singleton instance of the stepticker

    // Configure the timer
    LPC_TIM0->MR0 = 10000000;       // Initial dummy value for Match Register
    LPC_TIM0->MCR = 3;              // Match on MR0, reset on MR0, match on MR1
    LPC_TIM0->TCR = 0;              // Disable interrupt

    LPC_SC->PCONP |= (1 << 2);      // Power Ticker ON
    LPC_TIM1->MR0 = 1000000;
    LPC_TIM1->MCR = 1;
    LPC_TIM1->TCR = 0;              // Disable interrupt

    // Default start values
    this->set_frequency(100000);
    this->set_unstep_time(100);

    this->unstep.reset();
    this->num_motors = 0;

    this->move_issued = false;
    this->next_block = nullptr;
}

StepTicker::~StepTicker()
{
}

//called when everything is setup and interrupts can start
void StepTicker::start()
{
    NVIC_EnableIRQ(TIMER0_IRQn);     // Enable interrupt handler
    NVIC_EnableIRQ(TIMER1_IRQn);     // Enable interrupt handler
}

// Set the base stepping frequency
void StepTicker::set_frequency( float frequency )
{
    this->frequency = frequency;
    this->period = floorf((SystemCoreClock / 4.0F) / frequency); // SystemCoreClock/4 = Timer increments in a second
    LPC_TIM0->MR0 = this->period;
    if( LPC_TIM0->TC > LPC_TIM0->MR0 ) {
        LPC_TIM0->TCR = 3;  // Reset
        LPC_TIM0->TCR = 1;  // Reset
    }
}

// Set the reset delay
void StepTicker::set_unstep_time( float microseconds )
{
    uint32_t delay = floorf((SystemCoreClock / 4.0F) * (microseconds / 1000000.0F)); // SystemCoreClock/4 = Timer increments in a second
    LPC_TIM1->MR0 = delay;
}

// Reset step pins on any motor that was stepped
void StepTicker::unstep_tick()
{
    for (int i = 0; i < num_motors; i++) {
        if(this->unstep[i]) {
            this->motor[i]->unstep();
        }
    }
    this->unstep.reset();
}

extern "C" void TIMER1_IRQHandler (void)
{
    LPC_TIM1->IR |= 1 << 0;
    StepTicker::getInstance()->unstep_tick();
}

// The actual interrupt handler where we do all the work
extern "C" void TIMER0_IRQHandler (void)
{
    StepTicker::getInstance()->TIMER0_IRQHandler();
}

extern "C" void PendSV_Handler(void)
{
    StepTicker::getInstance()->PendSV_IRQHandler();
}

// slightly lower priority than TIMER0, the whole end of block/start of block is done here allowing the timer to continue ticking
void StepTicker::PendSV_IRQHandler (void)
{

    if(this->do_move_finished.load() > 0) {
        this->do_move_finished--;
#ifdef STEPTICKER_DEBUG_PIN
        stepticker_debug_pin = 1;
#endif

    // all moves finished signal block is finished
    if(finished_fnc) finished_fnc();

#ifdef STEPTICKER_DEBUG_PIN
        stepticker_debug_pin = 0;
#endif
    }
}


// step clock
void StepTicker::TIMER0_IRQHandler (void)
{
    static uint32_t current_tick = 0;

    // Reset interrupt register
    LPC_TIM0->IR |= 1 << 0;

    if(!move_issued) return; // if nothing has been setup we ignore the ticks

    current_tick++; // count number of ticks

    bool still_moving = false;

    // foreach motor, if it is active see if time to issue a step to that motor
    for (uint8_t m = 0; m < num_motors; m++) {
        if(tick_info[m].steps_to_move == 0) continue; // not active

        still_moving = true;
        tick_info[m].steps_per_tick += tick_info[m].acceleration_change;

        if(current_tick == tick_info[m].next_accel_event) {
            if(current_tick == block_info.accelerate_until) { // We are done accelerating, deceleration becomes 0 : plateau
                tick_info[m].acceleration_change = 0;
                if(block_info.decelerate_after < block_info.total_move_ticks) {
                    tick_info[m].next_accel_event = block_info.decelerate_after;
                    if(current_tick != block_info.decelerate_after) { // We start decelerating
                        tick_info[m].steps_per_tick = (tick_info[m].axis_ratio * block_info.maximum_rate) / frequency; // steps/sec / tick frequency to get steps per tick
                    }
                }
            }

            if(current_tick == block_info.decelerate_after) { // We start decelerating
                tick_info[m].acceleration_change = -block_info.deceleration_per_tick * tick_info[m].axis_ratio;
            }
        }

        // protect against rounding errors and such
        if(tick_info[m].steps_per_tick <= 0) {
            tick_info[m].counter = 1.0F; // we complete this step
            tick_info[m].steps_per_tick = 0;
        }

        tick_info[m].counter += tick_info[m].steps_per_tick;

        if(tick_info[m].counter >= 1.0F) { // step time
            tick_info[m].counter -= 1.0F;
            ++tick_info[m].step_count;

            // step the motor
            motor[m]->step();
            // we stepped so schedule an unstep
            unstep.set(m);

            if(tick_info[m].step_count == tick_info[m].steps_to_move) {
                // done
                tick_info[m].steps_to_move = 0;
            }
        }
    }

    // We may have set a pin on in this tick, now we reset the timer to set it off
    // Note there could be a race here if we run another tick before the unsteps have happened,
    // right now it takes about 3-4us but if the unstep were near 10uS or greater it would be an issue
    // also it takes at least 2us to get here so even when set to 1us pulse width it will still be about 3us
    if( unstep.any()) {
        LPC_TIM1->TCR = 3;
        LPC_TIM1->TCR = 1;
    }

    if(!still_moving) {
        current_tick = 0;

        // get next static block and tick info from next block
        // do it here so there is no delay in ticks
        if(next_block != nullptr) {
            // copy data
            copy_block(next_block);
            next_block = nullptr;

        } else {
            move_issued = false; // nothing to do as no more blocks
        }

        // all moves finished
        // we delegate the slow stuff to the pendsv handler which will run as soon as this interrupt exits
        //NVIC_SetPendingIRQ(PendSV_IRQn); this doesn't work
        SCB->ICSR = 0x10000000; // SCB_ICSR_PENDSVSET_Msk;
    }
}

// called in ISR if running, else can be called from anything to start
void StepTicker::copy_block(Block *block)
{
    block_info.accelerate_until = block->accelerate_until;
    block_info.decelerate_after = block->decelerate_after;
    block_info.maximum_rate = block->maximum_rate;
    block_info.deceleration_per_tick = block->deceleration_per_tick;
    block_info.total_move_ticks = block->total_move_ticks;

    float inv = 1.0F / block->steps_event_count;
    for (uint8_t m = 0; m < num_motors; m++) {
        uint32_t steps = block->steps[m];
        tick_info[m].steps_to_move = steps;
        if(steps == 0) continue;

        // set direction bit here
        motor[m]->set_direction(block->direction_bits[m]);

        float aratio = inv * steps;
        tick_info[m].steps_per_tick = (block->initial_rate * aratio) / frequency; // steps/sec / tick frequency to get steps per tick; // 2.30 fixed point
        tick_info[m].counter = 0; // 2.30 fixed point
        tick_info[m].axis_ratio = aratio;
        tick_info[m].step_count = 0;
        tick_info[m].next_accel_event = block->total_move_ticks + 1;
        tick_info[m].acceleration_change = 0;
        if(block->accelerate_until != 0) { // If the next accel event is the end of accel
            tick_info[m].next_accel_event = block->accelerate_until;
            tick_info[m].acceleration_change = block->acceleration_per_tick;

        } else if(block->decelerate_after == 0 /*&& block->accelerate_until == 0*/) {
            // we start off decelerating
            tick_info[m].acceleration_change = -block->deceleration_per_tick;

        } else if(block->decelerate_after != block->total_move_ticks /*&& block->accelerate_until == 0*/) {
            // If the next event is the start of decel ( don't set this if the next accel event is accel end )
            tick_info[m].next_accel_event = block->decelerate_after;
        }
        tick_info[m].acceleration_change *= aratio;
    }
    move_issued = true;
}

// returns index of the stepper motor in the array and bitset
int StepTicker::register_motor(StepperMotor* m)
{
    motor[num_motors++] = m;
    return num_motors - 1;
}
