/*
 * xbox360.c - Xbox 360 USB host class driver for ESP32-S3 (ESP-IDF)
 *
 * Protocol ported from felis/USB_Host_Shield_2.0 (XBOXRECV / XBOXUSB) by
 * Kristian Lauszus (GPL-2.0). Works with genuine and third-party dongles
 * (Redgear Pro family: 045E:028E / 045E:0719 / 045E:02A9 / 045E:0291...).
 *
 * Two layouts are detected at runtime from the interface descriptor:
 *   - 1 interrupt IN/OUT pair   -> wired-style protocol (XBOXUSB layout)
 *   - 4 interrupt IN/OUT pairs  -> wireless receiver protocol (XBOXRECV layout)
 *
 * Wireless messages (per pipe/slot):
 *   [0]==0x08            presence change (bit7 of [1] = controller connected)
 *   [1]==0x00 && ...     battery/status report, level = ([4]>>6)&3
 *   [1]==0x01            button data:
 *       buttons = [9]|[8]<<8|[7]<<16|[6]<<24   (digital = bits16..31)
 *       LT = [8], RT = [9]
 *       LX = [10..11], LY = [12..13], RX = [14..15], RY = [16..17] (BE)
 *
 * Host commands (interrupt OUT):
 *   {0x08,0x00,0x0F,0xC0}  presence poll   {0x00,0x00,0x00,0x40}  battery poll
 *   {0x00,0x00,0x08,V}     LED set         {0x00,0x01,0x0F,0xC0,0x00,L,R} rumble
 */
#include "xbox360.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"
#include "usb/usb_types_stack.h"

static const char *TAG = "xbox360";

#define XBOX360_XFER_SIZE   64
#define XBOX360_STATUS_MS   3000

typedef enum {
    XBOX360_MODE_NONE = 0,
    XBOX360_MODE_WIRELESS,      /* vendor-class Xbox 360 wireless receiver (4 pairs) */
    XBOX360_MODE_WIRED,         /* vendor-class wired-style clone (1 pair) */
} xbox360_mode_t;

typedef struct {
    usb_host_client_handle_t client;
    usb_device_handle_t dev;
    uint8_t dev_addr;
    uint8_t claimed_itf;
    uint8_t claimed_cnt;
    uint8_t claimed_nums[4];
    bool open;
    xbox360_mode_t mode;
    uint8_t pairs;                              /* 1 or 4 */
    uint8_t in_addr[XBOX360_MAX_PADS];
    uint8_t out_addr[XBOX360_MAX_PADS];
    usb_transfer_t *xfer_in[XBOX360_MAX_PADS];
    usb_transfer_t *xfer_out[XBOX360_MAX_PADS];
    bool out_busy[XBOX360_MAX_PADS];
    xbox360_pad_t pad[XBOX360_MAX_PADS];
    uint32_t last_status_ms;
    uint32_t last_data_ms;          /* timestamp of last successful IN transfer (heartbeat) */
    bool itf_claimed;               /* interface currently claimed */
    bool gone;                      /* device was removed - skip flush/clear on close */
} xbox360_drv_t;

static xbox360_drv_t s_drv;
static QueueHandle_t s_evt_queue;
static volatile bool s_open_pending;
static volatile bool s_close_pending;
static volatile uint8_t s_dev_addr_shadow;

static void send_cmd(uint8_t slot, const uint8_t *data, size_t len);
static void in_xfer_cb(usb_transfer_t *xfer);
static void out_xfer_cb(usb_transfer_t *xfer);

/* ---------------------------------------------------------------- events */

static void evt_push(uint8_t slot, bool connect_change, const uint8_t *raw, uint8_t raw_len)
{
    xbox360_evt_t evt = {
        .slot = slot,
        .connect_change = connect_change,
        .pad = s_drv.pad[slot],
    };
    if (raw && raw_len) {
        uint8_t n = raw_len;
        if (n > sizeof(evt.raw)) {
            n = sizeof(evt.raw);
        }
        memcpy(evt.raw, raw, n);
        evt.raw_len = n;
    }
    if (s_evt_queue) {
        xQueueSend(s_evt_queue, &evt, 0);
    }
}

/* --------------------------------------------------------------- parsing */

static bool pad_changed(const xbox360_pad_t *a, const xbox360_pad_t *b)
{
    return a->present != b->present || a->buttons != b->buttons ||
           a->lx != b->lx || a->ly != b->ly || a->rx != b->rx ||
           a->ry != b->ry || a->battery != b->battery ||
           a->battery_valid != b->battery_valid;
}

static void parse_presence(uint8_t slot, const uint8_t *d, uint8_t len)
{
    if (slot >= XBOX360_MAX_PADS) {
        return;
    }
    xbox360_pad_t *pad = &s_drv.pad[slot];
    bool now = (d[1] & 0x80) != 0;
    if (now != pad->present) {
        pad->present = now;
        if (!now) pad->battery_valid = false;   /* stale level must not linger */
        evt_push(slot, true, d, len);
        if (now && s_drv.mode == XBOX360_MODE_WIRELESS) {
            /* LED slot+1 on + battery poll */
            uint8_t led[4] = { 0x00, 0x00, 0x08, (uint8_t)(0x0C + 8 * slot) };
            send_cmd(slot, led, 4);
            static const uint8_t batt[4] = { 0x00, 0x00, 0x00, 0x40 };
            send_cmd(slot, batt, 4);
        }
    }
}

static void parse_report(uint8_t slot, const uint8_t *d, uint8_t len)
{
    if (slot >= XBOX360_MAX_PADS) {
        return;
    }
    xbox360_pad_t *pad = &s_drv.pad[slot];
    xbox360_pad_t before = *pad;

    if (len >= 2 && d[0] == 0x08) {
        parse_presence(slot, d, len);
        return;
    }

    if (s_drv.pairs > 1) {
        /* wireless receiver layout */
        if (len >= 18 && d[1] == 0x01) {
            pad->present = true;
            pad->buttons = (uint32_t)d[9] | ((uint32_t)d[8] << 8) |
                           ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24);
            pad->lt = d[8];
            pad->rt = d[9];
            pad->lx = (int16_t)((d[11] << 8) | d[10]);
            pad->ly = (int16_t)((d[13] << 8) | d[12]);
            pad->rx = (int16_t)((d[15] << 8) | d[14]);
            pad->ry = (int16_t)((d[17] << 8) | d[16]);
        } else if (len >= 5 && d[1] == 0x00 && (d[3] & 0x13) && d[4] >= 0x22) {
            pad->battery = (d[4] >> 6) & 3;
            pad->battery_valid = true;
        }
    } else {
        /* wired-style clone layout: 00 14 <buttons LE> LT RT LX LY RX RY
           Keep buttons low bytes same order as wireless: low=RT high=LT (felis compat) */
        if (len >= 14 && d[0] == 0x00 && d[1] == 0x14) {
            pad->present = true;
            uint16_t dig = (uint16_t)(d[2] | (d[3] << 8));
            pad->buttons = ((uint32_t)dig << 16) | ((uint32_t)d[4] << 8) | d[5];
            pad->lt = d[4];
            pad->rt = d[5];
            pad->lx = (int16_t)(d[6] | (d[7] << 8));
            pad->ly = (int16_t)(d[8] | (d[9] << 8));
            pad->rx = (int16_t)(d[10] | (d[11] << 8));
            pad->ry = (int16_t)(d[12] | (d[13] << 8));
        }
    }

    if (pad_changed(&before, pad)) {
        evt_push(slot, false, d, len);
    }
}

/* ----------------------------------------------------------- host commands */

/* --------------------------------------------------------------- OUT queue */
/* The single OUT pipe is shared by presence/battery polls (xbox360_task),
   rumble + LED (car_task). Back-to-back send_cmd calls used to DROP the
   second command (out_busy) - battery polls essentially never went out.
   Now: per-slot FIFO (8 deep), drained on TX completion. */
#define OUT_Q_N 8
typedef struct { uint8_t data[8]; uint8_t len; } out_cmd_t;
static out_cmd_t s_outq[XBOX360_MAX_PADS][OUT_Q_N];
static uint8_t s_outq_h[XBOX360_MAX_PADS];
static uint8_t s_outq_n[XBOX360_MAX_PADS];
static portMUX_TYPE s_out_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_out_drop;

static void out_pump_locked(uint8_t slot)
{
    /* caller holds s_out_mux */
    if (!s_drv.open || slot >= s_drv.pairs || s_drv.out_busy[slot] ||
        !s_drv.xfer_out[slot] || !s_outq_n[slot])
        return;
    out_cmd_t *c = &s_outq[slot][s_outq_h[slot]];
    usb_transfer_t *xfer = s_drv.xfer_out[slot];
    size_t len = c->len > xfer->data_buffer_size ? xfer->data_buffer_size : c->len;
    memcpy(xfer->data_buffer, c->data, len);
    xfer->num_bytes = (int)len;
    s_drv.out_busy[slot] = true;
    if (usb_host_transfer_submit(xfer) != ESP_OK) {
        s_drv.out_busy[slot] = false;   /* keep queued, retry on next pump */
    } else {
        s_outq_h[slot] = (uint8_t)((s_outq_h[slot] + 1) % OUT_Q_N);
        s_outq_n[slot]--;
    }
}

static void send_cmd(uint8_t slot, const uint8_t *data, size_t len)
{
    if (!s_drv.open || slot >= s_drv.pairs || !s_drv.xfer_out[slot]) {
        return;
    }
    portENTER_CRITICAL(&s_out_mux);
    if (s_outq_n[slot] >= OUT_Q_N) {
        s_out_drop++;
        portEXIT_CRITICAL(&s_out_mux);
        return;   /* full: drop incoming (LED/rumble are loss-tolerant) */
    }
    out_cmd_t *c = &s_outq[slot][(uint8_t)((s_outq_h[slot] + s_outq_n[slot]) % OUT_Q_N)];
    size_t n = len > sizeof(c->data) ? sizeof(c->data) : len;
    memcpy(c->data, data, n);
    c->len = (uint8_t)n;
    s_outq_n[slot]++;
    out_pump_locked(slot);
    portEXIT_CRITICAL(&s_out_mux);
}

static void status_poll(void)
{
    if (!s_drv.open || s_drv.mode != XBOX360_MODE_WIRELESS) {
        return;
    }
    static const uint8_t presence[4] = { 0x08, 0x00, 0x0F, 0xC0 };
    static const uint8_t battery[4] = { 0x00, 0x00, 0x00, 0x40 };
    for (uint8_t i = 0; i < s_drv.pairs; i++) {
        send_cmd(i, presence, 4);
        if (s_drv.pad[i].present) {
            send_cmd(i, battery, 4);
        }
    }
}

/* ------------------------------------------------------------- USB events */

static void client_event_cb(const usb_host_client_event_msg_t *event, void *arg)
{
    (void)arg;
    switch (event->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGI(TAG, "dongle connected (addr=%d)", event->new_dev.address);
        s_dev_addr_shadow = event->new_dev.address;
        s_open_pending = true;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        if (event->dev_gone.dev_hdl == s_drv.dev) {
            s_drv.gone = true;
            s_close_pending = true;
            ESP_LOGW(TAG, "dongle GONE (dev_hdl=%p) — will auto-reconnect on next NEW_DEV",
                     (void *)event->dev_gone.dev_hdl);
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------ open/close */

static void drv_open(void)
{
    if (s_drv.open) {
        return;
    }
    esp_err_t err = usb_host_device_open(s_drv.client, s_dev_addr_shadow, &s_drv.dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device open failed: %s", esp_err_to_name(err));
        return;
    }
    s_drv.dev_addr = s_dev_addr_shadow;
    s_drv.gone = false;

    const usb_device_desc_t *dev_desc = NULL;
    if (usb_host_get_device_descriptor(s_drv.dev, &dev_desc) != ESP_OK || !dev_desc) {
        ESP_LOGE(TAG, "get device descriptor failed");
        goto fail;
    }
    ESP_LOGI(TAG, "dongle VID:PID = %04X:%04X", dev_desc->idVendor, dev_desc->idProduct);

    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(s_drv.dev, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "no config descriptor");
        goto fail;
    }

    uint8_t in_addr[XBOX360_MAX_PADS] = { 0 };
    uint8_t out_addr[XBOX360_MAX_PADS] = { 0 };
    uint8_t n_in = 0, n_out = 0;
    uint8_t itf_nums[4] = {0};
    uint8_t itf_cnt = 0;
    uint8_t itf_class = 0;
    bool itf_found = false;

    const uint8_t *p = (const uint8_t *)cfg;
    const uint8_t *end = p + cfg->wTotalLength;
    while (p < end) {
        uint8_t len = p[0];
        if (len < 2) {
            break;
        }
        if (p[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *itf = (const usb_intf_desc_t *)p;
            if (itf_cnt < 4) itf_nums[itf_cnt++] = itf->bInterfaceNumber;
            if (!itf_found) itf_class = itf->bInterfaceClass;
            itf_found = true;
        } else if (p[1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT && itf_found) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)p;
            if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_INT) {
                if (ep->bEndpointAddress & 0x80) {
                    if (n_in < XBOX360_MAX_PADS) {
                        in_addr[n_in] = ep->bEndpointAddress;
                        n_in++;
                    }
                } else {
                    if (n_out < XBOX360_MAX_PADS) {
                        out_addr[n_out] = ep->bEndpointAddress;
                        n_out++;
                    }
                }
            }
        }
        p += len;
    }

    if (!itf_found || n_in == 0 || n_out == 0) {
        ESP_LOGW(TAG, "unsupported interface (class=0x%02X in=%u out=%u)",
                 itf_class, n_in, n_out);
        goto fail;
    }
    if (n_in > 4 || n_out > 4) {
        ESP_LOGW(TAG, "truncating endpoints in=%u out=%u to 4", n_in, n_out);
        if (n_in > 4) n_in = 4;
        if (n_out > 4) n_out = 4;
    }
    if (n_in != n_out) {
        ESP_LOGW(TAG, "mismatched pairs in=%u out=%u", n_in, n_out);
    }

    s_drv.pairs = n_in;
    s_drv.claimed_itf = itf_nums[0];
    memcpy(s_drv.in_addr, in_addr, XBOX360_MAX_PADS);
    memcpy(s_drv.out_addr, out_addr, XBOX360_MAX_PADS);

    if (itf_class == USB_CLASS_HID) {
        ESP_LOGW(TAG, "DInput/HID mode not supported - controller par HOME 5s hold karo (XInput mode)");
        goto fail;
    } else if (itf_class == USB_CLASS_VENDOR_SPEC) {
        s_drv.mode = (n_in > 1) ? XBOX360_MODE_WIRELESS : XBOX360_MODE_WIRED;
        ESP_LOGI(TAG, "vendor interfaces %u, %u interrupt pair(s) - %s",
                 itf_cnt, s_drv.pairs, s_drv.pairs > 1 ? "wireless receiver" : "wired-style");
    } else {
        ESP_LOGW(TAG, "unsupported interface (class=0x%02X)", itf_class);
        goto fail;
    }

    for (uint8_t i = 0; i < itf_cnt; i++) {
        if (usb_host_interface_claim(s_drv.client, s_drv.dev, itf_nums[i], 0) != ESP_OK) {
            ESP_LOGE(TAG, "interface %u claim failed", itf_nums[i]);
            goto fail;
        }
    }
    s_drv.claimed_cnt = itf_cnt;
    memcpy(s_drv.claimed_nums, itf_nums, itf_cnt);
    s_drv.itf_claimed = true;

    for (uint8_t i = 0; i < s_drv.pairs; i++) {
        if (usb_host_transfer_alloc(XBOX360_XFER_SIZE, 0, &s_drv.xfer_in[i]) != ESP_OK ||
            usb_host_transfer_alloc(XBOX360_XFER_SIZE, 0, &s_drv.xfer_out[i]) != ESP_OK) {
            ESP_LOGE(TAG, "transfer alloc failed");
            goto fail_clean;
        }
        s_drv.xfer_in[i]->device_handle = s_drv.dev;
        s_drv.xfer_in[i]->bEndpointAddress = in_addr[i];
        s_drv.xfer_in[i]->callback = in_xfer_cb;
        s_drv.xfer_in[i]->context = (void *)(uintptr_t)i;
        s_drv.xfer_in[i]->num_bytes = XBOX360_XFER_SIZE;

        s_drv.xfer_out[i]->device_handle = s_drv.dev;
        s_drv.xfer_out[i]->bEndpointAddress = out_addr[i];
        s_drv.xfer_out[i]->callback = out_xfer_cb;
        s_drv.xfer_out[i]->context = (void *)(uintptr_t)i;

        if (usb_host_transfer_submit(s_drv.xfer_in[i]) != ESP_OK) {
            ESP_LOGE(TAG, "IN transfer submit failed");
            goto fail_clean;
        }
    }

    memset(s_drv.pad, 0, sizeof(s_drv.pad));
    s_drv.open = true;
    s_drv.last_status_ms = 0;
    s_drv.last_data_ms = 0;  /* will be set on first successful transfer */
    ESP_LOGI(TAG, "dongle ready - controller ka HOME dabao");
    return;

fail_clean:
    for (uint8_t i = 0; i < s_drv.pairs; i++) {
        if (s_drv.xfer_in[i]) {
            usb_host_transfer_free(s_drv.xfer_in[i]);
            s_drv.xfer_in[i] = NULL;
        }
        if (s_drv.xfer_out[i]) {
            usb_host_transfer_free(s_drv.xfer_out[i]);
            s_drv.xfer_out[i] = NULL;
        }
    }
    if (s_drv.itf_claimed) {
        for (uint8_t i = 0; i < s_drv.claimed_cnt; i++) {
            usb_host_interface_release(s_drv.client, s_drv.dev, s_drv.claimed_nums[i]);
        }
        s_drv.itf_claimed = false;
        s_drv.claimed_cnt = 0;
    } else if (itf_cnt) {
        for (uint8_t i = 0; i < itf_cnt; i++) {
            usb_host_interface_release(s_drv.client, s_drv.dev, itf_nums[i]);
        }
    }
fail:
    /* Small delay: the host lib decrements its ctrl-xfer-inflight counter a
       moment AFTER delivering the transfer callback (usbh.c handle_ep0_dequeue),
       so closing immediately can hit "usbh_dev_close usbh.c:1334" assert. */
    vTaskDelay(pdMS_TO_TICKS(20));
    usb_host_device_close(s_drv.client, s_drv.dev);
    s_drv.dev = NULL;
    s_drv.dev_addr = 0;
    s_drv.gone = false;
}

static void drv_close(void)
{
    if (!s_drv.open) {
        return;
    }

    /* Diagnostic: log the reason for close */
    {
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        const char *reason = "unknown";
        if (s_drv.gone) reason = "DEV_GONE";
        else if (s_drv.last_data_ms > 0 && (now - s_drv.last_data_ms) > 2000)
            reason = "heartbeat timeout";
        else reason = "transfer error";
        ESP_LOGW(TAG, "drv_close: reason=%s, last_data_ums=%u", reason,
                 s_drv.last_data_ms > 0 ? (now - s_drv.last_data_ms) : 0);
    }

    if (!s_drv.gone) {
        for (uint8_t i = 0; i < s_drv.pairs; i++) {
            usb_host_endpoint_flush(s_drv.dev, s_drv.in_addr[i]);
            usb_host_endpoint_clear(s_drv.dev, s_drv.in_addr[i]);
            usb_host_endpoint_flush(s_drv.dev, s_drv.out_addr[i]);
            usb_host_endpoint_clear(s_drv.dev, s_drv.out_addr[i]);
        }
    }
    for (uint8_t i = 0; i < s_drv.pairs; i++) {
        if (s_drv.xfer_in[i]) {
            usb_host_transfer_free(s_drv.xfer_in[i]);
            s_drv.xfer_in[i] = NULL;
        }
        if (s_drv.xfer_out[i]) {
            usb_host_transfer_free(s_drv.xfer_out[i]);
            s_drv.xfer_out[i] = NULL;
        }
    }

    if (s_drv.itf_claimed) {
        for (uint8_t i = 0; i < s_drv.claimed_cnt; i++) {
            usb_host_interface_release(s_drv.client, s_drv.dev, s_drv.claimed_nums[i]);
        }
        s_drv.itf_claimed = false;
        s_drv.claimed_cnt = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    usb_host_device_close(s_drv.client, s_drv.dev);
    s_drv.dev = NULL;
    s_drv.dev_addr = 0;
    s_drv.open = false;
    s_drv.gone = false;
    s_drv.last_data_ms = 0;
    portENTER_CRITICAL(&s_out_mux);
    for (uint8_t i = 0; i < XBOX360_MAX_PADS; i++) {
        s_outq_h[i] = 0;
        s_outq_n[i] = 0;
        s_drv.out_busy[i] = false;
    }
    portEXIT_CRITICAL(&s_out_mux);

    for (uint8_t i = 0; i < XBOX360_MAX_PADS; i++) {
        if (s_drv.pad[i].present) {
            s_drv.pad[i].present = false;
            evt_push(i, true, NULL, 0);
        }
        memset(&s_drv.pad[i], 0, sizeof(s_drv.pad[i]));
    }
    s_drv.mode = XBOX360_MODE_NONE;
    ESP_LOGI(TAG, "dongle disconnected");
}

/* --------------------------------------------------------- transfer callbacks */

static void in_xfer_cb(usb_transfer_t *xfer)
{
    uint8_t slot = (uint8_t)(uintptr_t)xfer->context;
    static uint8_t retry_cnt[4];

    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        retry_cnt[slot] = 0;
        uint16_t n = xfer->actual_num_bytes;
        if (n > XBOX360_XFER_SIZE) n = XBOX360_XFER_SIZE;
        parse_report(slot, xfer->data_buffer, (uint8_t)n);
        s_drv.last_data_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        xfer->num_bytes = XBOX360_XFER_SIZE;
        esp_err_t err = usb_host_transfer_submit(xfer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "IN slot %u re-submit failed: %s — closing", slot, esp_err_to_name(err));
            s_close_pending = true;
        }
    } else if (xfer->status == USB_TRANSFER_STATUS_NO_DEVICE ||
               xfer->status == USB_TRANSFER_STATUS_CANCELED) {
        /* Device disconnected or transfer cancelled by driver — trigger close */
        ESP_LOGW(TAG, "IN slot %u device gone (status=%d) — closing", slot, xfer->status);
        retry_cnt[slot] = 0;
        s_close_pending = true;
    } else {
        if (++retry_cnt[slot] > 5) {
            ESP_LOGW(TAG, "IN slot %u stall %d, closing", slot, xfer->status);
            retry_cnt[slot] = 0;
            s_close_pending = true;
            return;
        }
        ESP_LOGW(TAG, "IN retry %u slot %u err %d", retry_cnt[slot], slot, xfer->status);
        // NOTE: usb_host_endpoint_clear + vTaskDelay removed from callback (was blocking USB task)
        // Retry is deferred: endpoint will be cleared by xbox360_task or next successful poll.
        // Attempt immediate re-submit without clear; if it fails, driver will close via retry limit.
        xfer->num_bytes = XBOX360_XFER_SIZE;
        esp_err_t rerr = usb_host_transfer_submit(xfer);
        if (rerr != ESP_OK) {
            ESP_LOGW(TAG, "IN retry submit failed slot %u: %s", slot, esp_err_to_name(rerr));
            s_close_pending = true;
        }
    }
}

static void out_xfer_cb(usb_transfer_t *xfer)
{
    uint8_t slot = (uint8_t)(uintptr_t)xfer->context;
    portENTER_CRITICAL(&s_out_mux);
    s_drv.out_busy[slot] = false;
    out_pump_locked(slot);   /* drain next queued command */
    portEXIT_CRITICAL(&s_out_mux);
}

/* ------------------------------------------------------------------ printing */

static void print_evt(const xbox360_evt_t *evt)
{
    if (evt->connect_change) {
        if (evt->pad.present) {
            if (evt->pad.battery_valid)
                ESP_LOGI(TAG, "slot %u: controller CONNECTED (battery %u/3)",
                         evt->slot, evt->pad.battery);
            else
                ESP_LOGI(TAG, "slot %u: controller CONNECTED (battery ?/3)",
                         evt->slot);
        } else {
            ESP_LOGI(TAG, "slot %u: controller DISCONNECTED", evt->slot);
        }
        return;
    }

    // Spam removed for performance - enable only if debug needed
    // uint16_t dig = (uint16_t)(evt->pad.buttons >> 16);
    // char names[96];
    // btn_name(dig, names, sizeof(names));
    // ESP_LOGI(TAG, "slot %u: B=0x%04X [%s] LT=%u RT=%u LX=%d LY=%d RX=%d RY=%d",
    //          evt->slot, dig, names,
    //          evt->pad.lt, evt->pad.rt, evt->pad.lx, evt->pad.ly, evt->pad.rx, evt->pad.ry);
    // print_hex_line(evt->raw, evt->raw_len);
}

/* --------------------------------------------------------------- public API */

const xbox360_pad_t *xbox360_pad(uint8_t slot)
{
    if (slot >= XBOX360_MAX_PADS) {
        return NULL;
    }
    return &s_drv.pad[slot];
}

bool xbox360_dongle_connected(void)
{
    return s_drv.open;
}

uint8_t xbox360_layout(void)
{
    return s_drv.pairs;
}

uint32_t xbox360_last_data_ms(void)
{
    return s_drv.last_data_ms;
}

void xbox360_rumble(uint8_t slot, uint8_t left, uint8_t right)
{
    /* Wireless (XBOXRECV): 00 01 0F C0 00 L R
       Wired (XBOXUSB): 00 08 00 L R 00 00 00  — handle both */
    if (s_drv.mode == XBOX360_MODE_WIRELESS) {
        uint8_t cmd[7] = { 0x00, 0x01, 0x0F, 0xC0, 0x00, left, right };
        send_cmd(slot, cmd, sizeof(cmd));
    } else {
        /* wired-style clone: single 8-byte report (was sending both and second was always dropped) */
        uint8_t cmd[8] = { 0x00, 0x08, 0x00, left, right, 0x00, 0x00, 0x00 };
        send_cmd(slot, cmd, sizeof(cmd));
    }
}

/* PART 13.1 LED ring: {00 00 08 V} on the command channel. Wireless only.
   Best-effort (dropped when the OUT pipe is busy); app rate-limits. */
void xbox360_set_led(uint8_t slot, xbox360_led_pattern_t pattern)
{
    if (s_drv.mode != XBOX360_MODE_WIRELESS) return;
    static const uint8_t table[XBOX_LED_COUNT] = {
        0x00, 0x0C, 0x14, 0x1C, 0x24, 0x2C,
    };
    if ((int)pattern < 0 || pattern >= XBOX_LED_COUNT) return;
    static uint32_t s_last_ms[XBOX360_MAX_PADS];
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (slot < XBOX360_MAX_PADS) {
        if (now - s_last_ms[slot] < 100) return;   /* min 100ms between cmds */
        s_last_ms[slot] = now;
    }
    uint8_t cmd[4] = { 0x00, 0x00, 0x08, table[pattern] };
    send_cmd(slot, cmd, sizeof(cmd));
}

/* ------------------------------------------------------------------- task */

void xbox360_task(void *arg)
{
    (void)arg;
    s_evt_queue = xQueueCreate(16, sizeof(xbox360_evt_t));
    if (!s_evt_queue) {
        ESP_LOGE(TAG, "queue create failed");
        vTaskDelete(NULL);
        return;
    }

    usb_host_client_config_t client_cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 32,   /* was 12 — further increased for noisy USB bus stability */
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client_cfg, &s_drv.client));
    ESP_LOGI(TAG, "USB host client ready - dongle plug karo");

    while (1) {
        if (s_open_pending) {
            s_open_pending = false;
            drv_open();
        }
        if (s_close_pending) {
            s_close_pending = false;
            drv_close();
        }
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (s_drv.open && s_drv.mode == XBOX360_MODE_WIRELESS) {
            if (now - s_drv.last_status_ms >= XBOX360_STATUS_MS) {
                s_drv.last_status_ms = now;
                status_poll();
            }
        }
        /* Heartbeat: if no successful transfer for 5 seconds, device is stale.
           Increased from 2s — some controllers briefly pause reports when idle. */
        if (s_drv.open && s_drv.last_data_ms > 0 && (now - s_drv.last_data_ms) > 5000) {
            ESP_LOGW(TAG, "heartbeat timeout — no data for %ums, closing", now - s_drv.last_data_ms);
            s_drv.last_data_ms = 0;
            s_close_pending = true;
        }
        /* Auto-reopen: if driver closed but no DEV_GONE, attempt reconnect every 2s.
           Handles brief USB glitches without requiring physical reconnect. */
        if (!s_drv.open && !s_drv.gone && s_dev_addr_shadow != 0) {
            static uint32_t last_reopen_ms;
            if (now - last_reopen_ms > 2000) {
                last_reopen_ms = now;
                ESP_LOGI(TAG, "auto-reopen attempt (dev was at addr=%u)", s_dev_addr_shadow);
                s_open_pending = true;
            }
        }
        xbox360_evt_t evt;
        while (xQueueReceive(s_evt_queue, &evt, 0)) {
            print_evt(&evt);
        }
        esp_err_t ev = usb_host_client_handle_events(s_drv.client, pdMS_TO_TICKS(20));
        if (ev == ESP_ERR_NOT_FOUND) {
            /* Device gone without explicit DEV_GONE — log for diagnosis */
            static uint32_t gone_cnt;
            ESP_LOGW(TAG, "USB event: device gone without callback (#%u)", ++gone_cnt);
            if (s_drv.open) s_close_pending = true;
        } else if (ev != ESP_OK && ev != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "USB event error: %s", esp_err_to_name(ev));
        }
    }
}