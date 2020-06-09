/* Copyright 2020 Tracy Wadleigh
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "tp69.h"
#include "quantum.h"
#include "backlight.h"
#include "debug.h"

static void breathing_callback(PWMDriver *pwmp);

#define BACKLIGHT_LED_COUNT 5
static uint32_t g_backlight_lines[BACKLIGHT_LED_COUNT] = {
    LINE_PIN23, LINE_PIN22, LINE_PIN21, LINE_PIN20, LINE_PIN5 };
static pwmchannel_t g_backlight_channels[BACKLIGHT_LED_COUNT] = {1, 0, 6, 5, 7};
static PWMConfig g_pwm_config;

// See http://jared.geek.nz/2013/feb/linear-led-pwm
static uint16_t cie_lightness(uint16_t v) {
    if (v <= 5243)     // if below 8% of max
        return v / 9;  // same as dividing by 900%
    else {
        uint32_t y = (((uint32_t)v + 10486) << 8) / (10486 + 0xFFFFUL);  // add 16% of max and compare
        // to get a useful result with integer division, we shift left in the expression above
        // and revert what we've done again after squaring.
        y = y * y * y >> 8;
        if (y > 0xFFFFUL)  // prevent overflow
            return 0xFFFFU;
        else
            return (uint16_t)y;
    }
}

void backlight_init_ports(void) {

    for (int i = 0; i < BACKLIGHT_LED_COUNT; ++i) {
        palSetLineMode(g_backlight_lines[i], PAL_MODE_ALTERNATIVE_4);
        g_pwm_config.channels[g_backlight_channels[i]].mode = PWM_OUTPUT_ACTIVE_HIGH;
    }
    g_pwm_config.callback = NULL;
    pwmStart(&PWMD1, &g_pwm_config);

    backlight_set(get_backlight_level());
    if (is_backlight_breathing()) {
        breathing_enable();
    }
}

void backlight_set(uint8_t level) {
    if (level == 0) {
        // Turn backlight off
        for (int i = 0; i < BACKLIGHT_LED_COUNT; ++i) {
            pwmDisableChannel(&PWMD1, g_backlight_channels[i]);
        }
    } else {
        // Turn backlight on
        if (!is_breathing()) {
            uint32_t duty = (uint32_t)(cie_lightness(0xFFFF * (uint32_t)level / BACKLIGHT_LEVELS));
            for (int i = 0; i < BACKLIGHT_LED_COUNT; ++i) {
                pwmEnableChannel(&PWMD1, g_backlight_channels[i], PWM_FRACTION_TO_WIDTH(&PWMD1, 0xFFFF, duty));
            }
        }
    }
}

void backlight_task(void) {}

#define BREATHING_NO_HALT 0
#define BREATHING_HALT_OFF 1
#define BREATHING_HALT_ON 2
#define BREATHING_STEPS 128

static uint8_t g_breathing_halt = BREATHING_NO_HALT;
static uint16_t g_breathing_counter = 0;

bool is_breathing(void) { return g_pwm_config.callback != NULL; }

static inline void breathing_min(void) { g_breathing_counter = 0; }
static inline void breathing_max(void) { g_breathing_counter = get_breathing_period() * 256 / 2; }

void breathing_interrupt_enable(void) {
    pwmStop(&PWMD1);
    g_pwm_config.callback = breathing_callback;
    pwmStart(&PWMD1, &g_pwm_config);
    chSysLockFromISR();
    pwmEnablePeriodicNotification(&PWMD1);
    for (int i = 0; i < BACKLIGHT_LED_COUNT; ++i) {
        pwmEnableChannelI(&PWMD1, g_backlight_channels[i], PWM_FRACTION_TO_WIDTH(&PWMD1, 0xFFFF, 0xFFFF));
    }
    chSysUnlockFromISR();
}

void breathing_interrupt_disable(void) {
    pwmStop(&PWMD1);
    g_pwm_config.callback = NULL;
    pwmStart(&PWMD1, &g_pwm_config);
}

void breathing_enable(void) {
    g_breathing_counter = 0;
    g_breathing_halt    = BREATHING_NO_HALT;
    breathing_interrupt_enable();
}

void breathing_pulse(void) {
    if (get_backlight_level() == 0)
        breathing_min();
    else
        breathing_max();
    g_breathing_halt = BREATHING_HALT_ON;
    breathing_interrupt_enable();
}

void breathing_disable(void) {
    breathing_interrupt_disable();
    backlight_set(get_backlight_level());
}

void breathing_self_disable(void) {
    if (get_backlight_level() == 0)
        g_breathing_halt = BREATHING_HALT_OFF;
    else
        g_breathing_halt = BREATHING_HALT_ON;
}

static const uint8_t breathing_table[BREATHING_STEPS] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 17, 20, 24, 28, 32, 36, 41, 46, 51, 57, 63, 70, 76, 83, 91, 98, 106, 113, 121, 129, 138, 146, 154, 162, 170, 178, 185, 193, 200, 207, 213, 220, 225, 231, 235, 240, 244, 247, 250, 252, 253, 254, 255, 254, 253, 252, 250, 247, 244, 240, 235, 231, 225, 220, 213, 207, 200, 193, 185, 178, 170, 162, 154, 146, 138, 129, 121, 113, 106, 98, 91, 83, 76, 70, 63, 57, 51, 46, 41, 36, 32, 28, 24, 20, 17, 15, 12, 10, 8, 6, 5, 4, 3, 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

// Use this before the cie_lightness function.
static inline uint16_t scale_backlight(uint16_t v) { return v / BACKLIGHT_LEVELS * get_backlight_level(); }

static void breathing_callback(PWMDriver *pwmp) {
    (void)pwmp;
    uint8_t  breathing_period = get_breathing_period();
    uint16_t interval         = (uint16_t)breathing_period * 256 / BREATHING_STEPS;
    // resetting after one period to prevent ugly reset at overflow.
    g_breathing_counter = (g_breathing_counter + 1) % (breathing_period * 256);
    uint8_t index     = g_breathing_counter / interval % BREATHING_STEPS;

    if (((g_breathing_halt == BREATHING_HALT_ON) && (index == BREATHING_STEPS / 2)) || ((g_breathing_halt == BREATHING_HALT_OFF) && (index == BREATHING_STEPS - 1))) {
        breathing_interrupt_disable();
    }

    uint32_t duty = cie_lightness(scale_backlight(breathing_table[index] * 256));

    chSysLockFromISR();
    for (int i = 0; i < BACKLIGHT_LED_COUNT; ++i) {
        pwmEnableChannelI(&PWMD1, g_backlight_channels[i], PWM_FRACTION_TO_WIDTH(&PWMD1, 0xFFFF, duty));
    }
    chSysUnlockFromISR();
}
