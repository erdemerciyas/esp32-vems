/*
 * K-Line OBD: ESP32-S3 OBD-II K-Line canlı veri göstergesi.
 * ECU'dan canlı veriyi sorgular, Waveshare 2.1" yuvarlak ekranda gösterir ve konsola (USB-Serial-JTAG,
 * kartın "USB" Type-C portu) yazar.
 */
#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "vems_proto.h"
#include "settings.h"
#include "srclog.h"
#if CONFIG_VEMS_UI_ENABLE
#include "board_lcd.h"
#include "dash_ui.h"
#endif

static const char *TAG = "main";

static void print_task(void *arg)
{
    vems_data_t d;
    vems_stats_t st;
    uint32_t last_seq = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_VEMS_PRINT_INTERVAL_MS));
        if (!vems_get_latest(&d)) {
            continue;
        }
        vems_get_stats(&st);
        int64_t age_ms = (esp_timer_get_time() - d.timestamp_us) / 1000;

        // ECU'nun göndermediği değerler NAN
        printf("RPM %5u | SPD %3.0f km/h | MAP %5.1f kPa | CLT %5.1f C | IAT %5.1f C | TPS %5.1f %% | LOAD %5.1f %% | "
               "BAT %4.1f V | AFR %4.1f | ADV %5.1f | %s | ok=%lu to=%lu age=%lldms%s\n",
               d.rpm, d.speed_kmh, d.map_kpa, d.clt_c, d.iat_c, d.tps_pct, d.load_pct,
               d.batt_v, d.afr, d.ign_adv_deg, vems_mode_name(d.mode),
               (unsigned long)st.frames_ok, (unsigned long)st.timeouts,
               age_ms, d.seq == last_seq ? " (STALE)" : "");

#if CONFIG_VEMS_PRINT_JSON
        printf("{\"seq\":%lu,\"rpm\":%u,\"speed\":%.0f,\"map\":%.1f,\"clt\":%.1f,\"iat\":%.1f,\"tps\":%.1f,"
               "\"load\":%.1f,\"batt\":%.2f,\"afr\":%.2f,\"adv\":%.1f,\"stft\":%.1f,\"ltft\":%.1f,\"maf\":%.2f}\n",
               (unsigned long)d.seq, d.rpm, d.speed_kmh, d.map_kpa, d.clt_c, d.iat_c, d.tps_pct,
               d.load_pct, d.batt_v, d.afr, d.ign_adv_deg, d.ego_corr_pct, d.ltft_pct, d.maf_gs);
#endif
        last_seq = d.seq;
    }
}

#if CONFIG_VEMS_UI_BOOT_BEEP
// The board has an active (fixed pitch) buzzer, so the "melody" is pure rhythm:
// three race start-light beeps, a long GO, then a quick blow-off double chirp.
// ~1.6 s, ends together with the splash intro.
static const struct {
    uint16_t on_ms;
    uint16_t off_ms;
} BOOT_MELODY[] = {
    {70, 230},      // light 1
    {70, 230},      // light 2
    {70, 330},      // light 3
    {350, 120},     // GO
    {40, 60},       // pssh-
    {40, 0},        // -t
};

static void boot_melody_task(void *arg)
{
    for (size_t i = 0; i < sizeof(BOOT_MELODY) / sizeof(BOOT_MELODY[0]); i++) {
        board_buzzer_set(true);
        vTaskDelay(pdMS_TO_TICKS(BOOT_MELODY[i].on_ms));
        board_buzzer_set(false);
        vTaskDelay(pdMS_TO_TICKS(BOOT_MELODY[i].off_ms));
    }
    vTaskDelete(NULL);
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "K-Line OBD, source: OBD-II K-Line");

    settings_init();    // NVS: brightness, beep ... before anything uses them

#if CONFIG_VEMS_UI_ENABLE
    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(board_lcd_init(&panel));
    srclog_step("ekran 480×480");
    srclog_done(SRCLOG_OK, "OK");
    ESP_ERROR_CHECK(dash_ui_start(panel));
    // let the splash render its first frame, then fade the backlight in
    vTaskDelay(pdMS_TO_TICKS(120));
#if CONFIG_VEMS_UI_BOOT_BEEP
    if (g_settings.beep) {
        xTaskCreate(boot_melody_task, "melody", 2048, NULL, 2, NULL);
    }
#endif
    for (int bl = 0; bl <= g_settings.brightness; bl += 2) {
        board_lcd_set_backlight(bl);
        vTaskDelay(pdMS_TO_TICKS(12));
    }
    board_lcd_set_backlight(g_settings.brightness);
    vTaskDelay(pdMS_TO_TICKS(400));    // first boot log line visible before the source starts
#endif

    ESP_ERROR_CHECK(obd_start());
    if (CONFIG_VEMS_PRINT_INTERVAL_MS > 0) {
        xTaskCreatePinnedToCore(print_task, "print", 4096, NULL, 3, NULL, 1);
    }
}
