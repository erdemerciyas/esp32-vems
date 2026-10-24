/*
 * OBD-II data source: reads the standard mode 01 PIDs from the diagnostic socket of any car and
 * fills the same vems_data_t the dashboard shows for VEMS (vems_get_latest() & co. live here).
 *
 * K-Line (OBD pin 7, L9637D) üzerinden algılama, bir ECU cevap verene kadar tekrarlanır: önce ISO 14230-4
 * hızlı init, sonra 5 baud init (ISO 9141-2 ya da ISO 14230-4, 2. anahtar kelimeyle ayırt edilir).
 * The first ECU answering 01 00 is polled alone, only for the PIDs it reports as supported.
 * RPM is asked every other request, the rest round robin (slow PIDs once per fast round).
 * Ekranda görünen kanalların PID'leri hızlı tura alınır (vems_set_wanted_channels), kalanlar yavaş turda.
 *
 * Read only: OBD ECUs only talk when asked, so requests are unavoidable, but the only service ever
 * sent is 01 (show current data) plus the K-Line wake-up handshake (5 baud address, ~KW2,
 * StartCommunication 81). Nothing that runs tests or writes is in this file;
 * obd_tx_allowed() rejects any other service id before it reaches the bus.
 * İstisna: arıza kodları, sadece kullanıcı ekrandaki ARIZA KODLARI sayfasından isteyince 03 (oku) ve onaylayınca
 * 04 (sil) gönderilir.
 * Araç profili: ECU'nun desteklediği ve obd_pid.c tablosunda olan her PID sorgulanır, değişmeyenler bir kez okunur.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "vems_proto.h"
#include "obd_pid.h"
#include "srclog.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "hal/gpio_ll.h"

static const char *TAG = "obd";

#define MAX_FAILS           5
#define RETRY_MS            2000
#define DATA_MAX            8

#define PID_MIL             0x01    // arıza lambası + kayıtlı kod sayısı
#define PID_FUEL_SYS        0x03
#define PID_LOAD            0x04
#define PID_CLT             0x05
#define PID_STFT1           0x06
#define PID_LTFT1           0x07
#define PID_MAP             0x0B
#define PID_RPM             0x0C
#define PID_SPEED           0x0D
#define PID_ADV             0x0E
#define PID_IAT             0x0F
#define PID_MAF             0x10
#define PID_TPS             0x11
#define PID_O2S1            0x14    // narrow band O2 B1S1: voltage + trim
#define PID_O2S2            0x15    // narrow band O2 B1S2 (katalizör arkası)
#define PID_O2S1_WR_V       0x24    // wide range O2 B1S1: lambda + voltage
#define PID_FUEL_LVL        0x2F
#define PID_O2S1_WR_I       0x34    // wide range O2 B1S1: lambda + current
#define PID_VBAT            0x42
#define PID_LAMBDA_CMD      0x44
#define PID_OIL             0x5C
#define PID_FUEL_RATE       0x5E
#define PID_LAST            0x5F    // sorgulanan en büyük PID (60 = sonraki destek haritası)

// ekran bir şey istemiyorsa hızlı turdakiler (her istek ~140 ms); desteklenen diğer bütün PID'ler yavaş turda
// hararet her zaman hızlı turda: uyarı hangi sayfada olunursa olsun gecikmesin
static const uint8_t FAST_PIDS[] = {PID_CLT, PID_MAP, PID_TPS, PID_O2S1_WR_V, PID_O2S1_WR_I, PID_O2S1, PID_SPEED, PID_MAF};

static SemaphoreHandle_t s_lock;
static vems_data_t s_data;
static bool s_have_data;
static vems_stats_t s_stats;
static volatile vems_state_t s_state = VEMS_STATE_STOPPED;
static volatile vems_mode_t s_mode = VEMS_MODE_NONE;
static uint32_t s_baud;
static uint32_t s_sup[3];           // supported PIDs 01-20, 21-40, 41-60 (01 00 / 01 20 / 01 40 bitmaps)
static uint8_t s_fast[PID_LAST], s_slow[PID_LAST];
// araç profili: her PID'in son ham cevabı ve iki cevap arası süre (s_lock ile)
static uint8_t s_raw[PID_LAST + 1][4];
static int64_t s_rx_us[PID_LAST + 1];   // son cevap, 0 = hiç gelmedi
static uint32_t s_period_ms[PID_LAST + 1];
static int s_nfast, s_nslow, s_fi, s_si;
static unsigned s_slot;
static volatile uint32_t s_want;        // ekrandaki kanallar (vems_set_wanted_channels)
static uint32_t s_built_want;           // listelerin kurulduğu s_want
static volatile uint32_t s_channels;    // CH_BIT mask of what is polled, 0 while not connected
static volatile bool s_rescan;
// arıza kodu isteği: ekran görevi s_dtc_req'i kurar, OBD görevi iki sorgu arasında çalıştırıp s_dtc_done'ı verir
static volatile uint8_t s_dtc_req;      // 0 = yok, SID_DTC_READ / SID_DTC_CLEAR
static SemaphoreHandle_t s_dtc_done;
static uint16_t s_dtc[VEMS_DTC_MAX];
static int s_ndtc;                      // -1 = cevap yok

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
/** Data bytes of a mode 01 answer (after 41 PID). */
static int pid_len(uint8_t pid)
{
    if (pid % 0x20 == 0) {
        return 4;               // 00 / 20 / 40: destek haritası
    }
    const obd_pid_def_t *d = obd_pid_def(pid);
    return d ? d->len : 1;
}

static bool pid_supported(uint8_t pid)
{
    if (pid == 0) {
        return true;
    }
    if (pid > 0x60) {
        return false;
    }
    return (s_sup[(pid - 1) / 32] >> (31 - (pid - 1) % 32)) & 1;
}

#define SID_CURRENT_DATA    0x01
#define SID_DTC_READ        0x03    // kayıtlı arıza kodları
#define SID_DTC_CLEAR       0x04    // arıza kodlarını ve MIL'i sil (sadece kullanıcı onayıyla)
#define SID_START_COMM      0x81    // KWP2000 StartCommunication (K-Line session open, no data)

static bool obd_tx_allowed(uint8_t sid)
{
    if (sid == SID_CURRENT_DATA || sid == SID_START_COMM || sid == SID_DTC_READ || sid == SID_DTC_CLEAR) {
        return true;
    }
    ESP_LOGE(TAG, "service 0x%02X blocked: read only", sid);
    return false;
}

static uint8_t sum8(const uint8_t *p, size_t n)
{
    uint8_t s = 0;
    while (n--) {
        s += *p++;
    }
    return s;
}

// ---------------------------------------------------------------------------
// K-Line (ISO 9141-2 / ISO 14230-4) over the L9637D, half duplex: every sent byte echoes back
// ---------------------------------------------------------------------------
#define KL_PORT         CONFIG_VEMS_OBD_KLINE_UART_PORT
#define KL_TX           CONFIG_VEMS_OBD_KLINE_TX_GPIO
#define KL_RX           CONFIG_VEMS_OBD_KLINE_RX_GPIO
#define KL_BAUD         10400
#define KL_P2_MS        210     // first answer byte (P2max 50 ms, some ECUs are late; ELM327 ATST32 = 205 ms)
#define KL_IDLE_MS      60      // answers end when the bus is idle > P3min (55 ms): also the gap before the next request
#define KL_P4_MS        5       // tester inter-byte time
#define KL_ECHO_MS      10
#define KL_W5_MS        300     // idle bus before an init
#define KL_P3MAX_MS     5000    // ECU session timeout, waited before a new init

static uint8_t s_kl_ecu;        // source address of the polled ECU, 0 = not locked yet

// why the last fast init failed, first problem seen on the wire (shown on the boot screen)
enum { KL_DIAG_SILENT, KL_DIAG_IDLE_LOW, KL_DIAG_NO_LOW, KL_DIAG_NO_ECHO, KL_DIAG_TX_STUCK };
static uint8_t s_kl_diag;

static esp_err_t kline_start(void)
{
    const uart_config_t cfg = {
        .baud_rate = KL_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(KL_PORT, 512, 0, 0, NULL, 0), TAG, "uart install");
    ESP_RETURN_ON_ERROR(uart_param_config(KL_PORT, &cfg), TAG, "uart config");
    ESP_RETURN_ON_ERROR(uart_set_pin(KL_PORT, KL_TX, KL_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart pins");
    gpio_set_direction(KL_RX, GPIO_MODE_INPUT);     // never drive RX (on the Waveshare board GPIO43 is the console TX pad)
    gpio_ll_input_enable(&GPIO, KL_TX);             // read the TX pad back: shows if something else holds it high
    uart_set_rx_timeout(KL_PORT, 2);                // hand bytes over after 2 idle symbols, not 10
    return ESP_OK;
}

/** One log line with the bytes in hex: what went out / came back on the K-Line. */
static bool s_kl_quiet;         // canlı sorgu turunda bayt logu kapalı: her istekte 3 log satırı sorguyu yavaşlatır

static void kl_log(const char *what, const uint8_t *b, size_t n)
{
    if (s_kl_quiet) {
        return;
    }
    char hex[3 * 32 + 4];
    size_t k = 0;
    for (size_t i = 0; i < n && i < 32; i++) {
        k += snprintf(&hex[k], sizeof(hex) - k, " %02X", b[i]);
    }
    if (n > 32) {
        snprintf(&hex[k], sizeof(hex) - k, " ..");
    }
    ESP_LOGI(TAG, "K %-5s %2u bayt:%s", what, (unsigned)n, n ? hex : " (yok)");
}

/** Init pulses: the inverted UART TX holds the line low while the transmitter idles. */
static void kl_drive_low(bool low)
{
    uart_set_line_inverse(KL_PORT, low ? UART_SIGNAL_TXD_INV : UART_SIGNAL_INV_DISABLE);
}

/** Returns false if a byte did not echo back unchanged (no K-Line driver or receiver). */
static bool kl_send(const uint8_t *p, size_t n)
{
    bool echoed = true;
    uint8_t echo[16];
    size_t ne = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t e;
        uart_write_bytes(KL_PORT, &p[i], 1);
        uart_wait_tx_done(KL_PORT, pdMS_TO_TICKS(20));
        if (uart_read_bytes(KL_PORT, &e, 1, pdMS_TO_TICKS(KL_ECHO_MS)) != 1) {
            echoed = false;
        } else {
            if (ne < sizeof(echo)) {
                echo[ne++] = e;
            }
            if (e != p[i]) {
                echoed = false;
            }
        }
        if (i + 1 < n) {
            vTaskDelay(pdMS_TO_TICKS(KL_P4_MS));
        }
    }
    kl_log("TX", p, n);         // after the frame: the answer waits in the UART buffer meanwhile
    kl_log("echo", echo, ne);
    return echoed;
}

/** Collects every answer byte until the bus is idle. */
static size_t kl_receive(uint8_t *buf, size_t max)
{
    size_t n = 0;
    TickType_t wait = pdMS_TO_TICKS(KL_P2_MS);
    while (n < max && uart_read_bytes(KL_PORT, &buf[n], 1, wait) == 1) {
        n++;
        wait = pdMS_TO_TICKS(KL_IDLE_MS);
    }
    kl_log("RX", buf, n);
    return n;
}

/** Builds a request frame, 0 (nothing to send) for a service that is not read only. */
static size_t kl_build(uint8_t *f, const uint8_t *data, size_t n)
{
    size_t i = 0;
    if (!obd_tx_allowed(data[0])) {
        return 0;
    }
    if (s_mode == VEMS_MODE_OBD_ISO9141) {
        f[i++] = 0x68;
        f[i++] = 0x6A;
    } else {
        f[i++] = 0xC0 | n;      // functional addressing + length
        f[i++] = 0x33;
    }
    f[i++] = 0xF1;
    memcpy(&f[i], data, n);
    i += n;
    f[i] = sum8(f, i);
    return i + 1;
}

/**
 * Finds the first valid frame of the polled ECU whose payload is <sid> [<pid>] in the received bytes,
 * starting at *pos (moved past the frame, so repeated calls walk multi frame answers).
 * pid < 0: no PID byte. Returns the data length after sid/pid, -1 if there is none.
 */
static int kl_find(const uint8_t *b, size_t n, size_t *pos, uint8_t sid, int pid, uint8_t *out, uint8_t *src_out)
{
    size_t i = *pos;
    while (i + 4 <= n) {
        size_t hdr, len;
        if (s_mode == VEMS_MODE_OBD_ISO9141) {
            if (b[i] != 0x48 || b[i + 1] != 0x6B) {
                i++;
                continue;
            }
            hdr = 3;
            // no length byte: 41 PID data / 43 + 3 DTC / 44
            len = sid == 0x41 ? 2 + pid_len(pid) : sid == 0x43 ? 7 : 1;
        } else {
            uint8_t fmt = b[i];
            if ((fmt & 0xC0) == 0) {    // OBD answers always carry addresses
                i++;
                continue;
            }
            hdr = 3;
            len = fmt & 0x3F;
            if (len == 0) {
                hdr = 4;
                len = b[i + 3];
            }
        }
        size_t total = hdr + len + 1;
        if (len == 0 || i + total > n || sum8(&b[i], total - 1) != b[i + total - 1]) {
            i++;
            continue;
        }
        const uint8_t *p = &b[i + hdr];
        uint8_t src = b[i + 2];
        i += total;
        size_t skip = pid < 0 ? 1 : 2;
        if (len < skip || p[0] != sid || (pid >= 0 && p[1] != pid) || (s_kl_ecu && src != s_kl_ecu)) {
            continue;
        }
        int dl = (int)(len - skip);
        if (dl > DATA_MAX) {
            dl = DATA_MAX;
        }
        memcpy(out, p + skip, dl);
        *src_out = src;
        *pos = i;
        return dl;
    }
    *pos = n;
    return -1;
}

static int kl_request(uint8_t pid, uint8_t *out)
{
    uint8_t req[8], rx[64], src;
    const uint8_t data[] = {SID_CURRENT_DATA, pid};
    size_t n = kl_build(req, data, sizeof(data)), pos = 0;
    uart_flush_input(KL_PORT);
    kl_send(req, n);
    int len = kl_find(rx, kl_receive(rx, sizeof(rx)), &pos, 0x41, pid, out, &src);
    if (len >= 0 && !s_kl_ecu) {
        s_kl_ecu = src;
    }
    return len;
}

/**
 * Arıza kodu servisi (03 oku / 04 sil). K-Line'da her cevap çerçevesi 3 kod taşır, boşlar 0000 ile doldurulur;
 * çok kodda birden fazla çerçeve gelir. Kod sayısını döner (04 için 0), cevap yoksa -1.
 */
static int kl_dtc(uint8_t sid, uint16_t *codes, int max)
{
    uint8_t req[8], rx[160], d[DATA_MAX], src;
    const uint8_t data[] = {sid};
    size_t n = kl_build(req, data, sizeof(data));
    uart_flush_input(KL_PORT);
    kl_send(req, n);
    int nc = -1;
    // 7F xx 78 (cevap bekleniyor) gelirse asıl cevap için tekrar dinlenir
    for (int round = 0; round < 5 && nc < 0; round++) {
        n = kl_receive(rx, sizeof(rx));
        if (n == 0) {
            break;
        }
        size_t pos = 0;
        int len;
        while ((len = kl_find(rx, n, &pos, sid | 0x40, -1, d, &src)) >= 0) {
            nc = nc < 0 ? 0 : nc;
            for (int i = 0; i + 1 < len && nc < max; i += 2) {
                const uint16_t c = (uint16_t)((d[i] << 8) | d[i + 1]);
                if (c) {
                    codes[nc++] = c;
                }
            }
        }
    }
    return nc;
}

static bool kl_fast_init(void)
{
    s_mode = VEMS_MODE_OBD_KWP_FAST;
    s_kl_ecu = 0;
    vTaskDelay(pdMS_TO_TICKS(KL_W5_MS));
    s_kl_diag = gpio_get_level(KL_RX) ? KL_DIAG_SILENT : KL_DIAG_IDLE_LOW;     // idle K must read high
    // TX pad and RX level every ms over TWuP (25 ms low, 25 ms high), logged after the init
    char tx_lv[51], rx_lv[51];
    vTaskDelay(1);                              // start on a tick edge
    TickType_t t = xTaskGetTickCount();
    kl_drive_low(true);                         // TiniL 25 ms
    for (int i = 0; i < 50; i++) {
        if (i == 25) {
            kl_drive_low(false);                // TiniH 25 ms
        }
        vTaskDelayUntil(&t, 1);
        tx_lv[i] = '0' + gpio_get_level(KL_TX);
        rx_lv[i] = '0' + gpio_get_level(KL_RX);
    }
    tx_lv[50] = rx_lv[50] = 0;
    if (s_kl_diag == KL_DIAG_SILENT && !memchr(tx_lv, '0', 25)) {
        s_kl_diag = KL_DIAG_TX_STUCK;           // the ESP pad itself never went low: held high on the board
    } else if (s_kl_diag == KL_DIAG_SILENT && !memchr(rx_lv, '0', 25)) {
        s_kl_diag = KL_DIAG_NO_LOW;             // K did not follow TX
    }
    uart_flush_input(KL_PORT);

    static const uint8_t START_COMM[] = {SID_START_COMM};
    uint8_t req[8], rx[64], d[DATA_MAX], src;
    if (!kl_send(req, kl_build(req, START_COMM, sizeof(START_COMM))) && s_kl_diag == KL_DIAG_SILENT) {
        s_kl_diag = KL_DIAG_NO_ECHO;
    }
    const size_t n = kl_receive(rx, sizeof(rx));
    size_t pos = 0;
    ESP_LOGI(TAG, "K init  TX pad LOW|HIGH: %.25s | %s", tx_lv, tx_lv + 25);
    ESP_LOGI(TAG, "K init  RX     LOW|HIGH: %.25s | %s", rx_lv, rx_lv + 25);
    if (kl_find(rx, n, &pos, 0xC1, -1, d, &src) < 0) {
        ESP_LOGW(TAG, "K fast init: StartCommunication cevabı (C1) yok");
        return false;
    }
    s_kl_ecu = src;
    ESP_LOGI(TAG, "K-Line fast init OK, ECU 0x%02X", src);
    return true;
}

static bool kl_slow_init(void)
{
    s_kl_ecu = 0;
    vTaskDelay(pdMS_TO_TICKS(KL_W5_MS));
    // address 0x33 at 5 baud: start bit, 8 data bits LSB first, stop bit, 200 ms each
    const uint16_t bits = (0x33 << 1) | (1 << 9);
    vTaskDelay(1);
    TickType_t t = xTaskGetTickCount();
    for (int i = 0; i < 10; i++) {
        kl_drive_low(!((bits >> i) & 1));
        vTaskDelayUntil(&t, pdMS_TO_TICKS(200));
    }
    uart_flush_input(KL_PORT);

    // sync 0x55 after W1 (60..300 ms), keywords within W2 / W3 (5..20 ms each)
    uint8_t b[3], ack;
    if (uart_read_bytes(KL_PORT, b, 1, pdMS_TO_TICKS(400)) != 1 || b[0] != 0x55) {
        ESP_LOGW(TAG, "K 5 baud init: senkron 0x55 gelmedi");
        return false;
    }
    if (uart_read_bytes(KL_PORT, &b[1], 2, pdMS_TO_TICKS(100)) != 2) {
        ESP_LOGW(TAG, "K 5 baud init: 0x55 geldi, anahtar kelimeler gelmedi");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(30));              // W4 25..50 ms
    const uint8_t inv_kw2 = ~b[2];
    kl_send(&inv_kw2, 1);
    if (uart_read_bytes(KL_PORT, &ack, 1, pdMS_TO_TICKS(100)) != 1 || ack != (uint8_t)~0x33) {
        ESP_LOGW(TAG, "K 5 baud init: KW1 0x%02X KW2 0x%02X geldi, ECU onayı (0xCC) gelmedi", b[1], b[2]);
        return false;
    }
    s_mode = b[2] == 0x8F ? VEMS_MODE_OBD_KWP_5BAUD : VEMS_MODE_OBD_ISO9141;
    ESP_LOGI(TAG, "K-Line 5 baud init OK, KW1 0x%02X KW2 0x%02X", b[1], b[2]);
    vTaskDelay(pdMS_TO_TICKS(KL_IDLE_MS));      // P3min before the first request
    return true;
}

/**
 * After "K inmiyor": holds TX low, then high, long enough to follow the level with a multimeter
 * (KL_TX → LV1/HV1 → L9637D pin 4 → pin 6 K → pin 1 → HV2/LV2 → KL_RX). Shows what RX reads.
 */
static void kl_hold_test(void)
{
    for (int low = 1; low >= 0; low--) {
        srclog_step("ölç · TX %s 5 sn", low ? "LOW" : "HIGH");
        kl_drive_low(low);
        vTaskDelay(pdMS_TO_TICKS(500));
        const int tx = gpio_get_level(KL_TX), rx = gpio_get_level(KL_RX);
        ESP_LOGI(TAG, "K ölç TX %s: TX pad %d, RX %d", low ? "LOW " : "HIGH", tx, rx);
        srclog_done(rx != low && tx != low ? SRCLOG_OK : SRCLOG_FAIL, "TX%d RX%d", tx, rx);
        vTaskDelay(pdMS_TO_TICKS(4500));
    }
}

// ---------------------------------------------------------------------------
// Protocol independent part
// ---------------------------------------------------------------------------
static bool detect(void)
{
    srclog_step("k-line hızlı init");
    if (kl_fast_init()) {
        srclog_done(SRCLOG_OK, "KWP2000");
        s_baud = KL_BAUD;
        return true;
    }
    static const char *const DIAG[] = {"ECU sessiz", "K hep LOW", "K inmiyor", "echo yok", "TX inmiyor"};
    srclog_done(SRCLOG_FAIL, "%s", DIAG[s_kl_diag]);
    if (s_kl_diag == KL_DIAG_NO_LOW || s_kl_diag == KL_DIAG_TX_STUCK) {
        kl_hold_test();
    }
    srclog_step("k-line 5 baud init");
    if (kl_slow_init()) {
        srclog_done(SRCLOG_OK, s_mode == VEMS_MODE_OBD_ISO9141 ? "ISO 9141" : "KWP2000");
        s_baud = KL_BAUD;
        return true;
    }
    srclog_done(SRCLOG_FAIL, "YOK");
    s_mode = VEMS_MODE_NONE;
    return false;
}

static void drop_link(void)
{
    vems_mode_t m = s_mode;
    s_mode = VEMS_MODE_NONE;
    if (m != VEMS_MODE_NONE) {
        vTaskDelay(pdMS_TO_TICKS(KL_P3MAX_MS));
    }
}

static int obd_request(uint8_t pid, uint8_t *out)
{
    int n = kl_request(pid, out);
    if (n < 0) {
        s_stats.timeouts++;
    }
    return n;
}

static bool read_supported(void)
{
    uint8_t d[DATA_MAX];
    memset(s_sup, 0, sizeof(s_sup));
    for (int k = 0; k < 3; k++) {
        if (k > 0 && !pid_supported(k * 0x20)) {
            break;
        }
        if (obd_request(k * 0x20, d) != 4) {
            return k > 0;
        }
        s_sup[k] = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
    }
    return true;
}

/** Channels one PID feeds. */
static uint32_t pid_channels(uint8_t pid)
{
    switch (pid) {
    case PID_SPEED:         return CH_BIT(CH_SPEED);
    case PID_MAP:           return CH_BIT(CH_MAP) | CH_BIT(CH_BOOST);
    case PID_O2S1_WR_V:
    case PID_O2S1_WR_I:     return CH_BIT(CH_AFR);
    case PID_O2S1:          return CH_BIT(CH_O2V);
    case PID_CLT:           return CH_BIT(CH_CLT);
    case PID_IAT:           return CH_BIT(CH_IAT);
    case PID_VBAT:          return CH_BIT(CH_BATT);
    case PID_TPS:           return CH_BIT(CH_TPS);
    case PID_LOAD:          return CH_BIT(CH_LOAD);
    case PID_ADV:           return CH_BIT(CH_ADV);
    case PID_OIL:           return CH_BIT(CH_OIL);
    case PID_FUEL_LVL:      return CH_BIT(CH_FUEL);
    case PID_LAMBDA_CMD:    return CH_BIT(CH_TARGET);
    case PID_STFT1:         return CH_BIT(CH_STFT);
    case PID_LTFT1:         return CH_BIT(CH_LTFT);
    case PID_MAF:           return CH_BIT(CH_MAF);
    case PID_O2S2:          return CH_BIT(CH_O2V2);
    case PID_FUEL_RATE:     return CH_BIT(CH_FUEL_LH) | CH_BIT(CH_FUEL_L100);
    default:                return 0;
    }
}

static bool fast_default(uint8_t pid)
{
    return memchr(FAST_PIDS, pid, sizeof(FAST_PIDS)) != NULL;
}

/** Tüketim hesabının girdileri: MAF varsa o, yoksa MAP + IAT (devir her zaman sorgulanır). */
#define FUEL_INPUTS     (CH_BIT(CH_MAF) | CH_BIT(CH_MAP) | CH_BIT(CH_IAT) | CH_BIT(CH_SPEED) | CH_BIT(CH_AFR))
#define FUEL_CHANNELS   (CH_BIT(CH_FUEL_LH) | CH_BIT(CH_FUEL_L100))

/**
 * Araç profilindeki (desteklenen + tabloda olan) her PID bir listeye girer; RPM ayrıca her ikinci istekte.
 * want = 0: FAST_PIDS hızlı turda. want != 0: ekranda görünen kanalları besleyen PID'ler hızlı turda.
 * Diğerleri yavaş turda (hızlı turun her dönüşünde bir tane). OPF_ONCE PID'ler bağlantıda bir kez okunur.
 */
static void build_poll_lists(uint32_t want)
{
    s_built_want = want;
    if (want & FUEL_CHANNELS) {
        want |= FUEL_INPUTS;
    }
    s_nfast = s_nslow = s_fi = s_si = 0;
    const bool wideband = pid_supported(PID_O2S1_WR_V) || pid_supported(PID_O2S1_WR_I);
    for (uint8_t pid = 1; pid <= PID_LAST; pid++) {
        const obd_pid_def_t *def = obd_pid_def(pid);
        if (pid == PID_RPM || !pid_supported(pid) || def == NULL || (def->flags & OPF_ONCE)) {
            continue;
        }
        // aynı değerin ikinci kaynağı hızlı tura girmez (geniş bant varken dar bant, akım varken voltaj)
        const bool dup = (pid == PID_O2S1_WR_I && pid_supported(PID_O2S1_WR_V)) || (pid == PID_O2S1 && wideband);
        if (!dup && (want ? (pid_channels(pid) & want) != 0 : fast_default(pid))) {
            s_fast[s_nfast++] = pid;
        } else {
            s_slow[s_nslow++] = pid;
        }
    }
}

/** What the dash can show with the polled PIDs. */
static uint32_t polled_channels(void)
{
    uint32_t m = pid_supported(PID_RPM) ? CH_BIT(CH_RPM) : 0;
    for (int i = 0; i < s_nfast + s_nslow; i++) {
        m |= pid_channels(i < s_nfast ? s_fast[i] : s_slow[i - s_nfast]);
    }
    if ((m & (CH_BIT(CH_MAF) | CH_BIT(CH_FUEL_LH))) ||
        (m & (CH_BIT(CH_MAP) | CH_BIT(CH_IAT))) == (CH_BIT(CH_MAP) | CH_BIT(CH_IAT))) {
        m |= CH_BIT(CH_FUEL_LH);
        if (m & CH_BIT(CH_SPEED)) {
            m |= CH_BIT(CH_FUEL_L100);
        }
    }
    return m;
}

static uint8_t next_pid(void)
{
    if ((s_slot++ & 1) == 0 || (s_nfast == 0 && s_nslow == 0)) {
        return PID_RPM;
    }
    if (s_fi < s_nfast) {
        return s_fast[s_fi++];
    }
    if (s_want != s_built_want) {
        build_poll_lists(s_want);   // sayfa değişti: tur sonunda yeni ekranın PID'lerine geç
        if (s_nfast) {
            return s_fast[s_fi++];
        }
    }
    s_fi = 0;
    if (s_nslow == 0) {
        return s_fast[s_fi++];
    }
    uint8_t pid = s_slow[s_si];
    s_si = (s_si + 1) % s_nslow;
    return pid;
}

static void data_reset(vems_data_t *d)
{
    memset(d, 0, sizeof(*d));
    d->map_kpa = d->iat_c = d->clt_c = d->tps_pct = d->batt_v = NAN;
    d->lambda = d->afr = d->lambda2 = d->lambda_target = d->ego_corr_pct = NAN;
    d->ign_adv_deg = d->pulsewidth_ms = d->dwell_ms = d->load_pct = d->speed_kmh = NAN;
    d->ltft_pct = d->maf_gs = d->o2_v = d->runtime_s = d->fuel_pct = d->baro_kpa = d->oil_c = NAN;
    d->fuel_lh = d->fuel_l100 = d->o2b_v = d->fuel_rate_lh = NAN;
    d->mil_codes = -1;
}

#define FUEL_VE         0.80f       // hız-yoğunluk için varsayılan dolum verimi
#define FUEL_DENS_GL    745.0f      // benzin yoğunluğu g/L

/** Anlık tüketim: hava debisi (MAF ya da MAP/IAT/RPM ile hız-yoğunluk) / AFR -> L/saat ve L/100 km. */
static void fuel_calc(vems_data_t *d)
{
    float air;      // g/s
    if (!isnan(d->fuel_rate_lh)) {                  // ECU yakıt debisini kendisi veriyorsa (PID 5E) o
        d->fuel_lh = d->fuel_rate_lh;
        d->fuel_l100 = !isnan(d->speed_kmh) && d->speed_kmh > 5 ? d->fuel_lh / d->speed_kmh * 100.0f : NAN;
        return;
    }
    if (!isnan(d->maf_gs)) {
        air = d->maf_gs;
    } else if (!isnan(d->map_kpa) && !isnan(d->iat_c)) {
        // hava yoğunluğu kPa / (0.287 · K) kg/m³ = g/L, emme başına hacim · devir / 120
        air = d->map_kpa / (0.287f * (d->iat_c + 273.15f)) * (CONFIG_VEMS_ENGINE_CC / 1000.0f) * d->rpm / 120.0f * FUEL_VE;
    } else {
        return;
    }
    if (d->rpm < 300) {
        air = 0;
    }
    const float afr = !isnan(d->afr) && d->afr > 8 ? d->afr : 14.7f;
    d->fuel_lh = air / afr / FUEL_DENS_GL * 3600.0f;
    d->fuel_l100 = !isnan(d->speed_kmh) && d->speed_kmh > 5 ? d->fuel_lh / d->speed_kmh * 100.0f : NAN;
}

static void decode(vems_data_t *d, uint8_t pid, const uint8_t *v, int n)
{
    if (n < pid_len(pid)) {
        return;
    }
    const uint16_t ab = n >= 2 ? (uint16_t)((v[0] << 8) | v[1]) : 0;
    switch (pid) {
    case PID_RPM:
        d->rpm = ab / 4;
        if (d->rpm > 300) {
            d->engine_status |= VEMS_ENG_RUNNING;
        } else {
            d->engine_status &= ~VEMS_ENG_RUNNING;
        }
        break;
    case PID_MAP:           d->map_kpa = v[0]; break;
    case PID_TPS:           d->tps_pct = v[0] * 100.0f / 255.0f; break;
    case PID_CLT:           d->clt_c = v[0] - 40.0f; break;
    case PID_IAT:           d->iat_c = v[0] - 40.0f; break;
    case PID_VBAT:          d->batt_v = ab / 1000.0f; break;
    case PID_ADV:           d->ign_adv_deg = v[0] * 0.5f - 64.0f; break;
    case PID_LOAD:          d->load_pct = v[0] * 100.0f / 255.0f; break;
    case PID_SPEED:         d->speed_kmh = v[0]; break;
    case PID_STFT1:         d->ego_corr_pct = 100.0f + (v[0] - 128) * 100.0f / 128.0f; break;   // VEMS style, 100 = none
    case PID_LAMBDA_CMD:    d->lambda_target = ab * 2.0f / 65536.0f; break;
    case PID_LTFT1:         d->ltft_pct = (v[0] - 128) * 100.0f / 128.0f; break;
    case PID_MAF:           d->maf_gs = ab / 100.0f; break;
    case PID_O2S1:          d->o2_v = v[0] / 200.0f; break;
    case PID_FUEL_LVL:      d->fuel_pct = v[0] * 100.0f / 255.0f; break;
    case PID_OIL:           d->oil_c = v[0] - 40.0f; break;
    case PID_O2S2:          d->o2b_v = v[0] / 200.0f; break;
    case PID_FUEL_RATE:     d->fuel_rate_lh = ab / 20.0f; break;
    case PID_MIL:
        d->mil_on = v[0] & 0x80;
        d->mil_codes = v[0] & 0x7F;
        break;
    case PID_O2S1_WR_V:
    case PID_O2S1_WR_I:
        d->lambda = ab * 2.0f / 65536.0f;
        d->afr = d->lambda * 14.7f;
        break;
    case PID_FUEL_SYS:      // fuel system 1 closed loop -> VEMS "CL" flag
        if (v[0] & 0x02) {
            d->status1 |= 0x04;
        } else {
            d->status1 &= ~0x04;
        }
        break;
    }
}

/** Profil için ham cevabı ve iki cevap arası süreyi saklar. */
static void profile_store(uint8_t pid, const uint8_t *v, int n, int64_t now)
{
    if (pid > PID_LAST) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_raw[pid], 0, sizeof(s_raw[pid]));
    memcpy(s_raw[pid], v, n < (int)sizeof(s_raw[pid]) ? n : (int)sizeof(s_raw[pid]));
    s_period_ms[pid] = s_rx_us[pid] ? (uint32_t)((now - s_rx_us[pid]) / 1000) : 0;
    s_rx_us[pid] = now;
    xSemaphoreGive(s_lock);
}

static void profile_reset(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_rx_us, 0, sizeof(s_rx_us));
    memset(s_period_ms, 0, sizeof(s_period_ms));
    xSemaphoreGive(s_lock);
}

static void pid_unsupport(uint8_t pid)
{
    s_sup[(pid - 1) / 32] &= ~(1u << (31 - (pid - 1) % 32));
}

static void obd_task(void *arg)
{
    static vems_data_t cur;
    static uint8_t pid_fail[PID_LAST + 1];  // art arda cevapsız kalan PID'ler profilden düşer
    uint8_t v[DATA_MAX];
    uint32_t seq = 0;

    while (1) {
        if (s_rescan) {
            s_rescan = false;
            srclog_clear();
        }
        s_state = VEMS_STATE_WAIT_LINK;
        if (!detect()) {
            srclog_step("ecu bulunamadı");
            srclog_done(SRCLOG_FAIL, "2 SN");
            vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
            continue;
        }
        srclog_step("pid taraması · 01 00/20/40");
        if (!read_supported()) {
            ESP_LOGW(TAG, "%s: no answer to 01 00", vems_mode_name(s_mode));
            srclog_done(SRCLOG_FAIL, "CEVAP YOK");
            drop_link();
            continue;
        }
        build_poll_lists(0);            // önce tam liste: kanal maskesi her desteklenen PID'i görsün
        uint32_t ch = polled_channels();
        s_slot = 0;
        srclog_done(SRCLOG_OK, "%d VERİ", __builtin_popcount(ch));
        ESP_LOGI(TAG, "ECU found: %s, supported PIDs %08lX %08lX %08lX, polling %d fast + %d slow + RPM",
                 vems_mode_name(s_mode), (unsigned long)s_sup[0], (unsigned long)s_sup[1], (unsigned long)s_sup[2],
                 s_nfast, s_nslow);
        data_reset(&cur);
        profile_reset();
        memset(pid_fail, 0, sizeof(pid_fail));
        // değişmeyen bilgiler (OBD standardı, O2 sensör haritası, yakıt tipi): bir kez
        for (uint8_t pid = 1; pid <= PID_LAST; pid++) {
            const obd_pid_def_t *def = obd_pid_def(pid);
            if (def && (def->flags & OPF_ONCE) && pid_supported(pid)) {
                const int n = obd_request(pid, v);
                if (n >= 0) {
                    profile_store(pid, v, n, esp_timer_get_time());
                }
            }
        }
        s_channels = ch;
        s_state = VEMS_STATE_LINK_UP;

        int fails = 0;
        s_kl_quiet = true;
        while (fails < MAX_FAILS && !s_rescan) {
            if (s_dtc_req) {
                s_kl_quiet = false;     // arıza kodu alışverişi logda görünsün
                s_ndtc = kl_dtc(s_dtc_req, s_dtc, VEMS_DTC_MAX);
                s_kl_quiet = true;
                ESP_LOGI(TAG, "DTC servis %02X: %d", s_dtc_req, s_ndtc);
                s_dtc_req = 0;
                xSemaphoreGive(s_dtc_done);
                continue;
            }
            uint8_t pid = next_pid();
            int n = obd_request(pid, v);
            if (n < 0) {
                fails++;
                // desteklenir dediği halde cevap vermeyen PID her turda P2 (~210 ms) yakar: 3 kezde çıkar
                if (pid != PID_RPM && pid <= PID_LAST && ++pid_fail[pid] >= 3) {
                    ESP_LOGW(TAG, "PID %02X cevap vermiyor, profilden çıkarıldı", pid);
                    pid_unsupport(pid);
                    build_poll_lists(s_built_want);
                }
                continue;
            }
            fails = 0;
            const int64_t now = esp_timer_get_time();
            if (pid <= PID_LAST) {
                pid_fail[pid] = 0;
            }
            profile_store(pid, v, n, now);
            decode(&cur, pid, v, n);
            fuel_calc(&cur);
            cur.seq = ++seq;
            cur.timestamp_us = now;
            cur.mode = s_mode;
            cur.baud = s_baud;

            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_data = cur;
            s_have_data = true;
            s_stats.frames_ok++;
            xSemaphoreGive(s_lock);
            s_state = VEMS_STATE_ECU_OK;
        }
        s_kl_quiet = false;
        s_channels = 0;
        if (!s_rescan) {
            ESP_LOGW(TAG, "ECU stopped answering (ignition off?), searching again");
            srclog_step("ecu cevap vermiyor");
            srclog_done(SRCLOG_FAIL, "YENİDEN");
            s_stats.resyncs++;
        }
        drop_link();
    }
}

esp_err_t obd_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_dtc_done = xSemaphoreCreateBinary();
    if (s_lock == NULL || s_dtc_done == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "K-Line on UART%d TX GPIO%d RX GPIO%d", KL_PORT, KL_TX, KL_RX);
    ESP_RETURN_ON_ERROR(kline_start(), TAG, "K-Line");
    srclog_step("arayüz K-Line");
    srclog_done(SRCLOG_OK, "OK");
    s_state = VEMS_STATE_WAIT_LINK;
    // çekirdek 1, ekran görevinden yüksek öncelik: K-Line init zamanlamasını çizim bozmamalı
    return xTaskCreatePinnedToCore(obd_task, "obd", 4096, NULL, 5, NULL, 1) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

// ---------------------------------------------------------------------------
// vems_proto.h getters
// ---------------------------------------------------------------------------
vems_state_t vems_get_state(void)
{
    return s_state;
}

bool vems_get_latest(vems_data_t *out)
{
    if (s_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_have_data;
    if (ok) {
        *out = s_data;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void vems_get_stats(vems_stats_t *out)
{
    *out = s_stats;
}

const char *vems_mode_name(vems_mode_t mode)
{
    switch (mode) {
    case VEMS_MODE_OBD_KWP_FAST:    return "KWP FAST";
    case VEMS_MODE_OBD_KWP_5BAUD:   return "KWP 5BAUD";
    case VEMS_MODE_OBD_ISO9141:     return "ISO 9141";
    default:                        return "none";
    }
}

uint32_t vems_get_channels(void)
{
    return s_channels;
}

void vems_get_link_info(vems_link_info_t *out)
{
    const vems_mode_t m = s_mode;
    memset(out, 0, sizeof(*out));
    out->obd = true;
    out->protocol = vems_mode_name(m);
    switch (m) {
    case VEMS_MODE_OBD_KWP_FAST:
    case VEMS_MODE_OBD_KWP_5BAUD:
    case VEMS_MODE_OBD_ISO9141:
        out->standard = m == VEMS_MODE_OBD_ISO9141 ? "ISO 9141-2" : m == VEMS_MODE_OBD_KWP_FAST
                        ? "ISO 14230-4 · hızlı init" : "ISO 14230-4 · 5 baud";
        snprintf(out->ecu, sizeof(out->ecu), "0x%02X", s_kl_ecu);
        snprintf(out->line, sizeof(out->line), "%d baud", KL_BAUD);
        break;
    default:
        out->protocol = "ARANIYOR";
        out->standard = "K-Line";
        strcpy(out->ecu, "--");
        strcpy(out->line, "--");
        break;
    }
}

void vems_rescan(void)
{
    s_rescan = true;
}

void vems_set_wanted_channels(uint32_t mask)
{
    s_want = mask;
}

int vems_get_profile(vems_pid_info_t *out, int max)
{
    if (s_channels == 0) {
        return 0;
    }
    const int64_t now = esp_timer_get_time();
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint8_t pid = 1; pid <= PID_LAST && n < max; pid++) {
        if (pid % 0x20 == 0 || !pid_supported(pid)) {
            continue;
        }
        const obd_pid_def_t *def = obd_pid_def(pid);
        vems_pid_info_t *p = &out[n++];
        p->pid = pid;
        p->name = def ? def->name : "Tanımsız PID";
        p->unit = def ? def->unit : "";
        p->once = def && (def->flags & OPF_ONCE);
        p->age_ms = s_rx_us[pid] ? (uint32_t)((now - s_rx_us[pid]) / 1000) : UINT32_MAX;
        p->period_ms = s_period_ms[pid];
        if (def && s_rx_us[pid]) {
            obd_pid_format(pid, s_raw[pid], p->text, sizeof(p->text));
        } else {
            strcpy(p->text, def ? "--" : "sorgulanmıyor");
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}

/** İsteği OBD görevine verip cevabı bekler: -1 bağlantı yok / ECU cevap vermedi. */
static int dtc_call(uint8_t sid, uint16_t *codes, int max)
{
    if (s_state != VEMS_STATE_ECU_OK) {
        return -1;
    }
    xSemaphoreTake(s_dtc_done, 0);              // önceki zaman aşımından kalan sinyali at
    s_dtc_req = sid;
    if (xSemaphoreTake(s_dtc_done, pdMS_TO_TICKS(4000)) != pdTRUE) {
        s_dtc_req = 0;
        return -1;
    }
    const int n = s_ndtc < max ? s_ndtc : max;
    if (codes && n > 0) {
        memcpy(codes, s_dtc, n * sizeof(codes[0]));
    }
    return n;
}

int vems_dtc_read(uint16_t *codes, int max)
{
    return dtc_call(SID_DTC_READ, codes, max);
}

bool vems_dtc_clear(void)
{
    return dtc_call(SID_DTC_CLEAR, NULL, 0) >= 0;
}
