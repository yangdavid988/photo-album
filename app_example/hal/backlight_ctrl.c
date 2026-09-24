/*
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ameba_soc.h"
#include "hal/backlight_ctrl.h"
#include "config/threshold_config.h"
#include "string.h"
#include "pwmout_api.h"
#include "log.h"

#ifndef TAG
#define TAG "BACKLIGHT"
#endif

/* ========================================================================
 * Backlight brightness control — raw register PWM (T1720A only)
 *
 * Uses TIM4 channel 0 with direct register access to drive the backlight
 * MOSFET gate.  Backlight pin is _PA_25.
 *
 * Timer: ~1 kHz PWM via TIM4, zero CPU overhead (hardware output).
 *
 * Reference design: pwm_raw_RGB_demo.c — channels configured BEFORE timer
 * start, full CCxInit (preload, polarity, output mode, compare value),
 * explicit prescaler in TimeBaseInit struct.
 *
 * Frequency note: the backlight MOSFET gate has an RC filter.  At low PWM
 * frequencies (~100 Hz) the gate fully discharges during the long OFF period,
 * so the MOSFET switches fully OFF each cycle and the LED is OFF except at
 * near-100 % duty.  At ~1 kHz (no prescaler) the RC filter smooths the PWM,
 * keeping the gate voltage above the MOSFET threshold across a wide duty
 * range — the MOSFET acts as a variable resistor, giving continuous
 * brightness control.
 * ======================================================================== */

#define BL_TIMER_IDX 4 /* TIM4 */
#define BL_PWM_CHAN  0 /* channel 0 */

/*
 * Timer frequency calculation (no prescaler):
 *   APB clock   = 40 MHz
 *   Prescaler   = 0       -> timer clock = 40 / 1 = 40 MHz
 *   Period      = 40000   -> PWM freq    = 40 MHz / 40000 = 1 kHz
 *   CCRx value  = duty_ratio * BL_PERIOD_TICKS
 *
 * The 16-bit ARR register limits the max period to 65535 ticks.
 * With PSC=0 the minimum PWM frequency is ~610 Hz (ARR=65535).
 * 1 kHz is a good balance: fast enough for RC smoothing, slow enough
 * for decent CCRx resolution (16-bit).
 */
#define BL_PRESCALER    0
#define BL_PERIOD_TICKS 40000                 /* full-scale compare value = ARR + 1 */
#define BL_ARR          (BL_PERIOD_TICKS - 1) /* counter reload */

static bool    g_bl_initialized = false;
static int     g_user_pct       = 100; /* user-space brightness (with remap applied internally) */
static PinName g_bl_pin;               /* backlight PinName (for pinmux) */
static u32     g_bl_gpio_pin;          /* backlight GPIO pin number (for GPIO ops) */

/*
 * Brightness remap: user_percent(0..100) -> actual PWM duty(0.0 .. 1.0)
 *
 * T1720A backlight is driven directly by PWM, no MOSFET saturation, so a
 * quadratic curve gives a gentle low-end roll-off:
 *   user  20 -> duty 4.0 %
 *   user  50 -> duty 25 %
 *   user 100 -> duty 100 %
 */
static float backlight_remap(int user_pct)
{
    float x = (float) user_pct / 100.0f;
    return x * x; /* quadratic — gentle low-end, no MOSFET */
}

/* ========================================================================
 * Public API
 * ======================================================================== */

void backlight_init(void)
{
    if (g_bl_initialized)
        return;

    /* T1720A backlight pin — used for both the PWM pinmux and the 0 % GPIO
     * drive-down path. */
    g_bl_pin      = PA_25;
    g_bl_gpio_pin = (u32) _PA_25;

    RTK_LOGI(TAG, "backlight_init: raw PWM on _PA_25\n");

    /* ---- 1. Enable TIM4 peripheral clock ---- */
    RCC_PeriphClockCmd(APBPeriph_TIMx[BL_TIMER_IDX],
                       APBPeriph_TIMx_CLOCK[BL_TIMER_IDX],
                       ENABLE);

    /* ---- 2. Configure timer base (explicit prescaler + period) ---- */
    {
        RTIM_TimeBaseInitTypeDef TIM_InitStruct;
        RTIM_TimeBaseStructInit(&TIM_InitStruct);
        TIM_InitStruct.TIM_Idx       = BL_TIMER_IDX;
        TIM_InitStruct.TIM_Prescaler = BL_PRESCALER;
        TIM_InitStruct.TIM_Period    = BL_ARR;
        RTIM_TimeBaseInit(TIMx[BL_TIMER_IDX], &TIM_InitStruct, TIMx_irq[BL_TIMER_IDX], NULL, NULL);
    }

    /* ---- 3. Configure PWM channel (complete CCx init) ----
     * Key differences from the mbed pwmout API:
     *   a) RTIM_CCxInit() sets OCxPE (preload), OCxM (output mode),
     *      polarity, and initial compare value -- all in one shot.
     *   b) TIM_OCPreload_Disable makes CCRx changes take effect
     *      immediately (no wait for next update event).
     *   c) Channel is configured BEFORE the timer counter starts.
     */
    {
        TIM_CCInitTypeDef TIM_CCInitStruct;
        RTIM_CCStructInit(&TIM_CCInitStruct);
        TIM_CCInitStruct.TIM_CCMode       = TIM_CCMode_PWM;
        TIM_CCInitStruct.TIM_OCPulse      = BL_PERIOD_TICKS; /* 100 % initially */
        TIM_CCInitStruct.TIM_OCProtection = TIM_OCPreload_Disable;
        TIM_CCInitStruct.TIM_CCPolarity   = TIM_CCPolarity_High;
        RTIM_CCxInit(TIMx[BL_TIMER_IDX], &TIM_CCInitStruct, BL_PWM_CHAN);
    }

    /* ---- 4. Enable channel output ---- */
    RTIM_CCxCmd(TIMx[BL_TIMER_IDX], BL_PWM_CHAN, TIM_CCx_Enable);

    /* ---- 5. Route pin to TIM4 PWM function ---- */
    Pinmux_Config(g_bl_pin, PINMUX_FUNCTION_TIM4_PWM0);

    /* ---- 6. Start timer LAST (counter begins after channel is fully set up) ---- */
    RTIM_Cmd(TIMx[BL_TIMER_IDX], ENABLE);

    g_bl_initialized = true;

    /*
     * Set initial brightness to the configured normal level.
     * RTIM_CCxInit (step 3) already set compare=100%, so the display is
     * briefly at full brightness before settling to the user's default.
     */
    backlight_set((int) BRIGHTNESS_NORMAL_PCT);

    RTK_LOGI(TAG, "backlight_init: done (PSC=%d ARR=%d en=%d normal=%d%%)\n", BL_PRESCALER, BL_ARR, (int) BRIGHTNESS_ENABLED, (int) BRIGHTNESS_NORMAL_PCT);
}

void backlight_set(int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    g_user_pct = percent;

    float f    = backlight_remap(percent);
    u32   ccrx = (u32) (f * (float) BL_PERIOD_TICKS);

    if (ccrx == 0)
    {
        /*
         * 0 % brightness: switch pin to GPIO and drive LOW.
         *
         * Writing CCRx compare = 0 (or even a tiny non-zero like 1)
         * corrupts the timer state machine — subsequent non-zero
         * writes produce 100 % output regardless of value.
         *
         * Bypass the PWM entirely: route the pin to GPIO, drive it
         * LOW so the MOSFET gate discharges and the LED turns off.
         * The PWM timer keeps running in the background, so when we
         * switch back the compare value is already loaded and the
         * output appears immediately at the correct duty cycle.
         */
        Pinmux_Config(g_bl_pin, PINMUX_FUNCTION_GPIO);
        {
            GPIO_InitTypeDef gi;
            memset(&gi, 0, sizeof(gi));
            gi.GPIO_Pin  = g_bl_gpio_pin;
            gi.GPIO_Mode = GPIO_Mode_OUT;
            GPIO_Init(&gi);
            GPIO_WriteBit(g_bl_gpio_pin, 0);
        }
    }
    else
    {
        /*
         * Normal (non-zero) brightness: load the compare value
         * first, then restore pinmux to TIM4 PWM.
         *
         * Order matters: compare must be written BEFORE the pin
         * switches back to PWM, so the correct duty appears on
         * the pin from the first cycle.
         */
        RTIM_CCRxSet(TIMx[BL_TIMER_IDX], ccrx, BL_PWM_CHAN);
        Pinmux_Config(g_bl_pin, PINMUX_FUNCTION_TIM4_PWM0);
    }
}

void backlight_adjust(int delta)
{
    int cur     = backlight_get();
    int new_val = cur + delta;

    /* Clamp to [BL_MIN_PCT, 100] */
    if (new_val < BL_MIN_PCT)
        new_val = BL_MIN_PCT;
    if (new_val > 100)
        new_val = 100;

    backlight_set(new_val);
}

int backlight_get(void)
{
    return g_user_pct;
}
