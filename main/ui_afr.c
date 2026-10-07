/*
 * AFR screen: zone-colored ring (lambda 0.68 .. 1.36 x stoich) with a moving marker,
 * big AFR value + zone name, lambda / target / EGO correction, session MIN / MAX / delta,
 * and the reference AFR standards for the selected fuel.
 */
#include <stdio.h>
#include <math.h>
#include "sdkconfig.h"
#include "ui_priv.h"

#define LAMBDA_MIN      0.68f
#define LAMBDA_MAX      1.36f
#define MARKER_HALF     1           // marker half width in scale units (0.1 AFR)

enum { T_LAMBDA, T_TARGET, T_EGO, T_MIN, T_MAX, T_DELTA, T_COUNT };

static struct {
    lv_obj_t *meter;
    lv_meter_indicator_t *marker;
    int scale_min;
    int scale_max;
    lv_obj_t *afr;
    lv_obj_t *zone;
    ui_tile_t tiles[T_COUNT];
    float afr_min;
    float afr_max;
    bool have_minmax;
} a;

static inline int afr_to_scale(float afr)
{
    return (int)lroundf(afr * 10.0f);
}

lv_obj_t *ui_afr_create(int page, int pages)
{
    const float st = ui_stoich_afr();
    char buf[96];
    lv_obj_t *scr = ui_screen_create();

    // ring scale in 0.1 AFR units, labels in whole AFR
    a.meter = ui_ring_meter(scr);
    a.scale_min = afr_to_scale(st * LAMBDA_MIN);
    a.scale_max = afr_to_scale(st * LAMBDA_MAX);
    lv_meter_scale_t *s = lv_meter_add_scale(a.meter);
    lv_meter_set_scale_range(a.meter, s, a.scale_min, a.scale_max, 270, 135);
    lv_meter_set_scale_ticks(a.meter, s, (a.scale_max - a.scale_min) / 2 + 1, 2, 8, C_TICK);
    lv_meter_set_scale_major_ticks(a.meter, s, 5, 3, 16, lv_color_hex(0xC9CFDA), 12);

    // colored zone bands
    float lo = LAMBDA_MIN;
    for (int i = 0; i < UI_AFR_ZONE_COUNT && lo < LAMBDA_MAX; i++) {
        float hi = UI_AFR_ZONES[i].lambda_max < LAMBDA_MAX ? UI_AFR_ZONES[i].lambda_max : LAMBDA_MAX;
        if (hi <= lo) {
            continue;
        }
        lv_meter_indicator_t *band = lv_meter_add_arc(a.meter, s, 14, lv_color_hex(UI_AFR_ZONES[i].color), 24);
        lv_meter_set_indicator_start_value(a.meter, band, afr_to_scale(st * lo));
        lv_meter_set_indicator_end_value(a.meter, band, afr_to_scale(st * hi));
        band->opa = LV_OPA_60;
        lo = hi;
    }

    // stoich tick + live marker
    lv_meter_indicator_t *stoich = lv_meter_add_scale_lines(a.meter, s, C_TEXT, C_TEXT, false, 0);
    lv_meter_set_indicator_start_value(a.meter, stoich, afr_to_scale(st));
    lv_meter_set_indicator_end_value(a.meter, stoich, afr_to_scale(st));

    a.marker = lv_meter_add_arc(a.meter, s, 22, C_TEXT, 28);
    lv_meter_set_indicator_start_value(a.meter, a.marker, a.scale_min);
    lv_meter_set_indicator_end_value(a.meter, a.marker, a.scale_min + 2 * MARKER_HALF);
    a.marker->opa = LV_OPA_TRANSP;

    // header
    snprintf(buf, sizeof(buf), "AFR  |  %s %.1f", ui_fuel_name(), st);
    lv_obj_t *hdr = ui_label(scr, &ui_font_label, C_MUTED, buf);
    lv_obj_set_style_text_letter_space(hdr, 2, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 84);

    a.afr = ui_label(scr, &ui_font_big, C_MUTED, "--.-");
    lv_obj_align(a.afr, LV_ALIGN_TOP_MID, 0, 104);

    a.zone = ui_label(scr, &ui_font_label_lg, C_MUTED, "SENSOR BEKLENIYOR");
    lv_obj_set_style_text_letter_space(a.zone, 2, 0);
    lv_obj_align(a.zone, LV_ALIGN_TOP_MID, 0, 176);

    ui_tile_create(&a.tiles[T_LAMBDA], scr, UI_COL_X0, 206, "LAMBDA");
    ui_tile_create(&a.tiles[T_TARGET], scr, UI_COL_X1, 206, "HEDEF AFR");
    ui_tile_create(&a.tiles[T_EGO],    scr, UI_COL_X2, 206, "EGO KOR %");
    ui_tile_create(&a.tiles[T_MIN],    scr, UI_COL_X0, 272, "MIN AFR");
    ui_tile_create(&a.tiles[T_DELTA],  scr, UI_COL_X1, 272, "FARK");
    ui_tile_create(&a.tiles[T_MAX],    scr, UI_COL_X2, 272, "MAX AFR");

    // reference standards (lambda windows converted to the selected fuel)
    snprintf(buf, sizeof(buf), "ROLANTI %.1f-%.1f     SEYIR %.1f-%.1f",
             st * 0.97f, st * 1.00f, st * 1.00f, st * 1.05f);
    lv_obj_t *ref1 = ui_label(scr, &ui_font_label, C_MUTED, buf);
    lv_obj_align(ref1, LV_ALIGN_TOP_MID, 0, 382);
    snprintf(buf, sizeof(buf), "TAM GAZ NA %.1f-%.1f     TURBO %.1f-%.1f",
             st * 0.85f, st * 0.90f, st * 0.78f, st * 0.82f);
    lv_obj_t *ref2 = ui_label(scr, &ui_font_label, C_MUTED, buf);
    lv_obj_align(ref2, LV_ALIGN_TOP_MID, 0, 400);
    snprintf(buf, sizeof(buf), "SOGUK %.1f-%.1f     EKONOMI %.1f-%.1f",
             st * 0.82f, st * 0.88f, st * 1.05f, st * 1.10f);
    lv_obj_t *ref3 = ui_label(scr, &ui_font_label, C_MUTED, buf);
    lv_obj_align(ref3, LV_ALIGN_TOP_MID, 0, 418);

    lv_obj_t *ref_title = ui_label(scr, &ui_font_label, C_BRAND, "STANDART AFR");
    lv_obj_set_style_text_letter_space(ref_title, 3, 0);
    lv_obj_align(ref_title, LV_ALIGN_TOP_MID, 0, 344);

    ui_page_dots(scr, page, pages);
    return scr;
}

void ui_afr_reset_minmax(void)
{
    a.have_minmax = false;
    ui_tile_set(&a.tiles[T_MIN], "--", C_MUTED);
    ui_tile_set(&a.tiles[T_MAX], "--", C_MUTED);
}

static void set_marker(int v, bool visible)
{
    lv_opa_t opa = visible ? LV_OPA_COVER : LV_OPA_TRANSP;
    if (v < a.scale_min + MARKER_HALF) {
        v = a.scale_min + MARKER_HALF;
    } else if (v > a.scale_max - MARKER_HALF) {
        v = a.scale_max - MARKER_HALF;
    }
    if (a.marker->opa != opa) {
        a.marker->opa = opa;
        lv_obj_invalidate(a.meter);
    }
    if (a.marker->start_value != v - MARKER_HALF) {
        lv_meter_set_indicator_start_value(a.meter, a.marker, v - MARKER_HALF);
        lv_meter_set_indicator_end_value(a.meter, a.marker, v + MARKER_HALF);
    }
}

void ui_afr_update(const vems_data_t *d, bool fresh)
{
    const float st = ui_stoich_afr();
    char buf[32];

    // target and EGO correction are valid whenever the ECU talks
    if (fresh) {
        snprintf(buf, sizeof(buf), "%.1f", d->lambda_target * st);
        ui_tile_set(&a.tiles[T_TARGET], buf, C_TEXT);
        snprintf(buf, sizeof(buf), "%.0f", d->ego_corr_pct);
        ui_tile_set(&a.tiles[T_EGO], buf, fabsf(d->ego_corr_pct - 100.0f) > 15.0f ? C_WARN : C_TEXT);
    } else {
        ui_tile_set(&a.tiles[T_TARGET], "--", C_MUTED);
        ui_tile_set(&a.tiles[T_EGO], "--", C_MUTED);
    }

    if (!fresh || !ui_lambda_valid(d)) {
        ui_label_set(a.afr, "--.-");
        lv_obj_set_style_text_color(a.afr, C_MUTED, 0);
        ui_label_set(a.zone, fresh ? "SENSOR HAZIR DEGIL" : "VERI YOK");
        lv_obj_set_style_text_color(a.zone, fresh ? C_WARN : C_ALERT, 0);
        ui_tile_set(&a.tiles[T_LAMBDA], "--", C_MUTED);
        ui_tile_set(&a.tiles[T_DELTA], "--", C_MUTED);
        set_marker(a.scale_min, false);
        return;
    }

    const float afr = d->lambda * st;
    const ui_afr_zone_t *z = ui_afr_zone(d->lambda);
    const lv_color_t zc = lv_color_hex(z->color);

    snprintf(buf, sizeof(buf), "%.1f", afr);
    ui_label_set(a.afr, buf);
    lv_obj_set_style_text_color(a.afr, zc, 0);
    ui_label_set(a.zone, z->name);
    lv_obj_set_style_text_color(a.zone, zc, 0);
    set_marker(afr_to_scale(afr), true);

    snprintf(buf, sizeof(buf), "%.2f", d->lambda);
    ui_tile_set(&a.tiles[T_LAMBDA], buf, zc);

    float delta = afr - d->lambda_target * st;
    snprintf(buf, sizeof(buf), "%+.1f", delta);
    ui_tile_set(&a.tiles[T_DELTA], buf, fabsf(delta) > 1.0f ? C_WARN : C_OK);

    if (!a.have_minmax) {
        a.afr_min = a.afr_max = afr;
        a.have_minmax = true;
    } else {
        a.afr_min = fminf(a.afr_min, afr);
        a.afr_max = fmaxf(a.afr_max, afr);
    }
    snprintf(buf, sizeof(buf), "%.1f", a.afr_min);
    ui_tile_set(&a.tiles[T_MIN], buf, lv_color_hex(ui_afr_zone(a.afr_min / st)->color));
    snprintf(buf, sizeof(buf), "%.1f", a.afr_max);
    ui_tile_set(&a.tiles[T_MAX], buf, lv_color_hex(ui_afr_zone(a.afr_max / st)->color));
}
