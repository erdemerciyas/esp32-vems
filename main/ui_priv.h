#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"
#include "vems_proto.h"

// palette (EXTREMEECU: black / carbon / brand red)
#define C_BG        lv_color_hex(0x000000)
#define C_TRACK     lv_color_hex(0x16181D)
#define C_TILE      lv_color_hex(0x0B0D11)
#define C_TILE_BRD  lv_color_hex(0x22262E)
#define C_TEXT      lv_color_hex(0xF4F5F7)
#define C_MUTED     lv_color_hex(0x8A909C)
#define C_TICK      lv_color_hex(0x4A505C)
#define C_ACCENT    lv_color_hex(0x00C8FF)  // "cold" (CLT warm-up, economy zone)
#define C_OK        lv_color_hex(0x2ED47A)
#define C_WARN      lv_color_hex(0xFFB020)
#define C_ALERT     lv_color_hex(0xFF3B30)
#define C_BRAND     lv_color_hex(0xEE2323)  // EXTREMEECU red

LV_IMG_DECLARE(img_extremeecu_logo);
LV_IMG_DECLARE(img_extremeecu_logo_small);

// Rajdhani (OFL), generated with lv_font_conv
LV_FONT_DECLARE(ui_font_big);       // 88 px bold, digits . + - only
LV_FONT_DECLARE(ui_font_value);     // 34 px bold, digits . + - only
LV_FONT_DECLARE(ui_font_label);     // 16 px semibold ASCII, falls back to Montserrat 14 for symbols
LV_FONT_DECLARE(ui_font_label_lg);  // 22 px bold ASCII, falls back to Montserrat 16 for symbols

#define UI_TILE_W   104
#define UI_TILE_H   58
#define UI_COL_X0   76      // 3 tile columns that stay clear of the ring labels
#define UI_COL_X1   188
#define UI_COL_X2   300

typedef struct {
    lv_obj_t *box;
    lv_obj_t *stripe;       // left accent bar, follows the value color when it is not plain
    lv_obj_t *caption;
    lv_obj_t *value;
    char last[16];
} ui_tile_t;

// ---- common helpers (ui_common.c) ----
lv_obj_t *ui_screen_create(void);
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *txt);
void ui_label_set(lv_obj_t *label, const char *txt);   // only invalidates on change
void ui_tile_create(ui_tile_t *t, lv_obj_t *parent, int x, int y, const char *caption);
void ui_tile_set(ui_tile_t *t, const char *txt, lv_color_t color);
void ui_page_dots(lv_obj_t *scr, int active, int count);
void ui_brand_footer(lv_obj_t *scr, int y);
lv_obj_t *ui_ring_meter(lv_obj_t *scr);
void ui_meter_label_div10_cb(lv_event_t *e);

// ---- AFR standards ----
typedef struct {
    float lambda_max;       // zone upper bound (lambda)
    const char *name;
    uint32_t color;
} ui_afr_zone_t;

extern const ui_afr_zone_t UI_AFR_ZONES[];
extern const int UI_AFR_ZONE_COUNT;

float ui_stoich_afr(void);
const char *ui_fuel_name(void);
const ui_afr_zone_t *ui_afr_zone(float lambda);
bool ui_lambda_valid(const vems_data_t *d);

// ---- screens ----
lv_obj_t *ui_splash_create(void);
bool ui_splash_update(void);                        // true when the splash may close

lv_obj_t *ui_main_create(int page, int pages);
void ui_main_update(const vems_data_t *d, bool fresh);

lv_obj_t *ui_afr_create(int page, int pages);
void ui_afr_update(const vems_data_t *d, bool fresh);
void ui_afr_reset_minmax(void);
