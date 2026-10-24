#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * SAE J1979 servis 01 PID tablosu: cevap uzunluğu (ISO 9141'de çerçeve uzunluğu bundan bulunur),
 * Türkçe ad, birim ve çözme. Tabloda olmayan PID'ler desteklense de sorgulanmaz.
 */
#define OPF_ONCE    0x01    // değişmeyen bilgi (OBD standardı, sensör haritası): bağlantıda bir kez okunur

typedef struct {
    uint8_t pid;
    uint8_t len;            // veri baytı (41 PID sonrası)
    uint8_t dec;            // ekranda ondalık
    uint8_t flags;          // OPF_*
    const char *name;
    const char *unit;
} obd_pid_def_t;

/** NULL: tabloda yok. */
const obd_pid_def_t *obd_pid_def(uint8_t pid);

/** Birincil değer (O2 için voltaj, geniş bant için lambda ...), sayısal değilse NAN. */
float obd_pid_value(uint8_t pid, const uint8_t *v);

/** Ekrandaki metin, birimsiz (sayısal olmayanlar için açıklama: "KAPALI ÇEVRİM", "EOBD" ...). */
void obd_pid_format(uint8_t pid, const uint8_t *v, char *buf, size_t len);
