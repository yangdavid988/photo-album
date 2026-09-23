/*
 * mjpeg_picker_ui.c — MJPEG video folder picker (multi-video selection)
 *
 * OPAQUE full-screen layer over the launcher, listing one card per video
 * folder in the MJPEG/ directory.  Uses the SAME recipe as the launcher:
 * a solid-fill LVGL layer + forcing an immediate lv_refr_now.
 *
 * The picker is a pure view: it holds no scan state, owns no route.  It
 * builds cards from the videos the launcher found, calls back into the
 * launcher on tap/back, and lets the launcher hide it before playback.
 */
#include "mjpeg_picker_ui.h"

#include "config/album_config.h" /* ALBUM_SCREEN_W/H */
#include "core/mjpeg_player.h"   /* mjpeg_video_at / sd_video_t */
#include "ui/album_ui.h"         /* album_ui_set_active for input gating */
#include "hal/lcd/lcd_drv.h"     /* lcd_get_fb_base — FB cover */
#include "hal/lcd/lcdc_core.h"   /* lcdc_core_get_info */

#include <stdio.h>  /* snprintf */
#include <string.h>

#include "ameba_soc.h"
#include "os_wrapper.h"
#include "log.h"

#ifndef TAG
#define TAG "MPICKER"
#endif

/* ---- Layer state ---- */
static lv_obj_t* s_picker_layer = NULL;
static int       s_count        = 0;
static int       s_page         = 0; /* current 0-based page (PICKER_PER_PAGE) */
static void (*s_on_pick)(int index, void* udata) = NULL;
static void (*s_on_back)(void* udata)            = NULL;
static void*     s_udata         = NULL;

/* ---- Card geometry (fits the 800-wide screen, 4 per row) ---- */
#define PICKER_CARD_W 150
#define PICKER_CARD_H 96
#define PICKER_GAP_X  24

/* ---- Pagination: 4 columns x 2 rows = 8 cards per page (MJPEG_MAX_VIDEOS
 * 24 → 3 pages max, so plain ‹ › pagers suffice, no scrolling widget). ---- */
#define PICKER_PER_PAGE 8
#define PICKER_ROWS     2

/* ---- FB cover --- mirror launcher_fill_screen: fill both LCDC FBs with the
 * layer background so stale launcher/photo pixels can't bleed through the
 * gaps between cards (LVGL only repaints regions it knows about). */
static void picker_fill_fb(uint32_t base, int w, int h, uint32_t pixel)
{
    uint32_t* p = (uint32_t*) base;
    uint32_t  n = (uint32_t) (w * h);
    for (uint32_t i = 0; i < n; i++)
        p[i] = pixel;
    DCache_Clean(base, (uint32_t) (w * h * 4));
}

static void picker_cover_fb(void)
{
    uint32_t b1 = 0, b2 = 0;
    int      w, h;
    lcdc_core_get_info(&w, &h);
    lcd_get_fb_base(&b1, &b2);
    if (b1 != 0)
        picker_fill_fb(b1, w, h, 0xFF080808u);
    if (b2 != 0)
        picker_fill_fb(b2, w, h, 0xFF080808u);
}

/* ---- One card ---- */
static lv_obj_t* picker_make_card(int index)
{
    const sd_video_t* v = mjpeg_video_at(index);
    if (v == NULL)
        return NULL;

    lv_obj_t* card = lv_obj_create(s_picker_layer);
    lv_obj_remove_style_all(card);
    lv_obj_set_style_bg_color(card, lv_color_make(0x14, 0x14, 0x14), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_make(0x33, 0x33, 0x33), 0);
    lv_obj_set_size(card, PICKER_CARD_W, PICKER_CARD_H);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Title = folder base name, clipped (names can be long LFN). */
    lv_obj_t* t = lv_label_create(card);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(t, PICKER_CARD_W - 20);
    lv_label_set_text(t, v->name);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 14);

    /* Subtitle = frame count. */
    char buf[40];
    snprintf(buf, sizeof(buf), "%d frames", v->frame_count);
    lv_obj_t* s = lv_label_create(card);
    lv_obj_set_style_text_color(s, lv_color_make(0x99, 0x99, 0x99), 0);
    lv_obj_set_style_text_font(s, &lv_font_montserrat_14, 0);
    lv_label_set_text(s, buf);
    lv_obj_align(s, LV_ALIGN_BOTTOM_MID, 0, -8);

    /* The tap handler + per-card index are attached by the build loop so the
     * index can ride as the event's user_data. */
    return card;
}

static void on_pick_cb(lv_event_t* e)
{
    int idx = (int) (intptr_t) lv_event_get_user_data(e);
    if (s_on_pick != NULL)
        s_on_pick(idx, s_udata);
}

static void on_back_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    if (s_on_back != NULL)
        s_on_back(s_udata);
}

/* ---- Pagination ---- */

/* Pagers + page indicator, recreated on each page flip. */
static lv_obj_t* s_page_prev = NULL;
static lv_obj_t* s_page_next = NULL;
static lv_obj_t* s_page_lbl  = NULL;

static void picker_page_cb(lv_event_t* e); /* defined below picker_rebuild */

/* Rebuild the ENTIRE layer content for the current page.  Called from show()
 * and on every page flip; children are rebuilt from scratch so no stale ones
 * survive a flip. */
static void picker_rebuild(void)
{
    if (s_picker_layer == NULL)
        return;

    lv_obj_clean(s_picker_layer);
    s_page_prev = NULL;
    s_page_next = NULL;
    s_page_lbl  = NULL;

    /* Title */
    lv_obj_t* title = lv_label_create(s_picker_layer);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_label_set_text(title, "Select MJPEG Video");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    /* Back arrow (top-left): a clickable container carrying a "<" label —
     * an lv_obj is clickable by default, a bare label is not. */
    lv_obj_t* back = lv_obj_create(s_picker_layer);
    lv_obj_remove_style_all(back);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0); /* just the hit area */
    lv_obj_set_size(back, 64, 40);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_add_event_cb(back, on_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* back_lbl = lv_label_create(back);
    lv_obj_set_style_text_color(back_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(back_lbl, "<");
    lv_obj_center(back_lbl);

    /* Cards on the current page. */
    int start    = s_page * PICKER_PER_PAGE;
    int end      = start + PICKER_PER_PAGE;
    if (end > s_count)
        end = s_count;
    int page_cnt = (end > start) ? (end - start) : 0;
    int pages    = (s_count + PICKER_PER_PAGE - 1) / PICKER_PER_PAGE;

    /* Card grid: fixed PICKER_ROWS x PICKER_COLS layout, centered. */
    const int row_gap = 24;
    int total_h = PICKER_ROWS * PICKER_CARD_H + (PICKER_ROWS - 1) * row_gap;
    int y0      = (ALBUM_SCREEN_H - total_h) / 2; /* centered; pager below */

    for (int i = start; i < end; i++)
    {
        int gi = i - start;
        int row = gi / MJPEG_PICKER_COLS;
        int col = gi % MJPEG_PICKER_COLS;
        lv_obj_t* card = picker_make_card(i);
        if (card == NULL)
            continue;
        lv_obj_add_event_cb(card, on_pick_cb, LV_EVENT_CLICKED, (void*) (intptr_t) i);
        int nin_row = (row + 1) * MJPEG_PICKER_COLS <= page_cnt
                          ? MJPEG_PICKER_COLS
                          : page_cnt - row * MJPEG_PICKER_COLS;
        int row_w = nin_row * PICKER_CARD_W + (nin_row - 1) * PICKER_GAP_X;
        int x     = (ALBUM_SCREEN_W - row_w) / 2 + col * (PICKER_CARD_W + PICKER_GAP_X);
        int y     = y0 + row * (PICKER_CARD_H + row_gap);
        lv_obj_set_pos(card, x, y);
    }

    /* Page indicator + ‹ › pagers, only when there is more than one page.
     * The pagers sit in full-height hit areas at the screen edges so they
     * never overlap the centered card grid. */
    if (pages > 1)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d/%d", s_page + 1, pages);
        s_page_lbl = lv_label_create(s_picker_layer);
        lv_obj_set_style_text_color(s_page_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(s_page_lbl, &lv_font_montserrat_14, 0);
        lv_label_set_text(s_page_lbl, buf);
        lv_obj_align(s_page_lbl, LV_ALIGN_BOTTOM_MID, 0, -20);
    }

    if (s_page > 0)
    {
        s_page_prev = lv_obj_create(s_picker_layer);
        lv_obj_remove_style_all(s_page_prev);
        lv_obj_set_style_bg_opa(s_page_prev, LV_OPA_TRANSP, 0);
        lv_obj_set_size(s_page_prev, 64, 96);
        lv_obj_align(s_page_prev, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t* l = lv_label_create(s_page_prev);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        lv_label_set_text(l, "<");
        lv_obj_center(l);
        lv_obj_add_event_cb(s_page_prev, picker_page_cb, LV_EVENT_CLICKED,
                            (void*) (intptr_t) -1);
    }
    if ((s_page + 1) < pages)
    {
        s_page_next = lv_obj_create(s_picker_layer);
        lv_obj_remove_style_all(s_page_next);
        lv_obj_set_style_bg_opa(s_page_next, LV_OPA_TRANSP, 0);
        lv_obj_set_size(s_page_next, 64, 96);
        lv_obj_align(s_page_next, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_t* r = lv_label_create(s_page_next);
        lv_obj_set_style_text_color(r, lv_color_white(), 0);
        lv_obj_set_style_text_font(r, &lv_font_montserrat_24, 0);
        lv_label_set_text(r, ">");
        lv_obj_center(r);
        lv_obj_add_event_cb(s_page_next, picker_page_cb, LV_EVENT_CLICKED,
                            (void*) (intptr_t) 1);
    }
}

/* Page-flip: adjust s_page by delta and rebuild.  picker_rebuild cleans the
 * whole layer first, so flips never stack children. */
static void picker_page_cb(lv_event_t* e)
{
    int delta = (int) (intptr_t) lv_event_get_user_data(e);
    int pages = (s_count + PICKER_PER_PAGE - 1) / PICKER_PER_PAGE;
    s_page += delta;
    if (s_page < 0)
        s_page = 0;
    if (s_page >= pages)
        s_page = (pages > 0) ? pages - 1 : 0;

    picker_rebuild();
    lv_obj_invalidate(s_picker_layer);
    lv_refr_now(NULL);
}

/* ========================================================================
 * Public
 * ======================================================================== */
void mjpeg_picker_show(int count,
                       void (*on_pick)(int index, void* udata),
                       void (*on_back)(void* udata),
                       void* udata)
{
    if (s_picker_layer != NULL)
        mjpeg_picker_hide(); /* re-open: drop any old layer */

    s_on_pick = on_pick;
    s_on_back = on_back;
    s_udata   = udata;
    s_count   = count;
    if (s_count > MJPEG_MAX_VIDEOS)
        s_count = MJPEG_MAX_VIDEOS;
    s_page    = 0; /* always start on page 1 of a fresh picker */

    lv_obj_t* scr = lv_scr_act();
    s_picker_layer = lv_obj_create(scr);
    lv_obj_remove_style_all(s_picker_layer);
    lv_obj_set_style_bg_color(s_picker_layer, lv_color_make(0x08, 0x08, 0x08), 0);
    lv_obj_set_style_bg_opa(s_picker_layer, LV_OPA_COVER, 0);
    lv_obj_set_size(s_picker_layer, ALBUM_SCREEN_W, ALBUM_SCREEN_H);
    lv_obj_align(s_picker_layer, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(s_picker_layer, LV_OBJ_FLAG_SCROLLABLE);

    picker_rebuild();

    /* Gate the album layer so its indev events can't react behind this OPAQUE
     * layer (same as the launcher does). */
    album_ui_set_active(false);

    /* Cover the FBs, then repaint: shows the picker over the launcher. */
    picker_cover_fb();
    lv_obj_invalidate(s_picker_layer);
    lv_refr_now(NULL);
}

void mjpeg_picker_hide(void)
{
    if (s_picker_layer != NULL)
    {
        lv_obj_clean(s_picker_layer); /* removes children + callbacks */
        lv_obj_del(s_picker_layer);
        s_picker_layer = NULL;
    }
    album_ui_set_active(false); /* never leave the album live under a hidden picker */
    s_on_pick   = NULL;
    s_on_back   = NULL;
    s_count     = 0;
    s_page      = 0;
    s_page_prev = NULL;
    s_page_next = NULL;
    s_page_lbl  = NULL;
}
