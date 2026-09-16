/*
 * album_config.h — photo album demo tunables
 */
#pragma once
#include <stdint.h>

/* Display (T1720A panel, matches lcdc_core.c WIDTH/HEIGHT) */
#define ALBUM_SCREEN_W 800
#define ALBUM_SCREEN_H 480

/* Slideshow auto-advance interval (ms). 0 = disabled (manual only). */
#define ALBUM_SLIDESHOW_INTERVAL_MS 5000

/* Status bar height overlaid on top of the photo */
#define ALBUM_STATUSBAR_H 36

/* Info bar auto-hide delay (ms).  Tap re-shows it; PP re-frames the photo
 * full-screen while hidden. */
#define ALBUM_BAR_AUTOHIDE_MS 3000

/* LVGL thread parameters (same as pc_dashboard_demo) */
#ifndef TASK_STACK_LVGL
#define TASK_STACK_LVGL 8192
#endif
#ifndef TASK_PRIO_LVGL
#define TASK_PRIO_LVGL (tskIDLE_PRIORITY + 3)
#endif
