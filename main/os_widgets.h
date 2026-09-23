/*
 * os_widgets.h - Reusable Car OS UI components (guide section 3.5).
 * Built on top of os_gfx primitives (5x7 font + rect/bar/blit).
 * Each widget draws a self-contained element; screens compose them.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "os_theme.h"
#include "car_os.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Small 16-bit geometric icons used by widgets/cards (drawn procedurally). */
typedef enum {
    UI_ICON_DRIVE = 0,   /* steering wheel */
    UI_ICON_GRAPH,       /* bar chart */
    UI_ICON_SCREEN,      /* small display */
    UI_ICON_GEAR,        /* settings gear */
    UI_ICON_GAMES,       /* gamepad */
    UI_ICON_DIAG,        /* wrench + bolt */
    UI_ICON_LIGHT,       /* headlight */
    UI_ICON_HAZARD,      /* triangle */
    UI_ICON_MIST,        /* droplet */
    UI_ICON_SPARK,       /* bolt */
    UI_ICON_AUTO,        /* play/auto */
    UI_ICON_IMU,         /* compass */
    UI_ICON_WARN,        /* exclamation */
    UI_ICON_OK,          /* check */
    UI_ICON_BACK,        /* arrow-left */
    UI_ICON_COUNT
} ui_icon_t;

/* Icons render at 16x16 inside these widgets; cards draw a 24x24 variant. */
#define UI_ICON_SMALL 16
#define UI_ICON_CARD  24

/* --------------------------- reusable components -------------------------- */

/* Top status bar (y 0..21) + divider at y 22. Shows mode / link / gear. */
void ui_draw_topbar(const os_ctx_t *ctx);

/* Bottom action/hint bar (y 220..239). */
void ui_draw_bottombar(const char *left, const char *right);

/* Elevated panel with optional selected highlight + 1px border. */
void ui_panel(int x, int y, int w, int h, bool selected);

/* Selection card: surface + icon + label; selected = amber border + lifted. */
void ui_card(int x, int y, int w, int h, ui_icon_t icon,
             const char *label, bool selected);

/* Horizontal progress bar (value 0..max). */
void ui_progress(int x, int y, int w, int h, int value, int max,
                 uint16_t color);

/* Small pill/chip showing a label; active = filled accent. */
void ui_pill(int x, int y, int w, const char *text, bool active);

/* Status icon: 16x16 geometric icon, dim when inactive. */
void ui_status_icon(int x, int y, ui_icon_t icon, bool active,
                    uint16_t active_color);

/* Centered modal dialog (title / message / confirm / cancel). */
void ui_dialog(const char *title, const char *message,
               const char *confirm, const char *cancel);

/* Draw one of the procedural ui_icon_t glyphs into the fb at (x,y,size). */
void ui_draw_icon(int x, int y, int size, ui_icon_t icon, uint16_t color);

#ifdef __cplusplus
}
#endif
