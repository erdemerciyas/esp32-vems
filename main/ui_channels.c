/*
 * Channel table shared by every page: names, units, ranges, warning colors and the value lookup
 * from vems_data_t. Pages never ask which source is connected, only which channels exist.
 */
#include <math.h>
#include <stdio.h>
#include "ui_priv.h"

const ui_ch_info_t UI_CH[CH_COUNT] = {
    [CH_RPM]     = {"Devir",            "DEVİR",     "rpm",  0,    0, 8000},
    [CH_SPEED]   = {"Hız",              "HIZ",       "km/h", 0,    0,  240},
    [CH_MAP]     = {"Manifold basıncı", "MAP",       "kPa",  0,   20,  250},
    [CH_BOOST]   = {"Boost",            "BOOST",     "bar",  2,   -1,  1.5f},
    [CH_AFR]     = {"AFR",              "AFR",       "",     1,   10,   20},
    [CH_O2V]     = {"O2 sensörü",       "O2",        "V",    2,    0,    1},
    [CH_O2V2]    = {"Arka O2 sensörü",  "O2 ARKA",   "V",    2,    0,    1},
    [CH_CLT]     = {"Hararet",          "HARARET",   "°C",   0,   40,  130},
    [CH_IAT]     = {"Emme havası",      "EMME",      "°C",   0,  -10,   80},
    [CH_BATT]    = {"Akü",              "AKÜ",       "V",    1,   10,   16},
    [CH_TPS]     = {"Gaz kelebeği",     "TPS",       "%",    0,    0,  100},
    [CH_LOAD]    = {"Motor yükü",       "YÜK",       "%",    0,    0,  100},
    [CH_ADV]     = {"Avans",            "AVANS",     "°",    1,  -10,   50},
    [CH_OIL]     = {"Yağ sıcaklığı",    "YAĞ",       "°C",   0,   40,  150},
    [CH_FUEL]    = {"Yakıt seviyesi",   "YAKIT",     "%",    0,    0,  100},
    [CH_TARGET]  = {"Hedef AFR",        "HEDEF",     "",     1,   10,   20},
    [CH_STFT]    = {"Kısa yakıt düz.",  "KISA DÜZ.", "%",    1,  -25,   25},
    [CH_LTFT]    = {"Uzun yakıt düz.",  "UZUN DÜZ.", "%",    1,  -25,   25},
    [CH_MAF]     = {"Hava akışı",       "MAF",       "g/s",  1,    0,  200},
    [CH_BARO]    = {"Barometre",        "BARO",      "kPa",  0,   70,  105},
    [CH_RUNTIME] = {"Çalışma süresi",   "SÜRE",      "",     0,    0, 3600},
    [CH_PW]      = {"Enjektör süresi",  "ENJ.",      "ms",   2,    0,   20},
    [CH_VE]      = {"VE",               "VE",        "%",    0,    0,  120},
    [CH_DWELL]   = {"Dwell",            "DWELL",     "ms",   1,    0,    6},
    [CH_IAC]     = {"Rölanti valfi",    "IAC",       "%",    0,    0,  100},
    [CH_FUEL_LH] = {"Yakıt akışı",      "AKIŞ",      "L/sa", 1,    0,   20},
    [CH_FUEL_L100] = {"Anlık tüketim",  "L/100KM",  "L/100", 1,   0,   30},   // kart başlığı birimi zaten içerir
};

static const vems_ch_t ORDER[] = {
    CH_RPM, CH_SPEED, CH_MAP, CH_BOOST, CH_AFR, CH_O2V, CH_O2V2, CH_TARGET, CH_CLT, CH_IAT, CH_OIL, CH_BATT, CH_TPS,
    CH_LOAD, CH_ADV, CH_FUEL_L100, CH_FUEL_LH, CH_STFT, CH_LTFT, CH_MAF, CH_BARO, CH_FUEL, CH_RUNTIME, CH_PW,
    CH_VE, CH_DWELL, CH_IAC,
};

// before the first connection: what most cars have
#define DEFAULT_MASK    (CH_BIT(CH_RPM) | CH_BIT(CH_MAP) | CH_BIT(CH_AFR) | CH_BIT(CH_CLT) | CH_BIT(CH_IAT) | \
                         CH_BIT(CH_BATT) | CH_BIT(CH_TPS) | CH_BIT(CH_ADV) | CH_BIT(CH_LOAD))

static struct {
    float lo[CH_COUNT];
    float hi[CH_COUNT];
    bool have[CH_COUNT];
} mm;

uint32_t ui_ch_layout_mask(uint32_t mask)
{
    return mask ? mask : DEFAULT_MASK;
}

int ui_ch_list(uint32_t mask, vems_ch_t *out, int max)
{
    mask = ui_ch_layout_mask(mask);
    int n = 0;
    for (size_t i = 0; i < sizeof(ORDER) / sizeof(ORDER[0]) && n < max; i++) {
        if (mask & CH_BIT(ORDER[i])) {
            out[n++] = ORDER[i];
        }
    }
    return n;
}

float ui_ch_value(const vems_data_t *d, vems_ch_t c)
{
    const float st = ui_stoich_afr();
    switch (c) {
    case CH_RPM:     return d->rpm;
    case CH_SPEED:   return d->speed_kmh;
    case CH_MAP:     return d->map_kpa;
    case CH_BOOST:   return isnan(d->map_kpa) ? NAN : (d->map_kpa - (isnan(d->baro_kpa) || d->baro_kpa <= 0 ? 101.3f : d->baro_kpa)) / 100.0f;
    case CH_AFR:     return ui_lambda_valid(d) ? d->lambda * st : NAN;
    case CH_O2V:     return d->o2_v;
    case CH_O2V2:    return d->o2b_v;
    case CH_CLT:     return d->clt_c;
    case CH_IAT:     return d->iat_c;
    case CH_BATT:    return d->batt_v;
    case CH_TPS:     return d->tps_pct;
    case CH_LOAD:    return d->load_pct;
    case CH_ADV:     return d->ign_adv_deg;
    case CH_OIL:     return d->oil_c;
    case CH_FUEL:    return d->fuel_pct;
    case CH_TARGET:  return (d->lambda_target > 0.3f) ? d->lambda_target * st : NAN;
    case CH_STFT:    return d->ego_corr_pct - 100.0f;
    case CH_LTFT:    return d->ltft_pct;
    case CH_MAF:     return d->maf_gs;
    case CH_BARO:    return d->baro_kpa;
    case CH_RUNTIME: return d->runtime_s;
    case CH_PW:      return d->pulsewidth_ms;
    case CH_VE:      return d->ve;
    case CH_DWELL:   return d->dwell_ms;
    case CH_IAC:     return d->iac_pct;
    case CH_FUEL_LH: return d->fuel_lh;
    case CH_FUEL_L100: return d->fuel_l100;
    default:         return NAN;
    }
}

void ui_ch_format(vems_ch_t c, float v, char *buf, size_t len)
{
    if (isnan(v)) {
        snprintf(buf, len, "--");
    } else if (c == CH_RUNTIME) {
        int s = (int)v;
        if (s >= 3600) {
            snprintf(buf, len, "%d:%02d", s / 3600, (s / 60) % 60);
        } else {
            snprintf(buf, len, "%d:%02d", s / 60, s % 60);
        }
    } else if (c == CH_BOOST && v > 0) {
        snprintf(buf, len, "+%.*f", UI_CH[c].dec, v);
    } else {
        snprintf(buf, len, "%.*f", UI_CH[c].dec, v);
    }
}

lv_color_t ui_ch_color(vems_ch_t c, float v)
{
    if (isnan(v)) {
        return C_MUTED;
    }
    const float batt_low = g_settings.batt_low / 10.0f;
    switch (c) {
    case CH_AFR:
    case CH_TARGET: return lv_color_hex(ui_afr_zone(v / ui_stoich_afr())->color);
    case CH_CLT:    return v >= g_settings.clt_warn ? C_ALERT : v >= g_settings.clt_warn - 7 ? C_WARN : v < 40 ? C_ACCENT : C_TEXT;
    case CH_IAT:    return v >= 60 ? C_WARN : C_TEXT;
    case CH_BATT:   return (v < batt_low || v > 15.5f) ? C_ALERT : v < batt_low + 0.7f ? C_WARN : C_TEXT;
    case CH_OIL:    return v >= 135 ? C_ALERT : v >= 120 ? C_WARN : C_TEXT;
    case CH_FUEL:   return v < 15 ? C_WARN : C_TEXT;
    case CH_BOOST:  return v > 1.5f ? C_WARN : C_TEXT;
    case CH_MAP:    return v > 250 ? C_WARN : C_TEXT;
    case CH_STFT:
    case CH_LTFT:   return fabsf(v) > 15 ? C_WARN : C_TEXT;
    case CH_RPM:    return v >= g_settings.shift_rpm ? C_ALERT : C_TEXT;
    default:        return C_TEXT;
    }
}

void ui_ch_track(const vems_data_t *d, uint32_t mask)
{
    for (int c = 0; c < CH_COUNT; c++) {
        if (!(mask & CH_BIT(c))) {
            continue;
        }
        float v = ui_ch_value(d, c);
        if (isnan(v)) {
            continue;
        }
        if (!mm.have[c]) {
            mm.lo[c] = mm.hi[c] = v;
            mm.have[c] = true;
        } else {
            mm.lo[c] = fminf(mm.lo[c], v);
            mm.hi[c] = fmaxf(mm.hi[c], v);
        }
    }
}

bool ui_ch_minmax(vems_ch_t c, float *lo, float *hi)
{
    *lo = mm.lo[c];
    *hi = mm.hi[c];
    return mm.have[c];
}

void ui_ch_reset(vems_ch_t c)
{
    mm.have[c] = false;
}
