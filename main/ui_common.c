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
    lv_obj_set_style_radius(t->box, 4, 0);
    lv_obj_set_style_pad_all(t->box, 0, 0);
    lv_obj_clear_flag(t->box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    t->stripe = lv_obj_create(t->box);
    lv_obj_remove_style_all(t->stripe);
    lv_obj_set_size(t->stripe, 3, UI_TILE_H - 18);
    lv_obj_align(t->stripe, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(t->stripe, C_BRAND, 0);
    lv_obj_set_style_bg_opa(t->stripe, LV_OPA_COVER, 0);

    t->caption = ui_label(t->box, &ui_font_label, C_MUTED, caption);
    lv_obj_set_style_text_letter_space(t->caption, 1, 0);
    lv_obj_align(t->caption, LV_ALIGN_TOP_LEFT, 10, 5);

    t->value = ui_label(t->box, &ui_font_value, C_MUTED, "--");
    lv_obj_align(t->value, LV_ALIGN_BOTTOM_RIGHT, -9, -3);
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
        // plain values keep the brand stripe, warnings / zones light it up
        bool plain = color.full == C_TEXT.full || color.full == C_MUTED.full;
        lv_obj_set_style_bg_color(t->stripe, plain ? C_BRAND : color, 0);
        lv_obj_set_style_border_color(t->box, plain ? C_TILE_BRD : color, 0);
        lv_obj_set_style_border_opa(t->box, plain ? LV_OPA_COVER : LV_OPA_60, 0);
    }
}

void ui_page_dots(lv_obj_t *scr, int active, int count)
{
    const int w_on = 22, w_off = 10, h = 4, gap = 6;
    int total = w_on + (count - 1) * w_off + (count - 1) * gap;
    int x = (BOARD_LCD_H_RES - total) / 2;
    for (int i = 0; i < count; i++) {
        lv_obj_t *d = lv_obj_create(scr);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, i == active ? w_on : w_off, h);
        lv_obj_set_style_radius(d, 2, 0);
        lv_obj_set_style_bg_color(d, i == active ? C_BRAND : C_TILE_BRD, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_align(d, LV_ALIGN_TOP_LEFT, x, 450);
        x += (i == active ? w_on : w_off) + gap;
    }
}

void ui_brand_footer(lv_obj_t *scr, int y)
{
    lv_obj_t *logo = lv_img_create(scr);
    lv_img_set_src(logo, &img_extremeecu_logo_small);
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, y);
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
    lv_obj_set_style_text_font(m, &ui_font_label_lg, 0);
    lv_obj_remove_style(m, NULL, LV_PART_INDICATOR);    // no needle hub
    lv_obj_clear_flag(m, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(m, ui_meter_label_div10_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    return m;
}
