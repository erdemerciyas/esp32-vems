#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

/** Starts LVGL on the given panel, builds the dashboard and runs the LVGL task. */
esp_err_t dash_ui_start(esp_lcd_panel_handle_t panel);
