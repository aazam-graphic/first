/*
 * os_gfx.c - Car OS framebuffer primitives (5x7 font, rects, bars, blits).
 * Draws into the tft_display internal DMA framebuffer; caller pushes once.
 */
#include "os_gfx.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

/* 5x7 font (ASCII 32..126) */
static const uint8_t s_font[95][5] = {
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
 {0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x08,0x04,0x08,0x10,0x08}
};

uint16_t *gfx_fb(void) { return tft_get_fb(); }

/* scissor clip: intersected with the screen on set; everything funnels
   through px/rect/hline/blit so enforcing there covers all drawing. */
static int s_cx0, s_cy0, s_cx1, s_cy1;
static bool s_clip_init;

void ui_clip_set(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > GFX_W) w = GFX_W - x;
    if (y + h > GFX_H) h = GFX_H - y;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    s_cx0 = x; s_cy0 = y; s_cx1 = x + w; s_cy1 = y + h;
    s_clip_init = true;
}

void ui_clip_reset(void)
{
    s_cx0 = 0; s_cy0 = 0; s_cx1 = GFX_W; s_cy1 = GFX_H;
    s_clip_init = true;
}

static inline bool in_clip(int x, int y)
{
    if (!s_clip_init) return true;
    return x >= s_cx0 && x < s_cx1 && y >= s_cy0 && y < s_cy1;
}

void gfx_clear(uint16_t c)
{
    uint16_t *f = gfx_fb();
    if (!f) return;
    for (int i = 0; i < GFX_W * GFX_H; i++) f[i] = c;
}

void gfx_px(int x, int y, uint16_t c)
{
    if ((unsigned)x >= GFX_W || (unsigned)y >= GFX_H) return;
    if (!in_clip(x, y)) return;
    uint16_t *f = gfx_fb();
    if (!f) return;
    f[y * GFX_W + x] = c;
}

void gfx_rect(int x, int y, int w, int h, uint16_t c)
{
    uint16_t *f = gfx_fb();
    if (!f) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > GFX_W) w = GFX_W - x;
    if (y + h > GFX_H) h = GFX_H - y;
    if (s_clip_init) {
        if (x < s_cx0) { w -= s_cx0 - x; x = s_cx0; }
        if (y < s_cy0) { h -= s_cy0 - y; y = s_cy0; }
        if (x + w > s_cx1) w = s_cx1 - x;
        if (y + h > s_cy1) h = s_cy1 - y;
    }
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        uint16_t *row = f + yy * GFX_W + x;
        for (int xx = 0; xx < w; xx++) row[xx] = c;
    }
}

void gfx_rect_outline(int x, int y, int w, int h, uint16_t c)
{
    gfx_hline(x, y, w, c);
    gfx_hline(x, y + h - 1, w, c);
    gfx_vline(x, y, h, c);
    gfx_vline(x + w - 1, y, h, c);
}

void gfx_hline(int x, int y, int w, uint16_t c)
{
    if (y < 0 || y >= GFX_H) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > GFX_W) w = GFX_W - x;
    if (s_clip_init) {
        if (y < s_cy0 || y >= s_cy1) return;
        if (x < s_cx0) { w -= s_cx0 - x; x = s_cx0; }
        if (x + w > s_cx1) w = s_cx1 - x;
    }
    if (w <= 0) return;
    uint16_t *f = gfx_fb();
    if (!f) return;
    uint16_t *row = f + y * GFX_W + x;
    for (int i = 0; i < w; i++) row[i] = c;
}

void gfx_vline(int x, int y, int h, uint16_t c)
{
    gfx_rect(x, y, 1, h, c);
}

void gfx_char(int x, int y, char ch, uint16_t fg, uint16_t bg, int scale)
{
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t *g = s_font[ch - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++) {
            bool on = bits & (1 << row);
            uint16_t c = on ? fg : bg;
            if (bg == 0 && !on) continue;          /* 0 bg = transparent */
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    gfx_px(x + col * scale + sx, y + row * scale + sy, c);
        }
        if (bg != 0)
            for (int sy = 0; sy < 7 * scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    gfx_px(x + 5 * scale + sx, y + sy, bg);
    }
}

void gfx_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    while (*s) {
        gfx_char(x, y, *s++, fg, bg, scale);
        x += 6 * scale;
    }
}

int gfx_text_w(const char *s, int scale)
{
    int n = 0;
    while (*s++) n++;
    return n * 6 * scale;
}

void gfx_text_center(int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    gfx_text((GFX_W - gfx_text_w(s, scale)) / 2, y, s, fg, bg, scale);
}

void gfx_text_right(int x_right, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    gfx_text(x_right - gfx_text_w(s, scale), y, s, fg, bg, scale);
}

void gfx_bar(int x, int y, int w, int h, int pct, uint16_t border, uint16_t fill)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    gfx_rect(x, y, w, h, RGB565(30,34,46));
    gfx_rect_outline(x, y, w, h, border);
    int fw = (w - 2) * pct / 100;
    if (fw > 0) gfx_rect(x + 1, y + 1, fw, h - 2, fill);
}

void gfx_blit(int x, int y, int w, int h, const uint16_t *src)
{
    uint16_t *f = gfx_fb();
    if (!f || !src) return;
    for (int yy = 0; yy < h; yy++) {
        int dy = y + yy;
        if (dy < 0 || dy >= GFX_H) continue;
        uint16_t *drow = f + dy * GFX_W;
        const uint16_t *srow = src + yy * w;
        for (int xx = 0; xx < w; xx++) {
            int dx = x + xx;
            if (dx < 0 || dx >= GFX_W) continue;
            if (!in_clip(dx, dy)) continue;
            uint16_t c = srow[xx];
            if (c != GFX_KEY) drow[dx] = c;
        }
    }
}

void gfx_blit_full(const uint16_t *src)
{
    uint16_t *f = gfx_fb();
    if (!f || !src) return;
    memcpy(f, src, GFX_W * GFX_H * sizeof(uint16_t));
}

void gfx_text_center_w(int cx, int w, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    gfx_text(cx + (w - gfx_text_w(s, scale)) / 2, y, s, fg, bg, scale);
}

void gfx_push(void)
{
    uint16_t *f = gfx_fb();
    if (f) tft_push_fb(f);
}

/* --------------------- dirty-region tracking (guide 6.1) ------------------ */
#define OS_MAX_DIRTY_RECTS 8
typedef struct { int16_t x, y, w, h; } ui_dirty_rect_t;
static struct {
    ui_dirty_rect_t r[OS_MAX_DIRTY_RECTS];
    uint8_t  count;
    bool     full;
} s_dirty;

void ui_mark_dirty(int x, int y, int w, int h)
{
    if (s_dirty.full) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > GFX_W) w = GFX_W - x;
    if (y + h > GFX_H) h = GFX_H - y;
    if (w <= 0 || h <= 0) return;
    if (w >= GFX_W && h >= GFX_H) { s_dirty.full = true; s_dirty.count = 0; return; }
    if (s_dirty.count >= OS_MAX_DIRTY_RECTS) {     /* too many -> full frame */
        s_dirty.full = true;
        s_dirty.count = 0;
        return;
    }
    s_dirty.r[s_dirty.count].x = (int16_t)x;
    s_dirty.r[s_dirty.count].y = (int16_t)y;
    s_dirty.r[s_dirty.count].w = (int16_t)w;
    s_dirty.r[s_dirty.count].h = (int16_t)h;
    s_dirty.count++;
}

void ui_mark_dirty_full(void) { s_dirty.full = true; s_dirty.count = 0; }
void ui_clear_dirty(void)    { s_dirty.full = false; s_dirty.count = 0; }
bool ui_has_dirty(void)      { return s_dirty.full || s_dirty.count > 0; }

void gfx_push_dirty(void)
{
    uint16_t *f = gfx_fb();
    if (!f) { ui_clear_dirty(); return; }
    if (s_dirty.full) {
        tft_push_fb(f);
    } else {
        for (uint8_t i = 0; i < s_dirty.count; i++) {
            tft_push_fb_region(f, s_dirty.r[i].x, s_dirty.r[i].y,
                               s_dirty.r[i].w, s_dirty.r[i].h);
        }
    }
    ui_clear_dirty();
}
/* ------------------------------- line ------------------------------------- */
void gfx_line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = x1 - x0, dy = y1 - y0;
    int steps = dx < 0 ? -dx : dx;
    int dy2 = dy < 0 ? -dy : dy;
    if (dy2 > steps) steps = dy2;
    if (steps == 0) { gfx_px(x0, y0, c); return; }
    float xi = (float)dx / steps;
    float yi = (float)dy / steps;
    float x = x0, y = y0;
    for (int i = 0; i <= steps; i++) {
        gfx_px((int)(x + 0.5f), (int)(y + 0.5f), c);
        x += xi; y += yi;
    }
}

/* ---------------------------- typography ---------------------------------- */
void gfx_text_small(int x, int y, const char *s, uint16_t c)
{
    gfx_text(x, y, s, c, 0, 1);
}

void gfx_text_medium(int x, int y, const char *s, uint16_t c)
{
    gfx_text(x, y, s, c, 0, 2);
}

void gfx_text_large(int x, int y, const char *s, uint16_t c)
{
    gfx_text(x, y, s, c, 0, 3);
}

void gfx_text_center_box(int x, int y, int w, const char *s, ui_font_t font, uint16_t c)
{
    int scale = (int)font;
    if (scale < 1) scale = 1;
    if (scale > 3) scale = 3;
    int wstr = gfx_text_w(s, scale);
    int x0 = x + (w - wstr) / 2;
    if (x0 < x) x0 = x;
    gfx_text(x0, y, s, c, 0, scale);
}

/* --------------------------- 7-segment digits ----------------------------- */
/* segments a=1 b=2 c=4 d=8 e=16 f=32 g=64 (standard 3x7 grid). */
static const uint8_t s_seg7[10] = {
    63, /* 0 */
    6,  /* 1 */
    91, /* 2 */
    79, /* 3 */
    102,/* 4 */
    109,/* 5 */
    125,/* 6 */
    7,  /* 7 */
    127,/* 8 */
    111 /* 9 */
};

/* segment rects in 3-wide x 7-tall grid: x,y,w,h,mask */
static const struct { int8_t sx, sy, w, h; uint8_t m; } s_seg_rects[7] = {
    { 0, 0, 3, 1, 1  },  /* a top     */
    { 2, 0, 1, 3, 2  },  /* b upper-r */
    { 2, 4, 1, 3, 4  },  /* c lower-r */
    { 0, 6, 3, 1, 8  },  /* d bottom  */
    { 0, 4, 1, 3, 16 },  /* e lower-l */
    { 0, 0, 1, 3, 32 },  /* f upper-l */
    { 0, 3, 3, 1, 64 }   /* g middle  */
};

static void seg7_digit(int x, int y, int s, uint8_t mask, uint16_t on, uint16_t off)
{
    if (off) gfx_rect(x, y, 3 * s, 7 * s, off);
    for (int i = 0; i < 7; i++) {
        if (mask & s_seg_rects[i].m)
            gfx_rect(x + s_seg_rects[i].sx * s, y + s_seg_rects[i].sy * s,
                     s_seg_rects[i].w * s, s_seg_rects[i].h * s, on);
    }
}

void gfx_number_7seg(int x, int y, int value, int digits, int scale,
                     uint16_t on, uint16_t off)
{
    if (digits < 1) digits = 1;
    if (scale < 1) scale = 1;
    if (scale > 5) scale = 5;

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    int len = (int)strlen(buf);

    /* right-aligned: slot 0 is leftmost, digit d is rightmost */
    int slot_w = 4 * scale;
    int cx = x + digits * slot_w - slot_w;
    for (int slot = digits - 1; slot >= 0; slot--) {
        int idx = len - (digits - slot);   /* index into buf for this slot */
        char ch = (idx >= 0) ? buf[idx] : ' ';
        if (ch == '-') {
            seg7_digit(cx, y, scale, 64, on, off);   /* just middle bar */
        } else if (ch >= '0' && ch <= '9') {
            seg7_digit(cx, y, scale, s_seg7[ch - '0'], on, off);
        } else {
            seg7_digit(cx, y, scale, 0, on, off);     /* blank slot */
        }
        cx -= slot_w;
    }
}