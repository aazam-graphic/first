/*
 * ui_drive_cockpit.c - DRIVE COCKPIT SCREEN (spec 1.md PART 3).
 *
 * Layout 320x240: status bar + controller battery, pitch/roll arc, FRT,
 * G-meter, heading ring (RELATIVE only, never true north) + 8-dir car sprite,
 * traction pill, LFT/RGT side arcs, IMU READY, stability shield, airtime
 * overlay, alerts (dim cockpit 40% behind). Sub-views + 3 palette themes.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "ui_drive_cockpit.h"
#include "os_gfx.h"
#include "car_global.h"
#include "car_os.h"
#include "xbox360.h"
#include "imu_adv.h"
#include "imu_driver.h"
#include "snd_bank.h"
#include "img_bank.h"   /* PART 6: wallpapers + car sprites */
#include "mic_in.h"     /* PART 12: M3 VU overlay */
#include "net_wifi.h"
#include "alexa_bridge.h"

/* ------------------------------ palette --------------------------------- */
typedef struct {
    uint16_t bg, surface, surface2, border;
    uint16_t text, muted;
    uint16_t arc;        /* main arc color */
    uint16_t navy;       /* solar arcs */
    uint16_t accent;     /* amber */
    uint16_t ok, warn, danger, grey;
    uint8_t  stroke;     /* 1 normal, 2 sunlight boost */
    bool     shadow;     /* soft shadow under car sprite */
} cockpit_pal_t;

static const cockpit_pal_t PAL_NIGHT = {
    .bg = RGB565(11, 18, 32), .surface = RGB565(17, 24, 39),
    .surface2 = RGB565(27, 37, 58), .border = RGB565(42, 58, 90),
    .text = RGB565(230, 235, 245), .muted = RGB565(130, 145, 165),
    .arc = RGB565(34, 211, 238), .navy = RGB565(34, 211, 238),
    .accent = RGB565(255, 176, 32), .ok = RGB565(53, 208, 127),
    .warn = RGB565(255, 197, 61), .danger = RGB565(255, 59, 48),
    .grey = RGB565(110, 125, 145), .stroke = 1, .shadow = true,
};
static const cockpit_pal_t PAL_SOLAR = {
    .bg = RGB565(244, 246, 250), .surface = RGB565(255, 255, 255),
    .surface2 = RGB565(225, 232, 240), .border = RGB565(140, 155, 175),
    .text = RGB565(16, 24, 40), .muted = RGB565(90, 105, 125),
    .arc = RGB565(20, 60, 120), .navy = RGB565(20, 60, 120),
    .accent = RGB565(200, 120, 0), .ok = RGB565(0, 140, 70),
    .warn = RGB565(180, 110, 0), .danger = RGB565(200, 20, 20),
    .grey = RGB565(140, 150, 165), .stroke = 1, .shadow = true,
};
static const cockpit_pal_t PAL_SUN = {
    .bg = RGB565(255, 255, 255), .surface = RGB565(255, 255, 255),
    .surface2 = RGB565(240, 240, 240), .border = RGB565(0, 0, 0),
    .text = RGB565(0, 0, 0), .muted = RGB565(40, 40, 40),
    .arc = RGB565(0, 0, 0), .navy = RGB565(0, 40, 120),
    .accent = RGB565(0, 0, 0), .ok = RGB565(0, 0, 0),
    .warn = RGB565(0, 0, 0), .danger = RGB565(0, 0, 0),
    .grey = RGB565(120, 120, 120), .stroke = 2, .shadow = false,
};

static const cockpit_pal_t *pal_for(uint8_t theme)
{
    if (theme == COCKPIT_THEME_SUN) return &PAL_SUN;
    if (theme == COCKPIT_THEME_NIGHT) return &PAL_NIGHT;
    return &PAL_SOLAR;
}

const char *ui_cockpit_view_name(uint8_t v)
{
    switch (v % COCKPIT_VIEW_COUNT) {
    case COCKPIT_VIEW_FULL: return "COCKPIT";
    case COCKPIT_VIEW_COMPACT: return "COMPACT";
    case COCKPIT_VIEW_SENSOR: return "SENSOR";
    default: return "NIGHT";
    }
}
const char *ui_cockpit_theme_name(uint8_t t)
{
    switch (t % COCKPIT_THEME_COUNT) {
    case COCKPIT_THEME_NIGHT: return "NIGHT DRIVE";
    case COCKPIT_THEME_SUN: return "SUNLIGHT BOOST";
    default: return "SOLAR LIGHT";
    }
}

void ui_cockpit_next_view(os_ctx_t *ctx)
{
    ctx->drive_sub_view = (uint8_t)((ctx->drive_sub_view + 1) % COCKPIT_VIEW_COUNT);
    /* PART 5: revealing the Sensor view reveals the Drive Score */
    if (ctx->drive_sub_view == COCKPIT_VIEW_SENSOR) snd_play("score_reveal");
}
void ui_cockpit_next_theme(os_ctx_t *ctx)
{
    ctx->cockpit_theme = (uint8_t)((ctx->cockpit_theme + 1) % COCKPIT_THEME_COUNT);
}

int ui_cockpit_battery_pct(void)
{
    const xbox360_pad_t *p = xbox360_pad(0);
    /* No pad, or no battery report yet (fresh pairing reads 0 = UNKNOWN,
       never shown as 15%): "--" instead of a false low reading. */
    if (!p || !p->present || !p->battery_valid) return -1;
    switch (p->battery & 3) {
    case 0: return 15;
    case 1: return 45;
    case 2: return 78;
    default: return 100;
    }
}

void ui_cockpit_dim_rect(int x, int y, int w, int h)
{
    uint16_t *fb = gfx_fb();
    if (!fb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 320) w = 320 - x;
    if (y + h > 240) h = 240 - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        uint16_t *row = fb + yy * 320 + x;
        for (int i = 0; i < w; i++) {
            uint16_t c = row[i];
            uint16_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            r = r * 3 / 5; g = g * 3 / 5; b = b * 3 / 5;   /* dim 40% */
            row[i] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

/* ------------------------------ primitives ------------------------------ */
static void circ(int cx, int cy, int r, uint16_t c)
{
    int a = 0, b = r, dd = 3 - 2 * r;
    while (b >= a) {
        gfx_px(cx + a, cy + b, c); gfx_px(cx - a, cy + b, c);
        gfx_px(cx + a, cy - b, c); gfx_px(cx - a, cy - b, c);
        gfx_px(cx + b, cy + a, c); gfx_px(cx - b, cy + a, c);
        gfx_px(cx + b, cy - a, c); gfx_px(cx - b, cy - a, c);
        a++;
        if (dd < 0) dd += 4 * a + 6;
        else { b--; dd += 4 * (a - b) + 10; }
    }
}
static void disc(int x, int y, int r, uint16_t c)
{
    for (int d = -r; d <= r; d++) {
        int w = (int)sqrtf((float)(r * r - d * d));
        gfx_hline(x - w, y + d, 2 * w + 1, c);
    }
}
/* zone color for tilt angle (3.2): <10 CYAN | 10-20 AMBER | >20 RED pulse */
static uint16_t tilt_zone_col(const cockpit_pal_t *p, float tilt, uint32_t now)
{
    float a = tilt < 0 ? -tilt : tilt;
    if (a < 10.0f) return p->arc;
    if (a < 20.0f) return p->warn;
    if (((now / 400) & 1) == 0) return p->danger;   /* RED pulse */
    return p->warn;
}
/* ultrasonic zone (3.2): >60 CYAN | 25-60 AMBER | <25 RED flash | no-echo GREY */
static uint16_t us_zone_col(const cockpit_pal_t *p, int cm, bool ok, uint32_t now)
{
    if (!ok) return p->grey;
    if (cm < 25) return (((now / 300) & 1) == 0) ? p->danger : p->surface;
    if (cm < 60) return p->warn;
    return p->arc;
}

/* 8-direction car sprite with soft shadow (3.2). PART 6: uses car_0..7 +
   car_shadow assets at half size; procedural fallback if missing. */
/* ------------------- image-faithful FULL-view helpers ------------------- */
static void ell(int cx, int cy, int rx, int ry, uint16_t c)
{
    if (rx < 1 || ry < 1) return;
    for (int d = -ry; d <= ry; d++) {
        int w = (int)(rx * sqrtf(1.0f - (float)(d * d) / (float)(ry * ry)));
        gfx_hline(cx - w, cy + d, 2 * w + 1, c);
    }
}

static void arc_seg(int cx, int cy, int r, int a0, int a1, uint16_t c)
{
    if (a0 > a1) { int t = a0; a0 = a1; a1 = t; }
    for (int a = a0; a <= a1; a += 3) {
        float rad = a * 0.0174533f;
        gfx_px(cx + (int)(r * cosf(rad)), cy + (int)(r * sinf(rad)), c);
    }
}

/* small filled triangle: dir 0=up 1=down */
static void tri(int x, int y, int s, int dir, uint16_t c)
{
    for (int i = 0; i <= s; i++) {
        int w = (dir == 0) ? (s - i) : i;
        int yy = (dir == 0) ? (y - s + i) : (y + i);
        gfx_hline(x - w, yy, 2 * w + 1, c);
    }
}

/* PART 7.0 band layout: every widget below draws ONLY inside its fixed rect
   (see draw_full). ui_clip_set() enforces it - overlap is impossible even
   on future edits. Set COCKPIT_DBG 1 to outline all band rects on screen. */
#define COCKPIT_DBG 0

/* ellipse outline (compass ring rx75/ry55) */
static void ell_outline(int cx, int cy, int rx, int ry, uint16_t c)
{
    for (int a = 0; a < 360; a += 2) {
        float rad = a * 0.0174533f;
        gfx_px(cx + (int)(rx * cosf(rad)), cy + (int)(ry * sinf(rad)), c);
    }
}

/* PART 7.0 section 4 sector table -> sprite names (dark set for light bg) */
static const char *cockpit_car_name(float heading_rel, bool dark)
{
    float h = heading_rel < 0 ? heading_rel + 360.0f : heading_rel;
    int sec = (int)((h + 22.5f) / 45.0f) % 8;
    static const char *base[8] = {
        "car_N", "car_NE", "car_E", "car_SE",
        "car_S", "car_SW", "car_W", "car_NW"
    };
    static char name[16];
    if (dark) snprintf(name, sizeof(name), "%s_d", base[sec]);
    else snprintf(name, sizeof(name), "%s", base[sec]);
    return name;
}

/* ------------------------------ status bar ------------------------------ */
static void draw_status_bar(const cockpit_pal_t *p, os_ctx_t *ctx,
                            const imu_adv_snap_t *sn, uint32_t now)
{
    gfx_rect(0, 0, 320, 22, p->surface);
    gfx_hline(0, 22, 320, p->border);
    /* SYS pill */
    const char *sys = "SYS OK";
    uint16_t sysc = p->ok;
    if (g.estop || (sn->valid && sn->tilt_lockout)) { sys = "SYS FAULT"; sysc = p->danger; }
    else if ((sn->valid && (sn->tilt_critical || sn->traction != IMU_TRACTION_GRIP
             || sn->shield == IMU_SHIELD_RED))
             || ctx->alert_active) { sys = "SYS WARN"; sysc = p->warn; }
    gfx_rect(6, 4, 62, 14, sysc);
    gfx_text_small(10, 8, sys, p->bg == PAL_SUN.bg ? p->surface : p->surface);
    /* activity dots (4, rotating) */
    for (int i = 0; i < 4; i++) {
        int x = 84 + i * 10;
        bool on = ((now / 250) % 4) == i;
        disc(x, 11, 2, on ? p->arc : p->grey);
    }
    /* controller battery (from existing 3 s poll, previously unused in UI) */
    int batt = ui_cockpit_battery_pct();
    char bb[16];
    if (batt < 0) snprintf(bb, sizeof(bb), "--%%");
    else snprintf(bb, sizeof(bb), "%d%%", batt);
    uint16_t bc = (batt < 0) ? p->grey : (batt <= 20 ? p->danger : p->text);
    /* battery icon + % */
    gfx_rect_outline(196, 6, 16, 10, bc);
    gfx_rect(212, 9, 2, 4, bc);
    int fill = (batt < 0) ? 0 : (14 * batt / 100);
    if (fill > 0) gfx_rect(197, 7, fill, 8, bc);
    gfx_text_small(220, 8, bb, bc);
    /* link */
    bool pad = xbox360_dongle_connected();
    gfx_text_small(262, 8, pad ? "LINK" : "NO PAD", pad ? p->ok : p->warn);
}

/* ------------------------- VIEW 1: image cockpit -------------------------
   Matches the reference render: status bar (SYS pill + dots + signal/
   battery), wide gradient tilt arc with -20/-5/0/+5/+20 labels, value +
   GRIP row, compass ring + 8-sprite car + N/W/E/S, LFT/RGT flank rows,
   FRONT as slim side vertical bar (never a big text), footer hints. */
static uint16_t grad_cyan_amber(float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int r = 34 + (int)((255 - 34) * t);
    int g = 211 + (int)((176 - 211) * t);
    int b = 238 + (int)((32 - 238) * t);
    return RGB565(r, g, b);
}

/* small pill: filled rounded rect + centered label */
static void pill(int x, int y, int w, int h, uint16_t fill, const char *s,
                 uint16_t tc)
{
    int r = h / 2;
    gfx_rect(x + r, y, w - 2 * r, h, fill);
    disc(x + r, y + r, r, fill);
    disc(x + w - r - 1, y + r, r, fill);
    gfx_text_small(x + (w - (int)strlen(s) * 6) / 2, y + (h - 7) / 2, s, tc);
}

static void draw_full(os_ctx_t *ctx, const cockpit_pal_t *p,
                      const imu_adv_snap_t *sn, uint32_t now)
{
    char b[64];
    float tilt = 0.0f, roll = 0.0f;
    if (sn->valid) {
        roll = sn->roll_deg;
        tilt = fabsf(sn->pitch_deg) > fabsf(roll) ? fabsf(sn->pitch_deg) : fabsf(roll);
    }
    uint16_t zc = sn->valid ? tilt_zone_col(p, tilt, now) : p->grey;
#if COCKPIT_DBG
    gfx_clear(p->bg);
    gfx_rect_outline(0, 0, 320, 21, p->danger);
    gfx_rect_outline(20, 22, 280, 96, p->danger);
    gfx_rect_outline(80, 122, 118, 96, p->danger);
    gfx_rect_outline(288, 122, 22, 92, p->danger);
    gfx_rect_outline(0, 221, 320, 19, p->danger);
#else
    gfx_clear(p->bg);
#endif

    /* ================= STATUS BAR (0-21) ================= */
    {
        const char *word = "SYS OK";
        uint16_t col = p->ok;
        if (g.estop || (sn->valid && sn->tilt_lockout)) { word = "FAULT"; col = p->danger; }
        else if ((sn->valid && (sn->tilt_critical ||
                 sn->traction != IMU_TRACTION_GRIP ||
                 sn->shield == IMU_SHIELD_RED)) || ctx->alert_active) {
            word = "WARN"; col = p->warn;
        }
        ui_clip_set(0, 0, 320, 21);
        pill(6, 4, 62, 13, col, word, RGB565(10, 14, 24));
        /* activity dots: USB WiFi Alexa mic */
        {
            uint16_t dots[4];
            dots[0] = xbox360_dongle_connected() ? p->ok : p->grey;
            dots[1] = net_wifi_up() ? p->ok : p->grey;
            dots[2] = alexa_mqtt_connected() ? p->ok : p->grey;
            dots[3] = mic_ready() ? p->ok : p->grey;
            for (int i = 0; i < 4; i++)
                disc(148 + i * 13, 10, 2, dots[i]);
        }
        /* signal bars + controller battery (fuel/temp: no sensors) */
        {
            uint16_t sc = net_wifi_up() ? p->text : p->grey;
            for (int i = 0; i < 4; i++) {
                int bh = 3 + i * 2;
                gfx_rect(244 + i * 4, 16 - bh, 3, bh, sc);
            }
            int batt = ui_cockpit_battery_pct();
            char bb[8];
            if (batt < 0) snprintf(bb, sizeof(bb), "--");
            else snprintf(bb, sizeof(bb), "%d%%", batt);
            uint16_t bc = (batt < 0) ? p->grey : (batt <= 20 ? p->danger : p->text);
            gfx_rect_outline(262, 6, 20, 9, bc);
            gfx_rect(282, 8, 2, 5, bc);
            int fill = (batt < 0) ? 0 : (18 * batt / 100);
            if (fill > 0) gfx_rect(263, 7, fill, 7, bc);
            gfx_text_small(286, 7, bb, bc);
        }
        gfx_hline(0, 21, 320, p->border);
        ui_clip_reset();
    }

    /* ================= TILT ARC (22-118) ================= */
    {
        ui_clip_set(20, 22, 280, 96);
        int cx = 160, cy = 182, r = 143;
        /* dark track */
        for (int rr = r - 4; rr <= r + 4; rr += 2)
            arc_seg(cx, cy, rr, 180 + 32, 360 - 32, p->surface2);
        /* gradient fill from left end (-58deg) to live roll */
        if (sn->valid) {
            float ra = roll / 20.0f * 58.0f;
            if (ra < -58) ra = -58;
            if (ra > 58) ra = 58;
            for (int a = -58; a <= (int)ra; a += 2) {
                float t = (a + 58.0f) / 116.0f;
                uint16_t c = grad_cyan_amber(t);
                float rad = (270.0f + a) * 0.0174533f;
                for (int rr = r - 4; rr <= r + 4; rr += 2)
                    gfx_px(cx + (int)(rr * cosf(rad)), cy + (int)(rr * sinf(rad)), c);
            }
        }
        /* minor ticks every ~11.6deg, majors at -20/-5/0/+5/+20 */
        for (int a = -58; a <= 58; a += 6) {
            float rad = (270.0f + a) * 0.0174533f;
            gfx_line(cx + (int)((r - 10) * cosf(rad)), cy + (int)((r - 10) * sinf(rad)),
                     cx + (int)((r - 5) * cosf(rad)), cy + (int)((r - 5) * sinf(rad)),
                     p->border);
        }
        {
            static const float ma[5] = { -58.0f, -14.5f, 0.0f, 14.5f, 58.0f };
            for (int i = 0; i < 5; i++) {
                float rad = (270.0f + ma[i]) * 0.0174533f;
                gfx_line(cx + (int)((r - 13) * cosf(rad)), cy + (int)((r - 13) * sinf(rad)),
                         cx + (int)((r - 3) * cosf(rad)), cy + (int)((r - 3) * sinf(rad)),
                         p->text);
            }
        }
        /* apex marker (fixed 0) */
        gfx_vline(160, 34, 8, p->text);
        tri(160, 44, 3, 1, p->text);
        /* tick labels */
        gfx_text_small(56, 92, "-20", p->arc);
        gfx_text_small(114, 92, "-5", p->arc);
        gfx_text_small(157, 92, "0", p->text);
        gfx_text_small(196, 92, "+5", p->accent);
        gfx_text_small(238, 92, "+20", p->accent);
        ui_clip_reset();
        /* value + GRIP row */
        ui_clip_set(20, 102, 280, 16);
        {
            char vb[20];
            if (sn->valid) snprintf(vb, sizeof(vb), "%+.1f/-%.1f",
                                    (double)sn->pitch_deg, (double)roll);
            else snprintf(vb, sizeof(vb), "--/--");
            gfx_text_small(56, 104, vb, zc);
        }
        {
            const char *t = "GRIP";
            uint16_t tc = p->ok;
            if (sn->valid) {
                if (sn->traction == IMU_TRACTION_STUCK) { t = "STUCK"; tc = p->danger; }
                else if (sn->traction == IMU_TRACTION_SLIPPING) { t = "SLIP"; tc = p->warn; }
            } else { t = "--"; tc = p->grey; }
            char pb[8];
            snprintf(pb, sizeof(pb), "%s", t);
            pill(248, 103, 58, 13, tc, pb, RGB565(10, 14, 24));
            if (sn->valid && sn->traction != IMU_TRACTION_GRIP &&
                (((now / 300) & 1) == 0))
                gfx_rect_outline(248, 103, 58, 13, tc);
        }
        gfx_hline(0, 118, 320, p->border);
        ui_clip_reset();
    }

    /* ================= COMPASS + CAR (119-220) ================= */
    {
        float h = sn->valid ? sn->heading_rel : 0.0f;
        int ccx = 138, ccy = 170;
        ui_clip_set(0, 119, 284, 102);
        ell_outline(ccx, ccy, 58, 48, p->border);
        for (int a = 0; a < 360; a += 15) {
            float rad = a * 0.0174533f;
            bool card = (a % 90 == 0);
            gfx_line(ccx + (int)((card ? 46 : 49) * cosf(rad)),
                     ccy + (int)((card ? 36 : 39) * sinf(rad)),
                     ccx + (int)(55 * cosf(rad)),
                     ccy + (int)(45 * sinf(rad)),
                     card ? p->text : p->border);
        }
        if (p->shadow) ell(ccx, ccy + 34, 34, 5, p->surface2);
        {
            const img_t *sp = img_get(cockpit_car_name(h,
                p == &PAL_SUN || p == &PAL_SOLAR));
            if (sp) gfx_blit(90, 131, 96, 78, sp->px);
            else {
                gfx_rect(100, 141, 76, 58, p->surface2);
                gfx_rect_outline(100, 141, 76, 58, p->border);
            }
        }
        if (sn->valid && sn->drift) {
            float dr = (-h + 35.0f) * 0.0174533f;
            int ax = ccx + (int)(15 * sinf(dr)), ay = ccy - (int)(15 * cosf(dr));
            gfx_line(ccx, ccy, ax, ay, p->warn);
        }
        /* FIXED letters drawn last over ring */
        gfx_rect(132, 119, 12, 9, p->bg);
        gfx_text_small(135, 120, "N", p->text);
        gfx_rect(70, 166, 12, 9, p->bg);
        gfx_text_small(73, 167, "W", p->arc);
        gfx_rect(198, 166, 12, 9, p->bg);
        gfx_text_small(201, 167, "E", p->accent);
        gfx_rect(132, 209, 12, 9, p->bg);
        gfx_text_small(135, 210, "S", p->text);
        ui_clip_reset();

        /* LFT / RGT flank rows */
        {
            int l = (g.dist_avg[0] == 65535) ? -1 : (int)g.dist_avg[0];
            int r = (g.dist_avg[2] == 65535) ? -1 : (int)g.dist_avg[2];
            uint16_t lc = us_zone_col(p, l, l >= 0, now);
            uint16_t rc = us_zone_col(p, r, r >= 0, now);
            ui_clip_set(0, 160, 80, 18);
            arc_seg(14, 169, 9, 100, 260, lc);
            gfx_line(4, 169, 9, 165, p->text);
            gfx_line(4, 169, 9, 173, p->text);
            if (l >= 0) snprintf(b, sizeof(b), "LFT:%dcm", l);
            else snprintf(b, sizeof(b), "LFT:--");
            gfx_text_small(24, 166, b, lc);
            ui_clip_set(198, 160, 86, 18);
            if (r >= 0) snprintf(b, sizeof(b), "RGT:%dcm", r);
            else snprintf(b, sizeof(b), "RGT:--");
            gfx_text_small(198, 166, b, rc);
            arc_seg(268, 169, 9, -80, 80, rc);
            gfx_line(280, 169, 275, 165, p->text);
            gfx_line(280, 169, 275, 173, p->text);
            ui_clip_reset();
        }

        /* FRONT: slim side vertical bar (no big text) */
        ui_clip_set(286, 119, 34, 102);
        {
            int f = (g.dist_avg[1] == 65535) ? -1 : (int)g.dist_avg[1];
            uint16_t c = us_zone_col(p, f, f >= 0, now);
            gfx_text_small(290, 121, "FRT", p->muted);
            int n = 0;
            if (f >= 0) n = (f > 60) ? 5 : (f > 45) ? 4 : (f > 30) ? 3 : (f >= 25) ? 2 : 1;
            for (int i = 0; i < 5; i++) {
                int sy = 132 + i * 12;
                bool lit = (i >= 5 - n) && f >= 0 &&
                           !(f < 25 && ((now / 250) & 1) == 0);
                if (lit) gfx_rect(293, sy, 12, 10, c);
                else gfx_rect_outline(293, sy, 12, 10, f < 0 ? p->grey : p->border);
            }
            if (f >= 0) snprintf(b, sizeof(b), "%d", f);
            else snprintf(b, sizeof(b), "--");
            gfx_text_small(290, 196, b, c);
            gfx_text_small(290, 205, "cm", p->muted);
        }
        ui_clip_reset();
    }

    /* ================= FOOTER (221-240) ================= */
    {
        ui_clip_set(0, 221, 320, 19);
        gfx_hline(0, 221, 320, p->border);
        /* transient box steals MENU slot when active */
        bool drew = false;
        if (!drew && sn->valid && (sn->airborne || sn->landing_until > now)) {
            if (sn->airborne) snprintf(b, sizeof(b), "AIRBORNE %ums", (unsigned)sn->airtime_ms);
            else snprintf(b, sizeof(b), "%s", sn->landing_msg);
            gfx_text_center_box(70, 224, 180, b, UI_FONT_MEDIUM, p->warn);
            drew = true;
        }
        if (!drew && ctx->mic_vu_until > now && mic_ready()) {
            uint16_t vu = mic_vu();
            gfx_text_small(76, 227, "VU", p->muted);
            gfx_rect(102, 229, 118, 6, p->surface2);
            int fill = 118 * vu / 100;
            if (fill > 2) gfx_rect(103, 230, fill - 2, 4, p->arc);
            drew = true;
        }
        if (!drew && ctx->toast_until > now && ctx->toast[0]) {
            gfx_text_center_box(70, 224, 180, ctx->toast, UI_FONT_MEDIUM, p->text);
            drew = true;
        }
        if (!drew) {
            gfx_text_small(141, 227, "= MENU", p->text);
            disc(160, 237, 2, p->danger);
        }
        gfx_text_small(8, 227, "< PAGE", p->muted);
        gfx_text_small(262, 227, "ENTER >", p->muted);
        ui_clip_reset();
    }

    /* GEAR POPUP (LB/RB paddle): large center G1-G5 / MAX / MIN for 1 s.
       Layer: above cockpit art, below E-stop/safety (those draw later). */
    if (ctx->gear_flash && now < ctx->gear_flash_until && ctx->gear_pop[0]) {
        ui_clip_set(100, 96, 120, 48);
        gfx_rect(100, 96, 120, 48, p->surface);
        gfx_rect_outline(100, 96, 120, 48, p->accent);
        gfx_text_center_box(100, 108, 120, ctx->gear_pop, UI_FONT_LARGE, p->text);
        ui_clip_reset();
    }

    ui_clip_reset();
    ui_mark_dirty(0, 0, 320, 240);
}


/* ------------------------- VIEW 2: compact (2 m) ------------------------ */
static void draw_compact(os_ctx_t *ctx, const cockpit_pal_t *p,
                         const imu_adv_snap_t *sn, uint32_t now)
{
    (void)ctx;
    char b[32];
    draw_status_bar(p, ctx, sn, now);
    /* 3 zone dots */
    int ds[3] = { (g.dist_avg[0] == 65535) ? -1 : (int)g.dist_avg[0],
                  (g.dist_avg[1] == 65535) ? -1 : (int)g.dist_avg[1],
                  (g.dist_avg[2] == 65535) ? -1 : (int)g.dist_avg[2] };
    static const char *dn[3] = { "L", "F", "R" };
    for (int i = 0; i < 3; i++) {
        int x = 60 + i * 100, y = 60;
        uint16_t c = us_zone_col(p, ds[i], ds[i] >= 0, now);
        disc(x, y, 20, c);
        disc(x, y, 20, c);
        gfx_text_large(x - 5, y - 8, dn[i], p->bg == PAL_SUN.bg ? p->surface : p->surface);
        if (ds[i] >= 0) snprintf(b, sizeof(b), "%d", ds[i]);
        else snprintf(b, sizeof(b), "--");
        gfx_text_center_box(x - 40, y + 26, 80, b, UI_FONT_MEDIUM, p->text);
    }
    /* big speed */
    int spd = (abs(g.cur_l) + abs(g.cur_r)) / 2;
    gfx_number_7seg(160 - 45, 120, spd, 3, 5,
                    p->text, p->surface);
    /* tilt pill */
    float tilt = sn->valid ?
        (fabsf(sn->pitch_deg) > fabsf(sn->roll_deg) ? fabsf(sn->pitch_deg) : fabsf(sn->roll_deg)) : 0;
    uint16_t tc = sn->valid ? tilt_zone_col(p, tilt, now) : p->grey;
    if (sn->valid) snprintf(b, sizeof(b), "TILT %.0f", (double)tilt);
    else snprintf(b, sizeof(b), "TILT --");
    gfx_rect(100, 190, 120, 20, tc);
    gfx_text_center_box(100, 195, 120, b, UI_FONT_SMALL,
                        p->bg == PAL_SUN.bg ? p->surface : p->surface);
    if (sn->valid && sn->airborne) {
        snprintf(b, sizeof(b), "AIRBORNE %ums", (unsigned)sn->airtime_ms);
        gfx_text_center_box(0, 100, 320, b, UI_FONT_MEDIUM, p->warn);
    }
    ui_mark_dirty_full();
}

/* --------------------- VIEW 3: sensor data + terrain -------------------- */
static void draw_sensor(os_ctx_t *ctx, const cockpit_pal_t *p,
                        const imu_adv_snap_t *sn, uint32_t now)
{
    char b[64];
    /* G-trace ring (local, sampled per frame ~30 Hz) */
    static float gtrace[96];
    static uint8_t gt_idx, gt_n;
    static uint32_t gt_last;
    if (now - gt_last >= 33 && sn->valid) {
        gt_last = now;
        gtrace[gt_idx] = sn->g_mag;
        gt_idx = (gt_idx + 1) & 95;
        if (gt_n < 96) gt_n++;
    }

    draw_status_bar(p, ctx, sn, now);
    int y = 30;
    imu_data_t raw;
    bool rok = motion_radar_imu_read(&raw);
    snprintf(b, sizeof(b), "L %d  F %d  R %d cm",
             g.dist_avg[0] == 65535 ? -1 : (int)g.dist_avg[0],
             g.dist_avg[1] == 65535 ? -1 : (int)g.dist_avg[1],
             g.dist_avg[2] == 65535 ? -1 : (int)g.dist_avg[2]);
    gfx_text_small(10, y, b, p->text); y += 14;
    if (rok && sn->valid) {
        snprintf(b, sizeof(b), "P %+5.1f R %+5.1f HD %+6.1f REL",
                 (double)sn->pitch_deg, (double)sn->roll_deg, (double)sn->heading_rel);
        gfx_text_small(10, y, b, p->text); y += 14;
        snprintf(b, sizeof(b), "YR %+6.1f ACC %+.2f %+.2f %+.2f",
                 (double)sn->yaw_rate_dps, (double)raw.accel_xg,
                 (double)raw.accel_yg, (double)raw.accel_zg);
        gfx_text_small(10, y, b, p->text); y += 14;
        snprintf(b, sizeof(b), "GY %+6.1f %+6.1f %+6.1f T %dC",
                 (double)raw.gyro_xdps, (double)raw.gyro_ydps, (double)raw.gyro_zdps,
                 (int)raw.temp_c);
        gfx_text_small(10, y, b, p->text); y += 14;
    } else {
        gfx_text_small(10, y, "IMU NO LINK", p->danger); y += 14;
    }
    int batt = ui_cockpit_battery_pct();
    if (batt < 0) snprintf(b, sizeof(b), "PAD BAT --  SCORE %u %c",
                           (unsigned)sn->drive_score, sn->drive_grade);
    else snprintf(b, sizeof(b), "PAD BAT %d%%  SCORE %u %c",
                  batt, (unsigned)sn->drive_score, sn->drive_grade);
    gfx_text_small(10, y, b, p->text); y += 16;

    /* terrain bar (Sensor sub-view only) */
    {
        gfx_text_small(10, y, "TERRAIN", p->muted);
        const char *tn = "SMOOTH";
        uint16_t tc = p->ok;
        if (sn->terrain == IMU_TERRAIN_ROUGH) { tn = "ROUGH"; tc = p->danger; }
        else if (sn->terrain == IMU_TERRAIN_MODERATE) { tn = "MODERATE"; tc = p->warn; }
        int bw = 180;
        gfx_rect(80, y, bw, 12, p->surface2);
        gfx_rect_outline(80, y, bw, 12, p->border);
        int fill = (sn->terrain == IMU_TERRAIN_SMOOTH) ? bw / 3 :
                   (sn->terrain == IMU_TERRAIN_MODERATE) ? bw * 2 / 3 : bw;
        if (sn->valid && fill > 0) gfx_rect(81, y + 1, fill - 2, 10, tc);
        gfx_text_small(264, y + 2, tn, tc);
        y += 16;
    }

    /* G-trace mini graph */
    {
        int gx = 10, gy = y, gw = 300, gh = 52;
        gfx_rect(gx, gy, gw, gh, p->surface);
        gfx_rect_outline(gx, gy, gw, gh, p->border);
        gfx_text_small(gx + 4, gy + 2, "G-TRACE", p->muted);
        if (gt_n > 1) {
            for (int i = 0; i < gw - 4 && i < gt_n; i++) {
                int idx = (gt_idx + 96 - gt_n + i) & 95;
                float v = gtrace[idx];           /* 0..4 g -> pixels */
                if (v < 0) v = 0;
                if (v > 4) v = 4;
                int py = gy + gh - 3 - (int)(v * (gh - 10) / 4.0f);
                uint16_t gc = (v < 1.2f) ? p->arc : (v < 2.0f) ? p->warn : p->danger;
                gfx_px(gx + 2 + i, py, gc);
            }
        }
        y += gh + 4;
    }
    /* traction + shield + drift row */
    {
        const char *t = "GRIP";
        if (sn->traction == IMU_TRACTION_STUCK) t = "STUCK";
        else if (sn->traction == IMU_TRACTION_SLIPPING) t = "SLIPPING";
        snprintf(b, sizeof(b), "%s %s DRIFT:%s", t,
                 sn->drift ? "DRIFT" : "no-drift",
                 sn->drift ? "ON" : "OFF");
        gfx_text_small(10, y, b, p->text);
    }
    ui_mark_dirty_full();
}

/* ------------------------- VIEW 4: night HUD ---------------------------- */
static void draw_night(os_ctx_t *ctx, const cockpit_pal_t *p,
                       const imu_adv_snap_t *sn, uint32_t now)
{
    char b[32];
    (void)ctx;
    /* minimal: no status clutter, night palette forced by caller */
    gfx_text_small(10, 8, "NIGHT", p->muted);
    int batt = ui_cockpit_battery_pct();
    if (batt < 0) snprintf(b, sizeof(b), "--%%");
    else snprintf(b, sizeof(b), "%d%%", batt);
    gfx_text_small(280, 8, b, p->text);

    int spd = (abs(g.cur_l) + abs(g.cur_r)) / 2;
    gfx_number_7seg(160 - 45, 60, spd, 3, 5, p->text, p->bg);
    gfx_text_center_box(0, 130, 320, "POWER", UI_FONT_SMALL, p->muted);

    int f = (g.dist_avg[1] == 65535) ? -1 : (int)g.dist_avg[1];
    if (f >= 0) snprintf(b, sizeof(b), "F %dcm", f);
    else snprintf(b, sizeof(b), "F --");
    gfx_text_center_box(0, 150, 320, b, UI_FONT_MEDIUM,
                        f < 0 ? p->grey : us_zone_col(p, f, true, now));
    /* heading arrow only */
    if (sn->valid) {
        float rad = (-sn->heading_rel) * 0.0174533f;
        int ax = 160 + (int)(30 * sinf(rad)), ay = 200 - (int)(30 * cosf(rad));
        gfx_line(160, 200, ax, ay, p->arc);
        disc(ax, ay, 3, p->arc);
    }
    if (sn->valid && sn->airborne) {
        snprintf(b, sizeof(b), "AIRBORNE %ums", (unsigned)sn->airtime_ms);
        gfx_text_center_box(0, 100, 320, b, UI_FONT_SMALL, p->warn);
    }
    ui_mark_dirty_full();
}

/* --------------------------------- entry -------------------------------- */
void ui_cockpit_draw(os_ctx_t *ctx, uint32_t now, bool skip_alert)
{
    imu_adv_snap_t sn;
    imu_adv_snapshot(&sn);

    uint8_t view = ctx->drive_sub_view % COCKPIT_VIEW_COUNT;
    uint8_t theme = ctx->cockpit_theme % COCKPIT_THEME_COUNT;
    if (view == COCKPIT_VIEW_NIGHT) theme = COCKPIT_THEME_NIGHT;  /* forced */
    const cockpit_pal_t *p = pal_for(theme);

    /* NIGHT view keeps its wallpaper backdrop; FULL draws its own scene,
       COMPACT/SENSOR self-clear. */
    if (view == COCKPIT_VIEW_NIGHT) {
        const img_t *wall = img_get("wall_night");
        if (wall) img_blit_full(wall);
        else gfx_clear(p->bg);
    }

    switch (view) {
    case COCKPIT_VIEW_COMPACT: draw_compact(ctx, p, &sn, now); break;
    case COCKPIT_VIEW_SENSOR: draw_sensor(ctx, p, &sn, now); break;
    case COCKPIT_VIEW_NIGHT: draw_night(ctx, &PAL_NIGHT, &sn, now); break;
    default: draw_full(ctx, p, &sn, now); break;
    }

    /* bottom bar: view + theme + REL tag */
    {
        char left[48], right[24];
        snprintf(left, sizeof(left), "BACK:VIEW %s", ui_cockpit_view_name(view));
        snprintf(right, sizeof(right), "%s", ui_cockpit_theme_name(theme));
        gfx_rect(0, 220, 320, 20, p->surface);
        gfx_hline(0, 220, 320, p->border);
        gfx_text_small(8, 226, left, p->muted);
        int w = 0;
        const char *rr = right;
        while (*rr++) w++;
        gfx_text_small(320 - 8 - w * 6, 226, right, p->muted);
    }

    /* toast (L2); ALERTS move to the L5 safety overlay (PART 10.1, drawn
       after sheets) unless the caller keeps legacy in-place alerts. */
    if (!skip_alert && ctx->alert_active) {
        ui_cockpit_dim_rect(0, 0, 320, 220);   /* dims cockpit 40% behind it */
        bool pulse = ((now / 600) & 1) != 0;
        int wx = 56, wy = 102, ww = 208, wh = 44;
        gfx_rect(wx, wy, ww, wh, p->surface2);
        gfx_rect_outline(wx, wy, ww, wh, pulse ? p->danger : p->border);
        gfx_text_center_box(wx, wy + 15, ww, ctx->alert_message,
                            UI_FONT_MEDIUM, p->text);
        ui_mark_dirty(wx, wy, ww, wh);
    }
    if (ctx->toast_until > now && ctx->toast[0]) {
        int tw = 260, tx = (320 - tw) / 2, ty = 150, th = 24;
        gfx_rect(tx, ty, tw, th, p->surface2);
        gfx_rect_outline(tx, ty, tw, th, p->accent);
        gfx_text_center_box(tx, ty + 5, tw, ctx->toast, UI_FONT_SMALL, p->text);
        ui_mark_dirty(tx, ty, tw, th);
    }
}
