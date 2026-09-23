#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 2.8" SPI TFT (ILI9341 240x320 / ST7789 compatible)
// Wiring - 5-wire SPI, BL hardwired to 3.3V:
//   SCLK -> GPIO 21
//   MOSI -> GPIO 38
//   CS   -> GPIO 45
//   DC   -> GPIO 0
//   RST  -> GPIO 2
//   BL   -> 3.3V direct (koi GPIO nahi - BL yahan kabhi mat lagao)
//   MISO -> not connected

// -1 = BL tied to 3.3V (koi GPIO nahi).
#define TFT_PIN_BL   -1

/* Pin defines yahan (header) rakhe gaye hain taaki car.c ka pin-uniqueness
   guard sab modules ke pins ek saath verify kar sake (review 2026-09-17). */
#define TFT_PIN_SCLK  21  /* was 46 (46 -> headlight relay) */
#define TFT_PIN_MOSI  38  /* was 13 */
#define TFT_PIN_CS    45
#define TFT_PIN_DC    0
#define TFT_PIN_RST   2   /* was 16 (16 -> mist relay) */
// NOTE: GPIO37/36/35 PSRAM ke internal pins hain (N16R8) - unpe kabhi BL mat lagao.

// Panel size - LANDSCAPE 320x240 (rotate 90)
#define TFT_H_RES 320
#define TFT_V_RES 240

void tft_init(void);
bool tft_is_ready(void);
// backlight brightness no-op (BL tied to 3.3V direct — TFT_PIN_BL = -1)
void tft_set_brightness(uint8_t pct);
struct esp_lcd_panel_t; struct esp_lcd_panel_io_t;
struct esp_lcd_panel_t* tft_get_panel(void);
struct esp_lcd_panel_io_t* tft_get_io(void);

// basic drawing (RGB565 16-bit)
void tft_fill(uint16_t color);
void tft_fill_rect(int x, int y, int w, int h, uint16_t color);
void tft_draw_text(int x, int y, const char *str, uint16_t color, uint16_t bg, int scale);
// framebuffer (240*320) - fast single push, no flicker
void tft_push_fb(uint16_t *fb);
void tft_push_fb_region(uint16_t *fb, int x, int y, int w, int h); // dirty-rect push
uint16_t* tft_get_fb(void); // returns internal DMA fb if allocated

// high level test / status
void tft_test_once(void);   // color bars + info
void tft_show_status(int mode, int batt_mv, int dist_f, int dist_l, int dist_r, int dist_b, int speed_pct);

#define RGB565(r,g,b) ( ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3) )
#define TFT_BLACK   0x0000
#define TFT_WHITE   0xFFFF
#define TFT_RED     RGB565(255,0,0)
#define TFT_GREEN   RGB565(0,255,0)
#define TFT_BLUE    RGB565(0,0,255)
#define TFT_YELLOW  RGB565(255,255,0)
#define TFT_CYAN    RGB565(0,255,255)
#define TFT_MAGENTA RGB565(255,0,255)
#define TFT_ORANGE  RGB565(255,165,0)
#define TFT_GRAY    RGB565(128,128,128)

#ifdef __cplusplus
}
#endif
