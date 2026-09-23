/*
 * os_widgets.c - Reusable Car OS UI components (guide section 3.5).
 * All drawing goes to the DMA framebuffer via os_gfx. Components are
 * self-contained; screens compose them. No blocking, no big allocations.
 */
#include "os_widgets.h"
#include "os_gfx.h"
#include "car_global.h"
#include "xbox360.h"
#include <string.h>
#include <stdio.h>

/* --------------------------- procedural icons ---------------------------- */
/* Icons are drawn on a 16x16 virtual grid and scaled by s (= size/16).
   s_ox/s_oy carry the requested draw origin (fb-space). */
static int s_ox = 0;
static int s_oy = 0;

static void Wrect(int x, int y, int w, int h, int s, uint16_t c)
{
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    gfx_rect(s_ox + x * s, s_oy + y * s, w * s, h * s, c);
}

static void Wpx(int x, int y, int s, uint16_t c)
{
    Wrect(x, y, 1, 1, s, c);
}

static void Wring(int cx, int cy, int r, int th, int s, uint16_t c)
{
    for (int yy = -r - th; yy <= r + th; yy++)
        for (int xx = -r - th; xx <= r + th; xx++) {
            int d2 = xx * xx + yy * yy;
            if (d2 <= (r + th) * (r + th) && d2 >= (r - th) * (r - th))
                Wpx(cx + xx, cy + yy, s, c);
        }
}

void ui_draw_icon(int x, int y, int size, ui_icon_t icon, uint16_t color)
{
    int s = size / 16;
    if (s < 1) s = 1;
    s_ox = x;
    s_oy = y;
    switch (icon) {
    case UI_ICON_DRIVE: /* steering wheel */
        Wring(8, 8, 5, 2, s, color);
        Wpx(8, 8, s, color);
        Wrect(7, 2, 2, 3, s, color);
        Wrect(2, 9, 3, 1, s, color);
        Wrect(11, 9, 3, 1, s, color);
        break;
    case UI_ICON_GRAPH: /* bar chart */
        Wrect(2, 2, 2, 12, s, color);
        Wrect(2, 13, 12, 2, s, color);
        Wrect(6, 8, 2, 5, s, color);
        Wrect(10, 4, 2, 9, s, color);
        break;
    case UI_ICON_SCREEN: /* small display */
        Wrect(1, 4, 14, 8, s, color);
        Wrect(3, 6, 10, 4, s, color);
        Wrect(6, 13, 4, 1, s, color);
        break;
    case UI_ICON_GEAR: /* settings gear */
        Wring(8, 8, 4, 2, s, color);
        for (int i = 0; i < 8; i++) {
            int dx = (i % 2) ? 12 : 2;
            int dy = (i / 2 == 0) ? 2 : (i / 2 == 1) ? 12 : (i / 2 == 2) ? 2 : 12;
            Wrect(dx, dy, 2, 2, s, color);
        }
        Wpx(8, 8, s, color);
        break;
    case UI_ICON_GAMES: /* gamepad */
        Wrect(1, 6, 14, 7, s, color);
        Wrect(0, 8, 1, 3, s, color);
        Wrect(15, 8, 1, 3, s, color);
        Wrect(4, 7, 1, 3, s, color);
        Wrect(5, 6, 1, 2, s, color);
        Wpx(10, 8, s, color);
        Wpx(12, 10, s, color);
        break;
    case UI_ICON_DIAG: /* wrench + bolt */
        Wring(5, 5, 3, 2, s, color);
        Wrect(7, 7, 8, 2, s, color);
        Wrect(12, 5, 2, 6, s, color);
        break;
    case UI_ICON_LIGHT: /* headlight */
        Wrect(3, 5, 8, 7, s, color);
        Wring(5, 5, 3, 1, s, color);
        break;
    case UI_ICON_HAZARD: /* triangle */
        for (int yy = 2; yy < 14; yy++) {
            int half = (yy - 2) / 2;
            for (int xx = 8 - half; xx <= 8 + half; xx++)
                Wpx(xx, yy, s, color);
        }
        Wpx(8, 5, s, color);
        break;
    case UI_ICON_MIST: /* droplet */
        Wring(8, 5, 4, 2, s, color);
        Wrect(6, 8, 5, 1, s, color);
        break;
    case UI_ICON_SPARK: /* bolt */
        Wrect(9, 1, 3, 6, s, color);
        Wrect(5, 6, 6, 3, s, color);
        Wrect(4, 8, 3, 7, s, color);
        Wrect(8, 12, 2, 3, s, color);
        break;
    case UI_ICON_AUTO: /* play */
        for (int yy = 3; yy < 13; yy++) {
            int half = (yy - 3) / 2;
            for (int xx = 8 - half; xx <= 8 + half; xx++)
                Wpx(xx, yy, s, color);
        }
        break;
    case UI_ICON_IMU: /* compass */
        Wring(8, 8, 6, 1, s, color);
        Wrect(7, 2, 2, 3, s, color);
        Wrect(7, 11, 2, 3, s, color);
        Wrect(2, 7, 3, 2, s, color);
        Wrect(11, 7, 3, 2, s, color);
        Wpx(8, 8, s, color);
        break;
    case UI_ICON_WARN: /* exclamation */
        Wrect(7, 2, 2, 8, s, color);
        Wpx(8, 12, s, color);
        break;
    case UI_ICON_OK: /* check */
        Wrect(3, 8, 3, 3, s, color);
        Wrect(6, 11, 3, 3, s, color);
        Wrect(9, 7, 3, 4, s, color);
        break;
    case UI_ICON_BACK: /* arrow-left */
        for (int yy = 4; yy < 12; yy++) {
            int half = (yy < 8) ? (yy - 4) : (12 - yy - 1);
            Wrect(2, yy, half + 2, 1, s, color);
        }
        Wrect(3, 6, 10, 4, s, color);
        break;
    default:
        break;
    }
    (void)x; (void)y;
}

/* ------------------------------- top bar ---------------------------------- */
void ui_draw_topbar(const os_ctx_t *ctx)
{
    gfx_rect(0, 0, UI_W, UI_TOP_H, UI_SURFACE);
    gfx_hline(0, UI_TOP_H, UI_W, UI_BORDER);

    const char *m = (g.mode == MODE_AUTO) ? "AUTO" :
                    (g.mode == MODE_CRAWL) ? "CRAWL" : "MANUAL";
    gfx_text(UI_GAP_S, 8, m, UI_ACCENT, 0, 1);

    /* PART 10.5 breadcrumb (‹ HOME › SETTINGS) when nested in windows */
    if (ctx->win_depth) {
        char bc[40];
        os_win_breadcrumb(ctx, bc, sizeof(bc));
        int bw = gfx_text_w(bc, 1);
        int bx = UI_W / 2 - bw / 2;
        if (bx < 70) bx = 70;
        gfx_text(bx, 8, bc, UI_TEXT_2, 0, 1);
    } else {
        char gb[16];
        snprintf(gb, sizeof(gb), "GEAR %u", (unsigned)(ctx->gear + 1));
        gfx_text(UI_W / 2 - 16, 8, gb, UI_TEXT, 0, 1);
    }

    if (g.estop) {
        gfx_text_right(UI_W - UI_GAP_S, 8, "E-STOP", UI_DANGER, 0, 1);
    } else if (xbox360_dongle_connected()) {
        gfx_text_right(UI_W - UI_GAP_S, 8, "PAD", UI_OK, 0, 1);
    } else {
        gfx_text_right(UI_W - UI_GAP_S, 8, "NO PAD", UI_WARNING, 0, 1);
    }
}

/* ----------------------------- bottom bar --------------------------------- */
void ui_draw_bottombar(const char *left, const char *right)
{
    gfx_rect(0, UI_CONTENT_BOTTOM, UI_W, 240 - UI_CONTENT_BOTTOM, UI_SURFACE);
    gfx_hline(0, UI_CONTENT_BOTTOM, UI_W, UI_BORDER);
    if (left && *left)  gfx_text(UI_GAP_S, UI_CONTENT_BOTTOM + 6, left, UI_MUTED, 0, 1);
    if (right && *right) gfx_text_right(UI_W - UI_GAP_S, UI_CONTENT_BOTTOM + 6, right, UI_MUTED, 0, 1);
}

/* ------------------------------- components ------------------------------- */
void ui_panel(int x, int y, int w, int h, bool selected)
{
    gfx_rect(x, y, w, h, selected ? UI_SURFACE_2 : UI_SURFACE);
    gfx_rect_outline(x, y, w, h, selected ? UI_ACCENT : UI_BORDER);
}

void ui_card(int x, int y, int w, int h, ui_icon_t icon,
             const char *label, bool selected)
{
    /* redesign pack 4: big white icon tiles, cyan focus glow. */
    int lift = selected ? 2 : 0;
    if (selected) {
        gfx_rect_outline(x - 3, y + lift - 3, w + 6, h + 6, UI_DATA);  /* outer glow */
    }
    gfx_rect(x, y + lift, w, h, selected ? UI_SURFACE_2 : UI_SURFACE);
    gfx_rect_outline(x, y + lift, w, h, selected ? UI_DATA : UI_BORDER);
    if (selected) {
        gfx_rect_outline(x + 1, y + lift + 1, w - 2, h - 2, UI_DATA);  /* 2px border */
    }
    /* 32px white icon (redesign pack: large white iconography) */
    ui_draw_icon(x + (w - 32) / 2, y + lift + 4, 32, icon, UI_TEXT);
    uint16_t lc = selected ? UI_TEXT : UI_TEXT_2;
    gfx_text_center_w(x, w, y + lift + h - 18, label, lc, 0, 1);
}

void ui_progress(int x, int y, int w, int h, int value, int max, uint16_t color)
{
    if (value < 0) value = 0;
    if (max <= 0) max = 1;
    if (value > max) value = max;
    gfx_rect(x, y, w, h, UI_SURFACE_3);
    gfx_rect_outline(x, y, w, h, UI_BORDER);
    int fw = (w - 2) * value / max;
    if (fw > 0) gfx_rect(x + 1, y + 1, fw, h - 2, color);
}

void ui_pill(int x, int y, int w, const char *text, bool active)
{
    uint16_t bg = active ? UI_ACCENT : UI_SURFACE_3;
    uint16_t fg = active ? (uint16_t)RGB565(20, 12, 0) : UI_MUTED;
    gfx_rect(x, y, w, 18, bg);
    gfx_rect_outline(x, y, w, 18, active ? UI_ACCENT : UI_BORDER);
    gfx_text_center_w(x, w, y + 5, text, fg, 0, 1);
}

void ui_status_icon(int x, int y, ui_icon_t icon, bool active, uint16_t active_color)
{
    gfx_rect(x, y, 22, 22, UI_SURFACE);
    gfx_rect_outline(x, y, 22, 22, active ? active_color : UI_BORDER);
    ui_draw_icon(x + 3, y + 3, 16, icon, active ? active_color : UI_MUTED);
}

void ui_dialog(const char *title, const char *message,
               const char *confirm, const char *cancel)
{
    int x = 40, y = 84, w = 240, h = 72;
    gfx_rect(x, y, w, h, UI_SURFACE_2);
    gfx_rect_outline(x, y, w, h, UI_ACCENT);
    gfx_text_center_w(x, w, y + 8, title, UI_TEXT, 0, 1);
    if (message && *message) {
        gfx_text_center_w(x, w, y + 26, message, UI_TEXT_2, 0, 1);
    }
    int cw = cancel ? 52 : 0;
    int px = x + w / 2 - (cw + 12 + 52) / 2;
    if (cancel) {
        gfx_rect(px, y + h - 26, 52, 18, UI_SURFACE_3);
        gfx_rect_outline(px, y + h - 26, 52, 18, UI_BORDER);
        gfx_text_center_w(px, 52, y + h - 21, cancel, UI_MUTED, 0, 1);
    }
    gfx_rect(px + cw + 12, y + h - 26, 52, 18, UI_ACCENT);
    gfx_rect_outline(px + cw + 12, y + h - 26, 52, 18, UI_ACCENT);
    gfx_text_center_w(px + cw + 12, 52, y + h - 21, confirm, RGB565(20, 12, 0), 0, 1);
}

