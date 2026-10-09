#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "can_frame.h"

typedef struct {
    int tx_gpio;                // -> transceiver TXD
    int rx_gpio;                // <- transceiver RXD (3.3 V level!)
    const uint16_t *rx_ids;     // ids to receive, exact match in software
    size_t rx_id_count;         // 0 = receive nothing
} can_twai_config_t;

/**
 * (Re)installs the ESP32 internal TWAI controller at 500 kbit/s and starts it.
 * Same contract as mcp2515_init(), but the controller is always present.
 */
esp_err_t can_twai_init(const can_twai_config_t *cfg);

/** Queues a frame. ESP_ERR_TIMEOUT if the TX queue is full (no ACK on the bus). */
esp_err_t can_twai_send(const can_frame_t *f);

/** Fetches one received frame with a wanted id, false if none is pending. */
bool can_twai_recv(can_frame_t *f);

/** False after bus-off or when stopped. */
bool can_twai_is_online(void);

/** Stops the controller (no TX, no ACK) until the next can_twai_init(). */
void can_twai_go_offline(void);

/** flags = twai_state_t */
void can_twai_get_errors(uint8_t *tec, uint8_t *rec, uint8_t *flags);
