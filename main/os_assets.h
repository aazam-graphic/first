/*
 * os_assets.h - Car OS assets in PSRAM.
 * Icon canvases (32x32 RGB565, GFX_KEY transparent) + a full-screen wallpaper
 * are rendered ONCE at boot into PSRAM; screens blit from PSRAM every frame.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "os_gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OSA_ICON_WH 32
enum {
    OSA_IC_DRIVE = 0,
    OSA_IC_GRAPH,
    OSA_IC_OLED,
    OSA_IC_SETUP,
    OSA_IC_GAMES,
    OSA_IC_DIAG,
    OSA_IC_COUNT
};

void os_assets_init(void);              /* allocates PSRAM + renders assets once */
bool os_assets_ok(void);                /* true if PSRAM (or fallback) allocated */
const uint16_t *osa_icon(int idx);      /* NULL if not ready */
const uint16_t *osa_wallpaper(void);    /* 320x240 RGB565, NULL if not ready */

#ifdef __cplusplus
}
#endif