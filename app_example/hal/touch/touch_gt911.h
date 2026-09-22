#ifndef TOUCH_GT911_H
#define TOUCH_GT911_H

#include <stdint.h>
#include <stdbool.h>

/* Initialize GT911 touch controller and register with LVGL.
 * Call once after lcd_init() and lv_display_create().          */
void touch_gt911_init(void);

/* Read the latest touch state, as fed to LVGL's indev.
 *
 * The GT911 work task refreshes s_lvgl_touch_data on every poll even while the
 * LVGL thread is paused (MJPEG playback freezes LVGL).  The MJPEG player polls
 * this to detect tap-to-pause / 3-finger-exit without touching any lv_* API.
 *
 * @param x    out: X in panel pixels (primary finger)
 * @param y    out: Y in panel pixels (primary finger)
 * @param down out: true while a finger is touching
 * @return     true when a valid state is available (always after init) */
void touch_gt911_get_state(int32_t* x, int32_t* y, bool* down);

/* A whole-press whole-release with ≥3 fingers, detected at driver level.
 *
 * LVGL only tracks a single pointer, so multi-finger chords must be recognised
 * here (GSTID keeps the live touch count).  The flag is one-shot and
 * latched until consumed — the launcher polls it from either the LVGL thread
 * (photo mode) or the MJPEG task (video mode), so the driver only records.
 *
 * @return true when a 3-finger tap happened since the last call (auto-clears) */
bool touch_gt911_get_three_tap(void);

/* Non-destructive peek at the 3-finger chord: returns true while a whole
 * 3-finger press is in flight or its tap is latched, WITHOUT clearing it.
 * This lets the album reject already-queued single-pointer gestures as soon
 * as the chord starts, while the launcher remains the sole latch consumer. */
bool touch_gt911_three_tap_peek(void);

#endif /* TOUCH_GT911_H */
