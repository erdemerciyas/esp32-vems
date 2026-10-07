/*
 * Plain UART transport for VEMS (direct TTL wiring or RS232 through a MAX3232 module).
 * VEMS TX is 5 V TTL on the processor side: use a level shifter / divider on the ESP32 RX pin.
 */
#include "sdkconfig.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "vems_link.h"

#ifdef CONFIG_VEMS_TRANSPORT_UART
#define VEMS_UART_PORT  CONFIG_VEMS_UART_PORT
#define VEMS_UART_TX    CONFIG_VEMS_UART_TX_GPIO
#define VEMS_UART_RX    CONFIG_VEMS_UART_RX_GPIO
#else
#define VEMS_UART_PORT  1
#define VEMS_UART_TX    UART_PIN_NO_CHANGE
#define VEMS_UART_RX    UART_PIN_NO_CHANGE
#endif

static const char *TAG = "uart_link";
static bool s_ready;

static esp_err_t uart_link_start(uint32_t baud)
{
    const uart_config_t cfg = {
        .baud_rate = (int)baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(VEMS_UART_PORT, 2048, 0, 0, NULL, 0), TAG, "install");
    ESP_RETURN_ON_ERROR(uart_param_config(VEMS_UART_PORT, &cfg), TAG, "config");
    ESP_RETURN_ON_ERROR(uart_set_pin(VEMS_UART_PORT, VEMS_UART_TX, VEMS_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "pins");
    s_ready = true;
    ESP_LOGI(TAG, "UART%d ready: TX=GPIO%d RX=GPIO%d %lu baud 8N1", VEMS_UART_PORT, VEMS_UART_TX, VEMS_UART_RX, (unsigned long)baud);
    return ESP_OK;
}

static bool uart_link_is_ready(void)
{
    return s_ready;
}

static esp_err_t uart_link_set_baud(uint32_t baud)
{
    return uart_set_baudrate(VEMS_UART_PORT, baud);
}

static esp_err_t uart_link_write(const uint8_t *data, size_t len, TickType_t timeout)
{
    if (uart_write_bytes(VEMS_UART_PORT, data, len) != (int)len) {
        return ESP_FAIL;
    }
    return uart_wait_tx_done(VEMS_UART_PORT, timeout);
}

static size_t uart_link_read(uint8_t *buf, size_t len, TickType_t timeout)
{
    size_t avail = 0;
    uart_get_buffered_data_len(VEMS_UART_PORT, &avail);
    if (avail == 0) {
        int n = uart_read_bytes(VEMS_UART_PORT, buf, 1, timeout);
        if (n <= 0) {
            return 0;
        }
        uart_get_buffered_data_len(VEMS_UART_PORT, &avail);
        size_t more = avail < len - 1 ? avail : len - 1;
        int m = more ? uart_read_bytes(VEMS_UART_PORT, buf + 1, more, 0) : 0;
        return 1 + (m > 0 ? m : 0);
    }
    int n = uart_read_bytes(VEMS_UART_PORT, buf, avail < len ? avail : len, 0);
    return n > 0 ? n : 0;
}

static void uart_link_flush_rx(void)
{
    uart_flush_input(VEMS_UART_PORT);
}

const vems_link_t vems_link_uart = {
    .name = "UART",
    .start = uart_link_start,
    .is_ready = uart_link_is_ready,
    .set_baud = uart_link_set_baud,
    .write = uart_link_write,
    .read = uart_link_read,
    .flush_rx = uart_link_flush_rx,
};
