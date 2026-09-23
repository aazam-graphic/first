/*
 * os_assets.c - heavy UI assets live in PSRAM (8MB), rendered once at boot.
 * Fallback: if PSRAM is unavailable the same buffers go to internal heap.
 */
#include "os_assets.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

static const char *TAG = "os_assets";

#define ICON_BYTES (OSA_IC_COUNT * OSA_ICON_WH * OSA_ICON_WH * sizeof(uint16_t))
#define WALL_BYTES (GFX_W * GFX_H * sizeof(uint16_t))

static uint16_t *s_icons = NULL;
static uint16_t *s_wall = NULL;
static bool s_ok = false;

/* -------- tiny canvas helpers (write straight into the PSRAM buffers) ----- */
static inline void ipx(uint16_t *base, int x, int y, uint16_t c)
{
    if ((unsigned)x >= OSA_ICON_WH || (unsigned)y >= OSA_ICON_WH) return;
    base[y * OSA_ICON_WH + x] = c;
}

static void irect(uint16_t *base, int x, int y, int w, int h, uint16_t c)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            ipx(base, xx, yy, c);
}

static void iring(uint16_t *base, int cx, int cy, int r, int th, uint16_t c)
{
    for (int y = -r - th; y <= r + th; y++) {
        for (int x = -r - th; x <= r + th; x++) {
            int d2 = x * x + y * y;
            if (d2 <= (r + th) * (r + th) && d2 >= (r - th) * (r - th))
                ipx(base, cx + x, cy + y, c);
        }
    }
}

static void idisc(uint16_t *base, int cx, int cy, int r, uint16_t c)
{
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r) ipx(base, cx + x, cy + y, c);
}

#define ICOL OS_TEAL
#define ICOL2 OS_WHITE

/* ------------------------------- icon art -------------------------------- */
static void icon_drive(uint16_t *b)    /* steering wheel */
{
    iring(b, 16, 16, 12, 3, ICOL);
    idisc(b, 16, 16, 4, ICOL2);
    irect(b, 15, 4, 3, 9, ICOL);
    irect(b, 5, 17, 8, 3, ICOL);
    irect(b, 20, 17, 8, 3, ICOL);
}

static void icon_graph(uint16_t *b)    /* bar chart */
{
    irect(b, 4, 4, 3, 24, ICOL2);
    irect(b, 4, 25, 24, 3, ICOL2);
    irect(b, 10, 16, 4, 9, ICOL);
    irect(b, 16, 10, 4, 15, ICOL);
    irect(b, 22, 5, 4, 20, ICOL);
}

static void icon_oled(uint16_t *b)     /* mini display */
{
    irect(b, 4, 7, 24, 16, ICOL);
    irect(b, 6, 9, 20, 12, RGB565(6,10,14));
    irect(b, 8, 12, 12, 2, ICOL2);
    irect(b, 8, 16, 16, 2, ICOL);
    irect(b, 13, 23, 6, 3, ICOL);
    irect(b, 9, 26, 14, 2, ICOL);
}

static void icon_setup(uint16_t *b)    /* gear */
{
    iring(b, 16, 16, 8, 4, ICOL);
    for (int i = 0; i < 8; i++) {
        float a = i * 0.785398f;
        int tx = 16 + (int)(13.0f * cosf(a)) - 2;
        int ty = 16 + (int)(13.0f * sinf(a)) - 2;
        irect(b, tx, ty, 5, 5, ICOL);
    }
    idisc(b, 16, 16, 3, RGB565(6,10,14));
}

static void icon_games(uint16_t *b)    /* gamepad */
{
    irect(b, 3, 10, 26, 13, ICOL);
    irect(b, 1, 13, 3, 7, ICOL);
    irect(b, 28, 13, 3, 7, ICOL);
    irect(b, 7, 14, 3, 2, ICOL2);      /* dpad cross */
    irect(b, 8, 13, 2, 4, ICOL2);
    idisc(b, 22, 14, 2, ICOL2);
    idisc(b, 25, 19, 2, ICOL2);
    irect(b, 10, 20, 12, 2, RGB565(6,10,14));
}

static void icon_diag(uint16_t *b)     /* wrench + bolt */
{
    iring(b, 10, 10, 6, 3, ICOL);
    irect(b, 8, 8, 5, 5, RGB565(6,10,14));
    for (int i = 0; i < 14; i++) {
        int x = 12 + i, y = 12 + i;
        irect(b, x, y, 4, 4, ICOL);
    }
    irect(b, 20, 4, 3, 12, OS_YELLOW);
    irect(b, 17, 9, 9, 3, OS_YELLOW);
    irect(b, 20, 12, 3, 10, OS_YELLOW);
}

/* ------------------------------- wallpaper ------------------------------- */
static void render_wallpaper(uint16_t *w)
{
    for (int y = 0; y < GFX_H; y++) {
        /* dark vertical gradient: deep navy -> near black */
        int t = y * 100 / GFX_H;
        int r = 14 - t * 10 / 100;
        int g = 18 - t * 13 / 100;
        int bl = 30 - t * 22 / 100;
        uint16_t c = RGB565(r < 0 ? 0 : r, g < 0 ? 0 : g, bl < 0 ? 0 : bl);
        uint16_t *row = w + y * GFX_W;
        for (int x = 0; x < GFX_W; x++) row[x] = c;
    }
    /* perspective grid floor */
    for (int i = 0; i < 7; i++) {
        int y = 150 + i * i * 3;
        if (y >= GFX_H) break;
        uint16_t c = RGB565(10, (45 + i * 4), (55 + i * 5));
        for (int x = 0; x < GFX_W; x++) w[y * GFX_W + x] = c;
    }
    for (int i = -6; i <= 6; i++) {
        int x0 = GFX_W / 2 + i * 24;
        int x1 = GFX_W / 2 + i * 90;
        for (int y = 150; y < GFX_H; y++) {
            int t = (y - 150) * 100 / (GFX_H - 150);
            int x = x0 + (x1 - x0) * t / 100;
            if (x < 0 || x >= GFX_W) continue;
            w[y * GFX_W + x] = RGB565(8, 40, 50);
        }
    }
    /* horizon glow */
    for (int x = 0; x < GFX_W; x++) {
        int d = abs(x - GFX_W / 2);
        int a = 90 - d * 90 / (GFX_W / 2);
        if (a > 0) {
            int gc = 232 * a / 100; if (gc > 255) gc = 255;
            int bc = 202 * a / 100; if (bc > 255) bc = 255;
            w[148 * GFX_W + x] = RGB565(0, gc, bc);
            w[149 * GFX_W + x] = RGB565(0, 60, 55);
        }
    }
}

/* --------------------------------- init ---------------------------------- */
void os_assets_init(void)
{
    if (s_ok) return;
    s_icons = (uint16_t *)heap_caps_malloc(ICON_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_wall  = (uint16_t *)heap_caps_malloc(WALL_BYTES,  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_icons) s_icons = (uint16_t *)malloc(ICON_BYTES);      /* no-PSRAM fallback */
    if (!s_wall)  s_wall  = (uint16_t *)malloc(WALL_BYTES);
    if (!s_icons || !s_wall) {
        ESP_LOGE(TAG, "asset alloc FAILED (icons=%p wall=%p)", s_icons, s_wall);
        if (!s_icons || !s_wall) {
            // Clean up partial alloc to avoid half-initialized state
            if (s_icons) { free(s_icons); s_icons = NULL; }
            if (s_wall) { free(s_wall); s_wall = NULL; }
            return;
        }
    }
    if (s_icons) memset(s_icons, 0, ICON_BYTES);
    if (s_wall) memset(s_wall, 0, WALL_BYTES);

    uint16_t *ic[OSA_IC_COUNT];
    for (int i = 0; i < OSA_IC_COUNT; i++)
        ic[i] = s_icons + i * OSA_ICON_WH * OSA_ICON_WH;

    for (int i = 0; i < OSA_IC_COUNT * OSA_ICON_WH * OSA_ICON_WH; i++)
        s_icons[i] = GFX_KEY;                    /* transparent base */

    icon_drive(ic[OSA_IC_DRIVE]);
    icon_graph(ic[OSA_IC_GRAPH]);
    icon_oled(ic[OSA_IC_OLED]);
    icon_setup(ic[OSA_IC_SETUP]);
    icon_games(ic[OSA_IC_GAMES]);
    icon_diag(ic[OSA_IC_DIAG]);

    render_wallpaper(s_wall);

    s_ok = true;
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "assets ready: icons %uB + wallpaper %uB | PSRAM total=%u free=%u",
             (unsigned)ICON_BYTES, (unsigned)WALL_BYTES,
             (unsigned)psram_total, (unsigned)psram_free);
}

bool os_assets_ok(void) { return s_ok; }

const uint16_t *osa_icon(int idx)
{
    if (!s_ok || idx < 0 || idx >= OSA_IC_COUNT) return NULL;
    return s_icons + idx * OSA_ICON_WH * OSA_ICON_WH;
}

const uint16_t *osa_wallpaper(void)
{
    return s_ok ? s_wall : NULL;
}