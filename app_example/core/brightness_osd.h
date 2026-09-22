/*
 * brightness_osd.h — brightness OSD drawn straight into an LCDC framebuffer,
 * shared by the album and the MJPEG player.
 *
 * An LVGL object cannot be used: while the player owns the FBs LVGL is stopped,
 * and in the album the photo is PP-written into the FBs, so a partially
 * transparent LVGL widget would blend against the black screen background
 * instead of the image.  A 180x44 pill, 40 %-black over the existing pixels,
 * "NN %" text and a bar that fills the gap between the text and the panel edge.
 */

#ifndef BRIGHTNESS_OSD_H
#define BRIGHTNESS_OSD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Draw the OSD once into an ARGB8888 framebuffer of w x h px (pct clamped to
 * 0..100).  The panel blends over what is already there, so drawing it twice
 * would darken twice — call on a freshly decoded or freshly presented frame. */
void brightness_osd_draw(uint32_t fb_addr, int w, int h, int pct);

/* Panel rect, also used to erase it (the album repaints these rows with the
 * photo when the OSD is dismissed). */
#define BRIGHTNESS_OSD_PANEL_W  180
#define BRIGHTNESS_OSD_PANEL_H  44
#define BRIGHTNESS_OSD_PANEL_GAP_BOTTOM 12

/* Panel top-left corner for a w x h screen. */
static inline int brightness_osd_x(int w)
{
    return (w - BRIGHTNESS_OSD_PANEL_W) / 2;
}

static inline int brightness_osd_y(int h)
{
    return h - BRIGHTNESS_OSD_PANEL_H - BRIGHTNESS_OSD_PANEL_GAP_BOTTOM;
}

#ifdef __cplusplus
}
#endif

#endif /* BRIGHTNESS_OSD_H */
