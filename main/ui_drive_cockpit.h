/*
 * ui_drive_cockpit.h - DRIVE COCKPIT SCREEN (spec 1.md PART 3, MPU-enriched).
 *
 * VIEW 1 COCKPIT (full, default) / VIEW 2 COMPACT / VIEW 3 SENSOR DATA /
 * VIEW 4 NIGHT HUD. BACK tap cycles views, BACK hold cycles theme.
 * Themes are palette LUT swaps only (no logic change).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "car_os.h"

#ifdef __cplusplus
extern "C" {
#endif

/* cockpit views (BACK tap cycles) */
#define COCKPIT_VIEW_FULL    0
#define COCKPIT_VIEW_COMPACT 1
#define COCKPIT_VIEW_SENSOR  2
#define COCKPIT_VIEW_NIGHT   3
#define COCKPIT_VIEW_COUNT   4

/* theme variants (palette LUT swap only) */
#define COCKPIT_THEME_NIGHT  0   /* NIGHT DRIVE */
#define COCKPIT_THEME_SOLAR  1   /* SOLAR LIGHT (default outdoor) */
#define COCKPIT_THEME_SUN    2   /* SUNLIGHT BOOST */
#define COCKPIT_THEME_COUNT  3

struct os_ctx_t;   /* (kept for doc) real type: os_ctx_t from car_os.h */

/* full cockpit frame (marks dirty regions itself). skip_alert=true when the
   L5 safety overlay is drawn separately on top (PART 10.1 layer model). */
void ui_cockpit_draw(os_ctx_t *ctx, uint32_t now_ms, bool skip_alert);

/* BACK tap / BACK hold handlers (call from os_handle_input DRIVE_MAIN) */
void ui_cockpit_next_view(os_ctx_t *ctx);
void ui_cockpit_next_theme(os_ctx_t *ctx);

const char *ui_cockpit_view_name(uint8_t view);
const char *ui_cockpit_theme_name(uint8_t theme);

/* controller battery % (0..100, -1 = no pad). From existing 3 s battery poll. */
int ui_cockpit_battery_pct(void);

/* dims a framebuffer rect by 40% (alerts dim cockpit behind overlay) */
void ui_cockpit_dim_rect(int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif
