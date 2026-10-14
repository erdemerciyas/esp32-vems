/*
 * OBD-II data source: reads the standard mode 01 PIDs from the diagnostic socket of any car and
 * fills the same vems_data_t the dashboard shows for VEMS (vems_get_latest() & co. live here).
 *
 * Detection, repeated until an ECU answers:
 *  1. CAN, ISO 15765-4, 11 bit ids (OBD pin 6 CAN-H / 14 CAN-L). The bit rate is first looked for
 *     in listen-only mode, so a running vehicle bus never sees frames, ACKs or error frames at a
 *     wrong rate. A silent bus (OBD port behind a gateway) is then asked at 500 and 250 kbit/s.
 *  2. K-Line (OBD pin 7, L9637D): ISO 14230-4 fast init, then 5 baud init (ISO 9141-2 or
 *     ISO 14230-4, told apart by keyword 2).
 * The first ECU answering 01 00 is polled alone, only for the PIDs it reports as supported.
 * RPM is asked every other request, the rest round robin (slow PIDs once per fast round).
 *
 * Read only: OBD ECUs only talk when asked, so requests are unavoidable, but the only service ever
 * sent is 01 (show current data) plus the K-Line wake-up handshake (5 baud address, ~KW2,
 * StartCommunication 81). Nothing that clears codes, runs tests or writes is in this file;
 * obd_tx_allowed() rejects any other service id before it reaches the bus.
 */
#include <math.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "vems_proto.h"
#if CONFIG_VEMS_OBD_CAN
#include "can_hw.h"
#endif
#if CONFIG_VEMS_OBD_KLINE
#include "driver/gpio.h"
#include "driver/uart.h"
#endif

#if !CONFIG_VEMS_OBD_CAN && !CONFIG_VEMS_OBD_KLINE
#error "OBD-II: enable CAN and/or K-Line in menuconfig"
#endif

#if CONFIG_VEMS_OBD_CAN && CONFIG_VEMS_CAN_HW_TWAI && CONFIG_VEMS_OBD_KLINE
#if CONFIG_VEMS_CAN_TWAI_TX_GPIO == CONFIG_VEMS_OBD_KLINE_TX_GPIO || CONFIG_VEMS_CAN_TWAI_TX_GPIO == CONFIG_VEMS_OBD_KLINE_RX_GPIO || \
    CONFIG_VEMS_CAN_TWAI_RX_GPIO == CONFIG_VEMS_OBD_KLINE_TX_GPIO || CONFIG_VEMS_CAN_TWAI_RX_GPIO == CONFIG_VEMS_OBD_KLINE_RX_GPIO
#error "TWAI and K-Line share a GPIO: move the TWAI pins (e.g. SD slot GPIO2 / GPIO1) or use the MCP2515"
#endif
#endif

static const char *TAG = "obd";

#define MAX_FAILS           5
#define RETRY_MS            2000
#define DATA_MAX            8

#define PID_FUEL_SYS        0x03
#define PID_LOAD            0x04
#define PID_CLT             0x05
#define PID_STFT1           0x06
#define PID_MAP             0x0B
#define PID_RPM             0x0C
#define PID_SPEED           0x0D
#define PID_ADV             0x0E
#define PID_IAT             0x0F
#define PID_TPS             0x11
#define PID_O2S1_WR_V       0x24    // wide range O2 B1S1: lambda + voltage
#define PID_O2S1_WR_I       0x34    // wide range O2 B1S1: lambda + current
#define PID_VBAT            0x42
#define PID_LAMBDA_CMD      0x44

static const uint8_t FAST_PIDS[] = {PID_MAP, PID_TPS, PID_O2S1_WR_V, PID_O2S1_WR_I, PID_ADV, PID_LOAD, PID_SPEED};
static const uint8_t SLOW_PIDS[] = {PID_CLT, PID_IAT, PID_VBAT, PID_STFT1, PID_LAMBDA_CMD, PID_FUEL_SYS};

static SemaphoreHandle_t s_lock;
static vems_data_t s_data;
static bool s_have_data;
static vems_stats_t s_stats;
static volatile vems_state_t s_state = VEMS_STATE_STOPPED;
static volatile vems_mode_t s_mode = VEMS_MODE_NONE;
static uint32_t s_baud;
static uint32_t s_sup[3];           // supported PIDs 01-20, 21-40, 41-60 (01 00 / 01 20 / 01 40 bitmaps)
static uint8_t s_fast[sizeof(FAST_PIDS)], s_slow[sizeof(SLOW_PIDS)];
static int s_nfast, s_nslow, s_fi, s_si;
static unsigned s_slot;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
/** Data bytes of a mode 01 answer (after 41 PID). */
static int pid_len(uint8_t pid)
{
    switch (pid) {
    case 0x00: case 0x20: case 0x40:
    case PID_O2S1_WR_V: case PID_O2S1_WR_I:
        return 4;
    case PID_FUEL_SYS: case PID_RPM: case PID_VBAT: case PID_LAMBDA_CMD:
        return 2;
    default:
        return 1;
    }
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
#define SID_START_COMM      0x81    // KWP2000 StartCommunication (K-Line session open, no data)

static bool obd_tx_allowed(uint8_t sid)
{
    if (sid == SID_CURRENT_DATA || sid == SID_START_COMM) {
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
// CAN (ISO 15765-4, single frame requests and answers only)
// ---------------------------------------------------------------------------
#if CONFIG_VEMS_OBD_CAN
#define CAN_ID_FUNCTIONAL   0x7DF
#define CAN_LISTEN_MS       300
#define CAN_TIMEOUT_MS      100     // P2can 50 ms + margin
#define CAN_GAP_MS          10

static const uint16_t CAN_RESP_IDS[] = {0x7E8, 0x7E9, 0x7EA, 0x7EB, 0x7EC, 0x7ED};
static can_hw_config_t s_can_cfg;
static bool s_can_up;
static uint16_t s_can_ecu;          // answer id of the polled ECU, its request id is 8 lower
static int s_can_kbps;

static bool can_setup(int kbps, bool listen)
{
    s_can_cfg.kbps = kbps;
    s_can_cfg.listen_only = listen;
    s_can_cfg.rx_all = listen;
    s_can_up = can_hw_init(&s_can_cfg) == ESP_OK;
    return s_can_up;
}

static void can_off(void)
{
    if (s_can_up) {
        can_hw_go_offline();
        s_can_up = false;
    }
}

/** Sends 01 <pid> and waits for the single frame answer. Returns the data length after the PID, -1 on timeout. */
static int can_request(uint16_t tx_id, uint8_t pid, uint8_t *out, uint16_t *from)
{
    can_frame_t f;
    while (can_hw_recv(&f)) {
        // drop late answers of other ECUs
    }
    can_frame_t req = {.id = tx_id, .dlc = 8, .data = {0x02, SID_CURRENT_DATA, pid}};
    if (!obd_tx_allowed(req.data[1])) {
        return -1;
    }
    if (can_hw_send(&req) != ESP_OK) {
        return -1;
    }
    int64_t deadline = esp_timer_get_time() + CAN_TIMEOUT_MS * 1000;
    while (esp_timer_get_time() < deadline) {
        while (can_hw_recv(&f)) {
            uint8_t len = f.data[0];    // single frame: high nibble 0, length 2..7
            if (len < 2 || len > 7 || f.dlc < len + 1 || f.data[1] != 0x41 || f.data[2] != pid) {
                continue;
            }
            if (s_can_ecu && f.id != s_can_ecu) {
                continue;
            }
            if (from) {
                *from = f.id;
            }
            memcpy(out, &f.data[3], len - 2);
            return len - 2;
        }
        vTaskDelay(1);
    }
    return -1;
}

/** Bit rate with traffic on the bus (listen-only), 0 for a silent bus, -1 without a CAN controller. */
static int can_find_kbps(void)
{
    static const int RATES[] = {500, 250};
    for (int i = 0; i < 2; i++) {
        if (!can_setup(RATES[i], true)) {
            return -1;
        }
        int frames = 0;
        can_frame_t f;
        int64_t until = esp_timer_get_time() + CAN_LISTEN_MS * 1000;
        while (esp_timer_get_time() < until) {
            while (can_hw_recv(&f)) {
                frames++;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        can_off();
        if (frames >= 3) {
            return RATES[i];
        }
    }
    return 0;
}

static bool can_probe(int kbps)
{
    if (!can_setup(kbps, false)) {
        return false;
    }
    s_can_ecu = 0;
    uint8_t d[DATA_MAX];
    uint16_t from;
    for (int i = 0; i < 2; i++) {
        if (can_request(CAN_ID_FUNCTIONAL, 0x00, d, &from) == 4) {
            s_can_ecu = from;
            s_can_kbps = kbps;
            return true;
        }
    }
    can_off();
    return false;
}

static bool can_detect(void)
{
    static bool no_ctrl_logged;
    int kbps = can_find_kbps();
    if (kbps < 0) {
        if (!no_ctrl_logged) {
            ESP_LOGW(TAG, "CAN controller not found, only K-Line is tried");
            no_ctrl_logged = true;
        }
        return false;
    }
    no_ctrl_logged = false;
    if (kbps > 0) {
        ESP_LOGI(TAG, "CAN traffic at %d kbit/s", kbps);
        return can_probe(kbps);
    }
    return can_probe(500) || can_probe(250);
}
#endif

// ---------------------------------------------------------------------------
// K-Line (ISO 9141-2 / ISO 14230-4) over the L9637D, half duplex: every sent byte echoes back
// ---------------------------------------------------------------------------
#if CONFIG_VEMS_OBD_KLINE
#define KL_PORT         CONFIG_VEMS_OBD_KLINE_UART_PORT
#define KL_TX           CONFIG_VEMS_OBD_KLINE_TX_GPIO
#define KL_RX           CONFIG_VEMS_OBD_KLINE_RX_GPIO
#define KL_BAUD         10400
#define KL_P2_MS        150     // first answer byte (P2max 50 ms, some ECUs are late)
#define KL_IDLE_MS      60      // answers end when the bus is idle > P3min (55 ms): also the gap before the next request
#define KL_P4_MS        5       // tester inter-byte time
#define KL_ECHO_MS      10
#define KL_W5_MS        300     // idle bus before an init
#define KL_P3MAX_MS     5000    // ECU session timeout, waited before a new init

static uint8_t s_kl_ecu;        // source address of the polled ECU, 0 = not locked yet

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
    gpio_set_direction(KL_RX, GPIO_MODE_INPUT);     // GPIO43 is the console TX pad by default: never drive RX
    uart_set_rx_timeout(KL_PORT, 2);                // hand bytes over after 2 idle symbols, not 10
    return ESP_OK;
}

/** Init pulses: the inverted UART TX holds the line low while the transmitter idles. */
static void kl_drive_low(bool low)
{
    uart_set_line_inverse(KL_PORT, low ? UART_SIGNAL_TXD_INV : UART_SIGNAL_INV_DISABLE);
}

static void kl_send(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t echo;
        uart_write_bytes(KL_PORT, &p[i], 1);
        uart_wait_tx_done(KL_PORT, pdMS_TO_TICKS(20));
        uart_read_bytes(KL_PORT, &echo, 1, pdMS_TO_TICKS(KL_ECHO_MS));
        if (i + 1 < n) {
            vTaskDelay(pdMS_TO_TICKS(KL_P4_MS));
        }
    }
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
 * Finds the first valid frame of the polled ECU whose payload is <sid> [<pid>] in the received bytes.
 * pid < 0: no PID byte. Returns the data length after sid/pid, -1 if there is none.
 */
static int kl_find(const uint8_t *b, size_t n, uint8_t sid, int pid, uint8_t *out, uint8_t *src_out)
{
    size_t i = 0;
    while (i + 4 <= n) {
        size_t hdr, len;
        if (s_mode == VEMS_MODE_OBD_ISO9141) {
            if (b[i] != 0x48 || b[i + 1] != 0x6B || pid < 0) {
                i++;
                continue;
            }
            hdr = 3;
            len = 2 + pid_len(pid);     // no length byte: 41 PID data
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
        return dl;
    }
    return -1;
}

static int kl_request(uint8_t pid, uint8_t *out)
{
    uint8_t req[8], rx[64], src;
    const uint8_t data[] = {SID_CURRENT_DATA, pid};
    size_t n = kl_build(req, data, sizeof(data));
    uart_flush_input(KL_PORT);
    kl_send(req, n);
    int len = kl_find(rx, kl_receive(rx, sizeof(rx)), 0x41, pid, out, &src);
    if (len >= 0 && !s_kl_ecu) {
        s_kl_ecu = src;
    }
    return len;
}

static bool kl_fast_init(void)
{
    s_mode = VEMS_MODE_OBD_KWP_FAST;
    s_kl_ecu = 0;
    vTaskDelay(pdMS_TO_TICKS(KL_W5_MS));
    vTaskDelay(1);                              // start on a tick edge
    TickType_t t = xTaskGetTickCount();
    kl_drive_low(true);                         // TiniL 25 ms
    vTaskDelayUntil(&t, pdMS_TO_TICKS(25));
    kl_drive_low(false);                        // TWuP 50 ms
    vTaskDelayUntil(&t, pdMS_TO_TICKS(25));
    uart_flush_input(KL_PORT);

    static const uint8_t START_COMM[] = {SID_START_COMM};
    uint8_t req[8], rx[64], d[DATA_MAX], src;
    kl_send(req, kl_build(req, START_COMM, sizeof(START_COMM)));
    if (kl_find(rx, kl_receive(rx, sizeof(rx)), 0xC1, -1, d, &src) < 0) {
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
        return false;
    }
    if (uart_read_bytes(KL_PORT, &b[1], 2, pdMS_TO_TICKS(100)) != 2) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(30));              // W4 25..50 ms
    const uint8_t inv_kw2 = ~b[2];
    kl_send(&inv_kw2, 1);
    if (uart_read_bytes(KL_PORT, &ack, 1, pdMS_TO_TICKS(100)) != 1 || ack != (uint8_t)~0x33) {
        return false;
    }
    s_mode = b[2] == 0x8F ? VEMS_MODE_OBD_KWP_5BAUD : VEMS_MODE_OBD_ISO9141;
    ESP_LOGI(TAG, "K-Line 5 baud init OK, KW1 0x%02X KW2 0x%02X", b[1], b[2]);
    vTaskDelay(pdMS_TO_TICKS(KL_IDLE_MS));      // P3min before the first request
    return true;
}
#endif

// ---------------------------------------------------------------------------
// Protocol independent part
// ---------------------------------------------------------------------------
static bool detect(void)
{
#if CONFIG_VEMS_OBD_CAN
    if (can_detect()) {
        s_mode = VEMS_MODE_OBD_CAN;
        s_baud = s_can_kbps * 1000;
        return true;
    }
#endif
#if CONFIG_VEMS_OBD_KLINE
    if (kl_fast_init() || kl_slow_init()) {
        s_baud = KL_BAUD;
        return true;
    }
#endif
    s_mode = VEMS_MODE_NONE;
    return false;
}

static void drop_link(void)
{
    vems_mode_t m = s_mode;
    s_mode = VEMS_MODE_NONE;
#if CONFIG_VEMS_OBD_CAN
    if (m == VEMS_MODE_OBD_CAN) {
        can_off();
        return;
    }
#endif
#if CONFIG_VEMS_OBD_KLINE
    if (m != VEMS_MODE_NONE) {
        vTaskDelay(pdMS_TO_TICKS(KL_P3MAX_MS));
    }
#endif
}

static int obd_request(uint8_t pid, uint8_t *out)
{
    int n = -1;
#if CONFIG_VEMS_OBD_CAN
    if (s_mode == VEMS_MODE_OBD_CAN) {
        n = can_request(s_can_ecu - 8, pid, out, NULL);
        vTaskDelay(pdMS_TO_TICKS(CAN_GAP_MS));
    }
#endif
#if CONFIG_VEMS_OBD_KLINE
    if (s_mode != VEMS_MODE_OBD_CAN) {
        n = kl_request(pid, out);
    }
#endif
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

static void build_poll_lists(void)
{
    s_nfast = s_nslow = s_fi = s_si = 0;
    s_slot = 0;
    for (size_t i = 0; i < sizeof(FAST_PIDS); i++) {
        uint8_t pid = FAST_PIDS[i];
        if (pid_supported(pid) && !(pid == PID_O2S1_WR_I && pid_supported(PID_O2S1_WR_V))) {
            s_fast[s_nfast++] = pid;
        }
    }
    for (size_t i = 0; i < sizeof(SLOW_PIDS); i++) {
        if (pid_supported(SLOW_PIDS[i])) {
            s_slow[s_nslow++] = SLOW_PIDS[i];
        }
    }
}

static uint8_t next_pid(void)
{
    if ((s_slot++ & 1) == 0 || (s_nfast == 0 && s_nslow == 0)) {
        return PID_RPM;
    }
    if (s_fi < s_nfast) {
        return s_fast[s_fi++];
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

static void obd_task(void *arg)
{
    static vems_data_t cur;
    uint8_t v[DATA_MAX];
    uint32_t seq = 0;

    while (1) {
        s_state = VEMS_STATE_WAIT_LINK;
        if (!detect()) {
            vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
            continue;
        }
        if (!read_supported()) {
            ESP_LOGW(TAG, "%s: no answer to 01 00", vems_mode_name(s_mode));
            drop_link();
            continue;
        }
        build_poll_lists();
        ESP_LOGI(TAG, "ECU found: %s, supported PIDs %08lX %08lX %08lX, polling %d fast + %d slow + RPM",
                 vems_mode_name(s_mode), (unsigned long)s_sup[0], (unsigned long)s_sup[1], (unsigned long)s_sup[2],
                 s_nfast, s_nslow);
        s_state = VEMS_STATE_LINK_UP;
        data_reset(&cur);

        int fails = 0;
        while (fails < MAX_FAILS) {
            uint8_t pid = next_pid();
            int n = obd_request(pid, v);
            if (n < 0) {
                fails++;
                continue;
            }
            fails = 0;
            decode(&cur, pid, v, n);
            cur.seq = ++seq;
            cur.timestamp_us = esp_timer_get_time();
            cur.mode = s_mode;
            cur.baud = s_baud;

            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_data = cur;
            s_have_data = true;
            s_stats.frames_ok++;
            xSemaphoreGive(s_lock);
            s_state = VEMS_STATE_ECU_OK;
        }
        ESP_LOGW(TAG, "ECU stopped answering (ignition off?), searching again");
        s_stats.resyncs++;
        drop_link();
    }
}

esp_err_t obd_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_VEMS_OBD_CAN
#if CONFIG_VEMS_CAN_HW_TWAI
    s_can_cfg = (can_twai_config_t) {
        .tx_gpio = CONFIG_VEMS_CAN_TWAI_TX_GPIO,
        .rx_gpio = CONFIG_VEMS_CAN_TWAI_RX_GPIO,
        .rx_ids = CAN_RESP_IDS,
        .rx_id_count = sizeof(CAN_RESP_IDS) / sizeof(CAN_RESP_IDS[0]),
    };
#else
    s_can_cfg = (mcp2515_config_t) {
        .host = SPI3_HOST,
        .sclk_gpio = CONFIG_VEMS_CAN_SCLK_GPIO,
        .mosi_gpio = CONFIG_VEMS_CAN_MOSI_GPIO,
        .miso_gpio = CONFIG_VEMS_CAN_MISO_GPIO,
        .cs_gpio = CONFIG_VEMS_CAN_CS_GPIO,
        .spi_hz = CONFIG_VEMS_CAN_SPI_KHZ * 1000,
        .osc_mhz = CONFIG_VEMS_CAN_OSC_MHZ,
        .rx_ids = CAN_RESP_IDS,
        .rx_id_count = sizeof(CAN_RESP_IDS) / sizeof(CAN_RESP_IDS[0]),
    };
#endif
#endif
#if CONFIG_VEMS_OBD_KLINE
    ESP_LOGI(TAG, "K-Line on UART%d TX GPIO%d RX GPIO%d (serial console on these pins goes quiet)",
             KL_PORT, KL_TX, KL_RX);
    ESP_RETURN_ON_ERROR(kline_start(), TAG, "K-Line");
#endif
    s_state = VEMS_STATE_WAIT_LINK;
    return xTaskCreatePinnedToCore(obd_task, "obd", 4096, NULL, 5, NULL, 0) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
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
    case VEMS_MODE_OBD_CAN:         return s_baud == 250000 ? "CAN 250K" : "CAN 500K";
    case VEMS_MODE_OBD_KWP_FAST:    return "KWP FAST";
    case VEMS_MODE_OBD_KWP_5BAUD:   return "KWP 5BAUD";
    case VEMS_MODE_OBD_ISO9141:     return "ISO 9141";
    default:                        return "none";
    }
}
