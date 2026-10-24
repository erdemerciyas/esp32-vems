/*
 * Uyarılar: her sayfanın üstünde (lv_layer_top) gösterilir ve buzzer çalar.
 *
 *   kritik  (hararet sınırı, yağ sıcaklığı): ekranı kaplayan kart, dokunulana kadar öter. Dokununca üstte kırmızı
 *           şerit kalır, durum REMIND_US sonra hâlâ sürüyorsa kart ve ses geri gelir.
 *   uyarı   (hararet yükseliyor, arıza kodu, şarj, aşırı voltaj, yakıt): şerit + bir kez ses. Şeride dokununca
 *           ya da BANNER_US sonra kapanır; arıza kodu şeridi arıza kodları sayfasını açar.
 *
 * Arıza kodu her yeni bağlantıda ve kod sayısı arttığında yeniden haber verilir. Buzzer LVGL görevinde bir
 * zamanlayıcıyla sürülür, kimse beklemez. Veri yokken koşullar değerlendirilmez, son durum korunur.
 */
#include <math.h>
#include <stdio.h>
#include "esp_timer.h"
#include "board_lcd.h"
#include "ui_priv.h"

#define CLT_EARLY       4           // hararet ön uyarısı: ayardaki sınırın bu kadar altı
#define OIL_CRIT        135
#define CHARGE_MIN      12.8f       // motor çalışırken bunun altı: şarj yok
#define VOLT_HIGH       15.5f
#define FUEL_LOW        10
#define RUNNING_RPM     500
#define REMIND_US       (60 * 1000000LL)
#define BANNER_US       (20 * 1000000LL)

typedef enum { AL_CLT, AL_OIL, AL_CLT_WARM, AL_DTC, AL_CHARGE, AL_VOLT_HIGH, AL_FUEL, AL_COUNT } alert_id_t;

// buzzer desenleri: ms olarak çal, sus, çal ... 0 = son
static const uint16_t BEEP_CRIT[] = {180, 120, 180, 120, 180, 900, 0};     // tekrarlanır
static const uint16_t BEEP_WARN[] = {300, 150, 300, 0};
static const uint16_t BEEP_DTC[] = {80, 80, 80, 80, 80, 0};

// sıra = öncelik
static const struct {
    bool crit;
    uint16_t delay_ms;      // koşul bu kadar sürmeli (marşta voltaj düşüşü, anlık sıçrama)
    const uint16_t *beep;
    const char *title;      // kritik kart başlığı
    const char *advice;
} DEF[AL_COUNT] = {
    [AL_CLT]       = {true, 2000, BEEP_CRIT, "HARARET",
                      "Güvenli bir yerde dur, motoru rölantide soğut. Klimayı kapat, kalorifer sıcakta aç. "
                      "Radyatör kapağını sıcakken açma."},
    [AL_OIL]       = {true, 2000, BEEP_CRIT, "YAĞ SICAKLIĞI", "Yükü ve devri düşür, motoru soğut."},
    [AL_CLT_WARM]  = {false, 3000, BEEP_WARN},
    [AL_DTC]       = {false, 0, BEEP_DTC},
    [AL_CHARGE]    = {false, 10000, BEEP_WARN},
    [AL_VOLT_HIGH] = {false, 5000, BEEP_WARN},
    [AL_FUEL]      = {false, 10000, BEEP_WARN},
};

static struct {
    bool cond;
    int64_t since;          // koşulun başladığı an
    bool active;
    bool acked;             // kart / şerit kapatıldı
    int64_t t;              // açılış ya da kapatılma anı
    float value;
} al[AL_COUNT];

static struct {
    lv_obj_t *card;
    lv_obj_t *card_title;
    lv_obj_t *card_val;
    lv_obj_t *card_msg;
    lv_obj_t *banner;
    lv_obj_t *banner_txt;
    int carded;             // kartta gösterilen uyarı, -1 = yok
    int bannered;
    int dtc_last;           // bu bağlantıda son görülen kod sayısı, -1 = bilinmiyor
    bool running;
} u = {.carded = -1, .bannered = -1, .dtc_last = -1};

static struct {
    const uint16_t *pat;
    int idx;
    bool repeat;
    int64_t next;
} bz;

// ---------------------------------------------------------------------------
// Buzzer
// ---------------------------------------------------------------------------
static void beep_stop(void)
{
    if (bz.pat) {
        bz.pat = NULL;
        board_buzzer_set(false);
    }
}

static void beep_start(const uint16_t *pat, bool repeat)
{
    if (bz.pat && bz.repeat && !repeat) {
        return;             // kritik ses sürerken tek seferlik ses araya girmez
    }
    bz.pat = pat;
    bz.idx = 0;
    bz.repeat = repeat;
    bz.next = 0;
}

static void buzzer_cb(lv_timer_t *timer)
{
    const int64_t now = esp_timer_get_time();
    if (bz.pat == NULL || now < bz.next) {
        return;
    }
    if (bz.pat[bz.idx] == 0) {
        if (!bz.repeat) {
            beep_stop();
            return;
        }
        bz.idx = 0;
    }
    board_buzzer_set((bz.idx & 1) == 0);    // çift indis çalar, tek indis susar
    bz.next = now + bz.pat[bz.idx] * 1000LL;
    bz.idx++;
}

// ---------------------------------------------------------------------------
// Koşullar
// ---------------------------------------------------------------------------
static void check(alert_id_t id, bool cond, float value, int64_t now)
{
    if (cond && !al[id].cond) {
        al[id].since = now;
    }
    al[id].cond = cond;
    al[id].value = value;
    if (!cond) {
        al[id].active = false;
    } else if (!al[id].active && now - al[id].since >= DEF[id].delay_ms * 1000LL) {
        al[id].active = true;
        al[id].acked = false;
        al[id].t = now;
        if (!DEF[id].crit) {
            beep_start(DEF[id].beep, false);
        }
    }
}

static void evaluate(const vems_data_t *d, uint32_t mask, int64_t now)
{
    const float clt = (mask & CH_BIT(CH_CLT)) ? d->clt_c : NAN;
    const float oil = (mask & CH_BIT(CH_OIL)) ? d->oil_c : NAN;
    const float batt = (mask & CH_BIT(CH_BATT)) ? d->batt_v : NAN;
    const float fuel = (mask & CH_BIT(CH_FUEL)) ? d->fuel_pct : NAN;
    const float lim = g_settings.clt_warn;
    u.running = d->rpm > RUNNING_RPM;

    // NAN karşılaştırmaları yanlış: kanal yoksa uyarı da yok. Çıkışta histerezis, sınırda titreşmesin.
    check(AL_CLT, clt >= lim - (al[AL_CLT].active ? 2 : 0), clt, now);
    check(AL_CLT_WARM, !al[AL_CLT].active && clt >= lim - CLT_EARLY - (al[AL_CLT_WARM].active ? 2 : 0), clt, now);
    check(AL_OIL, oil >= OIL_CRIT - (al[AL_OIL].active ? 3 : 0), oil, now);
    check(AL_CHARGE, batt < (u.running ? CHARGE_MIN : g_settings.batt_low / 10.0f), batt, now);
    check(AL_VOLT_HIGH, batt > VOLT_HIGH, batt, now);
    check(AL_FUEL, fuel < FUEL_LOW + (al[AL_FUEL].active ? 3 : 0), fuel, now);

    const int codes = d->mil_codes;
    if (al[AL_DTC].active && u.dtc_last >= 0 && codes > u.dtc_last) {
        al[AL_DTC].acked = false;       // yeni kod: tekrar haber ver
        al[AL_DTC].t = now;
        beep_start(BEEP_DTC, false);
    }
    if (codes >= 0) {
        u.dtc_last = codes;
    }
    check(AL_DTC, codes > 0, codes, now);
}

// ---------------------------------------------------------------------------
// Görünüm
// ---------------------------------------------------------------------------
static void alert_text(alert_id_t id, char *buf, size_t len)
{
    const float v = al[id].value;
    switch (id) {
    case AL_CLT:       snprintf(buf, len, LV_SYMBOL_WARNING "  HARARET %.0f°C", v); break;
    case AL_OIL:       snprintf(buf, len, LV_SYMBOL_WARNING "  YAĞ %.0f°C", v); break;
    case AL_CLT_WARM:  snprintf(buf, len, "HARARET " LV_SYMBOL_UP " %.0f°C", v); break;
    case AL_DTC:       snprintf(buf, len, LV_SYMBOL_WARNING "  ARIZA KODU · %d  ›", (int)v); break;
    case AL_CHARGE:    snprintf(buf, len, "%s %.1f V", u.running ? "ŞARJ YOK" : "AKÜ ZAYIF", v); break;
    case AL_VOLT_HIGH: snprintf(buf, len, "AŞIRI ŞARJ %.1f V", v); break;
    case AL_FUEL:      snprintf(buf, len, "YAKIT AZ · %%%.0f", v); break;
    default:           buf[0] = '\0'; break;
    }
}

static void show(lv_obj_t *o, bool on)
{
    if (on == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {     // sadece değişince: gizleme ekranı yeniden çizdirir
        if (on) {
            lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void view_update(int64_t now)
{
    int card = -1, banner = -1;
    for (int i = 0; i < AL_COUNT; i++) {
        if (!al[i].active) {
            continue;
        }
        if (DEF[i].crit && !al[i].acked) {
            card = card < 0 ? i : card;
        } else if (banner < 0 && (DEF[i].crit || !al[i].acked)) {
            banner = i;             // susturulan kritik uyarı şerit olarak kalır
        }
    }

    char buf[48];
    u.carded = card;
    if (card >= 0) {
        ui_label_set(u.card_title, DEF[card].title);
        snprintf(buf, sizeof(buf), "%.0f", al[card].value);
        ui_label_set(u.card_val, buf);
        ui_label_set(u.card_msg, DEF[card].advice);
        // kırmızı çerçeve 2 Hz yanıp söner
        const lv_opa_t opa = ((now / 250000) & 1) ? LV_OPA_COVER : LV_OPA_30;
        if (lv_obj_get_style_border_opa(u.card, 0) != opa) {
            lv_obj_set_style_border_opa(u.card, opa, 0);
        }
    }
    show(u.card, card >= 0);

    banner = card < 0 ? banner : -1;
    if (banner >= 0) {
        alert_text(banner, buf, sizeof(buf));
        ui_label_set(u.banner_txt, buf);
        if (banner != u.bannered) {
            const bool crit = DEF[banner].crit;
            lv_obj_set_style_bg_color(u.banner, crit ? C_ALERT : C_WARN, 0);
            lv_obj_set_style_text_color(u.banner_txt, crit ? C_TEXT : C_BG, 0);
        }
    }
    u.bannered = banner;
    show(u.banner, banner >= 0);
}

static void card_cb(lv_event_t *e)
{
    const int64_t now = esp_timer_get_time();
    if (u.carded >= 0) {
        al[u.carded].acked = true;
        al[u.carded].t = now;
    }
    beep_stop();            // başka susturulmamış kritik uyarı varsa sonraki güncellemede yeniden başlar
    view_update(now);
}

static void banner_cb(lv_event_t *e)
{
    const int id = u.bannered;
    if (id < 0) {
        return;
    }
    if (id == AL_DTC) {
        ui_request_dtc_page();
    }
    if (!DEF[id].crit) {
        al[id].acked = true;
    }
    view_update(esp_timer_get_time());
}

void ui_alert_init(void)
{
    lv_obj_t *top = lv_layer_top();

    u.card = lv_obj_create(top);
    lv_obj_remove_style_all(u.card);
    lv_obj_set_size(u.card, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_center(u.card);
    lv_obj_set_style_radius(u.card, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(u.card, C_BG, 0);
    lv_obj_set_style_bg_opa(u.card, 240, 0);
    lv_obj_set_style_border_color(u.card, C_ALERT, 0);
    lv_obj_set_style_border_width(u.card, 14, 0);
    lv_obj_clear_flag(u.card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(u.card, card_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon = ui_label(u.card, &lv_font_montserrat_48, C_ALERT, LV_SYMBOL_WARNING);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 66);
    u.card_title = ui_label(u.card, &ui_font_label_lg, C_ALERT, "");
    lv_obj_set_style_text_letter_space(u.card_title, 4, 0);
    lv_obj_align(u.card_title, LV_ALIGN_TOP_MID, 0, 128);

    lv_obj_t *row = lv_obj_create(u.card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 162);
    u.card_val = ui_label(row, &ui_font_big, C_TEXT, "");
    ui_label(row, &ui_font_label_lg, C_ALERT, "°C");

    u.card_msg = ui_label(u.card, &ui_font_label, C_SOFT, "");
    lv_label_set_long_mode(u.card_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(u.card_msg, 300);
    lv_obj_set_style_text_align(u.card_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(u.card_msg, 4, 0);
    lv_obj_align(u.card_msg, LV_ALIGN_TOP_MID, 0, 250);

    lv_obj_t *hint = ui_label(u.card, &ui_font_label, C_MUTED, "DOKUN · SESİ KAPAT");
    lv_obj_set_style_text_letter_space(hint, 2, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 372);
    lv_obj_add_flag(u.card, LV_OBJ_FLAG_HIDDEN);

    u.banner = lv_obj_create(top);
    lv_obj_remove_style_all(u.banner);
    lv_obj_set_size(u.banner, 270, 44);
    lv_obj_align(u.banner, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_radius(u.banner, 22, 0);
    lv_obj_set_style_bg_opa(u.banner, LV_OPA_COVER, 0);
    lv_obj_clear_flag(u.banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(u.banner, banner_cb, LV_EVENT_CLICKED, NULL);
    u.banner_txt = ui_label(u.banner, &ui_font_label_lg, C_BG, "");
    lv_obj_set_style_text_letter_space(u.banner_txt, 1, 0);
    lv_obj_center(u.banner_txt);
    lv_obj_add_flag(u.banner, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(buzzer_cb, 10, NULL);
}

void ui_alert_update(const vems_data_t *d, bool fresh, uint32_t mask)
{
    const int64_t now = esp_timer_get_time();
    if (vems_get_state() != VEMS_STATE_ECU_OK) {
        u.dtc_last = -1;            // yeni bağlantıda kayıtlı kodlar yeniden haber verilir
        al[AL_DTC].cond = al[AL_DTC].active = false;
    }
    if (fresh) {
        evaluate(d, mask, now);
    }

    bool crit_sound = false;
    for (int i = 0; i < AL_COUNT; i++) {
        if (!al[i].active) {
            continue;
        }
        if (DEF[i].crit && al[i].acked && now - al[i].t > REMIND_US) {
            al[i].acked = false;    // susturuldu ama sürüyor: tekrar
            al[i].t = now;
        }
        if (!DEF[i].crit && !al[i].acked && now - al[i].t > BANNER_US) {
            al[i].acked = true;
        }
        crit_sound |= DEF[i].crit && !al[i].acked;
    }
    if (crit_sound && !(bz.pat && bz.repeat)) {
        beep_start(BEEP_CRIT, true);
    } else if (!crit_sound && bz.pat && bz.repeat) {
        beep_stop();
    }
    view_update(now);
}
