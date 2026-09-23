/*
 * img_bank.h - PART 6 image/sprite assets: RGB565 .raw files in
 * SPIFFS (/spiffs/img/<name>.raw + <name>.wh sidecar for dimensions),
 * PSRAM LRU cache, blitted via os_gfx (magenta GFX_KEY = transparent).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint16_t *px;
    uint16_t w, h;
} img_t;

/* Fetch (loads + caches on first use). NULL if missing. */
const img_t *img_get(const char *name);

/* PART 15: slot tables live in PSRAM - call once at boot before any get. */
void img_bank_init(void);

/* Blit with magenta transparency; clipped. No-op on NULL/missing. */
void img_blit(const img_t *img, int x, int y);
/* Half-size blit (every 2nd pixel, transparent) for cockpit sprites. */
void img_blit_half(const img_t *img, int x, int y);
/* Opaque full-screen blit (wallpapers). */
void img_blit_full(const img_t *img);

#ifdef __cplusplus
}
#endif
