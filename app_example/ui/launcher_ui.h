#ifndef LAUNCHER_UI_H
#define LAUNCHER_UI_H

/*
 * launcher_ui.h — boot screen and the demo's only mode router.
 *
 * Two cards: JPG Album (ui/album_ui) and MJPEG Video (core/mjpeg_player).
 * Tapping one enters that mode, a 3-finger tap comes back.  While a clip plays
 * the player task owns the display and the LVGL thread stands down (app_main).
 */

/* Build the launcher and start the mode router.  Call on the LVGL thread after
 * display and touch indev creation. */
void launcher_ui_init(void);

/* Enter the JPG album directly: hide the launcher layer first, then decode
 * photo #0, so the album really takes the screen.  Used by the album's
 * SD-recovery "Flash Album" button.  Decoding while the OPAQUE launcher is
 * still visible flashed one frame and LVGL repainted the launcher over it. */
void launcher_enter_album(void);

#endif /* LAUNCHER_UI_H */
