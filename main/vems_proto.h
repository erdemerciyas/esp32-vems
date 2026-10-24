#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define VEMS_A_LEN      56      // MegaTune "A" response length (vemsv3.ini ochBlockSize)
#define VEMS_RAW_MAX    256

typedef enum {
    VEMS_MODE_NONE = 0,
    // OBD-II K-Line veri kaynağı (obd.c)
    VEMS_MODE_OBD_KWP_FAST,     // ISO 14230-4 fast init
    VEMS_MODE_OBD_KWP_5BAUD,    // ISO 14230-4 5 baud init
    VEMS_MODE_OBD_ISO9141,      // ISO 9141-2
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

    // OBD-II only. With the OBD source every value the ECU does not support is NAN.
    float load_pct;             // PID 04
    float speed_kmh;            // PID 0D
    float ltft_pct;             // PID 07, 0 = no correction
    float maf_gs;               // PID 10
    float o2_v;                 // PID 14, narrow band B1S1
    float runtime_s;            // PID 1F
    float fuel_pct;             // PID 2F
    float baro_kpa;             // PID 33
    float oil_c;                // PID 5C
    float fuel_lh;              // hesaplanan anlık tüketim L/saat (MAF ya da MAP/IAT/RPM)
    float fuel_l100;            // hesaplanan anlık tüketim L/100 km, 5 km/s altında NAN
    float o2b_v;                // PID 15, dar bant O2 B1S2 (katalizör arkası)
    float fuel_rate_lh;         // PID 5E, ECU'nun bildirdiği yakıt debisi
    int8_t mil_codes;           // PID 01, kayıtlı kod sayısı, -1 = bilinmiyor
    bool mil_on;                // PID 01, arıza lambası yanıyor
} vems_data_t;

/**
 * Values the dash can show. Each source reports the ones it really delivers
 * (vems_get_channels()), the UI builds its pages from that mask only.
 */
typedef enum {
    CH_RPM, CH_SPEED, CH_MAP, CH_BOOST, CH_AFR, CH_O2V, CH_CLT, CH_IAT, CH_BATT, CH_TPS, CH_LOAD, CH_ADV,
    CH_OIL, CH_FUEL, CH_TARGET, CH_STFT, CH_LTFT, CH_MAF, CH_BARO, CH_RUNTIME, CH_PW, CH_VE, CH_DWELL, CH_IAC,
    CH_FUEL_LH, CH_FUEL_L100, CH_O2V2,
    CH_COUNT
} vems_ch_t;

#define CH_BIT(c)   (1u << (c))

typedef struct {
    const char *protocol;       // "CAN 500K", "KWP FAST", "VEMS TF" ...
    const char *standard;       // "ISO 15765-4 · 11 bit" ...
    char ecu[12];               // "0x7E8", "0x10", "VEMS v3"
    char line[20];              // "500 kbit/s", "10400 baud", "USB 19200"
    bool obd;                   // read-only OBD source (service 01 only)
} vems_link_info_t;

typedef struct {
    uint32_t frames_ok;
    uint32_t timeouts;
    uint32_t crc_errors;
    uint32_t resyncs;
} vems_stats_t;

typedef enum {
    VEMS_STATE_STOPPED = 0,     // obd_start() henüz çağrılmadı
    VEMS_STATE_WAIT_LINK,       // K-Line UART açık, ECU aranıyor
    VEMS_STATE_LINK_UP,         // ECU bulundu, desteklenen PID'ler taranıyor
    VEMS_STATE_ECU_OK,          // ECU answers realtime requests
} vems_state_t;

/** OBD-II K-Line veri kaynağı: sorgu görevini başlatır, aşağıdaki fonksiyonlar bundan sonra onun verisini döndürür. */
esp_err_t obd_start(void);

vems_state_t vems_get_state(void);

/** Copies the latest decoded frame. Returns false if no frame was received yet. */
bool vems_get_latest(vems_data_t *out);

void vems_get_stats(vems_stats_t *out);

const char *vems_mode_name(vems_mode_t mode);

/** CH_BIT mask of the values the connected ECU delivers, 0 while nothing is connected. */
uint32_t vems_get_channels(void);

/** Protocol / ECU / line details of the current connection (for the connection page). */
void vems_get_link_info(vems_link_info_t *out);

/** Drops the connection and searches again (the boot log is cleared). */
void vems_rescan(void);

/**
 * Ekranda o an görünen kanallar: bunların PID'leri hızlı turda, kalanlar yavaş turda sorgulanır.
 * 0 = varsayılan sabit liste.
 */
void vems_set_wanted_channels(uint32_t mask);

/** Araç profilinde bir PID: ECU destekliyor, son değeri ve yenilenme aralığı. */
typedef struct {
    uint8_t pid;
    bool once;                  // değişmeyen bilgi, bağlantıda bir kez okundu
    const char *name;
    const char *unit;
    char text[24];              // biçimli değer, okunmadıysa "--"
    uint32_t age_ms;            // son cevaptan beri, UINT32_MAX = hiç gelmedi
    uint32_t period_ms;         // son iki cevap arası, 0 = henüz bilinmiyor
} vems_pid_info_t;

/** ECU'nun desteklediği servis 01 PID'leri (01-5F) sırayla. Bağlı değilken 0. */
int vems_get_profile(vems_pid_info_t *out, int max);

#define VEMS_DTC_MAX    32
/** Kayıtlı arıza kodları (OBD 03), ham 2 baytlık DTC'ler. Kod sayısı, bağlantı/cevap yoksa -1. ~4 sn'ye kadar bekler. */
int vems_dtc_read(uint16_t *codes, int max);
/** Arıza kodlarını ve MIL'i siler (OBD 04). ECU onaylamazsa false. */
bool vems_dtc_clear(void);
