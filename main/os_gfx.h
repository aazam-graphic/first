/*
 * os_gfx.h - Car OS framebuffer graphics primitives.
 * All drawing targets the internal-RAM DMA framebuffer from tft_display.c
 * (tft_get_fb), single push per frame via gfx_push().
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "tft_display.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_W  TFT_H_RES
#define GFX_H  TFT_V_RES
#define GFX_KEY 0xF81F   /* magenta = transparent key for sprite blits */

uint16_t *gfx_fb(void);
void gfx_clear(uint16_t c);
/* scissor clip (PART 7.0 §5): every primitive below honors it.
   Default = full screen. Cockpit sets one rect per widget. */
void ui_clip_set(int x, int y, int w, int h);
void ui_clip_reset(void);
void gfx_px(int x, int y, uint16_t c);
void gfx_rect(int x, int y, int w, int h, uint16_t c);
void gfx_rect_outline(int x, int y, int w, int h, uint16_t c);
void gfx_hline(int x, int y, int w, uint16_t c);
void gfx_vline(int x, int y, int h, uint16_t c);
void gfx_char(int x, int y, char ch, uint16_t fg, uint16_t bg, int scale);
void gfx_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale);
int  gfx_text_w(const char *s, int scale);
void gfx_text_center(int y, const char *s, uint16_t fg, uint16_t bg, int scale);
void gfx_text_right(int x_right, int y, const char *s, uint16_t fg, uint16_t bg, int scale);
void gfx_text_center_w(int cx, int w, int y, const char *s, uint16_t fg, uint16_t bg, int scale);
void gfx_bar(int x, int y, int w, int h, int pct, uint16_t border, uint16_t fill);
void gfx_line(int x0, int y0, int x1, int y1, uint16_t c);
void gfx_blit(int x, int y, int w, int h, const uint16_t *src);        /* GFX_KEY transparent */
void gfx_blit_full(const uint16_t *src);                               /* whole-screen copy */
void gfx_push(void);                                                   /* tft_push_fb */

/* --------------------- dirty-region tracking (guide 6.1) ------------------ */
/* Screens mark the rectangles that changed this frame; gfx_push_dirty()
   sends only those regions (or the whole frame when too many/unspecified). */
void ui_mark_dirty(int x, int y, int w, int h);   /* add region (>max -> full) */
void ui_mark_dirty_full(void);                    /* force whole-frame push   */
void ui_clear_dirty(void);
bool ui_has_dirty(void);
void gfx_push_dirty(void);                        /* push dirty rects or full */

/* ---------------------------- typography ---------------------------------- */
/* Guide 3.4: font hierarchy. Small labels use the 5x7 font scale 1;
   medium/large use scale 2/3 for titles and group headers. Hero values
   (speed/score/timer) should use gfx_number_7seg(). */
typedef enum {
    UI_FONT_SMALL = 1,   /* 5x7  (labels, hints)             */
    UI_FONT_MEDIUM = 2,  /* 10x14 (card labels, section titles) */
    UI_FONT_LARGE = 3    /* 15x21 (screen titles)            */
} ui_font_t;

void gfx_text_small(int x, int y, const char *s, uint16_t c);
void gfx_text_medium(int x, int y, const char *s, uint16_t c);
void gfx_text_large(int x, int y, const char *s, uint16_t c);
/* horizontally center text inside (x, y, w) rectangle */
void gfx_text_center_box(int x, int y, int w, const char *s, ui_font_t font, uint16_t c);
/* 7-segment style digits (right-aligned into `digits` slots), scale = 1..5 */
void gfx_number_7seg(int x, int y, int value, int digits, int scale,
                     uint16_t on, uint16_t off);

/* OS palette */
#define OS_BG        RGB565(10,12,18)
#define OS_BG2       RGB565(18,22,32)
#define OS_PANEL     RGB565(24,28,40)
#define OS_TEAL      RGB565(0,232,202)
#define OS_CYAN      RGB565(0,190,255)
#define OS_GREEN     RGB565(10,250,112)
#define OS_YELLOW    RGB565(255,208,0)
#define OS_ORANGE    RGB565(255,108,0)
#define OS_RED       RGB565(255,30,26)
#define OS_MAGENTA   RGB565(255,0,255)
#define OS_WHITE     RGB565(235,240,245)
#define OS_GREY      RGB565(120,130,145)
#define OS_DGREY     RGB565(55,62,78)

#ifdef __cplusplus
}
#endif