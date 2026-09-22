/*
 * launcher_ui.c — demo home screen + mode router (JPG album / MJPEG video)
 *
 * The launcher is a full-screen OPAQUE layer built over the album, which boots
 * with its photo table and info bar ready but nothing decoded.  Hiding the
 * layer reveals the album; a photo is decoded on the first entry (goto_album).
 *
 * Returning here is handled in this one place: a 3-finger tap is taken from
 * touch_gt911_get_three_tap() by the poll timer below, and the end of a clip
 * arrives through the player's end callback, which only flags the layer to be
 * re-shown — the repaint itself runs on the LVGL thread.
 */
#include "launcher_ui.h"

#include "config/album_config.h"
#include "core/mjpeg_player.h"
#include "hal/lcd/lcd_drv.h" /* lcd_get_fb_base — launcher FB fill */
#include "hal/touch/touch_gt911.h"
#include "ui/album_ui.h"
#include "assets/icons/icons.h" /* A8 icons — generated, tinted here */

#include <stdio.h> /* snprintf */

#include "ameba_soc.h"
#include "os_wrapper.h"
#include "log.h"

#ifndef TAG
#define TAG "LAUNCHER"
#endif

/* ---- Launcher layer + video-card subtitle ---- */
static lv_obj_t* s_launcher_layer = NULL;
static lv_obj_t* s_video_sub      = NULL; /* "MJPEG Video" card subtitle */

/* ---- Poll state ---- */
static lv_timer_t* s_poll_timer       = NULL;
static bool        s_video_was_on     = false; /* mjpeg active last poll   */
static int         s_video_last_count = -1;    /* cached video-count label */
/* Video just ended → the LVGL thread must re-assert the launcher.  The player
 * task cannot safely call lv_refr_now() (cross-thread refresh races the LVGL
 * main loop's flip handling and can wedge the display), so the end-of-playback
 * callback only sets this flag + covers the frozen frame; launcher_poll_cb,
 * running on the LVGL thread, performs the real show. */
static volatile bool s_video_end_pending = false;

/* ---- Mode the launcher is currently showing ---- */
static int s_mode = 0; /* 0 launcher / 1 album / 2 video */
#define MODE_LAUNCHER 0
#define MODE_ALBUM    1
#define MODE_VIDEO    2

static void launcher_alert(const char* title, const char* msg);

/* ========================================================================
 * Card construction
 * ======================================================================== */

/* One launcher entry card: icon image + title + subtitle, tap → cb.
 * @return card; its subtitle label is child 2 (may be relabelled via
 *         lv_obj_get_child).  The icon is an A8 alpha-mask image (see
 *         assets/icons) tinted to an accent via recolor, LVGL 9.3 style. */
static lv_obj_t* make_card(lv_obj_t* parent, const lv_image_dsc_t* icon,
                           const char* title, const char* sub, lv_event_cb_t cb)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_style_bg_color(card, lv_color_make(0x14, 0x14, 0x14), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_make(0x33, 0x33, 0x33), 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* ic = lv_image_create(card); /* child 0 — icon */
    lv_image_set_src(ic, icon);
    lv_obj_set_style_image_recolor(ic, lv_color_make(0x66, 0xB8, 0xFF), 0);
    lv_obj_set_style_image_recolor_opa(ic, LV_OPA_COVER, 0);
    lv_obj_set_pos(ic, 0, 0);

    lv_obj_t* t = lv_label_create(card); /* child 1 — title */
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_label_set_text(t, title);

    lv_obj_t* s = lv_label_create(card); /* child 2 — subtitle */
    lv_obj_set_style_text_color(s, lv_color_make(0x99, 0x99, 0x99), 0);
    lv_obj_set_style_text_font(s, &lv_font_montserrat_14, 0);
    lv_label_set_text(s, sub);
    lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* Layout: icon 72x72 at top-left (card content is tall), label column to
     * its right, subtitle pinned bottom-left of the card, under both. */
    const int icon_w = 72, label_x = icon_w + 16, label_h = 24;
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, -label_h);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, label_x, -label_h);

    return card;
}

/* ========================================================================
 * Mode routing
 *
 * Show/hide invalidates the whole layer rather than relying on LVGL's dirty
 * regions: the album's photos are PP-written into the framebuffers, which LVGL
 * does not track, so a partial repaint can leave photo pixels under the cards.
 * album_ui_set_active() gates input — the album's events live on the indev and
 * would fire even with the layer on top.
 * ======================================================================== */

/* Raw FB fill in the launcher background colour, for the same reason. */
static void fill_fb_u32(uint32_t base, int w, int h, uint32_t pixel)
{
    uint32_t* p = (uint32_t*) base;
    uint32_t  n = (uint32_t) (w * h);
    for (uint32_t i = 0; i < n; i++)
        p[i] = pixel;
    DCache_Clean(base, (uint32_t) (w * h * 4));
}

static void launcher_fill_screen(void)
{
    uint32_t b1 = 0, b2 = 0;
    int      w, h;
    lcdc_core_get_info(&w, &h);
    lcd_get_fb_base(&b1, &b2);
    if (b1 == 0)
        return;
    fill_fb_u32(b1, w, h, 0xFF080808u); /* ARGB8888 — launcher layer bg */
    if (b2 != 0)
        fill_fb_u32(b2, w, h, 0xFF080808u);
}

static void launcher_show(void)
{
    if (s_launcher_layer == NULL)
        return;
    album_ui_set_active(false); /* launcher on top: album must not react */
    launcher_fill_screen();     /* deterministically cover any photo residue */
    lv_obj_clear_flag(s_launcher_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_launcher_layer);
    s_mode = MODE_LAUNCHER;
    lv_refr_now(NULL);
    RTK_LOGI(TAG, "-> launcher\n");
}

/* End-of-playback handoff, run on the player task.  lv_refr_now() from a
 * foreign thread would race the main loop's flip logic, so this only sets a
 * flag; launcher_poll_cb does the actual show on the LVGL thread. */
static void launcher_show_pending(void)
{
    s_video_end_pending = true;
    RTK_LOGI(TAG, "video ended, launcher show pending\n");
}

static void launcher_hide(void)
{
    if (s_launcher_layer == NULL)
        return;
    lv_obj_add_flag(s_launcher_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_launcher_layer);
    lv_refr_now(NULL);
    album_ui_set_active(true); /* album visible again: restore input */
}

static void goto_album(void)
{
    if (album_ui_photo_count() <= 0)
    {
        launcher_alert("JPG Album", "No photos on SD or flash.\n"
                                    "Insert an SD card with JPG files.");
        return;
    }

    launcher_hide();
    s_mode = MODE_ALBUM;

    /* The album boots undeveloped, so the first entry decodes photo 0. */
    album_show_photo(0);
    RTK_LOGI(TAG, "-> JPG album\n");
}

static void goto_video(int index)
{
    if (index < 0)
        return;
    launcher_hide();
    s_mode = MODE_VIDEO;

    /* Both FBs are re-filled with the launcher colour: the album below was
     * never decoded, so without this the scanned FB can show residual pixels
     * for the one frame slot before the first video frame lands. */
    launcher_fill_screen();

    RTK_LOGI(TAG, "-> MJPEG video #%d\n", index);
    if (mjpeg_play(index) != 0)
        launcher_show(); /* start failed — back to launcher */
}

/* One-at-a-time launcher alert.  A modal msgbox lets the user dismiss it with
 * the "OK" footer button.  Mirrors the album's recovery dialog style. */
static lv_obj_t* s_alert_msgbox = NULL;

static void alert_msgbox_close_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    if (s_alert_msgbox != NULL)
    {
        lv_msgbox_close(s_alert_msgbox);
        s_alert_msgbox = NULL;
    }
}

static void launcher_alert(const char* title, const char* msg)
{
    if (s_alert_msgbox != NULL)
        return; /* already showing an alert — don't stack */

    lv_obj_t* mbox = lv_msgbox_create(NULL);
    if (title != NULL)
        lv_msgbox_add_title(mbox, title);
    if (msg != NULL)
        lv_msgbox_add_text(mbox, msg);

    lv_obj_t* btn = lv_msgbox_add_footer_button(mbox, "OK");
    lv_obj_add_event_cb(btn, alert_msgbox_close_cb, LV_EVENT_CLICKED, NULL);

    s_alert_msgbox = mbox;
}

/* ---- Card callbacks ---- */
static void card_jpg_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    if (album_ui_photo_count() <= 0)
    {
        launcher_alert("JPG Album", "No photos on SD or flash.\n"
                                    "Insert an SD card with JPG files.");
        return;
    }
    goto_album();
}

static void card_mjpeg_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    int n = mjpeg_video_count();
    if (n <= 0)
    {
        launcher_alert("MJPEG Video",
                       "No video folders on the SD card.\n"
                       "Copy a folder of numbered JPG frames\n"
                       "to the SD root.");
        return;
    }

    /* Single video → play it directly.  Multiple → play the first (a picker
     * can replace this later; the demo ships one video folder). */
    goto_video(0);
}

/* ========================================================================
 * Poll timer (LVGL) — 3-finger return from album + video-end re-show
 * ======================================================================== */
static void launcher_poll_cb(lv_timer_t* timer)
{
    LV_UNUSED(timer);

    /* 1) Video ended → the full launcher_show(), on this thread (see
     * launcher_show_pending). */
    if (s_video_end_pending)
    {
        s_video_end_pending = false;
        s_video_was_on      = false;
        if (s_mode != MODE_LAUNCHER)
            launcher_show();
        return;
    }
    if (mjpeg_player_is_active())
    {
        s_video_was_on = true;
        return;
    }
    if (s_video_was_on)
    {
        /* Backstop for the falling edge, in case the flag was never posted. */
        s_video_was_on = false;
        if (s_mode != MODE_LAUNCHER)
            launcher_show();
        return;
    }

    /* 2) 3-finger tap in the album returns here.  During playback the player
     * task consumes the same latch, so this only covers the album. */
    if (touch_gt911_get_three_tap())
    {
        if (s_mode != MODE_LAUNCHER)
        {
            launcher_show();
            RTK_LOGI(TAG, "3-finger tap -> launcher\n");
        }
        return;
    }

    /* 3) Keep the video-card subtitle in sync with the SD scan. */
    int n = mjpeg_video_count();
    if (n != s_video_last_count && s_video_sub != NULL)
    {
        s_video_last_count = n;
        char buf[48];
        if (n <= 0)
            snprintf(buf, sizeof(buf), "no videos on SD");
        else if (n == 1)
            snprintf(buf, sizeof(buf), "1 video");
        else
            snprintf(buf, sizeof(buf), "%d videos", n);
        lv_label_set_text(s_video_sub, buf);
    }
}

/* ========================================================================
 * Init
 * ======================================================================== */
void launcher_ui_init(void)
{
    /* Bottom layer: the photo album.  It mounts the SD card, probes the
     * photo source, and decodes the first photo at boot (app_main calls us
     * after lv_init).  The launcher's OPAQUE top layer hides it initially. */
    album_ui_init();

    /* Top layer: full-screen OPAQUE launcher over the album. */
    lv_obj_t* scr = lv_scr_act();

    s_launcher_layer = lv_obj_create(scr);
    lv_obj_remove_style_all(s_launcher_layer);
    lv_obj_set_style_bg_color(s_launcher_layer,
                              lv_color_make(0x08, 0x08, 0x08),
                              0);
    lv_obj_set_style_bg_opa(s_launcher_layer, LV_OPA_COVER, 0);
    lv_obj_set_size(s_launcher_layer, ALBUM_SCREEN_W, ALBUM_SCREEN_H);
    lv_obj_align(s_launcher_layer, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(s_launcher_layer, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t* title = lv_label_create(s_launcher_layer);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_label_set_text(title, "Demo Launcher");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    /* Two cards side by side. */
    const int card_w = 300, card_h = 120, gap = 30;

    lv_obj_t* c_jpg = make_card(s_launcher_layer, &icon_camera, "JPG Album",
                                "photos on SD / flash", card_jpg_cb);
    lv_obj_set_size(c_jpg, card_w, card_h);
    lv_obj_align(c_jpg, LV_ALIGN_CENTER, -(card_w + gap) / 2, 0);

    lv_obj_t* c_video = make_card(s_launcher_layer, &icon_video, "MJPEG Video",
                                  "frame clips on SD", card_mjpeg_cb);
    lv_obj_set_size(c_video, card_w, card_h);
    lv_obj_align(c_video, LV_ALIGN_CENTER, (card_w + gap) / 2, 0);

    /* Grab the video card's subtitle (child 2 — icon, title, subtitle) for
     * the live count label. */
    s_video_sub = lv_obj_get_child(c_video, 2);
    lv_label_set_text(s_video_sub, "scanning...");

    /* Footer hint */
    lv_obj_t* hint = lv_label_create(s_launcher_layer);
    lv_obj_set_style_text_color(hint, lv_color_make(0x66, 0x66, 0x66), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(hint, "3-finger tap: back to launcher");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

    /* Scan the SD for video folders now (album_ui_init above mounted it). */
    int n = mjpeg_scan_videos();
    {
        char buf[48];
        if (n <= 0)
            snprintf(buf, sizeof(buf), "no videos on SD");
        else if (n == 1)
            snprintf(buf, sizeof(buf), "1 video");
        else
            snprintf(buf, sizeof(buf), "%d videos", n);
        lv_label_set_text(s_video_sub, buf);
    }
    s_video_last_count = n;

    /* The player task only flags + covers on exit; this poll does the repaint,
     * fast enough that no queued photo gesture decodes in between. */
    mjpeg_playback_end_cb_set(launcher_show_pending);
    s_poll_timer   = lv_timer_create(launcher_poll_cb, 20, NULL);
    s_video_was_on = false;

    /* The launcher is the boot screen — it must own all touches until the
     * user picks JPG or MJPEG.  Gate the album so its indev events can't
     * react behind the OPAQUE launcher. */
    album_ui_set_active(false);
    launcher_fill_screen(); /* cover the first album photo (decoded at boot) */

    lv_refr_now(NULL); /* paint the launcher as the first visible page */

    RTK_LOGI(TAG, "launcher ready: %d video(s), album below\n", n);
}
