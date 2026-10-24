/*
 * Connection page: protocol, ECU, line, number of values, query rate, error counters and a
 * "search again" button (shows the boot screen with the detection steps again).
 */
#include <stdio.h>
#include "esp_timer.h"
#include "ui_priv.h"

#define ROWS    6

static struct {
    lv_obj_t *proto;
    lv_obj_t *standard;
    lv_obj_t *key[ROWS];
    lv_obj_t *val[ROWS];
    lv_obj_t *ro;
    int channels;
    int64_t t0;
    uint32_t ok0;
    int rate;
} k;

static void rescan_cb(lv_event_t *e)
{
    ui_request_rescan();
}

lv_obj_t *ui_link_create(int page, int pages, uint32_t mask)
{
    lv_obj_t *scr = ui_screen_create();
    k.channels = __builtin_popcount(mask);

    ui_header(scr, "BAĞLANTI", 66);
    k.proto = ui_label(scr, &ui_font_label_lg, C_TEXT, "");
    lv_obj_set_style_text_letter_space(k.proto, 4, 0);
    lv_obj_align(k.proto, LV_ALIGN_TOP_MID, 0, 94);
    k.standard = ui_label(scr, &ui_font_label, C_MUTED, "");
    lv_obj_align(k.standard, LV_ALIGN_TOP_MID, 0, 124);

    for (int i = 0; i < ROWS; i++) {
        const int y = 156 + i * 28;
        k.key[i] = ui_label(scr, &ui_font_label, C_MUTED, "");
        lv_obj_set_style_text_letter_space(k.key[i], 1, 0);
        lv_obj_align(k.key[i], LV_ALIGN_TOP_LEFT, 100, y + 3);
        k.val[i] = ui_label(scr, &ui_font_label_lg, C_TEXT, "");
        lv_obj_align(k.val[i], LV_ALIGN_TOP_RIGHT, -100, y);
    }

    k.ro = ui_label(scr, &ui_font_label, C_OK, "");
    lv_obj_set_style_text_letter_space(k.ro, 2, 0);
    lv_obj_align(k.ro, LV_ALIGN_TOP_MID, 0, 330);

    lv_obj_t *btn = ui_pill_button(scr, "YENİDEN ARA", 210);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 358);
    lv_obj_add_event_cb(btn, rescan_cb, LV_EVENT_CLICKED, NULL);

    ui_page_dots(scr, page, pages);
    k.t0 = 0;
    k.rate = 0;
    return scr;
}

static void row(int i, const char *key, const char *val)
{
    ui_label_set(k.key[i], key);
    ui_label_set(k.val[i], val);
}

void ui_link_update(bool fresh)
{
    char buf[32];
    vems_link_info_t li;
    vems_stats_t st;
    vems_get_link_info(&li);
    vems_get_stats(&st);

    int64_t now = esp_timer_get_time();
    if (k.t0 == 0 || now - k.t0 >= 1000000) {
        k.rate = k.t0 ? (int)((st.frames_ok - k.ok0) * 1e6f / (float)(now - k.t0) + 0.5f) : 0;
        k.t0 = now;
        k.ok0 = st.frames_ok;
    }

    const bool on = vems_get_channels() != 0;
    ui_label_set(k.proto, li.protocol);
    lv_obj_set_style_text_color(k.proto, on ? C_TEXT : C_WARN, 0);
    ui_label_set(k.standard, li.standard ? li.standard : "");

    snprintf(buf, sizeof(buf), "%d", on ? __builtin_popcount(vems_get_channels()) : k.channels);
    if (li.obd) {
        row(0, "ECU", li.ecu);
        row(1, "HAT HIZI", li.line);
        row(2, "DESTEKLENEN", on ? buf : "--");
        snprintf(buf, sizeof(buf), "%d / sn", fresh ? k.rate : 0);
        row(3, "SORGU", buf);
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)st.timeouts);
        row(4, "ZAMAN AŞIMI", buf);
        ui_label_set(k.ro, "CANLI VERİ 01 · ARIZA KODU 03/04");
    } else {
        row(0, "KAYNAK", li.ecu);
        row(1, "BAĞLANTI", li.line);
        row(2, "VERİ", on ? buf : "--");
        snprintf(buf, sizeof(buf), "%d / sn", fresh ? k.rate : 0);
        row(3, "ÇERÇEVE", buf);
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)st.crc_errors);
        row(4, "CRC HATASI", buf);
        ui_label_set(k.ro, "SADECE OKUMA");
    }
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)st.resyncs);
    row(5, "YENİDEN BAĞ.", buf);
}
