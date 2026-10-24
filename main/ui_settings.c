/*
 * Settings page: scrolling rows, a tap opens an editor (- / + for numbers, a list for choices).
 * TAMAM stores the value in NVS and applies it; layout settings rebuild the pages, the protocol
 * setting restarts the search with the boot screen.
 */
#include <stdio.h>
#include "sdkconfig.h"
#include "board_lcd.h"
#include "ui_priv.h"

typedef enum {
    ID_VIEW, ID_BRIGHT, ID_SHIFT, ID_RPMMAX, ID_FUEL, ID_PRESS, ID_PROTO, ID_CLT, ID_BATT, ID_BEEP, ID_RESCAN, ID_RESET,
    ID_VER, ID_COUNT
} set_id_t;
typedef enum { T_RANGE, T_ENUM, T_ACTION, T_INFO } row_type_t;

typedef struct {
    const char *name;
    row_type_t type;
    int min, max, step;     // T_RANGE
} row_def_t;

static const row_def_t ROWS[ID_COUNT] = {
    [ID_VIEW]   = {"Ana gösterge",   T_ENUM},
    [ID_BRIGHT] = {"Parlaklık",      T_RANGE, 5, 100, 5},
    [ID_SHIFT]  = {"Vites uyarısı",  T_RANGE, 3000, 9500, 100},
    [ID_RPMMAX] = {"Devir ölçeği",   T_ENUM},
    [ID_FUEL]   = {"Yakıt",          T_ENUM},
    [ID_PRESS]  = {"Basınç kartı",   T_ENUM},
    [ID_PROTO]  = {"Protokol",       T_ENUM},
    [ID_CLT]    = {"Hararet uyarısı", T_RANGE, 95, 115, 1},
    [ID_BATT]   = {"Akü alt sınırı", T_RANGE, 105, 125, 1},
    [ID_BEEP]   = {"Açılış sesi",    T_ENUM},
    [ID_RESCAN] = {"Bağlantı",       T_ACTION},
    [ID_RESET]  = {"Varsayılanlar",  T_ACTION},
    [ID_VER]    = {"Sürüm",          T_INFO},
};

static const int RPM_MAX_OPTS[] = {6000, 7000, 8000, 9000, 10000};
#define ENUM_MAX    6

static struct {
    lv_obj_t *scr;
    lv_obj_t *val[ID_COUNT];
    lv_obj_t *ed;           // editor overlay, NULL when closed
    lv_obj_t *ed_hidden;    // closed overlay waiting to be deleted
    lv_obj_t *ed_value;
    lv_obj_t *ed_opt[ENUM_MAX];
    set_id_t id;
    int v;                  // value being edited
} s;

static int get_val(set_id_t id)
{
    switch (id) {
    case ID_VIEW:   return g_settings.main_view;
    case ID_BRIGHT: return g_settings.brightness;
    case ID_SHIFT:  return g_settings.shift_rpm;
    case ID_RPMMAX:
        for (int i = 0; i < 5; i++) {
            if (RPM_MAX_OPTS[i] == g_settings.rpm_max) {
                return i;
            }
        }
        return 2;
    case ID_FUEL:   return g_settings.fuel;
    case ID_PRESS:  return g_settings.pressure;
    case ID_PROTO:  return g_settings.proto;
    case ID_CLT:    return g_settings.clt_warn;
    case ID_BATT:   return g_settings.batt_low;
    case ID_BEEP:   return g_settings.beep;
    default:        return 0;
    }
}

static int enum_count(set_id_t id)
{
    switch (id) {
    case ID_VIEW:   return 2;
    case ID_RPMMAX: return 5;
    case ID_FUEL:   return SETTINGS_FUEL_COUNT < ENUM_MAX ? SETTINGS_FUEL_COUNT : ENUM_MAX;
    case ID_PRESS:  return 2;
    case ID_PROTO:  return 3;
    case ID_BEEP:   return 2;
    default:        return 0;
    }
}

static void val_text(set_id_t id, int v, char *buf, size_t len)
{
    static const char *const PROTO[] = {"Otomatik", "Sadece CAN", "Sadece K-Line"};
    switch (id) {
    case ID_VIEW:   snprintf(buf, len, "%s", v ? "Hız" : "Devir"); break;
    case ID_BRIGHT: snprintf(buf, len, "%d %%", v); break;
    case ID_SHIFT:  snprintf(buf, len, "%d rpm", v); break;
    case ID_RPMMAX: snprintf(buf, len, "%d rpm", RPM_MAX_OPTS[v]); break;
    case ID_FUEL:   snprintf(buf, len, "%s %.1f", SETTINGS_FUELS[v].name, SETTINGS_FUELS[v].stoich); break;
    case ID_PRESS:  snprintf(buf, len, "%s", v ? "Boost bar" : "MAP kPa"); break;
    case ID_PROTO:  snprintf(buf, len, "%s", PROTO[v]); break;
    case ID_CLT:    snprintf(buf, len, "%d °C", v); break;
    case ID_BATT:   snprintf(buf, len, "%d.%d V", v / 10, v % 10); break;
    case ID_BEEP:   snprintf(buf, len, "%s", v ? "Açık" : "Kapalı"); break;
    case ID_RESCAN: snprintf(buf, len, "Yeniden ara"); break;
    case ID_RESET:  snprintf(buf, len, "Sıfırla"); break;
    case ID_VER:    snprintf(buf, len, "v2.0 · ESP32-S3"); break;
    default:        buf[0] = '\0'; break;
    }
}

static void refresh_rows(void)
{
    char buf[32];
    for (int i = 0; i < ID_COUNT; i++) {
        if (s.val[i]) {
            val_text(i, get_val(i), buf, sizeof(buf));
            ui_label_set(s.val[i], buf);
        }
    }
}

/** Writes the edited value, stores it and applies it. */
static void commit(set_id_t id, int v)
{
    bool rebuild = false, rescan = false;
    switch (id) {
    case ID_VIEW:   g_settings.main_view = v; rebuild = true; break;
    case ID_BRIGHT: g_settings.brightness = v; board_lcd_set_backlight(v); break;
    case ID_SHIFT:  g_settings.shift_rpm = v; rebuild = true; break;
    case ID_RPMMAX: g_settings.rpm_max = RPM_MAX_OPTS[v]; rebuild = true; break;
    case ID_FUEL:   g_settings.fuel = v; rebuild = true; break;
    case ID_PRESS:  g_settings.pressure = v; rebuild = true; break;
    case ID_PROTO:  rescan = g_settings.proto != v; g_settings.proto = v; break;
    case ID_CLT:    g_settings.clt_warn = v; break;
    case ID_BATT:   g_settings.batt_low = v; break;
    case ID_BEEP:   g_settings.beep = v; break;
    default:        return;
    }
    settings_save();
    refresh_rows();
    if (rescan) {
        ui_request_rescan();
    } else if (rebuild) {
        ui_request_rebuild();
    }
}

/*
 * Closing runs inside the editor's own button event and may be followed by a page rebuild that
 * deletes this whole screen, so the overlay is only hidden here and deleted on the next open.
 */
static void editor_close(void)
{
    if (s.ed) {
        lv_obj_add_flag(s.ed, LV_OBJ_FLAG_HIDDEN);
        s.ed_hidden = s.ed;
        s.ed = NULL;
    }
}

static void editor_show_value(void)
{
    char buf[32];
    if (ROWS[s.id].type == T_RANGE) {
        val_text(s.id, s.v, buf, sizeof(buf));
        ui_label_set(s.ed_value, buf);
    } else {
        for (int i = 0; i < enum_count(s.id); i++) {
            const bool on = i == s.v;
            lv_obj_set_style_border_color(s.ed_opt[i], on ? C_PRIMARY : C_TILE_BRD, 0);
            lv_obj_set_style_bg_color(s.ed_opt[i], on ? lv_color_hex(0x1A0A0B) : C_TILE, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(s.ed_opt[i], 0), on ? C_TEXT : C_SOFT, 0);
        }
    }
}

static void step_cb(lv_event_t *e)
{
    const int dir = (int)(intptr_t)lv_event_get_user_data(e);
    const row_def_t *r = &ROWS[s.id];
    s.v += dir * r->step;
    s.v = s.v < r->min ? r->min : s.v > r->max ? r->max : s.v;
    if (s.id == ID_BRIGHT) {
        board_lcd_set_backlight(s.v);       // live preview
    }
    editor_show_value();
}

static void opt_cb(lv_event_t *e)
{
    s.v = (int)(intptr_t)lv_event_get_user_data(e);
    editor_show_value();
}

static void ok_cb(lv_event_t *e)
{
    commit(s.id, s.v);
    editor_close();
}

static lv_obj_t *round_btn(lv_obj_t *parent, const char *sym, int x)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 84, 84);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, x, -10);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, C_TILE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x0D2C4A), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, C_TILE_BRD, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *l = ui_label(b, &lv_font_montserrat_16, C_TEXT, sym);
    lv_obj_center(l);
    return b;
}

static void editor_open(set_id_t id)
{
    const row_def_t *r = &ROWS[id];
    s.id = id;
    s.v = get_val(id);
    if (s.ed_hidden) {
        lv_obj_del(s.ed_hidden);
        s.ed_hidden = NULL;
    }

    s.ed = lv_obj_create(s.scr);
    lv_obj_remove_style_all(s.ed);
    lv_obj_set_size(s.ed, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s.ed, C_BG, 0);
    lv_obj_set_style_bg_opa(s.ed, 245, 0);
    lv_obj_clear_flag(s.ed, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);   // swipes don't change page

    lv_obj_t *title = ui_label(s.ed, &ui_font_label_lg, C_TEXT, r->name);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 96);

    int ok_y;
    if (r->type == T_RANGE) {
        s.ed_value = ui_label(s.ed, &ui_font_label_lg, C_PRIMARY, "");
        lv_obj_set_style_text_letter_space(s.ed_value, 2, 0);
        lv_obj_align(s.ed_value, LV_ALIGN_CENTER, 0, -10);
        lv_obj_t *minus = round_btn(s.ed, LV_SYMBOL_MINUS, 44);
        lv_obj_t *plus = round_btn(s.ed, LV_SYMBOL_PLUS, 480 - 44 - 84);
        lv_obj_add_event_cb(minus, step_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)-1);
        lv_obj_add_event_cb(minus, step_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)-1);
        lv_obj_add_event_cb(plus, step_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)1);
        lv_obj_add_event_cb(plus, step_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)1);
        ok_y = 330;
    } else {
        const int n = enum_count(id), h = 40, gap = 8;
        const int y0 = 240 - (n * (h + gap) - gap) / 2;
        char buf[32];
        for (int i = 0; i < n; i++) {
            lv_obj_t *o = lv_obj_create(s.ed);
            lv_obj_remove_style_all(o);
            lv_obj_set_size(o, 260, h);
            lv_obj_align(o, LV_ALIGN_TOP_MID, 0, y0 + i * (h + gap));
            lv_obj_set_style_radius(o, 8, 0);
            lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(o, 1, 0);
            lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
            lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
            val_text(id, i, buf, sizeof(buf));
            lv_obj_t *l = ui_label(o, &ui_font_label_lg, C_SOFT, buf);
            lv_obj_center(l);
            lv_obj_add_event_cb(o, opt_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            s.ed_opt[i] = o;
        }
        ok_y = y0 + n * (h + gap) + 14;
    }
    lv_obj_t *ok = ui_pill_button(s.ed, "TAMAM", 160);
    lv_obj_align(ok, LV_ALIGN_TOP_MID, 0, ok_y);
    lv_obj_clear_flag(ok, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(ok, ok_cb, LV_EVENT_CLICKED, NULL);
    editor_show_value();
}

static void row_cb(lv_event_t *e)
{
    const set_id_t id = (set_id_t)(intptr_t)lv_event_get_user_data(e);
    if (s.ed) {
        return;
    }
    if (id == ID_RESCAN) {
        ui_request_rescan();
    } else if (ROWS[id].type == T_ACTION) {
        settings_reset();
        board_lcd_set_backlight(g_settings.brightness);
        refresh_rows();
        ui_request_rebuild();
    } else if (ROWS[id].type != T_INFO) {
        editor_open(id);
    }
}

lv_obj_t *ui_settings_create(int page, int pages)
{
    s.scr = ui_screen_create();
    s.ed = NULL;
    s.ed_hidden = NULL;
    ui_header(s.scr, "AYARLAR", 66);

    lv_obj_t *list = ui_scroll_list(s.scr);
    for (int i = 0; i < ID_COUNT; i++) {
        s.val[i] = NULL;
        if (i == ID_PROTO) {
            continue;       // K-Line only: no protocol choice
        }
        lv_obj_t *row = ui_list_row(list, ROWS[i].name, &s.val[i]);
        if (ROWS[i].type != T_INFO) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_text_color(s.val[i], C_PRIMARY, 0);
            lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }
    refresh_rows();

    ui_page_dots(s.scr, page, pages);
    return s.scr;
}
