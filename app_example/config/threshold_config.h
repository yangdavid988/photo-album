/*
 * threshold_config.h — backlight tuning constants for the album demo.
 *
 * Trimmed from pc_dashboard_demo's version to only the symbols that
 * hal/backlight_ctrl.c references.  Values match the T1720A panel.
 */
#pragma once
#include <stdint.h>

/* Master enable: 0 = always 100 %, 1 = allow dimming */
#define BRIGHTNESS_ENABLED 1

/* Normal brightness (0..100) */
#define BRIGHTNESS_NORMAL_PCT 100

/* Hard floor for brightness (%) */
#define BL_MIN_PCT 2

/* Step size (%) for backlight_adjust(+/-) */
#define BL_STEP_PCT 10
