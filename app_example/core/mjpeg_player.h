#ifndef MJPEG_PLAYER_H
#define MJPEG_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#include "storage/album_sd_video.h" /* sd_video_t — the video descriptor */

/* MJPEG player — plays a folder of numbered JPEG frames (storage/album_sd_video)
 * through the persistent HW decode session, into the LCDC framebuffer the
 * scanner is not reading, flipped at the frame boundary.
 *
 * While mjpeg_player_is_active() the player owns the display and the LVGL
 * thread stands down (see app_main.c).  Gestures are polled by the player task
 * itself: tap = pause / resume, vertical drag = brightness, 3-finger tap = stop
 * back to the launcher.
 */

/* Playback rate (frames per second). */
#ifndef MJPEG_PLAY_FPS
#define MJPEG_PLAY_FPS 30
#endif

/* Cap on video folders found in the SD root.  Mirrors the storage-layer
 * SDV_MAX_VIDEOS; the picker shows these across multiple pages (4-per-row
 * grid).  Bump in sync if it ever grows past a page count that fits. */
#define MJPEG_MAX_VIDEOS 24

/* Scan the SD card for video folders (wraps album_sd_scan_videos).
 * @return number of videos found; 0 when none / no card. */
int mjpeg_scan_videos(void);

/* Number of videos found by mjpeg_scan_videos(). */
int mjpeg_video_count(void);

/* Descriptor of video index i (valid only after mjpeg_scan_videos). */
const sd_video_t* mjpeg_video_at(int index);

/* True while the player owns the display; the LVGL loop polls this and idles. */
bool mjpeg_player_is_active(void);

/* Start playback of a scanned video on its own task; returns immediately.
 * @return 0 on start, -1 on error. */
int mjpeg_play(int index);

/* Exit code of the last playback: 0 = clip ended, 1 = exited early (3-finger). */
int mjpeg_play_exit_code(void);

/* End-of-playback callback, run from the player task after is_active() clears.
 * Pass NULL to uninstall. */
void mjpeg_playback_end_cb_set(void (*cb)(void));

#endif /* MJPEG_PLAYER_H */
