/*
 * app_main.c — photo album demo entry (T1720A 800x480, RTL8721F / AmebaGreen2)
 *
 * Skeleton copied from pc_dashboard_demo/main/app_main.c, stripped to the
 * photo-album path: LCD + LVGL + GT911 touch + album UI.  No WiFi/MQTT/USB.
 */

#include "config/album_config.h"
#include "ui/launcher_ui.h"
#include "core/mjpeg_player.h"
#include "hal/lcd/lcd_drv.h"
#include "hal/lcd/lcdc_core.h"
#ifdef CONFIG_SCREEN_T1720A
#include "hal/touch/touch_gt911.h"
#endif
#include "ameba_pmu.h"
#include "FreeRTOS.h"
#include "task.h" /* taskYIELD */

/* PMU device ID for LCDC — hold wakelock during LVGL operation to prevent
 * tickless idle from gating LCDC clock (LCDC is SOC domain, cannot wake CPU). */
#define PMU_LCDC_DEVICE PMU_OS

#ifndef TAG
#define TAG "ALBUM_MAIN"
#endif

/* LVGL display buffers (dual-buffer via lcd_get_fb_base) */
static u8* lv_disp_buf1 = NULL;
static u8* lv_disp_buf2 = NULL;

/* Screen dimensions */
uint16_t lcd_w = ALBUM_SCREEN_W;
uint16_t lcd_h = ALBUM_SCREEN_H;

/* ========================================================================
 * LVGL main thread (initialization + render loop)
 * ======================================================================== */
static void lvgl_main_thread(void* parameters)
{
    (void) parameters;

    RTK_LOGS(TAG, RTK_LOG_INFO, "\r\n=== Photo Album Demo ===\r\n");

    /* LCD initialization */
    lcd_init();

    /* Hold PMU wakelock for entire LVGL runtime — prevents tickless idle from
     * entering sleep modes that gate LCDC clock. */
    pmu_acquire_wakelock(PMU_LCDC_DEVICE);

    /* Get framebuffer base addresses (PSRAM, dual-buffer) */
    {
        uint32_t fb1, fb2;
        lcd_get_fb_base(&fb1, &fb2);
        lv_disp_buf1 = (u8*) fb1;
        lv_disp_buf2 = (u8*) fb2;
        RTK_LOGI(TAG, "FB base1=0x%08lX base2=0x%08lX driver=%s\n",
                 (unsigned long) fb1, (unsigned long) fb2, lcd_get_driver_name());
    }

    /* LVGL initialization (must precede any lv_* API calls) */
    lv_init();
    lv_tick_set_cb(custom_tick_get);

    lv_display_t* display = lv_display_create(lcd_w, lcd_h);
    lv_display_set_flush_cb(display, lvgl_disp_flush);
    lv_display_set_buffers(display,
                           lv_disp_buf1,
                           lv_disp_buf2, /* Dual-buffer with VBlank page flip */
                           lcd_w * lcd_h * 4,
                           LV_DISPLAY_RENDER_MODE_DIRECT);

    /* Touch initialization (T1720A GT911) — after display create */
#ifdef CONFIG_SCREEN_T1720A
    touch_gt911_init();
#endif

    /* Build the launcher UI (album + video router below it) */
    launcher_ui_init();

    RTK_LOGI(TAG, "Launcher UI ready, starting main loop...\n");

    /* LVGL main loop */
    while (1)
    {
        /* MJPEG playback owns the LCDC FBs + DMA: the player task decodes into
         * the non-scanned FB and flips at the FRD boundary.  Standing the LVGL
         * thread down here keeps lv_timer_handler() (and its flush_cb /
         * record/commit flip path) from fighting the player for the FBs. */
        if (mjpeg_player_is_active())
        {
            rtos_time_delay_ms(50);
            continue;
        }

        /* Frame gate: wait for previous frame's pending flip to be consumed
         * by LINE ISR before starting a new LVGL frame (see dashboard note). */
        while (lcdc_core_is_flip_pending())
        {
            taskYIELD();
        }

        uint32_t time_till_next = lv_timer_handler();
        lcdc_core_flush_commit();
        if (time_till_next == LV_NO_TIMER_READY)
            time_till_next = LV_DEF_REFR_PERIOD;
        rtos_time_delay_ms(time_till_next);
    }

    rtos_task_delete(NULL);
}

/* ========================================================================
 * Application entry point
 * ======================================================================== */
void app_example(void)
{
    RTK_LOGI(TAG, "Photo Album demo started!\r\n");

    if (rtos_task_create(NULL,
                         "lvgl_thread",
                         (rtos_task_t) lvgl_main_thread,
                         NULL,
                         TASK_STACK_LVGL,
                         TASK_PRIO_LVGL) != RTK_SUCCESS)
    {
        RTK_LOGE(TAG, "Create LVGL thread failed!\r\n");
        return;
    }

    RTK_LOGI(TAG, "All tasks created.\r\n");
}
