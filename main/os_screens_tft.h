/*
 * os_screens_tft.h - Car OS TFT screens (raw s_fb rendering).
 * Each screen draws a full frame into the DMA framebuffer; car_os.c pushes.
 */
#pragma once
#include <stdint.h>
#include "car_os.h"

#ifdef __cplusplus
extern "C" {
#endif

void os_scr_boot(const os_ctx_t *ctx, uint32_t now);
void os_scr_standby(os_ctx_t *ctx, uint32_t now);
void os_scr_home(const os_ctx_t *ctx, uint32_t now);
void os_scr_drive(const os_ctx_t *ctx, uint32_t now);
void os_scr_analytics(const os_ctx_t *ctx, uint32_t now);
void os_scr_diag(const os_ctx_t *ctx, uint32_t now);
void os_scr_settings(const os_ctx_t *ctx, uint32_t now);
void os_scr_oledctrl(const os_ctx_t *ctx, uint32_t now);
void os_scr_gameshub(const os_ctx_t *ctx, uint32_t now);
/* Drive overlays (quick/roof/picker/AUTO preview) drawn above the cockpit.
   os_draw_tft calls ui_cockpit_draw() then this (1.md PART 3). */
void os_scr_drive_overlays(const os_ctx_t *ctx);
/* PART 10.1 L5 safety overlay (always top), PART 10.6 app switcher,
   PART 10 new windows + jump list. */
void os_scr_safety_overlay(const os_ctx_t *ctx, uint32_t now);
void os_scr_switcher(const os_ctx_t *ctx, uint32_t now);
void os_scr_score(const os_ctx_t *ctx, uint32_t now);
void os_scr_trip(const os_ctx_t *ctx, uint32_t now);
void os_scr_conn(const os_ctx_t *ctx, uint32_t now);
void os_scr_notif(const os_ctx_t *ctx, uint32_t now);
void os_notif_scroll(int dir, int n);
void os_scr_switcher(const os_ctx_t *ctx, uint32_t now);
void os_scr_jump(const os_ctx_t *ctx);

#ifdef __cplusplus
}
#endif