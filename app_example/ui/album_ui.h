#ifndef ALBUM_UI_H
#define ALBUM_UI_H

#include <stdbool.h>
#include "lvgl.h"

/* ========================================================================
 * Photo album UI
 *
 * Photos are HW-JPEG decoded (PP block) DIRECTLY into both LCDC framebuffers;
 * the info bar is a normal LVGL overlay on top.  Swipe left/right = prev/next
 * photo, double-tap = toggle fit mode (cover/stretch), tap = show/hide info
 * bar, long-press = jump to first photo.
 * ======================================================================== */

/* Build info bar + wire input.  Must run on the LVGL thread
 * (after lv_init / lv_display_create / touch indev creation).           */
void album_ui_init(void);

/* Photo navigation — safe to call from the LVGL thread only. */
void album_show_photo(int index); /* wraps around, decodes + renders       */
void album_next_photo(void);
void album_prev_photo(void);

/* Slideshow toggle (tap). */
void album_slideshow_toggle(void);

/* True while the canvas holds a decoded photo (vs. placeholder text). */
bool album_has_photo(void);

/* Photo count of the source the album is currently using (SD or flash). */
int album_ui_photo_count(void);

/* Input gate for the launcher: the album's events are registered on the indev,
 * so without this a tap on the launcher layer would still swipe photos behind
 * it.  The launcher clears it when it shows and sets it when it hides. */
void album_ui_set_active(bool active);

#endif /* ALBUM_UI_H */
