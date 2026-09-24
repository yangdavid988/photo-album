#include "lcd_drv.h"

/* ========================================================================
 * Screen selection — this demo is T1720A only (GT911 touch + PASSIVE
 * backlight).  DBL070 / ST7262 are not supported.
 * ======================================================================== */
#include "t1720a_cfg.h"
#define SCREEN_CFG (&g_t1720a_cfg)

/* ========================================================================
 * Unified interface implementation
 * ======================================================================== */

/** VBlank flip-done callback: forward-declared for lcd_init registration */
static void flip_done_cb(void* data);

void lcd_init(void)
{
    lcdc_core_init(SCREEN_CFG);
    lcdc_core_register_flip_done(flip_done_cb);
}

void lcd_get_info(int* width, int* height)
{
    lcdc_core_get_info(width, height);
}

void lcd_get_fb_base(uint32_t* base1, uint32_t* base2)
{
    int w, h;
    lcdc_core_get_info(&w, &h);
    uint32_t buf_size = (uint32_t) (w * h * 4);  /* ARGB8888: 4 bytes/pixel */
    uint32_t base     = lcdc_core_get_fb_base(); /* section-allocated PSRAM, not cfg->fb_base */

    if (base1 != NULL)
        *base1 = base;
    if (base2 != NULL)
        *base2 = base + buf_size;
}

const char* lcd_get_driver_name(void)
{
    return SCREEN_CFG->name;
}

/* ========================================================================
 * LVGL integration callbacks
 * ======================================================================== */

/** VBlank flip-done callback: notify LVGL that frame buffer has switched (DMA address updated) */
static void flip_done_cb(void* data)
{
    lv_display_t* disp = (lv_display_t*) data;
    lv_display_flush_ready(disp);
}

void lvgl_disp_flush(lv_display_t* disp, const lv_area_t* area, uint8_t* color_p)
{
    LV_UNUSED(area);   
    lcdc_core_record_flush((uint32_t) color_p, disp);
    lv_display_flush_ready(disp); /* unblock LVGL flushing state */
    lcdc_core_debug_flush_called();
}

uint32_t custom_tick_get(void)
{
    return rtos_time_get_current_system_time_ms();
}
