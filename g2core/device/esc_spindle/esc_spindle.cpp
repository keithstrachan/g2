/*
 * esc_spindle.cpp - toolhead driver for a ESC-driven brushless spindle
 * This file is part of the g2core project
 *
 * Copyright (c) 2019 Robert Giseburt
 * Copyright (c) 2019 Alden S. Hart, Jr.
 *
 * This file ("the software") is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as published by the
 * Free Software Foundation. You should have received a copy of the GNU General Public
 * License, version 2 along with the software.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, you may use this file as part of a software library without
 * restriction. Specifically, if other files instantiate templates or use macros or
 * inline functions from this file, or you compile this file and link it with  other
 * files to produce an executable, this file does not by itself cause the resulting
 * executable to be covered by the GNU General Public License. This exception does not
 * however invalidate any other reasons why the executable file might be covered by the
 * GNU General Public License.
 *
 * THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY
 * WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#if 0

#include "g2core.h"             // #1 dependency order
#include "config.h"             // #2
#include "canonical_machine.h"  // #3
#include "text_parser.h"        // #4

#include "gpio.h"
#include "esc_spindle.h"
#include "planner.h"
#include "hardware.h"
#include "pwm.h"
#include "util.h"

/**** Allocate structures ****/

#define SPINDLE_OVERRIDE_ENABLE false
#define SPINDLE_OVERRIDE_FACTOR 1.00
#define SPINDLE_OVERRIDE_MIN 0.05       // 5%
#define SPINDLE_OVERRIDE_MAX 2.00       // 200%
#define SPINDLE_OVERRIDE_RAMP_TIME 1    // change sped in seconds

enum spMode {
    SPINDLE_DISABLED = 0,       // spindle will not operate
    SPINDLE_PLAN_TO_STOP,       // spindle operating, plans to stop
    SPINDLE_CONTINUOUS,         // spindle operating, does not plan to stop
};
#define SPINDLE_MODE_MAX SPINDLE_CONTINUOUS

// *** NOTE: The spindle polarity active hi/low values currently agree with ioMode in gpio.h
// These will all need to be changed to ACTIVE_HIGH = 0, ACTIVE_LOW = 1
// See: https://github.com/synthetos/g2_private/wiki/GPIO-Design-Discussion#settings-common-to-all-io-types

enum spPolarity {                  // Note: These values agree with
    SPINDLE_ACTIVE_LOW = 0,     // Will set output to 0 to enable the spindle or CW direction
    SPINDLE_ACTIVE_HIGH = 1,    // Will set output to 1 to enable the spindle or CW direction
};

enum ESCState {                  // electronic speed controller for some spindles
    ESC_ONLINE = 0,
    ESC_OFFLINE,
    ESC_LOCKOUT,
    ESC_REBOOTING,
    ESC_LOCKOUT_AND_REBOOTING,
};

/*
 * Spindle control structure
 */

enum spState {                  // how spindle states are represented internally
    SPINDLE_STATE_OFF = 0,      // OFF - startup condition
    SPINDLE_STATE_PAUSED,       // Paused - was on, is still holding properties for when it's resumed
    SPINDLE_STATE_SPINUP,       // Spinning up - in the process of going to RUN
    SPINDLE_STATE_RUN,          // Running - all parameters are as requested
    SPINDLE_STATE_SPINDOWN,     // Spining down - on the way to paused
};

struct spSpindle_t {
    spControl   state;              // {spc:} OFF, ON, PAUSE, RESUME, WAIT
    spDirection direction;          //        1=CW, 2=CCW (subset of above state)

    float       speed;              // {sps:}  S in RPM
    float       speed_min;          // {spsn:} minimum settable spindle speed
    float       speed_max;          // {spsm:} maximum settable spindle speed
    float       speed_actual;       // hidden internal value used in speed ramping
    float    speed_change_per_tick; // hidden internal value used in speed ramping

    spPolarity  enable_polarity;    // {spep:} 0=active low, 1=active high
    spPolarity  dir_polarity;       // {spdp:} 0=clockwise low, 1=clockwise high
    bool        pause_enable;       // {spph:} pause on feedhold
    float       spinup_delay;       // {spde:} optional delay on spindle start (set to 0 to disable)

    bool        override_enable;    // {spoe:} TRUE = spindle speed override enabled (see also m48_enable in canonical machine)
    float       override_factor;    // {spo:}  1.0000 x S spindle speed. Go up or down from there
};

spSpindle_t spindle;


gpioDigitalOutput *spindle_enable_output = nullptr;
gpioDigitalOutput *spindle_direction_output = nullptr;

#ifndef SPINDLE_ENABLE_OUTPUT_NUMBER
#warning SPINDLE_ENABLE_OUTPUT_NUMBER is defaulted to 4!
#warning SPINDLE_ENABLE_OUTPUT_NUMBER should be defined in settings or a board file!
#define SPINDLE_ENABLE_OUTPUT_NUMBER 4
#endif

#ifndef SPINDLE_DIRECTION_OUTPUT_NUMBER
#warning SPINDLE_DIRECTION_OUTPUT_NUMBER is defaulted to 5!
#warning SPINDLE_DIRECTION_OUTPUT_NUMBER should be defined in settings or a board file!
#define SPINDLE_DIRECTION_OUTPUT_NUMBER 5
#endif

/**** Static functions ****/

static float _get_spindle_pwm (spSpindle_t &_spindle, pwmControl_t &_pwm);

#define SPINDLE_DIRECTION_ASSERT \
    if ((spindle.direction < SPINDLE_CW) || (spindle.direction > SPINDLE_CCW)) { \
         spindle.direction = SPINDLE_CW; \
    }

#ifndef SPINDLE_SPEED_CHANGE_PER_MS
#define SPINDLE_SPEED_CHANGE_PER_MS 0
#endif

/****************************************************************************************
 * spindle_init()
 * spindle_reset() - stop spindle, set speed to zero, and reset values
 */
void spindle_init()
{
    SPINDLE_DIRECTION_ASSERT        // spindle needs an initial direction

    if (SPINDLE_ENABLE_OUTPUT_NUMBER > 0) {
        spindle_enable_output = d_out[SPINDLE_ENABLE_OUTPUT_NUMBER-1];
        spindle_enable_output->setEnabled(IO_ENABLED);
        spindle_enable_output->setPolarity((ioPolarity)SPINDLE_ENABLE_POLARITY);
    }
    if (SPINDLE_DIRECTION_OUTPUT_NUMBER > 0) {
        spindle_direction_output = d_out[SPINDLE_DIRECTION_OUTPUT_NUMBER-1];
        spindle_direction_output->setEnabled(IO_ENABLED);
        spindle_direction_output->setPolarity((ioPolarity)SPINDLE_DIR_POLARITY);
    }

    if( pwm.c[PWM_1].frequency < 0 ) {
        pwm.c[PWM_1].frequency = 0;
    }
    pwm_set_freq(PWM_1, pwm.c[PWM_1].frequency);
    pwm_set_duty(PWM_1, pwm.c[PWM_1].phase_off);

    spindle.speed_change_per_tick = SPINDLE_SPEED_CHANGE_PER_MS;
}

void spindle_reset()
{
    spindle_set_speed(0);
    spindle_stop();
}

// to be used blow, assumes spindle.speed (etc) are already setup
void _actually_set_spindle_speed() {
    float speed_lo, speed_hi;
    bool clamp_speeds = false;
    if (spindle.state == SPINDLE_CW) {
        speed_lo = pwm.c[PWM_1].cw_speed_lo;
        speed_hi = pwm.c[PWM_1].cw_speed_hi;
        clamp_speeds = true;
    } else if (spindle.state == SPINDLE_CCW ) {
        speed_lo = pwm.c[PWM_1].ccw_speed_lo;
        speed_hi = pwm.c[PWM_1].ccw_speed_hi;
        clamp_speeds = true;
    } else {
        // off/disabled/paused
        spindle.speed_actual = 0;
    }

    if (clamp_speeds) {
        // clamp spindle speed to lo/hi range
        if (spindle.speed < speed_lo) {
            spindle.speed = speed_lo;
        }

        // allow spindle.speed_actual to start at 0 to match physical spinup

        if (spindle.speed > speed_hi) {
            spindle.speed = speed_hi;
        }

        if (spindle.speed_actual > speed_hi) {
            spindle.speed_actual = speed_hi;
        }
    } else {
        pwm_set_duty(PWM_1, _get_spindle_pwm(spindle, pwm));
        return;
    }

    if (fp_ZERO(spindle.speed_change_per_tick)) { //  || (spindle.speed <= spindle.speed_actual)
        spindle.speed_actual = spindle.speed;
    }
    pwm_set_duty(PWM_1, _get_spindle_pwm(spindle, pwm));

    if (fp_NE(spindle.speed_actual, spindle.speed)) { //  && (spindle.speed > spindle.speed_actual)
        // use the larger of: spindup_delay setting, or the time it'll take to ramp to the new speed, converted to seconds
        if (fp_NOT_ZERO(spindle.speed_change_per_tick)) {
            mp_request_out_of_band_dwell(spindle.spinup_delay + 0.001*std::abs(spindle.speed-spindle.speed_actual)/spindle.speed_change_per_tick);
        } else {
            mp_request_out_of_band_dwell(spindle.spinup_delay);
        }
    }
}

/****************************************************************************************
 * _exec_spindle_control()     - actually execute the spindle command
 * spindle_control_immediate() - execute spindle control immediately
 * spindle_control_sync()      - queue a spindle control to the planner buffer
 *
 *  Basic operation: Spindle function is executed by _exec_spindle_control().
 *  Spindle_control_immediate() performs the control as soon as it's received.
 *  Spindle_control_sync() inserts spindle move into the planner, and handles spinups.
 *
 *  Valid inputs to Spindle_control_immediate() and Spindle_control_sync() are:
 *
 *    - SPINDLE_OFF turns off spindle and sets spindle state to SPINDLE_OFF.
 *      This will also re-load enable and direction polarity to the pins if they have changed.
 *      The spindle.direction value is not affected (although this doesn't really matter).
 *
 *    - SPINDLE_CW or SPINDLE_CCW turns sets direction accordingly and spindle on.
 *      In spindle_control_sync() a non-zero spinup delay runs a dwell immediately
 *      following the spindle change, but only if the planner had planned the spindle
 *      operation to zero. (I.e. if the spindle controls / S words do not plan to zero
 *      the delay is not run). Spindle_control_immediate() has no spinup delay or
 *      dwell behavior.
 *
 *    - SPINDLE_PAUSE is only applicable to CW and CCW states. It forces the spindle OFF and
 *      sets spindle.state to PAUSE. A PAUSE received when not in CW or CCW state is ignored.
 *
 *    - SPINDLE_RESUME, if in a PAUSE state, reverts to previous SPINDLE_CW or SPINDLE_CCW.
 *      The SPEED is not changed, and if it were changed in the interim the "new" speed
 *      is used. If RESUME is received from spindle_control_sync() the usual spinup delay
 *      behavior occurs. If RESUME is received when not in a PAUSED state it is ignored.
 *      This recognizes that the main reason an immediate command would be issued - either
 *      manually by the user or by an alarm or some other program function - is to stop
 *      a spindle. So the Resume should be ignored for safety.
 */
/*  Notes:
 *    - Since it's possible to queue a sync'd control, and then set any spindle state
 *      with an immediate() before the queued command is reached, _exec_spindle_control()
 *      must gracefully handle any arbitrary state transition (not just the "legal" ones).
 *
 *    - The spinup and spindown rows are present, but are not implemented unless we
 *      find we need them. It's easy enough to set these flags using the bit vector
 *      passed from sync(),but unsetting them once the delay is complete would take
 *      some more work.
 *
 *    Q: Do we need a spin-down for direction reversal?
 *    Q: Should the JSON be able to pause and resume? For test purposes only?
 */
/*  State/Control matrix. Read "If you are in state X and get control Y do action Z"

    Control: OFF         CW          CCW        PAUSE      RESUME
 State: |-----------|-----------|-----------|-----------|-----------|
    OFF |    OFF    |    CW     |    CCW    |    NOP    |    NOP    |
        |-----------|-----------|-----------|-----------|-----------|
     CW |    OFF    |    NOP    |  REVERSE  |   PAUSE   |    NOP    |
        |-----------|-----------|-----------|-----------|-----------|
    CCW |    OFF    |  REVERSE  |    NOP    |   PAUSE   |    NOP    |
        |-----------|-----------|-----------|-----------|-----------|
  PAUSE |    OFF    |    CW     |    CCW    |    NOP    |   RESUME  |
        |-----------|-----------|-----------|-----------|-----------|
 RESUME |  invalid  |  invalid  |  invalid  |  invalid  |  invalid  |
        |-----------|-----------|-----------|-----------|-----------|

 Actions:
    - OFF       Turn spindle off. Even if it's already off (reloads polarities)
    - CW        Turn spindle on clockwise
    - CCW       Turn spindle on counterclockwise
    - PAUSE     Turn off spindle, enter PAUSE state
    - RESUME    Turn spindle on CW or CCW as before
    - NOP       No operation, ignore
    - REVERSE   Reverse spindle direction (Q: need a cycle to spin down then back up again?)
 */

static void _exec_spindle_control(float *value, bool *)
{
    spControl control = (spControl)value[0];
    if (control > SPINDLE_ACTION_MAX) {
        return;
    }
    spControl state = spindle.state;
    if (state >= SPINDLE_ACTION_MAX) {
//        rpt_exception(STAT_SPINDLE_ASSERTION_FAILURE, "illegal spindle state");
        return;
    }
    constexpr spControl matrix[20] = {
        SPINDLE_OFF,  SPINDLE_CW,    SPINDLE_CCW,    SPINDLE_NOP,   SPINDLE_NOP,
        SPINDLE_OFF,  SPINDLE_NOP,   SPINDLE_REV,    SPINDLE_PAUSE, SPINDLE_NOP,
        SPINDLE_OFF,  SPINDLE_REV,   SPINDLE_NOP,    SPINDLE_PAUSE, SPINDLE_NOP,
        SPINDLE_OFF,  SPINDLE_CW,    SPINDLE_CCW,    SPINDLE_NOP,   SPINDLE_RESUME
    };
    spControl action = matrix[(state*5)+control];

    SPINDLE_DIRECTION_ASSERT;               // ensure that the spindle direction is sane
    int8_t enable_bit = 0;                  // default to 0=off
    int8_t dir_bit = -1;                    // -1 will skip setting the direction. 0 & 1 are valid values

// #ifdef ENABLE_INTERLOCK_AND_ESTOP
//     if (!spindle_ready_to_resume()) { // In E-stop, don't process any spindle commands
//         action = SPINDLE_OFF;
//     }

//     // // If we're paused or in interlock, or the esc is rebooting, send the spindle an "OFF" command (invisible to cm->gm),
//     // // and issue a hold if necessary
//     // else if(action == SPINDLE_PAUSE || cm1.safety_state != 0) {
//     //     if(action != SPINDLE_PAUSE) {
//     //         action = SPINDLE_PAUSE;
//     //         cm_set_motion_state(MOTION_STOP);
//     //         cm_request_feedhold(FEEDHOLD_TYPE_ACTIONS, FEEDHOLD_EXIT_INTERLOCK);
//     //         sr_request_status_report(SR_REQUEST_IMMEDIATE);
//     //     }
//     // }
// #endif

    switch (action) {
        case SPINDLE_NOP: { return; }

        case SPINDLE_OFF: {                 // enable_bit already set for this case
            dir_bit = spindle.direction-1;  // spindle direction was stored as '1' & '2'
            spindle.state = SPINDLE_OFF;    // the control might have been something other than SPINDLE_OFF
            break;
        }
        case SPINDLE_CW: case SPINDLE_CCW: case SPINDLE_REV: {  // REV is handled same as CW or CCW for now
            enable_bit = 1;
            dir_bit = control-1;            // adjust direction to be used as a bitmask
            spindle.direction = control;
            spindle.state = control;
            break;
        }
        case SPINDLE_PAUSE : {
            spindle.state = SPINDLE_PAUSE;
            break;  // enable bit is already set up to stop the move
        }
        case SPINDLE_RESUME: {
            enable_bit = 1;
            dir_bit = spindle.direction-1;  // spindle direction was stored as '1' & '2'
            spindle.state = spindle.direction;
            break;
        }
        default: {}                         // reversals not handled yet
    }

    // Apply the enable and direction bits and adjust the PWM as required

    // set the direction first
    if (dir_bit >= 0) {
        if (spindle_direction_output != nullptr) {
            spindle_direction_output->setValue(dir_bit);
        }
    }

    // set spindle enable
    if (spindle_enable_output != nullptr) {
        spindle_enable_output->setValue(enable_bit);
    }

    _actually_set_spindle_speed();
}

/*
 * spindle_control_immediate() - execute spindle control immediately
 * spindle_control_sync()      - queue a spindle control to the planner buffer
 */

stat_t spindle_set_direction(spDirection direction)
{
    cm->gm.spindle_direction = direction;
    return(STAT_OK);
}


// stat_t spindle_control_immediate(spControl control)
// {
//     float value[] = { (float)control };
//     _exec_spindle_control(value, nullptr);
//     return(STAT_OK);
// }

// stat_t spindle_control_sync(spControl control)  // uses spControl arg: OFF, CW, CCW
// {
//     // skip the PAUSE operation if pause-enable is not enabled (pause-on-hold)
//     if ((control == SPINDLE_PAUSE) && (!spindle.pause_enable)) {
//         return (STAT_OK);
//     }

//     // ignore pause and resume if the spindle isn't even on
//     if ((spindle.state == SPINDLE_OFF) && (control == SPINDLE_PAUSE || control == SPINDLE_RESUME)) {
//         return (STAT_OK);
//     }

//     if (spindle.speed > 0.0 && !is_spindle_ready_to_resume()) {
//         // request a feedhold immediately
//         cm_request_feedhold(FEEDHOLD_TYPE_ACTIONS, FEEDHOLD_EXIT_CYCLE);
//     }

//     // queue the spindle control
//     float value[] = { (float)control };
//     mp_queue_command(_exec_spindle_control, value, nullptr);
//     return(STAT_OK);
// }

/****************************************************************************************
 * _exec_spindle_speed()     - actually execute the spindle speed command
 * spindle_speed_immediate() - execute spindle speed change immediately
 * spindle_speed_sync()      - queue a spindle speed change to the planner buffer
 *
 *  Setting S0 is considered as turning spindle off. Setting S to non-zero from S0
 *  will enable a spinup delay if spinups are npn-zero.
 */

static void _exec_spindle_speed(float *value, bool *flag)
{
    spindle.speed = value[0];

    _actually_set_spindle_speed();
}

static stat_t _casey_jones(float speed)
{
    if (speed < spindle.speed_min) { return (STAT_SPINDLE_SPEED_BELOW_MINIMUM); }
    if (speed > spindle.speed_max) { return (STAT_SPINDLE_SPEED_MAX_EXCEEDED); }
    return (STAT_OK);
}

// stat_t spindle_speed_immediate(float speed)
// {
//     ritorno(_casey_jones(speed));
//     float value[] = { speed };
//     _exec_spindle_speed(value, nullptr);
//     return (STAT_OK);
// }

stat_t spindle_set_speed(float speed)
{
    ritorno(_casey_jones(speed));
    cm->gm.spindle_speed = speed;

    // float value[] = { speed };
    // mp_queue_command(_exec_spindle_speed, value, nullptr);

    return (STAT_OK);
}

bool is_spindle_ready_to_resume() {
#ifdef ENABLE_INTERLOCK_AND_ESTOP
    if ((cm1.estop_state != 0) || (cm1.safety_state != 0)) {
        return false;
    }
#endif
    return true;
}

bool is_spindle_on_or_paused() {
    if (spindle.state != SPINDLE_OFF) {
        return true;
    }
    return false;
}

// returns if it's done
bool do_spindle_speed_ramp_from_systick() {
#ifdef ENABLE_INTERLOCK_AND_ESTOP
    bool done = false;
    if ((cm1.estop_state == 0) && (cm1.safety_state == 0)) {
        if (fp_EQ(spindle.speed_actual, spindle.speed)) {
            return true;
        } else if (spindle.speed_actual < spindle.speed) {
            spindle.speed_actual += spindle.speed_change_per_tick;
            if (spindle.speed_actual > spindle.speed) {
                spindle.speed_actual = spindle.speed;
                done = true;
            }
        }
        else {
            spindle.speed_actual -= spindle.speed_change_per_tick;
            if (spindle.speed_actual < spindle.speed) {
                spindle.speed_actual = spindle.speed;
                done = true;
            }
        }
        pwm_set_duty(PWM_1, _get_spindle_pwm(spindle, pwm));
    } else {
        spindle.speed_actual = 0;
        spindle.state = SPINDLE_PAUSE;
        pwm_set_duty(PWM_1, _get_spindle_pwm(spindle, pwm));
        done = (cm1.hold_state != FEEDHOLD_OFF);
    }
    return done;
#else
    return true;
#endif
}

/****************************************************************************************
 * _get_spindle_pwm() - return PWM phase (duty cycle) for dir and speed
 */

static float _get_spindle_pwm (spSpindle_t &_spindle, pwmControl_t &_pwm)
{
    float speed_lo, speed_hi, phase_lo, phase_hi;
    if (_spindle.direction == SPINDLE_CW ) {
        speed_lo = _pwm.c[PWM_1].cw_speed_lo;
        speed_hi = _pwm.c[PWM_1].cw_speed_hi;
        phase_lo = _pwm.c[PWM_1].cw_phase_lo;
        phase_hi = _pwm.c[PWM_1].cw_phase_hi;
    } else { // if (direction == SPINDLE_CCW ) {
        speed_lo = _pwm.c[PWM_1].ccw_speed_lo;
        speed_hi = _pwm.c[PWM_1].ccw_speed_hi;
        phase_lo = _pwm.c[PWM_1].ccw_phase_lo;
        phase_hi = _pwm.c[PWM_1].ccw_phase_hi;
    }

    if ((_spindle.state == SPINDLE_CW) || (_spindle.state == SPINDLE_CCW)) {
        // clamp spindle speed to lo/hi range
        // if (_spindle.speed_actual < speed_lo) {
        //     _spindle.speed_actual = speed_lo;
        // }
        if (_spindle.speed_actual > speed_hi) {
            _spindle.speed_actual = speed_hi;
        }
        // normalize speed to [0..1]
        float speed = std::max(0.0f, (_spindle.speed_actual - speed_lo)) / (speed_hi - speed_lo);
        return (speed * (phase_hi - phase_lo)) + phase_lo;
    } else {
        return (_pwm.c[PWM_1].phase_off);
    }
}

/****************************************************************************************
 * spindle_override_control()
 * spindle_start_override()
 * spindle_end_override()
 */

stat_t spindle_override_control(const float P_word, const bool P_flag) // M51
{
    bool new_enable = true;
    bool new_override = false;
    if (P_flag) {                           // if parameter is present in Gcode block
        if (fp_ZERO(P_word)) {
            new_enable = false;             // P0 disables override
        } else {
            if (P_word < SPINDLE_OVERRIDE_MIN) {
                return (STAT_INPUT_LESS_THAN_MIN_VALUE);
            }
            if (P_word > SPINDLE_OVERRIDE_MAX) {
                return (STAT_INPUT_EXCEEDS_MAX_VALUE);
            }
            spindle.override_factor = P_word;    // P word is valid, store it.
            new_override = true;
        }
    }
    if (cm->gmx.m48_enable) {               // if master enable is ON
        if (new_enable && (new_override || !spindle.override_enable)) {   // 3 cases to start a ramp
            spindle_start_override(SPINDLE_OVERRIDE_RAMP_TIME, spindle.override_factor);
        } else if (spindle.override_enable && !new_enable) {              // case to turn off the ramp
            spindle_end_override(SPINDLE_OVERRIDE_RAMP_TIME);
        }
    }
    spindle.override_enable = new_enable;        // always update the enable state
    return (STAT_OK);
}

void spindle_start_override(const float ramp_time, const float override_factor)
{
    return;
}

void spindle_end_override(const float ramp_time)
{
    return;
}

/****************************
 * END OF SPINDLE FUNCTIONS *
 ****************************/

/****************************************************************************************
 * CONFIGURATION AND INTERFACE FUNCTIONS
 * Functions to get and set variables from the cfgArray table
 ****************************************************************************************/

/****************************************************************************************
 **** Spindle Settings ******************************************************************
 ****************************************************************************************/

stat_t sp_get_spep(nvObj_t *nv) { return(get_integer(nv, spindle.enable_polarity)); }
stat_t sp_set_spep(nvObj_t *nv) {
    stat_t status = set_integer(nv, (uint8_t &)spindle.enable_polarity, 0, 1);
    spindle_enable_output->setPolarity((ioPolarity)spindle.enable_polarity);
    spindle_stop(); // stop spindle and apply new settings
    return (status);
}

stat_t sp_get_spdp(nvObj_t *nv) { return(get_integer(nv, spindle.dir_polarity)); }
stat_t sp_set_spdp(nvObj_t *nv) {
    stat_t status = set_integer(nv, (uint8_t &)spindle.dir_polarity, 0, 1);
    spindle_direction_output->setPolarity((ioPolarity)spindle.dir_polarity);
    spindle_stop(); // stop spindle and apply new settings
    return (status);
}

stat_t sp_get_spph(nvObj_t *nv) { return(get_integer(nv, spindle.pause_enable)); }
stat_t sp_set_spph(nvObj_t *nv) { return(set_integer(nv, (uint8_t &)spindle.pause_enable, 0, 1)); }
stat_t sp_get_spde(nvObj_t *nv) { return(get_float(nv, spindle.spinup_delay)); }
stat_t sp_set_spde(nvObj_t *nv) { return(set_float_range(nv, spindle.spinup_delay, 0, SPINDLE_DWELL_MAX)); }

stat_t sp_get_spsn(nvObj_t *nv) { return(get_float(nv, spindle.speed_min)); }
stat_t sp_set_spsn(nvObj_t *nv) { return(set_float_range(nv, spindle.speed_min, SPINDLE_SPEED_MIN, SPINDLE_SPEED_MAX)); }
stat_t sp_get_spsm(nvObj_t *nv) { return(get_float(nv, spindle.speed_max)); }
stat_t sp_set_spsm(nvObj_t *nv) { return(set_float_range(nv, spindle.speed_max, SPINDLE_SPEED_MIN, SPINDLE_SPEED_MAX)); }

stat_t sp_get_spoe(nvObj_t *nv) { return(get_integer(nv, spindle.override_enable)); }
stat_t sp_set_spoe(nvObj_t *nv) { return(set_integer(nv, (uint8_t &)spindle.override_enable, 0, 1)); }
stat_t sp_get_spo(nvObj_t *nv) { return(get_float(nv, spindle.override_factor)); }
stat_t sp_set_spo(nvObj_t *nv) { return(set_float_range(nv, spindle.override_factor, SPINDLE_OVERRIDE_MIN, SPINDLE_OVERRIDE_MAX)); }

// These are provided as a way to view and control spindles without using M commands
stat_t sp_get_spc(nvObj_t *nv) { return(get_integer(nv, spindle.state)); }
stat_t sp_set_spc(nvObj_t *nv) { return(spindle_control_immediate((spControl)nv->value_int)); }
stat_t sp_get_sps(nvObj_t *nv) { return(get_float(nv, spindle.speed)); }
stat_t sp_set_sps(nvObj_t *nv) { return(spindle_speed_immediate(nv->value_flt)); }

/****************************************************************************************
 * TEXT MODE SUPPORT
 * Functions to print variables from the cfgArray table
 ****************************************************************************************/

#ifdef __TEXT_MODE

const char fmt_spc[]  = "[spc]  spindle control:%12d [0=OFF,1=CW,2=CCW]\n";
const char fmt_sps[]  = "[sps]  spindle speed:%14.0f rpm\n";
const char fmt_spmo[] = "[spmo] spindle mode%16d [0=disabled,1=plan-to-stop,2=continuous]\n";
const char fmt_spep[] = "[spep] spindle enable polarity%5d [0=active_low,1=active_high]\n";
const char fmt_spdp[] = "[spdp] spindle direction polarity%2d [0=CW_low,1=CW_high]\n";
const char fmt_spph[] = "[spph] spindle pause on hold%7d [0=no,1=pause_on_hold]\n";
const char fmt_spde[] = "[spde] spindle spinup delay%10.1f seconds\n";
const char fmt_spsn[] = "[spsn] spindle speed min%14.2f rpm\n";
const char fmt_spsm[] = "[spsm] spindle speed max%14.2f rpm\n";
const char fmt_spoe[] = "[spoe] spindle speed override ena%2d [0=disable,1=enable]\n";
const char fmt_spo[]  = "[spo]  spindle speed override%10.3f [0.050 < spo < 2.000]\n";

void sp_print_spc(nvObj_t *nv)  { text_print(nv, fmt_spc);}     // TYPE_INT
void sp_print_sps(nvObj_t *nv)  { text_print(nv, fmt_sps);}     // TYPE_FLOAT
void sp_print_spmo(nvObj_t *nv) { text_print(nv, fmt_spmo);}    // TYPE_INT
void sp_print_spep(nvObj_t *nv) { text_print(nv, fmt_spep);}    // TYPE_INT
void sp_print_spdp(nvObj_t *nv) { text_print(nv, fmt_spdp);}    // TYPE_INT
void sp_print_spph(nvObj_t *nv) { text_print(nv, fmt_spph);}    // TYPE_INT
void sp_print_spde(nvObj_t *nv) { text_print(nv, fmt_spde);}    // TYPE_FLOAT
void sp_print_spsn(nvObj_t *nv) { text_print(nv, fmt_spsn);}    // TYPE_FLOAT
void sp_print_spsm(nvObj_t *nv) { text_print(nv, fmt_spsm);}    // TYPE_FLOAT
void sp_print_spoe(nvObj_t *nv) { text_print(nv, fmt_spoe);}    // TYPE INT
void sp_print_spo(nvObj_t *nv)  { text_print(nv, fmt_spo);}     // TYPE FLOAT

#endif // __TEXT_MODE


constexpr cfgItem_t spindle_config_items_1[] = {
    // Spindle functions
    { "sp","spmo", _i0,  0, sp_print_spmo, get_nul,     set_nul,     nullptr, 0 }, // keeping this key around, but it returns null and does nothing
    { "sp","spph", _bip, 0, sp_print_spph, sp_get_spph, sp_set_spph, nullptr, SPINDLE_PAUSE_ON_HOLD },
    { "sp","spde", _fip, 2, sp_print_spde, sp_get_spde, sp_set_spde, nullptr, SPINDLE_SPINUP_DELAY },
    { "sp","spsn", _fip, 2, sp_print_spsn, sp_get_spsn, sp_set_spsn, nullptr, SPINDLE_SPEED_MIN},
    { "sp","spsm", _fip, 2, sp_print_spsm, sp_get_spsm, sp_set_spsm, nullptr, SPINDLE_SPEED_MAX},
    { "sp","spep", _iip, 0, sp_print_spep, sp_get_spep, sp_set_spep, nullptr, SPINDLE_ENABLE_POLARITY },
    { "sp","spdp", _iip, 0, sp_print_spdp, sp_get_spdp, sp_set_spdp, nullptr, SPINDLE_DIR_POLARITY },
    { "sp","spoe", _bip, 0, sp_print_spoe, sp_get_spoe, sp_set_spoe, nullptr, SPINDLE_OVERRIDE_ENABLE},
    { "sp","spo",  _fip, 3, sp_print_spo,  sp_get_spo,  sp_set_spo,  nullptr, SPINDLE_OVERRIDE_FACTOR},
    { "sp","spc",  _i0,  0, sp_print_spc,  sp_get_spc,  sp_set_spc,  nullptr, 0 },   // spindle state
    { "sp","sps",  _f0,  0, sp_print_sps,  sp_get_sps,  sp_set_sps,  nullptr, 0 },   // spindle speed
};
constexpr cfgSubtableFromStaticArray spindle_config_1 {spindle_config_items_1};
const configSubtable * const getSpindleConfig_1() { return &spindle_config_1; }

#endif // 0