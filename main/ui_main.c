/*
 * Sürüş sayfası. Dış halka yarış aracı LED şeridi gibi segment segment yanar: devirde mavi -> sarı -> kırmızı
 * bölge (vites noktasında segmentler ve çerçeve flaşlar), hızda mavi. Büyük gösterilen değer (devir ya da hız)
 * sayfaya dokunarak ya da ayarlardan seçilir, diğeri üstte küçük durur. Tepe değeri beyaz segmentte kısa süre
 * kalır. Altta kaynağın gerçekten verdiği üç kanal (şarj, tüketim, yağ, emme ...), alt boşlukta hararet yayı.
 * Üst satır sadece söylenecek bir şey varsa yazar: bağlantı yok, arıza kodu, motor soğuk.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_timer.h"
#include "board_lcd.h"
#include "ui_priv.h"

#define POD_MAX         3
#define POD_DX          104         // kanal sütunları arası
#define SPEED_MAX       200
#define SEG_LEN         26
#define PEAK_HOLD_US    (1200 * 1000)
#define CLT_LO          40          // hararet yayı aralığı
#define CLT_HI          130
#define CLT_COLD        60          // altı "motor soğuk"

#define C_SEG_OFF       lv_color_hex(0x181B22)
#define C_SEG_RED       lv_color_hex(0x3A1416)  // yanmayan kırmızı bölge
#define C_SEG_LO        lv_color_hex(0x1E5BFF)
#define C_SEG_HI        lv_color_hex(0x35E0FF)

enum { LIT_N, LIT_W, LIT_R, LIT_COUNT };

static struct {
    lv_obj_t *meter;
    lv_meter_indicator_t *red_off;
    lv_meter_indicator_t *lit[LIT_COUNT];
    lv_meter_indicator_t *peak;
    int max, step, warn, red;   // ölçek biriminde: devirde 100 rpm, hızda km/h
    int peak_v;
    int64_t peak_t;
    lv_obj_t *shift;            // vites noktasında yanıp sönen çerçeve
    lv_obj_t *status;
    lv_obj_t *sec_val;
    lv_obj_t *sec_unit;
    lv_obj_t *val;
    lv_obj_t *unit;
    lv_obj_t *pod_cap[POD_MAX];
    lv_obj_t *pod_val[POD_MAX];
    vems_ch_t pod_ch[POD_MAX];
    int npods;
    lv_obj_t *clt_arc;
    lv_obj_t *clt_val;
    lv_color_t clt_color;
    bool speed_view;
    bool has_speed;
    bool has_clt;
    bool has_lh;
} m;

static void pick_channels(uint32_t mask)
{
    mask = ui_ch_layout_mask(mask);
    const vems_ch_t pressure = g_settings.pressure == SET_PRESS_BOOST ? CH_BOOST : CH_MAP;
    // sürücüyü ilgilendirenler önce; basınç, karışım ve diğerleri yer kalırsa
    const vems_ch_t prio[] = {CH_BATT, CH_FUEL_L100, CH_FUEL_LH, CH_OIL, CH_FUEL, CH_IAT, pressure, CH_AFR, CH_TPS,
                              CH_LOAD, CH_ADV};
    m.npods = 0;
    for (size_t i = 0; i < sizeof(prio) / sizeof(prio[0]) && m.npods < POD_MAX; i++) {
        const vems_ch_t c = prio[i];
        if (!(mask & CH_BIT(c)) || (c == CH_FUEL_LH && (mask & CH_BIT(CH_FUEL_L100)))) {
            continue;
        }
        m.pod_ch[m.npods++] = c;
    }
    m.has_speed = mask & CH_BIT(CH_SPEED);
    m.has_clt = mask & CH_BIT(CH_CLT);
    m.has_lh = mask & CH_BIT(CH_FUEL_LH);
}

uint32_t ui_main_channels(void)
{
    uint32_t want = CH_BIT(CH_RPM) | (m.has_speed ? CH_BIT(CH_SPEED) : 0) | (m.has_clt ? CH_BIT(CH_CLT) : 0);
    for (int i = 0; i < m.npods; i++) {
        want |= CH_BIT(m.pod_ch[i]);
    }
    return want;
}

bool ui_main_toggle_view(void)
{
    if (!m.has_speed) {
        return false;
    }
    g_settings.main_view = m.speed_view ? SET_VIEW_RPM : SET_VIEW_SPEED;
    settings_save();
    ui_request_rebuild();
    return true;
}

/** Ortalanmış "değer birim" satırı, yazı değişince kendiliğinden ortada kalır. */
static void value_row(lv_obj_t *scr, int y, const lv_font_t *font, lv_obj_t **val, lv_obj_t **unit, const char *u)
{
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    *val = ui_label(row, font, C_MUTED, "--");
    *unit = ui_label(row, &ui_font_label, C_MUTED, u);
    lv_obj_set_style_text_letter_space(*unit, 2, 0);
}

static const char *pod_caption(vems_ch_t c, char *buf, size_t len)
{
    const ui_ch_info_t *ci = &UI_CH[c];
    if (ci->unit[0] && c != CH_BOOST && c != CH_FUEL_L100) {
        snprintf(buf, len, "%s %s", ci->caption, ci->unit);
    } else {
        snprintf(buf, len, "%s", ci->caption);
    }
    return buf;
}

lv_obj_t *ui_main_create(int page, int pages, uint32_t mask)
{
    lv_obj_t *scr = ui_screen_create();
    pick_channels(mask);
    m.speed_view = m.has_speed && g_settings.main_view == SET_VIEW_SPEED;

    if (m.speed_view) {
        m.max = SPEED_MAX;
        m.step = 5;
        m.warn = m.red = SPEED_MAX + 1;     // hızda renk bölgesi yok
    } else {
        m.max = g_settings.rpm_max / 100;
        m.step = 2;
        m.red = g_settings.shift_rpm / 100;
        m.warn = m.red - 10;
    }
    const int n = m.max / m.step + 1;
    // segment genişliği aralığın ~%55'i (270° yay, ortalama yarıçap ~215 px)
    const int seg_w = (int)(2 * 3.14159f * 215 * 0.75f / (n - 1) * 0.55f);

    m.meter = ui_ring_meter(scr);
    lv_obj_set_style_pad_all(m.meter, 12, 0);
    lv_obj_set_style_text_font(m.meter, &ui_font_label, 0);
    if (m.speed_view) {
        lv_obj_remove_event_cb(m.meter, ui_meter_label_div10_cb);   // etiketler km/h
    }
    lv_meter_scale_t *s = lv_meter_add_scale(m.meter);
    lv_meter_set_scale_range(m.meter, s, 0, m.max, 270, 135);
    lv_meter_set_scale_ticks(m.meter, s, n, seg_w, SEG_LEN, C_SEG_OFF);
    // etiket: devirde her 1000 rpm, hızda her 20 km/h
    lv_meter_set_scale_major_ticks(m.meter, s, m.speed_view ? 4 : 5, seg_w, SEG_LEN, C_SEG_OFF, 12);

    // aralıklar hiç çakışmaz; çakışsa son eklenen kazanır (tepe segmenti)
    m.red_off = lv_meter_add_scale_lines(m.meter, s, C_SEG_RED, C_SEG_RED, false, 0);
    m.lit[LIT_N] = lv_meter_add_scale_lines(m.meter, s, C_SEG_LO, C_SEG_HI, false, 0);
    m.lit[LIT_W] = lv_meter_add_scale_lines(m.meter, s, C_WARN, C_WARN, false, 0);
    m.lit[LIT_R] = lv_meter_add_scale_lines(m.meter, s, C_ALERT, C_ALERT, false, 0);
    m.peak = lv_meter_add_scale_lines(m.meter, s, C_TEXT, C_TEXT, false, 0);
    m.peak_v = -1;

    m.shift = lv_obj_create(scr);
    lv_obj_remove_style_all(m.shift);
    lv_obj_set_size(m.shift, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_center(m.shift);
    lv_obj_set_style_radius(m.shift, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(m.shift, C_ALERT, 0);
    lv_obj_set_style_border_width(m.shift, 8, 0);
    lv_obj_set_style_border_opa(m.shift, LV_OPA_COVER, 0);
    lv_obj_clear_flag(m.shift, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(m.shift, LV_OBJ_FLAG_HIDDEN);

    m.status = ui_label(scr, &ui_font_label, C_WARN, "");
    lv_obj_set_style_text_letter_space(m.status, 2, 0);
    lv_obj_align(m.status, LV_ALIGN_TOP_MID, 0, 80);

    if (m.has_speed) {
        value_row(scr, 106, &ui_font_value, &m.sec_val, &m.sec_unit, m.speed_view ? "RPM" : "KM/H");
    }

    m.val = ui_label(scr, &ui_font_big, C_MUTED, "---");
    lv_obj_align(m.val, LV_ALIGN_TOP_MID, 0, 140);
    m.unit = ui_label(scr, &ui_font_label_lg, C_MUTED, m.speed_view ? "KM/H" : "RPM");
    lv_obj_set_style_text_letter_space(m.unit, 5, 0);
    lv_obj_align(m.unit, LV_ALIGN_TOP_MID, 0, 212);

    char cap[24];
    const int x0 = -(m.npods - 1) * POD_DX / 2;
    for (int i = 0; i < m.npods; i++) {
        const int x = x0 + i * POD_DX;
        m.pod_cap[i] = ui_label(scr, &ui_font_label, C_MUTED, pod_caption(m.pod_ch[i], cap, sizeof(cap)));
        lv_obj_set_style_text_letter_space(m.pod_cap[i], 1, 0);
        lv_obj_align(m.pod_cap[i], LV_ALIGN_TOP_MID, x, 262);
        m.pod_val[i] = ui_label(scr, &ui_font_value, C_MUTED, "--");
        lv_obj_align(m.pod_val[i], LV_ALIGN_TOP_MID, x, 282);
        if (i) {
            lv_obj_t *div = lv_obj_create(scr);
            lv_obj_remove_style_all(div);
            lv_obj_set_size(div, 1, 40);
            lv_obj_set_style_bg_color(div, C_TILE_BRD, 0);
            lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
            lv_obj_align(div, LV_ALIGN_TOP_MID, x - POD_DX / 2, 266);
        }
    }

    if (m.has_clt) {
        // alt boşlukta soldan sağa dolan yay
        m.clt_arc = lv_arc_create(scr);
        lv_obj_set_size(m.clt_arc, 408, 408);
        lv_obj_center(m.clt_arc);
        lv_obj_set_style_pad_all(m.clt_arc, 0, 0);
        lv_arc_set_mode(m.clt_arc, LV_ARC_MODE_REVERSE);
        lv_arc_set_bg_angles(m.clt_arc, 62, 118);
        lv_arc_set_range(m.clt_arc, CLT_LO, CLT_HI);
        lv_arc_set_value(m.clt_arc, CLT_LO);
        lv_obj_remove_style(m.clt_arc, NULL, LV_PART_KNOB);
        lv_obj_clear_flag(m.clt_arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(m.clt_arc, 10, LV_PART_MAIN);
        lv_obj_set_style_arc_color(m.clt_arc, C_TRACK, LV_PART_MAIN);
        lv_obj_set_style_arc_width(m.clt_arc, 10, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(m.clt_arc, C_OK, LV_PART_INDICATOR);
        m.clt_color = C_OK;

        lv_obj_t *unit;
        value_row(scr, 336, &ui_font_value, &m.clt_val, &unit, "°C");
        lv_obj_t *cap_clt = ui_label(scr, &ui_font_label, C_MUTED, "HARARET");
        lv_obj_set_style_text_letter_space(cap_clt, 3, 0);
        lv_obj_align(cap_clt, LV_ALIGN_TOP_MID, 0, 368);
    }

    ui_page_dots(scr, page, pages);
    return scr;
}

static void ind_set(lv_meter_indicator_t *ind, int a, int b)
{
    if (b < a) {
        a = -2;         // boş aralık: hiçbir segment
        b = -1;
    }
    if (ind->start_value != a) {
        lv_meter_set_indicator_start_value(m.meter, ind, a);
    }
    if (ind->end_value != b) {
        lv_meter_set_indicator_end_value(m.meter, ind, b);
    }
}

/** v: ölçek biriminde değer, -1 = veri yok. */
static void set_bar(int v, bool blink)
{
    v = v > m.max ? m.max : v;
    const int64_t now = esp_timer_get_time();
    if (v >= m.peak_v) {
        m.peak_v = v;
        m.peak_t = now;
    } else if (now - m.peak_t > PEAK_HOLD_US) {
        m.peak_v = m.peak_v - m.step > v ? m.peak_v - m.step : v;      // tutma bitince segment segment iner
    }
    ind_set(m.lit[LIT_N], 0, v < m.warn - 1 ? v : m.warn - 1);
    ind_set(m.lit[LIT_W], m.warn, v < m.red - 1 ? v : m.red - 1);
    ind_set(m.lit[LIT_R], m.red, v);
    ind_set(m.red_off, v + 1 > m.red ? v + 1 : m.red, m.max);
    const int pk = m.peak_v / m.step * m.step;
    const bool pk_on = v >= 0 && pk > v;
    ind_set(m.peak, pk_on ? pk : 0, pk_on ? pk : -1);

    const lv_color_t rc = blink ? C_TEXT : C_ALERT;
    if (rc.full != m.lit[LIT_R]->type_data.scale_lines.color_start.full) {
        m.lit[LIT_R]->type_data.scale_lines.color_start = rc;
        m.lit[LIT_R]->type_data.scale_lines.color_end = rc;
        lv_obj_invalidate(m.meter);
    }
}

static void set_shift_light(bool on)
{
    if (on) {
        lv_obj_clear_flag(m.shift, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(m.shift, LV_OBJ_FLAG_HIDDEN);
    }
}

static void status_update(const vems_data_t *d, bool fresh)
{
    char buf[40];
    const char *txt = "";
    lv_color_t col = C_MUTED;
    if (!fresh) {
        const vems_state_t st = vems_get_state();
        const bool lost = d->seq && st != VEMS_STATE_WAIT_LINK;
        txt = lost ? LV_SYMBOL_WARNING "  VERİ YOK"
              : st == VEMS_STATE_ECU_OK || st == VEMS_STATE_LINK_UP ? "ECU SORGULANIYOR" : "OBD ARANIYOR";
        col = lost ? C_ALERT : C_WARN;
    } else if (d->mil_on || d->mil_codes > 0) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING "  ARIZA · %d KOD", d->mil_codes > 0 ? d->mil_codes : 1);
        txt = buf;
        col = d->mil_on ? C_ALERT : C_WARN;
    } else if (m.has_clt && d->clt_c < CLT_COLD) {
        txt = "MOTOR SOĞUK";
        col = C_ACCENT;
    }
    ui_label_set(m.status, txt);
    lv_obj_set_style_text_color(m.status, col, 0);
}

void ui_main_update(const vems_data_t *d, bool fresh)
{
    char buf[24];
    status_update(d, fresh);

    const float rpm = fresh ? d->rpm : NAN;
    const float spd = fresh && m.has_speed ? d->speed_kmh : NAN;
    const float v = m.speed_view ? spd : rpm;
    // vites uyarısı: segmentler, sayı ve çerçeve 5 Hz flaş (saatten türetilir, her güncellemede tutarlı)
    const bool shift = !isnan(rpm) && rpm >= g_settings.shift_rpm;
    const bool blink = shift && ((esp_timer_get_time() / 100000) & 1);

    ui_ch_format(m.speed_view ? CH_SPEED : CH_RPM, v, buf, sizeof(buf));
    ui_label_set(m.val, isnan(v) ? "---" : buf);
    lv_obj_set_style_text_color(m.val, isnan(v) ? C_MUTED : (blink && !m.speed_view) ? C_ALERT : C_TEXT, 0);
    set_bar(isnan(v) ? -1 : (int)(m.speed_view ? v : v / 100), blink);
    set_shift_light(blink);
    if (isnan(v)) {
        m.peak_v = -1;
    }

    if (m.has_speed) {
        const float s = m.speed_view ? rpm : spd;
        ui_ch_format(m.speed_view ? CH_RPM : CH_SPEED, s, buf, sizeof(buf));
        ui_label_set(m.sec_val, buf);
        const lv_color_t sc = isnan(s) ? C_MUTED : (m.speed_view && shift) ? C_ALERT : C_SOFT;
        lv_obj_set_style_text_color(m.sec_val, sc, 0);
    }

    for (int i = 0; i < m.npods; i++) {
        vems_ch_t c = m.pod_ch[i];
        float x = fresh ? ui_ch_value(d, c) : NAN;
        if (c == CH_FUEL_L100) {
            // dururken L/100 km anlamsız: anlık L/saat göster
            const bool lh = fresh && isnan(x) && m.has_lh;
            ui_label_set(m.pod_cap[i], lh ? "L/SAAT" : UI_CH[CH_FUEL_L100].caption);
            if (lh) {
                c = CH_FUEL_LH;
                x = ui_ch_value(d, c);
            }
        }
        ui_ch_format(c, x, buf, sizeof(buf));
        ui_label_set(m.pod_val[i], buf);
        lv_obj_set_style_text_color(m.pod_val[i], ui_ch_color(c, x), 0);
    }

    if (m.has_clt) {
        const float t = fresh ? d->clt_c : NAN;
        ui_ch_format(CH_CLT, t, buf, sizeof(buf));
        ui_label_set(m.clt_val, buf);
        const lv_color_t col = ui_ch_color(CH_CLT, t);
        lv_obj_set_style_text_color(m.clt_val, col, 0);
        const lv_color_t ac = col.full == C_TEXT.full ? C_OK : col;
        if (ac.full != m.clt_color.full) {
            m.clt_color = ac;
            lv_obj_set_style_arc_color(m.clt_arc, ac, LV_PART_INDICATOR);
        }
        const int16_t a = isnan(t) ? CLT_LO : (int16_t)t;
        if (lv_arc_get_value(m.clt_arc) != a) {
            lv_arc_set_value(m.clt_arc, a);
        }
    }
}
