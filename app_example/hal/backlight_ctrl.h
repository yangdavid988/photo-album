#ifndef BACKLIGHT_CTRL_H
#define BACKLIGHT_CTRL_H

#include <stdint.h>
#include <stdbool.h>
#include "config/threshold_config.h"  /* BL_MIN_PCT, BL_STEP_PCT, BRIGHTNESS_* */

/* ========================================================================
 * Backlight brightness control
 *
 * Uses TIM4 channel 0 with mbed hardware PWM.
 *
 * Compile-time defaults (threshold_config.h) serve as the initial value
 * only; runtime changes never modify the header.
 *
 * Clamp limits (BL_MIN_PCT, BL_STEP_PCT) are also defined in
 * config/threshold_config.h — adjust them there for all consumers.
 *
 * API:
 *   backlight_init()               — one-time init
 *   backlight_set(percent)         — direct ..100 override
 *   backlight_adjust(delta)        — step up/down, clamped to [BL_MIN_PCT, 100]
 *   backlight_get()                — query current %
 * ======================================================================== */


/** Init TIM4 hardware PWM backlight */
void backlight_init(void);

/** Direct brightness set (0..100), bypasses BRIGHTNESS_ENABLED gate */
void backlight_set(int percent);

/** Step adjust — adds delta (±BL_STEP_PCT typical), clamped to [BL_MIN_PCT, 100] */
void backlight_adjust(int delta);

/** Get current brightness level (0..100) */
int backlight_get(void);

#endif /* BACKLIGHT_CTRL_H */
