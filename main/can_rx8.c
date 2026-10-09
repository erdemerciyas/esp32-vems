/*
 * Mazda RX-8 (SE3P) PCM emulation on the HS-CAN (500 kbit/s, OBD pins 6/14).
 * Frame layout from DaveBlackH/MazdaRX8Arduino (Mk1 PCM replacement). Series 2 cars are reported
 * to use the same frames, but verify every value on the bench before driving.
 */
#include "sdkconfig.h"
#include "can_out.h"

#define RX8_ID_PCM_STATUS   0x201   // rpm, speed, throttle pedal (cluster, EPS, DSC)
#define RX8_ID_PCM_MIL      0x420   // coolant temp, odometer, oil pressure, warning lamps
#define RX8_ID_DSC          0x212   // DSC / ABS / brake lamps
#define RX8_ID_IMMO_RESP    0x041
#define RX8_ID_IMMO_REQ     0x047   // keyless / immobilizer unit -> PCM
#define RX8_ID_WHEELS       0x4B0   // DSC wheel speeds FL FR RL RR, U16 BE, raw = km/h * 100 + 10000

#define RX8_FAST_US         (20 * 1000)
#define RX8_SLOW_US         (100 * 1000)
#define RX8_WHEELS_STALE_US (500 * 1000)
#define RX8_SPEED_ZERO      10000
#define RX8_CHARGE_MIN_V    12.5f

// 0x420 lamp bits
#define MIL5_CHECK_ENGINE   0x40
#define MIL6_BATT_CHARGE    0x40
#define MIL6_OIL_PRESSURE   0x80

// PCM frames with fixed content, needed to keep the ABS / DSC / TCS lamps off
static const struct {
    uint16_t id;
    uint8_t dlc;
    uint8_t data[8];
} RX8_STATIC[] = {
    {0x203, 7, {19, 19, 19, 19, 175, 3, 19}},
    {0x215, 8, {2, 45, 2, 45, 2, 42, 6, 129}},
    {0x231, 5, {15, 0, 255, 255, 0}},
    {0x240, 8, {4, 0, 40, 0, 2, 55, 6, 129}},
    {0x620, 7, {0, 0, 0, 0, 16, 0, 4}},
    {0x630, 8, {8, 0, 0, 0, 0, 0, 106, 106}},  // AT/MT and wheel size
    {0x650, 1, {0}},
};

static const uint16_t RX8_RX_IDS[] = {RX8_ID_IMMO_REQ, RX8_ID_WHEELS};

static int64_t s_next_fast;
static int64_t s_next_slow;
static uint16_t s_speed_raw = RX8_SPEED_ZERO;
static int64_t s_wheels_us;

static bool due(int64_t *next, int64_t now, int64_t period)
{
    if (now < *next) {
        return false;
    }
    *next = now + period;
    return true;
}

static uint8_t clamp_u8(float v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

static void rx8_on_rx(const can_frame_t *f, int64_t now)
{
    if (f->id == RX8_ID_WHEELS && f->dlc >= 4) {
        // vehicle speed for the speedometer = average of the front wheels (VEMS has no VSS)
        uint16_t fl = (f->data[0] << 8) | f->data[1];
        uint16_t fr = (f->data[2] << 8) | f->data[3];
        uint16_t avg = ((uint32_t)fl + fr) / 2;
        s_speed_raw = avg < RX8_SPEED_ZERO ? RX8_SPEED_ZERO : avg;
        s_wheels_us = now;
    }
#if CONFIG_VEMS_CAN_RX8_IMMO
    else if (f->id == RX8_ID_IMMO_REQ && f->dlc >= 3) {
        // immobilizer handshake, otherwise the key lamp keeps flashing
        static const uint8_t RESP_A[8] = {7, 12, 48, 242, 23, 0, 0, 0};
        static const uint8_t RESP_B[8] = {129, 127, 0, 0, 0, 0, 0, 0};
        if (f->data[1] == 127 && f->data[2] == 2) {
            can_out_send(RX8_ID_IMMO_RESP, RESP_A, sizeof(RESP_A));
        } else if (f->data[1] == 92 && f->data[2] == 244) {
            can_out_send(RX8_ID_IMMO_RESP, RESP_B, sizeof(RESP_B));
        }
    }
#endif
}

static void rx8_tick(const vems_data_t *d, int64_t now)
{
    bool running = d && (d->engine_status & VEMS_ENG_RUNNING);

    if (due(&s_next_fast, now, RX8_FAST_US)) {
        uint32_t rpm = d ? d->rpm * 385u / 100u : 0;   // cluster expects rpm * 3.85
        if (rpm > 0xFFFF) {
            rpm = 0xFFFF;
        }
        uint16_t speed = now - s_wheels_us < RX8_WHEELS_STALE_US ? s_speed_raw : RX8_SPEED_ZERO;
        uint8_t pcm[8] = {
            rpm >> 8, rpm & 0xFF, 0xFF, 0xFF,
            speed >> 8, speed & 0xFF,
            d ? clamp_u8(d->tps_pct * 2.0f) : 0,       // pedal 0.5 %/bit (TPS stands in for APP)
            0xFF,
        };
        can_out_send(RX8_ID_PCM_STATUS, pcm, sizeof(pcm));
    }

    if (!due(&s_next_slow, now, RX8_SLOW_US)) {
        return;
    }

    uint8_t mil[7] = {0};
    if (d) {
        mil[0] = clamp_u8(d->clt_c + 40.0f);        // assumed C + 40 like OBD, check the gauge on the bench
    }
    if (!d || (d->status1 & (1 << 1))) {           // no data from VEMS or trigger wheel error
        mil[5] |= MIL5_CHECK_ENGINE;
    }
    if (!running || d->batt_v < RX8_CHARGE_MIN_V) {
        mil[6] |= MIL6_BATT_CHARGE;
    }
#if CONFIG_VEMS_CAN_RX8_OIL_ASSUME_OK
    mil[4] = running ? 1 : 0;
    if (!running) {
        mil[6] |= MIL6_OIL_PRESSURE;
    }
#else
    mil[6] |= MIL6_OIL_PRESSURE;                    // no oil pressure source, keep the lamp honest
#endif
    can_out_send(RX8_ID_PCM_MIL, mil, sizeof(mil));

    static const uint8_t DSC_OK[7] = {0};
    can_out_send(RX8_ID_DSC, DSC_OK, sizeof(DSC_OK));

    for (size_t i = 0; i < sizeof(RX8_STATIC) / sizeof(RX8_STATIC[0]); i++) {
        can_out_send(RX8_STATIC[i].id, RX8_STATIC[i].data, RX8_STATIC[i].dlc);
    }
}

const can_profile_t can_profile_rx8 = {
    .name = "Mazda RX-8 (SE3P)",
    .rx_ids = RX8_RX_IDS,
    .rx_id_count = sizeof(RX8_RX_IDS) / sizeof(RX8_RX_IDS[0]),
    .tick = rx8_tick,
    .on_rx = rx8_on_rx,
};
