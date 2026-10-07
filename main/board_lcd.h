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

/** Polls the CST820 touch controller. Returns true while a finger is down. */
bool board_touch_read(uint16_t *x, uint16_t *y);
