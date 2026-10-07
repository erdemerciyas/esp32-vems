/*
 * Display bring-up for the Waveshare ESP32-S3-Touch-LCD-2.1 (480x480 round IPS, ST7701S).
 * Pinout, init sequence and timings taken from the Waveshare ESP-IDF demo.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_check.h"
#include "esp_log.h"
#include "board_lcd.h"

static const char *TAG = "lcd";

// I2C (shared with touch / RTC / IMU)
#define I2C_SDA_GPIO        15
#define I2C_SCL_GPIO        7
#define TCA9554_ADDR        0x20
#define TCA9554_REG_OUTPUT  0x01
#define TCA9554_REG_CONFIG  0x03
#define EXIO_LCD_RST        (1 << 0)    // EXIO1
#define EXIO_TP_RST         (1 << 1)    // EXIO2
#define EXIO_LCD_CS         (1 << 2)    // EXIO3
#define EXIO_BUZZER         (1 << 7)    // EXIO8

// ST7701S 3-wire SPI (9 bit: D/C + 8 bit)
#define LCD_SPI_HOST        SPI2_HOST
#define LCD_SPI_MOSI        1
#define LCD_SPI_SCLK        2

// RGB interface: two PSRAM frame buffers (LVGL renders into the hidden one, swapped at frame end)
// and internal bounce buffers so heavy PSRAM traffic from rendering can't starve the LCD DMA
#define LCD_PCLK_HZ         (18 * 1000 * 1000)
#define LCD_NUM_FBS         2
#define LCD_BOUNCE_LINES    20      // more slack for the refill ISR while LVGL hammers PSRAM
#define LCD_GPIO_BL         6
#define LCD_GPIO_HSYNC      38
#define LCD_GPIO_VSYNC      39
#define LCD_GPIO_DE         40
#define LCD_GPIO_PCLK       41

// CST820 capacitive touch
#define CST820_ADDR         0x15
#define CST820_REG_COUNT    0x02
#define CST820_REG_POINT    0x03

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_tca;
static i2c_master_dev_handle_t s_touch;
static uint8_t s_exio = EXIO_LCD_RST | EXIO_TP_RST | EXIO_LCD_CS;
static spi_device_handle_t s_spi;
static esp_lcd_panel_handle_t s_panel;
static void *s_fbs[LCD_NUM_FBS];
static SemaphoreHandle_t s_frame_done;

// ---------------------------------------------------------------------------
// TCA9554 IO expander
// ---------------------------------------------------------------------------
static esp_err_t tca_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_tca, buf, sizeof(buf), 100);
}

static esp_err_t exio_set(uint8_t mask, bool high)
{
    s_exio = high ? (s_exio | mask) : (s_exio & ~mask);
    return tca_write(TCA9554_REG_OUTPUT, s_exio);
}

static esp_err_t io_expander_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c), TAG, "i2c bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9554_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &dev_cfg, &s_tca), TAG, "tca9554");

    // outputs first (LCD/TP reset released, CS high, buzzer off), then all pins as outputs
    ESP_RETURN_ON_ERROR(tca_write(TCA9554_REG_OUTPUT, s_exio), TAG, "tca output");
    return tca_write(TCA9554_REG_CONFIG, 0x00);
}

// ---------------------------------------------------------------------------
// ST7701S register init
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[16];
    uint16_t delay_ms;
} st7701_cmd_t;

static const st7701_cmd_t ST7701_INIT[] = {
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x10}, 0},
    {0xC0, 2, {0x3B, 0x00}, 0},
    {0xC1, 2, {0x0B, 0x02}, 0},
    {0xC2, 2, {0x07, 0x02}, 0},
    {0xCC, 1, {0x10}, 0},
    {0xCD, 1, {0x08}, 0},
    {0xB0, 16, {0x00, 0x11, 0x16, 0x0E, 0x11, 0x06, 0x05, 0x09, 0x08, 0x21, 0x06, 0x13, 0x10, 0x29, 0x31, 0x18}, 0},
    {0xB1, 16, {0x00, 0x11, 0x16, 0x0E, 0x11, 0x07, 0x05, 0x09, 0x09, 0x21, 0x05, 0x13, 0x11, 0x2A, 0x31, 0x18}, 0},
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x11}, 0},
    {0xB0, 1, {0x6D}, 0},
    {0xB1, 1, {0x37}, 0},
    {0xB2, 1, {0x81}, 0},
    {0xB3, 1, {0x80}, 0},
    {0xB5, 1, {0x43}, 0},
    {0xB7, 1, {0x85}, 0},
    {0xB8, 1, {0x20}, 0},
    {0xC1, 1, {0x78}, 0},
    {0xC2, 1, {0x78}, 0},
    {0xD0, 1, {0x88}, 0},
    {0xE0, 3, {0x00, 0x00, 0x02}, 0},
    {0xE1, 11, {0x03, 0xA0, 0x00, 0x00, 0x04, 0xA0, 0x00, 0x00, 0x00, 0x20, 0x20}, 0},
    {0xE2, 13, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 0},
    {0xE3, 4, {0x00, 0x00, 0x11, 0x00}, 0},
    {0xE4, 2, {0x22, 0x00}, 0},
    {0xE5, 16, {0x05, 0xEC, 0xA0, 0xA0, 0x07, 0xEE, 0xA0, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 0},
    {0xE6, 4, {0x00, 0x00, 0x11, 0x00}, 0},
    {0xE7, 2, {0x22, 0x00}, 0},
    {0xE8, 16, {0x06, 0xED, 0xA0, 0xA0, 0x08, 0xEF, 0xA0, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 0},
    {0xEB, 7, {0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x00}, 0},
    {0xED, 16, {0xFF, 0xFF, 0xFF, 0xBA, 0x0A, 0xBF, 0x45, 0xFF, 0xFF, 0x54, 0xFB, 0xA0, 0xAB, 0xFF, 0xFF, 0xFF}, 0},
    {0xEF, 6, {0x10, 0x0D, 0x04, 0x08, 0x3F, 0x1F}, 0},
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x13}, 0},
    {0xEF, 1, {0x08}, 0},
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x00}, 0},
    {0x36, 1, {0x00}, 0},
    {0x3A, 1, {0x66}, 0},
    {0x11, 0, {0}, 480},    // sleep out
    {0x20, 0, {0}, 120},    // inversion off
    {0x29, 0, {0}, 0},      // display on
};

static void st7701_write9(bool is_data, uint8_t value)
{
    spi_transaction_t t = {
        .cmd = is_data ? 1 : 0,
        .addr = value,
        .length = 0,
    };
    spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t st7701_init_registers(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = LCD_SPI_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LCD_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_DISABLED), TAG, "spi bus");
    spi_device_interface_config_t dev_cfg = {
        .command_bits = 1,      // D/C bit
        .address_bits = 8,      // the actual byte
        .clock_speed_hz = 4 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,     // CS is EXIO3
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(LCD_SPI_HOST, &dev_cfg, &s_spi), TAG, "spi dev");

    // hardware reset, then select the controller
    exio_set(EXIO_LCD_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    exio_set(EXIO_LCD_RST, true);
    vTaskDelay(pdMS_TO_TICKS(50));
    exio_set(EXIO_LCD_CS, false);
    vTaskDelay(pdMS_TO_TICKS(20));

    for (size_t i = 0; i < sizeof(ST7701_INIT) / sizeof(ST7701_INIT[0]); i++) {
        const st7701_cmd_t *c = &ST7701_INIT[i];
        st7701_write9(false, c->cmd);
        for (int j = 0; j < c->len; j++) {
            st7701_write9(true, c->data[j]);
        }
        if (c->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }

    exio_set(EXIO_LCD_CS, true);
    spi_bus_remove_device(s_spi);
    spi_bus_free(LCD_SPI_HOST);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Backlight
// ---------------------------------------------------------------------------
static void backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);
    ledc_channel_config_t ch = {
        .gpio_num = LCD_GPIO_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ledc_channel_config(&ch);
}

void board_buzzer_set(bool on)
{
    exio_set(EXIO_BUZZER, on);
}

void board_lcd_set_backlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (1023u * percent) / 100u);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// ---------------------------------------------------------------------------
// CST820 touch (polled, no INT pin used)
// ---------------------------------------------------------------------------
static esp_err_t touch_init(void)
{
    exio_set(EXIO_TP_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    exio_set(EXIO_TP_RST, true);
    vTaskDelay(pdMS_TO_TICKS(50));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CST820_ADDR,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(s_i2c, &dev_cfg, &s_touch);
}

bool board_touch_read(uint16_t *x, uint16_t *y)
{
    if (s_touch == NULL) {
        return false;
    }
    uint8_t reg = CST820_REG_COUNT;
    uint8_t cnt = 0;
    if (i2c_master_transmit_receive(s_touch, &reg, 1, &cnt, 1, 20) != ESP_OK || (cnt & 0x0F) == 0) {
        return false;
    }
    uint8_t p[4];
    reg = CST820_REG_POINT;
    if (i2c_master_transmit_receive(s_touch, &reg, 1, p, sizeof(p), 20) != ESP_OK) {
        return false;
    }
    *x = ((p[0] & 0x0F) << 8) | p[1];
    *y = ((p[2] & 0x0F) << 8) | p[3];
    return *x < BOARD_LCD_H_RES && *y < BOARD_LCD_V_RES;
}

// ---------------------------------------------------------------------------
// Frame buffer swap, synchronised to the end of the scanned-out frame
// ---------------------------------------------------------------------------
// The bounce buffer ISR picks up the newly selected frame buffer when it wraps to the next frame
// and then calls this, so from here on the previous buffer is no longer read and may be redrawn.
static bool IRAM_ATTR on_frame_done(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *edata,
                                    void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_frame_done, &woken);
    return woken == pdTRUE;
}

void *board_lcd_frame_buffer(int index)
{
    return (index >= 0 && index < LCD_NUM_FBS) ? s_fbs[index] : NULL;
}

void board_lcd_present(const void *fb)
{
    xSemaphoreTake(s_frame_done, 0);    // drop an event from a frame that ended before this swap
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES, fb);
    xSemaphoreTake(s_frame_done, pdMS_TO_TICKS(50));
}

// ---------------------------------------------------------------------------
esp_err_t board_lcd_init(esp_lcd_panel_handle_t *out_panel)
{
    backlight_init();
    ESP_RETURN_ON_ERROR(io_expander_init(), TAG, "io expander");
    ESP_RETURN_ON_ERROR(st7701_init_registers(), TAG, "st7701");

    s_frame_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_frame_done, ESP_ERR_NO_MEM, TAG, "frame semaphore");

    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_width = 16,
        .num_fbs = LCD_NUM_FBS,
        .bounce_buffer_size_px = BOARD_LCD_H_RES * LCD_BOUNCE_LINES,
        .dma_burst_size = 64,
        .hsync_gpio_num = LCD_GPIO_HSYNC,
        .vsync_gpio_num = LCD_GPIO_VSYNC,
        .de_gpio_num = LCD_GPIO_DE,
        .pclk_gpio_num = LCD_GPIO_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            5, 45, 48, 47, 21,          // B0..B4
            14, 13, 12, 11, 10, 9,      // G0..G5
            46, 3, 8, 18, 17,           // R0..R4
        },
        .timings = {
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = BOARD_LCD_H_RES,
            .v_res = BOARD_LCD_V_RES,
            .hsync_back_porch = 10,
            .hsync_front_porch = 50,
            .hsync_pulse_width = 8,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .vsync_pulse_width = 3,
        },
        .flags.fb_in_psram = true,
    };
    esp_lcd_panel_handle_t panel;
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&cfg, &panel), TAG, "rgb panel");
    const esp_lcd_rgb_panel_event_callbacks_t cbs = {.on_bounce_frame_finish = on_frame_done};
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_register_event_callbacks(panel, &cbs, NULL), TAG, "panel callbacks");
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_get_frame_buffer(panel, LCD_NUM_FBS, &s_fbs[0], &s_fbs[1]), TAG, "fbs");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");
    s_panel = panel;

    if (touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "CST820 touch not available");
    }

    *out_panel = panel;
    ESP_LOGI(TAG, "ST7701S 480x480 RGB panel ready");
    return ESP_OK;
}
