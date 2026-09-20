/*
 * album_ui.c — fullscreen photo viewer (HW JPEG decode straight into LCDC FBs)
 *
 * Pixel ownership (same ~3 MB PSRAM budget as pc_dashboard_demo):
 *   The LCDC dual-FB pool already fills .psram_heap.start (2 × 1.5 MB), so
 *   there is NO room for a separate canvas buffer.  Instead the PP block
 *   DMA-decodes each photo DIRECTLY into BOTH framebuffers, full screen
 *   (800 × 480).
 *
 *   LVGL runs in RENDER_MODE_DIRECT, so it repaints only its own dirty rects
 *   — exactly the LVGL "FPS monitor" pattern: one small OPAQUE widget owning a
 *   few lines, everything else left untouched.  The info bar is that widget:
 *   it is not translucent (a translucent strip re-blends over itself on every
 *   repaint) and it owns rows 0..ALBUM_STATUSBAR_H-1 while visible.
 *
 *   Hiding it must hand those rows BACK to PP: LVGL erases the vacated strip
 *   with the screen background, so bar_hide() flags s_photo_dirty and the
 *   REFR_READY hook re-runs the PP decode over the full 480 rows before that
 *   frame flips.  Showing needs no decode — the FBs hold a clean full-screen
 *   photo and LVGL just paints the bar over it.
 *
 *   Gestiures: L/R = prev/next, up/down = backlight brightness (with OSD),
 *   double-tap = toggle cover/letterbox fit, tap = wake bar (auto-hides
 *   after ALBUM_BAR_AUTOHIDE_MS), stationary long-press = slideshow.
 *   LONG_PRESSED fires 400 ms into the hold (before release), so a slow
 *   swipe used to mis-fire the slideshow; the action is therefore DEFERRED
 *   to release and gated on "finger stayed put + no gesture fired".
 *
 * Photo sources: an SD card (VFS + FatFS, storage/album_sd.c) when a readable
 * card is present, else the flash C-array table (assets/photos/album_photos.h,
 * from tools/jpg2album.py).  Both feed the same HW decode path.
 */

#include "ui/album_ui.h"
#include "config/album_config.h"
#include "config/threshold_config.h"  /* BL_STEP_PCT for brightness gestures */
#include "core/jpeg_decode.h"
#include "assets/photos/album_photos.h" /* flash fallback album (builtin) */
#include "storage/album_sd.h"            /* SD-card album (VFS+FatFS)      */
#include "hal/backlight_ctrl.h"
#include "hal/lcd/lcd_drv.h"

#include <stdio.h>  /* snprintf */
#include <string.h> /* memcpy / memset (letterbox compositing) */

#include "ameba_soc.h"
#include "os_wrapper.h"
#include "log.h"

#ifndef TAG
#define TAG "ALBUM_UI"
#endif

/* Photo region: FULL screen — the info bar floats over the photo's top rows
 * (and hands them back to PP while hidden). */
#define PHOTO_W ALBUM_SCREEN_W
#define PHOTO_H ALBUM_SCREEN_H

/* ========================================================================
 * UI state
 * ======================================================================== */
static uint32_t s_fb[2] = {0, 0}; /* LCDC framebuffer bases (PSRAM) */

/* SD album first, flash C-array table as fallback. */
static bool s_use_sd = false;

static lv_obj_t* s_info_bar   = NULL;
static lv_obj_t* s_info_label = NULL;
static lv_obj_t* s_hint_label = NULL;

static int  s_cur_index = -1;
static bool s_has_photo = false;

/* Source JPEG dimensions, from the header probe; 0 until the first render. */
static uint32_t s_cur_w = 0;
static uint32_t s_cur_h = 0;

static lv_timer_t* s_slideshow_timer = NULL;
static bool        s_slideshow_on    = false;

/* Info bar overlay state (FPS-monitor pattern: LVGL owns ONLY the bar rect) */
static bool        s_bar_visible    = true;  /* bar drawn by LVGL?           */
static uint8_t     s_fit_mode       = FIT_COVER; /* FIT_COVER / FIT_LETTERBOX */
static lv_timer_t* s_autohide_timer = NULL;  /* periodic auto-hide check     */
static uint32_t    s_last_input_ms  = 0;     /* bar auto-hide reference tick */

/* SD-unreadable recovery dialog.  NULL when not shown.  One at a time. */
static lv_obj_t* s_recover_msgbox = NULL;
static void show_sd_recover_dialog(void); /* defined below the timers */

/* Double-tap detection, ported from pc_dashboard touch_gesture.c: LVGL's
 * DOUBLE_CLICKED is only sent to the clicked object, never to the indev, so
 * an indev-level callback can never see it.  We detect the 2nd SHORT_CLICKED
 * ourselves — a tap within DOUBLE_TAP_TIMEOUT_MS and DOUBLE_TAP_DIST_MAX of
 * the previous one counts as a double tap. */
#define DOUBLE_TAP_TIMEOUT_MS  400
#define DOUBLE_TAP_DIST_MAX    30
static uint32_t s_last_click_tick = 0;
static int16_t  s_last_click_x    = 0;
static int16_t  s_last_click_y    = 0;

/* Deferred long-press (anti swipe-misfire).  LVGL fires LONG_PRESSED 400 ms
 * INTO the hold — a slow vertical swipe passes that mark long before the
 * GESTURE comes out, so acting on it directly starts the slideshow on every
 * deliberate drag.  Instead LONG_PRESSED only arms s_lp_armed here; the
 * action runs on RELEASED and only if the finger never left the press point
 * (s_press_x/y) and no swipe gesture was claimed meanwhile (s_gesture_fired). */
#define LONG_PRESS_MOVE_MAX  24 /* px — beyond this the hold was a drag */
static bool   s_lp_armed       = false;
static bool   s_gesture_fired  = false;
static int16_t s_press_x       = 0;
static int16_t s_press_y       = 0;

/* Brightness OSD (ported from pc_dashboard gpio_control.c brightness_osd_show).
 * NOTE: unlike the dashboard's translucent panel we MUST stay OPAQUE here —
 * LVGL only owns its dirty rects over PP-owned photo pixels, so a partial
 * alpha would blend against the black screen bg, not the photo. */
static lv_obj_t* s_osd       = NULL;
static lv_obj_t* s_osd_bar   = NULL;
static lv_obj_t* s_osd_label = NULL;

/* PP owes a photo re-decode: set when LVGL painted over PP-owned photo rows
 * (bar-hide erase).  The REFR_READY hook decodes the photo back into BOTH
 * FBs before that frame flips to screen. */
static bool s_photo_dirty = false;

/* ========================================================================
 * Forward declarations
 * ======================================================================== */
static void render_current_photo(void);

/* ========================================================================
 * Photo source accessors — SD album or flash table.  album_sd_at() returns a
 * pointer into a shared PSRAM buffer that the next call overwrites, so a photo
 * is decoded as soon as it is fetched.
 * ======================================================================== */
static int photo_count(void)
{
    return s_use_sd ? album_sd_count() : album_photo_count();
}

static const uint8_t* photo_data(int index, uint32_t* len, const char** name)
{
    if (s_use_sd)
    {
        const sd_photo_t* p = album_sd_at(index);
        if (p == NULL)
            return NULL;
        *len  = p->len;
        *name = p->name;
        return p->data;
    }
    else
    {
        const album_photo_t* p = album_photo_at(index);
        if (p == NULL)
            return NULL;
        *len  = p->len;
        *name = p->name;
        return p->data;
    }
}

/* ========================================================================
 * Info bar
 * ======================================================================== */
static void info_bar_update(void)
{
    char buf[128];
    int count = photo_count();

    /* Label text stays inside the Montserrat ASCII glyph range */
    if (count <= 0)
    {
        lv_label_set_text(s_info_label, s_use_sd
                          ? "No photos on SD - use JPG folder"
                          : "No photos - run tools/jpg2album.py");
    }
    else
    {
        const char* name = s_use_sd ? album_sd_name(s_cur_index)
                                    : ((album_photo_at(s_cur_index) != NULL)
                                       ? album_photo_at(s_cur_index)->name : NULL);
        if (s_cur_w > 0 && s_cur_h > 0)
        {
            snprintf(buf, sizeof(buf), "%d/%d   [%s] %s %dx%d   [%s]",
                     s_cur_index + 1, count,
                     s_use_sd ? "SD" : "FLASH",
                     (name != NULL) ? name : "?",
                     (int) s_cur_w, (int) s_cur_h,
                     s_fit_mode == FIT_COVER ? "cover" : "fit");
        }
        else
        {
            snprintf(buf, sizeof(buf), "%d/%d   [%s] %s   [%s]",
                     s_cur_index + 1, count,
                     s_use_sd ? "SD" : "FLASH",
                     (name != NULL) ? name : "?",
                     s_fit_mode == FIT_COVER ? "cover" : "fit");
        }
        lv_label_set_text(s_info_label, buf);
    }

    lv_label_set_text(s_hint_label,
                      s_slideshow_on ? "slideshow ON, hold to stop"
                                     : "L/R photo  2x fit  hold play  U/D bright");
}

/* Fires after LVGL finishes a repaint, before the buffer is flipped to screen.
 * If LVGL painted over PP-owned photo rows (bar hide erase), decode the photo
 * back now — LCDC still scans the OTHER buffer, so this is not visible. */
static void refr_ready_hook(lv_event_t* e)
{
    LV_UNUSED(e);
    if (s_photo_dirty && s_has_photo)
    {
        /* OSD is painted over the photo rows — a decode now would wipe it.
         * Keep the flag; osd_hide_del() re-decodes once the OSD is gone. */
        if (s_osd != NULL)
            return;
        /* Recovery dialog up = PP held; re-decode later (flag stays set). */
        if (s_recover_msgbox != NULL)
            return;
        render_current_photo();
        s_photo_dirty = false;
    }
}

/* ========================================================================
 * Photo rendering — PP decode into both LCDC framebuffers
 * ======================================================================== */

/* Letterbox scratch: PP scales the image aspect-true into this tight buffer,
 * the CPU then blits it centered into the FBs and fills the bars black.  The
 * PP writes tight FB rows only (no output stride), so side bars cannot be
 * produced in HW — CPU compositing is the way.  PSRAM has ~13 MB free after
 * the 2×1.5 MB FB pool (PSRAM_END=0x61000000), so 1.5 MB scratch is free. */
#define LB_SCRATCH_MAX (PHOTO_W * PHOTO_H * 4)
__attribute__((section(".psram_heap.start"), aligned(64)))
static uint8_t s_lb_scratch[LB_SCRATCH_MAX];

/* Aspect-fit src into PHOTO_W×PHOTO_H (width-bound or height-bound), honouring
 * PP scale limits (≤3× up → rejected, min dim 16, out W%8 / H%2).
 * @return false when the source would need >3× upscale (caller falls back). */
static bool letterbox_scale(uint32_t srcW, uint32_t srcH, uint32_t* fw, uint32_t* fh)
{
    uint32_t w, h;

    if ((uint64_t) srcW * PHOTO_H >= (uint64_t) srcH * PHOTO_W)
    {
        /* Relatively wider than screen → width-bound */
        w = PHOTO_W;
        h = (uint32_t) (((uint64_t) PHOTO_W * srcH) / srcW);
    }
    else
    {
        /* Relatively taller → height-bound */
        h = PHOTO_H;
        w = (uint32_t) (((uint64_t) PHOTO_H * srcW) / srcH);
    }

    /* PP upscales at most 3× — tiny sources are not worth compositing */
    if (w > 3u * srcW || h > 3u * srcH)
        return false;

    w &= ~7u; /* PP output width multiple of 8 */
    h &= ~1u; /* PP output height multiple of 2 */
    if (w < 16u || h < 16u)
        return false;

    *fw = w;
    *fh = h;
    return true;
}

/* CPU-composited ARGB8888 blit of a tight fw×fh image, centered in the FB,
 * black bars around (LCDC/DMA has no output stride → side bars can't be HW).
 *
 * Every FB byte is written here (bars + picture), so the trailing Clean
 * pushes a complete, coherent frame to PSRAM for the LCDC DMA — stale dirty
 * lines from LVGL's earlier bar draws are fully overwritten before flushing.
 * (The scratch buffer is a different story — PP DMA-writes it, see the
 * DCache_Invalidate at the caller.) */
static void blit_centered_argb(uint32_t fb, const uint8_t* src, uint32_t fw, uint32_t fh)
{
    const uint32_t row_bytes = fw * 4u;
    uint32_t x0 = (ALBUM_SCREEN_W - fw) / 2u;
    uint32_t y0 = (ALBUM_SCREEN_H - fh) / 2u;
    uint32_t addr = fb;

    memset((void*) addr, 0, (size_t) y0 * ALBUM_SCREEN_W * 4u);
    addr += (size_t) y0 * ALBUM_SCREEN_W * 4u;

    for (uint32_t y = 0; y < fh; y++)
    {
        memset((void*) addr, 0, x0 * 4u);
        memcpy((void*) (addr + x0 * 4u), src + (size_t) y * row_bytes, row_bytes);
        memset((void*) (addr + x0 * 4u + row_bytes), 0,
               (ALBUM_SCREEN_W - x0 - fw) * 4u);
        addr += (size_t) ALBUM_SCREEN_W * 4u;
    }

    memset((void*) addr, 0, (size_t) (ALBUM_SCREEN_H - y0 - fh) * ALBUM_SCREEN_W * 4u);

    DCache_Clean(fb, (uint32_t) ALBUM_SCREEN_W * ALBUM_SCREEN_H * 4u);
}

/* PP upscales at most 3×.  FIT_COVER crops before scaling, so the magnification
 * is set by the cropped block; a source that needs >3× as a whole can never be
 * covered, and PPSetConfig would fail with PP_SET_OUT_SIZE_INVALID. */
static bool cover_fit_possible(uint32_t srcW, uint32_t srcH)
{
    return !(PHOTO_W > 3u * srcW || PHOTO_H > 3u * srcH);
}

/* Decode one photo into both FBs — source-agnostic, any JPEG stream works.
 * FIT_COVER: PP crop-to-fill into the FBs.  FIT_LETTERBOX: aspect-true scale to
 * scratch, CPU blit + black bars.
 * @return number of FBs written (0, 1 or 2). */
static int render_photo_stream(const uint8_t* data, uint32_t len)
{
    int ok = 0;

    /* Size first: cover needs it to avoid the >3× PP rejection, and the info
     * bar shows the source dimensions. */
    uint32_t srcW = 0, srcH = 0;
    bool have_size = (jpeg_peek_size(data, len, &srcW, &srcH) == 0
                      && srcW > 0 && srcH > 0);
    s_cur_w = have_size ? srcW : 0;
    s_cur_h = have_size ? srcH : 0;
    int  mode = s_fit_mode;

    if (mode == FIT_COVER && have_size && !cover_fit_possible(srcW, srcH))
    {
        RTK_LOGI(TAG, "cover %dx%d >3x upscale -> letterbox for this photo\n",
                 (int) srcW, (int) srcH);
        mode = FIT_LETTERBOX;
    }

    uint32_t fw = 0, fh = 0;
    if (mode == FIT_LETTERBOX)
    {
        if (have_size && letterbox_scale(srcW, srcH, &fw, &fh))
        {
            RTK_LOGI(TAG, "letterbox: src %dx%d -> tight %dx%d\n",
                     (int) srcW, (int) srcH, (int) fw, (int) fh);
            jpeg_dec_req_t req = {0};
            req.jpeg_data  = data;
            req.jpeg_len   = len;
            req.out_buffer = s_lb_scratch;
            req.out_w      = fw;
            req.out_h      = fh; /* tight scratch, no FB window */
            if (jpeg_decode_to_argb8888(&req) != 0)
            {
                RTK_LOGE(TAG, "letterbox decode failed\n");
                return 0;
            }

            /* PP wrote scratch via DMA; cached lines from the PREVIOUS
             * letterbox blit (CPU read of the same addresses) would be stale. */
            DCache_Invalidate((u32) s_lb_scratch, fw * fh * 4u);
        }
        else if (s_fit_mode == FIT_LETTERBOX)
        {
            /* >3× even aspect-fitted — try cover so the photo does not vanish. */
            RTK_LOGW(TAG, "src %dx%d unfit for letterbox -> try cover\n",
                     (int) srcW, (int) srcH);
            mode = FIT_COVER;
        }
        else
        {
            /* Already degraded from cover — neither mode can show it. */
            RTK_LOGE(TAG, "src %dx%d >3x, cannot display\n",
                     (int) srcW, (int) srcH);
            return 0;
        }
    }

    for (int i = 0; i < 2; i++)
    {
        if (s_fb[i] == 0)
            continue;

        if (mode == FIT_LETTERBOX)
        {
            blit_centered_argb(s_fb[i], s_lb_scratch, fw, fh);
            ok++;
            continue;
        }

        jpeg_dec_req_t req = {0};
        req.jpeg_data  = data;
        req.jpeg_len   = len;
        req.out_buffer = (void*) s_fb[i]; /* full-screen tight FB write */
        req.out_w      = PHOTO_W;
        req.out_h      = PHOTO_H;
        req.fit_mode   = FIT_COVER;

        if (jpeg_decode_to_argb8888(&req) == 0)
            ok++;
        else
            RTK_LOGE(TAG, "decode into FB%d failed\n", i);
    }

    /* PP just overwrote the bar rows in both FBs — if the bar is visible,
     * hand its rect back to LVGL so the next frame repaints it.  Same for
     * the transient brightness OSD (slideshow advance can land mid-OSD). */
    if (ok > 0 && s_bar_visible && s_info_bar != NULL)
        lv_obj_invalidate(s_info_bar);
    if (ok > 0 && s_osd != NULL)
        lv_obj_invalidate(s_osd);

    return ok;
}

/* Decode the current photo, whichever source it came from. */
static void render_current_photo(void)
{
    uint32_t    len  = 0;
    const char* name = NULL;
    const uint8_t* data = photo_data(s_cur_index, &len, &name);
    RTK_LOGI(TAG, "photo #%d [%s/%s] len=%u\n", s_cur_index,
             s_use_sd ? "sd" : "flash", (name != NULL) ? name : "?",
             (unsigned) len);
    if (data != NULL && len > 0)
    {
        render_photo_stream(data, len);
    }
    else
    {
        RTK_LOGE(TAG, "photo #%d fetch failed\n", s_cur_index);
    }
}

/* ========================================================================
 * Info bar visibility (FPS-monitor style: LVGL owns ONLY the bar rect)
 * ======================================================================== */
static void bar_hide(void)
{
    if (!s_bar_visible)
        return;
    s_bar_visible = false;
    lv_obj_add_flag(s_info_bar, LV_OBJ_FLAG_HIDDEN);
    /* LVGL now erases the vacated rect with the screen bg → those PP photo
     * rows are gone from this frame's buffer; flag so the REFR_READY hook
     * re-decodes the full frame into BOTH FBs. */
    s_photo_dirty = true;
    RTK_LOGI(TAG, "info bar hidden\n");
}

static void bar_show(void)
{
    if (s_bar_visible)
        return;
    /* FBs currently hold a clean full-screen photo (PP owned all rows), so
     * unhiding needs no PP decode — LVGL paints the bar over it next frame. */
    s_bar_visible = true;
    lv_obj_clear_flag(s_info_bar, LV_OBJ_FLAG_HIDDEN);
    s_last_input_ms = lv_tick_get();
    RTK_LOGI(TAG, "info bar shown\n");
}

static void autohide_timer_cb(lv_timer_t* timer)
{
    LV_UNUSED(timer);
    if (s_bar_visible && s_has_photo &&
        lv_tick_elaps(s_last_input_ms) >= ALBUM_BAR_AUTOHIDE_MS)
    {
        bar_hide();
    }

    /* SD card-detect poll at the bar auto-hide cadence (500 ms). */
    album_sd_poll_result_t poll = album_sd_cd_poll();
    if (poll == SD_POLL_INSERTED || poll == SD_POLL_REMOVED)
    {
        /* Either edge closes the recovery dialog and releases the held PP, so
         * the screen is never left black. */
        bool was_recovering = (s_recover_msgbox != NULL);
        if (s_recover_msgbox != NULL)
        {
            lv_msgbox_close(s_recover_msgbox);
            s_recover_msgbox = NULL;
        }

        bool had_sd = s_use_sd;
        s_use_sd = (album_sd_count() > 0);
        RTK_LOGI(TAG, "SD table changed (poll=0x%x) -> source %s (%d photos)\n",
                 (int) poll, s_use_sd ? "SD" : "flash", photo_count());

        if (s_use_sd != had_sd || was_recovering)
        {
            s_cur_index = 0; /* reset navigation to the new source's start */
            if (photo_count() > 0)
                album_show_photo(0);
        }
        info_bar_update();
    }
    else if (poll == SD_POLL_INSERT_FAILED)
    {
        /* Re-seated and still unreadable: stop the slideshow (PP goes back on
         * hold with the dialog) and ask again. */
        if (s_slideshow_on)
            album_slideshow_toggle();
        if (s_recover_msgbox == NULL)
            show_sd_recover_dialog();
    }
}

/* Double-tap feedback — lv_anim mechanism ported from pc_dashboard's
 * brightness OSD (gpio_control.c): there a transient overlay fades via
 * style_opa; here LVGL owns ONLY the bar rect (PP owns the photo pixels),
 * so instead of fading widget alpha we mix bg_color highlight → black.
 * The bar stays fully OPAQUE throughout — no photo rows bleed through. */
static void bar_flash_exec_cb(void* var, int32_t v)
{
    lv_color_t mixed = lv_color_mix(lv_color_make(0x1E, 0x46, 0x5A),
                                    lv_color_black(), (lv_opa_t) v);
    lv_obj_set_style_bg_color((lv_obj_t*) var, mixed, 0);
}

static void bar_flash_pulse(void)
{
    if (s_info_bar == NULL)
        return;

    lv_anim_delete(s_info_bar, NULL); /* re-trigger on rapid double-taps */

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_info_bar);
    lv_anim_set_exec_cb(&a, bar_flash_exec_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP); /* highlight → black */
    lv_anim_set_time(&a, 400);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, NULL);
    lv_anim_start(&a);
}

static void album_fit_mode_toggle(void)
{
    s_fit_mode = (s_fit_mode == FIT_COVER) ? FIT_LETTERBOX : FIT_COVER;
    RTK_LOGI(TAG, "fit mode → %s\n",
             s_fit_mode == FIT_COVER ? "cover" : "letterbox");
    render_current_photo();
    info_bar_update(); /* label carries the [cover]/[fit] tag */
    bar_flash_pulse();
}

void album_show_photo(int index)
{
    int count = photo_count();
    if (count <= 0)
        return;

    /* PP on hold: a full-screen decode would paint over the modal dialog. */
    if (s_recover_msgbox != NULL)
        return;

    /* Wrap around */
    index %= count;
    if (index < 0)
        index += count;

    uint32_t    len  = 0;
    const char* name = NULL;
    const uint8_t* data = photo_data(index, &len, &name);
    if (data == NULL || len == 0)
    {
        RTK_LOGE(TAG, "photo %d invalid\n", index);
        return;
    }

    RTK_LOGI(TAG, "show #%d '%s' (%d bytes)\n", index,
             name ? name : "?", (int) len);

    s_cur_index = index;

    if (render_photo_stream(data, len) == 0)
    {
        RTK_LOGE(TAG, "decode failed for #%d\n", index);
        info_bar_update();
        return;
    }

    s_has_photo   = true;
    s_photo_dirty = false; /* photo region is up to date in both FBs now */
    /* Only the info-bar label changes → LVGL dirties just that strip. */
    info_bar_update();
}

void album_next_photo(void)
{
    album_show_photo(s_cur_index + 1);
}

void album_prev_photo(void)
{
    album_show_photo(s_cur_index - 1);
}

bool album_has_photo(void)
{
    return s_has_photo;
}

/* ========================================================================
 * Slideshow
 * ======================================================================== */
static void slideshow_timer_cb(lv_timer_t* timer)
{
    LV_UNUSED(timer);
    album_next_photo();
}

void album_slideshow_toggle(void)
{
    s_slideshow_on = !s_slideshow_on;

    if (s_slideshow_on && s_slideshow_timer == NULL)
    {
        s_slideshow_timer = lv_timer_create(slideshow_timer_cb,
                                            ALBUM_SLIDESHOW_INTERVAL_MS, NULL);
    }
    else if (s_slideshow_timer != NULL)
    {
        lv_timer_delete(s_slideshow_timer);
        s_slideshow_timer = NULL;
    }

    RTK_LOGI(TAG, "slideshow %s\n", s_slideshow_on ? "ON" : "OFF");
    info_bar_update();
}

/* ========================================================================
 * Brightness OSD — ported from pc_dashboard gpio_control.c
 * (brightness_osd_show / osd_fadein_cb / osd_fadeout_cb).  Differences:
 * OPAQUE bg (DIRECT-mode pixel ownership), and deleting it marks the photo
 * dirty because LVGL erases the vacated rect with the black screen bg.
 * ======================================================================== */
static void osd_fade_cb(void* var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t*) var, (lv_opa_t) v, 0);
}

static void osd_close(void)
{
    if (s_osd == NULL)
        return;
    lv_anim_delete(s_osd, NULL);
    lv_obj_delete(s_osd);
    s_osd = NULL;
    s_osd_bar = NULL;
    s_osd_label = NULL;
    /* LVGL will erase the OSD rect with the screen bg → PP rows lost. */
    s_photo_dirty = true;
}

static void osd_fadeout_ready_cb(lv_anim_t* a)
{
    LV_UNUSED(a);
    osd_close();
}

static void osd_show(int percent)
{
    if (s_osd == NULL)
    {
        lv_obj_t* scr = lv_scr_act();

        s_osd = lv_obj_create(scr);
        lv_obj_remove_style_all(s_osd);
        lv_obj_set_size(s_osd, 180, 44);
        lv_obj_align(s_osd, LV_ALIGN_BOTTOM_MID, 0, -12);
        lv_obj_set_style_radius(s_osd, 22, 0);
        lv_obj_set_style_bg_color(s_osd, lv_color_make(0x10, 0x10, 0x10), 0);
        lv_obj_set_style_bg_opa(s_osd, LV_OPA_COVER, 0); /* OPAQUE, see note */
        lv_obj_remove_flag(s_osd, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(s_osd, LV_OBJ_FLAG_CLICKABLE);

        s_osd_label = lv_label_create(s_osd);
        lv_obj_set_style_text_color(s_osd_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(s_osd_label, &lv_font_montserrat_14, 0);
        lv_obj_align(s_osd_label, LV_ALIGN_LEFT_MID, 10, 0);

        s_osd_bar = lv_bar_create(s_osd);
        lv_obj_set_size(s_osd_bar, 90, 6);
        lv_obj_align(s_osd_bar, LV_ALIGN_RIGHT_MID, -10, 0);
        lv_bar_set_range(s_osd_bar, 0, 100);
        lv_obj_set_style_anim_duration(s_osd_bar, 200, 0);
        lv_obj_set_style_bg_color(s_osd_bar, lv_color_make(0x44, 0x44, 0x44), 0);
        lv_obj_set_style_bg_color(s_osd_bar, lv_color_make(0xFF, 0xCC, 0x33),
                                  LV_PART_INDICATOR);

        /* Fade in (100 ms) */
        lv_obj_set_style_opa(s_osd, LV_OPA_TRANSP, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_osd);
        lv_anim_set_exec_cb(&a, osd_fade_cb);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_time(&a, 100);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
    else
    {
        lv_anim_delete(s_osd, NULL); /* repeated swipe: restart fade-out */
        lv_obj_set_style_opa(s_osd, LV_OPA_COVER, 0);
    }

    lv_label_set_text_fmt(s_osd_label, "%d %%", (int) percent);
    lv_bar_set_value(s_osd_bar, percent, LV_ANIM_ON);

    /* Auto fade-out (300 ms) after 1.5 s idle */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_osd);
    lv_anim_set_exec_cb(&a, osd_fade_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_delay(&a, 1500);
    lv_anim_set_ready_cb(&a, osd_fadeout_ready_cb);
    lv_anim_start(&a);
}

/* ========================================================================
 * Gesture / input handling (events on the INDEV, like pc_dashboard
 * touch_gesture.c — fires even when child widgets consume the press)
 * ======================================================================== */
static void input_event_cb(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);

    /* Any contact keeps the info bar alive for another auto-hide period. */
    if (code == LV_EVENT_PRESSED)
    {
        lv_point_t p;
        s_last_input_ms = lv_tick_get();
        lv_indev_get_point(lv_indev_active(), &p);
        s_press_x       = p.x;
        s_press_y       = p.y;
        s_lp_armed      = false;
        s_gesture_fired = false;
    }

    if (code == LV_EVENT_GESTURE)
    {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        s_gesture_fired = true; /* disqualifies the armed long-press */

        /* While the recovery dialog is up navigation would silently no-op
         * behind it (album_show_photo() refuses to render), so swallow the
         * gesture. */
        if (s_recover_msgbox != NULL)
            return;

        /* With one photo, sideways swipes would only re-decode #0. */
        bool swipe_switches = photo_count() > 1;

        switch (dir)
        {
            case LV_DIR_LEFT:
                if (swipe_switches)
                    album_next_photo();
                else
                    info_bar_update(); /* reflect same photo, no re-decode */
                break;
            case LV_DIR_RIGHT:
                if (swipe_switches)
                    album_prev_photo();
                else
                    info_bar_update();
                break;
            case LV_DIR_TOP:
                backlight_adjust(BL_STEP_PCT);
                RTK_LOGI(TAG, "swipe UP -> brightness %d%%\n", backlight_get());
                osd_show(backlight_get());
                break;
            case LV_DIR_BOTTOM:
                backlight_adjust(-BL_STEP_PCT);
                RTK_LOGI(TAG, "swipe DOWN -> brightness %d%%\n", backlight_get());
                osd_show(backlight_get());
                break;
            default: break;
        }
    }
    else if (code == LV_EVENT_LONG_PRESSED)
    {
        /* Only ARM here — LVGL fires this 400 ms into the hold, while a slow
         * swipe is still mid-flight.  The decision happens on RELEASED. */
        s_lp_armed = true;
    }
    else if (code == LV_EVENT_RELEASED)
    {
        if (s_lp_armed)
        {
            s_lp_armed = false;
            lv_point_t p;
            lv_indev_get_point(lv_indev_active(), &p);
            int32_t dx = p.x - s_press_x;
            int32_t dy = p.y - s_press_y;
            if (!s_gesture_fired &&
                dx * dx + dy * dy < LONG_PRESS_MOVE_MAX * LONG_PRESS_MOVE_MAX)
            {
                album_slideshow_toggle(); /* stationary hold confirmed */
            }
        }
    }
    else if (code == LV_EVENT_SHORT_CLICKED)
    {
        /* Tap wakes the bar (no-op when already visible). */
        if (!s_bar_visible)
        {
            s_last_input_ms = lv_tick_get();
            bar_show();
        }

        /* Manual double-tap → fit-mode toggle (see note on the defines). */
        uint32_t now = lv_tick_get();
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);

        int32_t dx = p.x - s_last_click_x;
        int32_t dy = p.y - s_last_click_y;
        uint32_t elapsed = now - s_last_click_tick;

        if (elapsed < DOUBLE_TAP_TIMEOUT_MS &&
            dx * dx + dy * dy < DOUBLE_TAP_DIST_MAX * DOUBLE_TAP_DIST_MAX)
        {
            s_last_click_tick = 0; /* consume: a 3rd tap starts a fresh pair */
            album_fit_mode_toggle();
        }
        else
        {
            s_last_click_tick = now;
            s_last_click_x    = p.x;
            s_last_click_y    = p.y;
        }
    }
}

static void register_input_events(void)
{
    lv_indev_t* indev = lv_indev_get_next(NULL);
    if (indev == NULL)
    {
        RTK_LOGE(TAG, "No indev available — touch input disabled\n");
        return;
    }

    lv_indev_add_event_cb(indev, input_event_cb, LV_EVENT_PRESSED, NULL);
    lv_indev_add_event_cb(indev, input_event_cb, LV_EVENT_RELEASED, NULL);
    lv_indev_add_event_cb(indev, input_event_cb, LV_EVENT_GESTURE, NULL);
    lv_indev_add_event_cb(indev, input_event_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_indev_add_event_cb(indev, input_event_cb, LV_EVENT_LONG_PRESSED, NULL);
}

/* ---- SD-unreadable recovery dialog ----
 * A card that stops answering commands cannot be recovered in software on this
 * board (it is powered continuously), so ask for a physical re-plug or fall
 * back to the flash album instead of retrying. */
#define RECOVER_CHOICE_REPLUG   0
#define RECOVER_CHOICE_FALLBACK 1

static void recover_dialog_action_cb(lv_event_t* e)
{
    intptr_t choice = (intptr_t) lv_event_get_user_data(e);

    if (s_recover_msgbox != NULL)
    {
        lv_msgbox_close(s_recover_msgbox);
        s_recover_msgbox = NULL;
    }

    /* PP comes off hold as the dialog closes.  RE-PLUG keeps it up and waits
     * for the remove→insert edge that mounts the card. */
    if (choice == RECOVER_CHOICE_FALLBACK)
    {
        RTK_LOGI(TAG, "SD unreadable -> flash fallback\n");
        if (photo_count() > 0)
            album_show_photo(0);
    }
    else
    {
        RTK_LOGI(TAG, "SD unreadable -> waiting for re-plug\n");
    }
}

static void show_sd_recover_dialog(void)
{
    lv_obj_t* mbox = lv_msgbox_create(NULL);
    lv_msgbox_add_title(mbox, "SD card unreadable");
    lv_msgbox_add_text(mbox,
                       "Card detected but could not be read.\n"
                       "Please re-plug the SD card, or use the built-in album.");

    lv_obj_t* btn_replug = lv_msgbox_add_footer_button(mbox, "Re-plug SD");
    lv_obj_add_event_cb(btn_replug, recover_dialog_action_cb, LV_EVENT_CLICKED,
                        (void*) (intptr_t) RECOVER_CHOICE_REPLUG);

    lv_obj_t* btn_flash = lv_msgbox_add_footer_button(mbox, "Flash Album");
    lv_obj_add_event_cb(btn_flash, recover_dialog_action_cb, LV_EVENT_CLICKED,
                        (void*) (intptr_t) RECOVER_CHOICE_FALLBACK);

    s_recover_msgbox = mbox;
}

/* ========================================================================
 * UI construction
 * ======================================================================== */
void album_ui_init(void)
{
    lv_obj_t* scr = lv_scr_act();

    /* Framebuffer bases (both photo region + LVGL DIRECT draw target) */
    lcd_get_fb_base(&s_fb[0], &s_fb[1]);
    RTK_LOGI(TAG, "FB0=0x%08x FB1=0x%08x\n",
             (unsigned int) s_fb[0], (unsigned int) s_fb[1]);

    /* ---- Info bar overlay (top strip, floats OVER the full-screen photo) ----
     * OPAQUE, not translucent: in DIRECT mode the bar rect is re-painted on
     * top of PP's photo pixels, and a translucent bg would re-blend over
     * itself on every repaint (progressively darker).  FPS-monitor pattern. */
    s_info_bar = lv_obj_create(scr);
    lv_obj_remove_style_all(s_info_bar);
    lv_obj_set_size(s_info_bar, ALBUM_SCREEN_W, ALBUM_STATUSBAR_H);
    lv_obj_align(s_info_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_hor(s_info_bar, 10, 0);

    /* Fixed look (theme system removed): black letterbox/screen bg, white
     * text.  Screen bg only shows before the first photo decodes. */
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_set_style_bg_color(s_info_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_info_bar, LV_OPA_COVER, 0);

    s_info_label = lv_label_create(s_info_bar);
    lv_obj_align(s_info_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(s_info_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_info_label, &lv_font_montserrat_20, 0);

    s_hint_label = lv_label_create(s_info_bar);
    lv_obj_align(s_hint_label, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_color(s_hint_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_hint_label, &lv_font_montserrat_14, 0);

    /* ---- Events: gestures + tap/long-press on the indev ---- */
    register_input_events();

    /* ---- Restore photo after any full-screen LVGL repaint ---- */
    lv_display_add_event_cb(lv_display_get_default(), refr_ready_hook,
                            LV_EVENT_REFR_READY, NULL);

    /* ---- Info bar auto-hide watchdog (idle check, cheap) ---- */
    s_last_input_ms  = lv_tick_get();
    s_autohide_timer = lv_timer_create(autohide_timer_cb, 500, NULL);

    /* ---- Photo source: SD first, flash table as fallback ----
     * A card that mounts but yields no photos counts as unreadable, so the user
     * gets the re-plug / flash choice either way. */
    album_sd_result_t sd_res = album_sd_init();
    if (sd_res == SD_RES_OK)
    {
        album_sd_scan();
        s_use_sd = (album_sd_count() > 0);
        RTK_LOGI(TAG, "photo source: %s (%d photos)\n",
                 s_use_sd ? "SD card" : "flash (no SD photos)", photo_count());

        if (!s_use_sd)
            show_sd_recover_dialog();
    }
    else
    {
        s_use_sd = false;
        RTK_LOGI(TAG, "photo source: flash C-array (SD %s)\n",
                 sd_res == SD_RES_NO_CARD ? "no card" : "unreadable");

        if (sd_res == SD_RES_UNREADABLE)
            show_sd_recover_dialog();
    }

    lv_refr_now(NULL);

    /* PP stays on hold while the dialog is up; the album starts when the user
     * picks fallback-flash or a later insert edge mounts the card. */
    if (s_recover_msgbox == NULL && photo_count() > 0)
        album_show_photo(0);

    RTK_LOGI(TAG, "album UI ready: %d photos, full screen %dx%d, bar %dpx\n",
             photo_count(), PHOTO_W, PHOTO_H, ALBUM_STATUSBAR_H);
}
