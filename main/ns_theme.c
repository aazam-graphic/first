/*
 * NEON SERPENT: GRID BREACH - "LIGHT CYBERPUNK" visual theme (implementation)
 *
 * Draw-only module. See ns_theme.h for the integration contract.
 *
 * Renderer adapter: the module only needs the shared framebuffer primitives
 * below. If the project prototypes differ slightly, adjust the adapter macros
 * in ONE place (NS_GFX_*) - nothing else in this file calls gfx* directly.
 */
#include "ns_theme.h"
#include "os_gfx.h"           /* Car OS shared framebuffer renderer */

#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------------ */
/* Renderer adapter (single place to map onto the project's gfx API)        */
/* ------------------------------------------------------------------------ */
#ifndef NS_GFX_RECT
#define NS_GFX_RECT(x, y, w, h, c)          gfx_rect((x), (y), (w), (h), (c))
#endif
#ifndef NS_GFX_HLINE
#define NS_GFX_HLINE(x, y, w, c)            gfx_hline((x), (y), (w), (c))
#endif
#ifndef NS_GFX_VLINE
#define NS_GFX_VLINE(x, y, h, c)            gfx_vline((x), (y), (h), (c))
#endif
#ifndef NS_GFX_PX
#define NS_GFX_PX(x, y, c)                  gfx_px((x), (y), (c))
#endif
#ifndef NS_GFX_TEXT
/* Car OS: gfx_text(x, y, s, fg, bg, scale) - bg 0 = dark glyph backing */
#define NS_GFX_TEXT(x, y, s, c, sc)         gfx_text((x), (y), (s), (c), 0, (sc))
#endif

/* ------------------------------------------------------------------------ */
/* Clamped primitives - nothing below may write outside 320x240             */
/* ------------------------------------------------------------------------ */
static int ns_imin(int a, int b) { return a < b ? a : b; }
static int ns_imax(int a, int b) { return a > b ? a : b; }
static int ns_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void ns_rect(int x, int y, int w, int h, uint16_t c)
{
    int x0 = ns_imax(x, 0), y0 = ns_imax(y, 0);
    int x1 = ns_imin(x + w, NS_SCREEN_W), y1 = ns_imin(y + h, NS_SCREEN_H);
    if (x1 <= x0 || y1 <= y0) return;
    NS_GFX_RECT(x0, y0, x1 - x0, y1 - y0, c);
}

static void ns_hline(int x, int y, int w, uint16_t c)
{
    if (y < 0 || y >= NS_SCREEN_H) return;
    int x0 = ns_imax(x, 0), x1 = ns_imin(x + w, NS_SCREEN_W);
    if (x1 <= x0) return;
    NS_GFX_HLINE(x0, y, x1 - x0, c);
}

static void ns_vline(int x, int y, int h, uint16_t c)
{
    if (x < 0 || x >= NS_SCREEN_W) return;
    int y0 = ns_imax(y, 0), y1 = ns_imin(y + h, NS_SCREEN_H);
    if (y1 <= y0) return;
    NS_GFX_VLINE(x, y0, y1 - y0, c);
}

static void ns_px(int x, int y, uint16_t c)
{
    if ((unsigned)x >= NS_SCREEN_W || (unsigned)y >= NS_SCREEN_H) return;
    NS_GFX_PX(x, y, c);
}

static void ns_rect_outline(int x, int y, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0) return;
    ns_hline(x, y, w, c);
    ns_hline(x, y + h - 1, w, c);
    if (h > 2) {
        ns_vline(x, y + 1, h - 2, c);
        ns_vline(x + w - 1, y + 1, h - 2, c);
    }
}

/* Text metrics for the shared 5x7 font */
static int ns_text_w(const char *s, int scale)
{
    if (!s || scale < 1) return 0;
    int n = (int)strlen(s);
    if (n == 0) return 0;
    return (n * NS_FONT_ADV - 1) * scale;
}
static int ns_text_h(int scale) { return NS_FONT_H * (scale < 1 ? 1 : scale); }

/* Draws text fully inside the framebuffer; x is nudged rather than clipped. */
static void ns_text(int x, int y, const char *s, uint16_t c, int scale)
{
    if (!s || !*s) return;
    scale = ns_clampi(scale, 1, 4);
    int w = ns_text_w(s, scale), h = ns_text_h(scale);
    if (w > NS_SCREEN_W || h > NS_SCREEN_H) return;
    x = ns_clampi(x, 0, NS_SCREEN_W - w);
    y = ns_clampi(y, 0, NS_SCREEN_H - h);
    NS_GFX_TEXT(x, y, s, c, scale);
}

static void ns_text_center(int cx, int y, const char *s, uint16_t c, int scale)
{
    ns_text(cx - ns_text_w(s, scale) / 2, y, s, c, scale);
}

static void ns_text_right(int rx, int y, const char *s, uint16_t c, int scale)
{
    ns_text(rx - ns_text_w(s, scale), y, s, c, scale);
}

/* "Bold" 1x text: two sharp passes (x, x+1). Cheap, no blur, thicker strokes. */
static void ns_text_bold(int x, int y, const char *s, uint16_t c)
{
    ns_text(x, y, s, c, 1);
    ns_text(x + 1, y, s, c, 1);
}

/* ------------------------------------------------------------------------ */
/* Required reusable helpers                                                */
/* ------------------------------------------------------------------------ */
static void ns_draw_shadow_panel(int x, int y, int w, int h,
                                 uint16_t fill, uint16_t outer_border, uint16_t inner_border)
{
    if (w < 6 || h < 6) return;
    ns_rect(x + 2, y + 2, w, h, NS_PANEL_DARK);            /* solid shadow */
    ns_rect(x, y, w, h, fill);
    ns_rect_outline(x, y, w, h, outer_border);
    ns_rect_outline(x + 1, y + 1, w - 2, h - 2, inner_border);
}

static void ns_draw_status_bar(int x, int y, int w, int h,
                               int value, int max_value,
                               uint16_t normal_color, uint16_t caution_color, uint16_t danger_color)
{
    if (w < 4 || h < 4) return;
    if (max_value <= 0) max_value = 1;
    value = ns_clampi(value, 0, max_value);

    int inner_w = w - 2;
    int fill_w  = (inner_w * value) / max_value;
    int pct     = (value * 100) / max_value;

    uint16_t col = normal_color;
    if (pct < 25)      col = danger_color;
    else if (pct < 60) col = caution_color;

    ns_rect(x, y, w, h, NS_PANEL_DARK);
    ns_rect_outline(x, y, w, h, NS_GRID_BRIGHT);
    if (fill_w > 0) {
        ns_rect(x + 1, y + 1, fill_w, h - 2, col);
        ns_hline(x + 1, y + 1, fill_w, NS_WHITE);            /* crisp highlight */
    }
    /* quarter ticks keep the bar readable at any fill level */
    for (int i = 1; i < 4; i++) {
        int tx = x + 1 + (inner_w * i) / 4;
        ns_vline(tx, y + h - 3, 2, NS_PANEL_LIGHT);
    }
}

/* Heat is "reverse" semantic: low is good. */
static void ns_draw_heat_bar(int x, int y, int w, int h, int value, int max_value)
{
    if (w < 4 || h < 4) return;
    if (max_value <= 0) max_value = 1;
    value = ns_clampi(value, 0, max_value);
    int inner_w = w - 2;
    int fill_w  = (inner_w * value) / max_value;
    int pct     = (value * 100) / max_value;
    uint16_t col = NS_BLUE;
    if (pct > 70)      col = (pct > 88) ? NS_RED : NS_ORANGE;
    else if (pct >= 40) col = NS_YELLOW;

    ns_rect(x, y, w, h, NS_PANEL_DARK);
    ns_rect_outline(x, y, w, h, NS_GRID_BRIGHT);
    if (fill_w > 0) {
        ns_rect(x + 1, y + 1, fill_w, h - 2, col);
        ns_hline(x + 1, y + 1, fill_w, NS_WHITE);
    }
    for (int i = 1; i < 4; i++) {
        int tx = x + 1 + (inner_w * i) / 4;
        ns_vline(tx, y + h - 3, 2, NS_PANEL_LIGHT);
    }
}

#define NS_ARMOR_BLOCK_W 9
#define NS_ARMOR_BLOCK_H 7
#define NS_ARMOR_GAP     2
#define NS_ARMOR_MAX_BLOCKS 8

/* Maximum block count is remembered per frame so the helper keeps the
 * required (x, y, armor) signature while still drawing empty slots. */
static int  s_armor_max_cache = 3;
static void ns_set_armor_max(int m) { s_armor_max_cache = ns_clampi(m, 1, NS_ARMOR_MAX_BLOCKS); }

static void ns_draw_armor_blocks(int x, int y, int armor)
{
    if (armor < 0) armor = 0;
    if (armor > NS_ARMOR_MAX_BLOCKS) armor = NS_ARMOR_MAX_BLOCKS;
    int max = s_armor_max_cache;
    if (armor > max) max = armor;
    for (int i = 0; i < max; i++) {
        int bx = x + i * (NS_ARMOR_BLOCK_W + NS_ARMOR_GAP);
        if (i < armor) {
            uint16_t c = (armor <= 1) ? NS_RED : (armor == 2 ? NS_YELLOW : NS_GREEN);
            ns_rect(bx, y, NS_ARMOR_BLOCK_W, NS_ARMOR_BLOCK_H, c);
            ns_rect_outline(bx, y, NS_ARMOR_BLOCK_W, NS_ARMOR_BLOCK_H, NS_OBJ_OUTLINE);
            ns_hline(bx + 1, y + 1, NS_ARMOR_BLOCK_W - 2, NS_WHITE);
        } else {
            ns_rect_outline(bx, y, NS_ARMOR_BLOCK_W, NS_ARMOR_BLOCK_H, NS_PANEL_LIGHT);
        }
    }
}

/* Large centred title with a single sharp 1 px navy shadow. Scale is reduced
 * automatically until the text fits inside max_w (never clipped). */
static void ns_draw_large_center_text_fit(int y, const char *text, uint16_t color,
                                          int scale, int cx, int max_w)
{
    if (!text || !*text) return;
    scale = ns_clampi(scale, 1, 3);
    while (scale > 1 && ns_text_w(text, scale) > max_w) scale--;
    int x = cx - ns_text_w(text, scale) / 2;
    ns_text(x + 1, y + 1, text, NS_PANEL_DARK, scale);
    ns_text(x, y, text, color, scale);
}

static void ns_draw_large_center_text(int y, const char *text, uint16_t color, int scale)
{
    ns_draw_large_center_text_fit(y, text, color, scale, NS_SCREEN_W / 2, NS_SCREEN_W - 4);
}

/* Controller button badge:  [A] TEXT  */
static void ns_draw_button_hint(int x, int y, char btn, const char *label,
                                uint16_t btn_color, uint16_t text_color, int scale)
{
    scale = ns_clampi(scale, 1, 2);
    int bh = ns_text_h(scale) + 4;
    int bw = NS_FONT_W * scale + 6;
    char b[2] = { btn, 0 };
    ns_rect(x, y - 2, bw, bh, NS_PANEL_DARK);
    ns_rect_outline(x, y - 2, bw, bh, btn_color);
    ns_text(x + 3, y, b, btn_color, scale);
    ns_text(x + bw + 4, y, label, text_color, scale);
}
static int ns_button_hint_w(const char *label, int scale)
{
    scale = ns_clampi(scale, 1, 2);
    return NS_FONT_W * scale + 6 + 4 + ns_text_w(label, scale);
}

/* ------------------------------------------------------------------------ */
/* Background + playfield                                                   */
/* ------------------------------------------------------------------------ */
static void ns_draw_playfield(int scroll)
{
    /* outer arena border, 2 px visual weight */
    ns_rect_outline(NS_FIELD_LEFT, NS_FIELD_TOP,
                    NS_FIELD_RIGHT - NS_FIELD_LEFT, NS_FIELD_BOTTOM - NS_FIELD_TOP, NS_BORDER);
    ns_rect_outline(NS_FIELD_LEFT + 1, NS_FIELD_TOP + 1,
                    NS_FIELD_RIGHT - NS_FIELD_LEFT - 2, NS_FIELD_BOTTOM - NS_FIELD_TOP - 2, NS_GRID_BRIGHT);

    /* field fill with very subtle alternating column bands (cheap: 19 rects) */
    int off = ((scroll % NS_GRID_STEP) + NS_GRID_STEP) % NS_GRID_STEP;   /* 0..15, wraps */
    ns_rect(NS_FIELD_IN_X, NS_FIELD_IN_Y, NS_FIELD_IN_W, NS_FIELD_IN_H, NS_PLAYFIELD);
    {
        int col = 0;
        for (int gx = NS_FIELD_IN_X - NS_GRID_STEP + off; gx < NS_FIELD_IN_X + NS_FIELD_IN_W; gx += NS_GRID_STEP, col++) {
            if (col & 1) {
                int x0 = ns_imax(gx, NS_FIELD_IN_X);
                int x1 = ns_imin(gx + NS_GRID_STEP, NS_FIELD_IN_X + NS_FIELD_IN_W);
                if (x1 > x0) ns_rect(x0, NS_FIELD_IN_Y, x1 - x0, NS_FIELD_IN_H, NS_BG_ALT);
            }
        }
    }

    /* vertical grid lines: soft, every 4th major */
    {
        int col = 0;
        for (int gx = NS_FIELD_IN_X + off; gx < NS_FIELD_IN_X + NS_FIELD_IN_W; gx += NS_GRID_STEP, col++) {
            ns_vline(gx, NS_FIELD_IN_Y, NS_FIELD_IN_H, ((col + (scroll / NS_GRID_STEP)) & 3) == 0 ? NS_GRID : NS_GRID_SOFT);
        }
    }
    /* horizontal grid lines (static rows) */
    {
        int row = 0;
        for (int gy = NS_FIELD_IN_Y + NS_GRID_STEP; gy < NS_FIELD_IN_Y + NS_FIELD_IN_H; gy += NS_GRID_STEP, row++) {
            ns_hline(NS_FIELD_IN_X, gy, NS_FIELD_IN_W, ((row + 1) & 3) == 0 ? NS_GRID : NS_GRID_SOFT);
        }
    }
}

bool ns_field_contains(int x, int y, int w, int h)
{
    return x >= NS_FIELD_IN_X && y >= NS_FIELD_IN_Y &&
           x + w <= NS_FIELD_IN_X + NS_FIELD_IN_W && y + h <= NS_FIELD_IN_Y + NS_FIELD_IN_H;
}

/* clip a rectangle to the inner playfield (returns false if nothing left) */
static bool ns_clip_field(int *x, int *y, int *w, int *h)
{
    int x0 = ns_imax(*x, NS_FIELD_IN_X), y0 = ns_imax(*y, NS_FIELD_IN_Y);
    int x1 = ns_imin(*x + *w, NS_FIELD_IN_X + NS_FIELD_IN_W);
    int y1 = ns_imin(*y + *h, NS_FIELD_IN_Y + NS_FIELD_IN_H);
    if (x1 <= x0 || y1 <= y0) return false;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return true;
}
static void ns_frect(int x, int y, int w, int h, uint16_t c)
{
    if (ns_clip_field(&x, &y, &w, &h)) ns_rect(x, y, w, h, c);
}
static void ns_fpx(int x, int y, uint16_t c)
{
    int w = 1, h = 1;
    if (ns_clip_field(&x, &y, &w, &h)) ns_px(x, y, c);
}
static void ns_frect_outline(int x, int y, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0) return;
    ns_frect(x, y, w, 1, c);
    ns_frect(x, y + h - 1, w, 1, c);
    ns_frect(x, y, 1, h, c);
    ns_frect(x + w - 1, y, 1, h, c);
}

/* ------------------------------------------------------------------------ */
/* Gameplay objects                                                         */
/* ------------------------------------------------------------------------ */
void ns_draw_snake_segment(int x, int y, int size, int index, bool is_head, int dir_x, int dir_y)
{
    if (size < 4) size = 4;
    if (is_head) {
        ns_frect(x, y, size, size, NS_SNAKE_OUTLINE);
        ns_frect(x + 1, y + 1, size - 2, size - 2, NS_CYAN);
        ns_frect(x + 2, y + 2, size - 4, size - 4, NS_WHITE);
        /* two dark "eyes" facing the travel direction */
        int ex = x + size / 2 + dir_x * (size / 2 - 3);
        int ey = y + size / 2 + dir_y * (size / 2 - 3);
        if (dir_x != 0) { ns_fpx(ex, ey - 2, NS_SNAKE_OUTLINE); ns_fpx(ex, ey + 1, NS_SNAKE_OUTLINE); }
        else            { ns_fpx(ex - 2, ey, NS_SNAKE_OUTLINE); ns_fpx(ex + 1, ey, NS_SNAKE_OUTLINE); }
    } else {
        uint16_t c = (index & 1) ? NS_SNAKE_BODY_B : NS_SNAKE_BODY_A;
        ns_frect(x, y, size, size, NS_SNAKE_OUTLINE);
        ns_frect(x + 1, y + 1, size - 2, size - 2, c);
        /* subtle inner core for a rounded, premium look */
        ns_frect(x + 3, y + 3, size - 6, size - 6, (index & 1) ? NS_PANEL_LIGHT : NS_GRID_BRIGHT);
    }
}

void ns_draw_enemy(int x, int y, int size, ns_enemy_kind_t kind)
{
    if (size < 5) size = 5;
    int half = size / 2;
    switch (kind) {
    case NS_ENEMY_DRONE: {        /* red diamond with dark outline */
        for (int i = 0; i <= half; i++) {
            int w = (i * 2) + 1;
            ns_frect(x + half - i, y + i, w, 1, NS_OBJ_OUTLINE);
            ns_frect(x + half - i, y + size - 1 - i, w, 1, NS_OBJ_OUTLINE);
        }
        for (int i = 1; i < half; i++) {
            int w = (i * 2) - 1;
            ns_frect(x + half - i + 1, y + i, w, 1, NS_ENEMY_RED);
            ns_frect(x + half - i + 1, y + size - 1 - i, w, 1, NS_ENEMY_RED);
        }
        ns_frect(x + half - 1, y + half - 1, 3, 3, NS_WHITE);
        break;
    }
    case NS_ENEMY_HUNTER: {       /* orange chevron */
        ns_frect(x, y, size, size, NS_OBJ_OUTLINE);
        ns_frect(x + 1, y + 1, size - 2, size - 2, NS_ENEMY_ORANGE);
        ns_frect(x + 1, y + 1, size - 2, 2, NS_YELLOW);
        ns_frect(x + half - 1, y + half, 3, size - half - 1, NS_OBJ_OUTLINE);
        break;
    }
    default: {                    /* purple warden block */
        ns_frect(x, y, size, size, NS_OBJ_OUTLINE);
        ns_frect(x + 1, y + 1, size - 2, size - 2, NS_ENEMY_PURPLE);
        ns_frect(x + 3, y + 3, size - 6, size - 6, NS_MAGENTA);
        ns_frect(x + half - 1, y + half - 1, 3, 3, NS_WHITE);
        break;
    }
    }
}

void ns_draw_pickup(int x, int y, int size, ns_pickup_kind_t kind, uint32_t now_ms)
{
    if (size < 6) size = 6;
    /* slow 400 ms pulse on the outline only - never on the fill */
    bool pulse = ((now_ms / NS_BLINK_MS) & 1u) != 0;
    uint16_t core, ring;
    switch (kind) {
    case NS_PICKUP_REPAIR: core = NS_GREEN;   ring = NS_GREEN;   break;
    case NS_PICKUP_SHIELD: core = NS_PANEL;   ring = NS_GREEN;   break;
    case NS_PICKUP_EMP:    core = NS_MAGENTA; ring = NS_MAGENTA; break;
    default:               core = NS_CYAN;    ring = NS_CYAN;    break;
    }
    ns_frect(x, y, size, size, NS_OBJ_OUTLINE);
    ns_frect_outline(x + 1, y + 1, size - 2, size - 2, pulse ? ring : NS_WHITE);
    ns_frect(x + 2, y + 2, size - 4, size - 4, core);
    if (kind == NS_PICKUP_REPAIR) {   /* cross */
        ns_frect(x + size / 2 - 1, y + 2, 2, size - 4, NS_WHITE);
        ns_frect(x + 2, y + size / 2 - 1, size - 4, 2, NS_WHITE);
    } else if (kind == NS_PICKUP_CORE) {
        ns_frect(x + size / 2 - 1, y + size / 2 - 1, 2, 2, NS_WHITE);
    }
}

void ns_draw_mine(int x, int y, int size, uint32_t now_ms)
{
    if (size < 6) size = 6;
    bool on = ((now_ms / NS_BLINK_MS) & 1u) != 0;   /* local blink only */
    ns_frect(x, y, size, size, NS_OBJ_OUTLINE);
    ns_frect(x + 1, y + 1, size - 2, size - 2, NS_PANEL_LIGHT);
    ns_frect_outline(x + 1, y + 1, size - 2, size - 2, on ? NS_RED : NS_ORANGE);
    ns_frect(x + size / 2 - 1, y + size / 2 - 1, 3, 3, on ? NS_RED : NS_PANEL_DARK);
    ns_fpx(x + 1, y + 1, NS_RED); ns_fpx(x + size - 2, y + 1, NS_RED);
    ns_fpx(x + 1, y + size - 2, NS_RED); ns_fpx(x + size - 2, y + size - 2, NS_RED);
}

void ns_draw_projectile(int x, int y, int dir_x, int dir_y, uint16_t color)
{
    /* never a single pixel: 3x3 core plus a short tail */
    ns_frect(x - 2, y - 2, 5, 5, NS_OBJ_OUTLINE);
    ns_frect(x - 1, y - 1, 3, 3, color);
    ns_fpx(x, y, NS_WHITE);
    if (dir_x || dir_y) {
        ns_frect(x - dir_x * 3 - 1, y - dir_y * 3 - 1, dir_x ? 3 : 1, dir_y ? 3 : 1, color);
    }
}

/* ------------------------------------------------------------------------ */
/* Top HUD                                                                  */
/* ------------------------------------------------------------------------ */
static const ns_view_t *s_v;   /* current frame view (set in begin_frame) */

static void ns_draw_top_hud(void)
{
    const ns_view_t *v = s_v;
    char buf[16];

    ns_rect(0, 0, NS_SCREEN_W, NS_HUD_TOP_H, NS_BG_ALT);
    ns_hline(0, NS_HUD_TOP_H - 1, NS_SCREEN_W, NS_GRID_BRIGHT);

    /* Row 1 (y 3..16): ARMOR blocks left, SCORE (2x) right */
    ns_text(8, 7, "ARMOR", NS_TEXT_MUTED, 1);
    ns_set_armor_max(v->armor_max);
    ns_draw_armor_blocks(44, 6, v->armor);

    snprintf(buf, sizeof buf, "%06lu", (unsigned long)(v->score > 999999u ? 999999u : v->score));
    ns_text_right(NS_FIELD_RIGHT, 3, buf, NS_TEXT, 2);                   /* 72 px wide */
    ns_text_right(NS_FIELD_RIGHT - 72 - 6, 7, "SCORE", NS_TEXT_MUTED, 1);

    /* Row 2 (y 20..28): SHIELD bar left, HEAT bar right */
    ns_text(8, 21, "SHIELD", NS_TEXT_MUTED, 1);
    ns_draw_status_bar(48, 20, 84, 9, v->shield, v->shield_max, NS_GREEN, NS_CYAN, NS_YELLOW);
    ns_text(172, 21, "HEAT", NS_TEXT_MUTED, 1);
    ns_draw_heat_bar(202, 20, 110, 9, v->heat, v->heat_max);

    /* Row 3 (y 32..38): MODE + combo */
    ns_text(8, 32, "MODE", NS_TEXT_MUTED, 1);
    ns_text_bold(38, 32, v->mode_name ? v->mode_name : "-", NS_CYAN);
    {
        int combo = v->combo < 1 ? 1 : (v->combo > 99 ? 99 : v->combo);
        snprintf(buf, sizeof buf, "x%d", combo);
        uint16_t cc = combo >= 5 ? NS_MAGENTA : (combo >= 2 ? NS_YELLOW : NS_TEXT_MUTED);
        int w = ns_text_w(buf, 1) + 8;
        ns_rect(NS_FIELD_RIGHT - w, 30, w, 11, NS_PANEL);
        ns_rect_outline(NS_FIELD_RIGHT - w, 30, w, 11, cc);
        ns_text_right(NS_FIELD_RIGHT - 4, 32, buf, cc, 1);
    }
}

/* ------------------------------------------------------------------------ */
/* Bottom HUD                                                               */
/* ------------------------------------------------------------------------ */
static void ns_draw_bottom_hud(void)
{
    const ns_view_t *v = s_v;
    char buf[24];
    const int y0 = NS_SCREEN_H - NS_HUD_BOTTOM_H;     /* 206 */

    ns_rect(0, y0, NS_SCREEN_W, NS_HUD_BOTTOM_H, NS_BG_ALT);
    ns_hline(0, y0, NS_SCREEN_W, NS_GRID_BRIGHT);

    /* Row 1 (y 210..218): STAGE | EMP (or DASH while recharging) | B EXIT */
    snprintf(buf, sizeof buf, "STAGE %d / %d", ns_clampi(v->stage, 0, 99), ns_clampi(v->stage_max, 0, 99));
    ns_text(8, 211, buf, NS_TEXT, 1);

    if (v->dash_recharging) {
        ns_text(136, 211, "DASH", NS_TEXT_MUTED, 1);
        ns_draw_status_bar(164, 210, 64, 9, v->dash, v->dash_max, NS_BLUE, NS_BLUE, NS_BLUE);
    } else {
        ns_text(136, 211, "EMP", NS_TEXT_MUTED, 1);
        ns_draw_status_bar(164, 210, 64, 9, v->emp, v->emp_max, NS_MAGENTA, NS_MAGENTA, NS_TEXT_DIM);
    }
    {
        int w = ns_button_hint_w("EXIT", 1);
        ns_draw_button_hint(NS_FIELD_RIGHT - w, 211, 'B', "EXIT", NS_RED, NS_TEXT, 1);
    }

    /* Row 2 (y 222..236): objective / boss status / danger alert */
    if (v->danger_text && *v->danger_text) {
        ns_rect(NS_FIELD_LEFT, 222, NS_FIELD_RIGHT - NS_FIELD_LEFT, 15, NS_PANEL_DARK);
        ns_rect_outline(NS_FIELD_LEFT, 222, NS_FIELD_RIGHT - NS_FIELD_LEFT, 15, NS_RED);
        ns_text(14, 226, "!", NS_RED, 1);
        ns_text_bold(24, 226, v->danger_text, NS_RED);
    } else if (v->boss_active) {
        ns_text(8, 226, v->boss_name ? v->boss_name : "BOSS", NS_MAGENTA, 1);
        int bx = 8 + ns_text_w(v->boss_name ? v->boss_name : "BOSS", 1) + 8;
        int bw = NS_FIELD_RIGHT - bx - 40;
        if (bw > 40) ns_draw_status_bar(bx, 224, bw, 10, v->boss_hp, v->boss_hp_max, NS_MAGENTA, NS_RED, NS_RED);
        int pct = v->boss_hp_max > 0 ? ns_clampi((v->boss_hp * 100) / v->boss_hp_max, 0, 100) : 0;
        snprintf(buf, sizeof buf, "%d%%", pct);
        ns_text_right(NS_FIELD_RIGHT, 226, buf, NS_TEXT, 1);
    } else {
        ns_text(8, 226, "OBJECTIVE", NS_TEXT_MUTED, 1);
        const char *obj = v->objective ? v->objective : "";
        if (ns_text_w(obj, 2) <= NS_FIELD_RIGHT - 70) ns_text(70, 222, obj, NS_TEXT, 2);
        else                                          ns_text_bold(70, 226, obj, NS_TEXT);
    }
}

/* ------------------------------------------------------------------------ */
/* Overlays / banners                                                       */
/* ------------------------------------------------------------------------ */
#define NS_CARD_X 36
#define NS_CARD_Y 48
#define NS_CARD_W 248
#define NS_CARD_H 154
#define NS_CARD_CX (NS_CARD_X + NS_CARD_W / 2)

static void ns_draw_end_card(uint32_t now_ms, bool win)
{
    const ns_view_t *v = s_v;
    char buf[24];
    uint16_t accent = win ? NS_GREEN : NS_RED;

    ns_draw_shadow_panel(NS_CARD_X, NS_CARD_Y, NS_CARD_W, NS_CARD_H, NS_PANEL, NS_GRID_BRIGHT, accent);

    /* title (3x, auto-fit, single shadow) */
    ns_draw_large_center_text_fit(NS_CARD_Y + 8, win ? "GRID BREACHED" : "GAME OVER",
                                  win ? NS_CYAN : NS_RED, 3, NS_CARD_CX, NS_CARD_W - 8);

    /* FINAL SCORE  000100 */
    snprintf(buf, sizeof buf, "%06lu", (unsigned long)(v->score > 999999u ? 999999u : v->score));
    {
        int lw = ns_text_w("FINAL SCORE", 1), vw = ns_text_w(buf, 2);
        int x  = NS_CARD_CX - (lw + 8 + vw) / 2;
        ns_text(x, NS_CARD_Y + 40, "FINAL SCORE", NS_TEXT_MUTED, 1);
        ns_text(x + lw + 8, NS_CARD_Y + 36, buf, win ? NS_GREEN : NS_YELLOW, 2);
    }

    /* statistics, two clean columns, 11 px pitch */
    {
        const int lx = NS_CARD_X + 56, vx = NS_CARD_X + 136;
        int y = NS_CARD_Y + 56;
        uint32_t t = v->time_s > 5999u ? 5999u : v->time_s;

        ns_text(lx, y, "TIME", NS_TEXT_MUTED, 1);
        snprintf(buf, sizeof buf, "%02lu:%02lu", (unsigned long)(t / 60u), (unsigned long)(t % 60u));
        ns_text(vx, y, buf, NS_TEXT, 1);                     y += 11;

        ns_text(lx, y, "CORES", NS_TEXT_MUTED, 1);
        snprintf(buf, sizeof buf, "%02d", ns_clampi(v->cores, 0, 99));
        ns_text(vx, y, buf, NS_TEXT, 1);                     y += 11;

        ns_text(lx, y, "ENEMIES", NS_TEXT_MUTED, 1);
        snprintf(buf, sizeof buf, "%02d", ns_clampi(v->enemies, 0, 99));
        ns_text(vx, y, buf, NS_TEXT, 1);                     y += 11;

        ns_text(lx, y, "MAX COMBO", NS_TEXT_MUTED, 1);
        snprintf(buf, sizeof buf, "x%d", ns_clampi(v->max_combo, 1, 99));
        ns_text(vx, y, buf, NS_TEXT, 1);
    }

    /* NEW BEST SCORE (blinks 400 ms, text only) / KEEP BREACHING */
    if (v->new_best) {
        if (((now_ms / NS_BLINK_MS) & 1u) == 0)
            ns_text_center(NS_CARD_CX, NS_CARD_Y + 102, "* NEW BEST SCORE *", NS_GREEN, 1);
    } else {
        ns_text_center(NS_CARD_CX, NS_CARD_Y + 102, win ? "MISSION COMPLETE" : "KEEP BREACHING", NS_TEXT_MUTED, 1);
    }

    /* actions */
    {
        int w = ns_button_hint_w("PLAY AGAIN", 2);
        ns_draw_button_hint(NS_CARD_CX - w / 2, NS_CARD_Y + 115, 'A', "PLAY AGAIN", NS_GREEN, NS_GREEN, 2);
        w = ns_button_hint_w("GAMES HUB", 1);
        ns_draw_button_hint(NS_CARD_CX - w / 2, NS_CARD_Y + 137, 'B', "GAMES HUB", NS_TEXT, NS_TEXT, 1);
    }
}

static void ns_draw_game_over_overlay(uint32_t now_ms) { ns_draw_end_card(now_ms, false); }
static void ns_draw_win_overlay(uint32_t now_ms)       { ns_draw_end_card(now_ms, true);  }

static void ns_draw_pause_overlay(void)
{
    const int x = 60, y = 78, w = 200, h = 86, cx = x + w / 2;
    ns_draw_shadow_panel(x, y, w, h, NS_PANEL, NS_GRID_BRIGHT, NS_CYAN);
    ns_draw_large_center_text_fit(y + 10, "PAUSED", NS_CYAN, 3, cx, w - 8);
    ns_hline(x + 20, y + 38, w - 40, NS_GRID_BRIGHT);
    {
        int bw = ns_button_hint_w("START RESUME", 1);   /* drawn as [+] START RESUME */
        ns_text_center(cx, y + 48, "START  RESUME", NS_TEXT, 1);
        (void)bw;
        bw = ns_button_hint_w("GAMES HUB", 1);
        ns_draw_button_hint(cx - bw / 2, y + 64, 'B', "GAMES HUB", NS_TEXT_MUTED, NS_TEXT_MUTED, 1);
    }
}

static void ns_draw_controller_lost_overlay(void)
{
    const int x = 36, y = 72, w = 248, h = 96, cx = x + w / 2;
    ns_draw_shadow_panel(x, y, w, h, NS_PANEL, NS_GRID_BRIGHT, NS_RED);
    ns_draw_large_center_text_fit(y + 10, "CONTROLLER LOST", NS_RED, 3, cx, w - 8);
    ns_hline(x + 20, y + 36, w - 40, NS_GRID_BRIGHT);
    ns_text_center(cx, y + 46, "RECONNECT CONTROLLER", NS_TEXT, 1);
    {
        int bw = ns_button_hint_w("TO RESUME", 1);
        ns_draw_button_hint(cx - bw / 2, y + 66, 'A', "TO RESUME", NS_GREEN, NS_TEXT, 1);
    }
}

/* Slim centred banner - not a modal. */
static void ns_draw_alert_banner_ex(const char *text, const char *sub, uint16_t color, uint16_t border)
{
    if (!text || !*text) return;
    int tw = ns_text_w(text, 2);
    int sw = sub ? ns_text_w(sub, 1) : 0;
    int w  = ns_imax(tw, sw) + 32;
    if (w > NS_FIELD_RIGHT - NS_FIELD_LEFT - 16) w = NS_FIELD_RIGHT - NS_FIELD_LEFT - 16;
    int h  = sub ? 40 : 28;
    int x  = NS_SCREEN_W / 2 - w / 2;
    int y  = (NS_FIELD_TOP + NS_FIELD_BOTTOM) / 2 - h / 2;
    ns_draw_shadow_panel(x, y, w, h, NS_PANEL, NS_GRID_BRIGHT, border);
    ns_draw_large_center_text_fit(y + 7, text, color, 2, NS_SCREEN_W / 2, w - 8);
    if (sub && *sub) ns_text_center(NS_SCREEN_W / 2, y + 26, sub, NS_TEXT, 1);
}

static void ns_draw_alert_banner(const char *text, uint16_t color)
{
    ns_draw_alert_banner_ex(text, NULL, color, color);
}

/* Lightweight toast for pickup / combo: large text floating over the field
 * top, no panel, so it never hides the action. */
static void ns_draw_toast(const char *text, uint16_t color)
{
    if (!text || !*text) return;
    int w = ns_text_w(text, 2) + 16;
    int x = NS_SCREEN_W / 2 - w / 2;
    ns_rect(x, NS_FIELD_TOP + 8, w, 20, NS_PANEL_DARK);
    ns_draw_large_center_text(NS_FIELD_TOP + 11, text, color, 2);
}

static void ns_draw_boss_warning(uint32_t now_ms)
{
    /* animated border only (350 ms), no full-screen flash */
    bool alt = ((now_ms / NS_WARN_BORDER_MS) & 1u) != 0;
    uint16_t border = alt ? NS_MAGENTA : NS_RED;
    ns_draw_alert_banner_ex("GRID WARDEN", "DETECTED", NS_MAGENTA, border);
    /* thin pulse on the arena frame as well - two rect outlines, cheap */
    ns_rect_outline(NS_FIELD_LEFT, NS_FIELD_TOP, NS_FIELD_RIGHT - NS_FIELD_LEFT, NS_FIELD_BOTTOM - NS_FIELD_TOP, border);
}

/* ------------------------------------------------------------------------ */
/* Debug corner (compile-time only)                                         */
/* ------------------------------------------------------------------------ */
#ifdef NS_DEBUG_HUD
static void ns_draw_debug_corner(void)
{
    char buf[24];
    snprintf(buf, sizeof buf, "%dx%d %dFPS", NS_SCREEN_W, NS_SCREEN_H, s_v->debug_fps);
    int w = ns_text_w(buf, 1) + 6;
    ns_rect(NS_FIELD_RIGHT - 2 - w, NS_FIELD_BOTTOM - 13, w, 11, NS_PANEL_DARK);
    ns_text_right(NS_FIELD_RIGHT - 5, NS_FIELD_BOTTOM - 11, buf, NS_TEXT_DIM, 1);
}
#endif

/* ------------------------------------------------------------------------ */
/* Frame entry points                                                       */
/* ------------------------------------------------------------------------ */
static const ns_view_t s_empty_view = { 0 };

void ns_begin_frame(const ns_view_t *v)
{
    s_v = v ? v : &s_empty_view;
    ns_rect(0, 0, NS_SCREEN_W, NS_SCREEN_H, NS_BG);
    ns_draw_playfield(s_v->grid_scroll);
    ns_draw_top_hud();
    ns_draw_bottom_hud();
}

void ns_end_frame(const ns_view_t *v, uint32_t now_ms)
{
    s_v = v ? v : &s_empty_view;
#ifdef NS_DEBUG_HUD
    ns_draw_debug_corner();
#endif
    /* strict priority - exactly one urgent overlay per frame */
    switch (s_v->overlay) {
    case NS_OVERLAY_CONTROLLER_LOST: ns_draw_controller_lost_overlay(); return;
    case NS_OVERLAY_PAUSE:           ns_draw_pause_overlay();           return;
    case NS_OVERLAY_GAME_OVER:       ns_draw_game_over_overlay(now_ms); return;
    case NS_OVERLAY_WIN:             ns_draw_win_overlay(now_ms);       return;
    default: break;
    }
    switch (s_v->banner) {
    case NS_BANNER_BOSS_WARNING: ns_draw_boss_warning(now_ms); break;
    case NS_BANNER_OVERHEAT:     ns_draw_alert_banner_ex("OVERHEAT", "COOLING SYSTEM ACTIVE", NS_ORANGE, NS_ORANGE); break;
    case NS_BANNER_DAMAGE:       ns_draw_alert_banner(s_v->banner_text ? s_v->banner_text : "HULL DAMAGE", NS_RED); break;
    case NS_BANNER_PICKUP:       ns_draw_toast(s_v->banner_text ? s_v->banner_text : "CORE +1", NS_GREEN); break;
    case NS_BANNER_COMBO:        ns_draw_toast(s_v->banner_text ? s_v->banner_text : "COMBO", NS_YELLOW); break;
    default: break;
    }
}
