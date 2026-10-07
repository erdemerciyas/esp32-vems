/*
 * LVGL port + screen manager.
 *
 *   boot splash  ->  [ main dashboard | AFR ]   (swipe left/right or tap to switch,
 *                                                long press on AFR resets min/max)
 *
 * Rendering is tear-free: LVGL draws full frames straight into the hidden one of the two panel
 * frame buffers and flush swaps them at the end of the scanned-out frame.
 * Screen transitions slide / fade two snapshots (the frame on the glass and the target screen
 * rendered once) instead of re-rendering both live screens every frame.
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
#define UI_UPDATE_MS        100
#define DATA_STALE_MS       1500
#define SWITCH_DEBOUNCE_US  (400 * 1000)
#define SLIDE_MS            320
#define FADE_MS             450

enum { PAGE_MAIN, PAGE_AFR, PAGE_COUNT };

typedef enum { TR_LEFT, TR_RIGHT, TR_FADE } transition_t;

static struct {
    lv_disp_draw_buf_t draw_buf;
    lv_disp_drv_t disp_drv;
    lv_indev_drv_t indev_drv;
    lv_obj_t *pages[PAGE_COUNT];
    int page;
    bool in_splash;
    int64_t last_switch;
} ui;

static struct {
    bool active;
    transition_t type;
    lv_obj_t *scr;          // temporary screen holding the two snapshots
    lv_obj_t *img_from;
    lv_obj_t *img_to;
    lv_obj_t *from;
    lv_obj_t *to;
    bool del_from;
} tr;

// ---------------------------------------------------------------------------
// LVGL port
// ---------------------------------------------------------------------------
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    // full_refresh: one call per frame with the whole screen, color_map is a panel frame buffer
    board_lcd_present(color_map);
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
        // frame pacing comes from flush (it blocks until the frame swap), so only yield briefly
        uint32_t wait_ms = lv_timer_handler();
        if (wait_ms < 2) {
            wait_ms = 2;
        } else if (wait_ms > 10) {
            wait_ms = 10;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

// ---------------------------------------------------------------------------
// Snapshot transitions
// ---------------------------------------------------------------------------
#define FRAME_BYTES     (BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(lv_color_t))

static lv_img_dsc_t *snapshot_alloc(void)
{
    lv_img_dsc_t *dsc = lv_mem_alloc(sizeof(lv_img_dsc_t));
    void *data = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (dsc == NULL || data == NULL) {
        lv_mem_free(dsc);
        heap_caps_free(data);
        return NULL;
    }
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
    dsc->header.w = BOARD_LCD_H_RES;
    dsc->header.h = BOARD_LCD_V_RES;
    dsc->data_size = FRAME_BYTES;
    dsc->data = data;
    return dsc;
}

static void snapshot_free(lv_img_dsc_t *dsc)
{
    if (dsc) {
        heap_caps_free((void *)dsc->data);
        lv_mem_free(dsc);
    }
}

/** Copies the frame currently on the glass (the buffer LVGL is not drawing into). */
static lv_img_dsc_t *snapshot_front_buffer(void)
{
    const lv_disp_draw_buf_t *db = &ui.draw_buf;
    const void *front = (db->buf_act == db->buf1) ? db->buf2 : db->buf1;
    lv_img_dsc_t *dsc = snapshot_alloc();
    if (dsc) {
        memcpy((void *)dsc->data, front, FRAME_BYTES);
    }
    return dsc;
}

/** Renders a (not loaded) screen once into an image. */
static lv_img_dsc_t *snapshot_screen(lv_obj_t *scr)
{
    lv_img_dsc_t *dsc = snapshot_alloc();
    if (dsc && lv_snapshot_take_to_buf(scr, LV_IMG_CF_TRUE_COLOR, dsc, (void *)dsc->data, FRAME_BYTES) != LV_RES_OK) {
        snapshot_free(dsc);
        dsc = NULL;
    }
    return dsc;
}

static void snapshot_img_deleted_cb(lv_event_t *e)
{
    snapshot_free(lv_event_get_user_data(e));
}

static lv_obj_t *snapshot_img_create(lv_obj_t *parent, lv_img_dsc_t *dsc)
{
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, dsc);
    lv_obj_add_event_cb(img, snapshot_img_deleted_cb, LV_EVENT_DELETE, dsc);   // frees with the screen
    return img;
}

static void transition_anim_cb(void *var, int32_t v)
{
    switch (tr.type) {
    case TR_LEFT:       // content moves to the left, target comes in from the right
        lv_obj_set_x(tr.img_from, -v);
        lv_obj_set_x(tr.img_to, BOARD_LCD_H_RES - v);
        break;
    case TR_RIGHT:
        lv_obj_set_x(tr.img_from, v);
        lv_obj_set_x(tr.img_to, v - BOARD_LCD_H_RES);
        break;
    case TR_FADE:
        lv_obj_set_style_img_opa(tr.img_to, v, 0);
        break;
    }
}

static void transition_ready_cb(lv_anim_t *a)
{
    if (tr.del_from) {
        lv_obj_del(tr.from);
    }
    // screen loads are applied on the next LVGL tick; auto_del removes the snapshot screen
    // (and frees the snapshots) only once the target is really on the glass
    lv_scr_load_anim(tr.to, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
    tr.active = false;
}

static void transition_start(lv_obj_t *to, transition_t type, uint32_t time_ms, bool del_from)
{
    lv_obj_t *from = lv_scr_act();
    lv_img_dsc_t *snap_from = snapshot_front_buffer();
    lv_img_dsc_t *snap_to = snap_from ? snapshot_screen(to) : NULL;
    if (snap_to == NULL) {
        // out of PSRAM: fall back to LVGL's live screen animation
        snapshot_free(snap_from);
        static const lv_scr_load_anim_t FALLBACK[] = {LV_SCR_LOAD_ANIM_MOVE_LEFT, LV_SCR_LOAD_ANIM_MOVE_RIGHT,
                                                      LV_SCR_LOAD_ANIM_FADE_ON};
        lv_scr_load_anim(to, FALLBACK[type], time_ms, 0, del_from);
        return;
    }

    tr = (typeof(tr)){.active = true, .type = type, .from = from, .to = to, .del_from = del_from};
    tr.scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(tr.scr);    // the snapshots cover every pixel, no background fill
    tr.img_from = snapshot_img_create(tr.scr, snap_from);
    tr.img_to = snapshot_img_create(tr.scr, snap_to);
    transition_anim_cb(NULL, 0);
    lv_scr_load(tr.scr);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, tr.scr);
    lv_anim_set_exec_cb(&a, transition_anim_cb);
    lv_anim_set_values(&a, 0, type == TR_FADE ? LV_OPA_COVER : BOARD_LCD_H_RES);
    lv_anim_set_time(&a, time_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, transition_ready_cb);
    lv_anim_start(&a);
}

// ---------------------------------------------------------------------------
// Screen manager
// ---------------------------------------------------------------------------
static void show_page(int page, transition_t type)
{
    int64_t now = esp_timer_get_time();
    if (tr.active || now - ui.last_switch < SWITCH_DEBOUNCE_US || page == ui.page) {
        return;
    }
    ui.last_switch = now;
    ui.page = page;
    transition_start(ui.pages[page], type, SLIDE_MS, false);
}

static void page_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (ui.in_splash || tr.active) {
        return;
    }
    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
        if (dir == LV_DIR_LEFT) {
            show_page((ui.page + 1) % PAGE_COUNT, TR_LEFT);
        } else if (dir == LV_DIR_RIGHT) {
            show_page((ui.page + PAGE_COUNT - 1) % PAGE_COUNT, TR_RIGHT);
        }
        lv_indev_wait_release(lv_indev_get_act());
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        show_page((ui.page + 1) % PAGE_COUNT, TR_LEFT);
    } else if (code == LV_EVENT_LONG_PRESSED && ui.page == PAGE_AFR) {
        ui_afr_reset_minmax();
        ui.last_switch = esp_timer_get_time();  // don't treat the release as a tap
    }
}

static void ui_update_cb(lv_timer_t *timer)
{
    static vems_data_t d;

    bool have = vems_get_latest(&d);
    if (!have) {
        memset(&d, 0, sizeof(d));
    }
    bool fresh = have && (esp_timer_get_time() - d.timestamp_us) < (int64_t)DATA_STALE_MS * 1000;

    if (ui.in_splash) {
        if (ui_splash_update()) {
            ui.in_splash = false;
            ui.page = PAGE_MAIN;
            ui.last_switch = esp_timer_get_time();
            ui_main_update(&d, fresh);  // the target snapshot shows live values
            transition_start(ui.pages[PAGE_MAIN], TR_FADE, FADE_MS, true);  // frees the splash
        }
        return;
    }

    ui_main_update(&d, fresh);
    ui_afr_update(&d, fresh);
}

// ---------------------------------------------------------------------------
esp_err_t dash_ui_start(esp_lcd_panel_handle_t panel)
{
    lv_init();

    // draw straight into the panel frame buffers; fb0 is on the glass after init, so start with fb1
    void *fb_front = board_lcd_frame_buffer(0);
    void *fb_back = board_lcd_frame_buffer(1);
    ESP_RETURN_ON_FALSE(fb_front && fb_back, ESP_ERR_INVALID_STATE, TAG, "frame buffers");
    lv_disp_draw_buf_init(&ui.draw_buf, fb_back, fb_front, BOARD_LCD_H_RES * BOARD_LCD_V_RES);

    lv_disp_drv_init(&ui.disp_drv);
    ui.disp_drv.hor_res = BOARD_LCD_H_RES;
    ui.disp_drv.ver_res = BOARD_LCD_V_RES;
    ui.disp_drv.flush_cb = flush_cb;
    ui.disp_drv.draw_buf = &ui.draw_buf;
    ui.disp_drv.full_refresh = 1;
    ui.disp_drv.user_data = panel;
    lv_disp_t *disp = lv_disp_drv_register(&ui.disp_drv);
    lv_disp_set_bg_opa(disp, LV_OPA_TRANSP);    // every screen paints its own background

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
