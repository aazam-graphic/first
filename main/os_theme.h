/*
 * os_theme.h - Car OS semantic design tokens (color + spacing).
 *
 * Guide (car-os-ui-guide.md) section 3: dark automotive theme.
 *   - Deep blue-charcoal backgrounds, elevated dark slate panels.
 *   - Warm amber  = focus / interaction accent.
 *   - Cyan        = live sensor / data.
 *   - White       = primary information.
 *   - Green/yellow/red = status meaning only.
 * No screen may use hard-coded raw colors directly; use these tokens instead.
 */
#pragma once
#include <stdint.h>
#include "tft_display.h"   /* provides RGB565() */

#ifndef RGB565
#define RGB565(r,g,b) \
    (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#endif

/* ----------------------------- semantic colors ---------------------------- */
#define UI_BG          RGB565(7, 10, 15)
#define UI_SURFACE     RGB565(17, 24, 35)
#define UI_SURFACE_2   RGB565(27, 37, 52)
#define UI_SURFACE_3   RGB565(36, 49, 67)
#define UI_BORDER      RGB565(42, 58, 79)
#define UI_TEXT        RGB565(232, 238, 245)
#define UI_TEXT_2      RGB565(164, 177, 193)
#define UI_MUTED       RGB565(112, 130, 151)
#define UI_ACCENT      RGB565(255, 176, 32)   /* amber - action/focus */
#define UI_ACCENT_DARK RGB565(128, 82, 8)
#define UI_DATA        RGB565(34, 211, 238)   /* cyan - sensor/data */
#define UI_OK          RGB565(53, 208, 127)
#define UI_WARNING     RGB565(255, 197, 61)
#define UI_DANGER      RGB565(255, 59, 48)
#define UI_DISABLED    RGB565(55, 66, 80)

/* ---------------- premium tactical Motion Radar palette ---------------- */
/* Used only by the Motion Radar UI (ui_motion_radar.c). Deep navy tactical
   automotive look; distinct tokens so they don't clash with the OS theme.  */
#define UI_R_BG        RGB565(12, 20, 42)
#define UI_R_PANEL     RGB565(22, 36, 68)
#define UI_R_PANEL_HI  RGB565(30, 48, 84)
#define UI_R_GRID      RGB565(20, 64, 96)
#define UI_R_CYAN      RGB565(0, 242, 255)
#define UI_R_BLUE      RGB565(55, 145, 255)
#define UI_R_GREEN     RGB565(55, 255, 135)
#define UI_R_YELLOW    RGB565(255, 222, 60)
#define UI_R_ORANGE    RGB565(255, 145, 40)
#define UI_R_RED       RGB565(255, 72, 95)
#define UI_R_GREY      RGB565(95, 110, 130)
#define UI_R_TEXT      RGB565(238, 246, 255)
#define UI_R_MUTED     RGB565(112, 130, 151)
#define UI_R_GRID_DIM  RGB565(14, 36, 58)   /* rings recede during alerts */

/* ------------------------------- spacing ---------------------------------- */
#define UI_GAP_XS   4
#define UI_GAP_S    8
#define UI_GAP_M    12
#define UI_GAP_L    16
#define UI_RADIUS   4
#define UI_BORDER_W 1
#define UI_TOP_H    22
#define UI_BOTTOM_H 20

/* layout zones (320x240) */
#define UI_CONTENT_TOP    (UI_TOP_H + 1)     /* 23 */
#define UI_CONTENT_BOTTOM (240 - UI_BOTTOM_H) /* 220 */
#define UI_W 320
#define UI_H 240
