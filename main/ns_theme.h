/*
 * NEON SERPENT: GRID BREACH - "LIGHT CYBERPUNK" visual theme
 *
 * Game-local rendering layer for the ESP32-S3 Car OS (320x240 ST7789, RGB565).
 *
 * This module is DRAW-ONLY. It never reads controller input, never touches the
 * Car OS state machine, motor safety, frame pacing or game physics. The game's
 * existing draw function fills an ns_view_t (plain read-only snapshot of the
 * state it already owns) and calls:
 *
 *      ns_begin_frame(&view);            // background + playfield + HUDs
 *      ... existing object drawing (use the ns_draw_* object helpers) ...
 *      ns_end_frame(&view, now_ms);      // overlays / banners (priority-ordered)
 *
 * Constraints honoured:
 *  - integer-only drawing through the existing gfx* primitives
 *  - no malloc/free, no delays, no second framebuffer, no alpha blending
 *  - every helper clamps to the 320x240 framebuffer
 *  - all timed effects use unsigned (now_ms - last_ms) arithmetic, >= 300 ms
 */
#ifndef NS_THEME_H
#define NS_THEME_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Screen / renderer                                                        */
/* ------------------------------------------------------------------------ */
#define NS_SCREEN_W 320
#define NS_SCREEN_H 240


/* ------------------------------------------------------------------------ */
/* Palette - single source of truth, do not duplicate values elsewhere      */
/* ------------------------------------------------------------------------ */
#define NS_BG              RGB565(12, 20, 42)
#define NS_BG_ALT          RGB565(16, 28, 52)
#define NS_PLAYFIELD       RGB565(10, 24, 46)

#define NS_PANEL           RGB565(22, 36, 68)
#define NS_PANEL_LIGHT     RGB565(30, 48, 84)
#define NS_PANEL_DARK      RGB565(12, 22, 44)

#define NS_GRID            RGB565(20, 62, 96)
#define NS_GRID_SOFT       RGB565(16, 44, 72)
#define NS_GRID_BRIGHT     RGB565(0, 140, 185)

#define NS_BORDER          RGB565(0, 215, 240)
#define NS_CYAN            RGB565(0, 242, 255)
#define NS_BLUE            RGB565(55, 145, 255)

#define NS_TEXT            RGB565(238, 246, 255)
#define NS_TEXT_MUTED      RGB565(150, 180, 210)
#define NS_TEXT_DIM        RGB565(92, 120, 150)

#define NS_GREEN           RGB565(55, 255, 135)
#define NS_YELLOW          RGB565(255, 222, 60)
#define NS_ORANGE          RGB565(255, 145, 40)
#define NS_RED             RGB565(255, 72, 95)
#define NS_MAGENTA         RGB565(255, 72, 225)
#define NS_WHITE           RGB565(255, 255, 255)

/* Derived object colours (kept here so the file has one colour section) */
#define NS_SNAKE_OUTLINE   RGB565(4, 30, 60)
#define NS_SNAKE_BODY_A    NS_CYAN
#define NS_SNAKE_BODY_B    NS_BLUE
#define NS_ENEMY_RED       NS_RED
#define NS_ENEMY_ORANGE    NS_ORANGE
#define NS_ENEMY_PURPLE    RGB565(190, 80, 255)
#define NS_OBJ_OUTLINE     RGB565(6, 12, 30)

/* ------------------------------------------------------------------------ */
/* Layout                                                                   */
/* ------------------------------------------------------------------------ */
#define NS_HUD_TOP_H       42
#define NS_HUD_BOTTOM_H    34
#define NS_FIELD_TOP       46
#define NS_FIELD_BOTTOM    204
#define NS_FIELD_LEFT       8
#define NS_FIELD_RIGHT    312
#define NS_GRID_STEP       16

/* Inner drawable playfield (inside the 2 px double border) */
#define NS_FIELD_IN_X      (NS_FIELD_LEFT + 2)
#define NS_FIELD_IN_Y      (NS_FIELD_TOP + 2)
#define NS_FIELD_IN_W      (NS_FIELD_RIGHT - NS_FIELD_LEFT - 4)
#define NS_FIELD_IN_H      (NS_FIELD_BOTTOM - NS_FIELD_TOP - 4)

/* Bitmap font metrics of the shared 5x7 renderer font */
#define NS_FONT_W          5
#define NS_FONT_H          7
#define NS_FONT_ADV        6

/* Timing (ms) */
#define NS_BLINK_MS        400u
#define NS_WARN_BORDER_MS  350u

/* Compile with -DNS_DEBUG_HUD to show FPS / resolution in the field corner */

/* ------------------------------------------------------------------------ */
/* Read-only view of game state                                             */
/* ------------------------------------------------------------------------ */
typedef enum {
    NS_OVERLAY_NONE = 0,
    NS_OVERLAY_GAME_OVER,
    NS_OVERLAY_WIN,
    NS_OVERLAY_PAUSE,
    NS_OVERLAY_CONTROLLER_LOST
} ns_overlay_t;

typedef enum {
    NS_BANNER_NONE = 0,
    NS_BANNER_BOSS_WARNING,
    NS_BANNER_OVERHEAT,
    NS_BANNER_DAMAGE,
    NS_BANNER_PICKUP,
    NS_BANNER_COMBO
} ns_banner_t;

typedef enum {
    NS_ENEMY_DRONE = 0,     /* red diamond      */
    NS_ENEMY_HUNTER,        /* orange chevron   */
    NS_ENEMY_WARDEN         /* purple block     */
} ns_enemy_kind_t;

typedef enum {
    NS_PICKUP_CORE = 0,     /* cyan core        */
    NS_PICKUP_REPAIR,       /* green cross      */
    NS_PICKUP_SHIELD,       /* green ring       */
    NS_PICKUP_EMP           /* magenta bolt     */
} ns_pickup_kind_t;

typedef struct {
    /* --- top HUD --- */
    uint32_t    score;
    int         armor, armor_max;       /* blocks (armor_max <= 8) */
    int         shield, shield_max;
    int         heat, heat_max;
    const char *mode_name;              /* "SHIELD", "EMP", ... (<= 10 chars) */
    int         combo;                  /* 1.. shown as x1, x2 */

    /* --- bottom HUD --- */
    int         stage, stage_max;
    int         emp, emp_max;
    int         dash, dash_max;
    bool        dash_recharging;        /* DASH meter only while recharging */
    const char *objective;              /* e.g. "COLLECT 01/10 CORES" (<= 19 chars) */
    bool        boss_active;
    const char *boss_name;              /* "GRID WARDEN" */
    int         boss_hp, boss_hp_max;
    const char *danger_text;            /* non-NULL => replaces objective row */

    /* --- playfield --- */
    int         grid_scroll;            /* existing continuous scroll, any int */

    /* --- overlays / end-screen statistics --- */
    ns_overlay_t overlay;
    uint32_t    time_s;
    int         cores;
    int         enemies;
    int         max_combo;
    bool        new_best;

    /* --- lower-priority banners (only one is ever drawn) --- */
    ns_banner_t banner;
    const char *banner_text;            /* optional custom text for PICKUP / COMBO */

#ifdef NS_DEBUG_HUD
    int         debug_fps;
#endif
} ns_view_t;

/* ------------------------------------------------------------------------ */
/* Frame API                                                                */
/* ------------------------------------------------------------------------ */
void ns_begin_frame(const ns_view_t *v);               /* bg, playfield, HUDs */
void ns_end_frame(const ns_view_t *v, uint32_t now_ms); /* overlays (priority) */

/* Object helpers (call between begin/end; clipped to the playfield) */
void ns_draw_snake_segment(int x, int y, int size, int index, bool is_head,
                           int dir_x, int dir_y);
void ns_draw_enemy(int x, int y, int size, ns_enemy_kind_t kind);
void ns_draw_pickup(int x, int y, int size, ns_pickup_kind_t kind, uint32_t now_ms);
void ns_draw_mine(int x, int y, int size, uint32_t now_ms);
void ns_draw_projectile(int x, int y, int dir_x, int dir_y, uint16_t color);
bool ns_field_contains(int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif
#endif /* NS_THEME_H */
