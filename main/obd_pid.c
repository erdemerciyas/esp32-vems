/*
 * SAE J1979 servis 01 PID tablosu ve çözücüler (A, B, C, D = 41 PID sonrası veri baytları).
 */
#include <math.h>
#include <stdio.h>
#include "obd_pid.h"

#define AB(v)   ((uint16_t)(((v)[0] << 8) | (v)[1]))
#define CD(v)   ((uint16_t)(((v)[2] << 8) | (v)[3]))
#define PCT(x)  ((x) * 100.0f / 255.0f)
#define TRIM(x) (((x) - 128) * 100.0f / 128.0f)

static const obd_pid_def_t DEFS[] = {
    {0x01, 4, 0, 0, "Arıza lambası", ""},
    {0x03, 2, 0, 0, "Yakıt sistemi", ""},
    {0x04, 1, 0, 0, "Motor yükü", "%"},
    {0x05, 1, 0, 0, "Hararet", "°C"},
    {0x06, 1, 1, 0, "Kısa yakıt düz. B1", "%"},
    {0x07, 1, 1, 0, "Uzun yakıt düz. B1", "%"},
    {0x08, 1, 1, 0, "Kısa yakıt düz. B2", "%"},
    {0x09, 1, 1, 0, "Uzun yakıt düz. B2", "%"},
    {0x0A, 1, 0, 0, "Yakıt basıncı", "kPa"},
    {0x0B, 1, 0, 0, "Manifold basıncı", "kPa"},
    {0x0C, 2, 0, 0, "Devir", "rpm"},
    {0x0D, 1, 0, 0, "Hız", "km/h"},
    {0x0E, 1, 1, 0, "Ateşleme avansı", "°"},
    {0x0F, 1, 0, 0, "Emme havası", "°C"},
    {0x10, 2, 1, 0, "Hava akışı (MAF)", "g/s"},
    {0x11, 1, 0, 0, "Gaz kelebeği", "%"},
    {0x13, 1, 0, OPF_ONCE, "O2 sensörleri", ""},
    {0x14, 2, 3, 0, "O2 B1S1 (ön)", "V"},
    {0x15, 2, 3, 0, "O2 B1S2 (arka)", "V"},
    {0x16, 2, 3, 0, "O2 B1S3", "V"},
    {0x17, 2, 3, 0, "O2 B1S4", "V"},
    {0x18, 2, 3, 0, "O2 B2S1", "V"},
    {0x19, 2, 3, 0, "O2 B2S2", "V"},
    {0x1A, 2, 3, 0, "O2 B2S3", "V"},
    {0x1B, 2, 3, 0, "O2 B2S4", "V"},
    {0x1C, 1, 0, OPF_ONCE, "OBD standardı", ""},
    {0x1D, 1, 0, OPF_ONCE, "O2 sensörleri (4 bank)", ""},
    {0x1E, 1, 0, 0, "Yardımcı giriş (PTO)", ""},
    {0x1F, 2, 0, 0, "Çalışma süresi", ""},
    {0x21, 2, 0, 0, "Lamba yanarken mesafe", "km"},
    {0x22, 2, 1, 0, "Yakıt rayı (vakuma göre)", "kPa"},
    {0x23, 2, 0, 0, "Yakıt rayı basıncı", "kPa"},
    {0x24, 4, 3, 0, "Geniş bant O2 S1", "LAM"},
    {0x25, 4, 3, 0, "Geniş bant O2 S2", "LAM"},
    {0x26, 4, 3, 0, "Geniş bant O2 S3", "LAM"},
    {0x27, 4, 3, 0, "Geniş bant O2 S4", "LAM"},
    {0x28, 4, 3, 0, "Geniş bant O2 S5", "LAM"},
    {0x29, 4, 3, 0, "Geniş bant O2 S6", "LAM"},
    {0x2A, 4, 3, 0, "Geniş bant O2 S7", "LAM"},
    {0x2B, 4, 3, 0, "Geniş bant O2 S8", "LAM"},
    {0x2C, 1, 0, 0, "Hedef EGR", "%"},
    {0x2D, 1, 1, 0, "EGR hatası", "%"},
    {0x2E, 1, 0, 0, "Kanister temizleme", "%"},
    {0x2F, 1, 0, 0, "Yakıt seviyesi", "%"},
    {0x30, 1, 0, 0, "Silmeden beri ısınma", ""},
    {0x31, 2, 0, 0, "Silmeden beri mesafe", "km"},
    {0x32, 2, 0, 0, "EVAP buhar basıncı", "Pa"},
    {0x33, 1, 0, 0, "Atmosfer basıncı", "kPa"},
    {0x34, 4, 3, 0, "Geniş bant O2 S1 (akım)", "LAM"},
    {0x35, 4, 3, 0, "Geniş bant O2 S2 (akım)", "LAM"},
    {0x36, 4, 3, 0, "Geniş bant O2 S3 (akım)", "LAM"},
    {0x37, 4, 3, 0, "Geniş bant O2 S4 (akım)", "LAM"},
    {0x38, 4, 3, 0, "Geniş bant O2 S5 (akım)", "LAM"},
    {0x39, 4, 3, 0, "Geniş bant O2 S6 (akım)", "LAM"},
    {0x3A, 4, 3, 0, "Geniş bant O2 S7 (akım)", "LAM"},
    {0x3B, 4, 3, 0, "Geniş bant O2 S8 (akım)", "LAM"},
    {0x3C, 2, 0, 0, "Katalizör B1S1", "°C"},
    {0x3D, 2, 0, 0, "Katalizör B2S1", "°C"},
    {0x3E, 2, 0, 0, "Katalizör B1S2", "°C"},
    {0x3F, 2, 0, 0, "Katalizör B2S2", "°C"},
    {0x42, 2, 2, 0, "Modül voltajı", "V"},
    {0x43, 2, 0, 0, "Mutlak yük", "%"},
    {0x44, 2, 3, 0, "Hedef lambda", "LAM"},
    {0x45, 1, 0, 0, "Bağıl gaz kelebeği", "%"},
    {0x46, 1, 0, 0, "Ortam sıcaklığı", "°C"},
    {0x47, 1, 0, 0, "Gaz kelebeği B", "%"},
    {0x48, 1, 0, 0, "Gaz kelebeği C", "%"},
    {0x49, 1, 0, 0, "Gaz pedalı D", "%"},
    {0x4A, 1, 0, 0, "Gaz pedalı E", "%"},
    {0x4B, 1, 0, 0, "Gaz pedalı F", "%"},
    {0x4C, 1, 0, 0, "Gaz kelebeği hedefi", "%"},
    {0x4D, 2, 0, 0, "Lamba yanık süre", "dk"},
    {0x4E, 2, 0, 0, "Silmeden beri süre", "dk"},
    {0x51, 1, 0, OPF_ONCE, "Yakıt tipi", ""},
    {0x52, 1, 0, 0, "Etanol oranı", "%"},
    {0x53, 2, 1, 0, "EVAP mutlak basınç", "kPa"},
    {0x54, 2, 0, 0, "EVAP basıncı", "Pa"},
    {0x59, 2, 0, 0, "Yakıt rayı mutlak basınç", "kPa"},
    {0x5A, 1, 0, 0, "Bağıl gaz pedalı", "%"},
    {0x5B, 1, 0, 0, "Hibrit batarya", "%"},
    {0x5C, 1, 0, 0, "Yağ sıcaklığı", "°C"},
    {0x5D, 2, 1, 0, "Enjeksiyon zamanlaması", "°"},
    {0x5E, 2, 2, 0, "Yakıt debisi", "L/sa"},
};

const obd_pid_def_t *obd_pid_def(uint8_t pid)
{
    for (size_t i = 0; i < sizeof(DEFS) / sizeof(DEFS[0]); i++) {
        if (DEFS[i].pid == pid) {
            return &DEFS[i];
        }
    }
    return NULL;
}

float obd_pid_value(uint8_t pid, const uint8_t *v)
{
    switch (pid) {
    case 0x01:  return v[0] & 0x7F;                         // kayıtlı kod sayısı
    case 0x04: case 0x11: case 0x2C: case 0x2E: case 0x2F: case 0x45: case 0x47: case 0x48: case 0x49:
    case 0x4A: case 0x4B: case 0x4C: case 0x52: case 0x5A: case 0x5B:
        return PCT(v[0]);
    case 0x05: case 0x0F: case 0x46: case 0x5C:
        return v[0] - 40.0f;
    case 0x06: case 0x07: case 0x08: case 0x09: case 0x2D:
        return TRIM(v[0]);
    case 0x0A:  return v[0] * 3.0f;
    case 0x0B: case 0x0D: case 0x30: case 0x33:
        return v[0];
    case 0x0C:  return AB(v) / 4.0f;
    case 0x0E:  return v[0] / 2.0f - 64.0f;
    case 0x10:  return AB(v) / 100.0f;
    case 0x1E:  return v[0] & 1;
    case 0x1F: case 0x21: case 0x31: case 0x4D: case 0x4E:
        return AB(v);
    case 0x22:  return AB(v) * 0.079f;
    case 0x23: case 0x59:
        return AB(v) * 10.0f;
    case 0x32:  return (int16_t)AB(v) / 4.0f;
    case 0x3C: case 0x3D: case 0x3E: case 0x3F:
        return AB(v) / 10.0f - 40.0f;
    case 0x42:  return AB(v) / 1000.0f;
    case 0x43:  return AB(v) * 100.0f / 255.0f;
    case 0x44:  return AB(v) * 2.0f / 65536.0f;
    case 0x53:  return AB(v) / 200.0f;
    case 0x54:  return AB(v) - 32767.0f;
    case 0x5D:  return AB(v) / 128.0f - 210.0f;
    case 0x5E:  return AB(v) / 20.0f;
    default:
        if (pid >= 0x14 && pid <= 0x1B) {
            return v[0] / 200.0f;                           // A: voltaj, B: kısa düzeltme
        }
        if (pid >= 0x24 && pid <= 0x2B) {
            return AB(v) * 2.0f / 65536.0f;                 // AB: lambda, CD: voltaj
        }
        if (pid >= 0x34 && pid <= 0x3B) {
            return AB(v) * 2.0f / 65536.0f;                 // AB: lambda, CD: akım
        }
        return NAN;
    }
}

static const char *fuel_system(uint8_t a)
{
    switch (a) {
    case 0x01:  return "AÇIK ÇEVRİM";
    case 0x02:  return "KAPALI ÇEVRİM";
    case 0x04:  return "AÇIK · YÜK";
    case 0x08:  return "AÇIK · ARIZA";
    case 0x10:  return "KAPALI · ARIZA";
    default:    return "--";
    }
}

static const char *obd_standard(uint8_t a)
{
    static const char *const S[] = {"--", "OBD-II (CARB)", "OBD (EPA)", "OBD + OBD-II", "OBD-I", "OBD YOK", "EOBD",
                                    "EOBD + OBD-II", "EOBD + OBD", "EOBD + OBD + OBD-II", "JOBD", "JOBD + OBD-II",
                                    "JOBD + EOBD", "JOBD + EOBD + OBD-II"};
    return a < sizeof(S) / sizeof(S[0]) ? S[a] : "DİĞER";
}

static const char *fuel_type(uint8_t a)
{
    static const char *const S[] = {"--", "BENZİN", "METANOL", "ETANOL", "DİZEL", "LPG", "CNG", "PROPAN", "ELEKTRİK"};
    return a < sizeof(S) / sizeof(S[0]) ? S[a] : "ÇİFT YAKIT / DİĞER";
}

void obd_pid_format(uint8_t pid, const uint8_t *v, char *buf, size_t len)
{
    switch (pid) {
    case 0x01:
        snprintf(buf, len, "%s · %d KOD", (v[0] & 0x80) ? "YANIYOR" : "SÖNÜK", v[0] & 0x7F);
        return;
    case 0x03:
        snprintf(buf, len, "%s", fuel_system(v[0]));
        return;
    case 0x13: {
        size_t n = 0;
        buf[0] = '\0';
        for (int b = 0; b < 8; b++) {
            if (v[0] & (1 << b)) {
                n += snprintf(buf + n, n < len ? len - n : 0, "%sB%dS%d", n ? " " : "", b / 4 + 1, b % 4 + 1);
            }
        }
        if (n == 0) {
            snprintf(buf, len, "YOK");
        }
        return;
    }
    case 0x1C:
        snprintf(buf, len, "%s", obd_standard(v[0]));
        return;
    case 0x1D:
        snprintf(buf, len, "0x%02X", v[0]);
        return;
    case 0x1E:
        snprintf(buf, len, "%s", (v[0] & 1) ? "AÇIK" : "KAPALI");
        return;
    case 0x1F:
        snprintf(buf, len, "%d:%02d", AB(v) / 60, AB(v) % 60);
        return;
    case 0x51:
        snprintf(buf, len, "%s", fuel_type(v[0]));
        return;
    default: {
        const obd_pid_def_t *d = obd_pid_def(pid);
        const float x = obd_pid_value(pid, v);
        if (d == NULL || isnan(x)) {
            snprintf(buf, len, "--");
        } else {
            snprintf(buf, len, "%.*f", d->dec, x);
        }
        return;
    }
    }
}
