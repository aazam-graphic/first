/*
 * mic_in.c - PART 8 MIC SYSTEM.
 *
 * I2S1 RX (master, 16kHz/16-bit/mono) -> DMA -> capture task -> 64KB PSRAM
 * ring. RMS envelope per DMA frame; mic_poll() (car_task tick) derives the
 * 50ms VU level, beat events (attack threshold-cross) and double-clap.
 */
#include <string.h>
#include "mic_in.h"
#include "snd_bank.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

static const char *TAG = "mic";

#define MIC_RING_BYTES   (64 * 1024)          /* 2 s @32KB/s */
#define MIC_DMA_FRAMES   320                  /* 20 ms per DMA frame (HWM-proven) */
#define MIC_LISTEN_BYTES (MIC_SAMPLE_HZ * 2 * 3)  /* 3 s = 96 KB */

static i2s_chan_handle_t s_rx;
static TaskHandle_t s_cap;
static bool s_ready;
static SemaphoreHandle_t s_mtx;

/* ring */
static uint8_t *s_ring;
static uint32_t s_head, s_tail;

/* levels (capture task writes, mic_poll reads under lock) */
static uint16_t s_rms, s_peak;
static uint32_t s_rms_ms;

/* VU / beat / clap (mic_poll domain) */
static uint16_t s_vu;
static uint32_t s_vu_ms;
static uint32_t s_beat_ms;       /* last beat timestamp */
static bool s_beat_pend;
static uint32_t s_floor;         /* adaptive noise floor (rms avg) */
static uint32_t s_spike_ms;      /* last loud spike (clap stage 1) */
static uint32_t s_clap_cool_until; /* ignore window after a clap toggle */
static bool s_clap_pend;

/* M3 listen */
static uint8_t *s_listen;
static uint32_t s_listen_n;
static bool s_listening;
static uint32_t s_listen_t0;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

static void ring_push(const int16_t *pcm, size_t samples)
{
    for (size_t i = 0; i < samples; i++) {
        s_ring[s_head] = (uint8_t)(pcm[i] & 0xFF);
        s_ring[s_head + 1] = (uint8_t)((pcm[i] >> 8) & 0xFF);
        s_head = (s_head + 2) & (MIC_RING_BYTES - 1);
        if (s_head == s_tail) s_tail = (s_tail + 2) & (MIC_RING_BYTES - 1);
    }
}

static void capture_task(void *arg)
{
    (void)arg;
    static int16_t frame[MIC_DMA_FRAMES];
    while (1) {
        size_t got = 0;
        if (i2s_channel_read(s_rx, (uint8_t *)frame, sizeof(frame),
                             &got, pdMS_TO_TICKS(100)) != ESP_OK || !got) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        size_t n = got / 2;
        uint64_t sum = 0;   /* FIX (audit): 480 * 32767^2 overflows u32 */
        uint16_t peak = 0;
        for (size_t i = 0; i < n; i++) {
            int v = frame[i] < 0 ? -frame[i] : frame[i];
            if (v > peak) peak = (uint16_t)v;
            sum += (uint64_t)((int32_t)frame[i] * frame[i]);
        }
        /* integer sqrt via Newton (no float in fast path). 20 iters with
           early exit: 8 is NOT enough from full-scale (needs ~15). */
        uint32_t mean = n ? (uint32_t)(sum / n) : 0;
        uint32_t r = 0;
        if (mean) {
            uint32_t g = mean;
            for (int k = 0; k < 20; k++) {
                uint32_t ng = (g + mean / (g ? g : 1)) / 2;
                if (ng >= g) break;
                g = ng;
            }
            r = g;
        }
        if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5)) == pdTRUE) {
            s_rms = (uint16_t)(r > 32767 ? 32767 : r);
            s_peak = peak;
            s_rms_ms = now_ms();
            ring_push(frame, n);
            xSemaphoreGive(s_mtx);
        }
    }
}

void mic_init(void)
{
    if (s_ready) return;
    s_mtx = xSemaphoreCreateMutex();
    s_ring = heap_caps_malloc(MIC_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_listen = heap_caps_malloc(MIC_LISTEN_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_mtx || !s_ring || !s_listen) {
        ESP_LOGE(TAG, "PSRAM alloc failed - mic disabled");
        return;
    }
    memset(s_ring, 0, MIC_RING_BYTES);
    i2s_chan_config_t cc = {
        .id = I2S_NUM_1, .role = I2S_ROLE_MASTER,
        .dma_desc_num = 4, .dma_frame_num = MIC_DMA_FRAMES, .auto_clear = true,
    };
    if (i2s_new_channel(&cc, NULL, &s_rx) != ESP_OK) {
        ESP_LOGE(TAG, "I2S1 chan failed");
        return;
    }
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)MIC_PIN_SCK,
            .ws = (gpio_num_t)MIC_PIN_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = (gpio_num_t)MIC_PIN_SD,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    sc.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    if (i2s_channel_init_std_mode(s_rx, &sc) != ESP_OK) {
        ESP_LOGE(TAG, "I2S1 mode failed");
        return;
    }
    if (i2s_channel_enable(s_rx) != ESP_OK) {
        ESP_LOGE(TAG, "I2S1 enable failed");
        return;
    }
    s_ready = true;
    /* PART 16: 3KB stack (HWM shows <1KB used); saves 1KB internal heap */
    xTaskCreatePinnedToCore(capture_task, "mic_cap", 3072, NULL, 2, &s_cap, 1);
    ESP_LOGI(TAG, "init OK I2S1 SCK=%d WS=%d SD=%d 16kHz ring 64KB PSRAM",
             MIC_PIN_SCK, MIC_PIN_WS, MIC_PIN_SD);
}

bool mic_ready(void) { return s_ready; }

/* PART 16 mem_diag: capture-task stack high-water (0 if not running) */
uint32_t mic_stack_free(void)
{
    if (!s_cap) return 0;
    return (uint32_t)uxTaskGetStackHighWaterMark(s_cap);
}

void mic_poll(uint32_t now)
{
    if (!s_ready) return;
    uint16_t rms;
    if (xSemaphoreTake(s_mtx, 0) != pdTRUE) return;
    rms = s_rms;
    bool fresh = (now - s_rms_ms) < 150;
    xSemaphoreGive(s_mtx);
    if (!fresh) rms = 0;

    /* adaptive floor (slow) */
    if (!s_floor) s_floor = rms;
    else s_floor += (rms > s_floor) ? 1 : (rms < s_floor ? (uint32_t)-1 : 0);

    /* 50ms VU cadence (M1 exact target) */
    if (now - s_vu_ms >= 50) {
        s_vu_ms = now;
        s_vu = rms > 4000 ? 100 : (uint16_t)(rms * 100 / 4000);
    }

    /* M1 beat: attack threshold-cross, >=180ms apart */
    uint32_t thr = s_floor * 2 + 300;
    if (thr < 600) thr = 600;
    if (rms > thr && now - s_beat_ms >= 180) {
        s_beat_ms = now;
        s_beat_pend = true;
    }

    /* M2 double-clap: two sharp spikes 150..600ms apart, quiet room only.
       The mic hears the car's own speaker, so: adaptive threshold (5x floor,
       min 6000), ambient-floor gate (noisy/engine-loud = ignore), and a 1s
       cooldown after each toggle (no echo retrigger). */
    bool quiet = (s_floor < 2000);
    uint32_t cthr = s_floor * 5 + 4000;
    if (cthr < 6000) cthr = 6000;
    if (now < s_clap_cool_until) {
        if (s_spike_ms && now - s_spike_ms > 600) s_spike_ms = 0;
    } else if (quiet && rms > cthr) {
        if (s_spike_ms && now - s_spike_ms >= 150 && now - s_spike_ms <= 600) {
            s_clap_pend = true;
            s_spike_ms = 0;
            s_clap_cool_until = now + 1000;
        } else if (!s_spike_ms || now - s_spike_ms > 600) {
            s_spike_ms = now;
        }
    } else if (s_spike_ms && now - s_spike_ms > 600) {
        s_spike_ms = 0;
    }

    /* M3: drain ring snapshot into listen buffer while recording */
    if (s_listening) {
        if (now - s_listen_t0 >= 3000 || s_listen_n >= MIC_LISTEN_BYTES) {
            mic_listen_stop();
        }
    }
}

uint16_t mic_rms(void) { return s_rms; }
uint16_t mic_vu(void) { return s_vu; }
uint16_t mic_peak(void) { return s_peak; }

bool mic_beat(void)
{
    if (!s_beat_pend) return false;
    s_beat_pend = false;
    return true;
}

bool mic_clap(void)
{
    if (!s_clap_pend) return false;
    s_clap_pend = false;
    return true;
}

bool mic_listen_start(void)
{
    if (!s_ready || s_listening) return false;
    /* snapshot newest ~3s from ring — whole copy under lock so the
       capture task can't tear the listen buffer mid-copy */
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    uint32_t tail = s_tail;
    uint32_t head = s_head;
    /* copy last up-to-96KB ending at head (ring may not be full yet) */
    uint32_t avail = (head >= tail) ? (head - tail) :
                     (MIC_RING_BYTES - tail + head);
    uint32_t n = avail > MIC_LISTEN_BYTES ? MIC_LISTEN_BYTES : avail;
    uint32_t start = (head + MIC_RING_BYTES - n) & (MIC_RING_BYTES - 1);
    for (uint32_t i = 0; i < n; i++)
        s_listen[i] = s_ring[(start + i) & (MIC_RING_BYTES - 1)];
    xSemaphoreGive(s_mtx);
    s_listen_n = n;
    s_listen_t0 = now_ms();
    s_listening = true;
    snd_play("voice_listen_start");
    ESP_LOGI(TAG, "M3 listen start (%lu B seeded)", (unsigned long)n);
    return true;
}

void mic_listen_stop(void)
{
    if (!s_listening) return;
    s_listening = false;
    snd_play("voice_listen_end");
    ESP_LOGI(TAG, "M3 listen stop (%lu ms, %lu B) - foundation only, no TX",
             (unsigned long)(now_ms() - s_listen_t0), (unsigned long)s_listen_n);
}

bool mic_listening(void) { return s_listening; }
uint32_t mic_listen_ms(void)
{
    return s_listening ? (now_ms() - s_listen_t0) : 0;
}

size_t mic_listen_read(uint8_t *buf, size_t max_len)
{
    if (!buf || !s_listen_n) return 0;
    size_t n = s_listen_n < max_len ? s_listen_n : max_len;
    memcpy(buf, s_listen, n);
    return n;
}
