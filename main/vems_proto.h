#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "vems_link.h"

#define VEMS_A_LEN      56      // MegaTune "A" response length (vemsv3.ini ochBlockSize)
#define VEMS_RAW_MAX    256

typedef enum {
    VEMS_MODE_NONE = 0,
    VEMS_MODE_TF,               // TriggerFrame: 7E A0 BF 38 7E -> HDLC(payload, type=0x20, CRC16) 7E
    VEMS_MODE_A,                // MegaTune compatible 'A' -> 56 raw bytes
} vems_mode_t;

// engine status bits (byte 2), see firmware global.h
#define VEMS_ENG_RUNNING    (1 << 0)
#define VEMS_ENG_CRANK      (1 << 1)
#define VEMS_ENG_STARTW     (1 << 2)
#define VEMS_ENG_WARMUP     (1 << 3)
#define VEMS_ENG_TPSAEN     (1 << 4)
#define VEMS_ENG_TPSDEN     (1 << 5)
#define VEMS_ENG_IDLE       (1 << 7)

typedef struct {
    uint32_t seq;               // incremented on each valid frame
    int64_t timestamp_us;
    vems_mode_t mode;
    uint32_t baud;

    uint8_t raw[VEMS_RAW_MAX];  // realtime payload (first 56 bytes = MegaTune 'A' layout)
    uint16_t raw_len;

    // Scaling from VemsTune vemsTune-v3-1.2.15.ini [OutputChannels] (identical for 1.2.16/1.2.17)
    uint8_t secl;               // 0  seconds counter
    float boost_dc_alt_pct;     // 1  * 100/255
    uint8_t engine_status;      // 2  VEMS_ENG_* bits
    uint8_t kpa_tps_blend;      // 3
    float map_kpa;              // 4-5 U16 / 4
    float iat_c;                // 6  raw - 100
    float clt_c;                // 7  raw - 100
    float tps_pct;              // 8  * 100/255
    float batt_v;               // 9  raw / 255 * 30
    uint8_t ego_raw;            // 10
    float lambda;               // 10 raw > 211 ? (8*raw - 1171)/32/14.7 : (raw + 306)/470
    float afr;
    float ego_corr_pct;         // 11
    uint8_t ego2_raw;           // 12
    float lambda2;
    float warmup_pct;           // 13 (raw + 45) * 2
    uint16_t rpm;               // 14-15
    float pulsewidth_ms;        // 16-17 * 0.004
    float baro_corr_pct;        // 18 (raw + 272) * 0.25
    float gamma_pct;            // 19 * 0.99
    uint8_t ve;                 // 20 %
    float dwell_ms;             // 21 * 0.064
    float ign_adv_deg;          // 22 /2 - 64
    float iac_pct;              // 23 * 100/255
    uint16_t egt1_raw;          // 24-25 MCP3208 ch4 raw
    uint16_t egt2_raw;          // 26-27 MCP3208 ch3 raw
    float lambda_target;        // 40 256 / (raw + 200)
    uint8_t status1;            // 47 bit7 ALS sw, 6 launch sw, 5 shiftcut, 4 igncut, 3 idle, 2 closed loop, 1 wheel err, 0 ALS active
    float boost_target_kpa;     // 48 * 4
    float boost_dc_pct;         // 49 * 100/255
} vems_data_t;

typedef struct {
    uint32_t frames_ok;
    uint32_t timeouts;
    uint32_t crc_errors;
    uint32_t resyncs;
} vems_stats_t;

typedef enum {
    VEMS_STATE_STOPPED = 0,     // vems_proto_start() not called yet
    VEMS_STATE_WAIT_LINK,       // transport started, waiting for FTDI / UART
    VEMS_STATE_LINK_UP,         // transport ready, probing the ECU
    VEMS_STATE_ECU_OK,          // ECU answers realtime requests
} vems_state_t;

esp_err_t vems_proto_start(const vems_link_t *link);

vems_state_t vems_get_state(void);

/** Copies the latest decoded frame. Returns false if no frame was received yet. */
bool vems_get_latest(vems_data_t *out);

void vems_get_stats(vems_stats_t *out);

const char *vems_mode_name(vems_mode_t mode);
