/*
 * Main dashboard: RPM ring, big RPM, 2x3 tiles (MAP, AFR, TPS / CLT, IAT, BATT), info + flags.
 */
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_timer.h"
#include "ui_priv.h"

#define RPM_MAX     CONFIG_VEMS_UI_RPM_MAX
#define RPM_SHIFT   CONFIG_VEMS_UI_RPM_SHIFT
#define RPM_WARN    (RPM_SHIFT - 1000)

enum { T_MAP, T_AFR, T_TPS, T_CLT, T_IAT, T_BATT, T_COUNT };

static struct {
    lv_obj_t *meter;
    lv_meter_indicator_t *rpm_arc;
    lv_color_t rpm_color;
    lv_obj_t *status;
    lv_obj_t *rpm;
    lv_obj_t *info;
    lv_obj_t *flags;
    lv_obj_t *rate;
    ui_tile_t tiles[T_COUNT];
    int64_t rate_t0;
    uint32_t rate_seq0;
} m;

lv_obj_t *ui_main_create(int page, int pages)
{
    lv_obj_t *scr = ui_screen_create();

    // RPM ring, scale in 100 rpm units, labels in 1000 rpm
    m.meter = ui_ring_meter(scr);
    const int max = RPM_MAX / 100;
    lv_meter_scale_t *s = lv_meter_add_scale(m.meter);
    lv_meter_set_scale_range(m.meter, s, 0, max, 270, 135);
    lv_meter_set_scale_ticks(m.meter, s, max / 2 + 1, 2, 8, C_TICK);
    lv_meter_set_scale_major_ticks(m.meter, s, 5, 3, 16, lv_color_hex(0xC9CFDA), 12);

    lv_meter_indicator_t *red_ticks = lv_meter_add_scale_lines(m.meter, s, C_ALERT, C_ALERT, false, 0);
    lv_meter_set_indicator_start_value(m.meter, red_ticks, RPM_SHIFT / 100);
    lv_meter_set_indicator_end_value(m.meter, red_ticks, max);
    lv_meter_indicator_t *red_arc = lv_meter_add_arc(m.meter, s, 3, C_ALERT, 0);
    lv_meter_set_indicator_start_value(m.meter, red_arc, RPM_SHIFT / 100);
    lv_meter_set_indicator_end_value(m.meter, red_arc, max);

    lv_meter_indicator_t *track = lv_meter_add_arc(m.meter, s, 16, C_TRACK, 24);
    lv_meter_set_indicator_start_value(m.meter, track, 0);
    lv_meter_set_indicator_end_value(m.meter, track, max);
    m.rpm_color = C_ACCENT;
    m.rpm_arc = lv_meter_add_arc(m.meter, s, 16, m.rpm_color, 24);
    lv_meter_set_indicator_start_value(m.meter, m.rpm_arc, 0);
    lv_meter_set_indicator_end_value(m.meter, m.rpm_arc, 0);

    m.status = ui_label(scr, &lv_font_montserrat_14, C_WARN, LV_SYMBOL_USB "  VEMS BEKLENIYOR");
    lv_obj_align(m.status, LV_ALIGN_TOP_MID, 0, 88);

    m.rpm = ui_label(scr, &lv_font_montserrat_48, C_MUTED, "----");
    lv_obj_align(m.rpm, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_t *rpm_cap = ui_label(scr, &lv_font_montserrat_14, C_MUTED, "RPM");
    lv_obj_align(rpm_cap, LV_ALIGN_TOP_MID, 0, 176);

    ui_tile_create(&m.tiles[T_MAP],  scr, UI_COL_X0, 206, "MAP kPa");
    ui_tile_create(&m.tiles[T_AFR],  scr, UI_COL_X1, 206, "AFR");
    ui_tile_create(&m.tiles[T_TPS],  scr, UI_COL_X2, 206, "TPS %");
    ui_tile_create(&m.tiles[T_CLT],  scr, UI_COL_X0, 272, "CLT C");
    ui_tile_create(&m.tiles[T_IAT],  scr, UI_COL_X1, 272, "IAT C");
    ui_tile_create(&m.tiles[T_BATT], scr, UI_COL_X2, 272, "AKU V");

    m.info = ui_label(scr, &lv_font_montserrat_16, C_TEXT, "ADV --   PW --   VE --");
    lv_obj_align(m.info, LV_ALIGN_TOP_MID, 0, 342);

    m.flags = ui_label(scr, &lv_font_montserrat_14, C_MUTED, "");
    lv_label_set_recolor(m.flags, true);
    lv_obj_align(m.flags, LV_ALIGN_TOP_MID, 0, 380);

    m.rate = ui_label(scr, &lv_font_montserrat_12, C_TICK, "");
    lv_obj_align(m.rate, LV_ALIGN_TOP_MID, 0, 412);

    ui_page_dots(scr, page, pages);
    return scr;
}

static void set_rpm_arc(uint16_t rpm)
{
    lv_color_t col = rpm >= RPM_SHIFT ? C_ALERT : (rpm >= RPM_WARN ? C_WARN : C_ACCENT);
    if (col.full != m.rpm_color.full) {
        m.rpm_color = col;
        m.rpm_arc->type_data.arc.color = col;
        lv_obj_invalidate(m.meter);
    }
    int v = rpm / 100;
    if (v > RPM_MAX / 100) {
        v = RPM_MAX / 100;
    }
    if (m.rpm_arc->end_value != v) {
        lv_meter_set_indicator_end_value(m.meter, m.rpm_arc, v);
    }
}

void ui_main_update(const vems_data_t *d, bool fresh)
{
    char buf[128];

    if (!fresh) {
        ui_label_set(m.status, d->seq ? LV_SYMBOL_WARNING "  VERI YOK" : LV_SYMBOL_USB "  VEMS BEKLENIYOR");
        lv_obj_set_style_text_color(m.status, d->seq ? C_ALERT : C_WARN, 0);
        ui_label_set(m.rpm, "----");
        lv_obj_set_style_text_color(m.rpm, C_MUTED, 0);
        set_rpm_arc(0);
        for (int i = 0; i < T_COUNT; i++) {
            ui_tile_set(&m.tiles[i], "--", C_MUTED);
        }
        ui_label_set(m.info, "ADV --   PW --   VE --");
        ui_label_set(m.flags, "");
        ui_label_set(m.rate, "");
        return;
    }

    snprintf(buf, sizeof(buf), LV_SYMBOL_OK "  VEMS  %s  %lu", d->mode == VEMS_MODE_TF ? "TF" : "A", (unsigned long)d->baud);
    ui_label_set(m.status, buf);
    lv_obj_set_style_text_color(m.status, C_OK, 0);

    snprintf(buf, sizeof(buf), "%u", d->rpm);
    ui_label_set(m.rpm, buf);
    lv_obj_set_style_text_color(m.rpm, d->rpm >= RPM_SHIFT ? C_ALERT : C_TEXT, 0);
    set_rpm_arc(d->rpm);

    snprintf(buf, sizeof(buf), "%.0f", d->map_kpa);
    ui_tile_set(&m.tiles[T_MAP], buf, d->map_kpa > 250 ? C_WARN : C_TEXT);

    if (ui_lambda_valid(d)) {
        snprintf(buf, sizeof(buf), "%.1f", d->lambda * ui_stoich_afr());
        ui_tile_set(&m.tiles[T_AFR], buf, lv_color_hex(ui_afr_zone(d->lambda)->color));
    } else {
        ui_tile_set(&m.tiles[T_AFR], "--", C_MUTED);
    }

    snprintf(buf, sizeof(buf), "%.0f", d->tps_pct);
    ui_tile_set(&m.tiles[T_TPS], buf, C_TEXT);

    snprintf(buf, sizeof(buf), "%.0f", d->clt_c);
    ui_tile_set(&m.tiles[T_CLT], buf, d->clt_c >= 105 ? C_ALERT : (d->clt_c >= 98 ? C_WARN : (d->clt_c < 40 ? C_ACCENT : C_TEXT)));

    snprintf(buf, sizeof(buf), "%.0f", d->iat_c);
    ui_tile_set(&m.tiles[T_IAT], buf, d->iat_c >= 60 ? C_WARN : C_TEXT);

    snprintf(buf, sizeof(buf), "%.1f", d->batt_v);
    ui_tile_set(&m.tiles[T_BATT], buf, (d->batt_v < 11.5f || d->batt_v > 15.5f) ? C_ALERT : (d->batt_v < 12.5f ? C_WARN : C_TEXT));

    snprintf(buf, sizeof(buf), "ADV %.1f   PW %.2f   VE %u", d->ign_adv_deg, d->pulsewidth_ms, d->ve);
    ui_label_set(m.info, buf);

    snprintf(buf, sizeof(buf), "%s%s%s%s%s%s",
             (d->engine_status & VEMS_ENG_RUNNING) ? "#2ED47A RUN#  " : "#5A6272 STOP#  ",
             (d->engine_status & VEMS_ENG_CRANK) ? "#FFB020 CRANK#  " : "",
             (d->engine_status & VEMS_ENG_WARMUP) ? "#00C8FF WARM#  " : "",
             (d->engine_status & VEMS_ENG_IDLE) ? "#C9CFDA IDLE#  " : "",
             (d->status1 & 0x04) ? "#2ED47A CL#  " : "",
             (d->status1 & 0x10) ? "#FF3B30 CUT#" : "");
    ui_label_set(m.flags, buf);

    int64_t now = esp_timer_get_time();
    if (now - m.rate_t0 >= 1000000) {
        float hz = (d->seq - m.rate_seq0) * 1e6f / (float)(now - m.rate_t0);
        snprintf(buf, sizeof(buf), "%.0f Hz", hz);
        ui_label_set(m.rate, buf);
        m.rate_t0 = now;
        m.rate_seq0 = d->seq;
    }
}
