/*
 * Araç profili: ECU'nun desteklediği her servis 01 PID'i, canlı değeri ve yenilenme aralığıyla.
 * Hangi verinin ne sıklıkla geldiği buradan görülür (K-Line saniyede ~7 istek taşır, ekrandaki sayfanın
 * PID'leri öne alınır). PID listesi değişince (yeni bağlantı, cevapsız PID düştü) satırlar yeniden kurulur.
 */
#include <stdio.h>
#include <string.h>
#include "ui_priv.h"

#define MAX_ROWS    96

typedef struct {
    lv_obj_t *value;
    lv_obj_t *info;
} row_t;

static struct {
    lv_obj_t *sub;
    lv_obj_t *list;
    lv_obj_t *empty;
    row_t rows[MAX_ROWS];
    uint8_t pids[MAX_ROWS];
    int count;
    vems_pid_info_t buf[MAX_ROWS];
} p;

static void rows_build(const vems_pid_info_t *info, int n)
{
    lv_obj_clean(p.list);
    p.count = n;
    for (int i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(p.list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 50);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, C_LINE, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *name = ui_label(row, &ui_font_label, C_SOFT, info[i].name);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, 190);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 6, 5);
        p.rows[i].info = ui_label(row, &ui_font_label, C_TICK, "");
        lv_obj_align(p.rows[i].info, LV_ALIGN_TOP_LEFT, 6, 26);
        p.rows[i].value = ui_label(row, &ui_font_label_lg, C_TEXT, "");
        lv_obj_align(p.rows[i].value, LV_ALIGN_RIGHT_MID, -6, 0);
        p.pids[i] = info[i].pid;
    }
}

static bool same_pids(const vems_pid_info_t *info, int n)
{
    if (n != p.count) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        if (info[i].pid != p.pids[i]) {
            return false;
        }
    }
    return true;
}

lv_obj_t *ui_profile_create(int page, int pages)
{
    lv_obj_t *scr = ui_screen_create();
    ui_header(scr, "ARAÇ PROFİLİ", 66);
    p.sub = ui_label(scr, &ui_font_label, C_MUTED, "");
    lv_obj_set_style_text_letter_space(p.sub, 1, 0);
    lv_obj_align(p.sub, LV_ALIGN_TOP_MID, 0, 94);

    p.list = lv_obj_create(scr);
    lv_obj_remove_style_all(p.list);
    lv_obj_set_size(p.list, 332, 306);
    lv_obj_align(p.list, LV_ALIGN_TOP_LEFT, 74, 122);
    lv_obj_set_flex_flow(p.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(p.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p.list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_bottom(p.list, 40, 0);
    lv_obj_clear_flag(p.list, LV_OBJ_FLAG_CLICKABLE);

    p.empty = ui_label(scr, &ui_font_label_lg, C_MUTED, "ECU BAĞLANTISI BEKLENİYOR");
    lv_obj_align(p.empty, LV_ALIGN_CENTER, 0, 0);
    p.count = 0;

    ui_page_dots(scr, page, pages);
    return scr;
}

void ui_profile_update(bool fresh)
{
    const int n = vems_get_profile(p.buf, MAX_ROWS);
    if (!same_pids(p.buf, n)) {
        rows_build(p.buf, n);
    }
    if (n == 0) {
        lv_obj_clear_flag(p.empty, LV_OBJ_FLAG_HIDDEN);
        ui_label_set(p.sub, "");
        return;
    }
    lv_obj_add_flag(p.empty, LV_OBJ_FLAG_HIDDEN);

    char buf[48];
    int polled = 0;
    for (int i = 0; i < n; i++) {
        const vems_pid_info_t *x = &p.buf[i];
        if (x->unit[0] && strcmp(x->text, "--") != 0) {
            snprintf(buf, sizeof(buf), "%s %s", x->text, x->unit);
        } else {
            snprintf(buf, sizeof(buf), "%s", x->text);
        }
        ui_label_set(p.rows[i].value, buf);
        lv_obj_set_style_text_color(p.rows[i].value, x->age_ms == UINT32_MAX ? C_MUTED : C_TEXT, 0);

        // yenilenme: sabit bilgi / ilk cevap bekleniyor / iki cevap arası süre
        if (x->once) {
            snprintf(buf, sizeof(buf), "PID %02X · SABİT BİLGİ", x->pid);
        } else if (x->age_ms == UINT32_MAX) {
            snprintf(buf, sizeof(buf), "PID %02X · BEKLENİYOR", x->pid);
        } else if (x->period_ms == 0) {
            snprintf(buf, sizeof(buf), "PID %02X · İLK OKUMA", x->pid);
        } else {
            snprintf(buf, sizeof(buf), "PID %02X · HER %.1f SN", x->pid, x->period_ms / 1000.0f);
        }
        ui_label_set(p.rows[i].info, buf);
        polled += x->age_ms != UINT32_MAX;
    }
    vems_link_info_t li;
    vems_get_link_info(&li);
    snprintf(buf, sizeof(buf), "%s · %d PID · %d OKUNDU", li.protocol, n, polled);
    ui_label_set(p.sub, fresh ? buf : "VERİ YOK");
}
