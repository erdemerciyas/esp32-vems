idf_component_register(
    SRCS "main.c" "ftdi_host.c" "uart_link.c" "vems_proto.c" "board_lcd.c"
         "dash_ui.c" "ui_common.c" "ui_splash.c" "ui_main.c" "ui_afr.c"
    INCLUDE_DIRS "."
    REQUIRES usb esp_driver_uart esp_driver_i2c esp_driver_spi esp_driver_ledc esp_lcd esp_timer lvgl
)
