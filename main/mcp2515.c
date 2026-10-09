/*
 * Minimal MCP2515 driver (HW-184 module: MCP2515 + TJA1050, 8 MHz crystal) over SPI.
 * Polled, standard 11 bit ids only, fixed 500 kbit/s. Not thread safe: use from a single task.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "mcp2515.h"

static const char *TAG = "mcp2515";

// SPI instructions
#define INSTR_RESET         0xC0
#define INSTR_READ          0x03
#define INSTR_WRITE         0x02
#define INSTR_READ_STATUS   0xA0
#define INSTR_LOAD_TX       0x40    // | (n << 1): TXBnSIDH
#define INSTR_RTS           0x80    // | (1 << n)
#define INSTR_READ_RX0      0x90    // RXB0SIDH, clears RX0IF on CS high
#define INSTR_READ_RX1      0x94

// registers
#define REG_RXF0            0x00
#define REG_RXF1            0x04
#define REG_RXF2            0x08
#define REG_RXF3            0x10
#define REG_RXF4            0x14
#define REG_RXF5            0x18
#define REG_CANSTAT         0x0E
#define REG_CANCTRL         0x0F
#define REG_TEC             0x1C
#define REG_REC             0x1D
#define REG_RXM0            0x20
#define REG_RXM1            0x24
#define REG_CNF3            0x28    // CNF3, CNF2, CNF1 are consecutive
#define REG_CANINTE         0x2B
#define REG_EFLG            0x2D
#define REG_RXB0CTRL        0x60
#define REG_RXB1CTRL        0x70

#define OPMODE_MASK         0xE0
#define OPMODE_NORMAL       0x00
#define OPMODE_CONFIG       0x80

// READ STATUS bits
#define ST_RX0IF            0x01
#define ST_RX1IF            0x02

static const uint8_t TXREQ_BITS[3] = {0x04, 0x10, 0x40};

static spi_device_handle_t s_dev;

static esp_err_t xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_polling_transmit(s_dev, &t);
}

static uint8_t read_reg(uint8_t addr)
{
    uint8_t tx[3] = {INSTR_READ, addr, 0};
    uint8_t rx[3] = {0};
    xfer(tx, rx, sizeof(tx));
    return rx[2];
}

static void write_regs(uint8_t addr, const uint8_t *val, size_t n)
{
    uint8_t tx[2 + 8] = {INSTR_WRITE, addr};
    memcpy(&tx[2], val, n);
    xfer(tx, NULL, 2 + n);
}

static void write_reg(uint8_t addr, uint8_t val)
{
    write_regs(addr, &val, 1);
}

static uint8_t read_status(void)
{
    uint8_t tx[2] = {INSTR_READ_STATUS, 0};
    uint8_t rx[2] = {0};
    xfer(tx, rx, sizeof(tx));
    return rx[1];
}

static void write_id(uint8_t addr, uint16_t id)
{
    uint8_t v[4] = {id >> 3, (id & 0x07) << 5, 0, 0};    // EXIDE = 0: standard frames only
    write_regs(addr, v, sizeof(v));
}

static bool wait_opmode(uint8_t mode)
{
    for (int i = 0; i < 20; i++) {
        if ((read_reg(REG_CANSTAT) & OPMODE_MASK) == mode) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

esp_err_t mcp2515_init(const mcp2515_config_t *cfg)
{
    // 500 kbit/s: 8 TQ @ 8 MHz (sample point 62.5 %) or 16 TQ @ 16 MHz (56 %)
    uint8_t cnf[3];     // CNF3, CNF2, CNF1
    if (cfg->osc_mhz == 8) {
        cnf[0] = 0x02; cnf[1] = 0x90; cnf[2] = 0x00;
    } else if (cfg->osc_mhz == 16) {
        cnf[0] = 0x06; cnf[1] = 0xF0; cnf[2] = 0x00;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_dev) {
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = cfg->mosi_gpio,
            .miso_io_num = cfg->miso_gpio,
            .sclk_io_num = cfg->sclk_gpio,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 32,
        };
        ESP_RETURN_ON_ERROR(spi_bus_initialize(cfg->host, &bus_cfg, SPI_DMA_DISABLED), TAG, "spi bus");
        spi_device_interface_config_t dev_cfg = {
            .clock_speed_hz = cfg->spi_hz,
            .mode = 0,
            .spics_io_num = cfg->cs_gpio,
            .queue_size = 1,
        };
        ESP_RETURN_ON_ERROR(spi_bus_add_device(cfg->host, &dev_cfg, &s_dev), TAG, "spi dev");
    }

    uint8_t reset = INSTR_RESET;
    xfer(&reset, NULL, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t canstat = read_reg(REG_CANSTAT);
    if ((canstat & OPMODE_MASK) != OPMODE_CONFIG) {
        ESP_LOGD(TAG, "no answer after reset (CANSTAT=0x%02x)", canstat);
        return ESP_ERR_NOT_FOUND;
    }

    write_regs(REG_CNF3, cnf, sizeof(cnf));
    write_reg(REG_CANINTE, 0);      // polled, INT pin unused

    // RXB0: RXF0/1, RXB1: RXF2..5; unused slots repeat the list
    static const uint8_t FILTER_REGS[MCP2515_MAX_FILTERS] = {REG_RXF0, REG_RXF1, REG_RXF2, REG_RXF3, REG_RXF4, REG_RXF5};
    size_t n = cfg->rx_id_count > MCP2515_MAX_FILTERS ? MCP2515_MAX_FILTERS : cfg->rx_id_count;
    write_id(REG_RXM0, 0x7FF);
    write_id(REG_RXM1, 0x7FF);
    for (int i = 0; i < MCP2515_MAX_FILTERS; i++) {
        write_id(FILTER_REGS[i], n ? cfg->rx_ids[i % n] : 0x7FF);
    }
    write_reg(REG_RXB0CTRL, 0x04);  // RXM = filters, BUKT: roll over into RXB1
    write_reg(REG_RXB1CTRL, 0x00);

    write_reg(REG_CANCTRL, OPMODE_NORMAL);  // also CLKOUT off, one-shot off
    if (!wait_opmode(OPMODE_NORMAL)) {
        ESP_LOGE(TAG, "did not enter normal mode (CANSTAT=0x%02x)", read_reg(REG_CANSTAT));
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "ready, 500 kbit/s, %d MHz crystal, %u rx filter(s)", cfg->osc_mhz, (unsigned)n);
    return ESP_OK;
}

esp_err_t mcp2515_send(const can_frame_t *f)
{
    uint8_t dlc = f->dlc > 8 ? 8 : f->dlc;
    // 3 TX buffers; a full 8 byte frame takes ~250 us at 500 kbit/s
    for (int tries = 0; tries < 10; tries++) {
        uint8_t st = read_status();
        for (int n = 0; n < 3; n++) {
            if (st & TXREQ_BITS[n]) {
                continue;
            }
            uint8_t tx[6 + 8] = {INSTR_LOAD_TX | (n << 1), f->id >> 3, (f->id & 0x07) << 5, 0, 0, dlc};
            memcpy(&tx[6], f->data, dlc);
            xfer(tx, NULL, 6 + dlc);
            uint8_t rts = INSTR_RTS | (1 << n);
            xfer(&rts, NULL, 1);
            return ESP_OK;
        }
        esp_rom_delay_us(100);
    }
    return ESP_ERR_TIMEOUT;
}

bool mcp2515_recv(can_frame_t *f)
{
    uint8_t st = read_status();
    uint8_t tx[1 + 13] = {0};
    if (st & ST_RX0IF) {
        tx[0] = INSTR_READ_RX0;
    } else if (st & ST_RX1IF) {
        tx[0] = INSTR_READ_RX1;
    } else {
        return false;
    }
    uint8_t rx[1 + 13];
    xfer(tx, rx, sizeof(tx));
    f->id = ((uint16_t)rx[1] << 3) | (rx[2] >> 5);
    f->dlc = (rx[5] & 0x0F) > 8 ? 8 : (rx[5] & 0x0F);
    memcpy(f->data, &rx[6], 8);
    return true;
}

bool mcp2515_is_online(void)
{
    // a power-cycled / unplugged module reads back config mode / 0xFF
    return (read_reg(REG_CANSTAT) & OPMODE_MASK) == OPMODE_NORMAL;
}

void mcp2515_go_offline(void)
{
    write_reg(REG_CANCTRL, OPMODE_CONFIG | 0x10);   // ABAT: drop pending frames, then stop TX and ACK
}

void mcp2515_get_errors(uint8_t *tec, uint8_t *rec, uint8_t *eflg)
{
    *tec = read_reg(REG_TEC);
    *rec = read_reg(REG_REC);
    *eflg = read_reg(REG_EFLG);
}
