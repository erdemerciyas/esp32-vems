set(srcs "main.c" "ftdi_host.c" "uart_link.c" "vems_proto.c" "board_lcd.c"
         "dash_ui.c" "ui_common.c" "ui_splash.c" "ui_main.c" "ui_afr.c"
         "img_extremeecu_logo.c" "img_extremeecu_logo_small.c"
         "ui_font_big.c" "ui_font_value.c" "ui_font_label.c" "ui_font_label_lg.c")

if(CONFIG_VEMS_CAN_ENABLE)
    list(APPEND srcs "can_out.c" "can_rx8.c")
    if(CONFIG_VEMS_CAN_HW_TWAI)
        list(APPEND srcs "can_twai.c")
    else()
        list(APPEND srcs "mcp2515.c")
    endif()
    if(CONFIG_VEMS_UI_ENABLE)
        list(APPEND srcs "ui_can.c")
    endif()
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES usb driver esp_driver_uart esp_driver_i2c esp_driver_spi esp_driver_ledc esp_lcd esp_timer nvs_flash lvgl
)
