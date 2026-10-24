#pragma once

#include <stdint.h>

/*
 * User settings changed on the device (settings page) and kept in NVS.
 * Defaults come from menuconfig. Read from any task, written only by the UI task.
 */

typedef enum { SET_PROTO_AUTO = 0, SET_PROTO_CAN, SET_PROTO_KLINE } set_proto_t;
typedef enum { SET_PRESS_MAP = 0, SET_PRESS_BOOST } set_press_t;
typedef enum { SET_VIEW_RPM = 0, SET_VIEW_SPEED } set_view_t;

typedef struct {
    uint8_t brightness;     // 5..100 %
    uint16_t shift_rpm;     // 3000..9500
    uint16_t rpm_max;       // 6000..10000
    uint8_t fuel;           // index into SETTINGS_FUELS
    uint8_t pressure;       // set_press_t: drive page shows MAP kPa or boost bar
    uint8_t proto;          // set_proto_t, OBD source only
    uint8_t clt_warn;       // 95..115 C
    uint8_t batt_low;       // 105..125 = 10.5..12.5 V
    uint8_t beep;           // boot melody 0 / 1
    uint8_t main_view;      // set_view_t: sürüş sayfasında büyük gösterilen değer
} settings_t;

typedef struct {
    const char *name;
    float stoich;
} settings_fuel_t;

extern const settings_fuel_t SETTINGS_FUELS[];
extern const int SETTINGS_FUEL_COUNT;

extern settings_t g_settings;

/** Initialises NVS and loads the settings (menuconfig defaults if nothing valid is stored). */
void settings_init(void);
void settings_save(void);
void settings_reset(void);
