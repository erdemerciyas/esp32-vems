/*
 * CAN over the ESP32-S3 internal TWAI controller + external transceiver (TJA1050 / SN65HVD230).
 * Standard 11 bit ids only, fixed 500 kbit/s. Not thread safe: use from a single task.
 */
#include <string.h>
#include "driver/twai.h"
#include "esp_check.h"
#include "esp_log.h"
#include "can_twai.h"

static const char *TAG = "can_twai";

#define TWAI_TX_QUEUE_LEN   8
#define TWAI_RX_QUEUE_LEN   32      // ~1.5 frames/ms on a busy bus, drained every 5 ms

static const uint16_t *s_rx_ids;
static size_t s_rx_id_count;

static twai_filter_config_t make_filter(const uint16_t *ids, size_t n)
{
    if (n == 0) {
        // nothing wanted: only id 0x7FF passes
        return (twai_filter_config_t) {.acceptance_code = 0x7FFu << 21, .acceptance_mask = 0x001FFFFF, .single_filter = true};
    }
    if (n <= 2) {
        // dual filter: id + RTR of filter 1 (bits 31..20) and filter 2 (bits 15..4), data bits don't care
        uint16_t id1 = ids[n - 1];
        return (twai_filter_config_t) {
            .acceptance_code = ((uint32_t)ids[0] << 21) | ((uint32_t)id1 << 5),
            .acceptance_mask = 0x001F001F,
            .single_filter = false,
        };
    }
    // single filter over the common bits, the rest is filtered in can_twai_recv()
    uint16_t all_and = 0x7FF, all_or = 0;
    for (size_t i = 0; i < n; i++) {
        all_and &= ids[i];
        all_or |= ids[i];
    }
    uint32_t dont_care = (all_and ^ all_or) & 0x7FF;
    return (twai_filter_config_t) {
        .acceptance_code = (uint32_t)all_and << 21,
        .acceptance_mask = (dont_care << 21) | 0x001FFFFF,
        .single_filter = true,
    };
}

esp_err_t can_twai_init(const can_twai_config_t *cfg)
{
    twai_status_info_t st;
    if (twai_get_status_info(&st) == ESP_OK) {
        // already installed (re-probe after bus-off / UI off): start from scratch
        if (st.state == TWAI_STATE_RUNNING) {
            twai_stop();
        }
        ESP_RETURN_ON_ERROR(twai_driver_uninstall(), TAG, "uninstall");
    }

    s_rx_ids = cfg->rx_ids;
    s_rx_id_count = cfg->rx_id_count;

    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(cfg->tx_gpio, cfg->rx_gpio, TWAI_MODE_NORMAL);
    g.tx_queue_len = TWAI_TX_QUEUE_LEN;
    g.rx_queue_len = TWAI_RX_QUEUE_LEN;
    g.alerts_enabled = TWAI_ALERT_NONE;
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = make_filter(cfg->rx_ids, cfg->rx_id_count);

    ESP_RETURN_ON_ERROR(twai_driver_install(&g, &t, &f), TAG, "install");
    esp_err_t err = twai_start();
    if (err != ESP_OK) {
        twai_driver_uninstall();
        return err;
    }
    ESP_LOGI(TAG, "ready, 500 kbit/s, TX GPIO%d, RX GPIO%d, %u rx id(s)",
             cfg->tx_gpio, cfg->rx_gpio, (unsigned)cfg->rx_id_count);
    return ESP_OK;
}

esp_err_t can_twai_send(const can_frame_t *f)
{
    twai_message_t m = {
        .identifier = f->id & 0x7FF,
        .data_length_code = f->dlc > 8 ? 8 : f->dlc,
    };
    memcpy(m.data, f->data, m.data_length_code);
    return twai_transmit(&m, 0);
}

bool can_twai_recv(can_frame_t *f)
{
    twai_message_t m;
    while (twai_receive(&m, 0) == ESP_OK) {
        if (m.extd || m.rtr) {
            continue;
        }
        for (size_t i = 0; i < s_rx_id_count; i++) {
            if (m.identifier == s_rx_ids[i]) {
                f->id = m.identifier;
                f->dlc = m.data_length_code > 8 ? 8 : m.data_length_code;
                memcpy(f->data, m.data, 8);
                return true;
            }
        }
    }
    return false;
}

bool can_twai_is_online(void)
{
    twai_status_info_t st;
    return twai_get_status_info(&st) == ESP_OK && st.state == TWAI_STATE_RUNNING;
}

void can_twai_go_offline(void)
{
    twai_status_info_t st;
    if (twai_get_status_info(&st) == ESP_OK && st.state == TWAI_STATE_RUNNING) {
        twai_stop();
    }
}

void can_twai_get_errors(uint8_t *tec, uint8_t *rec, uint8_t *flags)
{
    twai_status_info_t st = {0};
    twai_get_status_info(&st);
    *tec = st.tx_error_counter > 255 ? 255 : st.tx_error_counter;
    *rec = st.rx_error_counter > 255 ? 255 : st.rx_error_counter;
    *flags = st.state;
}
