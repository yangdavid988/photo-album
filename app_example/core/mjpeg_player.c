/*
 * mjpeg_player.c — MJPEG clip player (hardware decode, owns the display)
 *
 * mjpeg_play() starts a task that decodes each frame of a clip into the LCDC
 * framebuffer the scanner is not reading, then lcdc_core_flush_now() swaps the
 * DMA pointer at the frame edge — no tearing, and LVGL is not involved, so the
 * LVGL thread must stay out of lv_timer_handler() while this runs.  Each frame
 * is paced to MJPEG_PLAY_FPS with the remainder of the slot slept off.
 *
 * Gestures are polled here, not by LVGL: tap = pause / resume, vertical drag =
 * brightness, 3-finger tap = stop and return to the launcher.
 */
#include "mjpeg_player.h"

#include "core/jpeg_decode.h"
#include "core/brightness_osd.h"  /* shared raw OSD (see comment in .h) */
#include "storage/album_sd_video.h"
#include "hal/lcd/lcdc_core.h"
#include "hal/lcd/lcd_drv.h"
#include "hal/touch/touch_gt911.h"
#include "hal/backlight_ctrl.h"
#include "config/threshold_config.h" /* BL_STEP_PCT */
#include "storage/album_sd.h"        /* album_sd_mounted() */

#include <string.h>
#include <stdio.h>  /* snprintf (OSD label) */

#include "ameba_soc.h"
#include "os_wrapper.h"
#include "log.h"

#ifndef TAG
#define TAG "MJPEG"
#endif

/* ---- Tunables ---- */
#define MJPEG_SCREEN_W 800
#define MJPEG_SCREEN_H 480

#define FRAME_INTERVAL_NS (1000000000ULL / MJPEG_PLAY_FPS)
#define TAP_TIMEOUT_MS    250 /* press shorter than this = a tap       */
#define TAP_MOVE_MAX      24  /* finger moved more = not a tap         */
/* Minimum gap between pause toggles.  A touch that de-asserts (finger
 * lifted) leaves s_lvgl_touch_data PRESSED until the GT911 INT-high path
 * clears it, and the release edge can re-fire; the 350 ms lock absorbs that
 * so one deliberate tap = one pause toggle (never a double-toggle). */
#define TAP_TOGGLE_LOCK_MS 350

/* Vertical-drag brightness thresholds.  A touch must pass DRAG_MOVE_MIN to
 * count as a drag (jellyfinger guard); each DRAG_STEP_PX applies one
 * BL_STEP_PCT, the same step the album uses.  A drag is not a clip seek — the
 * demo has none. */
#define DRAG_MOVE_MIN 12 /* px past press point before a drag starts */
#define DRAG_STEP_PX  60 /* px of travel per brightness step          */
#define OSD_HOLD_MS    1500 /* match album OSD idle delay                */

/* ---- Cached scan (valid while the launcher holds it) ---- */
static sd_video_t s_videos[MJPEG_MAX_VIDEOS];
static int        s_video_count = 0;

/* ---- Playback state (shared across tasks) ---- */
static volatile bool s_active    = false; /* player owns the display     */
static volatile bool s_run       = false; /* task keep-alive             */
static volatile int  s_exit_code = 0;

/* End-of-playback callback (set by launcher_ui at startup, called from the
 * player task just before it deletes itself).  Lets the launcher distinguish
 * "still playing" from "just ended" so it can re-assert the launcher/screen
 * without spuriously gating input or re-rendering. */
static void (*s_playback_end_cb)(void) = NULL;

/* ---- 1-finger gesture state (tap + vertical drag) ---- */
static bool     s_tap_armed      = false;
static uint32_t s_tap_start_ms   = 0;
static int32_t  s_tap_x          = 0;
static int32_t  s_tap_y          = 0;
static uint64_t s_tap_busy_until = 0; /* monotonic ms: no toggle before   */
static bool     s_paused         = false;
/* Drag: starts when the finger leaves the tap radius; cumulative px travel
 * (absolute) since the drag started, consumed in DRAG_STEP_PX increments. */
static int32_t s_drag_axis      = 0;
static int32_t s_drag_travel_px = 0;

/* ---- Brightness OSD (raw FB overlay, LVGL is stopped during playback) ---- */
static volatile int32_t s_osd_pct = -1; /* -1 = no OSD.  Set by drag below. */
static uint64_t          s_osd_until_ms = 0; /* idle timeout, like album OSD */

/* ---- Frame pace deadline ---- */
static uint64_t s_next_deadline_ns = 0;

/* ========================================================================
 * Video scan (wraps the storage layer)
 * ======================================================================== */
int mjpeg_scan_videos(void)
{
    s_video_count = album_sd_scan_videos(s_videos, MJPEG_MAX_VIDEOS);
    return s_video_count;
}

int mjpeg_video_count(void)
{
    return s_video_count;
}

const sd_video_t* mjpeg_video_at(int index)
{
    if (index < 0 || index >= s_video_count)
        return NULL;
    return &s_videos[index];
}

/* ========================================================================
 * Gesture detection
 * ======================================================================== */

/* Tap (press, no move, quick release) and vertical drag (brightness).
 *
 * The tap toggle is rate-limited by TAP_TOGGLE_LOCK_MS: the GT911 release edge
 * can re-fire after the finger lifts, and each re-fire would otherwise read as
 * another tap and immediately undo the pause. */
static void tap_tracker_tick(void)
{
    int32_t x = 0, y = 0;
    bool    down = false;
    touch_gt911_get_state(&x, &y, &down);

    if (down)
    {
        if (s_drag_axis != 0)
        {
            /* Ongoing drag: accumulate travel along the chosen vertical
             * axis from the press point and apply a brightness step per
             * DRAG_STEP_PX.  (Finger still down and past the tap radius —
             * never re-arm as a new press here.) */
            int32_t dy     = y - s_tap_y;
            int32_t travel = (s_drag_axis < 0) ? -dy : dy;
            if (travel > s_drag_travel_px)
                s_drag_travel_px = travel;

            while (s_drag_travel_px >= DRAG_STEP_PX)
            {
                s_drag_travel_px -= DRAG_STEP_PX;
                backlight_adjust(s_drag_axis > 0 ? -BL_STEP_PCT : BL_STEP_PCT);
                s_osd_pct = backlight_get(); /* show on the next video frame */
                s_osd_until_ms = (uint64_t) rtos_time_get_current_system_time_ms() + OSD_HOLD_MS;
                RTK_LOGI(TAG, "drag %s -> brightness %d%%\n",
                         s_drag_axis > 0 ? "down" : "up", backlight_get());
            }
            return;
        }

        if (!s_tap_armed)
        {
            s_tap_armed    = true;
            s_tap_start_ms = rtos_time_get_current_system_time_ms();
            s_tap_x        = x;
            s_tap_y        = y;
        }
        else
        {
            int32_t dx = x - s_tap_x;
            int32_t dy = y - s_tap_y;

            /* Not a tap anymore once the finger leaves the tap radius —
             * become a vertical drag instead (brightness).  The tap release
             * path below checks the same radius, so a drag can never also
             * fire a pause toggle.  Small vertical wobble (< DRAG_MOVE_MIN)
             * or a horizontal sweep is neither: it just disarms the tap. */
            if (dx * dx + dy * dy >= TAP_MOVE_MAX * TAP_MOVE_MAX)
            {
                s_tap_armed = false;

                if (dy < -DRAG_MOVE_MIN || dy > DRAG_MOVE_MIN)
                {
                    /* Drag started: count absolute vertical travel from the
                     * press point (minus the threshold we already crossed). */
                    s_drag_axis      = (dy < 0) ? -1 : 1;
                    s_drag_travel_px = (dy < 0 ? -dy : dy) - DRAG_MOVE_MIN;
                }
            }
        }
    }
    else if (s_tap_armed)
    {
        uint32_t now = rtos_time_get_current_system_time_ms();

        if ((now - s_tap_start_ms) <= TAP_TIMEOUT_MS &&
            !(now < s_tap_busy_until))
        {
            s_tap_busy_until = (uint64_t) now + TAP_TOGGLE_LOCK_MS;
            s_paused         = !s_paused;
            RTK_LOGI(TAG, "tap -> %s\n", s_paused ? "PAUSED" : "play");
        }
        s_tap_armed = false;
    }
    else
    {
        /* Finger up, not armed: this is also the end of any drag — the
         * per-step backlight changes already happened.  Nothing to do here;
         * the accumulator is reset on the next press. */
        s_drag_axis      = 0;
        s_drag_travel_px = 0;
        /* Keep the last value visible for the album OSD's idle interval. */
    }
}

static void gesture_reset(void)
{
    s_tap_armed      = false;
    s_tap_busy_until = 0;
    s_paused         = false;
    s_drag_axis      = 0;
    s_drag_travel_px = 0;
    s_osd_pct        = -1;
    s_osd_until_ms   = 0;
}

/* ========================================================================
 * Frame pacing
 * ======================================================================== */
static void pace_start(void)
{
    s_next_deadline_ns = rtos_time_get_current_system_time_ns();
}

/* Sleep the remainder of the current frame slot. */
static void pace_wait(void)
{
    uint64_t now = rtos_time_get_current_system_time_ns();

    if (now < s_next_deadline_ns)
    {
        uint64_t remain_ns = s_next_deadline_ns - now;
        if (remain_ns >= 1000000ULL) /* ≥1 ms — real OS sleep */
            rtos_time_delay_ms((uint32_t) (remain_ns / 1000000ULL));
        else /* <1 ms — busy-spin (OS tick too coarse for 30 fps) */
            while (rtos_time_get_current_system_time_ns() < s_next_deadline_ns)
            {
            }
    }

    s_next_deadline_ns += FRAME_INTERVAL_NS;
}

/* ========================================================================
 * Runner — one clip, until the 3-finger tap or the clip ends
 * ======================================================================== */
static int play_video(const sd_video_t* video, uint32_t fb0, uint32_t fb1)
{
    jpeg_session_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.out_w = MJPEG_SCREEN_W;
    cfg.out_h = MJPEG_SCREEN_H;
    /* Output format is RGB32 via jpeg_decode's PP_OUT_PIX_FMT (matches the
     * ARGB8888 LCDC).  Not exposed on the session API. */

    jpeg_session_t session = jpeg_session_open(&cfg);
    if (session == NULL)
    {
        RTK_LOGE(TAG, "open session failed for '%s'\n", video->name);
        return 0;
    }

    int nframes = album_sd_video_prepare(video);
    if (nframes <= 0)
    {
        RTK_LOGE(TAG, "no frames in '%s'\n", video->name);
        jpeg_session_close(session);
        return 0;
    }

    gesture_reset();

    /* Decode target = the FB the LCDC is NOT scanning right now.  The FRD
     * swap is 60 Hz; the first flush_now lands the first decoded frame. */
    uint32_t active = lcdc_core_get_active_fb();
    uint32_t target = (active != fb0) ? fb0 : fb1;
    int      fi     = 0; /* 0-based frame index in the clip       */
    int      shown  = 0; /* frames actually presented             */

    RTK_LOGI(TAG, "--> play '%s' %d frames @ %dfps\n", video->name, nframes, MJPEG_PLAY_FPS);

    /* Deadline starts now; the first frame's decode cost falls inside the
     * first 30 fps slot, and every pace_wait() after it aligns the cadence. */
    pace_start();

    while (s_run)
    {
        /* Gestures are polled FIRST, on every iteration — including while
         * paused, where the screen is frozen and the 3-finger exit must still
         * work.  (The GT911 work task keeps feeding s_lvgl_touch_data while
         * LVGL is stopped, so this is a driver-level read, not an lv_* call.) */
        tap_tracker_tick();
        if (s_osd_pct >= 0 &&
            rtos_time_get_current_system_time_ms() >= s_osd_until_ms)
            s_osd_pct = -1;
        if (touch_gt911_get_three_tap())
        {
            RTK_LOGI(TAG, "3-finger tap — return to launcher\n");
            s_exit_code = 1;
            break;
        }

        if (s_paused)
        {
            /* Paused — freeze on the current still; do NOT advance fi. */
            rtos_time_delay_ms(20);
            continue;
        }

        uint32_t       len   = 0;
        const uint8_t* frame = album_sd_video_frame(fi % nframes, &len);
        if (frame == NULL || len == 0)
        {
            RTK_LOGE(TAG, "frame %d read failed — skip\n", fi % nframes);
            fi++;
            rtos_time_delay_ms(10);
            continue;
        }

        if (jpeg_session_decode_frame(session, frame, len, target) != 0)
        {
            RTK_LOGE(TAG, "frame %d decode failed — skip\n", fi % nframes);
            fi++;
            rtos_time_delay_ms(2);
            continue;
        }
        shown++;

        if (s_osd_pct >= 0)
            brightness_osd_draw(target, MJPEG_SCREEN_W, MJPEG_SCREEN_H,
                                (int) s_osd_pct);

        /* Make the freshly-decoded FB cache-coherent, then hand it to the
         * LCDC.  FRD swaps the DMA pointer at the frame boundary. */
        DCache_Clean(target, (uint32_t) MJPEG_SCREEN_W * MJPEG_SCREEN_H * 4u);
        lcdc_core_flush_now(target);

        /* Next frame goes into the other (now invisible) FB. */
        target = (target != fb0) ? fb0 : fb1;

        pace_wait();
        fi++;

        /* Clip loops forever (fi % nframes).  Per the launcher model, the
         * video ends only on a three-finger tap. */
    }

    RTK_LOGI(TAG, "<-- stop '%s' (%d frames shown)\n", video->name, shown);
    jpeg_session_close(session);
    return s_exit_code;
}

/* ========================================================================
 * Task + public API
 * ======================================================================== */
static void mjpeg_play_task(void* param)
{
    int index = (int) (intptr_t) param;

    if (index < 0 || index >= s_video_count)
    {
        RTK_LOGE(TAG, "bad video index %d (have %d)\n", index, s_video_count);
        goto out;
    }

    uint32_t fb0 = 0, fb1 = 0;
    lcd_get_fb_base(&fb0, &fb1);

    s_exit_code = 0;
    play_video(&s_videos[index], fb0, fb1);

out:
    s_active = false;
    s_run    = false;
    RTK_LOGI(TAG, "playback ended (exit=%d), display released\n", s_exit_code);

    /* NB: must stay in the player task, AFTER s_active cleared.  The launcher
     * poll sees the falling edge and re-asserts the launcher over the frozen
     * last video frame.  A photo decode here would flash the album — the
     * launcher_show() path does the teardown + repaint instead. */
    if (s_playback_end_cb != NULL)
        s_playback_end_cb();

    rtos_task_delete(NULL);
}

int mjpeg_play(int index)
{
    if (s_active)
    {
        RTK_LOGI(TAG, "already playing\n");
        return s_exit_code;
    }
    if (index < 0 || index >= s_video_count)
        return -1;

    s_exit_code = 0;
    s_active    = true;
    s_run       = true;

    if (rtos_task_create(NULL, "mjpeg_play", (rtos_task_t) mjpeg_play_task,
                         (void*) (intptr_t) index, 4096, 3) != RTK_SUCCESS)
    {
        RTK_LOGE(TAG, "create play task failed\n");
        s_active = false;
        s_run    = false;
        return -1;
    }
    return 0;
}

bool mjpeg_player_is_active(void)
{
    return s_active;
}

void mjpeg_playback_end_cb_set(void (*cb)(void))
{
    s_playback_end_cb = cb;
}

int mjpeg_play_exit_code(void)
{
    return s_exit_code;
}
