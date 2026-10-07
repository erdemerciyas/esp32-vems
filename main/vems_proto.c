/*
 * VEMS v3 realtime data protocol.
 *
 * Sources:
 *  - http://www.vems.hu/wiki/index.php?page=SerialComm%2FTriggerFrameFormat
 *    TriggerFrame: request 7E A0 BF 38 7E (READ_REALTIME_STUFFED_TYPE_COMMAND = 0xA0 + Modbus CRC16, LE).
 *    Reply: HDLC_bytestuffed(payload, type=0x20, CRC16_LE(payload+type)) 7E, payload = MTsendRTvar() bytes.
 *    HDLC stuffing: 7E -> 7D 5E, 7D -> 7D 5D.
 *    "Leave TF mode": 7E B1 7F 34 7E.
 *  - VEMS firmware comm.c MTsendRTvar() ("A" -> 56 bytes, big-endian U16).
 *  - Scaling: VemsTune 2016-06-06 config/vemsTune-v3-1.2.15.ini [OutputChannels] (1.2.16 uses the 1.2.15 ini).
 */
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "vems_proto.h"

static const char *TAG = "vems";

#define HDLC_FLAG           0x7E
#define HDLC_ESC            0x7D
#define HDLC_XOR            0x20
#define TF_TYPE_RT_STUFFED  0x20

#define TF_TIMEOUT_MS       300
#define A_TIMEOUT_MS        300
#define A_TRAILING_MS       20
#define MAX_FAILS           5

static const uint8_t TF_REQ_REALTIME[] = {HDLC_FLAG, 0xA0, 0xBF, 0x38, HDLC_FLAG};
static const uint8_t TF_LEAVE[] = {HDLC_FLAG, 0xB1, 0x7F, 0x34, HDLC_FLAG};
static const uint8_t A_REQ[] = {'A'};

#if CONFIG_VEMS_AUTO_BAUD
static const uint32_t SCAN_BAUDS[] = {19200, 115200, 57600, 38400, 9600};
#endif

static const vems_link_t *s_link;
static SemaphoreHandle_t s_lock;
static vems_data_t s_data;
static bool s_have_data;
static vems_stats_t s_stats;
static uint32_t s_baud = CONFIG_VEMS_BAUD;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static uint16_t crc16_modbus(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
        }
    }
    return crc;
}

static inline uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

// VemsTune 1.2.x: lambda = (egoADC > 211) ? ((8 * egoADC - 1171) / 32 / 14.7) : (egoADC + 306) / 470
static inline float wbo2_lambda(uint8_t v)
{
    return (v > 211) ? ((8.0f * v - 1171.0f) / 32.0f / 14.7f) : ((v + 306.0f) / 470.0f);
}

static inline TickType_t ms_left(int64_t deadline_us)
{
    int64_t left = deadline_us - esp_timer_get_time();
    if (left <= 0) {
        return 0;
    }
    TickType_t t = pdMS_TO_TICKS(left / 1000);
    return t ? t : 1;
}

const char *vems_mode_name(vems_mode_t mode)
{
    switch (mode) {
    case VEMS_MODE_TF: return "TriggerFrame(0xA0)";
    case VEMS_MODE_A:  return "MegaTune 'A'";
    default:           return "none";
    }
}

static void dump_hex(const char *title, const uint8_t *p, size_t len)
{
    ESP_LOGI(TAG, "%s (%u bytes):", title, (unsigned)len);
    ESP_LOG_BUFFER_HEXDUMP(TAG, p, len, ESP_LOG_INFO);
}

// ---------------------------------------------------------------------------
// TriggerFrame request
// ---------------------------------------------------------------------------
/** Validates an unstuffed frame. Returns payload length for a realtime frame, -1 otherwise. */
static int tf_check(const uint8_t *frame, size_t flen, uint8_t *out, size_t out_max)
{
    if (flen < 3) {
        return -1;
    }
    uint16_t crc_rx = frame[flen - 2] | (frame[flen - 1] << 8);
    if (crc16_modbus(frame, flen - 2) != crc_rx) {
        s_stats.crc_errors++;
        ESP_LOGD(TAG, "TF CRC error (len %u)", (unsigned)flen);
        return -1;
    }
    uint8_t type = frame[flen - 3];
    if (type != TF_TYPE_RT_STUFFED) {
        ESP_LOGD(TAG, "TF frame type 0x%02X ignored", type);
        return -1;
    }
    size_t plen = flen - 3;
    memcpy(out, frame, plen < out_max ? plen : out_max);
    return (int)(plen < out_max ? plen : out_max);
}

static int tf_request(uint8_t *out, size_t out_max)
{
    uint8_t frame[VEMS_RAW_MAX + 8];
    uint8_t rx[64];
    size_t flen = 0;
    bool esc = false;
    bool overflow = false;

    s_link->flush_rx();
    if (s_link->write(TF_REQ_REALTIME, sizeof(TF_REQ_REALTIME), pdMS_TO_TICKS(200)) != ESP_OK) {
        return -1;
    }

    int64_t deadline = esp_timer_get_time() + TF_TIMEOUT_MS * 1000;
    TickType_t wait;
    while ((wait = ms_left(deadline)) > 0) {
        size_t n = s_link->read(rx, sizeof(rx), wait);
        for (size_t i = 0; i < n; i++) {
            uint8_t b = rx[i];
            if (b == HDLC_FLAG) {
                if (flen > 0 && !overflow) {
                    int r = tf_check(frame, flen, out, out_max);
                    if (r >= 0) {
                        return r;
                    }
                }
                flen = 0;
                esc = false;
                overflow = false;
                continue;
            }
            if (b == HDLC_ESC) {
                esc = true;
                continue;
            }
            if (esc) {
                b ^= HDLC_XOR;
                esc = false;
            }
            if (flen < sizeof(frame)) {
                frame[flen++] = b;
            } else {
                overflow = true;
            }
        }
    }
    s_stats.timeouts++;
    return -1;
}

// ---------------------------------------------------------------------------
// MegaTune 'A' request
// ---------------------------------------------------------------------------
static int a_request(uint8_t *out, bool strict)
{
    size_t got = 0;

    s_link->flush_rx();
    if (s_link->write(A_REQ, sizeof(A_REQ), pdMS_TO_TICKS(200)) != ESP_OK) {
        return -1;
    }

    int64_t deadline = esp_timer_get_time() + A_TIMEOUT_MS * 1000;
    TickType_t wait;
    while (got < VEMS_A_LEN && (wait = ms_left(deadline)) > 0) {
        got += s_link->read(out + got, VEMS_A_LEN - got, wait);
    }
    if (got < VEMS_A_LEN) {
        s_stats.timeouts++;
        return -1;
    }
    if (strict) {
        // A real 'A' answer is exactly 56 bytes; anything trailing means we talk to something else
        uint8_t extra[16];
        if (s_link->read(extra, sizeof(extra), pdMS_TO_TICKS(A_TRAILING_MS)) > 0) {
            ESP_LOGD(TAG, "'A' answer longer than %d bytes", VEMS_A_LEN);
            return -1;
        }
    }
    return VEMS_A_LEN;
}

// ---------------------------------------------------------------------------
// Detection
// ---------------------------------------------------------------------------
static vems_mode_t probe_modes(uint8_t *buf)
{
#if !CONFIG_VEMS_PROTOCOL_A
    for (int i = 0; i < 2; i++) {
        if (tf_request(buf, VEMS_RAW_MAX) >= 0) {
            return VEMS_MODE_TF;
        }
    }
#endif
#if !CONFIG_VEMS_PROTOCOL_TF
#if CONFIG_VEMS_PROTOCOL_AUTO
    // ECU might still be in TF mode (e.g. VemsTune was connected): ask it to go back to compat mode
    s_link->write(TF_LEAVE, sizeof(TF_LEAVE), pdMS_TO_TICKS(200));
    vTaskDelay(pdMS_TO_TICKS(50));
#endif
    for (int i = 0; i < 2; i++) {
        if (a_request(buf, true) == VEMS_A_LEN) {
            return VEMS_MODE_A;
        }
    }
#endif
    return VEMS_MODE_NONE;
}

static vems_mode_t detect(uint8_t *buf)
{
    s_baud = CONFIG_VEMS_BAUD;
    s_link->set_baud(s_baud);
    vTaskDelay(pdMS_TO_TICKS(20));
    vems_mode_t mode = probe_modes(buf);
    if (mode != VEMS_MODE_NONE) {
        return mode;
    }

#if CONFIG_VEMS_AUTO_BAUD && !CONFIG_VEMS_PROTOCOL_A
    // Only the CRC protected TF request is used at foreign baud rates (no stray single-byte commands)
    for (size_t i = 0; i < sizeof(SCAN_BAUDS) / sizeof(SCAN_BAUDS[0]); i++) {
        if (SCAN_BAUDS[i] == CONFIG_VEMS_BAUD) {
            continue;
        }
        s_baud = SCAN_BAUDS[i];
        s_link->set_baud(s_baud);
        vTaskDelay(pdMS_TO_TICKS(20));
        if (tf_request(buf, VEMS_RAW_MAX) >= 0) {
            return VEMS_MODE_TF;
        }
    }
    s_baud = CONFIG_VEMS_BAUD;
    s_link->set_baud(s_baud);
#endif
    return VEMS_MODE_NONE;
}

// ---------------------------------------------------------------------------
// Decoding (MegaTune 'A' layout, vemsv3.ini / comm.c MTsendRTvar)
// ---------------------------------------------------------------------------
static void decode(vems_data_t *d, const uint8_t *payload, size_t len)
{
    uint8_t r[VEMS_RAW_MAX] = {0};
    size_t n = len < VEMS_RAW_MAX ? len : VEMS_RAW_MAX;
    memcpy(r, payload, n);
    memcpy(d->raw, r, VEMS_RAW_MAX);
    d->raw_len = n;

    d->secl = r[0];
    d->boost_dc_alt_pct = r[1] * 100.0f / 255.0f;
    d->engine_status = r[2];
    d->kpa_tps_blend = r[3];
    d->map_kpa = be16(&r[4]) / 4.0f;
    d->iat_c = (float)r[6] - 100.0f;
    d->clt_c = (float)r[7] - 100.0f;
    d->tps_pct = r[8] * 100.0f / 255.0f;
    d->batt_v = r[9] * 30.0f / 255.0f;
    d->ego_raw = r[10];
    d->lambda = wbo2_lambda(r[10]);
    d->afr = d->lambda * 14.7f;
    d->ego_corr_pct = r[11];
    d->ego2_raw = r[12];
    d->lambda2 = wbo2_lambda(r[12]);
    d->warmup_pct = (r[13] + 45) * 2.0f;
    d->rpm = be16(&r[14]);
    d->pulsewidth_ms = be16(&r[16]) * 0.004f;
    d->baro_corr_pct = (r[18] + 272) * 0.25f;
    d->gamma_pct = r[19] * 0.99f;
    d->ve = r[20];
    d->dwell_ms = r[21] * 0.064f;
    d->ign_adv_deg = r[22] * 0.5f - 64.0f;
    d->iac_pct = r[23] * 100.0f / 255.0f;
    d->egt1_raw = be16(&r[24]);
    d->egt2_raw = be16(&r[26]);
    d->lambda_target = 256.0f / (r[40] + 200);
    d->status1 = r[47];
    d->boost_target_kpa = r[48] * 4.0f;
    d->boost_dc_pct = r[49] * 100.0f / 255.0f;
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------
static void vems_task(void *arg)
{
    static uint8_t payload[VEMS_RAW_MAX];
    static vems_data_t tmp;
    vems_mode_t mode = VEMS_MODE_NONE;
    int fails = 0;
    int dumped = 0;
    uint32_t seq = 0;
    bool waiting_logged = false;

    while (1) {
        if (!s_link->is_ready()) {
            if (!waiting_logged) {
                ESP_LOGI(TAG, "waiting for %s link...", s_link->name);
                waiting_logged = true;
            }
            mode = VEMS_MODE_NONE;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        waiting_logged = false;

        if (mode == VEMS_MODE_NONE) {
            ESP_LOGI(TAG, "probing VEMS at %d baud...", CONFIG_VEMS_BAUD);
            mode = detect(payload);
            if (mode == VEMS_MODE_NONE) {
                ESP_LOGW(TAG, "no answer from VEMS (check ECU power / cable / baud); retrying");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            ESP_LOGI(TAG, "VEMS connected: protocol %s, %lu baud", vems_mode_name(mode), (unsigned long)s_baud);
            fails = 0;
        }

        int len = (mode == VEMS_MODE_TF) ? tf_request(payload, sizeof(payload)) : a_request(payload, false);
        if (len > 0) {
            fails = 0;
            if (dumped < CONFIG_VEMS_DUMP_RAW_FRAMES) {
                dumped++;
                dump_hex(mode == VEMS_MODE_TF ? "TF realtime payload" : "'A' realtime payload", payload, len);
            }
            decode(&tmp, payload, len);
            tmp.seq = ++seq;
            tmp.timestamp_us = esp_timer_get_time();
            tmp.mode = mode;
            tmp.baud = s_baud;

            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_data = tmp;
            s_have_data = true;
            s_stats.frames_ok++;
            xSemaphoreGive(s_lock);
        } else if (++fails >= MAX_FAILS) {
            ESP_LOGW(TAG, "%d consecutive failures, re-detecting", fails);
            s_stats.resyncs++;
            mode = VEMS_MODE_NONE;
        }

        vTaskDelay(CONFIG_VEMS_POLL_INTERVAL_MS ? pdMS_TO_TICKS(CONFIG_VEMS_POLL_INTERVAL_MS) : 1);
    }
}

esp_err_t vems_proto_start(const vems_link_t *link)
{
    s_link = link;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = link->start(CONFIG_VEMS_BAUD);
    if (err != ESP_OK) {
        return err;
    }
    return xTaskCreatePinnedToCore(vems_task, "vems", 6144, NULL, 5, NULL, 1) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool vems_get_latest(vems_data_t *out)
{
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
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_stats;
    xSemaphoreGive(s_lock);
}
