/*
 * Minimal USB host driver for FTDI USB-serial chips (FT232R / FT232BM / FT-X / FT2232),
 * used to talk to the FTDI chip that sits on the VEMS v3 board.
 *
 * The ESP32-S3 native USB port (GPIO19/20) runs in host mode. Protocol notes:
 *  - Vendor control requests configure the chip (baud divisor, 8N1, latency timer).
 *  - Every bulk IN packet starts with 2 modem/line status bytes, which are stripped here.
 *  - Bulk OUT carries raw data.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_check.h"
#include "usb/usb_host.h"
#include "vems_link.h"

static const char *TAG = "ftdi";

#define FTDI_VID                0x0403

#define FTDI_SIO_RESET          0x00
#define FTDI_SIO_SET_FLOW_CTRL  0x02
#define FTDI_SIO_SET_BAUDRATE   0x03
#define FTDI_SIO_SET_DATA       0x04
#define FTDI_SIO_SET_LATENCY    0x09

#define FTDI_SIO_PURGE_RX       1
#define FTDI_SIO_PURGE_TX       2
#define FTDI_DATA_8N1           0x0008
#define FTDI_LATENCY_MS         4
#define FTDI_STATUS_BYTES       2

#define RX_STREAM_SIZE          4096
#define OUT_XFER_SIZE           64
#define CTRL_TIMEOUT_MS         1000
#define OUT_TIMEOUT_MS          1000
#define ENUM_RETRY_MS           4000

typedef enum { EVT_NEW_DEV, EVT_DEV_GONE } ftdi_evt_type_t;

typedef struct {
    ftdi_evt_type_t type;
    uint8_t addr;
    usb_device_handle_t dev_hdl;
} ftdi_evt_t;

static struct {
    usb_host_client_handle_t client;
    usb_device_handle_t dev;
    uint8_t intf_num;
    uint8_t ep_in;
    uint8_t ep_out;
    uint16_t mps_in;
    uint16_t port_index;        // wIndex low byte: 0 for single-port chips, intf+1 for multi-port
    usb_transfer_t *xfer_ctrl;
    usb_transfer_t *xfer_in;
    usb_transfer_t *xfer_out;
    SemaphoreHandle_t ctrl_done;
    SemaphoreHandle_t out_done;
    SemaphoreHandle_t ctrl_mutex;
    StreamBufferHandle_t rx;
    QueueHandle_t evt_q;
    uint32_t baud;
    volatile bool ready;
    volatile bool closing;
    volatile bool in_busy;
} s;

// ---------------------------------------------------------------------------
// Baud rate divisor (FT232BM/FT232R/FT-X, 3 MHz base clock, 1/8 fractional steps)
// ---------------------------------------------------------------------------
static uint32_t ftdi_baud_divisor(uint32_t baud)
{
    static const uint8_t frac_code[8] = {0, 3, 2, 4, 1, 5, 6, 7};
    if (baud >= 3000000) {
        return 0;
    }
    if (baud >= 2000000) {
        return 1;
    }
    uint32_t div8 = (3000000u * 8u + baud / 2) / baud;
    return (div8 >> 3) | ((uint32_t)frac_code[div8 & 7] << 14);
}

// ---------------------------------------------------------------------------
// Transfer callbacks (run in the client task context)
// ---------------------------------------------------------------------------
static void ctrl_cb(usb_transfer_t *xfer)
{
    xSemaphoreGive(s.ctrl_done);
}

static void out_cb(usb_transfer_t *xfer)
{
    xSemaphoreGive(s.out_done);
}

static void in_cb(usb_transfer_t *xfer)
{
    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        const uint8_t *p = xfer->data_buffer;
        int left = xfer->actual_num_bytes;
        while (left > 0) {
            int chunk = left < s.mps_in ? left : s.mps_in;
            if (chunk > FTDI_STATUS_BYTES) {
                size_t n = chunk - FTDI_STATUS_BYTES;
                if (xStreamBufferSend(s.rx, p + FTDI_STATUS_BYTES, n, 0) != n) {
                    ESP_LOGW(TAG, "RX buffer overflow");
                }
            }
            p += chunk;
            left -= chunk;
        }
    } else if (xfer->status != USB_TRANSFER_STATUS_CANCELED && xfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
        ESP_LOGW(TAG, "bulk IN status %d", xfer->status);
    }

    if (!s.closing && xfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        if (usb_host_transfer_submit(xfer) == ESP_OK) {
            return;
        }
        ESP_LOGE(TAG, "bulk IN resubmit failed");
    }
    s.in_busy = false;
}

// ---------------------------------------------------------------------------
// Control requests (called from non-client tasks, waits for completion)
// ---------------------------------------------------------------------------
static esp_err_t ftdi_ctrl(uint8_t request, uint16_t value, uint16_t index)
{
    if (s.dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s.ctrl_mutex, portMAX_DELAY);

    usb_setup_packet_t *setup = (usb_setup_packet_t *)s.xfer_ctrl->data_buffer;
    setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_VENDOR | USB_BM_REQUEST_TYPE_RECIP_DEVICE;
    setup->bRequest = request;
    setup->wValue = value;
    setup->wIndex = index;
    setup->wLength = 0;
    s.xfer_ctrl->num_bytes = sizeof(usb_setup_packet_t);
    s.xfer_ctrl->device_handle = s.dev;
    s.xfer_ctrl->bEndpointAddress = 0;
    s.xfer_ctrl->callback = ctrl_cb;
    s.xfer_ctrl->timeout_ms = CTRL_TIMEOUT_MS;

    xSemaphoreTake(s.ctrl_done, 0);
    esp_err_t err = usb_host_transfer_submit_control(s.client, s.xfer_ctrl);
    if (err == ESP_OK) {
        if (xSemaphoreTake(s.ctrl_done, pdMS_TO_TICKS(CTRL_TIMEOUT_MS)) != pdTRUE) {
            err = ESP_ERR_TIMEOUT;
        } else if (s.xfer_ctrl->status != USB_TRANSFER_STATUS_COMPLETED) {
            err = ESP_FAIL;
        }
    }
    xSemaphoreGive(s.ctrl_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "control req 0x%02X failed: %s", request, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t ftdi_apply_baud(uint32_t baud)
{
    uint32_t div = ftdi_baud_divisor(baud);
    uint16_t index = (uint16_t)(((div >> 16) & 0xFF) << (s.port_index ? 8 : 0)) | s.port_index;
    return ftdi_ctrl(FTDI_SIO_SET_BAUDRATE, (uint16_t)(div & 0xFFFF), index);
}

static esp_err_t ftdi_configure(void)
{
    ESP_RETURN_ON_ERROR(ftdi_ctrl(FTDI_SIO_RESET, FTDI_SIO_PURGE_RX, s.port_index), TAG, "purge rx");
    ESP_RETURN_ON_ERROR(ftdi_ctrl(FTDI_SIO_RESET, FTDI_SIO_PURGE_TX, s.port_index), TAG, "purge tx");
    ESP_RETURN_ON_ERROR(ftdi_ctrl(FTDI_SIO_SET_LATENCY, FTDI_LATENCY_MS, s.port_index), TAG, "latency");
    ESP_RETURN_ON_ERROR(ftdi_ctrl(FTDI_SIO_SET_DATA, FTDI_DATA_8N1, s.port_index), TAG, "8N1");
    ESP_RETURN_ON_ERROR(ftdi_ctrl(FTDI_SIO_SET_FLOW_CTRL, 0, s.port_index), TAG, "flow");
    // DTR/RTS are intentionally left untouched so the ECU is never reset by modem lines.
    return ftdi_apply_baud(s.baud);
}

// ---------------------------------------------------------------------------
// Device attach / detach (handled in the manager task)
// ---------------------------------------------------------------------------
static bool find_bulk_endpoints(const usb_config_desc_t *cfg, const usb_intf_desc_t *intf, int intf_offset)
{
    s.ep_in = 0;
    s.ep_out = 0;
    for (int i = 0; i < intf->bNumEndpoints; i++) {
        int off = intf_offset;
        const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(intf, i, cfg->wTotalLength, &off);
        if (ep == NULL || USB_EP_DESC_GET_XFERTYPE(ep) != USB_TRANSFER_TYPE_BULK) {
            continue;
        }
        if (USB_EP_DESC_GET_EP_DIR(ep)) {
            s.ep_in = ep->bEndpointAddress;
            s.mps_in = USB_EP_DESC_GET_MPS(ep);
        } else {
            s.ep_out = ep->bEndpointAddress;
        }
    }
    return s.ep_in && s.ep_out && s.mps_in > FTDI_STATUS_BYTES;
}

static void handle_new_device(uint8_t addr)
{
    if (s.dev != NULL) {
        return;
    }
    usb_device_handle_t dev;
    if (usb_host_device_open(s.client, addr, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "cannot open USB device addr %u", addr);
        return;
    }

    const usb_device_desc_t *dd;
    const usb_config_desc_t *cfg;
    usb_host_get_device_descriptor(dev, &dd);
    ESP_LOGI(TAG, "USB device: VID=%04X PID=%04X bcdDevice=%04X", dd->idVendor, dd->idProduct, dd->bcdDevice);
    if (dd->idVendor != FTDI_VID) {
        ESP_LOGW(TAG, "not an FTDI device, ignored");
        usb_host_device_close(s.client, dev);
        return;
    }
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) {
        usb_host_device_close(s.client, dev);
        return;
    }

    int intf_offset = 0;
    const usb_intf_desc_t *intf = usb_parse_interface_descriptor(cfg, 0, 0, &intf_offset);
    if (intf == NULL || !find_bulk_endpoints(cfg, intf, intf_offset)) {
        ESP_LOGE(TAG, "FTDI bulk endpoints not found");
        usb_host_device_close(s.client, dev);
        return;
    }
    s.intf_num = intf->bInterfaceNumber;
    s.port_index = (cfg->bNumInterfaces > 1) ? (s.intf_num + 1) : 0;

    if (usb_host_interface_claim(s.client, dev, s.intf_num, 0) != ESP_OK) {
        ESP_LOGE(TAG, "interface claim failed");
        usb_host_device_close(s.client, dev);
        return;
    }
    s.dev = dev;

    if (ftdi_configure() != ESP_OK) {
        ESP_LOGE(TAG, "FTDI configuration failed");
        usb_host_interface_release(s.client, dev, s.intf_num);
        usb_host_device_close(s.client, dev);
        s.dev = NULL;
        return;
    }

    xStreamBufferReset(s.rx);
    s.closing = false;
    s.xfer_in->device_handle = s.dev;
    s.xfer_in->bEndpointAddress = s.ep_in;
    s.xfer_in->callback = in_cb;
    s.xfer_in->num_bytes = s.mps_in;
    s.in_busy = true;
    if (usb_host_transfer_submit(s.xfer_in) != ESP_OK) {
        s.in_busy = false;
        ESP_LOGE(TAG, "bulk IN submit failed");
        return;
    }
    s.ready = true;
    ESP_LOGI(TAG, "FTDI ready (EP IN 0x%02X, EP OUT 0x%02X, MPS %u, %lu baud 8N1)",
             s.ep_in, s.ep_out, s.mps_in, (unsigned long)s.baud);
}

static void handle_device_gone(usb_device_handle_t dev_hdl)
{
    if (s.dev == NULL || dev_hdl != s.dev) {
        return;
    }
    ESP_LOGW(TAG, "FTDI device disconnected");
    s.ready = false;
    s.closing = true;

    usb_host_endpoint_halt(s.dev, s.ep_in);
    usb_host_endpoint_flush(s.dev, s.ep_in);
    usb_host_endpoint_clear(s.dev, s.ep_in);
    usb_host_endpoint_halt(s.dev, s.ep_out);
    usb_host_endpoint_flush(s.dev, s.ep_out);
    usb_host_endpoint_clear(s.dev, s.ep_out);
    for (int i = 0; i < 100 && s.in_busy; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    usb_host_interface_release(s.client, s.dev, s.intf_num);
    usb_host_device_close(s.client, s.dev);
    s.dev = NULL;
}

// ---------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------
static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    ftdi_evt_t evt = {0};
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        evt.type = EVT_NEW_DEV;
        evt.addr = msg->new_dev.address;
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        evt.type = EVT_DEV_GONE;
        evt.dev_hdl = msg->dev_gone.dev_hdl;
    } else {
        return;
    }
    xQueueSend(s.evt_q, &evt, 0);
}

static void usb_lib_task(void *arg)
{
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void usb_client_task(void *arg)
{
    while (1) {
        usb_host_client_handle_events(s.client, portMAX_DELAY);
    }
}

static void ftdi_manager_task(void *arg)
{
    // Devices that were already enumerated before the client registered
    uint8_t addrs[8];
    int num = 0;
    if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &num) == ESP_OK) {
        for (int i = 0; i < num; i++) {
            handle_new_device(addrs[i]);
        }
    }

    ftdi_evt_t evt;
    while (1) {
        if (xQueueReceive(s.evt_q, &evt, pdMS_TO_TICKS(ENUM_RETRY_MS)) != pdTRUE) {
            // The host library does not retry a failed enumeration: power-cycle the root port
            if (s.dev == NULL) {
                ESP_LOGW(TAG, "no FTDI yet, re-triggering USB enumeration");
                usb_host_lib_set_root_port_power(false);
                vTaskDelay(pdMS_TO_TICKS(200));
                usb_host_lib_set_root_port_power(true);
            }
            continue;
        }
        if (evt.type == EVT_NEW_DEV) {
            handle_new_device(evt.addr);
        } else {
            handle_device_gone(evt.dev_hdl);
        }
    }
}

// ---------------------------------------------------------------------------
// vems_link_t implementation
// ---------------------------------------------------------------------------
static esp_err_t ftdi_start(uint32_t baud)
{
    s.baud = baud;
    s.ctrl_done = xSemaphoreCreateBinary();
    s.out_done = xSemaphoreCreateBinary();
    s.ctrl_mutex = xSemaphoreCreateMutex();
    s.rx = xStreamBufferCreate(RX_STREAM_SIZE, 1);
    s.evt_q = xQueueCreate(8, sizeof(ftdi_evt_t));
    ESP_RETURN_ON_FALSE(s.ctrl_done && s.out_done && s.ctrl_mutex && s.rx && s.evt_q, ESP_ERR_NO_MEM, TAG, "alloc");

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_RETURN_ON_ERROR(usb_host_install(&host_cfg), TAG, "usb_host_install");
    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL, 0);

    const usb_host_client_config_t client_cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    ESP_RETURN_ON_ERROR(usb_host_client_register(&client_cfg, &s.client), TAG, "client register");

    ESP_RETURN_ON_ERROR(usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 64, 0, &s.xfer_ctrl), TAG, "xfer ctrl");
    ESP_RETURN_ON_ERROR(usb_host_transfer_alloc(512, 0, &s.xfer_in), TAG, "xfer in");
    ESP_RETURN_ON_ERROR(usb_host_transfer_alloc(OUT_XFER_SIZE, 0, &s.xfer_out), TAG, "xfer out");

    xTaskCreatePinnedToCore(usb_client_task, "usb_client", 4096, NULL, 9, NULL, 0);
    xTaskCreatePinnedToCore(ftdi_manager_task, "ftdi_mgr", 4096, NULL, 8, NULL, 0);
    ESP_LOGI(TAG, "USB host started, waiting for VEMS FTDI on the native USB port...");
    return ESP_OK;
}

static bool ftdi_is_ready(void)
{
    return s.ready;
}

static esp_err_t ftdi_set_baud(uint32_t baud)
{
    s.baud = baud;
    if (!s.ready) {
        return ESP_OK;  // applied on next attach
    }
    return ftdi_apply_baud(baud);
}

static esp_err_t ftdi_write(const uint8_t *data, size_t len, TickType_t timeout)
{
    while (len > 0) {
        if (!s.ready) {
            return ESP_ERR_INVALID_STATE;
        }
        size_t n = len < OUT_XFER_SIZE ? len : OUT_XFER_SIZE;
        memcpy(s.xfer_out->data_buffer, data, n);
        s.xfer_out->num_bytes = n;
        s.xfer_out->device_handle = s.dev;
        s.xfer_out->bEndpointAddress = s.ep_out;
        s.xfer_out->callback = out_cb;
        s.xfer_out->timeout_ms = OUT_TIMEOUT_MS;

        xSemaphoreTake(s.out_done, 0);
        ESP_RETURN_ON_ERROR(usb_host_transfer_submit(s.xfer_out), TAG, "bulk OUT submit");
        if (xSemaphoreTake(s.out_done, timeout) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        if (s.xfer_out->status != USB_TRANSFER_STATUS_COMPLETED) {
            return ESP_FAIL;
        }
        data += n;
        len -= n;
    }
    return ESP_OK;
}

static size_t ftdi_read(uint8_t *buf, size_t len, TickType_t timeout)
{
    return xStreamBufferReceive(s.rx, buf, len, timeout);
}

static void ftdi_flush_rx(void)
{
    uint8_t tmp[64];
    while (xStreamBufferReceive(s.rx, tmp, sizeof(tmp), 0) > 0) {
    }
}

const vems_link_t vems_link_usb_ftdi = {
    .name = "USB-FTDI",
    .start = ftdi_start,
    .is_ready = ftdi_is_ready,
    .set_baud = ftdi_set_baud,
    .write = ftdi_write,
    .read = ftdi_read,
    .flush_rx = ftdi_flush_rx,
};
