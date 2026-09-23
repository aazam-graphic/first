#include "tft_display.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "TFT";

// Pins - LANDSCAPE wiring (BL hardwired 3.3V). Pins tft_display.h me define hain.
// Pins (SCLK/MOSI/CS/DC/RST) ab tft_display.h me define hain - single source
// of truth, taaki car.c ka pin-uniqueness guard unhe bhi check kar sake.
#define TFT_SPI_HOST  SPI2_HOST
#define TFT_SPI_HZ    (40*1000*1000) // 40 MHz (ILI9341 max ~40MHz, ST7789 up to 80MHz)

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;
static bool s_ready = false;
static uint16_t *s_fb = NULL; // 240*320*2 = 153600 bytes DMA

/* --- RGB565 byte-order (color-calibration WINNER, 2026-09-02) ----------------
   The panel decodes each 16-bit color word HIGH-byte-first on the SPI wire,
   but this little-endian MCU stores uint16_t LOW-byte-first, so unswapped
   pixels render as R<->B swapped. The standalone tft_color_test split screen
   proved it: RIGHT half (byteswapped pixels) showed correct colors.
   So every pixel is byte-swapped right before EVERY SPI push. The framebuffer
   itself stays in standard RGB565 - only SPI-bound data is swapped.
   Full winning config = INVERT OFF + RGB channel order + this byte-swap. */
static inline uint16_t tft_bswap16(uint16_t v){ return (uint16_t)((v << 8) | (v >> 8)); }
static inline void s_swap_buf(uint16_t *buf, size_t n){ for(size_t i=0;i<n;i++) buf[i]=tft_bswap16(buf[i]); }

#define ROWBUF_BYTES (320 * 50 * 2)   /* 32 KB DMA scratch (internal RAM) */
static uint16_t *s_rowbuf = NULL;     /* packed-row scratch (region + full push) */
static uint16_t *s_swaprow = NULL;    /* 1-row swap scratch (alloc-failure path) */

/* --- SPI tx completion tracking (async DMA safety) ---
   esp_lcd_panel_io_tx_color QUEUES the transfer and returns immediately
   (spi_device_queue_trans). Any buffer reused before its DMA completes gets
   corrupted mid-transfer (seen as horizontal garbage on dirty-rect pushes).
   We count outstanding transfers and drain them before touching a buffer. */
static SemaphoreHandle_t s_tx_done_sem = NULL;
static volatile int s_tx_inflight = 0;

static bool s_tx_done_cb(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io; (void)edata; (void)ctx;
    if (s_tx_done_sem) xSemaphoreGive(s_tx_done_sem);
    return false;
}

/* wait until every queued SPI transfer has completed */
static void s_tx_drain(void)
{
    if (!s_tx_done_sem) { s_tx_inflight = 0; return; }
    while (s_tx_inflight > 0) {
        if (xSemaphoreTake(s_tx_done_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
            s_tx_inflight = 0;   /* safety net - never hang the car loop */
            break;
        }
        s_tx_inflight--;
    }
}

// 5x7 font (ASCII 32..126) - minimal 96 chars
static const uint8_t s_font5x7[][5] = {
 {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},
 {0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},
 {0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},{0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
 {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
 {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},{0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},
 {0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},
 {0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
 {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},
 {0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},
 {0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},{0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
 {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
 {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},{0x02,0x04,0x08,0x10,0x20},
 {0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},{0x00,0x03,0x05,0x00,0x00},{0x20,0x54,0x54,0x54,0x78},
 {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},
 {0x0C,0x52,0x52,0x52,0x3E},{0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x7F,0x10,0x28,0x44,0x00},
 {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0x7C,0x14,0x14,0x14,0x08},
 {0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},{0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},
 {0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},
 {0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x10,0x08,0x08,0x10,0x08},{0x78,0x46,0x46,0x46,0x31}
};

bool tft_is_ready(void) { return s_ready; }

/* backlight brightness 0..100 -> PWM duty 0..1023 (10-bit). Always safe. */
void tft_set_brightness(uint8_t pct)
{
#if TFT_PIN_BL != -1
    if (pct > 100) pct = 100;
    uint32_t duty = (uint32_t)pct * 1023u / 100u;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
    ESP_LOGI(TAG, "TFT brightness %u%% (duty %lu)", pct, (unsigned long)duty);
#endif
}
void tft_push_fb(uint16_t *fb){
    if(!s_ready || !fb) return;
    /* Byte-swap through the DMA scratch so the FB itself stays in standard
       RGB565 (later region pushes still push swapped bytes). 50 rows/chunk
       => 5 SPI transfers per full frame, same bytes on the wire. */
    if (!s_rowbuf) s_rowbuf = heap_caps_malloc(ROWBUF_BYTES, MALLOC_CAP_DMA);
    if (s_rowbuf) {
        const int per = ROWBUF_BYTES / (TFT_H_RES * 2);
        int r = 0;
        while (r < TFT_V_RES) {
            int n = TFT_V_RES - r;
            if (n > per) n = per;
            s_tx_drain();                       /* previous chunk DMA must finish */
            memcpy(s_rowbuf, fb + (size_t)r * TFT_H_RES, (size_t)TFT_H_RES * n * 2);
            s_swap_buf(s_rowbuf, (size_t)TFT_H_RES * n);
            s_tx_inflight++;
            esp_lcd_panel_draw_bitmap(s_panel, 0, r, TFT_H_RES, r + n, s_rowbuf);
            r += n;
        }
        return;
    }
    /* No 32 KB scratch: per-row swap via the small row buffer */
    if (!s_swaprow) s_swaprow = heap_caps_malloc(TFT_H_RES * 2, MALLOC_CAP_DMA);
    for (int r = 0; r < TFT_V_RES; r++) {
        uint16_t *src = fb + (size_t)r * TFT_H_RES;
        if (s_swaprow) {
            s_tx_drain();
            memcpy(s_swaprow, src, (size_t)TFT_H_RES * 2);
            s_swap_buf(s_swaprow, TFT_H_RES);
            s_tx_inflight++;
            esp_lcd_panel_draw_bitmap(s_panel, 0, r, TFT_H_RES, r + 1, s_swaprow);
        } else {
            /* last resort: unswapped direct push (colors wrong, display alive) */
            s_tx_drain();
            s_tx_inflight++;
            esp_lcd_panel_draw_bitmap(s_panel, 0, r, TFT_H_RES, r + 1, src);
        }
    }
}
// Push a sub-rectangle of the framebuffer (guide 6.1 dirty regions).
// Sub-rect rows are NOT contiguous (stride = full width), so we pack rows
// into a persistent DMA scratch buffer and push in large chunks. The scratch
// is only reused AFTER the previous transfer's DMA completed (s_tx_drain) -
// esp_lcd color transfers are queued asynchronously!

void tft_push_fb_region(uint16_t *fb, int x, int y, int w, int h){
    if(!s_ready || !fb) return;
    if (x<0) { w+=x; x=0; }
    if (y<0) { h+=y; y=0; }
    if (x+w > TFT_H_RES) w = TFT_H_RES - x;
    if (y+h > TFT_V_RES) h = TFT_V_RES - y;
    if (w<=0||h<=0) return;
    if (!s_rowbuf) s_rowbuf = heap_caps_malloc(ROWBUF_BYTES, MALLOC_CAP_DMA);
    int stride = w * 2;
    int chunk  = s_rowbuf ? (int)(ROWBUF_BYTES / stride) : 1;
    if (chunk < 1) chunk = 1;
    for (int r = 0; r < h; r += chunk) {
        int n = (h - r) < chunk ? (h - r) : chunk;
        if (s_rowbuf) {
            s_tx_drain();               /* scratch free? previous DMA done */
            memcpy(s_rowbuf, fb + (size_t)(y + r) * TFT_H_RES + x, (size_t)stride * n);
            s_swap_buf(s_rowbuf, (size_t)w * n);   /* panel wants high-byte-first */
            s_tx_inflight++;
            esp_lcd_panel_draw_bitmap(s_panel, x, y + r, x + w, y + r + n, s_rowbuf);
        } else {
            /* no scratch: per-row swap via small row buffer */
            if (!s_swaprow) s_swaprow = heap_caps_malloc(TFT_H_RES * 2, MALLOC_CAP_DMA);
            uint16_t *src = fb + (size_t)(y + r) * TFT_H_RES + x;
            if (s_swaprow) {
                s_tx_drain();
                memcpy(s_swaprow, src, (size_t)w * 2);
                s_swap_buf(s_swaprow, (size_t)w);
                s_tx_inflight++;
                esp_lcd_panel_draw_bitmap(s_panel, x, y + r, x + w, y + r + 1, s_swaprow);
            } else {
                /* last resort: unswapped direct push (colors wrong, display alive) */
                s_tx_drain();
                s_tx_inflight++;
                esp_lcd_panel_draw_bitmap(s_panel, x, y + r, x + w, y + r + 1, src);
            }
        }
    }
}
uint16_t* tft_get_fb(void){ return s_fb; }
struct esp_lcd_panel_t* tft_get_panel(void){ return (struct esp_lcd_panel_t*)s_panel; }
struct esp_lcd_panel_io_t* tft_get_io(void){ return (struct esp_lcd_panel_io_t*)s_io; }

void tft_fill(uint16_t color) {
    if (!s_ready) return;
    // Use panel fill via buffer chunk (draw bitmap)
    // For speed, allocate line buffer
    const int chunk_h = 16;
    uint16_t *buf = (uint16_t*)heap_caps_malloc(TFT_H_RES * chunk_h * 2, MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "malloc fail");
        return;
    }
    for (int i=0;i<TFT_H_RES*chunk_h;i++) buf[i]=tft_bswap16(color); /* panel byte order */
    for (int y=0; y<TFT_V_RES; y+=chunk_h) {
        int h = (TFT_V_RES - y) < chunk_h ? (TFT_V_RES - y) : chunk_h;
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, TFT_H_RES, y+h, buf);
    }
    heap_caps_free(buf);
}

void tft_fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (!s_ready) return;
    if (x<0) { w+=x; x=0; }
    if (y<0) { h+=y; y=0; }
    if (x+w > TFT_H_RES) w = TFT_H_RES - x;
    if (y+h > TFT_V_RES) h = TFT_V_RES - y;
    if (w<=0||h<=0) return;
    uint16_t *buf = (uint16_t*)heap_caps_malloc(w*h*2, MALLOC_CAP_DMA);
    if (!buf) return;
    for (int i=0;i<w*h;i++) buf[i]=tft_bswap16(color); /* panel byte order */
    s_tx_drain();                       /* async safety: buffer freed below */
    s_tx_inflight++;
    esp_lcd_panel_draw_bitmap(s_panel, x, y, x+w, y+h, buf);
    s_tx_drain();                       /* wait this transfer before free */
    heap_caps_free(buf);
}

static void draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = s_font5x7[c-32];
    for (int col=0; col<5; col++) {
        uint8_t bits = glyph[col];
        for (int row=0; row<7; row++) {
            bool on = bits & (1<<row);
            uint16_t colr = on ? fg : bg;
            if (scale==1) {
                uint16_t px = tft_bswap16(colr); /* panel byte order */
                esp_lcd_panel_draw_bitmap(s_panel, x+col, y+row, x+col+1, y+row+1, &px);
            } else {
                tft_fill_rect(x+col*scale, y+row*scale, scale, scale, colr);
            }
        }
    }
    // one pixel gap
    if (scale==1) {
        for (int row=0;row<7;row++) { uint16_t px=tft_bswap16(bg); esp_lcd_panel_draw_bitmap(s_panel, x+5, y+row, x+6, y+row+1, &px); }
    } else {
        tft_fill_rect(x+5*scale, y, scale, 7*scale, bg);
    }
}

void tft_draw_text(int x, int y, const char *str, uint16_t color, uint16_t bg, int scale) {
    if (!s_ready || !str) return;
    int cx=x;
    while (*str) {
        if (*str=='\n') { y+= 8*scale; cx=x; str++; continue; }
        draw_char(cx, y, *str, color, bg, scale);
        cx += 6*scale;
        if (cx+6*scale > TFT_H_RES) { cx=x; y+=8*scale; }
        str++;
    }
}

void tft_init(void) {
    ESP_LOGI(TAG, "TFT init: SCLK=%d MOSI=%d CS=%d DC=%d RST=%d BL=%d  %dx%d  SPI %d Hz",
        TFT_PIN_SCLK, TFT_PIN_MOSI, TFT_PIN_CS, TFT_PIN_DC, TFT_PIN_RST, TFT_PIN_BL, TFT_H_RES, TFT_V_RES, TFT_SPI_HZ);

#if TFT_PIN_BL != -1
    /* backlight = LEDC_CHANNEL_2 + LEDC_TIMER_3 (free: car.c uses ch0/1 + timers 0/1/2)
       20 kHz PWM on GPIO 13, 10-bit duty -> tft_set_brightness(0..100). */
    ledc_timer_config_t bl_t = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_3,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz         = 20000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_t));
    ledc_channel_config_t bl_c = {
        .gpio_num   = TFT_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_2,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_3,
        .duty       = 1023,          /* 100% default */
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_c));
    ESP_LOGI(TAG, "BL PWM on GPIO %d (LEDC_CHANNEL_2, 20kHz) ON", TFT_PIN_BL);
#else
    ESP_LOGI(TAG, "BL tied to 3.3V (no GPIO)");
#endif

    // Reset pulse
    gpio_config_t rst_cfg = { .pin_bit_mask = 1ULL<<TFT_PIN_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rst_cfg);
    gpio_set_level(TFT_PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(TFT_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));

    spi_bus_config_t buscfg = {
        .mosi_io_num = TFT_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = TFT_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_H_RES * 80 * 2 + 8,
    };
    esp_err_t err = spi_bus_initialize(TFT_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = TFT_PIN_DC,
        .cs_gpio_num = TFT_PIN_CS,
        .pclk_hz = TFT_SPI_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = s_tx_done_cb,   /* async completion tracking */
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)TFT_SPI_HOST, &io_config, &io_handle));
    s_tx_done_sem = xSemaphoreCreateBinary();

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = TFT_PIN_RST,
        // Calibration winner: RGB channel order (MADCTL BGR bit stays 0)
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    // Built-in ST7789 driver (240x320) - also drives ILI9341 2.8" TFT (same init, compatible)
    err = esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 create failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    // Calibration result: INV=OFF + RGB + byte-swap = correct (was invert ON, wrong)
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, false));
    // LANDSCAPE: swap_xy true for 320x240
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false)); // mirror X for correct left-right
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    s_panel = panel_handle;
    s_io = io_handle;
    s_ready = true;
    // allocate framebuffer for flicker-free game (single push per frame)
    s_fb = (uint16_t*)heap_caps_malloc(TFT_H_RES*TFT_V_RES*2, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_fb) {
        // fallback: try PSRAM (DMA capable on S3) then internal non-DMA
        s_fb = (uint16_t*)heap_caps_malloc(TFT_H_RES*TFT_V_RES*2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_fb) ESP_LOGW(TAG,"TFT FB DMA FAIL -> PSRAM fallback %d bytes", TFT_H_RES*TFT_V_RES*2);
    }
    if (!s_fb) s_fb = (uint16_t*)heap_caps_malloc(TFT_H_RES*TFT_V_RES*2, MALLOC_CAP_8BIT);
    if(s_fb) ESP_LOGI(TAG,"TFT FB allocated %d bytes", TFT_H_RES*TFT_V_RES*2);
    else ESP_LOGE(TAG,"TFT FB malloc failed - flicker will remain");
    ESP_LOGI(TAG, "TFT ready %dx%d landscape", TFT_H_RES, TFT_V_RES);
    // NOTE: GPIO14 ab BRAKE LED hai (car.c) - isko yahan reset MAT karo!
    // Purana GPIO14-free block hata diya (wo BL tha, ab BL = GPIO18).
    // tft_test_once removed - now only dual ignition boot anim (tft_boot_anim) runs
}

void tft_test_once(void) {
    if (!s_ready) return;
    ESP_LOGI(TAG, "TFT test: color bars");
    // Color bars
    int bar_h = TFT_V_RES / 8;
    uint16_t colors[8] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_YELLOW, TFT_CYAN, TFT_MAGENTA, TFT_WHITE, TFT_BLACK};
    for (int i=0;i<8;i++) {
        tft_fill_rect(0, i*bar_h, TFT_H_RES, bar_h, colors[i]);
    }
    vTaskDelay(pdMS_TO_TICKS(200)); // fast boot - was 1200

    tft_fill(TFT_BLACK);
    tft_draw_text(10, 10, "ESP32-S3 TFT OK!", TFT_CYAN, TFT_BLACK, 2);
    tft_draw_text(10, 35, "ILI9341 240x320 SPI", TFT_WHITE, TFT_BLACK, 1);
    char buf[64];
    snprintf(buf, sizeof(buf), "SCLK=%d MOSI=%d CS=%d", TFT_PIN_SCLK, TFT_PIN_MOSI, TFT_PIN_CS);
    tft_draw_text(10, 55, buf, TFT_YELLOW, TFT_BLACK, 1);
    snprintf(buf, sizeof(buf), "DC=%d RST=%d BL=%d", TFT_PIN_DC, TFT_PIN_RST, TFT_PIN_BL);
    tft_draw_text(10, 70, buf, TFT_YELLOW, TFT_BLACK, 1);
    tft_draw_text(10, 90, "Wiring: BL->3.3V OK", TFT_GREEN, TFT_BLACK, 1);
    tft_draw_text(10, 110, "Display test PASS", TFT_GREEN, TFT_BLACK, 2);
    tft_draw_text(10, 145, "Next: HUD update...", TFT_GRAY, TFT_BLACK, 1);
    ESP_LOGI(TAG, "TFT test done");
}

void tft_show_status(int mode, int batt_mv, int dist_f, int dist_l, int dist_r, int dist_b, int speed_pct) {
    if (!s_ready) return;
    uint16_t *fb = s_fb;
    (void)dist_b; // rear removed
    if(fb){
        // PREMIUM BIG DASHBOARD - landscape 320x240
        for(int i=0;i<TFT_H_RES*TFT_V_RES;i++) fb[i]=RGB565(12,12,18);
        #define FB_SET(x,y,c) do{ if((x)>=0&&(x)<TFT_H_RES&&(y)>=0&&(y)<TFT_V_RES) fb[(y)*TFT_H_RES+(x)]=(c); }while(0)
        #define FB_RECT(x,y,w,h,c) do{ int _x=(x),_y=(y),_w=(w),_h=(h); if(_x<0){_w+=_x;_x=0;} if(_y<0){_h+=_y;_y=0;} if(_x+_w>TFT_H_RES)_w=TFT_H_RES-_x; if(_y+_h>TFT_V_RES)_h=TFT_V_RES-_y; if(_w>0&&_h>0) for(int _yy=_y;_yy<_y+_h;_yy++) for(int _xx=_x;_xx<_x+_w;_xx++) fb[_yy*TFT_H_RES+_xx]=(c); }while(0)
        // header bar
        FB_RECT(0,0,320,22, RGB565(20,20,30));
        FB_RECT(0,22,320,2, TFT_CYAN);
        // helper to draw text
        char line[48];
        const char *p; int cx, cy, sc;
        // Title
        p="ROBO CAR"; cx=8; cy=6; sc=1; while(*p){ char ch=*p; if(ch>=32&&ch<=126){ const uint8_t *g=s_font5x7[ch-32]; for(int col=0;col<5;col++){uint8_t bits=g[col]; for(int row=0;row<7;row++){bool on=bits&(1<<row); uint16_t colc=on?TFT_CYAN:RGB565(20,20,30); for(int sy=0;sy<sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+col*sc+sx, cy+row*sc+sy, colc);} } for(int sy=0;sy<7*sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+5*sc+sx, cy+sy, RGB565(20,20,30));} cx+=6*sc; p++; }
        const char *mstr = mode==2?"AUTO": mode==1?"CRAWL":"MANUAL";
        snprintf(line,sizeof(line),"%s",mstr);
        uint16_t mcol = mode==2?TFT_GREEN: mode==1?TFT_YELLOW:TFT_WHITE;
        p=line; cx=220; cy=6; sc=1; while(*p){ char ch=*p; if(ch>=32&&ch<=126){ const uint8_t *g=s_font5x7[ch-32]; for(int col=0;col<5;col++){uint8_t bits=g[col]; for(int row=0;row<7;row++){bool on=bits&(1<<row); uint16_t colc=on?mcol:RGB565(20,20,30); for(int sy=0;sy<sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+col*sc+sx, cy+row*sc+sy, colc);} } for(int sy=0;sy<7*sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+5*sc+sx, cy+sy, RGB565(20,20,30));} cx+=6*sc; p++; }
        // BIG FRONT distance centered - scale 5
        char fstr[16];
        uint16_t fcol;
        if(dist_f<0) { snprintf(fstr,sizeof(fstr),"---"); fcol=TFT_GRAY; }
        else if(dist_f<15) { snprintf(fstr,sizeof(fstr),"STOP"); fcol=TFT_RED; }
        else { snprintf(fstr,sizeof(fstr),"%d", dist_f); if(dist_f<30) fcol=TFT_YELLOW; else if(dist_f<80) fcol=TFT_GREEN; else fcol=TFT_WHITE; }
        int flen=strlen(fstr);
        sc= (flen>3)?3:5; // STOP uses 3, numbers use 5
        int fw=flen*6*sc;
        cx=(320-fw)/2; cy=40;
        p=fstr; while(*p){ char ch=*p; if(ch>=32&&ch<=126){ const uint8_t *g=s_font5x7[ch-32]; for(int col=0;col<5;col++){uint8_t bits=g[col]; for(int row=0;row<7;row++){bool on=bits&(1<<row); uint16_t colc=on?fcol:RGB565(12,12,18); for(int sy=0;sy<sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+col*sc+sx, cy+row*sc+sy, colc);} } for(int sy=0;sy<7*sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+5*sc+sx, cy+sy, RGB565(12,12,18));} cx+=6*sc; p++; }
        p="cm"; cx= (320-12)/2; cy=78; sc=1; while(*p){ char ch=*p; if(ch>=32&&ch<=126){ const uint8_t *g=s_font5x7[ch-32]; for(int col=0;col<5;col++){uint8_t bits=g[col]; for(int row=0;row<7;row++){bool on=bits&(1<<row); uint16_t colc=on?TFT_GRAY:RGB565(12,12,18); for(int sy=0;sy<sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+col*sc+sx, cy+row*sc+sy, colc);} } for(int sy=0;sy<7*sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+5*sc+sx, cy+sy, RGB565(12,12,18));} cx+=6*sc; p++; }
        // L/R as side bars with big numbers
        char lstr[16], rstr[16];
        if(dist_l<0) snprintf(lstr,sizeof(lstr),"--"); else snprintf(lstr,sizeof(lstr),"%d",dist_l);
        if(dist_r<0) snprintf(rstr,sizeof(rstr),"--"); else snprintf(rstr,sizeof(rstr),"%d",dist_r);
        FB_RECT(10,100,140,50, RGB565(25,25,35));
        FB_RECT(10,100,140,2, TFT_CYAN);
        p="L"; cx=18; cy=108; sc=1; while(*p){ char ch=*p; if(ch>=32&&ch<=126){ const uint8_t *g=s_font5x7[ch-32]; for(int col=0;col<5;col++){uint8_t bits=g[col]; for(int row=0;row<7;row++){bool on=bits&(1<<row); uint16_t colc=on?TFT_CYAN:RGB565(25,25,35); for(int sy=0;sy<sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+col*sc+sx, cy+row*sc+sy, colc);} } for(int sy=0;sy<7*sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+5*sc+sx, cy+sy, RGB565(25,25,35));} cx+=6*sc; p++; }
        p=lstr; cx=40; cy=118; sc=2; while(*p){ char ch=*p; if(ch>=32&&ch<=126){ const uint8_t *g=s_font5x7[ch-32]; for(int col=0;col<5;col++){uint8_t bits=g[col]; for(int row=0;row<7;row++){bool on=bits&(1<<row); uint16_t colc=on?TFT_WHITE:RGB565(25,25,35); for(int sy=0;sy<sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+col*sc+sx, cy+row*sc+sy, colc);} } for(int sy=0;sy<7*sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+5*sc+sx, cy+sy, RGB565(25,25,35));} cx+=6*sc; p++; }
        FB_RECT(170,100,140,50, RGB565(25,25,35));
        FB_RECT(170,100,140,2, TFT_CYAN);
        p="R"; cx=178; cy=108; sc=1; while(*p){ char ch=*p; if(ch>=32&&ch<=126){ const uint8_t *g=s_font5x7[ch-32]; for(int col=0;col<5;col++){uint8_t bits=g[col]; for(int row=0;row<7;row++){bool on=bits&(1<<row); uint16_t colc=on?TFT_CYAN:RGB565(25,25,35); for(int sy=0;sy<sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+col*sc+sx, cy+row*sc+sy, colc);} } for(int sy=0;sy<7*sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+5*sc+sx, cy+sy, RGB565(25,25,35));} cx+=6*sc; p++; }
        p=rstr; cx=200; cy=118; sc=2; while(*p){ char ch=*p; if(ch>=32&&ch<=126){ const uint8_t *g=s_font5x7[ch-32]; for(int col=0;col<5;col++){uint8_t bits=g[col]; for(int row=0;row<7;row++){bool on=bits&(1<<row); uint16_t colc=on?TFT_WHITE:RGB565(25,25,35); for(int sy=0;sy<sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+col*sc+sx, cy+row*sc+sy, colc);} } for(int sy=0;sy<7*sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+5*sc+sx, cy+sy, RGB565(25,25,35));} cx+=6*sc; p++; }
        // Battery + Speed bar
        FB_RECT(10,165,300,45, RGB565(25,25,35));
        snprintf(line,sizeof(line),"BATT %d.%02dV", batt_mv/1000, (batt_mv%1000)/10);
        uint16_t bcol2 = batt_mv < 9800 ? TFT_RED : batt_mv < 10800 ? TFT_YELLOW : TFT_GREEN;
        p=line; cx=18; cy=172; sc=1; while(*p){ char ch=*p; if(ch>=32&&ch<=126){ const uint8_t *g=s_font5x7[ch-32]; for(int col=0;col<5;col++){uint8_t bits=g[col]; for(int row=0;row<7;row++){bool on=bits&(1<<row); uint16_t colc=on?bcol2:RGB565(25,25,35); for(int sy=0;sy<sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+col*sc+sx, cy+row*sc+sy, colc);} } for(int sy=0;sy<7*sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+5*sc+sx, cy+sy, RGB565(25,25,35));} cx+=6*sc; p++; }
        int bw=280,bh=8;
        int bperc = batt_mv<9800?10: batt_mv>12600?100: (batt_mv-9800)*90/2800+10;
        FB_RECT(18,188,bw,bh, RGB565(40,40,50));
        FB_RECT(18,188,bw*bperc/100,bh, bcol2);
        snprintf(line,sizeof(line),"SPD %d%%", speed_pct);
        p=line; cx=220; cy=172; sc=1; while(*p){ char ch=*p; if(ch>=32&&ch<=126){ const uint8_t *g=s_font5x7[ch-32]; for(int col=0;col<5;col++){uint8_t bits=g[col]; for(int row=0;row<7;row++){bool on=bits&(1<<row); uint16_t colc=on?TFT_YELLOW:RGB565(25,25,35); for(int sy=0;sy<sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+col*sc+sx, cy+row*sc+sy, colc);} } for(int sy=0;sy<7*sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+5*sc+sx, cy+sy, RGB565(25,25,35));} cx+=6*sc; p++; }
        // bottom info
        p="320x240 LANDSCAPE  BL18"; cx=8; cy=222; sc=1; while(*p){ char ch=*p; if(ch>=32&&ch<=126){ const uint8_t *g=s_font5x7[ch-32]; for(int col=0;col<5;col++){uint8_t bits=g[col]; for(int row=0;row<7;row++){bool on=bits&(1<<row); uint16_t colc=on?TFT_GRAY:RGB565(12,12,18); for(int sy=0;sy<sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+col*sc+sx, cy+row*sc+sy, colc);} } for(int sy=0;sy<7*sc;sy++) for(int sx=0;sx<sc;sx++) FB_SET(cx+5*sc+sx, cy+sy, RGB565(12,12,18));} cx+=6*sc; p++; }
        #undef FB_SET
        #undef FB_RECT
        tft_push_fb(fb);
        return;
    }
    // fallback (no fb)
    char line[48];
    tft_fill(TFT_BLACK);
    tft_draw_text(10, 8, "ROBO CAR HUD", TFT_CYAN, TFT_BLACK, 2);
    const char *mstr = mode==2?"AUTO": mode==1?"CRAWL":"MANUAL";
    snprintf(line,sizeof(line),"MODE:%s SPD:%d%%", mstr, speed_pct);
    tft_draw_text(10, 35, line, TFT_WHITE, TFT_BLACK, 1);
    snprintf(line,sizeof(line),"BATT: %d.%02d V", batt_mv/1000, (batt_mv%1000)/10);
    uint16_t bcol = batt_mv < 9800 ? TFT_RED : batt_mv < 10800 ? TFT_YELLOW : TFT_GREEN;
    tft_draw_text(10, 50, line, bcol, TFT_BLACK, 1);
    snprintf(line,sizeof(line),"F:%3d  L:%3d", dist_f, dist_l);
    tft_draw_text(10, 70, line, TFT_WHITE, TFT_BLACK, 1);
    snprintf(line,sizeof(line),"R:%3d  B:%3d", dist_r, dist_b);
    tft_draw_text(10, 85, line, TFT_WHITE, TFT_BLACK, 1);
    int bw = 200, bh=8;
    int ffill = dist_f>=0 && dist_f<200 ? (200-dist_f)*bw/200 : 0;
    tft_fill_rect(10, 110, bw, bh, TFT_GRAY);
    tft_fill_rect(10, 110, ffill, bh, ffill>150?TFT_RED:ffill>100?TFT_YELLOW:TFT_GREEN);
    tft_draw_text(10, 122, "FRONT", TFT_GRAY, TFT_BLACK, 1);
    tft_draw_text(10, 145, "TFT LANDSCAPE 320x240  SCLK46 MOSI13", TFT_GRAY, TFT_BLACK, 1);
    tft_draw_text(10, 160, "BL=3.3V  40MHz  RGB OFF", TFT_GRAY, TFT_BLACK, 1);
}
