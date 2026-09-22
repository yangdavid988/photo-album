/*
 * brightness_osd.c — shared raw-framebuffer brightness OSD (see .h).
 */

#include "brightness_osd.h"

#include <stdbool.h> /* bool (osd_in_round_rect return) */
#include <stdint.h>
#include <stdio.h>   /* snprintf */

/* ---- Tunables (must match the LVGL album OSD / pc_dashboard) ---- */
#define OSD_PANEL_W   180
#define OSD_PANEL_H    44
#define OSD_PANEL_RAD  22          /* pill: clamped to 44>>1 in LVGL too */
#define OSD_PANEL_ALPHA  102       /* 40 % (soft glass; photo shows through) */
#define OSD_PANEL_BLACK 0x000000u

#define OSD_TEXT_X     10          /* left margin, like the LVGL label x+10 */

#define OSD_BAR_H       6
#define OSD_BAR_RAD     3
#define OSD_BAR_RIGHT  OSD_TEXT_X  /* bar right edge: panel-right minus this */
#define OSD_BAR_LEFT_GAP   10     /* gap between the '%' and the bar's left edge */
#define OSD_BAR_TRACK   0xFF444444u
#define OSD_BAR_FILL    0xFFFFCC33u

/* ---- 5x7 bitmap digits + '%', one byte per row, MSB = leftmost px ---- */
static const uint8_t s_osd_glyphs[11][7] =
{
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},   /* '0' */
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},   /* '1' */
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},   /* '2' */
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},   /* '3' */
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},   /* '4' */
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},   /* '5' */
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},   /* '6' */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},   /* '7' */
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},   /* '8' */
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},   /* '9' */
    {0x0C, 0x12, 0x04, 0x08, 0x14, 0x09, 0x06},   /* '%' */
};

/* Test if (x, y) lies inside the rounded rectangle from (x0,y0)-(x1,y1) with
 * corner radius rad.  Center spans are always in; each corner tests against
 * its quadrant's circle.  This is exact for both the r=22 pill and the small
 * r=3 bars. */
static bool osd_in_round_rect(int x0, int y0, int x1, int y1, int rad,
                              int x, int y)
{
    if (rad > (x1 - x0 + 1) / 2)
        rad = (x1 - x0 + 1) / 2;
    if (rad > (y1 - y0 + 1) / 2)
        rad = (y1 - y0 + 1) / 2;

    if (x < x0 + rad && y < y0 + rad)              /* TL */
    {
        int dx = (x0 + rad - 1) - x, dy = (y0 + rad - 1) - y;
        return (dx * dx + dy * dy) <= rad * rad;
    }
    if (x > x1 - rad && y < y0 + rad)              /* TR */
    {
        int dx = x - (x1 - rad + 1), dy = (y0 + rad - 1) - y;
        return (dx * dx + dy * dy) <= rad * rad;
    }
    if (x < x0 + rad && y > y1 - rad)              /* BL */
    {
        int dx = (x0 + rad - 1) - x, dy = y - (y1 - rad + 1);
        return (dx * dx + dy * dy) <= rad * rad;
    }
    if (x > x1 - rad && y > y1 - rad)              /* BR */
    {
        int dx = x - (x1 - rad + 1), dy = y - (y1 - rad + 1);
        return (dx * dx + dy * dy) <= rad * rad;
    }
    return true;
}

/* Write blended (pb / pa) over one dest ARGB8888 pixel.  Result stays opaque.
 * Uses 8-bit panel black @ OSD_PANEL_ALPHA; full covers pass straight through
 * (an opaque colour with alpha 255 avoids the 2-step blend cost). */
static void osd_put(uint32_t* dest, uint32_t pb, uint32_t pa)
{
    if (pa >= 255u)
    {
        *dest = pb;
        return;
    }

    uint32_t da = 255u - pa;
    uint32_t d0 = *dest;

    uint32_t r = ((((pb >> 16) & 0xFFu) * pa +
                   ((d0 >> 16) & 0xFFu) * da) / 255u) & 0xFFu;
    uint32_t g = ((((pb >> 8) & 0xFFu) * pa +
                   ((d0 >> 8) & 0xFFu) * da) / 255u) & 0xFFu;
    uint32_t b = (((pb & 0xFFu) * pa +
                   (d0 & 0xFFu) * da) / 255u) & 0xFFu;

    *dest = 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* Fill a rounded rectangle: every pixel inside the (clip-tested) rect gets
 * (pb, pa) written over the target, respecting the FB bounds. */
static void osd_fill_round_rect(uint32_t* fb, int fb_w, int fb_h,
                                int x0, int y0, int x1, int y1,
                                int rad, uint32_t pb, uint32_t pa)
{
    for (int y = y0; y <= y1; y++)
    {
        if (y < 0 || y >= fb_h)
            continue;
        uint32_t* row = fb + (uint32_t) y * (uint32_t) fb_w;
        for (int x = x0; x <= x1; x++)
        {
            if (x < 0 || x >= fb_w)
                continue;
            if (osd_in_round_rect(x0, y0, x1, y1, rad, x, y))
                osd_put(&row[x], pb, pa);
        }
    }
}

/* "NN %" in the 5x7 glyphs at 2x scale, opaque white, with a 6-px gap before
 * the '%'.  @return the text width, so the caller can place the bar after it. */
static int osd_draw_text(uint32_t* fb, int fb_w, int fb_h,
                         int x, int y, int value)
{
    char buf[8];
    int  len = snprintf(buf, sizeof(buf), "%d%%", value);
    const uint32_t px = 0xFFFFFFFFu;
    const int start_x = x;

    int  cursor    = x;            /* next glyph's left edge */
    int  last_right = x;           /* exclusive right edge of the last glyph */
    for (int i = 0; i < len; i++)
    {
        char c = buf[i];
        int  gi = (c >= '0' && c <= '9') ? (c - '0') : (c == '%' ? 10 : -1);

        if (gi < 0)
        {
            cursor += 2 * 6; /* treat unknown as a space at 2x pitch */
            continue;
        }

        if (c == '%' && i > 0)
            cursor += 2 * 3; /* gap between the last digit and the '%' */

        for (int row = 0; row < 7; row++)
        {
            uint8_t bits = s_osd_glyphs[gi][row];
            for (int col = 0; col < 5; col++)
            {
                if (!(bits & (0x10u >> col)))
                    continue;
                int gx = cursor + 2 * col;     /* block -> px (6 px wide) */
                int gy = y      + 2 * row;     /* block -> px (14 px tall) */
                int gx1 = gx + 1, gy1 = gy + 1;
                if (gx1 < 0 || gx >= fb_w || gy1 < 0 || gy >= fb_h)
                    continue;
                if (gx >= 0 && gy >= 0)
                    fb[(uint32_t) gy * (uint32_t) fb_w + (uint32_t) gx] = px;
                if (gx1 < fb_w && gy >= 0)
                    fb[(uint32_t) gy * (uint32_t) fb_w + (uint32_t) gx1] = px;
                if (gy1 < fb_h && gx >= 0)
                    fb[(uint32_t) gy1 * (uint32_t) fb_w + (uint32_t) gx] = px;
                if (gx1 < fb_w && gy1 < fb_h)
                    fb[(uint32_t) gy1 * (uint32_t) fb_w + (uint32_t) gx1] = px;
            }
        }
        last_right = cursor + 2 * 5; /* inclusive glyph right col +1 */
        cursor    += 2 * 5 + 2;      /* 2x pitch: 10 px glyph + 2 px spacing */
    }
    return last_right - start_x;
}

/* Bar between the text and the panel's right edge: right end fixed at
 * (panel_right - OSD_BAR_RIGHT), left end OSD_BAR_LEFT_GAP after text_right, so
 * it widens on short text and never overlaps "100 %".  text_right is absolute —
 * a relative width here starts the bar outside the panel.  Track first, then
 * the indicator, so a partial fill keeps the rounded ends. */
static void osd_draw_bar(uint32_t* fb, int fb_w, int fb_h,
                         int text_right, int panel_right, int y, int pct)
{
    const int x0       = text_right + OSD_BAR_LEFT_GAP;
    const int x1       = panel_right - OSD_BAR_RIGHT;
    const int bar_w    = x1 - x0 + 1;
    const int fill_pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    const int fill_w   = (bar_w * fill_pct) / 100;

    if (bar_w <= 0)
        return;
    osd_fill_round_rect(fb, fb_w, fb_h, x0, y, x1,
                        y + OSD_BAR_H - 1, OSD_BAR_RAD, OSD_BAR_TRACK, 255u);
    if (fill_w > 0)
        osd_fill_round_rect(fb, fb_w, fb_h, x0, y, x0 + fill_w - 1,
                            y + OSD_BAR_H - 1, OSD_BAR_RAD, OSD_BAR_FILL, 255u);
}

void brightness_osd_draw(uint32_t fb_addr, int w, int h, int pct)
{
    if (fb_addr == 0u)
        return;

    uint32_t* fb = (uint32_t*) (uintptr_t) fb_addr;

    const int px = (w - OSD_PANEL_W) / 2;
    const int py = h - BRIGHTNESS_OSD_PANEL_GAP_BOTTOM - OSD_PANEL_H;

    /* 40 % black panel over the existing (photo / video) pixels. */
    osd_fill_round_rect(fb, w, h, px, py, px + OSD_PANEL_W - 1,
                        py + OSD_PANEL_H - 1, OSD_PANEL_RAD,
                        OSD_PANEL_BLACK, OSD_PANEL_ALPHA);

    const int text_x = px + OSD_TEXT_X;
    const int text_right = text_x +
                           osd_draw_text(fb, w, h, text_x,
                                         py + (OSD_PANEL_H - 14) / 2, pct);

    osd_draw_bar(fb, w, h, text_right, px + OSD_PANEL_W - 1,
                 py + (OSD_PANEL_H - OSD_BAR_H) / 2, pct);
}
