#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "board_lcd.h"
#include "ui_priv.h"

// ---------------------------------------------------------------------------
// AFR standards (gasoline reference, expressed in lambda so they hold for any fuel)
//   < 0.75  very rich      : fouling plugs, washing cylinder walls
//   0.75-0.82 turbo WOT    : boosted full load (AFR 11.0-12.0)
//   0.82-0.90 power        : NA full load best power (AFR 12.0-13.2)
//   0.90-0.97 slightly rich: accel / warm-up (AFR 13.2-14.3)
//   0.97-1.03 stoich       : idle / closed loop cruise (AFR 14.3-15.1)
//   1.03-1.10 economy      : light load lean cruise (AFR 15.1-16.2)
//   > 1.10  very lean      : misfire / high EGT / knock risk
// ---------------------------------------------------------------------------
const ui_afr_zone_t UI_AFR_ZONES[] = {
    {0.75f, "COK ZENGIN",    0xFF3B30},
    {0.82f, "TURBO TAM GAZ", 0xFF7A1A},
    {0.90f, "TAM GAZ / GUC", 0xFFB020},
    {0.97f, "HAFIF ZENGIN",  0xC8E03C},
    {1.03f, "STOKIYOMETRIK", 0x2ED47A},
    {1.10f, "EKONOMI",       0x00C8FF},
    {9.99f, "COK FAKIR",     0xFF3B30},
};
const int UI_AFR_ZONE_COUNT = sizeof(UI_AFR_ZONES) / sizeof(UI_AFR_ZONES[0]);

float ui_stoich_afr(void)
{
#if CONFIG_VEMS_UI_FUEL_E85
    return 9.8f;
#elif CONFIG_VEMS_UI_FUEL_LPG
    return 15.5f;
#elif CONFIG_VEMS_UI_FUEL_METHANOL
    return 6.4f;
#else
    return 14.7f;
#endif
}

const char *ui_fuel_name(void)
{
#if CONFIG_VEMS_UI_FUEL_E85
    return "E85";
#elif CONFIG_VEMS_UI_FUEL_LPG
    return "LPG";
#elif CONFIG_VEMS_UI_FUEL_METHANOL
    return "METANOL";
#else
    return "BENZIN";
#endif
}

const ui_afr_zone_t *ui_afr_zone(float lambda)
{
    for (int i = 0; i < UI_AFR_ZONE_COUNT - 1; i++) {
        if (lambda < UI_AFR_ZONES[i].lambda_max) {
            return &UI_AFR_ZONES[i];
        }
    }
    return &UI_AFR_ZONES[UI_AFR_ZONE_COUNT - 1];
}

bool ui_lambda_valid(const vems_data_t *d)
{
    // raw 250..255 = WBO2 not ready / heating / not connected
    return d->ego_raw < 250 && d->lambda > 0.60f && d->lambda < 1.60f;
}

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------
lv_obj_t *ui_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, txt);
    return l;
}

void ui_label_set(lv_obj_t *label, const char *txt)
{
    if (strcmp(lv_label_get_text(label), txt) != 0) {
        lv_label_set_text(label, txt);
    }
}

void ui_tile_create(ui_tile_t *t, lv_obj_t *parent, int x, int y, const char *caption)
{
    t->box = lv_obj_create(parent);
    lv_obj_set_size(t->box, UI_TILE_W, UI_TILE_H);
    lv_obj_align(t->box, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(t->box, C_TILE, 0);
    lv_obj_set_style_bg_opa(t->box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(t->box, C_TILE_BRD, 0);
    lv_obj_set_style_border_width(t->box, 1, 0);
    lv_obj_set_style_radius(t->box, 12, 0);
    lv_obj_set_style_pad_all(t->box, 0, 0);
    lv_obj_clear_flag(t->box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    t->caption = ui_label(t->box, &lv_font_montserrat_12, C_MUTED, caption);
    lv_obj_align(t->caption, LV_ALIGN_TOP_MID, 0, 6);

    t->value = ui_label(t->box, &lv_font_montserrat_24, C_MUTED, "--");
    lv_obj_align(t->value, LV_ALIGN_BOTTOM_MID, 0, -6);
    strcpy(t->last, "--");
}

void ui_tile_set(ui_tile_t *t, const char *txt, lv_color_t color)
{
    if (strcmp(t->last, txt) != 0) {
        strlcpy(t->last, txt, sizeof(t->last));
        lv_label_set_text(t->value, txt);
    }
    if (lv_obj_get_style_text_color(t->value, 0).full != color.full) {
        lv_obj_set_style_text_color(t->value, color, 0);
    }
}

void ui_page_dots(lv_obj_t *scr, int active, int count)
{
    const int size = 8, gap = 10;
    int total = count * size + (count - 1) * gap;
    for (int i = 0; i < count; i++) {
        lv_obj_t *d = lv_obj_create(scr);
        lv_obj_set_size(d, size, size);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_bg_color(d, i == active ? C_ACCENT : C_TILE_BRD, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(d, LV_ALIGN_TOP_LEFT, (BOARD_LCD_H_RES - total) / 2 + i * (size + gap), 448);
    }
}

void ui_meter_label_div10_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->type == LV_METER_DRAW_PART_TICK && dsc->text != NULL) {
        lv_snprintf(dsc->text, 16, "%d", (int)(dsc->value / 10));
    }
}

lv_obj_t *ui_ring_meter(lv_obj_t *scr)
{
    lv_obj_t *m = lv_meter_create(scr);
    lv_obj_set_size(m, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_center(m);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 26, 0);                 // ticks start inside the outer ring
    lv_obj_set_style_text_color(m, C_MUTED, 0);
    lv_obj_set_style_text_font(m, &lv_font_montserrat_16, 0);
    lv_obj_remove_style(m, NULL, LV_PART_INDICATOR);    // no needle hub
    lv_obj_clear_flag(m, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(m, ui_meter_label_div10_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    return m;
}
