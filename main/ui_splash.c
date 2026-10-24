/*
 * Boot screen: title, a progress ring around the glass and the real start-up log of
 * the data source (srclog): display, interface, CAN listening, K-Line init, ECU, PID scan or the
 * VEMS link. Every line ends with its result (OK / SESSİZ / YOK ...), the running one blinks.
 */
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_timer.h"
#include "srclog.h"
#include "ui_priv.h"

#define LINES           7
#define LINE_H          24
#define CON_Y           150
#define READY_HOLD_US   (700 * 1000)
#define TITLE_Y         78
#define T_TITLE         600
#define T_CON_DELAY     700

#define VERSION_TEXT    "ISO 9141-2  ·  ISO 14230-4  ·  v2.1"

static struct {
    lv_obj_t *ring;
    lv_obj_t *con;
    lv_obj_t *text[LINES];
    lv_obj_t *res[LINES];
    lv_obj_t *msg;
    uint32_t seq;
    int wait_line;          // index of the blinking line, -1 = none
    bool blink;
    int progress;           // shown, x10
    int64_t t0;
    int64_t ready_since;
} sp;

static void anim_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, v, 0);
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

lv_obj_t *ui_splash_create(void)
{
    lv_obj_t *scr = ui_screen_create();

    sp.ring = lv_arc_create(scr);
    lv_obj_set_size(sp.ring, 460, 460);
    lv_obj_center(sp.ring);
    lv_arc_set_bg_angles(sp.ring, 0, 360);
    lv_arc_set_rotation(sp.ring, 270);
    lv_arc_set_range(sp.ring, 0, 1000);
    lv_arc_set_value(sp.ring, 0);
    lv_obj_remove_style(sp.ring, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(sp.ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(sp.ring, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(sp.ring, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp.ring, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sp.ring, C_PRIMARY, LV_PART_INDICATOR);

    // başlık: marka yok, sadece ne olduğu; yukarı kayarak belirir
    lv_obj_t *title = lv_obj_create(scr);
    lv_obj_remove_style_all(title);
    lv_obj_set_size(title, LV_PCT(100), 60);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, TITLE_Y);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *name = ui_label(title, &ui_font_label_lg, C_TEXT, "K-LINE OBD");
    lv_obj_set_style_text_letter_space(name, 6, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *what = ui_label(title, &ui_font_label, C_PRIMARY, "CANLI VERİ  ·  ARAÇ PROFİLİ  ·  ARIZA KODU");
    lv_obj_set_style_text_letter_space(what, 1, 0);
    lv_obj_align(what, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_opa(title, LV_OPA_TRANSP, 0);
    run_anim(title, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, T_TITLE, 0, lv_anim_path_ease_out);

    // start-up log, fades in after the title
    sp.con = lv_obj_create(scr);
    lv_obj_remove_style_all(sp.con);
    lv_obj_set_size(sp.con, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(sp.con, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < LINES; i++) {
        const int y = CON_Y + i * LINE_H;
        sp.text[i] = ui_label(sp.con, &ui_font_label, C_SOFT, "");
        lv_label_set_long_mode(sp.text[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(sp.text[i], 210);
        lv_obj_align(sp.text[i], LV_ALIGN_TOP_LEFT, 92, y);
        sp.res[i] = ui_label(sp.con, &ui_font_label, C_TICK, "");
        lv_obj_align(sp.res[i], LV_ALIGN_TOP_RIGHT, -92, y);
    }
    sp.msg = ui_label(sp.con, &ui_font_label_lg, C_MUTED, "");
    lv_obj_set_style_text_letter_space(sp.msg, 2, 0);
    lv_obj_align(sp.msg, LV_ALIGN_TOP_MID, 0, CON_Y + LINES * LINE_H + 10);
    lv_obj_t *ver = ui_label(sp.con, &ui_font_label, C_TICK, VERSION_TEXT);
    lv_obj_set_style_text_letter_space(ver, 1, 0);
    lv_obj_align(ver, LV_ALIGN_TOP_MID, 0, CON_Y + LINES * LINE_H + 44);
    lv_obj_set_style_opa(sp.con, LV_OPA_TRANSP, 0);
    run_anim(sp.con, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, 450, T_CON_DELAY, lv_anim_path_ease_out);

    sp.seq = UINT32_MAX;
    sp.wait_line = -1;
    sp.progress = 0;
    sp.t0 = esp_timer_get_time();
    sp.ready_since = 0;
    return scr;
}

static lv_color_t state_color(uint8_t st)
{
    switch (st) {
    case SRCLOG_OK:   return C_OK;
    case SRCLOG_SKIP: return C_TICK;
    case SRCLOG_FAIL: return C_ALERT;
    default:          return C_WARN;
    }
}

static void show_log(void)
{
    srclog_line_t lines[LINES];
    uint32_t seq;
    const int n = srclog_read(lines, LINES, &seq);
    if (seq == sp.seq) {
        return;
    }
    sp.seq = seq;
    sp.wait_line = -1;
    char buf[48];
    for (int i = 0; i < LINES; i++) {
        if (i < n) {
            snprintf(buf, sizeof(buf), "› %.31s", lines[i].text);
            ui_label_set(sp.text[i], buf);
            ui_label_set(sp.res[i], lines[i].result);
            lv_obj_set_style_text_color(sp.res[i], state_color(lines[i].state), 0);
            lv_obj_set_style_text_color(sp.text[i], i == n - 1 ? C_TEXT : C_SOFT, 0);
            if (lines[i].state == SRCLOG_WAIT) {
                sp.wait_line = i;
            }
        } else {
            ui_label_set(sp.text[i], "");
            ui_label_set(sp.res[i], "");
        }
    }
}

bool ui_splash_update(void)
{
    const int64_t now = esp_timer_get_time();
    const int64_t elapsed_ms = (now - sp.t0) / 1000;
    const vems_state_t st = vems_get_state();

    show_log();
    if (sp.wait_line >= 0) {
        sp.blink = !sp.blink;
        lv_obj_set_style_text_opa(sp.res[sp.wait_line], sp.blink ? LV_OPA_COVER : LV_OPA_30, 0);
    }

    int target;
    const char *msg;
    switch (st) {
    case VEMS_STATE_ECU_OK:    target = 1000; msg = "HAZIR"; break;
    case VEMS_STATE_LINK_UP:   target = 850;  msg = "VERİLER TARANIYOR"; break;
    case VEMS_STATE_WAIT_LINK: target = 300 + (int)(elapsed_ms / 40 > 400 ? 400 : elapsed_ms / 40); msg = "PROTOKOL ARANIYOR"; break;
    default:                   target = 120;  msg = "BAŞLATILIYOR"; break;
    }
    sp.progress += (target - sp.progress) / 4 + (target > sp.progress ? 1 : 0);
    lv_arc_set_value(sp.ring, sp.progress);
    ui_label_set(sp.msg, msg);
    lv_obj_set_style_text_color(sp.msg, st == VEMS_STATE_ECU_OK ? C_OK : C_MUTED, 0);

    if (st == VEMS_STATE_ECU_OK) {
        if (sp.ready_since == 0) {
            sp.ready_since = now;
        }
        return elapsed_ms >= CONFIG_VEMS_UI_SPLASH_MIN_MS && (now - sp.ready_since) >= READY_HOLD_US;
    }
    sp.ready_since = 0;
    return elapsed_ms >= CONFIG_VEMS_UI_SPLASH_TIMEOUT_MS;     // the pages show "ARANIYOR" themselves
}
