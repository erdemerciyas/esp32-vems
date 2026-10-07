/*
 * Boot splash: logo, spinning ring and the real start-up steps
 * (display -> USB host -> VEMS USB/FTDI -> ECU data) with a progress bar.
 */
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_timer.h"
#include "ui_priv.h"

#define STEP_COUNT      4
#define READY_HOLD_US   (600 * 1000)

static const char *STEP_NAMES[STEP_COUNT] = {
    "EKRAN",
    "USB HOST",
    "VEMS USB (FTDI)",
    "ECU VERISI",
};

static struct {
    lv_obj_t *bar;
    lv_obj_t *msg;
    lv_obj_t *icon[STEP_COUNT];
    lv_obj_t *name[STEP_COUNT];
    int shown_done;
    int64_t t0;
    int64_t ready_since;
} sp;

lv_obj_t *ui_splash_create(void)
{
    lv_obj_t *scr = ui_screen_create();

    lv_obj_t *spin = lv_spinner_create(scr, 1400, 70);
    lv_obj_set_size(spin, 452, 452);
    lv_obj_center(spin);
    lv_obj_set_style_arc_width(spin, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spin, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_clear_flag(spin, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *logo = ui_label(scr, &lv_font_montserrat_48, C_TEXT, "VEMS");
    lv_obj_set_style_text_letter_space(logo, 10, 0);
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 5, 92);

    lv_obj_t *sub = ui_label(scr, &lv_font_montserrat_16, C_ACCENT, "LIVE DASH");
    lv_obj_set_style_text_letter_space(sub, 6, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 3, 150);

    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_set_size(line, 160, 2);
    lv_obj_set_style_bg_color(line, C_TILE_BRD, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 182);

    for (int i = 0; i < STEP_COUNT; i++) {
        int y = 200 + i * 30;
        sp.icon[i] = ui_label(scr, &lv_font_montserrat_16, C_TICK, LV_SYMBOL_MINUS);
        lv_obj_align(sp.icon[i], LV_ALIGN_TOP_LEFT, 140, y);
        sp.name[i] = ui_label(scr, &lv_font_montserrat_16, C_TICK, STEP_NAMES[i]);
        lv_obj_align(sp.name[i], LV_ALIGN_TOP_LEFT, 172, y);
    }

    sp.bar = lv_bar_create(scr);
    lv_obj_set_size(sp.bar, 220, 6);
    lv_obj_align(sp.bar, LV_ALIGN_TOP_MID, 0, 330);
    lv_bar_set_range(sp.bar, 0, 100);
    lv_obj_set_style_bg_color(sp.bar, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sp.bar, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_anim_time(sp.bar, 400, 0);

    sp.msg = ui_label(scr, &lv_font_montserrat_14, C_MUTED, "Baslatiliyor...");
    lv_obj_align(sp.msg, LV_ALIGN_TOP_MID, 0, 348);

    lv_obj_t *ver = ui_label(scr, &lv_font_montserrat_12, C_TICK, "ESP32-S3  |  VEMS v3  |  FW 1.2.x");
    lv_obj_align(ver, LV_ALIGN_TOP_MID, 0, 392);

    sp.shown_done = -1;
    sp.t0 = esp_timer_get_time();
    sp.ready_since = 0;
    return scr;
}

static int steps_done(vems_state_t st)
{
    switch (st) {
    case VEMS_STATE_ECU_OK:    return 4;
    case VEMS_STATE_LINK_UP:   return 3;
    case VEMS_STATE_WAIT_LINK: return 2;
    default:                   return 1;    // display is up if we are drawing
    }
}

bool ui_splash_update(void)
{
    const int64_t now = esp_timer_get_time();
    const int64_t elapsed_ms = (now - sp.t0) / 1000;
    const int done = steps_done(vems_get_state());

    if (done != sp.shown_done) {
        sp.shown_done = done;
        for (int i = 0; i < STEP_COUNT; i++) {
            if (i < done) {
                lv_label_set_text(sp.icon[i], LV_SYMBOL_OK);
                lv_obj_set_style_text_color(sp.icon[i], C_OK, 0);
                lv_obj_set_style_text_color(sp.name[i], C_TEXT, 0);
            } else if (i == done) {
                lv_label_set_text(sp.icon[i], LV_SYMBOL_REFRESH);
                lv_obj_set_style_text_color(sp.icon[i], C_WARN, 0);
                lv_obj_set_style_text_color(sp.name[i], C_WARN, 0);
            } else {
                lv_label_set_text(sp.icon[i], LV_SYMBOL_MINUS);
                lv_obj_set_style_text_color(sp.icon[i], C_TICK, 0);
                lv_obj_set_style_text_color(sp.name[i], C_TICK, 0);
            }
        }
        lv_bar_set_value(sp.bar, done * 100 / STEP_COUNT, LV_ANIM_ON);
        static const char *MSGS[] = {"Baslatiliyor...", "USB host baslatiliyor...", "VEMS USB bekleniyor...",
                                     "ECU sorgulaniyor...", "Hazir"};
        lv_label_set_text(sp.msg, MSGS[done]);
        lv_obj_set_style_text_color(sp.msg, done == STEP_COUNT ? C_OK : C_MUTED, 0);
    }

    if (done == STEP_COUNT) {
        if (sp.ready_since == 0) {
            sp.ready_since = now;
        }
        return elapsed_ms >= CONFIG_VEMS_UI_SPLASH_MIN_MS && (now - sp.ready_since) >= READY_HOLD_US;
    }
    if (elapsed_ms >= CONFIG_VEMS_UI_SPLASH_TIMEOUT_MS) {
        return true;    // dashboard shows "VEMS BEKLENIYOR" itself
    }
    return false;
}
