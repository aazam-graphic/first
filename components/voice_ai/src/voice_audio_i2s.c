/*
 * voice_audio_i2s.c — I2S mic RX DMA driver for XiaoZhi.
 * Pipeline: I2S_NUM_1 (master RX) -> DMA -> capture task -> ring buffer (PSRAM)
 * Capture task: Core 1, priority 4.  car_task on Core 0 is independent.
 */

#include "voice_audio_i2s.h"
#include "voice_ai.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
static bool IRAM_ATTR i2s_isr(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx)
{
    (void)handle; (void)user_ctx;
    if (event->size > 0) {
        BaseType_t woken = pdFALSE;
        if (s_dma_sem) xSemaphoreGiveFromISR(s_dma_sem, &woken);
        return woken == pdTRUE;
    }
    return false;
}

static void capture_task(void *arg)
{
    (void)arg;
    size_t dummy;
    while (s_running) {
        if (xSemaphoreTake(s_dma_sem, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        esp_err_t err = i2s_channel_read(s_rx_chan, s_frame, sizeof(s_frame), &dummy, 0);
        if (err != ESP_OK || dummy == 0) continue;
        size_t bytes = dummy;
        int peak = 0; unsigned long sum = 0; bool clip = false;
        size_t samples = bytes / 2;
        for (size_t i = 0; i < samples; i++) {
            int s = s_frame[i]; if (s < 0) s = -s;
            if (s > peak) peak = s;
            if (s >= 32000) clip = true;
            sum += (unsigned long)((int)s_frame[i] * s_frame[i]);
        }
        s_diag.frames_rx++;
        s_diag.peak_level = peak;
        s_diag.clipping = clip;
        s_diag.rms_level = samples ? (int)(sum / samples) : 0;
        if (xRingbufferSend(s_ring, s_frame, bytes, 0) != pdTRUE) s_diag.overruns++;
    }
    vTaskDelete(NULL);
}

esp_err_t voice_audio_i2s_init(void)
{
    if (s_rx_chan) return ESP_OK;
    s_ring = xRingbufferCreateWithCaps(VOICE_I2S_RING_SIZE, RINGBUF_TYPE_BYTEBUF,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) return ESP_ERR_NO_MEM;
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_1, .role = I2S_ROLE_MASTER,
        .dma_desc_num = VOICE_I2S_DMA_DESC, .dma_frame_num = VOICE_I2S_DMA_FRAMES, .auto_clear = true,
    };
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (err != ESP_OK) return err;
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(VOICE_I2S_BITS, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)VOICE_I2S_BCLK_GPIO,
            .ws = (gpio_num_t)VOICE_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = (gpio_num_t)VOICE_I2S_SD_GPIO,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (err != ESP_OK) return err;
    i2s_event_callbacks_t cb = { .on_recv = i2s_isr, .on_recv_q_ovf = NULL, .on_sent = NULL, .on_send_q_ovf = NULL };
    i2s_channel_register_event_callback(s_rx_chan, &cb, NULL);
    s_dma_sem = xSemaphoreCreateBinary();
    memset(&s_diag, 0, sizeof(s_diag));
    ESP_LOGI(TAG, "init OK I2S1 BCLK=%d WS=%d SD=%d ring=%u PSRAM",
             VOICE_I2S_BCLK_GPIO, VOICE_I2S_WS_GPIO, VOICE_I2S_SD_GPIO, VOICE_I2S_RING_SIZE);
    return ESP_OK;
}
esp_err_t voice_audio_i2s_start(void)
{
    if (!s_rx_chan || s_running) return ESP_ERR_INVALID_STATE;
    s_running = true;
    xTaskCreatePinnedToCore(capture_task, "voice_cap", 4096, NULL, 4, &s_cap_task, 1);
    esp_err_t err = i2s_channel_enable(s_rx_chan);
    if (err != ESP_OK) { s_running = false; return err; }
    ESP_LOGI(TAG, "capture started (Core 1, pri 4)");
    return ESP_OK;
}
void voice_audio_i2s_stop(void)
{
    if (!s_running) return;
    s_running = false;
    if (s_cap_task) { vTaskDelay(pdMS_TO_TICKS(50)); s_cap_task = NULL; }
    if (s_rx_chan) i2s_channel_disable(s_rx_chan);
    ESP_LOGI(TAG, "capture stopped");
}
size_t voice_audio_i2s_read(uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    if (!s_ring || !buf) return 0;
    size_t item_size = 0;
    uint8_t *item = xRingbufferReceiveUpTo(s_ring, &item_size, pdMS_TO_TICKS(timeout_ms), max_len);
    if (!item) return 0;
    memcpy(buf, item, item_size);
    vRingbufferReturnItem(s_ring, item);
    return item_size;
}
size_t voice_audio_i2s_available(void)
{
    if (!s_ring) return 0;
    size_t free_sz = xRingbufferGetCurFreeSize(s_ring);
    return free_sz < VOICE_I2S_RING_SIZE ? (VOICE_I2S_RING_SIZE - free_sz) : 0;
}
void voice_audio_i2s_flush(void)
{
    if (!s_ring) return;
    size_t dummy; uint8_t *item;
    while ((item = xRingbufferReceive(s_ring, &dummy, 0)) != NULL) vRingbufferReturnItem(s_ring, item);
}
void voice_audio_i2s_get_diag(voice_i2s_diag_t *out)
{
    if (out) *out = s_diag;
}

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include <string.h>

static const char *TAG = "voice_i2s";

static i2s_chan_handle_t s_rx_chan = NULL;
static RingbufHandle_t   s_ring    = NULL;
static TaskHandle_t      s_cap_task = NULL;
static volatile bool     s_running  = false;
static SemaphoreHandle_t s_dma_sem  = NULL;
static voice_i2s_diag_t  s_diag;
static int16_t           s_frame[VOICE_I2S_DMA_FRAMES];

static bool IRAM_ATTR i2s_isr(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx)
{
    (void)handle; (void)user_ctx;
    if (event->size > 0) {
        BaseType_t woken = pdFALSE;
        if (s_dma_sem) xSemaphoreGiveFromISR(s_dma_sem, &woken);
        return woken == pdTRUE;
    }
    return false;
}
