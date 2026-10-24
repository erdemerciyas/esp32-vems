set(srcs "main.c" "settings.c" "srclog.c" "obd.c" "obd_pid.c")

if(CONFIG_VEMS_UI_ENABLE)
    list(APPEND srcs "board_lcd.c"
         "dash_ui.c" "ui_common.c" "ui_channels.c" "ui_splash.c" "ui_main.c" "ui_focus.c" "ui_list.c"
         "ui_afr.c" "ui_dtc.c" "ui_settings.c" "ui_alert.c"
         "ui_font_big.c" "ui_font_value.c" "ui_font_label.c" "ui_font_label_lg.c")
endif()

# REQUIRES, Kconfig yüklenmeden çözülür: CONFIG_VEMS_UI_ENABLE değerine bağlanamaz
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_gpio esp_driver_uart esp_driver_i2c esp_driver_spi esp_driver_ledc esp_lcd esp_timer nvs_flash lvgl
)
