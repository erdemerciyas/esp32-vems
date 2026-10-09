#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "can_frame.h"
#include "vems_proto.h"

// Vehicle profile: translates VEMS realtime data into the CAN frames of the original ECU.
typedef struct {
    const char *name;
    const uint16_t *rx_ids;     // ids the profile wants to receive (hardware filters, max 6)
    size_t rx_id_count;
    /** Called every few ms; d is NULL while there is no fresh VEMS data. Sends the frames that are due. */
    void (*tick)(const vems_data_t *d, int64_t now_us);
    void (*on_rx)(const can_frame_t *f, int64_t now_us);
} can_profile_t;

extern const can_profile_t can_profile_rx8;

typedef enum {
    CAN_OUT_OFF = 0,            // switched off from the UI
    CAN_OUT_NO_MODULE,          // controller does not answer / bus-off, probed again every 2 s
    CAN_OUT_NO_ACK,             // frames are not acknowledged (bus not connected / ignition off)
    CAN_OUT_ACTIVE,
} can_out_state_t;

typedef struct {
    can_out_state_t state;
    uint32_t tx_ok;
    uint32_t tx_drop;
    uint32_t rx;
} can_out_status_t;

/**
 * Starts the CAN task (also when no module is connected yet). Call after board_lcd_init(),
 * it reuses the LCD init SPI pins. The on/off switch is restored from NVS.
 */
esp_err_t can_out_start(void);

/** On/off from the UI, stored in NVS. Off = the controller leaves the bus completely. */
void can_out_set_enabled(bool on);
bool can_out_get_enabled(void);

void can_out_get_status(can_out_status_t *out);
const char *can_out_profile_name(void);

/** For profiles: queues one standard frame. */
void can_out_send(uint16_t id, const uint8_t *data, uint8_t dlc);
