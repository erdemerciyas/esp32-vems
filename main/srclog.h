#pragma once

#include <stdint.h>

/*
 * Veri kaynağının açılış günlüğü: gerçek algılama adımları (K-Line init, ECU,
 * PID taraması) sonuçlu kısa satırlar halinde; açılış ekranında, konsolda ve telefon panelinde görünür.
 * Kaynak görevi yazar; ekran okur.
 */

typedef enum {
    SRCLOG_WAIT = 0,    // step running
    SRCLOG_OK,
    SRCLOG_SKIP,        // not applicable / nothing there, not an error
    SRCLOG_FAIL,
} srclog_state_t;

typedef struct {
    char text[32];
    char result[16];
    uint8_t state;      // srclog_state_t
} srclog_line_t;

void srclog_clear(void);

/** Starts a new line in the WAIT state. */
void srclog_step(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** Closes the last line. */
void srclog_done(srclog_state_t state, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/** Copies up to max of the newest lines, oldest first. Returns the count; *seq changes on every update. */
int srclog_read(srclog_line_t *out, int max, uint32_t *seq);
