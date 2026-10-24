/*
 * Arıza kodları sayfası: kayıtlı kodları (OBD 03) açıklamalarıyla listeler, SİL (OBD 04) iki dokunuşla onaylanır.
 * ECU cevabı birkaç saniye sürebildiği için istek ayrı bir görevde çalışır, LVGL görevi beklemez.
 * Sayfa ilk açıldığında bağlantı varsa kodlar kendiliğinden okunur.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "ui_priv.h"

#define CONFIRM_US  (4 * 1000 * 1000)

// genel OBD-II kod açıklamaları (Kalos 1.2/1.4 motorlarında sık görülenler)
static const struct {
    uint16_t code;
    const char *txt;
} DTC_TXT[] = {
    {0x0100, "MAF devresi"}, {0x0105, "MAP sensörü devresi"}, {0x0106, "MAP sensörü aralık/performans"},
    {0x0107, "MAP sensörü düşük"}, {0x0108, "MAP sensörü yüksek"}, {0x0110, "Emme havası sensörü devresi"},
    {0x0112, "Emme havası sensörü düşük"}, {0x0113, "Emme havası sensörü yüksek"}, {0x0115, "Hararet sensörü devresi"},
    {0x0117, "Hararet sensörü düşük"}, {0x0118, "Hararet sensörü yüksek"}, {0x0120, "Gaz kelebeği sensörü devresi"},
    {0x0121, "Gaz kelebeği sensörü aralık"}, {0x0122, "Gaz kelebeği sensörü düşük"},
    {0x0123, "Gaz kelebeği sensörü yüksek"}, {0x0125, "Motor kapalı çevrim için ısınmıyor"},
    {0x0130, "Ön O2 sensörü devresi"}, {0x0131, "Ön O2 sensörü düşük voltaj"}, {0x0132, "Ön O2 sensörü yüksek voltaj"},
    {0x0133, "Ön O2 sensörü yavaş tepki"}, {0x0134, "Ön O2 sensörü aktivite yok"}, {0x0135, "Ön O2 ısıtıcı devresi"},
    {0x0136, "Arka O2 sensörü devresi"}, {0x0137, "Arka O2 sensörü düşük voltaj"},
    {0x0138, "Arka O2 sensörü yüksek voltaj"}, {0x0140, "Arka O2 sensörü aktivite yok"},
    {0x0141, "Arka O2 ısıtıcı devresi"}, {0x0170, "Yakıt düzeltme arızası"}, {0x0171, "Karışım fakir"},
    {0x0172, "Karışım zengin"}, {0x0201, "1. enjektör devresi"}, {0x0202, "2. enjektör devresi"},
    {0x0203, "3. enjektör devresi"}, {0x0204, "4. enjektör devresi"}, {0x0230, "Yakıt pompası rölesi devresi"},
    {0x0261, "1. enjektör düşük"}, {0x0264, "2. enjektör düşük"}, {0x0267, "3. enjektör düşük"},
    {0x0270, "4. enjektör düşük"}, {0x0300, "Rastgele / çoklu silindir tekleme"}, {0x0301, "1. silindir tekleme"},
    {0x0302, "2. silindir tekleme"}, {0x0303, "3. silindir tekleme"}, {0x0304, "4. silindir tekleme"},
    {0x0325, "Vuruntu sensörü devresi"}, {0x0327, "Vuruntu sensörü düşük"}, {0x0328, "Vuruntu sensörü yüksek"},
    {0x0335, "Krank sensörü devresi"}, {0x0336, "Krank sensörü aralık/performans"},
    {0x0340, "Eksantrik sensörü devresi"}, {0x0351, "1. ateşleme bobini devresi"},
    {0x0352, "2. ateşleme bobini devresi"}, {0x0400, "EGR akış arızası"}, {0x0401, "EGR akışı yetersiz"},
    {0x0402, "EGR akışı fazla"}, {0x0404, "EGR aralık/performans"}, {0x0405, "EGR konum sensörü düşük"},
    {0x0406, "EGR konum sensörü yüksek"}, {0x0420, "Katalizör verimi düşük"}, {0x0441, "Kanister (EVAP) akış hatası"},
    {0x0443, "Kanister valfi devresi"}, {0x0480, "Fan rölesi devresi"}, {0x0500, "Hız sensörü"},
    {0x0501, "Hız sensörü aralık/performans"}, {0x0505, "Rölanti kontrol sistemi"},
    {0x0506, "Rölanti beklenenden düşük"}, {0x0507, "Rölanti beklenenden yüksek"},
    {0x0530, "Klima basınç sensörü devresi"}, {0x0560, "Sistem voltajı"}, {0x0562, "Sistem voltajı düşük"},
    {0x0563, "Sistem voltajı yüksek"}, {0x0601, "ECU bellek hatası"}, {0x0602, "ECU programlanmamış"},
    {0x0604, "ECU RAM hatası"}, {0x0605, "ECU ROM hatası"},
};

static struct {
    lv_obj_t *status;
    lv_obj_t *list;
    lv_obj_t *clear_lbl;
    int64_t confirm_t;          // SİL'e ilk dokunuş, 0 = onay beklenmiyor
    bool read_once;             // bu bağlantıda kendiliğinden okundu
    uint16_t rpm;
    // istek görevi: busy iken sadece görev yazar, done olunca sadece LVGL okur
    volatile bool busy;
    volatile bool done;
    bool result;                // gösterilecek bir sonuç var (sayfa yeniden kurulunca da)
    bool cleared;
    bool clear_ok;
    int n;
    uint16_t codes[VEMS_DTC_MAX];
} t;

static const char *dtc_text(uint16_t c)
{
    for (size_t i = 0; i < sizeof(DTC_TXT) / sizeof(DTC_TXT[0]); i++) {
        if (DTC_TXT[i].code == c) {
            return DTC_TXT[i].txt;
        }
    }
    return (c & 0x3000) == 0x1000 ? "Üreticiye özel kod" : "Açıklama yok";
}

static void status_set(const char *txt, lv_color_t col)
{
    ui_label_set(t.status, txt);
    lv_obj_set_style_text_color(t.status, col, 0);
}

static void worker(void *arg)
{
    t.cleared = arg != NULL;
    t.clear_ok = t.cleared && vems_dtc_clear();
    t.n = (!t.cleared || t.clear_ok) ? vems_dtc_read(t.codes, VEMS_DTC_MAX) : -1;
    t.done = true;
    t.busy = false;
    vTaskDelete(NULL);
}

static void start(bool clear)
{
    if (t.busy) {
        return;
    }
    t.busy = true;
    t.done = false;
    status_set(clear ? "SİLİNİYOR…" : "OKUNUYOR…", C_WARN);
    if (xTaskCreate(worker, "dtc", 3072, clear ? (void *)1 : NULL, 3, NULL) != pdPASS) {
        t.busy = false;
        status_set("BELLEK YOK", C_ALERT);
    }
}

static void show_result(void)
{
    lv_obj_clean(t.list);
    if (t.cleared && !t.clear_ok) {
        status_set("ECU SİLMEYİ ONAYLAMADI", C_ALERT);
        return;
    }
    if (t.n < 0) {
        status_set("ECU CEVAP VERMEDİ", C_ALERT);
        return;
    }
    char buf[40];
    if (t.n == 0) {
        status_set(t.cleared ? "SİLİNDİ · KAYITLI KOD YOK" : "KAYITLI ARIZA KODU YOK", C_OK);
        return;
    }
    snprintf(buf, sizeof(buf), "%d KAYITLI KOD", t.n);
    status_set(buf, C_WARN);
    for (int i = 0; i < t.n; i++) {
        lv_obj_t *row = lv_obj_create(t.list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_ver(row, 7, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, C_LINE, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        // üst 2 bit sistem harfi, kalan 14 bit 4 hane: 0x0301 -> P0301
        snprintf(buf, sizeof(buf), "%c%04X", "PCBU"[t.codes[i] >> 14], t.codes[i] & 0x3FFF);
        lv_obj_t *code = ui_label(row, &ui_font_label_lg, C_WARN, buf);
        lv_obj_align(code, LV_ALIGN_TOP_LEFT, 6, 0);
        lv_obj_t *txt = ui_label(row, &ui_font_label, C_SOFT, dtc_text(t.codes[i]));
        lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(txt, 226);
        lv_obj_align(txt, LV_ALIGN_TOP_LEFT, 96, 3);
    }
}

static void read_cb(lv_event_t *e)
{
    t.confirm_t = 0;
    ui_label_set(t.clear_lbl, "SİL");
    start(false);
}

static void clear_cb(lv_event_t *e)
{
    if (t.busy) {
        return;
    }
    if (t.rpm > 300) {
        status_set("MOTOR ÇALIŞIYOR · ÖNCE DURDUR", C_ALERT);
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (t.confirm_t == 0 || now - t.confirm_t > CONFIRM_US) {
        t.confirm_t = now;
        ui_label_set(t.clear_lbl, "EMİN MİSİN?");
        status_set("SİLMEK İÇİN TEKRAR BAS", C_WARN);
        return;
    }
    t.confirm_t = 0;
    ui_label_set(t.clear_lbl, "SİL");
    start(true);
}

lv_obj_t *ui_dtc_create(int page, int pages)
{
    lv_obj_t *scr = ui_screen_create();
    ui_header(scr, "ARIZA KODLARI", 66);

    t.status = ui_label(scr, &ui_font_label, C_MUTED, "");
    lv_obj_set_style_text_letter_space(t.status, 1, 0);
    lv_obj_align(t.status, LV_ALIGN_TOP_MID, 0, 96);

    t.list = lv_obj_create(scr);
    lv_obj_remove_style_all(t.list);
    lv_obj_set_size(t.list, 332, 208);
    lv_obj_align(t.list, LV_ALIGN_TOP_LEFT, 74, 124);
    lv_obj_set_flex_flow(t.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(t.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(t.list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(t.list, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *rd = ui_pill_button(scr, "OKU", 128);
    lv_obj_align(rd, LV_ALIGN_TOP_MID, -72, 346);
    lv_obj_add_event_cb(rd, read_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = ui_pill_button(scr, "SİL", 128);
    lv_obj_align(cl, LV_ALIGN_TOP_MID, 72, 346);
    lv_obj_add_event_cb(cl, clear_cb, LV_EVENT_CLICKED, NULL);
    t.clear_lbl = lv_obj_get_child(cl, 0);

    ui_page_dots(scr, page, pages);
    t.confirm_t = 0;
    if (t.result || t.done) {
        show_result();          // sayfalar yeniden kurulunca son sonuç kalsın
    } else {
        status_set(t.busy ? "OKUNUYOR…" : "OKU'YA BAS", t.busy ? C_WARN : C_MUTED);
    }
    return scr;
}

void ui_dtc_update(const vems_data_t *d, bool fresh)
{
    t.rpm = fresh ? d->rpm : 0;
    if (t.done && !t.busy) {
        show_result();
        t.done = false;
        t.result = true;
        t.read_once = true;
    }
    if (t.confirm_t && esp_timer_get_time() - t.confirm_t > CONFIRM_US) {
        t.confirm_t = 0;
        ui_label_set(t.clear_lbl, "SİL");
        status_set("", C_MUTED);
    }
    const bool on = vems_get_state() == VEMS_STATE_ECU_OK;
    if (!on) {
        t.read_once = false;    // yeni bağlantıda tekrar kendiliğinden oku
    } else if (!t.read_once && !t.busy) {
        t.read_once = true;
        start(false);
    }
}
