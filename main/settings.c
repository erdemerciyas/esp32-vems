#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "settings.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE   "vems"
#define NVS_KEY         "settings"
#define SETTINGS_VER    2

const settings_fuel_t SETTINGS_FUELS[] = {
    {"Benzin", 14.7f},
    {"E85", 9.8f},
    {"LPG", 15.5f},
    {"Metanol", 6.4f},
    {"Dizel", 14.5f},
};
const int SETTINGS_FUEL_COUNT = sizeof(SETTINGS_FUELS) / sizeof(SETTINGS_FUELS[0]);

settings_t g_settings;

typedef struct {
    uint8_t ver;
    settings_t s;
} stored_t;

static settings_t defaults(void)
{
    settings_t s = {
#if CONFIG_VEMS_UI_ENABLE
        .brightness = CONFIG_VEMS_UI_BACKLIGHT,
        .shift_rpm = CONFIG_VEMS_UI_RPM_SHIFT,
        .rpm_max = CONFIG_VEMS_UI_RPM_MAX,
#else
        .brightness = 80,
        .shift_rpm = 6500,
        .rpm_max = 8000,
#endif
#if CONFIG_VEMS_UI_FUEL_E85
        .fuel = 1,
#elif CONFIG_VEMS_UI_FUEL_LPG
        .fuel = 2,
#elif CONFIG_VEMS_UI_FUEL_METHANOL
        .fuel = 3,
#else
        .fuel = 0,
#endif
        .pressure = SET_PRESS_MAP,
        .proto = SET_PROTO_AUTO,
        .clt_warn = 105,
        .batt_low = 115,
#if CONFIG_VEMS_UI_BOOT_BEEP
        .beep = 1,
#else
        .beep = 0,
#endif
        .main_view = SET_VIEW_RPM,
    };
    return s;
}

static bool valid(const settings_t *s)
{
    return s->brightness >= 5 && s->brightness <= 100 &&
           s->shift_rpm >= 3000 && s->shift_rpm <= 9500 &&
           s->rpm_max >= 6000 && s->rpm_max <= 10000 &&
           s->fuel < SETTINGS_FUEL_COUNT && s->pressure <= SET_PRESS_BOOST && s->proto <= SET_PROTO_KLINE &&
           s->clt_warn >= 95 && s->clt_warn <= 115 && s->batt_low >= 105 && s->batt_low <= 125 && s->beep <= 1 &&
           s->main_view <= SET_VIEW_SPEED;
}

void settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    g_settings = defaults();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed (%s), using defaults", esp_err_to_name(err));
        return;
    }

    nvs_handle_t h;
    stored_t st;
    size_t len = sizeof(st);
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, NVS_KEY, &st, &len) == ESP_OK && len == sizeof(st) && st.ver == SETTINGS_VER && valid(&st.s)) {
            g_settings = st.s;
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "brightness %u%%, shift %u/%u rpm, fuel %s, proto %u",
             g_settings.brightness, g_settings.shift_rpm, g_settings.rpm_max,
             SETTINGS_FUELS[g_settings.fuel].name, g_settings.proto);
}

void settings_save(void)
{
    stored_t st = {.ver = SETTINGS_VER, .s = g_settings};
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY, &st, sizeof(st));
        nvs_commit(h);
        nvs_close(h);
    }
}

void settings_reset(void)
{
    g_settings = defaults();
    settings_save();
}
