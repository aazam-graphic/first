/*
 * os_screens_tft.c - Car OS screens. All rendering goes into the tft_display
 * DMA framebuffer (s_fb) via os_gfx primitives; wallpaper/icons blit from PSRAM.
 */
#include "os_screens_tft.h"
#include "os_gfx.h"
#include "os_assets.h"
#include "os_theme.h"
#include "os_widgets.h"
#include "car_games.h"
#include "car_global.h"
#include "car_os.h"
#include "roof_light.h"
#include "xbox360.h"
#include "ui_motion_radar.h"
#include "imu_driver.h"
#include "imu_adv.h"  /* PART 10: score/trip windows */
#include "ui_drive_cockpit.h"
#include "mic_in.h"   /* PART 8: DIAG VU meter */
#include "img_bank.h" /* PART 6: boot logo + bg */
#include "alexa_bridge.h" /* PART 10: QCC profile name */
#include "notif.h"    /* PART 10.3 Notification Center */
#include "net_wifi.h" /* PART 10: connectivity window */
#include "snd_bank.h" /* standby: first-sync ding */
#include <time.h>     /* standby clock: IST wall time */
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ------------------------------ helpers ---------------------------------- */
static const char *ast_name_ui(uint8_t ast)
{
    switch (ast) {
    case AST_CRUISE:  return "CRUISE";
    case AST_SLOW:    return "SLOW";
    case AST_TURN_L:  return "TURN_L";
    case AST_TURN_R:  return "TURN_R";
    case AST_REVERSE: return "REVERSE";
    case AST_ESCAPE:  return "ESCAPE";
    case AST_STUCK:   return "STUCK";
    case AST_PAUSE:   return "PAUSE";
    case AST_SEARCH:  return "SEARCH";
    default:          return "?";
    }
}

static const char *mode_name(uint8_t m)
{
    return m == MODE_AUTO ? "AUTO" : m == MODE_CRAWL ? "CRAWL" : "MANUAL";
}

/* common chrome (guide 3.5): consistent top/bottom bars on every screen.
   Screens call ui_draw_topbar(ctx) + ui_draw_bottombar(left, right). */

/* bottom bar with per-state hints */
static void scr_footer(const os_ctx_t *ctx)
{
    ui_draw_bottombar(os_state_hint(ctx->state), os_state_name(ctx->state));
}

/* ------------------------------ BOOT ------------------------------------- */
#define BOOT_TOTAL_MS 1500

static void scr_hexagon(int cx, int cy, int r, uint16_t col)
{
    for (int i = 0; i < 6; i++) {
        double a0 = i * (M_PI / 3.0) - M_PI / 2.0;
        double a1 = (i + 1) * (M_PI / 3.0) - M_PI / 2.0;
        gfx_line(cx + (int)(r * cos(a0)), cy + (int)(r * sin(a0)),
                 cx + (int)(r * cos(a1)), cy + (int)(r * sin(a1)), col);
    }
}

void os_scr_boot(const os_ctx_t *ctx, uint32_t now)
{
    /* PART 6: boot wallpaper + logo (procedural fallback if missing) */
    const img_t *bbg = img_get("boot_bg");
    if (bbg) img_blit_full(bbg);
    else gfx_clear(UI_BG);
    const img_t *blg = img_get("boot_logo");
    uint32_t t = now - ctx->state_enter_ms;
    if (t > BOOT_TOTAL_MS) t = BOOT_TOTAL_MS;

    if (blg) {
        img_blit(blg, 80, 2);
    } else {
        /* emblem: hexagon lines draw outward over 0-300 ms (guide 5.1) */
        int r = 10 + (22 * (int)t / 300);
        if (r > 32) r = 32;
        scr_hexagon(160, 48, r, UI_ACCENT);
        scr_hexagon(160, 48, r - 6, UI_ACCENT_DARK);
        if (t > 180) {                 /* inner car glyph */
            gfx_rect(152, 50, 16, 11, UI_TEXT_2);
            gfx_rect(156, 44, 8, 6, UI_TEXT_2);
        }
    }

    if (t >= 300) {                /* title appears 300-800 ms */
        gfx_text_center_box(0, 104, UI_W, "AZAM CAR OS", UI_FONT_LARGE, UI_TEXT);
        gfx_hline(80, 128, 160, UI_BORDER);
    }
    if (t >= 500)
        gfx_text_center_box(0, 138, UI_W, "ROBOTICS CONTROL SYSTEM",
                            UI_FONT_SMALL, UI_MUTED);

    /* amber progress line x=40..280 */
    int pct = (int)t * 100 / BOOT_TOTAL_MS;
    ui_progress(40, 164, 240, 6, pct, 100, UI_ACCENT);

    /* bottom boot checklist: CONTROL SENSORS DISPLAY PAD */
    static const char *items[4] = { "CONTROL", "SENSORS", "DISPLAY", "PAD" };
    for (int i = 0; i < 4; i++) {
        int cx = 62 + i * 68;
        bool ok = t >= (uint32_t)(800 + i * 120);
        ui_status_icon(cx - 11, 182, ok ? UI_ICON_OK : UI_ICON_WARN, ok, UI_OK);
        gfx_text_center_box(cx - 30, 208, 60, items[i], UI_FONT_SMALL,
                            ok ? UI_OK : UI_MUTED);
    }
}

/* --------------------- STANDBY (table clock) ---------------------------- */
/* Premium minimal table clock: cream card, giant time, date top, weekday +
   numeric date below, subtle PRESS START. No status rows, no footers.
   Sync appears ONLY as a 1.5 s toast after SNTP locks. Before sync the
   time shows "--:--" placeholders — never a faked time. */
#define SB_BG    RGB565(250, 247, 240)
#define SB_INK   RGB565(32, 32, 36)
#define SB_MUTED RGB565(130, 128, 122)
#define SB_GOLD  RGB565(178, 141, 87)
#define SB_TRACK RGB565(222, 218, 210)
#define SB_PILL  RGB565(232, 222, 196)

static const uint8_t s_sb_seg[10] = {
    63, 6, 91, 79, 102, 109, 125, 7, 127, 111
};
static const struct { int8_t sx, sy, w, h; uint8_t m; } s_sb_rects[7] = {
    { 0, 0, 3, 1, 1 }, { 2, 0, 1, 3, 2 }, { 2, 4, 1, 3, 4 },
    { 0, 6, 3, 1, 8 }, { 0, 4, 1, 3, 16 }, { 0, 0, 1, 3, 32 },
    { 0, 3, 3, 1, 64 }
};

/* oversized 7-seg digit: s = pixel size of one grid cell */
static void sb_digit(int x, int y, int s, int d, uint16_t on)
{
    uint8_t mask = (d >= 0 && d <= 9) ? s_sb_seg[d] : 0;
    for (int i = 0; i < 7; i++) {
        if (mask & s_sb_rects[i].m)
            gfx_rect(x + s_sb_rects[i].sx * s, y + s_sb_rects[i].sy * s,
                     s_sb_rects[i].w * s, s_sb_rects[i].h * s, on);
    }
}

static void sb_str_upper(char *s)
{
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
}

void os_scr_standby(os_ctx_t *ctx, uint32_t now)
{
    (void)ctx;
    gfx_clear(SB_BG);
    bool synced = net_wifi_time_synced();

    /* first-sync: ding + 1.5 s toast, then never again */
    static bool s_dinged = false;
    static uint32_t s_toast_until = 0;
    if (synced && !s_dinged) {
        s_dinged = true;
        s_toast_until = now + 1500;
        snd_play("ui_toggle_on");
    }

    /* top date + thin progress line (seconds within the minute) */
    if (synced) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        char db[28];
        strftime(db, sizeof(db), "%a, %d %b %Y", &tm);
        sb_str_upper(db);
        gfx_text_medium(20, 14, db, SB_INK);
        gfx_rect(20, 36, 200, 3, SB_TRACK);
        int fill = 200 * tm.tm_sec / 60;
        if (fill > 0) gfx_rect(20, 36, fill, 3, SB_GOLD);

        /* giant HH:MM + smaller SS + AM/PM pill (12-hour) */
        int hh = tm.tm_hour % 12;
        if (hh == 0) hh = 12;
        bool pm = tm.tm_hour >= 12;
        int s = 10, slot = 4 * s;               /* digit 30x70 */
        int x0 = 30, y0 = 56;
        sb_digit(x0, y0, s, hh / 10, SB_INK);
        sb_digit(x0 + slot, y0, s, hh % 10, SB_INK);
        if (((now / 500) & 1) == 0) {           /* classic colon blink */
            gfx_rect(x0 + 2 * slot + 4, y0 + 16, 9, 9, SB_INK);
            gfx_rect(x0 + 2 * slot + 4, y0 + 45, 9, 9, SB_INK);
        }
        sb_digit(x0 + 2 * slot + 22, y0, s, tm.tm_min / 10, SB_INK);
        sb_digit(x0 + 3 * slot + 22, y0, s, tm.tm_min % 10, SB_INK);
        /* seconds, smaller, top-right of the big digits */
        int sx = x0 + 4 * slot + 34, ss = 5;
        sb_digit(sx, y0 + 2, ss, tm.tm_sec / 10, SB_MUTED);
        sb_digit(sx + 4 * ss + 4, y0 + 2, ss, tm.tm_sec % 10, SB_MUTED);
        /* AM/PM pill under seconds */
        gfx_rect(sx - 2, y0 + 44, 52, 20, SB_PILL);
        gfx_text_small(sx + 8, y0 + 49, pm ? "PM" : "AM", SB_INK);

        /* weekday (letterspaced) + gold divider + numeric date */
        char wd[16];
        strftime(wd, sizeof(wd), "%A", &tm);
        sb_str_upper(wd);
        char spaced[32];
        {
            int p = 0;
            for (int i = 0; wd[i] && p < (int)sizeof(spaced) - 3; i++) {
                spaced[p++] = wd[i];
                if (wd[i + 1]) spaced[p++] = ' ';
            }
            spaced[p] = 0;
        }
        gfx_text_center_box(0, 146, 320, spaced, UI_FONT_SMALL, SB_MUTED);
        gfx_hline(70, 168, 90, SB_GOLD);
        gfx_hline(160, 168, 90, SB_GOLD);
        gfx_rect(157, 165, 6, 6, SB_GOLD);
        char nd[20];
        strftime(nd, sizeof(nd), "%d / %m / %Y", &tm);
        gfx_text_center_box(0, 180, 320, nd, UI_FONT_SMALL, SB_MUTED);
    } else {
        /* not synced: honest placeholders, zero faked digits */
        gfx_text_medium(20, 14, "TABLE CLOCK", SB_INK);
        gfx_rect(20, 36, 200, 3, SB_TRACK);
        gfx_text_center_box(0, 70, 320, "-- : --", UI_FONT_LARGE, SB_MUTED);
        gfx_text_center_box(0, 146, 320, "- - - - - - -", UI_FONT_SMALL, SB_MUTED);
        gfx_hline(70, 168, 90, SB_GOLD);
        gfx_hline(160, 168, 90, SB_GOLD);
        gfx_rect(157, 165, 6, 6, SB_GOLD);
        gfx_text_center_box(0, 180, 320, "-- / -- / ----", UI_FONT_SMALL, SB_MUTED);
    }

    /* subtle centered PRESS START */
    gfx_rect_outline(106, 204, 108, 20, SB_MUTED);
    gfx_text_small(117, 210, "PRESS START", SB_INK);

    /* sync toast 1.5 s only */
    if (s_toast_until && (int32_t)(s_toast_until - now) > 0) {
        const char *msg = "TIME SYNCED - IST";
        int w = (int)strlen(msg) * 6 + 20;
        gfx_rect((320 - w) / 2, 128, w, 18, SB_INK);
        gfx_text_small((320 - w) / 2 + 10, 132, msg, SB_BG);
    }
    ui_mark_dirty_full();
}

/* ------------------------------ HOME ------------------------------------- */
/* dirty helper (guide 6.1): top/bottom chrome re-pushed only when the
   values they show (mode/gear/estop/pad) actually change */
static void mark_chrome(const os_ctx_t *ctx)
{
    static uint8_t p_gear = 0xFF;
    static int     p_mode = -1;
    static bool    p_es = false, p_pad = false, p_init = false;
    if (!p_init) {
        p_init = true;
        p_gear = ctx->gear; p_mode = (int)g.mode;
        p_es = g.estop;     p_pad = xbox360_dongle_connected();
        return;
    }
    if (p_gear != ctx->gear || p_mode != (int)g.mode ||
        p_es != g.estop || p_pad != xbox360_dongle_connected()) {
        ui_mark_dirty(0, 0, 320, 22);
        ui_mark_dirty(0, 220, 320, 20);
        p_gear = ctx->gear; p_mode = (int)g.mode;
        p_es = g.estop;     p_pad = xbox360_dongle_connected();
    }
}

/* PART 10.2 HOME widget dashboard: 3x3 live cards, 2 pages (RB flips). */
static const char *s_home_labels_p0[9] = {
    "DRIVE", "RADAR", "ROOF", "VOICE", "TRIP", "SETTINGS", "GAMES", "DIAG", "NOTIFY"
};
static const char *s_home_labels_p1[9] = {
    "OLED", "GRAPHS", "SCORE", "LINK", "VLOG", "-", "-", "-", "CLOCK"
};
static const ui_icon_t s_home_icons_p0[9] = {
    UI_ICON_DRIVE, UI_ICON_IMU, UI_ICON_LIGHT,
    UI_ICON_AUTO, UI_ICON_GRAPH, UI_ICON_GEAR,
    UI_ICON_GAMES, UI_ICON_DIAG, UI_ICON_WARN
};
static const ui_icon_t s_home_icons_p1[9] = {
    UI_ICON_SCREEN, UI_ICON_GRAPH, UI_ICON_SPARK,
    UI_ICON_AUTO, UI_ICON_WARN,
    UI_ICON_BACK, UI_ICON_BACK, UI_ICON_BACK, UI_ICON_IMU
};

static void home_card_value(int slot, uint8_t page, char *out, size_t n)
{
    if (!page) {
        switch (slot) {
        case 0: {
            int spd = (abs(g.cur_l) + abs(g.cur_r)) / 2;
            const char *m = (g.mode == MODE_AUTO) ? "AUT" : (g.mode == MODE_CRAWL) ? "CRL" : "MAN";
            snprintf(out, n, "%s %d%%", m, spd);
            break;
        }
        case 1: snprintf(out, n, "HDG %+.0f", (double)car_get_yaw()); break;
        case 2: snprintf(out, n, "%s", roof_light_state()->enabled ? roof_light_label() : "OFF"); break;
        case 3: snprintf(out, n, "%s", alexa_profile_name()); break;
        case 4: {
            uint32_t t = trip_time_s();
            snprintf(out, n, "%02u:%02u", (unsigned)(t / 60), (unsigned)(t % 60));
            break;
        }
        case 8: {
            notif_item_t tmp[24];
            snprintf(out, n, "%d NEW", notif_list(tmp, 24));
            break;
        }
        default: snprintf(out, n, "OPEN"); break;
        }
    } else {
        switch (slot) {
        case 2: {
            imu_adv_snap_t sn;
            imu_adv_snapshot(&sn);
            snprintf(out, n, "%u %c", (unsigned)sn.drive_score, sn.drive_grade);
            break;
        }
        case 3: snprintf(out, n, "%s", alexa_mqtt_connected() ? "MQTT UP" : (net_wifi_up() ? "WIFI" : "OFF")); break;
        case 8: snprintf(out, n, "IDLE"); break;
        default: snprintf(out, n, slot < 5 ? "OPEN" : "--"); break;
        }
    }
}

void os_scr_home(const os_ctx_t *ctx, uint32_t now)
{
    gfx_clear(UI_BG);
    ui_draw_topbar(ctx);

    static uint8_t  s_prev_sel = 0;
    static uint32_t s_chg_ms   = 0;
    if (s_prev_sel != ctx->home_sel) {
        s_prev_sel = ctx->home_sel;
        s_chg_ms   = now;
    }
    uint32_t anim = now - s_chg_ms;

    const char *const *labels = ctx->home_page ? s_home_labels_p1 : s_home_labels_p0;
    const ui_icon_t *icons = ctx->home_page ? s_home_icons_p1 : s_home_icons_p0;
    for (int i = 0; i < 9; i++) {
        int col = i % 3, row = i / 3;
        int x = 8 + col * 103, y = 34 + row * 64, w = 98, h = 52;
        bool sel = (i == (int)ctx->home_sel);
        bool dis = (ctx->home_page && i >= 5 && i != 8);   /* 8 = PARK live */
        gfx_rect(x, y, w, h, sel ? UI_SURFACE_2 : UI_SURFACE);
        gfx_rect_outline(x, y, w, h, sel ? UI_DATA : UI_BORDER);
        if (sel && anim < 150)
            gfx_rect_outline(x - 1, y - 1, w + 2, h + 2, UI_DATA);
        ui_draw_icon(x + 5, y + 5, 16, icons[i], sel ? UI_TEXT : UI_TEXT_2);
        gfx_text_small(x + 25, y + 8, labels[i], sel ? UI_TEXT : UI_TEXT_2);
        if (!dis) {
            char vb[20];
            home_card_value(i, ctx->home_page, vb, sizeof(vb));
            gfx_text_small(x + 25, y + 26, vb, UI_DATA);
        } else {
            gfx_text_small(x + 25, y + 26, "SOON", UI_MUTED);
        }
        ui_mark_dirty(x, y, w, h);
    }

    /* page dots + title */
    gfx_text_small(UI_GAP_S, UI_CONTENT_TOP + 2, ctx->home_page ? "HOME 2/2" : "HOME 1/2", UI_MUTED);
    gfx_rect(150, 27, 8, 8, ctx->home_page == 0 ? UI_DATA : UI_BORDER);
    gfx_rect(164, 27, 8, 8, ctx->home_page == 1 ? UI_DATA : UI_BORDER);
    mark_chrome(ctx);

    ui_draw_bottombar("A OPEN  B DRIVE  RB PAGE", "HOME");
}


/* ------------------------------ DRIVE HUD -------------------------------- */
/* guide 5.2: hero speed center, L/F/R distance cards left, status icons
   right, gear pills bottom center, semantic sensor colors + warnings. */
static void smooth_val(int *store, int target, int div)
{
    if (target < 0) return;                 /* keep last shown on invalid */
    *store += (target - *store) / div;
    if (abs(target - *store) <= 1) *store = target;
}

/* --- status strip (guide �5 lower strip): 6 compact chips at bottom of Drive --- */
static void draw_status_strip(const os_ctx_t *ctx, int y)
{
    struct { ui_icon_t ic; bool on; uint16_t col; const char *lbl; } chips[6] = {
        { UI_ICON_LIGHT, g.headlight, UI_ACCENT, "HEADLIGHT" },
        { UI_ICON_MIST,  g.mist,      UI_DATA,   "MIST" },
        { UI_ICON_SPARK, g.turbo,     UI_WARNING,"TURBO" },
        { UI_ICON_LIGHT, roof_light_state()->enabled, UI_DANGER, "ROOF" },
        { UI_ICON_HAZARD,g.hazard,    UI_WARNING,"HAZARD" },
        { UI_ICON_WARN, g.braking,    UI_DANGER, "PARK" },
    };
    int cw = 48, ch = 18, gap = 4, total = 6 * cw + 5 * gap;
    int sx = (320 - total) / 2;
    for (int i = 0; i < 6; i++) {
        int x = sx + i * (cw + gap);
        uint16_t bg = chips[i].on ? chips[i].col : UI_SURFACE;
        uint16_t fg = chips[i].on ? UI_TEXT : UI_MUTED;
        gfx_rect(x, y, cw, ch, bg);
        gfx_rect_outline(x, y, cw, ch, UI_BORDER);
        gfx_text_center_box(x, y + 3, cw, chips[i].lbl, UI_FONT_SMALL, fg);
    }
}

/* --- sensor view (guide �7): large L/F/R distances front-and-center --- */
static void draw_sensor_view(const os_ctx_t *ctx, int s_l, int s_f, int s_r, uint16_t obs)
{
    static const char *dn[3] = { "LEFT", "FRONT", "RIGHT" };
    int dvals[3] = { s_l, s_f, s_r };
    for (int i = 0; i < 3; i++) {
        int x = 20 + i * 100, y = 40, w = 80, h = 100;
        bool ok = dvals[i] >= 0;
        uint16_t col = !ok ? UI_MUTED : dvals[i] < 20 ? UI_DANGER : dvals[i] < (int)obs ? UI_WARNING : UI_DATA;
        ui_panel(x, y, w, h, false);
        gfx_text_center_box(x, y + 6, w, dn[i], UI_FONT_SMALL, col);
        char buf[16];
        if (ok) snprintf(buf, sizeof(buf), "%d", dvals[i]);
        else    snprintf(buf, sizeof(buf), "--");
        gfx_text_center_box(x, y + 35, w, buf, UI_FONT_LARGE, ok ? col : UI_MUTED);
        gfx_text_center_box(x, y + 70, w, "cm", UI_FONT_SMALL, UI_MUTED);
    }
}

/* --- performance view (guide �7): speed cap, gear caps, turbo, mode --- */
static void draw_performance_view(const os_ctx_t *ctx, int s_spd, uint32_t now)
{
    char buf[32];
    int x = 20, y = 36, w = 280;
    ui_panel(x, y, w, 50, false);
    gfx_text_small(x + 10, y + 6, "SPEED CAP", UI_MUTED);
    snprintf(buf, sizeof(buf), "%d%%", car_speed_cap_pct());
    gfx_text_right(x + w - 10, y + 6, buf, UI_DATA, UI_SURFACE, 2);
    ui_progress(x + 10, y + 28, w - 20, 12, car_speed_cap_pct(), 100, UI_DATA);
    ui_panel(x, y + 56, w, 50, false);
    gfx_text_small(x + 10, y + 62, "GEAR CAPS", UI_MUTED);
    for (int i = 0; i < 5; i++) {
        snprintf(buf, sizeof(buf), "G%d:%d", i+1, ctx->gear_caps[i]);
        gfx_text_small(x + 10 + i * 52, y + 82, buf, ctx->gear == i ? UI_ACCENT : UI_MUTED);
    }
    ui_panel(x, y + 112, w, 40, false);
    gfx_text_small(x + 10, y + 118, "MODE", UI_MUTED);
    gfx_text_right(x + w - 10, y + 118, mode_name(g.mode), UI_TEXT, UI_SURFACE, 2);
    gfx_text_small(x + 10, y + 134, g.cooldown ? "TURBO COOLDOWN" : "TURBO READY", g.cooldown ? UI_WARNING : UI_OK);

}

/* (drive warnings moved to event-lifecycle alerts - see car_os.c alert_watch) */

/* --- quick overlay (guide 5.9): START on Drive. Sits above the HUD,
       below the alert panel (alerts must never be hidden). --- */

/* ambient static color (defined in car.c) - used by quick menu + picker */
extern uint8_t g_static_r, g_static_g, g_static_b;

/* PART 10.4 Quick Control Center (START hold, bottom sheet):
   4 chips HEAD/HAZARD/ROOF/MUTE + BRIGHT/VOLUME sliders + THEME + PROFILE. */
static void draw_quick_overlay(const os_ctx_t *ctx)
{
    int x = 0, w = 320, h = 170, y = 220 - h;
    gfx_rect(x, y, w, h, UI_SURFACE);
    gfx_hline(x, y, w, UI_ACCENT);
    gfx_rect_outline(x, y, w, h, UI_ACCENT);
    gfx_text_center_box(x, y + 6, w, "QUICK", UI_FONT_MEDIUM, UI_ACCENT);

    /* row 0: 4 chips */
    static const char *chips[4] = { "HEAD", "HAZ", "ROOF", "MUTE" };
    {
        bool foc = (ctx->quick_row == 0);
        int cw = 68, gap = 8, total = 4 * cw + 3 * gap;
        int sx = x + (w - total) / 2, cy = y + 26;
        for (int i = 0; i < 4; i++) {
            bool on = (i == 0) ? g.headlight : (i == 1) ? g.hazard :
                      (i == 2) ? roof_light_state()->enabled : g.snd_mute;
            bool sel = foc && (int)ctx->quick_col == i;
            int ix = sx + i * (cw + gap);
            gfx_rect(ix, cy, cw, 26, on ? UI_ACCENT : UI_SURFACE_2);
            gfx_rect_outline(ix, cy, cw, 26, sel ? UI_TEXT : (on ? UI_ACCENT : UI_BORDER));
            gfx_text_center_box(ix, cy + 6, cw, chips[i], UI_FONT_MEDIUM,
                                on ? RGB565(20, 12, 0) : UI_MUTED);
        }
    }
    /* rows 1..4: sliders / selectors */
    static const char *rnames[4] = { "BRIGHT", "VOLUME", "THEME", "PROFILE" };
    for (int r = 1; r <= 4; r++) {
        int ry = y + 56 + (r - 1) * 22;
        bool foc = ((int)ctx->quick_row == r);
        if (foc) { gfx_rect(x + 4, ry, w - 8, 20, UI_SURFACE_2); }
        gfx_text_small(x + 12, ry + 6, rnames[r - 1], foc ? UI_TEXT : UI_MUTED);
        char vb[24];
        uint16_t vc = foc ? UI_ACCENT : UI_MUTED;
        uint16_t bg = foc ? UI_SURFACE_2 : UI_SURFACE;
        if (r == 1) {
            uint8_t v = car_get_setting(3);
            snprintf(vb, sizeof(vb), "%u%%", (unsigned)v);
            ui_progress(x + 108, ry + 4, 100, 12, v, 100, UI_DATA);
        } else if (r == 2) {
            uint8_t v = car_get_setting(1);
            snprintf(vb, sizeof(vb), "%u%%", (unsigned)v);
            ui_progress(x + 108, ry + 4, 100, 12, v, 100, UI_DATA);
        } else if (r == 3) {
            snprintf(vb, sizeof(vb), "%s", ui_cockpit_theme_name(ctx->cockpit_theme));
        } else {
            snprintf(vb, sizeof(vb), "%s", alexa_profile_name());
        }
        gfx_text_right(x + w - 12, ry + 3, vb, vc, bg, 2);
    }
    /* row 5: exit */
    {
        bool foc = (ctx->quick_row == 5);
        int ry = y + 144;
        if (foc) { gfx_rect(x + 4, ry, w - 8, 20, UI_SURFACE_2); }
        gfx_text_center_box(x, ry + 3, w, "EXIT TO HOME (A)",
                            UI_FONT_MEDIUM, foc ? UI_TEXT : UI_MUTED);
    }
}

/* --- roof light quick panel (premium guide �6.3): B hold. --- */
/* PART 7 roof bottom sheet: MODE / COLOR (steady only) / BRIGHTNESS */
static void draw_roof_panel(const os_ctx_t *ctx)
{
    int x = 0, w = 320, h = 118, y = 220 - h;
    gfx_rect(x, y, w, h, UI_SURFACE);
    gfx_hline(x, y, w, UI_ACCENT);
    gfx_rect_outline(x, y, w, h, UI_ACCENT);
    gfx_text_center_box(x, y + 5, w, "ROOF LIGHT", UI_FONT_SMALL, UI_ACCENT);

    const roof_light_state_t *rl = roof_light_state();
    char vb[24];
    /* row 0: MODE */
    bool foc = (ctx->roof_row == 0);
    if (foc) { gfx_rect(x + 4, y + 20, w - 8, 20, UI_SURFACE_2); }
    gfx_text_small(x + 12, y + 26, "MODE", foc ? UI_TEXT : UI_MUTED);
    gfx_text_right(x + w - 12, y + 26, roof_light_label(),
                   foc ? UI_ACCENT : UI_MUTED, foc ? UI_SURFACE_2 : UI_SURFACE, 1);
    /* row 1: COLOR dots (steady only, dimmed otherwise) */
    foc = (ctx->roof_row == 1);
    bool steady = (rl->mode == ROOF_LIGHT_STEADY);
    if (foc) { gfx_rect(x + 4, y + 42, w - 8, 26, UI_SURFACE_2); }
    gfx_text_small(x + 12, y + 50, "COLOR", (foc && steady) ? UI_TEXT : UI_MUTED);
    {
        int sw = 18, gap = 8, total = ROOF_COLOR_COUNT * sw + (ROOF_COLOR_COUNT - 1) * gap;
        int sx = x + w - 12 - total;
        for (int i = 0; i < ROOF_COLOR_COUNT; i++) {
            uint8_t cr, cg, cb;
            roof_light_color_rgb((uint8_t)i, &cr, &cg, &cb);
            int ix = sx + i * (sw + gap);
            bool sel = (i == (int)rl->color_idx);
            gfx_rect(ix, y + 46, sw, 18, RGB565(cr, cg, cb));
            if (sel && steady)
                gfx_rect_outline(ix - 2, y + 44, sw + 4, 22, UI_ACCENT);
            else
                gfx_rect_outline(ix - 1, y + 45, sw + 2, 20, UI_BORDER);
        }
        if (!steady)
            gfx_text_small(sx, y + 50, "STEADY ONLY", UI_MUTED);
    }
    /* row 2: BRIGHTNESS */
    foc = (ctx->roof_row == 2);
    if (foc) { gfx_rect(x + 4, y + 70, w - 8, 20, UI_SURFACE_2); }
    gfx_text_small(x + 12, y + 76, "BRIGHT", foc ? UI_TEXT : UI_MUTED);
    snprintf(vb, sizeof(vb), "%u%%", (unsigned)rl->brightness);
    ui_progress(x + 90, y + 76, 130, 10, rl->brightness, 100, UI_DANGER);
    gfx_text_right(x + w - 12, y + 76, vb, foc ? UI_ACCENT : UI_MUTED,
                   foc ? UI_SURFACE_2 : UI_SURFACE, 1);

    gfx_hline(x, y + 94, w, UI_BORDER);
    gfx_text_center_box(x, y + 98, w, "A APPLY    B CANCEL    START KEEP",
                        UI_FONT_SMALL, UI_MUTED);
}

/* --- AUTO drive-mode preview overlay (premium guide �13): X tap. --- */
static void draw_auto_preview(const os_ctx_t *ctx)
{
    int x = 44, y = 34, w = 232, h = 132;
    gfx_rect(x, y, w, h, UI_SURFACE);
    gfx_rect_outline(x, y, w, h, UI_OK);
    gfx_text_center_box(x, y + 6, w, "AUTO MODE", UI_FONT_SMALL, UI_OK);
    gfx_hline(x, y + 18, w, UI_BORDER);

    bool ok_es   = !g.estop;
    bool ok_sens = car_us_front_ok();
    bool ok_stop = !(g.cur_l || g.cur_r || g.tgt_l || g.tgt_r);
    bool ok_ntr  = true;
    const xbox360_pad_t *p0 = xbox360_pad(0);
    if (p0 && p0->present) {
        ok_ntr = !(abs(p0->lx) > 4000 || abs(p0->ly) > 4000 ||
                   abs(p0->rx) > 4000 || abs(p0->ry) > 4000 ||
                   p0->rt > 5 || p0->lt > 5);
    }
    bool ready = ok_es && ok_sens && ok_stop && ok_ntr;

    static const char *items[4] = { "E-STOP CLEAR", "SENSORS OK", "STATIONARY", "STICKS NEUTRAL" };
    bool oks[4] = { ok_es, ok_sens, ok_stop, ok_ntr };
    for (int i = 0; i < 4; i++) {
        ui_status_icon(x + 24, y + 26 + i * 22,
                       oks[i] ? UI_ICON_OK : UI_ICON_WARN, oks[i], UI_OK);
        gfx_text_small(x + 48, y + 31 + i * 22, items[i],
                       oks[i] ? UI_TEXT : UI_MUTED);
    }

    gfx_hline(x, y + 116, w, UI_BORDER);
    gfx_text_center_box(x, y + 120, w,
                        ready ? "HOLD X TO ENABLE" : "BLOCKED - FIX ITEMS",
                        UI_FONT_MEDIUM, ready ? UI_OK : UI_DANGER);
}

/* --- drive HUD toast (premium guide �9): 1.2 s, above the HUD. --- */
static void draw_toast(const os_ctx_t *ctx)
{
    if (!ctx->toast[0]) return;
    int tw = 260, tx = (UI_W - tw) / 2, ty = 150, th = 24;
    gfx_rect(tx, ty, tw, th, UI_SURFACE_2);
    gfx_rect_outline(tx, ty, tw, th, UI_ACCENT);
    gfx_text_center_box(tx, ty + 5, tw, ctx->toast, UI_FONT_MEDIUM, UI_TEXT);
}

/* --- ambient color picker overlay (premium guide 5.2): Y hold on Drive. --- */
extern uint8_t g_static_r, g_static_g, g_static_b;   /* defined in car.c */

static void draw_ambient_picker(const os_ctx_t *ctx)
{
    int x = 60, y = 64, w = 200, h = 92;
    gfx_rect(x, y, w, h, UI_SURFACE);
    gfx_rect_outline(x, y, w, h, UI_ACCENT);
    gfx_text_center_box(x, y + 6, w, "AMBIENT COLOR", UI_FONT_SMALL, UI_ACCENT);
    gfx_hline(x, y + 19, w, UI_BORDER);

    /* live-preview swatch: currently applied color */
    gfx_rect_outline(x + 12, y + 28, 26, 26, UI_BORDER);
    gfx_rect(x + 14, y + 30, 22, 22, RGB565(g_static_r, g_static_g, g_static_b));
    gfx_text_small(x + 46, y + 38, "PREVIEW", UI_MUTED);

    /* 6 selectable swatches */
    int sw = 20, gap = 6, total = 6 * sw + 5 * gap;
    int sx = x + (w - total) / 2;
    for (int i = 0; i < 6; i++) {
        int ix = sx + i * (sw + gap);
        gfx_rect(ix, y + 62, sw, sw, RGB565(os_ambient_palette[i][0],
                                            os_ambient_palette[i][1],
                                            os_ambient_palette[i][2]));
        if (i == (int)ctx->picker_sel) {
            gfx_rect_outline(ix - 2, y + 60, sw + 4, sw + 4, UI_ACCENT);
            gfx_rect_outline(ix - 3, y + 59, sw + 6, sw + 6, UI_ACCENT);
        }
    }
    gfx_text_center_box(x, y + h - 14, w,
                        os_ambient_name(ctx->picker_sel), UI_FONT_SMALL, UI_TEXT);
    gfx_text_center_box(x, y + h - 2, w, "L/R SELECT  A APPLY  B CANCEL",
                        UI_FONT_SMALL, UI_MUTED);
}

void os_scr_drive(const os_ctx_t *ctx, uint32_t now)
{
    /* ---- Motion Radar (default view): full-frame, self-contained ---- */
    if (ctx->drive_sub_view == 0) {
        gfx_clear(UI_R_BG);
        ui_motion_radar_draw(now, (ctx->radar_view == 1));
        ui_mark_dirty_full();
        return;
    }

    gfx_clear(UI_BG);
    ui_draw_topbar(ctx);

    /* display-only smoothing (guide 5.2): real control values untouched */
    int t_spd = (abs(g.cur_l) + abs(g.cur_r)) / 2;
    static int s_spd = 0, s_l = -1, s_f = -1, s_r = -1;
    smooth_val(&s_spd, t_spd, 4);
    int fl = (g.dist_avg[0] == 65535) ? -1 : (int)g.dist_avg[0];
    int ff = (g.dist_avg[1] == 65535) ? -1 : (int)g.dist_avg[1];
    int fr = (g.dist_avg[2] == 65535) ? -1 : (int)g.dist_avg[2];
    smooth_val(&s_l, fl, 3); smooth_val(&s_f, ff, 3); smooth_val(&s_r, fr, 3);

    uint16_t obs = car_get_setting(2);
    char buf[16];

    /* --- gear flash (guide �10): highlight on gear change --- */
    if (ctx->gear_flash && now < ctx->gear_flash_until) {
        gfx_rect(120, 30, 80, 20, UI_ACCENT);
        gfx_text_center_box(120, 32, 80, "GEAR", UI_FONT_SMALL, UI_TEXT);
    }

    /* --- AUTO blocked reason (guide �9): show exact reason --- */
    if (g.mode == MODE_AUTO) {
        const char *reason = NULL;
        if (g.estop) reason = "AUTO BLOCKED - ESTOP";
        else if (!car_us_front_ok()) reason = "AUTO BLOCKED - SENSOR";
        else if (g.cur_l || g.cur_r || g.tgt_l || g.tgt_r) reason = "AUTO BLOCKED - MOVING";
        else { const xbox360_pad_t *p0 = xbox360_pad(0); if (!p0 || !p0->present) reason = "AUTO BLOCKED - PAD"; }
        if (reason) { gfx_text_center_box(0, 24, 320, reason, UI_FONT_SMALL, UI_WARNING); ui_mark_dirty(0, 22, 320, 14); }
    }

    /* --- sub-view switch (guide �7): Sensor / Performance --- */
    switch (ctx->drive_sub_view) {
    case 1: draw_sensor_view(ctx, s_l, s_f, s_r, obs); break;
    case 2: draw_performance_view(ctx, s_spd, now); break;
    default: break;  /* premium HUD (sub_view 3) below */
    }
    if (ctx->drive_sub_view == 1 || ctx->drive_sub_view == 2) {
        draw_status_strip(ctx, 170);
        ui_mark_dirty(0, 168, 320, 22);
    } else {

    /* --- left: three distance cards L/F/R (guide 5.2); hidden in minimal --- */
    if (ctx->hud_layout < 2) {
    static const char *dn[3] = { "L", "F", "R" };
    int dvals[3] = { s_l, s_f, s_r };
    for (int i = 0; i < 3; i++) {
        int x = 8, y = 32 + i * 46, w = 108, h = 40;
        bool ok = dvals[i] >= 0;
        uint16_t col = !ok ? UI_MUTED :
                       dvals[i] < 20 ? UI_DANGER :
                       dvals[i] < (int)obs ? UI_WARNING : UI_DATA;
        ui_panel(x, y, w, h, false);
        gfx_text_small(x + 8, y + 6, dn[i], col);
        if (ok) { snprintf(buf, sizeof(buf), "%d", dvals[i]); }
        else    { snprintf(buf, sizeof(buf), "--"); }
        gfx_text_center_box(x, y + 18, w, buf, UI_FONT_MEDIUM,
                            ok ? col : UI_MUTED);
        gfx_text_small(x + w - 22, y + 24, "cm", UI_MUTED);
    }
    }

    /* --- center: hero speed (88x66) + POWER label (guide 5.2) --- */
    int hx = 124, hy = 56, hw = 88, hh = 66;
    ui_panel(hx, hy, hw, hh, false);
    uint16_t scol = s_spd < 30 ? UI_OK : s_spd < 70 ? UI_WARNING : UI_DANGER;
    gfx_number_7seg(hx + (hw - 3 * 4 * 3) / 2, hy + 12,
                    s_spd, 3, 3, scol, UI_SURFACE_2);
    gfx_text_center_box(hx, hy + hh - 22, hw, "POWER", UI_FONT_SMALL, UI_MUTED);
    if (g.mode == MODE_AUTO) {
        gfx_text_center_box(hx, 20, hw, "AUTO", UI_FONT_SMALL, UI_OK);
    }

    /* --- right: 2x3 status icons (hidden in minimal HUD) --- */
    if (ctx->hud_layout < 2) {
    const struct { ui_icon_t ic; bool on; uint16_t col; } sis[6] = {
        { UI_ICON_LIGHT, g.headlight, UI_ACCENT },
        { UI_ICON_HAZARD, g.hazard,   UI_WARNING },
        { UI_ICON_MIST,  g.mist,      UI_DATA },
        { UI_ICON_LIGHT, roof_light_state()->enabled, UI_DANGER },
        { UI_ICON_AUTO,  (g.mode == MODE_AUTO), UI_OK },
        { UI_ICON_IMU,   motion_radar_imu_is_valid(), UI_OK },  /* live IMU link */
    };
    static const char *slbl[6] = { "LGT", "HAZ", "MIST", "ROOF", "AUTO", "IMU" };
    static const int xc[2] = { 232, 280 };
    static const int yc[3] = { 40, 84, 128 };
    for (int i = 0; i < 6; i++) {
        int x = xc[i & 1], y = yc[i >> 1];
        ui_status_icon(x, y, sis[i].ic, sis[i].on, sis[i].col);
        gfx_text_center_box(x, y + 24, 32, slbl[i], UI_FONT_SMALL,
                            sis[i].on ? UI_TEXT : UI_MUTED);
    }
    }
    /* throttle bar under status */
    int rt = 0;
    const xbox360_pad_t *p0 = xbox360_pad(0);
    if (p0 && p0->present) rt = p0->rt * 100 / 255;
    ui_progress(232, 158, 72, 10, rt, 100, UI_DATA);
    gfx_text_center_box(232, 170, 72, "RT", UI_FONT_SMALL, UI_MUTED);
    snprintf(buf, sizeof(buf), "CAP %d%%", car_speed_cap_pct());
    gfx_text_center_box(232, 182, 72, buf, UI_FONT_SMALL, UI_WARNING);

    /* --- bottom center: five gear pills G1-G5 (hidden in compact/minimal) --- */
    if (ctx->hud_layout < 1) {
    int pw = 42, gap = 6, total = CAR_GEAR_COUNT * pw + (CAR_GEAR_COUNT - 1) * gap;
    int px = (UI_W - total) / 2;
    for (int i = 0; i < CAR_GEAR_COUNT; i++) {
        char gb[4];
        snprintf(gb, sizeof(gb), "G%d", i + 1);
        ui_pill(px + i * (pw + gap), 196, pw, gb, i == (int)ctx->gear);
    }
    }
    /* --- status strip (guide �5 lower strip) --- */
    if (ctx->drive_sub_view == 0) { draw_status_strip(ctx, 170); ui_mark_dirty(0, 168, 320, 22); }

    /* --- steering direction indicator (guide 9 center zone) --- */
    {
        int st_diff = (int)g.tgt_l - (int)g.tgt_r;   /* + => turning right */
        const char *sdir = NULL;
        uint16_t dcol = UI_TEXT;
        if (st_diff > 8)       { sdir = ">>>";  dcol = UI_WARNING; }
        else if (st_diff < -8) { sdir = "<<<";  dcol = UI_WARNING; }
        else if (st_diff > 2)  { sdir = ">"; }
        else if (st_diff < -2) { sdir = "<"; }
        if (sdir) {
            gfx_text_center_box(124, 128, 88, sdir, UI_FONT_SMALL, dcol);
            ui_mark_dirty(124, 126, 88, 14);
        }
    }

    /* --- turbo cooldown indication (guide 6.4 priority chain) --- */
    if (g.cooldown && g.cooldown_until > now) {
        uint32_t cd_left = (g.cooldown_until - now) / 1000 + 1;
        char cdb[24];
        snprintf(cdb, sizeof(cdb), "TURBO COOLDOWN %lus", (unsigned long)cd_left);
        gfx_text_center_box(0, 24, UI_W, cdb, UI_FONT_SMALL, UI_WARNING);
        ui_mark_dirty(0, 22, 320, 14);
    }

    /* --- neutral-release lock badge (premium guide 2.6/12) --- */
    {
        static bool     s_was_locked = false;
        static uint32_t s_unlocked_at = 0;
        bool locked = car_drive_locked();
        if (!locked && s_was_locked) s_unlocked_at = now;
        s_was_locked = locked;
        if (locked) {
            gfx_text_center_box(0, 36, UI_W, "LOCKED - RELEASE STICKS",
                                UI_FONT_SMALL, UI_WARNING);
            ui_mark_dirty(0, 34, 320, 16);
        } else if (s_unlocked_at && now - s_unlocked_at < 1500) {
            gfx_text_center_box(0, 36, UI_W, "DRIVE READY",
                                UI_FONT_SMALL, UI_OK);
            ui_mark_dirty(0, 34, 320, 16);
        }
    }

    /* --- lighting override message (premium guide 6.4): hazard/indicators
       own the shared outputs; the HUD explains instead of looking broken --- */
    if (roof_light_state()->enabled && (g.hazard || g.sig_l || g.sig_r)) {
        gfx_text_center_box(0, 178, UI_W, "ROOF LIGHT OVERRIDDEN BY HAZARD",
                            UI_FONT_SMALL, UI_MUTED);
        ui_mark_dirty(0, 176, 320, 14);
    }

    /* --- drive alert overlay (guide 5.2): event lifecycle, pulse border,
       never flash the whole screen. Rendered LAST so it sits above anything. */
    /* --- dirty regions (guide 6.1): only dynamic areas re-pushed --- */
    ui_mark_dirty(8, 32, 108, 134);          /* L/F/R distance cards */
    ui_mark_dirty(124, 20, 88, 102);         /* AUTO tag + hero speed */
    ui_mark_dirty(228, 34, 86, 158);         /* status icons + RT + CAP */
    ui_mark_dirty(43, 196, 234, 20);         /* gear pills */
    if (ctx->quick_open) ui_mark_dirty(0, 50, 320, 170);
    if (ctx->roof_panel) ui_mark_dirty(56, 40, 208, 120);
    if (ctx->auto_prev)  ui_mark_dirty(44, 34, 232, 132);
    if (ctx->picker_open) ui_mark_dirty(60, 64, 200, 92);
    if (ctx->toast_until > now || ctx->toast[0]) ui_mark_dirty(30, 150, 260, 24);
    if (ctx->alert_active) ui_mark_dirty(56, 102, 208, 44);
    mark_chrome(ctx);

    /* --- overlays: quick settings / roof panel / picker / AUTO preview --- */
    if (ctx->quick_open) draw_quick_overlay(ctx);
    if (ctx->roof_panel) draw_roof_panel(ctx);
    if (ctx->picker_open) draw_ambient_picker(ctx);
    if (ctx->auto_prev)  draw_auto_preview(ctx);
    if (ctx->toast_until > now) draw_toast(ctx);

    if (ctx->alert_active) {
        bool pulse = ((now / 600) & 1) != 0;
        int wx = 56, wy = 102, ww = 208, wh = 44;
        gfx_rect(wx, wy, ww, wh, UI_SURFACE_2);
        gfx_rect_outline(wx, wy, ww, wh, pulse ? UI_DANGER : UI_SURFACE_3);
        ui_draw_icon(wx + 18, wy + 14, 16, UI_ICON_WARN, UI_DANGER);
        {   /* 164px box fits 13 MEDIUM chars — truncate, never spill */
            char am[16];
            snprintf(am, sizeof(am), "%.13s", ctx->alert_message);
            gfx_text_center_box(wx + 30, wy + 15, ww - 44, am,
                                UI_FONT_MEDIUM, UI_TEXT);
        }
    }

    /* --- bottom bar: context-aware with sub-view cycling (guide �7) --- */
    {
        const char *sub_names[4] = { "RADAR", "SENSOR", "PERFORMANCE", "NIGHT" };
        char hint[48];
        snprintf(hint, sizeof(hint), "BACK QUICK  START HOME");
        ui_draw_bottombar(hint, sub_names[ctx->drive_sub_view]);
    }
    }
}
/* ---- Drive overlays above the cockpit (1.md PART 3): quick settings /
   roof panel / ambient picker / AUTO preview. Alerts + toast are drawn
   by ui_cockpit_draw itself (dims cockpit 40% behind alerts). --- */
void os_scr_drive_overlays(const os_ctx_t *ctx)
{
    if (ctx->quick_open) {
        draw_quick_overlay(ctx);
        ui_mark_dirty(0, 62, 320, 158);   /* PART 10 QCC bottom sheet */
    }
    if (ctx->roof_panel) {
        draw_roof_panel(ctx);
        ui_mark_dirty(0, 102, 320, 118);   /* PART 7 bottom sheet */
    }
    if (ctx->picker_open) {
        draw_ambient_picker(ctx);
        ui_mark_dirty(60, 64, 200, 92);
    }
    if (ctx->auto_prev) {
        draw_auto_preview(ctx);
        ui_mark_dirty(44, 34, 232, 132);
    }
}

/* PART 10.1 L5 SAFETY OVERLAY (always top): alert card above everything. */
void os_scr_safety_overlay(const os_ctx_t *ctx, uint32_t now)
{
    if (!ctx->alert_active) return;
    ui_cockpit_dim_rect(0, 0, 320, 220);
    bool pulse = ((now / 600) & 1) != 0;
    int wx = 56, wy = 102, ww = 208, wh = 44;
    gfx_rect(wx, wy, ww, wh, UI_SURFACE_2);
    gfx_rect_outline(wx, wy, ww, wh, pulse ? UI_DANGER : UI_SURFACE_3);
    /* box fits 17 MEDIUM chars — truncate, never spill into widgets */
    {
        char am[20];
        snprintf(am, sizeof(am), "%.17s", ctx->alert_message);
        gfx_text_center_box(wx, wy + 15, ww, am, UI_FONT_MEDIUM, UI_TEXT);
    }
    ui_mark_dirty(wx, wy, ww, wh);
}

static void plot_card(int x, int y, int w, int h, const int16_t *hist,
                      const char *label, int lo, int hi, uint16_t col,
                      const char *cur)
{
    ui_panel(x, y, w, h, false);
    gfx_text_small(x + 8, y + 6, label, col);
    gfx_text_center_box(x + w - 70, y + 3, 66, cur, UI_FONT_MEDIUM, UI_TEXT);

    int gx0 = x + 4, gy0 = y + 16, gw = w - 8, gh = h - 22;
    if (gw <= 1 || gh <= 4) return;
    /* 1px grid */
    for (int gy = gy0 + 4; gy < y + h - 3; gy += 8) gfx_hline(gx0, gy, gw, UI_BORDER);
    for (int gx = gx0 + 22; gx < x + w - 4; gx += 22) gfx_vline(gx, gy0, gh, UI_BORDER);
    if (!hist) return;

    uint16_t pos = os_hist_pos();
    uint16_t hn  = os_hist_n();
    uint16_t start = (uint16_t)((pos + hn - (uint16_t)(gw % hn)) % hn);
    int prev_py = -1;
    for (int i = 0; i < gw; i++) {
        int idx = (start + i) % hn;
        int v = hist[idx];
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        int py = gy0 + gh - 2 - (v - lo) * (gh - 6) / (hi - lo);
        if (py < gy0) py = gy0;
        if (prev_py >= 0) {
            int a = prev_py < py ? prev_py : py;
            int b = prev_py < py ? py : prev_py;
            gfx_vline(gx0 + 1 + i, a, b - a + 1, col);
        } else {
            gfx_px(gx0 + 1 + i, py, col);
        }
        prev_py = py;
    }
}

void os_scr_analytics(const os_ctx_t *ctx, uint32_t now)
{
    (void)now;
    gfx_clear(UI_BG);
    ui_draw_topbar(ctx);

    int spd = (abs(g.cur_l) + abs(g.cur_r)) / 2;
    uint16_t f = g.dist_avg[1];
    char buf[26];

    snprintf(buf, sizeof(buf), "%d%%", spd);
    plot_card(8, 32, 304, 52, os_h_spd, "POWER", 0, 100, UI_DATA, buf);

    if (f == 65535) snprintf(buf, sizeof(buf), "--");
    else snprintf(buf, sizeof(buf), "%ucm", (unsigned)f);
    plot_card(8, 92, 304, 52, os_h_front, "FRONT DIST", 0, 200, UI_DATA, buf);

    snprintf(buf, sizeof(buf), "%.0f", car_get_yaw());
    plot_card(8, 152, 304, 52, os_h_yaw, "YAW", -180, 180, UI_DATA, buf);

    /* bottom info strip */
    char info[48];
    snprintf(info, sizeof(info), "MODE %s   GEAR %u   HIST %u@10Hz",
             mode_name(g.mode), (unsigned)(ctx->gear + 1),
             (unsigned)ctx->analytics_hist_n);
    gfx_text_small(10, 216, info, UI_MUTED);

    scr_footer(ctx);
}

/* ------------------------------ DIAG ------------------------------------- */
/* guide 5.5: tabs instead of a single text wall. L/R switches tab, A/B back. */
#define DIAG_TABS 5
static const char *s_diag_tabs[DIAG_TABS] = { "SYSTEM", "SENSORS", "CONTROL", "DISPLAY", "THEME" };

void os_scr_diag(const os_ctx_t *ctx, uint32_t now)
{
    gfx_clear(UI_BG);
    ui_draw_topbar(ctx);

    int tab = (int)ctx->diag_tab % DIAG_TABS;

    /* tab bar */
    int tw = UI_W / DIAG_TABS;
    for (int i = 0; i < DIAG_TABS; i++) {
        int tx = i * tw;
        bool sel = (i == tab);
        gfx_rect(tx, 30, tw, 22, sel ? UI_SURFACE_2 : UI_SURFACE);
        gfx_rect_outline(tx, 30, tw, 22, sel ? UI_ACCENT : UI_BORDER);
        gfx_text_center_box(tx, 36, tw, s_diag_tabs[i], UI_FONT_SMALL,
                            sel ? UI_TEXT : UI_MUTED);
    }

    char buf[48];
    int y = 66, x = UI_GAP_S;

    switch (tab) {
    case 0: { /* SYSTEM */
        size_t fp = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t fi = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t mi = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        snprintf(buf, sizeof(buf), "UPTIME          %um %02us", (unsigned)(up / 60), (unsigned)(up % 60));
        gfx_text_medium(x, y, buf, UI_TEXT); y += 24;
        /* perf counters (guide 9) */
        const os_perf_t *pf = os_perf();
        snprintf(buf, sizeof(buf), "TFT %u FPS   FRAME %luus   PUSH %luus",
                 (unsigned)pf->fps, (unsigned long)pf->frame_us,
                 (unsigned long)pf->push_us);
        gfx_text_small(x, y + 2, buf, UI_TEXT); y += 15;
        snprintf(buf, sizeof(buf), "PEAK %luus   SKIPPED %lu",
                 (unsigned long)pf->max_frame_us, (unsigned long)pf->skipped_frames);
        gfx_text_small(x, y + 2, buf, UI_MUTED); y += 15;
        snprintf(buf, sizeof(buf), "INT %uK (MIN %uK)   PSRAM %uK",
                 (unsigned)(fi / 1024), (unsigned)(mi / 1024), (unsigned)(fp / 1024));
        gfx_text_small(x, y + 2, buf,
                       fi > 49152 && fp > 1048576 ? UI_OK : UI_WARNING);
        y += 18;
        /* heap trend: free must not shrink over repeated panel open/close */
        gfx_text_small(x, y + 2, "ESP32-S3  16MB FLASH  8MB PSRAM", UI_MUTED); y += 16;

        /* dangerous actions (premium guide 4/11): A hold 1.5 s each */
        {
            const char *items[2] = { "RESTART", "FACTORY RESET" };
            for (int i = 0; i < 2; i++) {
                int ix = x + i * 160, iy = y;
                bool foc = (ctx->diag_item == i + 1);
                uint16_t bcol = foc ? UI_ACCENT : UI_BORDER;
                gfx_rect(ix, iy, 148, 18, foc ? UI_SURFACE_2 : UI_SURFACE);
                gfx_rect_outline(ix, iy, 148, 18, bcol);
                gfx_text_center_box(ix, iy + 4, 148, items[i],
                                    UI_FONT_SMALL, foc ? UI_TEXT : UI_MUTED);
                if (foc && ctx->sys_hold_t0) {      /* hold-A progress fill */
                    uint32_t el = now - ctx->sys_hold_t0;
                    int pct = (int)(el * 100 / 1500);
                    if (pct > 100) pct = 100;
                    if (pct > 0) {
                        gfx_rect(ix + 1, iy + 1, 146 * pct / 100, 16, UI_DANGER);
                        gfx_text_center_box(ix, iy + 4, 148, items[i],
                                            UI_FONT_SMALL, UI_TEXT);
                    }
                }
            }
            gfx_text_small(x, y + 24, "HOLD A 1.5s TO RUN   B CANCEL", UI_MUTED);
        }
        break;
    }
    case 1: { /* SENSORS (compact rows: 8 lines must fit 60..216) */
        snprintf(buf, sizeof(buf), "RAW  L %4u  F %4u  R %4u", g.dist[0], g.dist[1], g.dist[2]);
        gfx_text_medium(x, y, buf, UI_TEXT); y += 22;
        snprintf(buf, sizeof(buf), "AVG  L %4u  F %4u  R %4u", g.dist_avg[0], g.dist_avg[1], g.dist_avg[2]);
        gfx_text_medium(x, y, buf, UI_TEXT); y += 22;
        snprintf(buf, sizeof(buf), "FRONT SENSOR   %s", car_us_front_ok() ? "OK" : "FAULT");
        gfx_text_medium(x, y, buf, car_us_front_ok() ? UI_OK : UI_DANGER); y += 22;
        snprintf(buf, sizeof(buf), "YAW            %.1f deg", car_get_yaw());
        gfx_text_medium(x, y, buf, UI_TEXT); y += 22;
        snprintf(buf, sizeof(buf), "OBST THRESH    %u cm", car_get_setting(2));
        gfx_text_medium(x, y, buf, UI_TEXT); y += 22;
        /* PART 10.8: controller battery in SENS tab */
        {
            int batt = ui_cockpit_battery_pct();
            if (batt < 0) snprintf(buf, sizeof(buf), "PAD BAT        --");
            else snprintf(buf, sizeof(buf), "PAD BAT        %d%%", batt);
            gfx_text_medium(x, y, buf, batt < 0 ? UI_MUTED : UI_TEXT); y += 22;
        }
        /* PART 8 M1: live mic VU meter (RMS sampled every 50ms) */
        {
            uint16_t vu = mic_ready() ? mic_vu() : 0;
            snprintf(buf, sizeof(buf), "MIC VU %3u%%", (unsigned)vu);
            gfx_text_medium(x, y, buf, mic_ready() ? UI_TEXT : UI_MUTED);
            ui_progress(x + 170, y + 4, 120, 10, vu, 100,
                        vu > 80 ? UI_DANGER : vu > 50 ? UI_WARNING : UI_OK);
            y += 22;
        }
        break;
    }
    case 2: { /* CONTROL */
        snprintf(buf, sizeof(buf), "MOTOR CUR  L%3d  R%3d", g.cur_l, g.cur_r);
        gfx_text_medium(x, y, buf, UI_TEXT); y += 26;
        snprintf(buf, sizeof(buf), "MOTOR TGT  L%3d  R%3d", g.tgt_l, g.tgt_r);
        gfx_text_medium(x, y, buf, UI_TEXT); y += 26;
        snprintf(buf, sizeof(buf), "MODE           %s", mode_name(g.mode));
        gfx_text_medium(x, y, buf, g.mode == MODE_AUTO ? UI_DATA : UI_TEXT); y += 26;
        snprintf(buf, sizeof(buf), "AUTO STATE     %s", ast_name_ui(g.ast));
        gfx_text_medium(x, y, buf, UI_TEXT); y += 26;
        snprintf(buf, sizeof(buf), "GEAR %u  CAP %u%%", (unsigned)(g.gear + 1), car_speed_cap_pct());
        gfx_text_medium(x, y, buf, UI_TEXT); y += 26;
        snprintf(buf, sizeof(buf), "DONGLE         %s", xbox360_dongle_connected() ? "CONNECTED" : "NO PAD");
        gfx_text_medium(x, y, buf, xbox360_dongle_connected() ? UI_OK : UI_DANGER);
        break;
    }
    default: { /* DISPLAY */
        snprintf(buf, sizeof(buf), "TFT FRAME   320x240 RGB565");
        gfx_text_medium(x, y, buf, UI_TEXT); y += 26;
        snprintf(buf, sizeof(buf), "TFT PUSH    %u bytes/frame", 320 * 240 * 2);
        gfx_text_medium(x, y, buf, UI_TEXT); y += 26;
        snprintf(buf, sizeof(buf), "TFT RATE    ~30 FPS (33ms)");
        gfx_text_medium(x, y, buf, UI_TEXT); y += 26;
        snprintf(buf, sizeof(buf), "OLED        128x64 throttled");
        gfx_text_medium(x, y, buf, UI_TEXT); y += 26;
        snprintf(buf, sizeof(buf), "OLED RATE   ~12 FPS (80ms)");
        gfx_text_medium(x, y, buf, UI_TEXT); y += 26;
        snprintf(buf, sizeof(buf), "ROOF        %s %u%% [%s]",
                 roof_light_state()->enabled ? "ON" : "OFF",
                 roof_light_state()->brightness,
                 roof_light_label());
        gfx_text_medium(x, y, buf, roof_light_state()->enabled ? UI_DANGER : UI_MUTED); y += 26;
        gfx_text_small(x, y, "DIRTY-REGIONS: DRIVE CARDS ONLY", UI_MUTED);
        break;
    }
    case 4: { /* THEME swatches (guide 3.2): hardware contrast verification */
        static const struct { uint16_t c; const char *n; } sw[8] = {
            { UI_BG,      "BG"     }, { UI_SURFACE, "SURF"   },
            { UI_ACCENT,  "ACCENT" }, { UI_DATA,    "DATA"   },
            { UI_TEXT,    "TEXT"   }, { UI_MUTED,   "MUTED"  },
            { UI_OK,      "OK"     }, { UI_DANGER,  "DANGER" },
        };
        for (int i = 0; i < 8; i++) {
            int sx = x + (i % 4) * 76, sy = y + (i / 4) * 64;
            gfx_rect(sx, sy, 68, 36, sw[i].c);
            gfx_rect_outline(sx, sy, 68, 36, UI_BORDER);
            gfx_text_center_box(sx, sy + 40, 68, sw[i].n, UI_FONT_SMALL, UI_TEXT_2);
        }
        break;
    }
    }

    ui_draw_bottombar("L/R:TAB  A/B:MENU", "DIAGNOSTICS");
}

/* ------------------------------ SETTINGS --------------------------------- */
/* guide 5.6: left category list (max 5 visible rows + scroll), right large
   value panel, draft editing. A applies item, START saves all, B discards. */
#define SET_ITEM_COUNT_LOCAL 14   /* keep in sync with car_os.h SET_ITEM_COUNT */
static const char *s_set_names[SET_ITEM_COUNT_LOCAL] = {
    "SPEED CAP", "ENGINE VOL", "OBST DIST", "LED BRIGHT",
    "GEAR 1", "GEAR 2", "GEAR 3", "GEAR 4", "GEAR 5",
    "OLED LAYOUT", "HUD LAYOUT", "MIST MAX", "TFT BRIGHT", "SAVE"
};
static const char *s_set_cat[SET_ITEM_COUNT_LOCAL] = {
    "DRIVE", "AUDIO", "SAFETY", "LIGHTS",
    "GEARS", "GEARS", "GEARS", "GEARS", "GEARS",
    "OLED", "DISPLAY", "SAFETY", "DISPLAY", ""
};
static const char *s_oled_layout_names[4] = { "MINI HUD", "RADAR", "STATUS", "TEXT" };
static const char *s_hud_layout_names[3] = { "FULL", "COMPACT", "NIGHT" };

void os_scr_settings(const os_ctx_t *ctx, uint32_t now)
{
    (void)now;
    gfx_clear(UI_BG);
    ui_draw_topbar(ctx);

    int sel = (int)ctx->settings_sel;

    /* --- left: item list, 5 visible rows, scroll follows selection --- */
    const int lx = 8, lw = 134, lx2 = lx + lw;
    const int row_h = 28, vis = 5;
    int top = sel < vis ? 0 : sel - vis + 1;
    if (top > SET_ITEM_COUNT - vis) top = SET_ITEM_COUNT - vis;
    gfx_text_small(lx, 30, "ITEMS", UI_MUTED);
    for (int r = 0; r < vis; r++) {
        int i = top + r;
        if (i >= SET_ITEM_COUNT) break;
        int y = 38 + r * row_h;
        bool foc = (i == sel);
        if (foc) {
            gfx_rect(lx, y, lw, row_h - 4, UI_SURFACE_2);
            gfx_rect_outline(lx, y, lw, row_h - 4, UI_ACCENT);
        } else {
            gfx_rect(lx, y, lw, row_h - 4, UI_SURFACE);
            gfx_rect_outline(lx, y, lw, row_h - 4, UI_BORDER);
        }
        gfx_text_small(lx + 8, y + 5, s_set_names[i], foc ? UI_TEXT : UI_TEXT_2);
        if (s_set_cat[i][0])
            gfx_text_right(lx2 - 8, y + 6, s_set_cat[i], UI_MUTED, 0, 1);
    }

    /* --- right: large value panel --- */
    int rx = 150, rw = 320 - 150 - 8, ry = 36, rh = 178;
    gfx_rect(rx, ry, rw, rh, UI_SURFACE);
    gfx_rect_outline(rx, ry, rw, rh, UI_BORDER);
    gfx_text_medium(rx + 12, ry + 10, s_set_names[sel], UI_TEXT);
    if (ctx->settings_dirty)
        gfx_text_right(rx + rw - 12, ry + 11, "CHANGED", UI_WARNING, 0, 1);
    else
        gfx_text_right(rx + rw - 12, ry + 11, "OK", UI_OK, 0, 1);

    char buf[24];
    int v = 0, vmin = 0, vmax = 100;
    bool is_bar = false;
    switch (sel) {
    case 0: v = ctx->draft.spd_cap;     vmin = 0;  vmax = 100; is_bar = true; break;
    case 1: v = ctx->draft.engine_vol;  vmin = 0;  vmax = 100; is_bar = true; break;
    case 2: v = ctx->draft.obstacle_cm; vmin = 10; vmax = 80;  is_bar = true; break;
    case 3: v = ctx->draft.led_bright;  vmin = 10; vmax = 100; is_bar = true; break;
    case 4: case 5: case 6: case 7: case 8:
        v = ctx->draft.gear_caps[sel - 4]; vmin = 10; vmax = 100; is_bar = true; break;
    case 12: v = ctx->draft.tft_bright; vmin = 0; vmax = 100; is_bar = true; break;
    default: break;
    }

    if (sel == 9) {
        snprintf(buf, sizeof(buf), "<%s>", s_oled_layout_names[ctx->draft.oled_layout % 4]);
        gfx_text_center_box(rx, ry + 58, rw, buf, UI_FONT_LARGE, UI_ACCENT);
        gfx_text_center_box(rx, ry + 108, rw, "L/R CHOICE", UI_FONT_SMALL, UI_MUTED);
    } else if (sel == 10) {
        snprintf(buf, sizeof(buf), "<%s>", s_hud_layout_names[ctx->draft.hud_layout % 3]);
        gfx_text_center_box(rx, ry + 58, rw, buf, UI_FONT_LARGE, UI_ACCENT);
        gfx_text_center_box(rx, ry + 108, rw, "L/R CHOICE", UI_FONT_SMALL, UI_MUTED);
    } else if (sel == 11) {
        if (ctx->draft.mist_max == 0)
            snprintf(buf, sizeof(buf), "UNLIMITED");
        else
            snprintf(buf, sizeof(buf), "%us", ctx->draft.mist_max * 10);
        gfx_text_center_box(rx, ry + 58, rw, buf, UI_FONT_LARGE, UI_ACCENT);
        gfx_text_center_box(rx, ry + 108, rw, "AUTO CUT-OFF TIME", UI_FONT_SMALL, UI_MUTED);
    } else if (sel == 13) {
        gfx_text_center_box(rx, ry + 58, rw, "SAVE ALL", UI_FONT_LARGE, UI_OK);
        gfx_text_center_box(rx, ry + 108, rw, "WRITE LIVE + NVS", UI_FONT_SMALL, UI_MUTED);
    } else {
        snprintf(buf, sizeof(buf), "%d", v);
        gfx_text_center_box(rx, ry + 50, rw, buf, UI_FONT_LARGE, is_bar ? UI_DATA : UI_TEXT);
        ui_progress(rx + 12, ry + 112, rw - 24, 10, v - vmin, vmax - vmin, UI_ACCENT);
        snprintf(buf, sizeof(buf), "%d .. %d", vmin, vmax);
        gfx_text_center_box(rx, ry + 130, rw, buf, UI_FONT_SMALL, UI_MUTED);
    }
    gfx_text_center_box(rx, ry + 152, rw, "A:APPLY  L/R:SET", UI_FONT_SMALL, UI_ACCENT);

    /* --- discard-changes dialog (guide 5.6) --- */
    if (ctx->settings_confirm)
        ui_dialog("DISCARD CHANGES?", "Unsaved edits will be lost", "DISCARD", "BACK");

    /* PART 10.7 jump-list overlay */
    if (ctx->settings_jump)
        os_scr_jump(ctx);

    ui_draw_bottombar("A:APPLY  B:BACK  START:SAVE", "SETTINGS");
}

/* ------------------------------ OLED CTRL -------------------------------- */
/* guide 5.7: enlarged 128x64 preview rendered from the same model as the
   physical OLED + layout pills. Preview shows the SELECTED layout (Y-style
   preview); A applies it to the physical OLED. */
static int s_ol_x = 0, s_ol_y = 0;   /* OLED preview origin (fb-space) */

static void ol_bar(int x, int y, int w, int h, uint16_t c)
{
    gfx_rect(s_ol_x + x * 2, s_ol_y + y * 2, w * 2, h * 2, c);
}

static void ol_text(int x, int y, const char *s, uint16_t c, int scale)
{
    gfx_text(s_ol_x + x * 2, s_ol_y + y * 2, s, c, 0, scale);
}

static void ol_text_center(const char *s, int y, uint16_t c, int scale)
{
    int w = gfx_text_w(s, scale);
    int x = (128 - w) / 2;
    if (x < 0) x = 0;
    gfx_text(s_ol_x + x * 2, s_ol_y + y * 2, s, c, 0, scale);
}

static void oled_preview(uint8_t layout, const os_ctx_t *ctx)
{
    char buf[32];
    int spd = (abs(g.cur_l) + abs(g.cur_r)) / 2;

    switch (layout) {
    case 0: /* MINI HUD */
        ol_text(2, 2, "SPD", UI_TEXT, 1);
        snprintf(buf, sizeof(buf), "%d", spd);
        ol_text(2, 14, buf, UI_TEXT, 2);
        snprintf(buf, sizeof(buf), "G%u", (unsigned)(ctx->gear + 1));
        ol_text(72, 2, buf, UI_TEXT, 1);
        ol_text(2, 48, "FRONT", UI_TEXT, 1);
        if (g.dist_avg[1] != 65535) {
            snprintf(buf, sizeof(buf), "%u", g.dist_avg[1]);
            ol_text(72, 48, buf, UI_TEXT, 1);
        } else {
            ol_text(72, 48, "--", UI_TEXT, 1);
        }
        break;
    case 1: { /* RADAR */
        snprintf(buf, sizeof(buf), "SPD %3d%% G%u", spd, (unsigned)(ctx->gear + 1));
        ol_text(2, 2, buf, UI_TEXT, 1);
        static const char *dn[3] = { "L", "F", "R" };
        for (int i = 0; i < 3; i++) {
            int d = (g.dist_avg[i] == 65535) ? -1 : (int)g.dist_avg[i];
            int y = 18 + i * 16;
            ol_text(2, y, dn[i], UI_TEXT, 1);
            int bw = (d < 0) ? 0 : ((d > 100) ? 100 : d) * 88 / 100;
            ol_bar(16, y, 2 + bw, 10, UI_TEXT);
            if (d >= 0) { snprintf(buf, sizeof(buf), "%d", d); ol_text(108, y, buf, UI_TEXT, 1); }
            else        ol_text(108, y, "--", UI_TEXT, 1);
        }
        ol_text(2, 56, "STATUS", UI_TEXT, 1);
        break;
    }
    case 2: /* STATUS */
        ol_text_center("CAR OS", 16, UI_TEXT, 3);
        ol_text(14, 50, "PAUSED", UI_TEXT, 1);
        break;
    default: /* 3 = CUSTOM TEXT */
        ol_text_center(ctx->oled_text, 24, UI_TEXT, 2);
        ol_text(2, 56, "CUSTOM", UI_TEXT, 1);
        break;
    }
}

void os_scr_oledctrl(const os_ctx_t *ctx, uint32_t now)
{
    (void)now;
    gfx_clear(UI_BG);
    ui_draw_topbar(ctx);

    static const char *names[4] = { "MINI HUD", "RADAR", "STATUS", "TEXT" };

    /* --- preview window (128x64 scaled x2 + border) --- */
    int bx = 22, by = 36, bw = 276, bh = 146;
    gfx_rect(bx, by, bw, bh, UI_SURFACE);
    gfx_rect_outline(bx, by, bw, bh, UI_BORDER);
    gfx_text_small(bx + 6, by + 5, "OLED PREVIEW 128x64 (x2)", UI_MUTED);
    s_ol_x = bx + 10;
    s_ol_y = by + 16;                 /* content area 256x128, black OLED bg */
    oled_preview(ctx->oled_sel, ctx);

    /* --- layout pills row --- */
    int pw = 66, gap = 6, total = 4 * pw + 3 * gap;
    int px = (UI_W - total) / 2;
    for (int i = 0; i < 4; i++) {
        int lx = px + i * (pw + gap), ly = 194;
        bool sel = (i == (int)ctx->oled_sel);
        bool active = (i == (int)ctx->oled_layout);
        uint16_t bg, border, fg;
        if (active && sel)       { bg = UI_ACCENT; border = UI_ACCENT; fg = RGB565(20,12,0); }
        else if (active)         { bg = UI_ACCENT_DARK; border = UI_ACCENT; fg = UI_TEXT; }
        else if (sel)            { bg = UI_SURFACE_2; border = UI_ACCENT; fg = UI_TEXT; }
        else                     { bg = UI_SURFACE_3; border = UI_BORDER; fg = UI_MUTED; }
        gfx_rect(lx, ly, pw, 20, bg);
        gfx_rect_outline(lx, ly, pw, 20, border);
        gfx_text_center_box(lx, ly + 6, pw, names[i], UI_FONT_SMALL, fg);
    }

    /* custom text editor hint when TEXT selected */
    if (ctx->oled_sel == 3) {
        gfx_text_center_box(0, 220, UI_W, "L/R: CHANGE TEXT PRESET",
                            UI_FONT_SMALL, UI_MUTED);
    }
    gfx_text_center_box(0, 208, UI_W, "UP/DN LAYOUT  A:APPLY  Y:PREVIEW  B:BACK",
                        UI_FONT_SMALL, UI_MUTED);
    scr_footer(ctx);
}

/* ------------------------------ GAMES HUB -------------------------------- */
/* guide 5.8: theme rows - number badge + icon + name/desc, amber focus. */
static const ui_icon_t s_game_icons[OS_GAME_COUNT] = {
    UI_ICON_SPARK,   /* NEON CONVOY  */
    UI_ICON_AUTO,    /* NEON SERPENT */
};
static const char *const s_game_nums[OS_GAME_COUNT] = { "1", "2" };

void os_scr_gameshub(const os_ctx_t *ctx, uint32_t now)
{
    (void)now;
    gfx_clear(UI_BG);
    ui_draw_topbar(ctx);

    for (int i = 0; i < OS_GAME_COUNT; i++) {
        bool foc = (i == ctx->game_sel);
        int y = 28 + i * 32 - (foc ? 2 : 0);   /* 6 rows fit above bottom bar */
        int h = 30;
        ui_panel(8, y, 304, h, foc);

        /* number badge */
        gfx_rect(16, y + 4, 22, 22, foc ? UI_ACCENT : UI_SURFACE_3);
        gfx_rect_outline(16, y + 4, 22, 22, foc ? UI_ACCENT : UI_BORDER);
        gfx_text_center_box(16, y + 10, 22, s_game_nums[i], UI_FONT_SMALL,
                            foc ? RGB565(20, 12, 0) : UI_MUTED);

        /* icon + name + desc */
        ui_draw_icon(48, y + 7, 16, s_game_icons[i], foc ? UI_ACCENT : UI_MUTED);
        gfx_text_medium(72, y + 7, OS_GAMES[i].name, foc ? UI_TEXT : UI_TEXT_2);
        gfx_text_right(304, y + 11, OS_GAMES[i].desc, UI_MUTED, 0, 1);
    }
    ui_draw_bottombar("UP/DN:SELECT   A:PLAY   B:BACK", "GAMES HUB");
}

/* ---------------- PART 10 new windows + switcher ------------------------ */
static uint16_t notif_col(notif_cat_t c)
{
    switch (c) {
    case NOTIF_ALEXA: return UI_DATA;
    case NOTIF_SAFETY: return UI_DANGER;
    case NOTIF_RULES: return UI_WARNING;
    case NOTIF_SOUND: return RGB565(255, 220, 80);
    default: return RGB565(190, 120, 255);
    }
}

void os_scr_score(const os_ctx_t *ctx, uint32_t now)
{
    (void)ctx; (void)now;
    gfx_clear(UI_BG);
    ui_draw_topbar(ctx);
    imu_adv_snap_t sn;
    imu_adv_snapshot(&sn);
    char b[32];
    snprintf(b, sizeof(b), "%u", (unsigned)sn.drive_score);
    gfx_text_center_box(0, 34, 150, b, UI_FONT_LARGE, UI_TEXT);
    snprintf(b, sizeof(b), "GRADE %c", sn.drive_grade);
    gfx_text_center_box(150, 48, 170, b, UI_FONT_MEDIUM,
                        sn.drive_grade == 'A' ? UI_OK :
                        sn.drive_grade == 'F' ? UI_DANGER : UI_WARNING);
    static const char *nm[6] = { "CORNER", "BRAKE", "BUMPS", "STEADY", "GRIP", "TILT" };
    uint8_t vv[6] = { sn.sc_corner, sn.sc_brake, sn.sc_bumps,
                      sn.sc_stable, sn.sc_traction, sn.sc_tilt };
    for (int i = 0; i < 6; i++) {
        int y = 92 + i * 20;
        gfx_text_small(14, y + 5, nm[i], UI_TEXT_2);
        ui_progress(100, y + 4, 150, 10, vv[i], 100,
                    vv[i] >= 70 ? UI_OK : vv[i] >= 40 ? UI_WARNING : UI_DANGER);
        snprintf(b, sizeof(b), "%u", (unsigned)vv[i]);
        gfx_text_right(310, y + 5, b, UI_TEXT, 0, 1);
    }
    ui_draw_bottombar("B:BACK", "DRIVE SCORE");
}

void os_scr_trip(const os_ctx_t *ctx, uint32_t now)
{
    (void)now;
    gfx_clear(UI_BG);
    ui_draw_topbar(ctx);
    char b[32];
    uint32_t t = trip_time_s();
    snprintf(b, sizeof(b), "%02u:%02u", (unsigned)(t / 60), (unsigned)(t % 60));
    gfx_text_center_box(0, 36, 320, b, UI_FONT_LARGE, UI_TEXT);
    gfx_text_center_box(0, 66, 320, "TIME", UI_FONT_SMALL, UI_MUTED);
    snprintf(b, sizeof(b), "DIST %u m (est)", (unsigned)trip_dist_m());
    gfx_text_medium(14, 100, b, UI_TEXT);
    snprintf(b, sizeof(b), "AVG %u%%  TOP %u%%", (unsigned)trip_avg_speed(),
             (unsigned)trip_top_speed());
    gfx_text_medium(14, 128, b, UI_TEXT);
    snprintf(b, sizeof(b), "MAX TILT %.0f   BUMPS %u", (double)trip_max_tilt(),
             (unsigned)trip_hard_bumps());
    gfx_text_medium(14, 156, b, UI_TEXT);
    snprintf(b, sizeof(b), "ODO %u m", (unsigned)alexa_odo_m());
    gfx_text_medium(14, 182, b, UI_TEXT_2);
    gfx_text_center_box(0, 200, 320, "A: RESET TRIP", UI_FONT_SMALL, UI_ACCENT);
    ui_draw_bottombar("A:RESET B:BACK", "TRIP");
}

void os_scr_conn(const os_ctx_t *ctx, uint32_t now)
{
    (void)now;
    gfx_clear(UI_BG);
    ui_draw_topbar(ctx);
    char b[48];
    int y = 40, x = 14;
    bool dongle = xbox360_dongle_connected();
    int pads = 0;
    for (int i = 0; i < 4; i++) {
        const xbox360_pad_t *p = xbox360_pad((uint8_t)i);
        if (p && p->present) pads++;
    }
    snprintf(b, sizeof(b), "USB DONGLE   %s", dongle ? "OK" : "MISSING");
    gfx_text_medium(x, y, b, dongle ? UI_OK : UI_DANGER); y += 26;
    snprintf(b, sizeof(b), "PADS         %d/4", pads);
    gfx_text_medium(x, y, b, pads ? UI_TEXT : UI_MUTED); y += 26;
    int batt = ui_cockpit_battery_pct();
    if (batt < 0) snprintf(b, sizeof(b), "PAD BAT      --");
    else {
        const xbox360_pad_t *pb = xbox360_pad(0);
        unsigned lvl = (pb && pb->present) ? (unsigned)(pb->battery & 3) : 0;
        snprintf(b, sizeof(b), "PAD BAT      %d%% LVL %u/3", batt, lvl);
    }
    gfx_text_medium(x, y, b, batt < 0 ? UI_MUTED : UI_TEXT); y += 26;
    snprintf(b, sizeof(b), "LINK AGE     %lums",
             (unsigned long)(now - xbox360_last_data_ms()));
    gfx_text_medium(x, y, b, UI_TEXT); y += 26;
    snprintf(b, sizeof(b), "WIFI         %s",
             net_wifi_up() ? "UP" : "DOWN");
    gfx_text_medium(x, y, b, net_wifi_up() ? UI_OK : UI_MUTED); y += 26;
    snprintf(b, sizeof(b), "MQTT         %s", alexa_mqtt_connected() ? "UP" : "DOWN");
    gfx_text_medium(x, y, b, alexa_mqtt_connected() ? UI_OK : UI_MUTED);
    ui_draw_bottombar("B:BACK", "CONNECT");
}

static uint8_t s_notif_scroll;

void os_scr_notif(const os_ctx_t *ctx, uint32_t now)
{
    (void)ctx; (void)now;
    gfx_clear(UI_BG);
    ui_draw_topbar(ctx);
    gfx_text_small(UI_GAP_S, UI_CONTENT_TOP + 2, "NOTIFICATIONS", UI_MUTED);
    notif_item_t items[24];
    int n = notif_list(items, 24);
    if (!n) {
        gfx_text_center_box(0, 120, 320, "NO EVENTS YET", UI_FONT_MEDIUM, UI_MUTED);
    } else {
        if (s_notif_scroll >= (uint8_t)n) s_notif_scroll = 0;
        int vis = 6;   /* 6 rows fit above the legend strip */
        int top = s_notif_scroll;
        if (top > n - vis) top = n - vis < 0 ? 0 : n - vis;
        for (int i = 0; i < vis && top + i < n; i++) {
            int y = 44 + i * 24;
            notif_item_t *it = &items[top + i];
            gfx_rect(10, y, 300, 20, UI_SURFACE);
            gfx_rect_outline(10, y, 300, 20, UI_BORDER);
            gfx_rect(12, y + 3, 6, 14, notif_col(it->cat));
            gfx_text_small(24, y + 6, it->text, UI_TEXT);
        }
    }
    /* legend */
    gfx_text_small(10, 196, "ALEXA", UI_DATA);
    gfx_text_small(70, 196, "SAFETY", UI_DANGER);
    gfx_text_small(130, 196, "RULES", UI_WARNING);
    gfx_text_small(190, 196, "SOUND", RGB565(255, 220, 80));
    gfx_text_small(250, 196, "MPU", RGB565(190, 120, 255));
    ui_draw_bottombar("UP/DN:SCROLL B:BACK", "NOTIFY");
}

void os_notif_scroll(int dir, int n)
{
    if (dir > 0) s_notif_scroll = (uint8_t)(s_notif_scroll + 1 < (uint8_t)n ? s_notif_scroll + 1 : s_notif_scroll);
    else if (s_notif_scroll) s_notif_scroll--;
}

/* PART 10.6 App Switcher (START double-tap): last-4 window thumbnails */
void os_scr_switcher(const os_ctx_t *ctx, uint32_t now)
{
    (void)now;
    os_state_t hist[4];
    int n = os_hist_win_list(hist, 4);
    gfx_rect(10, 14, 300, 196, UI_SURFACE);
    gfx_rect_outline(10, 14, 300, 196, UI_ACCENT);
    gfx_text_center_box(10, 20, 300, "APP SWITCHER", UI_FONT_SMALL, UI_ACCENT);
    if (!n) {
        gfx_text_center_box(10, 110, 300, "NO WINDOWS YET", UI_FONT_SMALL, UI_MUTED);
    } else {
        for (int i = 0; i < n; i++) {
            int col = i % 2, row = i / 2;
            int x = 20 + col * 148, y = 40 + row * 76, w = 140, h = 68;
            bool sel = (i == (int)ctx->switch_sel);
            uint16_t tw, th;
            const uint16_t *tp = os_thumb_get(hist[i], &tw, &th);
            if (tp) {
                /* thumb is 160x120; draw every 2nd px into 80x60 box */
                for (int ty = 0; ty < 60 && ty * 2 < th; ty++)
                    for (int tx = 0; tx < 80 && tx * 2 < tw; tx++)
                        gfx_px(x + 30 + tx, y + 4 + ty, tp[(ty * 2) * tw + tx * 2]);
            } else {
                gfx_rect(x + 30, y + 4, 80, 60, UI_SURFACE_3);
            }
            if (sel) gfx_rect_outline(x, y, w, h, UI_ACCENT);
            else gfx_rect_outline(x, y, w, h, UI_BORDER);
            gfx_text_center_box(x, y + h - 2, w, os_state_name(hist[i]),
                                UI_FONT_SMALL, sel ? UI_TEXT : UI_MUTED);
        }
    }
    gfx_text_center_box(10, 214 - 8, 300, "A OPEN  B CLOSE", UI_FONT_SMALL, UI_MUTED);
}

/* PART 10.7 Settings jump-list overlay (X in Settings) */
void os_scr_jump(const os_ctx_t *ctx)
{
    static const char *cats[7] = {
        "DRIVE", "AUDIO", "SAFETY", "LIGHTS", "GEARS", "OLED", "DISPLAY"
    };
    gfx_rect(70, 46, 180, 150, UI_SURFACE_2);
    gfx_rect_outline(70, 46, 180, 150, UI_ACCENT);
    gfx_text_center_box(70, 52, 180, "JUMP TO", UI_FONT_SMALL, UI_ACCENT);
    for (int i = 0; i < 7; i++) {
        bool sel = (i == (int)ctx->jump_sel);
        if (sel) gfx_rect(76, 68 + i * 17, 168, 15, UI_ACCENT);
        gfx_text_center_box(76, 70 + i * 17, 168, cats[i], UI_FONT_SMALL,
                            sel ? RGB565(20, 12, 0) : UI_TEXT_2);
    }
}
