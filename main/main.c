/*
 * ESP32-S3 <-> VEMS v3 (firmware 1.2.x) live data bridge.
 * Polls realtime data from the ECU and prints it on the console (UART0 / CH343P "UART" Type-C port).
 */
#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "vems_proto.h"
#if CONFIG_VEMS_UI_ENABLE
#include "board_lcd.h"
#include "dash_ui.h"
#endif

static const char *TAG = "main";

#if CONFIG_VEMS_TRANSPORT_UART
#define VEMS_LINK vems_link_uart
#else
#define VEMS_LINK vems_link_usb_ftdi
#endif

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

        printf("RPM %5u | MAP %5.1f kPa | CLT %5.1f C (r%u) | IAT %5.1f C (r%u) | TPS %5.1f %% | BAT %4.1f V | "
               "LAMBDA %.2f (AFR %4.1f) | ADV %5.1f | PW %5.2f ms | VE %3u | %s%s%s| ok=%lu to=%lu crc=%lu age=%lldms%s\n",
               d.rpm, d.map_kpa, d.clt_c, d.raw[7], d.iat_c, d.raw[6], d.tps_pct, d.batt_v,
               d.lambda, d.afr, d.ign_adv_deg, d.pulsewidth_ms, d.ve,
               (d.engine_status & VEMS_ENG_RUNNING) ? "RUN " : "",
               (d.engine_status & VEMS_ENG_CRANK) ? "CRANK " : "",
               (d.engine_status & VEMS_ENG_WARMUP) ? "WARM " : "",
               (unsigned long)st.frames_ok, (unsigned long)st.timeouts, (unsigned long)st.crc_errors,
               age_ms, d.seq == last_seq ? " (STALE)" : "");

#if CONFIG_VEMS_PRINT_JSON
        printf("{\"seq\":%lu,\"rpm\":%u,\"map\":%.1f,\"clt\":%.1f,\"iat\":%.1f,\"tps\":%.1f,\"batt\":%.2f,"
               "\"lambda\":%.3f,\"afr\":%.2f,\"lambda_tgt\":%.3f,\"adv\":%.1f,\"pw\":%.3f,\"dwell\":%.2f,\"ve\":%u,"
               "\"ego_corr\":%.0f,\"warm\":%.0f,\"iac\":%.1f,\"status\":%u,\"status1\":%u}\n",
               (unsigned long)d.seq, d.rpm, d.map_kpa, d.clt_c, d.iat_c, d.tps_pct, d.batt_v,
               d.lambda, d.afr, d.lambda_target, d.ign_adv_deg, d.pulsewidth_ms, d.dwell_ms, d.ve,
               d.ego_corr_pct, d.warmup_pct, d.iac_pct, d.engine_status, d.status1);
#endif
        last_seq = d.seq;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "EXTREMEECU dash, link: %s, %d baud", VEMS_LINK.name, CONFIG_VEMS_BAUD);

#if CONFIG_VEMS_UI_ENABLE
    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(board_lcd_init(&panel));
    ESP_ERROR_CHECK(dash_ui_start(panel));
    // let the splash render its first frame, then fade the backlight in
    vTaskDelay(pdMS_TO_TICKS(120));
    for (int bl = 0; bl <= CONFIG_VEMS_UI_BACKLIGHT; bl += 2) {
        board_lcd_set_backlight(bl);
        vTaskDelay(pdMS_TO_TICKS(12));
    }
    board_lcd_set_backlight(CONFIG_VEMS_UI_BACKLIGHT);
    vTaskDelay(pdMS_TO_TICKS(400));    // "EKRAN" step visible before USB host starts
#endif

    ESP_ERROR_CHECK(vems_proto_start(&VEMS_LINK));
    if (CONFIG_VEMS_PRINT_INTERVAL_MS > 0) {
        xTaskCreatePinnedToCore(print_task, "print", 4096, NULL, 3, NULL, 1);
    }
}
