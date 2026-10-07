/*
 * LVGL port + screen manager.
 *
 *   boot splash  ->  [ main dashboard | AFR ]   (swipe left/right or tap to switch,
 *                                                long press on AFR resets min/max)
 *
 * All LVGL calls run in the LVGL task; data is pulled from vems_get_latest() by an lv_timer.
 */
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"
#include "board_lcd.h"
#include "ui_priv.h"
#include "dash_ui.h"

static const char *TAG = "ui";

#define LVGL_TICK_MS        2
#define DRAW_BUF_LINES      48
#define UI_UPDATE_MS        100
#define DATA_STALE_MS       1500
#define SWITCH_DEBOUNCE_US  (400 * 1000)

enum { PAGE_MAIN, PAGE_AFR, PAGE_COUNT };

static struct {
    lv_disp_draw_buf_t draw_buf;
    lv_disp_drv_t disp_drv;
    lv_indev_drv_t indev_drv;
    lv_obj_t *pages[PAGE_COUNT];
    int page;
    bool in_splash;
    int64_t last_switch;
} ui;

// ---------------------------------------------------------------------------
// LVGL port
// ---------------------------------------------------------------------------
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
    lv_disp_flush_ready(drv);
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    uint16_t x, y;
    if (board_touch_read(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

static void tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_MS);
}

static void lvgl_task(void *arg)
{
    while (1) {
        uint32_t wait_ms = lv_timer_handler();
        if (wait_ms < 5) {
            wait_ms = 5;
        } else if (wait_ms > 30) {
            wait_ms = 30;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

// ---------------------------------------------------------------------------
// Screen manager
// ---------------------------------------------------------------------------
static void show_page(int page, lv_scr_load_anim_t anim)
{
    int64_t now = esp_timer_get_time();
    if (now - ui.last_switch < SWITCH_DEBOUNCE_US || page == ui.page) {
        return;
    }
    ui.last_switch = now;
    ui.page = page;
    lv_scr_load_anim(ui.pages[page], anim, 250, 0, false);
}

static void page_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (ui.in_splash) {
        return;
    }
    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
        if (dir == LV_DIR_LEFT) {
            show_page((ui.page + 1) % PAGE_COUNT, LV_SCR_LOAD_ANIM_MOVE_LEFT);
        } else if (dir == LV_DIR_RIGHT) {
            show_page((ui.page + PAGE_COUNT - 1) % PAGE_COUNT, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
        }
        lv_indev_wait_release(lv_indev_get_act());
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        show_page((ui.page + 1) % PAGE_COUNT, LV_SCR_LOAD_ANIM_MOVE_LEFT);
    } else if (code == LV_EVENT_LONG_PRESSED && ui.page == PAGE_AFR) {
        ui_afr_reset_minmax();
        ui.last_switch = esp_timer_get_time();  // don't treat the release as a tap
    }
}

static void ui_update_cb(lv_timer_t *timer)
{
    static vems_data_t d;

    if (ui.in_splash) {
        if (ui_splash_update()) {
            ui.in_splash = false;
            ui.page = PAGE_MAIN;
            ui.last_switch = esp_timer_get_time();
            lv_scr_load_anim(ui.pages[PAGE_MAIN], LV_SCR_LOAD_ANIM_FADE_ON, 600, 0, true);  // frees the splash
        }
        return;
    }

    bool have = vems_get_latest(&d);
    if (!have) {
        memset(&d, 0, sizeof(d));
    }
    bool fresh = have && (esp_timer_get_time() - d.timestamp_us) < (int64_t)DATA_STALE_MS * 1000;

    ui_main_update(&d, fresh);
    ui_afr_update(&d, fresh);
}

// ---------------------------------------------------------------------------
esp_err_t dash_ui_start(esp_lcd_panel_handle_t panel)
{
    lv_init();

    size_t buf_px = BOARD_LCD_H_RES * DRAW_BUF_LINES;
    lv_color_t *buf1 = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    lv_color_t *buf2 = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG, "draw buffers");
    lv_disp_draw_buf_init(&ui.draw_buf, buf1, buf2, buf_px);

    lv_disp_drv_init(&ui.disp_drv);
    ui.disp_drv.hor_res = BOARD_LCD_H_RES;
    ui.disp_drv.ver_res = BOARD_LCD_V_RES;
    ui.disp_drv.flush_cb = flush_cb;
    ui.disp_drv.draw_buf = &ui.draw_buf;
    ui.disp_drv.user_data = panel;
    lv_disp_drv_register(&ui.disp_drv);

    lv_indev_drv_init(&ui.indev_drv);
    ui.indev_drv.type = LV_INDEV_TYPE_POINTER;
    ui.indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&ui.indev_drv);

    const esp_timer_create_args_t tick_args = {.callback = tick_cb, .name = "lv_tick"};
    esp_timer_handle_t tick;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick), TAG, "tick timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick, LVGL_TICK_MS * 1000), TAG, "tick start");

    // pages are built up-front so switching is instant
    ui.pages[PAGE_MAIN] = ui_main_create(PAGE_MAIN, PAGE_COUNT);
    ui.pages[PAGE_AFR] = ui_afr_create(PAGE_AFR, PAGE_COUNT);
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_add_event_cb(ui.pages[i], page_event_cb, LV_EVENT_GESTURE, NULL);
        lv_obj_add_event_cb(ui.pages[i], page_event_cb, LV_EVENT_SHORT_CLICKED, NULL);
        lv_obj_add_event_cb(ui.pages[i], page_event_cb, LV_EVENT_LONG_PRESSED, NULL);
    }

    ui.in_splash = true;
    lv_scr_load(ui_splash_create());
    lv_timer_create(ui_update_cb, UI_UPDATE_MS, NULL);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, NULL, 1);
    return ESP_OK;
}
