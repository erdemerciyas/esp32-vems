/*
 * Single gauge: one channel at a time, large, with a 270 degree ring over its range and the session
 * min / max. Tap the left / right half for the previous / next channel, long press resets min / max.
 * The channel list is whatever the source delivers.
 */
#include <math.h>
#include <stdio.h>
#include "ui_priv.h"

static struct {
    lv_obj_t *arc;
    lv_obj_t *name;
    lv_obj_t *value;
    lv_obj_t *unit;
    lv_obj_t *minmax;
    lv_obj_t *index;
    vems_ch_t list[CH_COUNT];
    int count;
    int cur;
    lv_color_t arc_color;
} f;

uint32_t ui_focus_channels(void)
{
    return f.count ? CH_BIT(f.list[f.cur]) : 0;
}

lv_obj_t *ui_focus_create(int page, int pages, uint32_t mask)
{
    lv_obj_t *scr = ui_screen_create();
    vems_ch_t keep = f.count ? f.list[f.cur] : CH_RPM;
    f.count = ui_ch_list(mask, f.list, CH_COUNT);
    f.cur = 0;
    for (int i = 0; i < f.count; i++) {
        if (f.list[i] == keep) {
            f.cur = i;
        }
    }

    f.arc = lv_arc_create(scr);
    lv_obj_set_size(f.arc, 452, 452);
    lv_obj_center(f.arc);
    lv_arc_set_bg_angles(f.arc, 135, 45);
    lv_arc_set_range(f.arc, 0, 1000);
    lv_arc_set_value(f.arc, 0);
    lv_obj_remove_style(f.arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(f.arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(f.arc, 20, LV_PART_MAIN);
    lv_obj_set_style_arc_color(f.arc, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_width(f.arc, 20, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(f.arc, C_PRIMARY, LV_PART_INDICATOR);
    f.arc_color = C_PRIMARY;

    f.name = ui_label(scr, &ui_font_label_lg, C_TEXT, "");
    lv_obj_set_style_text_letter_space(f.name, 2, 0);
    lv_obj_align(f.name, LV_ALIGN_TOP_MID, 0, 92);

    f.value = ui_label(scr, &ui_font_big, C_MUTED, "--");
    lv_obj_align(f.value, LV_ALIGN_TOP_MID, 0, 150);

    f.unit = ui_label(scr, &ui_font_label_lg, C_MUTED, "");
    lv_obj_set_style_text_letter_space(f.unit, 3, 0);
    lv_obj_align(f.unit, LV_ALIGN_TOP_MID, 0, 248);

    f.minmax = ui_label(scr, &ui_font_label_lg, C_MUTED, "");
    lv_obj_align(f.minmax, LV_ALIGN_TOP_MID, 0, 296);

    f.index = ui_label(scr, &ui_font_label, C_TICK, "");
    lv_obj_set_style_text_letter_space(f.index, 2, 0);
    lv_obj_align(f.index, LV_ALIGN_TOP_MID, 0, 334);

    lv_obj_t *l = ui_label(scr, &lv_font_montserrat_16, C_TICK, LV_SYMBOL_LEFT);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 46, 0);
    lv_obj_t *r = ui_label(scr, &lv_font_montserrat_16, C_TICK, LV_SYMBOL_RIGHT);
    lv_obj_align(r, LV_ALIGN_RIGHT_MID, -46, 0);

    lv_obj_t *hint = ui_label(scr, &ui_font_label, C_TICK, "basılı tut: min / maks sıfırla");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 380);

    ui_page_dots(scr, page, pages);
    return scr;
}

void ui_focus_step(int dir)
{
    if (f.count) {
        f.cur = (f.cur + dir + f.count) % f.count;
    }
}

void ui_focus_reset(void)
{
    if (f.count) {
        ui_ch_reset(f.list[f.cur]);
    }
}

void ui_focus_update(const vems_data_t *d, bool fresh)
{
    char buf[96], a[16], b[16];
    if (!f.count) {
        return;
    }
    const vems_ch_t c = f.list[f.cur];
    const ui_ch_info_t *ci = &UI_CH[c];
    const float x = fresh ? ui_ch_value(d, c) : NAN;

    ui_label_set(f.name, ci->name);
    ui_ch_format(c, x, buf, sizeof(buf));
    ui_label_set(f.value, buf);
    const lv_color_t col = ui_ch_color(c, x);
    lv_obj_set_style_text_color(f.value, col, 0);

    if (c == CH_AFR || c == CH_TARGET) {
        ui_label_set(f.unit, ui_fuel_name());
    } else if (c == CH_RUNTIME) {
        ui_label_set(f.unit, "dk:sn");
    } else {
        ui_label_set(f.unit, ci->unit);
    }

    float lo, hi;
    if (ui_ch_minmax(c, &lo, &hi)) {
        ui_ch_format(c, lo, a, sizeof(a));
        ui_ch_format(c, hi, b, sizeof(b));
        snprintf(buf, sizeof(buf), "#8A909C MİN#  %s     #8A909C MAKS#  %s", a, b);
    } else {
        snprintf(buf, sizeof(buf), "#8A909C MİN#  --     #8A909C MAKS#  --");
    }
    lv_label_set_recolor(f.minmax, true);
    ui_label_set(f.minmax, buf);

    snprintf(buf, sizeof(buf), "%d / %d", f.cur + 1, f.count);
    ui_label_set(f.index, buf);

    // ring: AFR is scaled in lambda so the zones mean the same for every fuel
    float frac = 0;
    if (!isnan(x)) {
        if (c == CH_AFR || c == CH_TARGET) {
            frac = (x / ui_stoich_afr() - 0.7f) / 0.6f;
        } else if (c == CH_RPM) {
            frac = x / g_settings.rpm_max;
        } else {
            frac = (x - ci->min) / (ci->max - ci->min);
        }
    }
    frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    lv_arc_set_value(f.arc, (int16_t)(frac * 1000));
    const lv_color_t ac = (col.full == C_TEXT.full || col.full == C_MUTED.full) ? C_PRIMARY : col;
    if (ac.full != f.arc_color.full) {
        f.arc_color = ac;
        lv_obj_set_style_arc_color(f.arc, ac, LV_PART_INDICATOR);
    }
}
