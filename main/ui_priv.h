#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"
#include "vems_proto.h"
#include "settings.h"

// palette: siyah zemin, nötr mavi vurgu; kırmızı sadece alarm ve devir sınırı
#define C_BG        lv_color_hex(0x000000)
#define C_TRACK     lv_color_hex(0x16181D)
#define C_TILE      lv_color_hex(0x0B0D11)
#define C_TILE_BRD  lv_color_hex(0x22262E)
#define C_LINE      lv_color_hex(0x15181D)
#define C_TEXT      lv_color_hex(0xF4F5F7)
#define C_SOFT      lv_color_hex(0xC9CFDA)
#define C_MUTED     lv_color_hex(0x8A909C)
#define C_TICK      lv_color_hex(0x4A505C)
#define C_ACCENT    lv_color_hex(0x00C8FF)  // "cold" (CLT warm-up, economy zone)
#define C_OK        lv_color_hex(0x2ED47A)
#define C_WARN      lv_color_hex(0xFFB020)
#define C_ALERT     lv_color_hex(0xFF3B30)
#define C_PRIMARY   lv_color_hex(0x2D9CFF)  // vurgu: ibre, seçili öğe, kart şeridi


// Rajdhani (OFL), generated with lv_font_conv; labels include the Turkish letters, ° · … ›
LV_FONT_DECLARE(ui_font_big);       // 88 px bold, digits . + - : only
LV_FONT_DECLARE(ui_font_value);     // 34 px bold, digits . + - : only
LV_FONT_DECLARE(ui_font_label);     // 16 px semibold, falls back to Montserrat 14 for symbols
LV_FONT_DECLARE(ui_font_label_lg);  // 22 px bold, falls back to Montserrat 16 for symbols

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
void ui_tile_set_num(ui_tile_t *t, const char *fmt, float v, lv_color_t color);  // NAN (not supported by the ECU) -> "--"
void ui_page_dots(lv_obj_t *scr, int active, int count);
lv_obj_t *ui_ring_meter(lv_obj_t *scr);
void ui_meter_label_div10_cb(lv_event_t *e);
lv_obj_t *ui_header(lv_obj_t *scr, const char *txt, int y);            // small spaced page title
lv_obj_t *ui_scroll_list(lv_obj_t *scr);                                // round-safe vertical list area
lv_obj_t *ui_list_row(lv_obj_t *list, const char *name, lv_obj_t **value);
lv_obj_t *ui_pill_button(lv_obj_t *parent, const char *txt, int w);

// ---- AFR standards ----
typedef struct {
    float lambda_max;       // zone upper bound (lambda)
    const char *name;
    uint32_t color;
} ui_afr_zone_t;

extern const ui_afr_zone_t UI_AFR_ZONES[];
extern const int UI_AFR_ZONE_COUNT;

float ui_stoich_afr(void);
const char *ui_fuel_name(void);         // upper case, Turkish
const ui_afr_zone_t *ui_afr_zone(float lambda);
bool ui_lambda_valid(const vems_data_t *d);

// ---- channels (ui_channels.c): one table for every source ----
typedef struct {
    const char *name;       // list / single gauge, e.g. "Hararet"
    const char *caption;    // tiles, upper case, e.g. "HARARET"
    const char *unit;
    uint8_t dec;
    float min;              // single gauge ring range
    float max;
} ui_ch_info_t;

extern const ui_ch_info_t UI_CH[CH_COUNT];

/** Value of a channel from the latest frame, NAN when the frame has none (yet). */
float ui_ch_value(const vems_data_t *d, vems_ch_t c);
void ui_ch_format(vems_ch_t c, float v, char *buf, size_t len);
lv_color_t ui_ch_color(vems_ch_t c, float v);
/** Channels of a mask in display order; mask 0 (nothing connected yet) gives a typical set. */
int ui_ch_list(uint32_t mask, vems_ch_t *out, int max);
uint32_t ui_ch_layout_mask(uint32_t mask);
/** Session min / max per channel (single gauge page). */
void ui_ch_track(const vems_data_t *d, uint32_t mask);
bool ui_ch_minmax(vems_ch_t c, float *lo, float *hi);
void ui_ch_reset(vems_ch_t c);

// ---- services of the screen manager (dash_ui.c), safe to call from widget events ----
void ui_request_rebuild(void);          // a setting changed the page layout
void ui_request_rescan(void);           // drop the connection, show the boot screen again
void ui_request_dtc_page(void);         // arıza kodları sayfasına geç (uyarı şeridi)

// ---- uyarılar (ui_alert.c): her sayfanın üstünde şerit / kart ve buzzer ----
void ui_alert_init(void);
void ui_alert_update(const vems_data_t *d, bool fresh, uint32_t mask);

// ---- screens ----
lv_obj_t *ui_splash_create(void);
bool ui_splash_update(void);                        // true when the splash may close

lv_obj_t *ui_main_create(int page, int pages, uint32_t mask);
void ui_main_update(const vems_data_t *d, bool fresh);
uint32_t ui_main_channels(void);                    // sayfadaki kanallar: OBD bunları hızlı sorgular
bool ui_main_toggle_view(void);                     // büyük değer devir <-> hız, hız yoksa false

lv_obj_t *ui_focus_create(int page, int pages, uint32_t mask);
void ui_focus_update(const vems_data_t *d, bool fresh);
void ui_focus_step(int dir);
void ui_focus_reset(void);
uint32_t ui_focus_channels(void);

lv_obj_t *ui_list_create(int page, int pages, uint32_t mask);
void ui_list_update(const vems_data_t *d, bool fresh);

lv_obj_t *ui_afr_create(int page, int pages);
void ui_afr_update(const vems_data_t *d, bool fresh);
void ui_afr_reset_minmax(void);

lv_obj_t *ui_dtc_create(int page, int pages);
void ui_dtc_update(const vems_data_t *d, bool fresh);

lv_obj_t *ui_settings_create(int page, int pages);

