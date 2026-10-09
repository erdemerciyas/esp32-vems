/*
 * VEMS -> vehicle CAN bus bridge (HW-184 / MCP2515 on SPI, or internal TWAI + transceiver).
 * The selected vehicle profile emulates the original ECU's broadcast so the stock cluster,
 * ABS/DSC and power steering keep working with VEMS.
 */
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "can_out.h"

#if CONFIG_VEMS_CAN_HW_TWAI
#include "can_twai.h"
#define can_hw_config_t     can_twai_config_t
#define can_hw_init         can_twai_init
#define can_hw_send         can_twai_send
#define can_hw_recv         can_twai_recv
#define can_hw_is_online    can_twai_is_online
#define can_hw_go_offline   can_twai_go_offline
#define can_hw_get_errors   can_twai_get_errors
#else
#include "mcp2515.h"
#define can_hw_config_t     mcp2515_config_t
#define can_hw_init         mcp2515_init
#define can_hw_send         mcp2515_send
#define can_hw_recv         mcp2515_recv
#define can_hw_is_online    mcp2515_is_online
#define can_hw_go_offline   mcp2515_go_offline
#define can_hw_get_errors   mcp2515_get_errors
#endif

static const char *TAG = "can_out";

#define CAN_TICK_MS         5
#define CAN_RX_MAX_PER_TICK 8
#define CAN_PROBE_US        (2 * 1000 * 1000)
#define CAN_CHECK_US        (1000 * 1000)
#define CAN_REPORT_US       (5 * 1000 * 1000)
#define NVS_NAMESPACE       "vems"
#define NVS_KEY_ENABLED     "can_on"

#if CONFIG_VEMS_CAN_VEHICLE_RX8
static const can_profile_t *const s_profile = &can_profile_rx8;
#endif
static can_hw_config_t s_cfg;
static volatile bool s_enabled = true;
static volatile can_out_state_t s_state = CAN_OUT_NO_MODULE;
static volatile uint32_t s_tx_ok;
static volatile uint32_t s_tx_drop;
static volatile uint32_t s_rx;
static bool s_tx_blocked;           // a send timed out this tick: skip the rest instead of busy-waiting

void can_out_send(uint16_t id, const uint8_t *data, uint8_t dlc)
{
    if (s_tx_blocked) {
        s_tx_drop++;
        return;
    }
    can_frame_t f = {.id = id, .dlc = dlc};
    memcpy(f.data, data, dlc > 8 ? 8 : dlc);
    if (can_hw_send(&f) == ESP_OK) {
        s_tx_ok++;
    } else {
        s_tx_drop++;
        s_tx_blocked = true;
    }
}

static void save_enabled(bool on)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_ENABLED, on);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool load_enabled(void)
{
    nvs_handle_t h;
    uint8_t on = 1;     // default on: the feature was selected in menuconfig
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_ENABLED, &on);
        nvs_close(h);
    }
    return on;
}

static void can_task(void *arg)
{
    static vems_data_t d;
    TickType_t wake = xTaskGetTickCount();
    bool ready = false;         // chip configured and on the bus
    bool applied = s_enabled;
    int64_t next_probe = 0, next_check = 0, next_report = 0;
    uint32_t check_drop = 0, report_drop = 0;

    while (1) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(CAN_TICK_MS));
        int64_t now = esp_timer_get_time();

        bool enabled = s_enabled;
        if (enabled != applied) {
            applied = enabled;
            save_enabled(enabled);
            ESP_LOGI(TAG, "output %s", enabled ? "on" : "off");
            if (!enabled && ready) {
                can_hw_go_offline();
                ready = false;
            }
            next_probe = now;
        }
        if (!enabled) {
            s_state = CAN_OUT_OFF;
            continue;
        }

        if (!ready) {
            if (now < next_probe) {
                continue;
            }
            next_probe = now + CAN_PROBE_US;
            if (can_hw_init(&s_cfg) != ESP_OK) {
                s_state = CAN_OUT_NO_MODULE;
                continue;
            }
            ready = true;
            s_state = CAN_OUT_ACTIVE;
            next_check = now + CAN_CHECK_US;
            check_drop = s_tx_drop;
        }

        s_tx_blocked = false;
        can_frame_t f;
        for (int i = 0; i < CAN_RX_MAX_PER_TICK && can_hw_recv(&f); i++) {
            s_rx++;
            s_profile->on_rx(&f, now);
        }

        bool fresh = vems_get_latest(&d) && now - d.timestamp_us < CONFIG_VEMS_CAN_STALE_MS * 1000LL;
        s_profile->tick(fresh ? &d : NULL, now);

        if (now >= next_check) {
            next_check = now + CAN_CHECK_US;
            if (!can_hw_is_online()) {
                ESP_LOGW(TAG, "CAN controller lost (unplugged / power cycled / bus-off), probing again");
                ready = false;
                s_state = CAN_OUT_NO_MODULE;
                continue;
            }
            s_state = s_tx_drop != check_drop ? CAN_OUT_NO_ACK : CAN_OUT_ACTIVE;
            check_drop = s_tx_drop;
        }

        if (now >= next_report) {
            next_report = now + CAN_REPORT_US;
            if (s_tx_drop != report_drop) {
                uint8_t tec, rec, eflg;
                can_hw_get_errors(&tec, &rec, &eflg);
                ESP_LOGW(TAG, "tx dropped %lu (ok %lu, rx %lu), TEC=%u REC=%u flags=0x%02x - nothing ACKs on the bus?",
                         (unsigned long)s_tx_drop, (unsigned long)s_tx_ok, (unsigned long)s_rx, tec, rec, eflg);
                report_drop = s_tx_drop;
            }
        }
    }
}

esp_err_t can_out_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");
    s_enabled = load_enabled();

#if CONFIG_VEMS_CAN_HW_TWAI
    s_cfg = (can_twai_config_t) {
        .tx_gpio = CONFIG_VEMS_CAN_TWAI_TX_GPIO,
        .rx_gpio = CONFIG_VEMS_CAN_TWAI_RX_GPIO,
        .rx_ids = s_profile->rx_ids,
        .rx_id_count = s_profile->rx_id_count,
    };
#else
    s_cfg = (mcp2515_config_t) {
        .host = SPI3_HOST,
        .sclk_gpio = CONFIG_VEMS_CAN_SCLK_GPIO,
        .mosi_gpio = CONFIG_VEMS_CAN_MOSI_GPIO,
        .miso_gpio = CONFIG_VEMS_CAN_MISO_GPIO,
        .cs_gpio = CONFIG_VEMS_CAN_CS_GPIO,
        .spi_hz = CONFIG_VEMS_CAN_SPI_KHZ * 1000,
        .osc_mhz = CONFIG_VEMS_CAN_OSC_MHZ,
        .rx_ids = s_profile->rx_ids,
        .rx_id_count = s_profile->rx_id_count,
    };
#endif
    ESP_LOGI(TAG, "vehicle profile: %s, output %s", s_profile->name, s_enabled ? "on" : "off");

    // core 0 next to USB host, away from LVGL rendering on core 1
    return xTaskCreatePinnedToCore(can_task, "can_out", 4096, NULL, 7, NULL, 0) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void can_out_set_enabled(bool on)
{
    s_enabled = on;
}

bool can_out_get_enabled(void)
{
    return s_enabled;
}

void can_out_get_status(can_out_status_t *out)
{
    out->state = s_state;
    out->tx_ok = s_tx_ok;
    out->tx_drop = s_tx_drop;
    out->rx = s_rx;
}

const char *can_out_profile_name(void)
{
    return s_profile->name;
}
