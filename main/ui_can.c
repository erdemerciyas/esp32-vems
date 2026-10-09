/*
 * CAN output screen: on / off switch (long press anywhere on the page toggles, so a stray tap
 * can't cut the cluster / power steering feed), link state and frame rates.
 */
#include <stdio.h>
#include "esp_timer.h"
#include "can_out.h"
#include "ui_priv.h"

#define RATE_US     (1000 * 1000)

enum { T_TX, T_DROP, T_RX, T_COUNT };

static struct {
    lv_obj_t *pill;
    lv_obj_t *pill_text;
    lv_obj_t *state;
    ui_tile_t tiles[T_COUNT];
    int on;                     // -1 until the first update
    int64_t rate_t0;
    can_out_status_t st0;
} c = {.on = -1};

lv_obj_t *ui_can_create(int page, int pages)
{
    char buf[64];
    lv_obj_t *scr = ui_screen_create();

    snprintf(buf, sizeof(buf), "CAN CIKISI  |  %s", can_out_profile_name());
    lv_obj_t *hdr = ui_label(scr, &ui_font_label, C_MUTED, buf);
    lv_obj_set_style_text_letter_space(hdr, 2, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 84);

    c.pill = lv_obj_create(scr);
    lv_obj_remove_style_all(c.pill);
    lv_obj_clear_flag(c.pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);    // presses reach the page
    lv_obj_set_size(c.pill, 200, 64);
    lv_obj_align(c.pill, LV_ALIGN_TOP_MID, 0, 116);
    lv_obj_set_style_radius(c.pill, 32, 0);
    lv_obj_set_style_bg_color(c.pill, C_TILE, 0);
    lv_obj_set_style_bg_opa(c.pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c.pill, 3, 0);
    lv_obj_set_style_border_color(c.pill, C_TILE_BRD, 0);

    c.pill_text = ui_label(c.pill, &ui_font_label_lg, C_MUTED, "");
    lv_obj_set_style_text_letter_space(c.pill_text, 4, 0);
    lv_obj_center(c.pill_text);

    c.state = ui_label(scr, &ui_font_label_lg, C_MUTED, "");
    lv_obj_set_style_text_letter_space(c.state, 2, 0);
    lv_obj_align(c.state, LV_ALIGN_TOP_MID, 0, 196);

    ui_tile_create(&c.tiles[T_TX],   scr, UI_COL_X0, 236, "TX /s");
    ui_tile_create(&c.tiles[T_DROP], scr, UI_COL_X1, 236, "HATA /s");
    ui_tile_create(&c.tiles[T_RX],   scr, UI_COL_X2, 236, "RX /s");

    lv_obj_t *hint = ui_label(scr, &ui_font_label, C_TEXT, "ACMAK / KAPATMAK ICIN BASILI TUTUN");
    lv_obj_set_style_text_letter_space(hint, 1, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 318);

    lv_obj_t *note = ui_label(scr, &ui_font_label, C_MUTED,
                              "KAPALIYKEN ARAC GOSTERGESI VE\nDIREKSIYON DESTEGI DEVIR ALAMAZ");
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 352);

    ui_page_dots(scr, page, pages);
    return scr;
}

void ui_can_toggle(void)
{
    can_out_set_enabled(!can_out_get_enabled());
}

static void set_rate(ui_tile_t *t, uint32_t now, uint32_t before, lv_color_t nonzero, lv_color_t zero)
{
    char buf[16];
    uint32_t n = now - before;
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)n);
    ui_tile_set(t, buf, n ? nonzero : zero);
}

void ui_can_update(void)
{
    can_out_status_t st;
    can_out_get_status(&st);

    int on = can_out_get_enabled();
    if (on != c.on) {
        c.on = on;
        lv_color_t col = on ? C_OK : C_MUTED;
        ui_label_set(c.pill_text, on ? "ACIK" : "KAPALI");
        lv_obj_set_style_text_color(c.pill_text, col, 0);
        lv_obj_set_style_border_color(c.pill, on ? C_OK : C_TILE_BRD, 0);
    }

    static const struct {
        const char *text;
        uint32_t color;
    } STATES[] = {
        [CAN_OUT_OFF]       = {"VERI GONDERILMIYOR", 0x8A909C},
        [CAN_OUT_NO_MODULE] = {"MODUL YOK", 0xFF3B30},
        [CAN_OUT_NO_ACK]    = {"HAT CEVAP VERMIYOR", 0xFFB020},
        [CAN_OUT_ACTIVE]    = {"VERI GONDERILIYOR", 0x2ED47A},
    };
    ui_label_set(c.state, STATES[st.state].text);
    lv_obj_set_style_text_color(c.state, lv_color_hex(STATES[st.state].color), 0);

    int64_t now = esp_timer_get_time();
    if (now - c.rate_t0 < RATE_US) {
        return;
    }
    if (st.state == CAN_OUT_OFF || st.state == CAN_OUT_NO_MODULE) {
        for (int i = 0; i < T_COUNT; i++) {
            ui_tile_set(&c.tiles[i], "--", C_MUTED);
        }
    } else if (c.rate_t0) {
        set_rate(&c.tiles[T_TX], st.tx_ok, c.st0.tx_ok, C_TEXT, C_MUTED);
        set_rate(&c.tiles[T_DROP], st.tx_drop, c.st0.tx_drop, C_WARN, C_TEXT);
        set_rate(&c.tiles[T_RX], st.rx, c.st0.rx, C_TEXT, C_MUTED);
    }
    c.rate_t0 = now;
    c.st0 = st;
}
