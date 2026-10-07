/*
 * Round 480x480 dashboard for VEMS live data (LVGL 8.2).
 *
 *   outer ring : RPM arc (0..CONFIG_VEMS_UI_RPM_MAX) with redline zone
 *   center     : link status, big RPM, 2x3 tiles (MAP, LAMBDA, TPS / CLT, IAT, BATT)
 *   bottom     : ignition advance, pulse width, VE, engine status flags, frame rate
 *
 * All LVGL calls run in the LVGL task; data is pulled from vems_get_latest() by an lv_timer.
 */
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"
#include "lvgl.h"
#include "board_lcd.h"
#include "vems_proto.h"
#include "dash_ui.h"

static const char *TAG = "ui";

#define LVGL_TICK_MS        2
#define DRAW_BUF_LINES      48
#define UI_UPDATE_MS        100
#define DATA_STALE_MS       1500

#define RPM_MAX             CONFIG_VEMS_UI_RPM_MAX
#define RPM_SHIFT           CONFIG_VEMS_UI_RPM_SHIFT
#define RPM_WARN            (RPM_SHIFT - 1000)

// palette
#define C_BG        lv_color_hex(0x000000)
#define C_TRACK     lv_color_hex(0x1A1D23)
#define C_TILE      lv_color_hex(0x10131A)
#define C_TILE_BRD  lv_color_hex(0x252A35)
#define C_TEXT      lv_color_hex(0xF2F4F8)
#define C_MUTED     lv_color_hex(0x7D8696)
#define C_TICK      lv_color_hex(0x5A6272)
#define C_ACCENT    lv_color_hex(0x00C8FF)
#define C_OK        lv_color_hex(0x2ED47A)
#define C_WARN      lv_color_hex(0xFFB020)
#define C_ALERT     lv_color_hex(0xFF3B30)

typedef struct {
    lv_obj_t *box;
    lv_obj_t *value;
    char last[16];
} tile_t;

enum { T_MAP, T_LAMBDA, T_TPS, T_CLT, T_IAT, T_BATT, T_COUNT };

static struct {
    lv_disp_draw_buf_t draw_buf;
    lv_disp_drv_t disp_drv;
    lv_obj_t *meter;
    lv_meter_indicator_t *rpm_arc;
    lv_color_t rpm_color;
    lv_obj_t *status;
    lv_obj_t *rpm;
    lv_obj_t *info;
    lv_obj_t *flags;
    lv_obj_t *rate;
    tile_t tiles[T_COUNT];
    char rpm_last[8];
    uint32_t last_seq;
    int64_t rate_t0;
    uint32_t rate_seq0;
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
// Widgets
// ---------------------------------------------------------------------------
static void meter_label_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->type == LV_METER_DRAW_PART_TICK && dsc->text != NULL) {
        // scale is in 100 rpm units, label in 1000 rpm
        lv_snprintf(dsc->text, 16, "%d", (int)(dsc->value / 10));
    }
}

static void create_meter(lv_obj_t *scr)
{
    lv_obj_t *m = lv_meter_create(scr);
    ui.meter = m;
    lv_obj_set_size(m, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_center(m);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 26, 0);                 // ticks start inside the RPM ring
    lv_obj_set_style_text_color(m, C_MUTED, 0);
    lv_obj_set_style_text_font(m, &lv_font_montserrat_16, 0);
    lv_obj_remove_style(m, NULL, LV_PART_INDICATOR);    // no needle hub
    lv_obj_clear_flag(m, LV_OBJ_FLAG_CLICKABLE);

    const int max = RPM_MAX / 100;
    lv_meter_scale_t *s = lv_meter_add_scale(m);
    lv_meter_set_scale_range(m, s, 0, max, 270, 135);
    lv_meter_set_scale_ticks(m, s, max / 2 + 1, 2, 8, C_TICK);
    lv_meter_set_scale_major_ticks(m, s, 5, 3, 16, lv_color_hex(0xC9CFDA), 12);

    // redline: tick color + thin inner arc
    lv_meter_indicator_t *red_ticks = lv_meter_add_scale_lines(m, s, C_ALERT, C_ALERT, false, 0);
    lv_meter_set_indicator_start_value(m, red_ticks, RPM_SHIFT / 100);
    lv_meter_set_indicator_end_value(m, red_ticks, max);
    lv_meter_indicator_t *red_arc = lv_meter_add_arc(m, s, 3, C_ALERT, 0);
    lv_meter_set_indicator_start_value(m, red_arc, RPM_SHIFT / 100);
    lv_meter_set_indicator_end_value(m, red_arc, max);

    // RPM ring: dark track + live arc on the outer edge
    lv_meter_indicator_t *track = lv_meter_add_arc(m, s, 16, C_TRACK, 24);
    lv_meter_set_indicator_start_value(m, track, 0);
    lv_meter_set_indicator_end_value(m, track, max);
    ui.rpm_color = C_ACCENT;
    ui.rpm_arc = lv_meter_add_arc(m, s, 16, ui.rpm_color, 24);
    lv_meter_set_indicator_start_value(m, ui.rpm_arc, 0);
    lv_meter_set_indicator_end_value(m, ui.rpm_arc, 0);

    lv_obj_add_event_cb(m, meter_label_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, txt);
    return l;
}

static void create_tile(tile_t *t, lv_obj_t *scr, int x, int y, const char *caption, const char *unit)
{
    t->box = lv_obj_create(scr);
    lv_obj_set_size(t->box, 104, 58);
    lv_obj_align(t->box, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(t->box, C_TILE, 0);
    lv_obj_set_style_bg_opa(t->box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(t->box, C_TILE_BRD, 0);
    lv_obj_set_style_border_width(t->box, 1, 0);
    lv_obj_set_style_radius(t->box, 12, 0);
    lv_obj_set_style_pad_all(t->box, 0, 0);
    lv_obj_clear_flag(t->box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    char cap[24];
    snprintf(cap, sizeof(cap), "%s %s", caption, unit);
    lv_obj_t *c = make_label(t->box, &lv_font_montserrat_12, C_MUTED, cap);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 6);

    t->value = make_label(t->box, &lv_font_montserrat_24, C_TEXT, "--");
    lv_obj_align(t->value, LV_ALIGN_BOTTOM_MID, 0, -6);
    t->last[0] = '\0';
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    create_meter(scr);

    ui.status = make_label(scr, &lv_font_montserrat_14, C_WARN, LV_SYMBOL_USB "  VEMS BEKLENIYOR");
    lv_obj_align(ui.status, LV_ALIGN_TOP_MID, 0, 88);

    ui.rpm = make_label(scr, &lv_font_montserrat_48, C_TEXT, "----");
    lv_obj_align(ui.rpm, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_t *rpm_cap = make_label(scr, &lv_font_montserrat_14, C_MUTED, "RPM");
    lv_obj_align(rpm_cap, LV_ALIGN_TOP_MID, 0, 176);

    // 2x3 tiles, x = 76 / 188 / 300, rows at y = 206 / 272
    static const int xs[3] = {76, 188, 300};
    create_tile(&ui.tiles[T_MAP],    scr, xs[0], 206, "MAP", "kPa");
    create_tile(&ui.tiles[T_LAMBDA], scr, xs[1], 206, "LAMBDA", "");
    create_tile(&ui.tiles[T_TPS],    scr, xs[2], 206, "TPS", "%");
    create_tile(&ui.tiles[T_CLT],    scr, xs[0], 272, "CLT", "C");
    create_tile(&ui.tiles[T_IAT],    scr, xs[1], 272, "IAT", "C");
    create_tile(&ui.tiles[T_BATT],   scr, xs[2], 272, "AKU", "V");

    ui.info = make_label(scr, &lv_font_montserrat_16, C_TEXT, "ADV --   PW --   VE --");
    lv_obj_align(ui.info, LV_ALIGN_TOP_MID, 0, 342);

    ui.flags = make_label(scr, &lv_font_montserrat_14, C_MUTED, "");
    lv_label_set_recolor(ui.flags, true);
    lv_obj_align(ui.flags, LV_ALIGN_TOP_MID, 0, 380);

    ui.rate = make_label(scr, &lv_font_montserrat_12, C_TICK, "");
    lv_obj_align(ui.rate, LV_ALIGN_TOP_MID, 0, 412);
}

// ---------------------------------------------------------------------------
// Updating
// ---------------------------------------------------------------------------
static void tile_set(tile_t *t, const char *txt, lv_color_t color)
{
    if (strcmp(t->last, txt) != 0) {
        strlcpy(t->last, txt, sizeof(t->last));
        lv_label_set_text(t->value, txt);
    }
    lv_obj_set_style_text_color(t->value, color, 0);
}

static void tiles_invalid(void)
{
    for (int i = 0; i < T_COUNT; i++) {
        tile_set(&ui.tiles[i], "--", C_MUTED);
    }
}

static void set_rpm_arc(uint16_t rpm)
{
    lv_color_t col = rpm >= RPM_SHIFT ? C_ALERT : (rpm >= RPM_WARN ? C_WARN : C_ACCENT);
    if (col.full != ui.rpm_color.full) {
        ui.rpm_color = col;
        ui.rpm_arc->type_data.arc.color = col;
        lv_obj_invalidate(ui.meter);
    }
    int v = rpm / 100;
    if (v > RPM_MAX / 100) {
        v = RPM_MAX / 100;
    }
    lv_meter_set_indicator_end_value(ui.meter, ui.rpm_arc, v);
}

static void ui_update_cb(lv_timer_t *timer)
{
    static vems_data_t d;
    char buf[128];
    bool have = vems_get_latest(&d);
    int64_t now = esp_timer_get_time();
    bool fresh = have && (now - d.timestamp_us) < (int64_t)DATA_STALE_MS * 1000;

    if (!fresh) {
        lv_label_set_text(ui.status, have ? LV_SYMBOL_WARNING "  VERI YOK" : LV_SYMBOL_USB "  VEMS BEKLENIYOR");
        lv_obj_set_style_text_color(ui.status, have ? C_ALERT : C_WARN, 0);
        lv_label_set_text(ui.rpm, "----");
        lv_obj_set_style_text_color(ui.rpm, C_MUTED, 0);
        ui.rpm_last[0] = '\0';
        set_rpm_arc(0);
        tiles_invalid();
        lv_label_set_text(ui.info, "ADV --   PW --   VE --");
        lv_label_set_text(ui.flags, "");
        lv_label_set_text(ui.rate, "");
        return;
    }

    // status line
    snprintf(buf, sizeof(buf), LV_SYMBOL_OK "  VEMS  %s  %lu", d.mode == VEMS_MODE_TF ? "TF" : "A", (unsigned long)d.baud);
    lv_label_set_text(ui.status, buf);
    lv_obj_set_style_text_color(ui.status, C_OK, 0);

    // RPM
    snprintf(buf, sizeof(buf), "%u", d.rpm);
    if (strcmp(buf, ui.rpm_last) != 0) {
        strlcpy(ui.rpm_last, buf, sizeof(ui.rpm_last));
        lv_label_set_text(ui.rpm, buf);
    }
    lv_obj_set_style_text_color(ui.rpm, d.rpm >= RPM_SHIFT ? C_ALERT : C_TEXT, 0);
    set_rpm_arc(d.rpm);

    // tiles
    snprintf(buf, sizeof(buf), "%.0f", d.map_kpa);
    tile_set(&ui.tiles[T_MAP], buf, d.map_kpa > 250 ? C_WARN : C_TEXT);

    if (d.lambda > 0.5f && d.lambda < 2.0f) {
        snprintf(buf, sizeof(buf), "%.2f", d.lambda);
        tile_set(&ui.tiles[T_LAMBDA], buf, (d.lambda < 0.75f || d.lambda > 1.15f) ? C_WARN : C_TEXT);
    } else {
        tile_set(&ui.tiles[T_LAMBDA], "--", C_MUTED);
    }

    snprintf(buf, sizeof(buf), "%.0f", d.tps_pct);
    tile_set(&ui.tiles[T_TPS], buf, C_TEXT);

    snprintf(buf, sizeof(buf), "%.0f", d.clt_c);
    tile_set(&ui.tiles[T_CLT], buf, d.clt_c >= 105 ? C_ALERT : (d.clt_c >= 98 ? C_WARN : (d.clt_c < 40 ? C_ACCENT : C_TEXT)));

    snprintf(buf, sizeof(buf), "%.0f", d.iat_c);
    tile_set(&ui.tiles[T_IAT], buf, d.iat_c >= 60 ? C_WARN : C_TEXT);

    snprintf(buf, sizeof(buf), "%.1f", d.batt_v);
    tile_set(&ui.tiles[T_BATT], buf, (d.batt_v < 11.5f || d.batt_v > 15.5f) ? C_ALERT : (d.batt_v < 12.5f ? C_WARN : C_TEXT));

    // bottom info
    snprintf(buf, sizeof(buf), "ADV %.1f   PW %.2f   VE %u", d.ign_adv_deg, d.pulsewidth_ms, d.ve);
    lv_label_set_text(ui.info, buf);

    snprintf(buf, sizeof(buf), "%s%s%s%s%s%s",
             (d.engine_status & VEMS_ENG_RUNNING) ? "#2ED47A RUN#  " : "#5A6272 STOP#  ",
             (d.engine_status & VEMS_ENG_CRANK) ? "#FFB020 CRANK#  " : "",
             (d.engine_status & VEMS_ENG_WARMUP) ? "#00C8FF WARM#  " : "",
             (d.engine_status & VEMS_ENG_IDLE) ? "#C9CFDA IDLE#  " : "",
             (d.status1 & 0x04) ? "#2ED47A CL#  " : "",
             (d.status1 & 0x10) ? "#FF3B30 CUT#" : "");
    lv_label_set_text(ui.flags, buf);

    // frame rate, once per second
    if (now - ui.rate_t0 >= 1000000) {
        float hz = (d.seq - ui.rate_seq0) * 1e6f / (float)(now - ui.rate_t0);
        snprintf(buf, sizeof(buf), "%.0f Hz", hz);
        lv_label_set_text(ui.rate, buf);
        ui.rate_t0 = now;
        ui.rate_seq0 = d.seq;
    }
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

    const esp_timer_create_args_t tick_args = {.callback = tick_cb, .name = "lv_tick"};
    esp_timer_handle_t tick;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick), TAG, "tick timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick, LVGL_TICK_MS * 1000), TAG, "tick start");

    build_ui();
    lv_timer_create(ui_update_cb, UI_UPDATE_MS, NULL);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, NULL, 1);
    return ESP_OK;
}
