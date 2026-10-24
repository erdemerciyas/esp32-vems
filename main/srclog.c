#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "srclog.h"

#define SRCLOG_LINES    12

static srclog_line_t s_lines[SRCLOG_LINES];
static int s_count;         // lines written since the last clear (only the newest SRCLOG_LINES are kept)
static uint32_t s_seq;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void srclog_clear(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_count = 0;
    s_seq++;
    taskEXIT_CRITICAL(&s_mux);
}

void srclog_step(const char *fmt, ...)
{
    srclog_line_t l = {.state = SRCLOG_WAIT};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l.text, sizeof(l.text), fmt, ap);
    va_end(ap);
    strcpy(l.result, "...");

    taskENTER_CRITICAL(&s_mux);
    s_lines[s_count % SRCLOG_LINES] = l;
    s_count++;
    s_seq++;
    taskEXIT_CRITICAL(&s_mux);
}

void srclog_done(srclog_state_t state, const char *fmt, ...)
{
    char res[sizeof(((srclog_line_t *)0)->result)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(res, sizeof(res), fmt, ap);
    va_end(ap);

    char text[sizeof(((srclog_line_t *)0)->text)] = "";
    taskENTER_CRITICAL(&s_mux);
    if (s_count > 0) {
        srclog_line_t *l = &s_lines[(s_count - 1) % SRCLOG_LINES];
        l->state = state;
        memcpy(l->result, res, sizeof(res));
        memcpy(text, l->text, sizeof(text));
        s_seq++;
    }
    taskEXIT_CRITICAL(&s_mux);

    // aynı satır konsola da yazılır: ekran olmadığında açılış adımları sadece burada görünür
    static const char *const STATE[] = {"...", "OK", "--", "HATA"};
    ESP_LOGI("src", "%-26s %-4s %s", text, STATE[state], res);
}

int srclog_read(srclog_line_t *out, int max, uint32_t *seq)
{
    taskENTER_CRITICAL(&s_mux);
    int n = s_count < SRCLOG_LINES ? s_count : SRCLOG_LINES;
    if (n > max) {
        n = max;
    }
    for (int i = 0; i < n; i++) {
        out[i] = s_lines[(s_count - n + i) % SRCLOG_LINES];
    }
    if (seq) {
        *seq = s_seq;
    }
    taskEXIT_CRITICAL(&s_mux);
    return n;
}
