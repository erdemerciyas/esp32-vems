#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/spi_master.h"
#include "can_frame.h"

#define MCP2515_MAX_FILTERS 6

typedef struct {
    spi_host_device_t host;
    int sclk_gpio;
    int mosi_gpio;
    int miso_gpio;
    int cs_gpio;
    int spi_hz;
    int osc_mhz;                // crystal on the module: 8 or 16
    const uint16_t *rx_ids;     // hardware acceptance filters, up to MCP2515_MAX_FILTERS
    size_t rx_id_count;         // 0 = receive nothing
} mcp2515_config_t;

/**
 * Resets the controller, sets 500 kbit/s + acceptance filters and enters normal mode.
 * Can be called again to re-probe; ESP_ERR_NOT_FOUND if the chip does not answer.
 */
esp_err_t mcp2515_init(const mcp2515_config_t *cfg);

/** Queues a frame in a free TX buffer. ESP_ERR_TIMEOUT if all 3 buffers stay busy (no ACK on the bus). */
esp_err_t mcp2515_send(const can_frame_t *f);

/** Fetches one received frame, false if both RX buffers are empty. */
bool mcp2515_recv(can_frame_t *f);

/** False if the chip lost its configuration (power cycle) or stopped answering. */
bool mcp2515_is_online(void);

/** Aborts pending frames and leaves the bus (no TX, no ACK) until the next mcp2515_init(). */
void mcp2515_go_offline(void);

void mcp2515_get_errors(uint8_t *tec, uint8_t *rec, uint8_t *eflg);
