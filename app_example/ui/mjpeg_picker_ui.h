#ifndef MJPEG_PICKER_UI_H
#define MJPEG_PICKER_UI_H

/* ========================================================================
 * MJPEG video picker — a folder-selection layer for a multi-video MJPEG/
 * directory.  Rendered by LVGL as an OPAQUE full-screen layer (same recipe as
 * the launcher) that hides to start one of several videos.
 *
 * The picker does NOT route itself — it only calls back into the owner:
 *
 *   build cards → on tap:  on_pick(idx) hides the layer + starts playback
 *               → on back: on_back()   hides the layer + re-shows the launcher
 *
 * Playback ends back at the LAUNCHER (launcher_ui's poll timer), not at the
 * picker — the picker is a transient chooser, re-shown only when the user taps
 * the MJPEG card again.  The owner calls mjpeg_picker_hide() before navigating
 * away.  The picker owns no persistent state; it rebuilds its cards on every
 * show.
 * ======================================================================== */

#include <stdbool.h>

/* Number of picker cards per row on this 800-wide screen (fits 4). */
#define MJPEG_PICKER_COLS 4

/* Show the picker for the current MJPEG scan (reads mjpeg_video_count() /
 * mjpeg_video_at() — the launcher must have scanned already).
 * @param count   number of videos to list (≤ MJPEG_MAX_VIDEOS)
 * @param on_pick callback when a video card is tapped  (index, userdata)
 * @param on_back callback when the back arrow is tapped (userdata)
 * @param udata   opaque context passed to both callbacks. */
void mjpeg_picker_show(int count,
                       void (*on_pick)(int index, void* udata),
                       void (*on_back)(void* udata),
                       void* udata);

/* Hide the picker layer (called by the owner when starting playback or
 * returning to the launcher).  Safe to call when not visible. */
void mjpeg_picker_hide(void);

#endif /* MJPEG_PICKER_UI_H */
