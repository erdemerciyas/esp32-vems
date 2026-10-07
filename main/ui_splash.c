/*
 * Boot splash: animated EXTREMEECU logo intro, spinning ring and the real start-up steps
 * (display -> USB host -> VEMS USB/FTDI -> ECU data) with a progress bar.
 *
 * Intro timeline (ms):  0 logo zooms in with overshoot + fades in, ring fades in
 *                     600 red underline grows
 *                     800 "EXTREMEECU" fades in while its letter spacing closes
 *                    1200 start-up steps fade in, underline starts pulsing
 */
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_timer.h"
#include "ui_priv.h"

#define STEP_COUNT      4
#define READY_HOLD_US   (600 * 1000)

#define LOGO_Y          86
#define LINE_W          200
#define T_LOGO          900
#define T_LINE_DELAY    600
#define T_NAME_DELAY    800
#define T_STEPS_DELAY   1200

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

// ---------------------------------------------------------------------------
// animation helpers
// ---------------------------------------------------------------------------
static void set_opa(lv_obj_t *obj, lv_opa_t v)
{
    lv_obj_set_style_opa(obj, v, LV_PART_MAIN);
    lv_obj_set_style_opa(obj, v, LV_PART_INDICATOR);
}

static void anim_opa_cb(void *obj, int32_t v)
{
    set_opa(obj, v);
}

static void anim_group_opa_cb(void *obj, int32_t v)
{
    uint32_t n = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < n; i++) {
        set_opa(lv_obj_get_child(obj, i), v);
    }
}

static void anim_zoom_cb(void *obj, int32_t v)
{
    lv_img_set_zoom(obj, v);
}

static void anim_img_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_img_opa(obj, v, 0);
}

static void anim_width_cb(void *obj, int32_t v)
{
    lv_obj_set_width(obj, v);
}

static void anim_letter_space_cb(void *obj, int32_t v)
{
    lv_obj_set_style_text_letter_space(obj, v, 0);
}

static void anim_bg_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa(obj, v, 0);
}

static void run_anim(void *obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, uint32_t time, uint32_t delay,
                     lv_anim_path_cb_t path)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, time);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, path);
    lv_anim_start(&a);
}

// ---------------------------------------------------------------------------
lv_obj_t *ui_splash_create(void)
{
    lv_obj_t *scr = ui_screen_create();

    lv_obj_t *spin = lv_spinner_create(scr, 1400, 70);
    lv_obj_set_size(spin, 452, 452);
    lv_obj_center(spin);
    lv_obj_set_style_arc_width(spin, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spin, C_BRAND, LV_PART_INDICATOR);
    lv_obj_clear_flag(spin, LV_OBJ_FLAG_CLICKABLE);
    set_opa(spin, LV_OPA_TRANSP);
    run_anim(spin, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, 600, 0, lv_anim_path_ease_out);

    // logo: zoom 25% -> 100% with overshoot, fade in
    lv_obj_t *logo = lv_img_create(scr);
    lv_img_set_src(logo, &img_extremeecu_logo);
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, LOGO_Y);
    lv_img_set_zoom(logo, LV_IMG_ZOOM_NONE / 4);
    lv_obj_set_style_img_opa(logo, LV_OPA_TRANSP, 0);
    run_anim(logo, anim_zoom_cb, LV_IMG_ZOOM_NONE / 4, LV_IMG_ZOOM_NONE, T_LOGO, 0, lv_anim_path_overshoot);
    run_anim(logo, anim_img_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, T_LOGO / 2, 0, lv_anim_path_ease_out);

    // red underline grows from the centre, then keeps pulsing while we wait
    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_set_size(line, 0, 3);
    lv_obj_set_style_bg_color(line, C_BRAND, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 2, 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, LOGO_Y + img_extremeecu_logo.header.h + 12);
    run_anim(line, anim_width_cb, 0, LINE_W, 400, T_LINE_DELAY, lv_anim_path_ease_out);

    lv_anim_t pulse;
    lv_anim_init(&pulse);
    lv_anim_set_var(&pulse, line);
    lv_anim_set_exec_cb(&pulse, anim_bg_opa_cb);
    lv_anim_set_values(&pulse, LV_OPA_COVER, LV_OPA_30);
    lv_anim_set_time(&pulse, 700);
    lv_anim_set_playback_time(&pulse, 700);
    lv_anim_set_delay(&pulse, T_STEPS_DELAY);
    lv_anim_set_repeat_count(&pulse, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&pulse, lv_anim_path_ease_in_out);
    lv_anim_start(&pulse);

    // product name: fades in while the letter spacing closes up
    lv_obj_t *name = ui_label(scr, &ui_font_label_lg, C_TEXT, "EXTREMEECU");
    lv_obj_align(name, LV_ALIGN_TOP_MID, 3, LOGO_Y + img_extremeecu_logo.header.h + 22);
    lv_obj_set_style_text_letter_space(name, 22, 0);
    set_opa(name, LV_OPA_TRANSP);
    run_anim(name, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, 400, T_NAME_DELAY, lv_anim_path_ease_out);
    run_anim(name, anim_letter_space_cb, 22, 6, 600, T_NAME_DELAY, lv_anim_path_ease_out);

    // start-up steps, progress bar and info share one fade-in group
    lv_obj_t *grp = lv_obj_create(scr);
    lv_obj_remove_style_all(grp);
    lv_obj_set_size(grp, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(grp, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < STEP_COUNT; i++) {
        int y = 212 + i * 28;
        sp.icon[i] = ui_label(grp, &lv_font_montserrat_16, C_TICK, LV_SYMBOL_MINUS);
        lv_obj_align(sp.icon[i], LV_ALIGN_TOP_LEFT, 140, y);
        sp.name[i] = ui_label(grp, &ui_font_label_lg, C_TICK, STEP_NAMES[i]);
        lv_obj_align(sp.name[i], LV_ALIGN_TOP_LEFT, 172, y);
    }

    sp.bar = lv_bar_create(grp);
    lv_obj_set_size(sp.bar, 220, 6);
    lv_obj_align(sp.bar, LV_ALIGN_TOP_MID, 0, 330);
    lv_bar_set_range(sp.bar, 0, 100);
    lv_obj_set_style_bg_color(sp.bar, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sp.bar, C_BRAND, LV_PART_INDICATOR);
    lv_obj_set_style_anim_time(sp.bar, 400, 0);

    sp.msg = ui_label(grp, &ui_font_label, C_MUTED, "Baslatiliyor...");
    lv_obj_align(sp.msg, LV_ALIGN_TOP_MID, 0, 348);

    lv_obj_t *ver = ui_label(grp, &ui_font_label, C_TICK, "ESP32-S3  |  VEMS v3  |  FW 1.2.x");
    lv_obj_align(ver, LV_ALIGN_TOP_MID, 0, 392);

    anim_group_opa_cb(grp, LV_OPA_TRANSP);
    run_anim(grp, anim_group_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, 500, T_STEPS_DELAY, lv_anim_path_ease_out);

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
