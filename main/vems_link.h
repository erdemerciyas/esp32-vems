#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/**
 * Byte-stream transport between the ESP32 and the VEMS ECU.
 * Implemented by the USB host FTDI driver and by a plain UART.
 * All I/O functions are meant to be called from a single (protocol) task.
 */
typedef struct {
    const char *name;
    esp_err_t (*start)(uint32_t baud);
    bool (*is_ready)(void);
    esp_err_t (*set_baud)(uint32_t baud);
    esp_err_t (*write)(const uint8_t *data, size_t len, TickType_t timeout);
    /** Waits until at least one byte is available (or timeout), returns up to len bytes. */
    size_t (*read)(uint8_t *buf, size_t len, TickType_t timeout);
    void (*flush_rx)(void);
} vems_link_t;

extern const vems_link_t vems_link_usb_ftdi;
extern const vems_link_t vems_link_uart;
