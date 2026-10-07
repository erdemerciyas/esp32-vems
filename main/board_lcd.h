#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#define BOARD_LCD_H_RES     480
#define BOARD_LCD_V_RES     480

/**
 * Waveshare ESP32-S3-Touch-LCD-2.1: TCA9554 IO expander (I2C), ST7701S init over 3-wire SPI,
 * then 16-bit RGB565 panel with the frame buffer in PSRAM.
 */
esp_err_t board_lcd_init(esp_lcd_panel_handle_t *out_panel);

void board_lcd_set_backlight(uint8_t percent);

/** On-board active buzzer (TCA9554 EXIO8): on / off only, the pitch is fixed. */
void board_buzzer_set(bool on);

/** One of the two full-screen PSRAM frame buffers (RGB565), index 0 is shown after init. */
void *board_lcd_frame_buffer(int index);

/**
 * Shows the given frame buffer and blocks until the panel has switched to it at the end of the
 * current frame, so the other buffer can then be redrawn without tearing.
 */
void board_lcd_present(const void *fb);

/** Polls the CST820 touch controller. Returns true while a finger is down. */
bool board_touch_read(uint16_t *x, uint16_t *y);
