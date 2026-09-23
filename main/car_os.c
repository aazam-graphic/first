/*
 * car_os.c - Car OS core: state machine, input routing, throttled draw calls.
 * Car control logic (car.c) is never blocked; the OS only observes g.* state
 * and renders UI. Games park the car (motors 0) and own the buttons.
 */
#include "car_os.h"
#include "car_games.h"
#include "os_screens_tft.h"
#include "os_oled.h"
#include "os_assets.h"
#include "input_events.h"
#include "alexa_bridge.h"
#include "tft_display.h"
#include "car_global.h"
#include "imu_driver.h"
#include "imu_adv.h"
#include "ui_drive_cockpit.h"
#include "ui_motion_radar.h"  /* PART 12: BACK 1.5s radar-full */
#include "snd_bank.h"
#include "notif.h"
#include "os_gfx.h"   /* PART 10: thumbnail capture */
#include "os_theme.h" /* PART 12: radar-full clear color */
#include "alexa_bridge.h"   /* PART 10: QCC profile dropdown */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "car_os";

/* ambient LED static color (defined in car.c) */
extern uint8_t g_static_r, g_static_g, g_static_b;

/* ambient picker palette (premium guide 5.2) - shared by picker overlay and
   the Y-tap preset cycle. Index order = picker order. */
const uint8_t os_ambient_palette[6][3] = {
    {255, 0,   0},    /* 0 RED    */
    {0,   255, 0},    /* 1 GREEN  */
    {0,   0,   255},  /* 2 BLUE   */
    {255, 255, 0},    /* 3 YELLOW */
    {255, 0,   255},  /* 4 MAGENTA*/
    {255, 255, 255},  /* 5 WHITE  */
};
const char *os_ambient_name(uint8_t idx)
{
    static const char *names[6] = { "RED", "GREEN", "BLUE", "YELLOW", "MAGENTA", "WHITE" };
    return names[idx & 5];
}

int16_t *os_h_spd   = NULL;
int16_t *os_h_front = NULL;
int16_t *os_h_yaw   = NULL;

static os_ctx_t *s_ctx = NULL;
static uint16_t  s_hist_idx = 0;
static uint32_t  s_hist_last = 0;
static uint16_t  s_hist_n = OS_HIST_N;   /* active ring size (480 or 64 fallback) */

/* perf counters (guide 9) */
static os_perf_t g_perf;
static uint32_t  s_perf_win_start = 0, s_perf_win_frames = 0;
static uint32_t  s_perf_last_log = 0;

const os_perf_t *os_perf(void) { return &g_perf; }

static uint32_t os_now(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }


static void go(os_ctx_t *ctx, os_state_t st, uint32_t now);  /* fwd decl */
static void settings_save_all(os_ctx_t *ctx);               /* fwd decl */

/* PART 10 window stack (max depth 3): push the state being left when both
   sides are menu windows; cleared on DRIVE/GAME/BOOT fresh contexts. */
void os_win_push(os_ctx_t *ctx, os_state_t st)
{
    if (ctx->win_depth >= 3) {
        ctx->win_st[0] = ctx->win_st[1];
        ctx->win_st[1] = ctx->win_st[2];
        ctx->win_depth = 2;
    }
    ctx->win_st[ctx->win_depth++] = st;
}

bool os_win_pop(os_ctx_t *ctx, uint32_t now)
{
    if (!ctx->win_depth) {
        if (ctx->state != OS_HOME) go(ctx, OS_HOME, now);
        return false;
    }
    os_state_t st = ctx->win_st[--ctx->win_depth];
    go(ctx, st, now);
    return true;
}

static bool is_menu_state(os_state_t st)
{
    switch (st) {
    case OS_HOME:
    case OS_DRIVE_ANALYTICS:
    case OS_DIAG:
    case OS_SETTINGS:
    case OS_OLED_CTRL:
    case OS_GAMES_HUB:
    case OS_ALEXA_LOG:
    case OS_SCORE:
    case OS_TRIP:
    case OS_CONN:
    case OS_NOTIF:
        return true;
    default:
        return false;
    }
}

void os_win_breadcrumb(const os_ctx_t *ctx, char *out, size_t n)
{
    if (!out || !n) return;
    out[0] = 0;
    if (!ctx->win_depth) return;
    /* ASCII only: the 5x7 font has no UTF-8 glyphs */
    size_t pos = 0;
    for (uint8_t i = 0; i < ctx->win_depth; i++) {
        const char *nm = os_state_name(ctx->win_st[i]);
        int w = snprintf(out + pos, n - pos, "%s%s", i ? ">" : "<", nm);
        if (w < 0 || (size_t)w >= n - pos) break;
        pos += (size_t)w;
    }
}

/* PART 10 app-switcher history: last 4 window states (dedupe consecutive).
   Thumbnails captured in os_draw_tft on transitions (fb still shows old). */
#define OS_HIST_WIN 4
static os_state_t s_hist_win[OS_HIST_WIN];
static uint8_t s_hist_win_n;

static void hist_record(os_state_t st)
{
    if (st == OS_BOOT) return;
    if (st >= OS_GAME_1 && st <= OS_GAME_6) return;
    if (s_hist_win_n && s_hist_win[0] == st) return;
    for (int i = OS_HIST_WIN - 1; i > 0; i--) s_hist_win[i] = s_hist_win[i - 1];
    s_hist_win[0] = st;
    if (s_hist_win_n < OS_HIST_WIN) s_hist_win_n++;
}

/* 160x120 RGB565 thumbnails in PSRAM (38KB each) */
#define THUMB_W 160
#define THUMB_H 120
static uint16_t *s_thumbs[OS_HIST_WIN];
static os_state_t s_thumb_st[OS_HIST_WIN];

/* PART 16: preallocate all thumbnail slots at boot (zero runtime malloc) */
static void os_thumbs_init(void)
{
    for (int i = 0; i < OS_HIST_WIN; i++) {
        if (!s_thumbs[i])
            s_thumbs[i] = heap_caps_malloc(THUMB_W * THUMB_H * 2,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
}

void os_thumb_capture(os_state_t st)
{
    uint16_t *fb = gfx_fb();
    if (!fb) return;
    int slot = -1;
    for (int i = 0; i < OS_HIST_WIN; i++)
        if (s_thumb_st[i] == st && s_thumbs[i]) slot = i;
    if (slot < 0) {
        for (int i = 0; i < OS_HIST_WIN; i++)
            if (!s_thumbs[i]) { slot = i; break; }
    }
    if (slot < 0) slot = OS_HIST_WIN - 1;   /* evict oldest */
    if (!s_thumbs[slot]) return;            /* prealloc failed: skip silently */
    for (int y = 0; y < THUMB_H; y++)
        for (int x = 0; x < THUMB_W; x++)
            s_thumbs[slot][y * THUMB_W + x] = fb[(y * 2) * GFX_W + x * 2];
    s_thumb_st[slot] = st;
}

const uint16_t *os_thumb_get(os_state_t st, uint16_t *w, uint16_t *h)
{
    for (int i = 0; i < OS_HIST_WIN; i++) {
        if (s_thumb_st[i] == st && s_thumbs[i]) {
            if (w) *w = THUMB_W;
            if (h) *h = THUMB_H;
            return s_thumbs[i];
        }
    }
    return NULL;
}

/* switcher reads history directly */
int os_hist_win_list(os_state_t *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = s_hist_win_n < max ? s_hist_win_n : max;
    for (int i = 0; i < n; i++) out[i] = s_hist_win[i];
    return n;
}

static void go(os_ctx_t *ctx, os_state_t st, uint32_t now)
{
    if (ctx->state == st) return;
    /* PART 10 window stack: menu-to-menu pushes, fresh contexts clear */
    if (is_menu_state(st) && is_menu_state(ctx->state)) {
        os_win_push(ctx, ctx->state);
    } else if (st == OS_DRIVE_MAIN || st == OS_BOOT || st == OS_HOME ||
               st == OS_STANDBY || st == OS_DRIVE_ARMING ||
               (st >= OS_GAME_1 && st <= OS_GAME_6)) {
        ctx->win_depth = 0;
    }
    if (ctx->state == OS_DRIVE_MAIN && st != OS_DRIVE_MAIN) {
        ctx->radar_full = false;   /* PART 12: radar overlay is drive-local */
        ctx->start_home_at = 0;    /* PART 12: cancel deferred START HOME */
    }
    ctx->prev_state = ctx->state;
    ctx->state = st;
    ctx->state_enter_ms = now;
    /* context transition (guide 3/15): drop every pending semantic event so a
       button used for OS navigation (A/B/X/Y in Settings etc.) can never leak
       into the Drive context (roof toggle, headlight, preview...). */
    input_consume_all();
    /* PART 5: every arrival at HOME is back-navigation */
    if (st == OS_HOME) snd_play("ui_back");
    ESP_LOGI(TAG, "state -> %s", os_state_name(st));
}

/* tile -> state, PART 10 HOME pages (3x3). Page 0: main cards, page 1: more. */
static const os_state_t s_tile_state_p0[9] = {
    OS_DRIVE_MAIN, OS_DRIVE_MAIN, OS_DRIVE_MAIN,   /* Drive Radar Roof */
    OS_ALEXA_LOG,  OS_TRIP,       OS_SETTINGS,     /* Voice Trip Settings */
    OS_GAMES_HUB,  OS_DIAG,       OS_NOTIF,        /* Games Diag Notify */
};
static const os_state_t s_tile_state_p1[9] = {
    OS_OLED_CTRL, OS_DRIVE_ANALYTICS, OS_SCORE,
    OS_CONN,      OS_ALEXA_LOG,       OS_HOME,
    OS_HOME,      OS_HOME,            OS_HOME,      /* spare */
};

static const char *s_names[] = {
    "BOOT", "HOME", "DRIVE", "ANALYTICS", "DIAG", "SETTINGS",
    "OLED CTRL", "GAMES", "LANE RUNNER", "NEON SERPENT", "REFLEX", "MEMORY", "SENSOR",
    "REDLINE OPS", "VOICE LOG",
    "SCORE", "TRIP", "CONNECT", "NOTIFY", "STANDBY", "ARMING"
};
static const char *s_hints[] = {
    "", "A:OPEN B:CLOCK RB:PAGE", "A/B:MENU RS:GEAR", "B:MENU", "L/R:TAB A/B:MENU",
    "UP/DN:SEL A:APPLY B:BACK ST:SAVE X:JUMP", "UP/DN:LAYOUT A:APPLY B:MENU",
    "UP/DN:SEL A:PLAY B:MENU", "B:EXIT", "B:EXIT", "B:EXIT", "B:EXIT", "B:EXIT",
    "B:EXIT", "B:BACK",
    "B:BACK", "A:RESET B:BACK", "B:BACK", "UP/DN:SCROLL B:BACK", "START:MENU", "WAIT..."
};

const char *os_state_name(os_state_t st)
{
    return (st >= 0 && st <= OS_DRIVE_ARMING) ? s_names[st] : "?";
}

const char *os_state_hint(os_state_t st)
{
    return (st >= 0 && st <= OS_DRIVE_ARMING) ? s_hints[st] : "";
}

/* ---- Alexa bridge entry points (car_task context only) ------------------ */
os_ctx_t *alexa_os_ctx(void) { return s_ctx; }

void os_request_screen(os_ctx_t *ctx, int st, uint32_t now)
{
    if (!ctx || st < OS_BOOT || st > OS_DRIVE_ARMING) return;
    if (g.estop) return;
    go(ctx, (os_state_t)st, now);
}

void os_alexa_save_settings(os_ctx_t *ctx)
{
    if (!ctx) return;
    settings_save_all(ctx);
}

/* unified input context (premium guide §3): single source of truth.
   car.c derives motor safety + button capture from this - no separate
   ui_capture/menu_active flags can disagree with it. */
input_context_t os_context(void)
{
    if (!s_ctx) return INPUT_CTX_DRIVE;          /* pre-OS boot: drive allowed */
    if (g.estop) return INPUT_CTX_ESTOP;         /* highest priority            */
    switch (s_ctx->state) {
    case OS_BOOT:                            return INPUT_CTX_BOOT;
    case OS_DRIVE_MAIN:
        return (s_ctx->quick_open || s_ctx->roof_panel || s_ctx->picker_open)
                   ? INPUT_CTX_OS : INPUT_CTX_DRIVE;
    case OS_GAME_1: case OS_GAME_2: case OS_GAME_3:
    case OS_GAME_4: case OS_GAME_5: case OS_GAME_6:
        return INPUT_CTX_GAME;
    case OS_STANDBY:                       return INPUT_CTX_STANDBY;
    case OS_DRIVE_ARMING:                   return INPUT_CTX_ARMING;
    default:                                 return INPUT_CTX_OS;
    }
}

bool os_input_captured(void) { return os_context() != INPUT_CTX_DRIVE; }
bool os_parked(void)         { return os_context() != INPUT_CTX_DRIVE; }

bool os_game_active(void)
{
    return s_ctx && s_ctx->state >= OS_GAME_1 && s_ctx->state <= OS_GAME_6;
}

/* --------------------------------- init ---------------------------------- */
void os_init(os_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = OS_BOOT;
    ctx->prev_state = OS_BOOT;
    ctx->state_enter_ms = os_now();
    ctx->home_sel = 0;
    ctx->oled_sel = 0;
    ctx->game_sel = 0;
    ctx->settings_sel = 0;
    ctx->diag_tab = 0;
    ctx->oled_layout = 0;
    snprintf(ctx->oled_text, sizeof(ctx->oled_text), "CAR OS READY");
    ctx->drive_sub_view = 0;    /* VIEW 1 COCKPIT default (1.md 3.4) */
    ctx->cockpit_theme = 1;     /* SOLAR LIGHT default outdoor (1.md 3.5) */

    /* Analytics history (guide 2.5): ONE PSRAM block split into 3 ring buffers.
       If PSRAM is unavailable, retry with a reduced 64-sample block in internal
       RAM - never silently consume scarce internal RAM, log every fallback. */
    ctx->analytics_available = true;
    ctx->analytics_hist_n    = OS_HIST_N;
    if (!os_h_spd && !os_h_front && !os_h_yaw) {
        int16_t *block = (int16_t *)heap_caps_calloc(3, OS_HIST_N * sizeof(int16_t),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (block) {
            os_h_spd   = block;
            os_h_front = block + OS_HIST_N;
            os_h_yaw   = block + OS_HIST_N * 2;
            s_hist_n   = OS_HIST_N;
            ESP_LOGI(TAG, "analytics history: %.1f KB in PSRAM",
                     3.0f * OS_HIST_N * sizeof(int16_t) / 1024.0f);
        } else {
            enum { OS_HIST_N_FALLBACK = 64 };   /* guide 2.5 reduced depth */
            block = (int16_t *)malloc(3 * OS_HIST_N_FALLBACK * sizeof(int16_t));
            if (block) {
                os_h_spd   = block;
                os_h_front = block + OS_HIST_N_FALLBACK;
                os_h_yaw   = block + OS_HIST_N_FALLBACK * 2;
                s_hist_n   = OS_HIST_N_FALLBACK;
                ctx->analytics_hist_n = OS_HIST_N_FALLBACK;
                ESP_LOGW(TAG, "analytics history reduced to %d samples (internal RAM)",
                         OS_HIST_N_FALLBACK);
            } else {
                ctx->analytics_available = false;
                ESP_LOGE(TAG, "analytics history allocation failed; disabled");
            }
        }
    }

    s_ctx = ctx;
    os_assets_init();
    os_thumbs_init();   /* PART 16: thumbnail slots once at boot */
    car_games_all_init();
    ESP_LOGI(TAG, "Car OS init done (boot @%u)", (unsigned)ctx->state_enter_ms);
}

/* ------------------------------ settings --------------------------------- */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* snapshot live settings into the editable draft (guide 5.6) */
static void settings_draft_load(os_ctx_t *ctx)
{
    ctx->draft.spd_cap     = car_get_setting(0);
    ctx->draft.engine_vol  = car_get_setting(1);
    ctx->draft.obstacle_cm = car_get_setting(2);
    ctx->draft.led_bright  = car_get_setting(3);
    ctx->draft.mist_max    = car_get_setting(4);
    ctx->draft.tft_bright  = car_get_setting(5);
    for (int i = 0; i < 5; i++) ctx->draft.gear_caps[i] = car_get_gear_cap(i);
    ctx->draft.oled_layout = ctx->oled_layout;
    ctx->draft.hud_layout  = ctx->hud_layout;
}

/* true if draft differs from live settings (drives B discard prompt) */
static bool settings_dirty_check(const os_ctx_t *ctx)
{
    if (car_get_setting(0) != ctx->draft.spd_cap)     return true;
    if (car_get_setting(1) != ctx->draft.engine_vol)  return true;
    if (car_get_setting(2) != ctx->draft.obstacle_cm) return true;
    if (car_get_setting(3) != ctx->draft.led_bright)  return true;
    if (car_get_setting(4) != ctx->draft.mist_max)    return true;
    if (car_get_setting(5) != ctx->draft.tft_bright)  return true;
    for (int i = 0; i < 5; i++)
        if (car_get_gear_cap(i) != ctx->draft.gear_caps[i]) return true;
    if (ctx->oled_layout != ctx->draft.oled_layout)   return true;
    if (ctx->hud_layout  != ctx->draft.hud_layout)    return true;
    return false;
}

/* adjust the selected draft item (used by tap + hold-repeat) */
static void settings_adjust(os_ctx_t *ctx, int dir)
{
    os_settings_draft_t *d = &ctx->draft;
    uint8_t i = ctx->settings_sel;
    switch (i) {
    case 0: d->spd_cap     = (uint8_t)clampi(d->spd_cap     + dir * 5, 0, 100); break;
    case 1: d->engine_vol  = (uint8_t)clampi(d->engine_vol  + dir * 5, 0, 100); break;
    case 2: d->obstacle_cm = (uint8_t)clampi(d->obstacle_cm + dir * 5, 10, 80); break;
    case 3: d->led_bright  = (uint8_t)clampi(d->led_bright  + dir * 5, 10, 100); break;
    case 4: case 5: case 6: case 7: case 8:
        d->gear_caps[i - 4] = (uint8_t)clampi(d->gear_caps[i - 4] + dir * 5, 10, 100);
        break;
    case 9:
        d->oled_layout = (uint8_t)((d->oled_layout + 4 + (dir != 0 ? dir : 1)) % 4);
        break;
    case 10:                                /* HUD layout (guide 11 Display) */
        d->hud_layout = (uint8_t)((d->hud_layout + 3 + (dir != 0 ? dir : 1)) % 3);
        break;
    case 11:                                /* mist max run x10s, 0 = unlim */
        d->mist_max = (uint8_t)clampi(d->mist_max + dir, 0, 12);
        break;
    case 12:                                /* TFT backlight % - LIVE apply */
        d->tft_bright = (uint8_t)clampi(d->tft_bright + dir * 5, 0, 100);
        car_set_setting(5, d->tft_bright);   /* instant hardware effect */
        break;
    default: break;   /* 13 = SAVE handled on A/START */
    }
    ctx->settings_dirty = settings_dirty_check(ctx);
}

/* A on an item: apply its draft value to the live setting */
static void settings_apply_item(os_ctx_t *ctx, uint8_t i)
{
    switch (i) {
    case 0: car_set_setting(0, ctx->draft.spd_cap); break;
    case 1: car_set_setting(1, ctx->draft.engine_vol); break;
    case 2: car_set_setting(2, ctx->draft.obstacle_cm); break;
    case 3: car_set_setting(3, ctx->draft.led_bright); break;
    case 4: case 5: case 6: case 7: case 8:
        car_set_gear_cap(i - 4, ctx->draft.gear_caps[i - 4]);
        break;
    case 9: ctx->oled_layout = ctx->draft.oled_layout; break;
    case 10: ctx->hud_layout = ctx->draft.hud_layout; break;
    case 11: car_set_setting(4, ctx->draft.mist_max); break;
    case 12: car_set_setting(5, ctx->draft.tft_bright); break;
    }
    ctx->settings_dirty = settings_dirty_check(ctx);
}

/* START / SAVE item: apply all + persist to NVS */
static void settings_save_all(os_ctx_t *ctx)
{
    car_set_setting(0, ctx->draft.spd_cap);
    car_set_setting(1, ctx->draft.engine_vol);
    car_set_setting(2, ctx->draft.obstacle_cm);
    car_set_setting(3, ctx->draft.led_bright);
    for (int i = 0; i < 5; i++) car_set_gear_cap(i, ctx->draft.gear_caps[i]);
    ctx->oled_layout = ctx->draft.oled_layout;
    ctx->hud_layout  = ctx->draft.hud_layout;
    car_set_setting(4, ctx->draft.mist_max);
    car_set_setting(5, ctx->draft.tft_bright);
    car_settings_save();
    ctx->settings_dirty = false;
}

/* quick overlay (guide 5.9): mode cycler - AUTO entry mirrors car.c X-tap init.
   Caller must refuse while the car is still moving. */
/* (quick_mode_step removed in PART 12: mode changes live in X-hold AUTO
   and LS-click crawl paths; QCC no longer carries a MODE row) */

/* ------------------------------ input ------------------------------------ */
void os_handle_input(os_ctx_t *ctx, const xbox360_pad_t *pad, uint16_t dig, uint16_t tap, uint32_t now)
{
    /* E-stop hard lock (premium guide §15): while latched, the OS ignores all
       navigation input. GUIDE tap clears the latch in car.c's central router
       (safety priority 1) - no other path may exit E-stop. */
    if (g.estop) return;

    /* Alexa confirm dialog (A/B) eats input on ANY screen while open */
    if (alexa_confirm_handle_input(dig, tap, now)) return;

    /* global: BACK+START = HOME shortcut.
       Armed latch (guide 2.8): triggers once per press, re-arms only after at
       least one button is released -> no auto-retrigger while held. */
    static bool        s_gs_armed = true;
    static uint32_t    s_gs_t = 0;
    static uint32_t    s_rb_t0 = 0;      /* BACK+START reboot-hold stamp */
    static uint8_t     s_rb_blips = 0;   /* reboot countdown blips fired */
    /* (PART 12 deferred START-tap uses ctx->start_home_at) */
    bool bs_held = (dig & B_BACK) && (dig & B_START);
    if (bs_held) {
        if (s_gs_armed && now - s_gs_t > 800) {
            s_gs_armed = false;
            s_gs_t = now;
            /* START+BACK = HOME (instant — koi heavy work yahan nahi,
               warna car_task block ho jata = lag). */
            if (ctx->state != OS_HOME) go(ctx, OS_HOME, now);
            car_sfx_click();
            return;
        }
        /* BACK+START held 3 s while PARKED = ESP restart (safe shutdown).
           Timer runs ONLY while continuously parked: holding it in DRIVE
           does nothing (no auto-HOME shortcut into a reboot). Countdown
           blips at 1 s / 2 s, restart at 3 s. */
        if (!os_parked()) { s_rb_t0 = 0; s_rb_blips = 0; }
        else {
            if (!s_rb_t0) { s_rb_t0 = now; s_rb_blips = 0; }
            uint32_t held = now - s_rb_t0;
            if (held >= 1000 && s_rb_blips < 1) { s_rb_blips = 1; car_sfx_blip(500); }
            if (held >= 2000 && s_rb_blips < 2) { s_rb_blips = 2; car_sfx_blip(500); }
            if (held >= 3000) {
                s_rb_t0 = 0; s_rb_blips = 0;
                snd_play("sys_warning");
                car_system_restart();   /* never returns */
                return;
            }
        }
    } else {
        s_gs_armed = true;
        s_gs_t = 0;
        s_rb_t0 = 0;
        s_rb_blips = 0;
    }

    /* TURBO (= A button, raw 0x1000) held 5 s while PARKED (standby/menus,
       never games, never DRIVE) = ESP restart. Tap stays headlight/confirm,
       short hold stays mist/panel — only a deliberate 5 s hold reboots.
       Blips at 3 s / 4 s warn before the 5 s restart. */
    {
        static uint32_t s_tb_t0 = 0;
        static uint8_t s_tb_blips = 0;
        input_context_t ictx = os_context();
        bool allow = (ictx == INPUT_CTX_STANDBY || ictx == INPUT_CTX_OS ||
                      ictx == INPUT_CTX_BOOT) && !g.estop;
        if (allow && input_pressed(B_A)) {
            if (!s_tb_t0) { s_tb_t0 = now; s_tb_blips = 0; }
            uint32_t held = now - s_tb_t0;
            if (held >= 3000 && s_tb_blips < 1) { s_tb_blips = 1; car_sfx_blip(500); }
            if (held >= 4000 && s_tb_blips < 2) { s_tb_blips = 2; car_sfx_blip(500); }
            if (held >= 5000) {
                s_tb_t0 = 0; s_tb_blips = 0;
                snd_play("sys_warning");
                car_system_restart();   /* never returns */
                return;
            }
        } else {
            s_tb_t0 = 0;
            s_tb_blips = 0;
        }
    }

    /* PART 10.6 app switcher (START double-tap, DRIVE + HOME only) */
    if ((ctx->state == OS_DRIVE_MAIN || ctx->state == OS_HOME) && ev_double(B_START)) {
        if (!ctx->switch_open) {
            ctx->switch_open = true;
            ctx->switch_sel = 0;
            ctx->quick_open = false;
            ctx->start_home_at = 0;   /* cancel deferred tap-HOME */
            car_sfx_blip(1200);
            input_consume(B_START);
            return;
        }
    }
    if (ctx->switch_open) {
        os_state_t hist[4];
        int n = os_hist_win_list(hist, 4);
        if (n) {
            if (tap & (B_DUP | B_DLEFT)) { ctx->switch_sel = (uint8_t)((ctx->switch_sel + n - 1) % n); car_sfx_blip(900); }
            if (tap & (B_DDOWN | B_DRIGHT)) { ctx->switch_sel = (uint8_t)((ctx->switch_sel + 1) % n); car_sfx_blip(900); }
            if (tap & B_A) {
                os_state_t tgt = hist[ctx->switch_sel % n];
                ctx->switch_open = false;
                car_sfx_click();
                go(ctx, tgt, now);
                input_consume(B_A);
                return;
            }
        }
        if ((tap & (B_B | B_START)) || ev_tap(B_BACK)) {
            ctx->switch_open = false;
            car_sfx_click();
            input_consume(B_B);
            return;
        }
        return;   /* switcher owns all input while open */
    }

    /* PART 12.3 BACK = home in menus (HOME itself goes to Notif, DRIVE
       cycles views, games go to hub - handled in their own cases).
       Dialogs/sheets open = BACK cancels those first (their cases run). */
    if (ev_tap(B_BACK) && is_menu_state(ctx->state) && ctx->state != OS_HOME &&
        !ctx->settings_confirm && !ctx->settings_jump && !ctx->diag_item &&
        !ctx->quick_open && !ctx->roof_panel && !ctx->picker_open &&
        !ctx->switch_open && !ctx->auto_prev && !alexa_overlay_active()) {
        if (ctx->state == OS_SETTINGS && ctx->settings_dirty) {
            /* unsaved edits: open the discard dialog instead of leaving */
            ctx->settings_confirm = 1;
            car_sfx_blip(300);
            snd_play("ui_error");
        } else {
            car_sfx_click();
            go(ctx, OS_HOME, now);
        }
        input_consume(B_BACK);
        return;
    }

    /* settings hold-repeat for L/R adjust (guide 2.7): first action immediate,
       next repeat after 350ms, then every 90ms. Direction change restarts the
       window; releasing resets the state. Runs even when tap==0 (held).
       Disabled while the discard-changes dialog is open. */
    if (ctx->state == OS_SETTINGS && !ctx->settings_confirm) {
        static uint32_t s_rep_next = 0;
        static int      s_rep_dir = 0;
        int adj = (dig & B_DRIGHT) ? 1 : (dig & B_DLEFT) ? -1 : 0;
        if (adj) {
            if (s_rep_dir != adj) {
                s_rep_dir = adj;
                s_rep_next = now + 350;
                settings_adjust(ctx, adj);
            } else if ((int32_t)(now - s_rep_next) >= 0) {
                settings_adjust(ctx, adj);
                s_rep_next += 90;
                if ((int32_t)(now - s_rep_next) > 200) s_rep_next = now + 90;
            }
        } else {
            s_rep_dir = 0;
        }
    }


    switch (ctx->state) {
    case OS_BOOT:
        break;

    case OS_STANDBY: {
        /* CLOCK_IDLE: START opens HOME. A/B/Y/D-pad are LIVE for lights
           only (A tap = headlight, A hold = mist, B tap = roof,
           Y tap = ambient, D-pad = indicators). Everything else is an
           explicit no-op. GUIDE bypasses everything in car.c. Motors/horn/
           engine are hard-blocked (parked gating + standby-entry all-off
           in car.c); MPU + ultrasonic sampling pauses in idle. */
        if (ev_tap(B_START)) {
            car_sfx_click();
            input_consume(B_START);
            go(ctx, OS_HOME, now);
            break;
        }
        /* A tap = headlight (tap only — hold/mist stays dead in idle) */
        if (ev_tap(B_A)) {
            g.headlight = !g.headlight;
            car_sfx_click();
            snd_play(g.headlight ? "ui_toggle_on" : "ui_toggle_off");
            input_consume(B_A);
        }
        /* A hold 700ms = mist toggle (idle gets its mist option) */
        if (ev_hold(B_A, IE_STD_HOLD_MS)) {
            g.mist = !g.mist;
            input_consume(B_A);
            car_sfx_blip(900);
            snd_play(g.mist ? "ui_toggle_on" : "ui_toggle_off");
        }
        /* B tap = roof on/off (tap only — panel stays shut in idle) */
        if (ev_tap(B_B)) {
            bool on = !roof_light_state()->enabled;
            roof_light_set_enabled(on);
            car_sfx_click();
            snd_play(on ? "ui_toggle_on" : "ui_toggle_off");
            input_consume(B_B);
        }
        /* Y tap = ambient preset step (tap only, no rumble in idle) */
        if (ev_tap(B_Y)) {
            static uint8_t s_idl_rgb = 0;
            const uint8_t cols[6][3] = {{255,0,0},{0,255,0},{0,0,255},{255,255,0},{255,0,255},{255,255,255}};
            extern uint8_t g_static_r, g_static_g, g_static_b;
            if (g.led_mode == 0) { g.led_mode = 1; s_idl_rgb = 0; }
            else { s_idl_rgb = (uint8_t)((s_idl_rgb + 1) % 6); }
            g_static_r = cols[s_idl_rgb][0];
            g_static_g = cols[s_idl_rgb][1];
            g_static_b = cols[s_idl_rgb][2];
            car_sfx_click();
            input_consume(B_Y);
        }
        /* D-pad = indicators/hazard only (DOWN/gripper stays dead) */
        if (tap & B_DUP)    { g.hazard = !g.hazard; car_sfx_blip(1200); }
        if (tap & B_DLEFT)  { g.sig_l = !g.sig_l;  g.sig_r = false; car_sfx_blip(1000); }
        if (tap & B_DRIGHT) { g.sig_r = !g.sig_r;  g.sig_l = false; car_sfx_blip(1000); }
        if (tap & (B_X | B_LB | B_RB | B_BACK | B_DDOWN |
                   B_LS | B_RS)) {
            input_consume(B_X); input_consume(B_LB);
            input_consume(B_RB); input_consume(B_BACK);
            input_consume(B_DDOWN);
            input_consume(B_LS); input_consume(B_RS);
        }
        break;
    }

    case OS_DRIVE_ARMING: {
        /* Pre-drive stage: boot anim + chime play, sticks dead, motors 0.
           Only B cancels back to HOME. Entry to DRIVE happens in car.c
           after ~1.3 s AND 250 ms neutral (never from here directly). */
        if (tap & B_B) {
            car_sfx_click();
            input_consume(B_B);
            go(ctx, OS_HOME, now);
            break;
        }
        if (tap & (B_A | B_X | B_Y | B_LB | B_RB | B_BACK | B_START |
                   B_DUP | B_DDOWN | B_DLEFT | B_DRIGHT | B_LS | B_RS)) {
            input_consume(B_A); input_consume(B_X);
            input_consume(B_Y); input_consume(B_LB);
            input_consume(B_RB); input_consume(B_BACK);
            input_consume(B_START);
            input_consume(B_DUP); input_consume(B_DDOWN);
            input_consume(B_DLEFT); input_consume(B_DRIGHT);
            input_consume(B_LS); input_consume(B_RS);
        }
        break;
    }

    case OS_HOME: {                    /* PART 10: 3x3 widget cards, 2 pages */
        uint8_t row = ctx->home_sel / 3, col = ctx->home_sel % 3;
        if (tap & B_DUP)    { row = (row + 2) % 3; ctx->home_sel = row * 3 + col; car_sfx_blip(900); snd_play("ui_nav"); }
        if (tap & B_DDOWN)  { row = (row + 1) % 3; ctx->home_sel = row * 3 + col; car_sfx_blip(900); snd_play("ui_nav"); }
        if (tap & B_DLEFT)  { ctx->home_sel = row * 3 + (col + 2) % 3; car_sfx_blip(900); snd_play("ui_nav"); }
        if (tap & B_DRIGHT) { ctx->home_sel = row * 3 + (col + 1) % 3; car_sfx_blip(900); snd_play("ui_nav"); }
        if (tap & B_RB)     { ctx->home_page ^= 1; ctx->home_sel = 0; car_sfx_blip(1100); break; }
        if (tap & B_LB)     { ctx->home_page ^= 1; ctx->home_sel = 0; car_sfx_blip(1100); break; }
        if (ev_tap(B_BACK)) { go(ctx, OS_NOTIF, now); break; }   /* PART 10.3 */
        if (tap & B_A) {
            const os_state_t *map = ctx->home_page ? s_tile_state_p1 : s_tile_state_p0;
            os_state_t tgt = map[ctx->home_sel];
            /* page-1 spare tiles are disabled, except sel 8 = PARK clock */
            if (ctx->home_page && ctx->home_sel == 8) {
                car_sfx_click();
                go(ctx, OS_STANDBY, now);
                break;
            }
            if (ctx->home_page && ctx->home_sel >= 5) { car_sfx_blip(300); break; }
            car_sfx_click();
            if (tgt == OS_DRIVE_MAIN) {
                if (!ctx->home_page && ctx->home_sel == 1)
                    ctx->drive_sub_view = 0;   /* Radar card: full cockpit */
                if (!ctx->home_page && ctx->home_sel == 2)
                    ctx->roof_panel = true;    /* Roof card: open sheet */
                /* DRIVE tile enters ARMING (1.3 s anim+chime+neutral gate),
                   never straight into live DRIVE. Startup sound once. */
                snd_play("sys_boot_chime");
                go(ctx, OS_DRIVE_ARMING, now);
                break;
            }
            go(ctx, tgt, now);
        }
        /* B climbs the window stack; at HOME root it returns to CLOCK_IDLE
           (motors lock) — never hardcoded to Drive HUD. */
        if (tap & B_B) { os_win_pop(ctx, now); go(ctx, OS_STANDBY, now); }
        break;
    }

    case OS_DRIVE_MAIN:
        /* ambient color picker (premium guide 5.2): Y hold opens; car parks */
        if (ctx->picker_open) {
            extern const uint8_t os_ambient_palette[6][3];
            if (tap & B_DLEFT)  { ctx->picker_sel = (uint8_t)((ctx->picker_sel + 5) % 6); car_sfx_blip(900); }
            if (tap & B_DRIGHT) { ctx->picker_sel = (uint8_t)((ctx->picker_sel + 1) % 6); car_sfx_blip(900); }
            /* live preview while open: selection updates the static color */
            g.led_mode  = 1;
            g_static_r  = os_ambient_palette[ctx->picker_sel][0];
            g_static_g  = os_ambient_palette[ctx->picker_sel][1];
            g_static_b  = os_ambient_palette[ctx->picker_sel][2];
            if (tap & B_A) {                    /* apply + close */
                ctx->picker_open = false;
                car_sfx_score();
                input_consume(B_A);
            } else if (tap & B_B) {             /* cancel: restore previous */
                g.led_mode = ctx->picker_prev_mode;
                g_static_r = ctx->picker_prev_r;
                g_static_g = ctx->picker_prev_g;
                g_static_b = ctx->picker_prev_b;
                ctx->picker_open = false;
                car_sfx_click();
                input_consume(B_B);
            } else if (tap & B_START) {         /* keep preview state + close */
                ctx->picker_open = false;
                car_sfx_click();
                input_consume(B_START);
            }
            break;
        }
        /* PART 7 roof bottom sheet: rows MODE / COLOR / BRIGHTNESS */
        if (ctx->roof_panel) {
            uint8_t b = roof_light_state()->brightness;
            uint8_t c = roof_light_state()->color_idx;
            if (tap & B_DUP)   { ctx->roof_row = (uint8_t)((ctx->roof_row + 2) % 3); car_sfx_blip(900); }
            if (tap & B_DDOWN) { ctx->roof_row = (uint8_t)((ctx->roof_row + 1) % 3); car_sfx_blip(900); }
            if (ctx->roof_row == 0) {         /* MODE */
                if (tap & B_DLEFT)  { roof_light_cycle_mode(-1); car_sfx_blip(900); snd_play("roof_mode_cycle"); }
                if (tap & B_DRIGHT) { roof_light_cycle_mode(1); car_sfx_blip(900); snd_play("roof_mode_cycle"); }
            } else if (ctx->roof_row == 1) {  /* COLOR (steady only) */
                if (tap & B_DLEFT)  { roof_light_set_color_idx((uint8_t)(c + ROOF_COLOR_COUNT - 1)); car_sfx_blip(900); }
                if (tap & B_DRIGHT) { roof_light_set_color_idx((uint8_t)(c + 1)); car_sfx_blip(900); }
            } else {                          /* BRIGHTNESS */
                if (tap & B_DLEFT)  { b = (b < 10) ? 0 : (uint8_t)(b - 10); roof_light_set_brightness(b); car_sfx_blip(700); }
                if (tap & B_DRIGHT) { b = (b > 90) ? 100 : (uint8_t)(b + 10); roof_light_set_brightness(b); car_sfx_blip(700); }
            }
            /* Right stick X = fine brightness ±1% (guide 6.3), rate-limited */
            if (pad && (abs(pad->rx) > 6000)) {
                static uint32_t s_rs_next = 0;
                if ((int32_t)(now - s_rs_next) >= 0) {
                    int nb = (int)b + (pad->rx > 0 ? 1 : -1);
                    if (nb < 0) nb = 0;
                    if (nb > 100) nb = 100;
                    roof_light_set_brightness((uint8_t)nb);
                    s_rs_next = now + 70;
                }
            }
            if (tap & B_A) { ctx->roof_panel = false; roof_light_set_enabled(true); car_settings_save(); car_sfx_score(); snd_play("ui_toggle_on"); input_consume(B_A); break; }  /* apply + close + persist */
            if (tap & B_B)    { ctx->roof_panel = false; roof_light_set_mode(ctx->roof_prev_mode); roof_light_set_brightness(ctx->roof_prev_br); roof_light_set_color_idx(ctx->roof_prev_col); car_sfx_click(); snd_play("ui_close_sheet"); input_consume(B_B); break; } /* cancel */
            if (tap & B_START){ ctx->roof_panel = false; car_sfx_click(); snd_play("ui_close_sheet"); input_consume(B_START); break; }  /* keep current */
            break;
        }
        if (ctx->quick_open) {              /* PART 10.4 Quick Control Center */
            if (tap & B_START)  { ctx->quick_open = false; car_sfx_click(); snd_play("ui_close_sheet"); input_consume(B_START); break; }   /* close */
            if (tap & B_B)      { ctx->quick_open = false; car_sfx_click(); snd_play("ui_close_sheet"); input_consume(B_B); break; }
            if (tap & B_DUP)   { ctx->quick_row = (uint8_t)((ctx->quick_row + 5) % 6); car_sfx_blip(900); }
            if (tap & B_DDOWN) { ctx->quick_row = (uint8_t)((ctx->quick_row + 1) % 6); car_sfx_blip(900); }
            switch (ctx->quick_row) {
            case 0: {                       /* 4 chips: HEAD/HAZARD/ROOF/MUTE */
                if (tap & B_DLEFT)  { ctx->quick_col = (uint8_t)((ctx->quick_col + 3) % 4); car_sfx_blip(900); }
                if (tap & B_DRIGHT) { ctx->quick_col = (uint8_t)((ctx->quick_col + 1) % 4); car_sfx_blip(900); }
                if (tap & B_A) {
                    bool now_on = false;
                    switch (ctx->quick_col) {
                    case 0: g.headlight = !g.headlight; now_on = g.headlight; break;
                    case 1: g.hazard = !g.hazard; now_on = g.hazard; break;
                    case 2:
                        if (!roof_light_state()->enabled) {
                            if (roof_light_state()->mode == ROOF_LIGHT_OFF)
                                roof_light_set_mode(ROOF_LIGHT_POLICE);
                            roof_light_set_enabled(true);
                            now_on = true;
                            snd_play("roof_police_on");
                        } else {
                            roof_light_set_enabled(false);
                        }
                        break;
                    default: g.snd_mute = !g.snd_mute; now_on = !g.snd_mute; break;
                    }
                    car_sfx_click();
                    snd_play(now_on ? "ui_toggle_on" : "ui_toggle_off");
                }
                break;
            }
            case 1: {                       /* BRIGHTNESS (LED) slider */
                uint8_t v = car_get_setting(3);
                if (tap & B_DLEFT)  { v = (v > 10) ? (uint8_t)(v - 10) : 10; car_set_setting(3, v); car_sfx_blip(700); }
                if (tap & (B_DRIGHT | B_A)) { v = (v < 90) ? (uint8_t)(v + 10) : 100; car_set_setting(3, v); car_sfx_blip(700); }
                break;
            }
            case 2: {                       /* VOLUME slider */
                uint8_t v = car_get_setting(1);
                if (tap & B_DLEFT)  { v = (v > 5) ? (uint8_t)(v - 5) : 0; car_set_setting(1, v); car_sfx_blip(700); }
                if (tap & (B_DRIGHT | B_A)) { v = (v < 95) ? (uint8_t)(v + 5) : 100; car_set_setting(1, v); car_sfx_blip(700); }
                break;
            }
            case 3:                         /* THEME 1-tap cycle */
                if (tap & (B_DLEFT | B_DRIGHT | B_A)) {
                    ui_cockpit_next_theme(ctx);
                    car_sfx_blip(900);
                }
                break;
            case 4: {                       /* PROFILE dropdown */
                static uint8_t s_qprof = 1;
                if (tap & B_DLEFT)  { s_qprof = (uint8_t)((s_qprof + 7) % 8); car_sfx_blip(900); }
                if (tap & B_DRIGHT) { s_qprof = (uint8_t)((s_qprof + 1) % 8); car_sfx_blip(900); }
                if (tap & B_A) {
                    if (alexa_request_profile(s_qprof)) car_sfx_score();
                    else { car_sfx_blip(250); snd_play("gear_limit"); }
                }
                break;
            }
            default:                        /* EXIT TO HOME */
                if (tap & B_A) {
                    ctx->quick_open = false;
                    car_sfx_click();
                    go(ctx, OS_HOME, now);
                }
                break;
            }
            break;
        }
        /* ---- Drive Cockpit: BACK tap cycles views / radar exit, BACK holds
           set Night HUD / radar-full, RS-hold calibrates. B (tap/double/
           hold) belongs to the roof light in car.c - never hijack it here. */
        if (ev_hold(B_RS, 1000)) {   /* PART 12: MPU CAL + heading zero */
            imu_driver_cal_start();
            imu_adv_reset_heading();
            snprintf(ctx->toast, sizeof(ctx->toast), "CAL + ZERO");
            ctx->toast_until = now + 1200;
            car_sfx_click();
            input_consume(B_RS);
        }
        /* PART 12 BACK: tap = Quick Control Center instant toggle,
           double-tap = view cycle (sheet auto-closes, or exit radar-full),
           hold 700ms = Night HUD, hold 1.5s = Motion Radar full. NOTE:
           ev_hold shares one fired-flag per key, so the 1.5s arm uses
           ev_held + own latch. */
        static bool s_b15 = false;
        if (!input_pressed(B_BACK)) s_b15 = false;
        if (ev_double(B_BACK)) {
            if (ctx->radar_full) {
                ctx->radar_full = false;
                car_sfx_click();
            } else {
                if (ctx->quick_open) {
                    ctx->quick_open = false;
                    snd_play("ui_close_sheet");
                }
                ui_cockpit_next_view(ctx);
                snprintf(ctx->toast, sizeof(ctx->toast), "%s",
                         ui_cockpit_view_name(ctx->drive_sub_view));
                ctx->toast_until = now + 1200;
                car_sfx_blip(800);
            }
            input_consume(B_BACK);
            break;
        }
        if (ev_tap(B_BACK)) {
            if (ctx->radar_full) {
                ctx->radar_full = false;
                car_sfx_click();
            } else if (ctx->quick_open) {
                ctx->quick_open = false;
                car_sfx_click();
                snd_play("ui_close_sheet");
            } else {
                ctx->quick_open = true;
                ctx->quick_row = 0;
                car_sfx_blip(1200);
                snd_play("ui_open_sheet");
            }
            input_consume(B_BACK);
            break;
        }
        if (!s_b15 && ev_held(B_BACK, 1500)) {
            s_b15 = true;
            ctx->radar_full = true;
            snprintf(ctx->toast, sizeof(ctx->toast), "RADAR");
            ctx->toast_until = now + 1200;
            car_sfx_blip(1000);
            input_consume(B_BACK);
            break;
        }
        if (ev_hold(B_BACK, IE_STD_HOLD_MS)) {
            ctx->radar_full = false;
            ctx->drive_sub_view = COCKPIT_VIEW_NIGHT;
            snprintf(ctx->toast, sizeof(ctx->toast), "NIGHT HUD");
            ctx->toast_until = now + 1200;
            car_sfx_blip(1000);
            input_consume(B_BACK);
            break;
        }
        /* B (tap/double/hold) belongs to the roof light in DRIVE context
           (car.c drive controls) - never hijack it here, else roof breaks.
           HOME is reachable via START-hold. */
        /* PART 12 START: tap = HOME, hold 700ms = Quick Control Center.
           (Overlays above consume START first: picker/roof/switcher.)
           Tap is deferred ~350ms so a START double-tap still opens the
           App Switcher instead of stranding the user on HOME. */
        if (ev_tap(B_START)) {
            if (ctx->quick_open) {
                ctx->quick_open = false;
                car_sfx_click();
                snd_play("ui_close_sheet");
                ctx->start_home_at = 0;
            } else {
                ctx->start_home_at = now + IE_MAX_TAP_MS + 20;
            }
            input_consume(B_START);
            break;
        }
        if (ctx->start_home_at && !ctx->switch_open &&
            (int32_t)(now - ctx->start_home_at) >= 0) {
            ctx->start_home_at = 0;
            car_sfx_click();
            go(ctx, OS_HOME, now);
            break;
        }
        if (ev_hold(B_START, IE_STD_HOLD_MS)) {
            if (!ctx->quick_open) {
                ctx->start_home_at = 0;
                ctx->quick_open = true;
                ctx->quick_row = 0;
                ctx->quick_col = 0;
                car_sfx_blip(1200);
                snd_play("ui_open_sheet");
            }
            input_consume(B_START);
            break;
        }
        break;

    case OS_DRIVE_ANALYTICS:

    case OS_DIAG:                        /* L/R (or LB/RB) switches diagnostics tab */
        /* LB/RB tab switching (premium guide 7) */
        if (tap & B_RB) { ctx->diag_tab = (uint8_t)((ctx->diag_tab + 1) % 5); ctx->diag_item = 0; ctx->sys_hold_t0 = 0; car_sfx_blip(900); }
        if (tap & B_LB) { ctx->diag_tab = (uint8_t)((ctx->diag_tab + 4) % 5); ctx->diag_item = 0; ctx->sys_hold_t0 = 0; car_sfx_blip(900); }
        /* SYSTEM tab hosts the dangerous actions (premium guide 4/11):
           RESTART and FACTORY RESET, both requiring A hold 1.5 s. */
        if (ctx->diag_tab == 0 && ctx->diag_item) {
            if (tap & B_DUP)   { ctx->diag_item = (uint8_t)(ctx->diag_item == 1 ? 2 : 1); car_sfx_blip(900); }
            if (tap & B_DDOWN) { ctx->diag_item = (uint8_t)(ctx->diag_item == 1 ? 2 : 1); car_sfx_blip(900); }
            if (tap & B_B)     { ctx->diag_item = 0; ctx->sys_hold_t0 = 0; car_sfx_click(); break; }
            if (dig & B_A) {
                if (ctx->sys_hold_t0 == 0) ctx->sys_hold_t0 = now;
                if (now - ctx->sys_hold_t0 >= 1500) {   /* dangerous-confirm timing */
                    ctx->sys_hold_t0 = 0;
                    if (ctx->diag_item == 1) {
                        car_system_restart();           /* never returns */
                    } else {
                        car_settings_factory_reset();
                        car_system_restart();           /* reboot into defaults */
                    }
                }
            } else {
                ctx->sys_hold_t0 = 0;
            }
            if (tap & B_A) input_consume(B_A);   /* no Home exit while item focused */
            if (tap & B_B) input_consume(B_B);
            break;
        }
        ctx->diag_item   = 0;
        ctx->sys_hold_t0 = 0;
        if (tap & (B_A | B_B)) go(ctx, OS_HOME, now);
        if (tap & B_DLEFT)  { ctx->diag_tab = (uint8_t)((ctx->diag_tab + 4) % 5); car_sfx_blip(900); }
        if (tap & B_DRIGHT) { ctx->diag_tab = (uint8_t)((ctx->diag_tab + 1) % 5); car_sfx_blip(900); }
        if (ctx->diag_tab == 0 && tap & (B_DUP | B_DDOWN)) { ctx->diag_item = 1; car_sfx_blip(900); }
        break;

    case OS_SETTINGS:                  /* draft + A/B/START model (guide 5.6) */
        if (ctx->settings_jump) {     /* PART 10.7 jump-list overlay */
            if (tap & B_DUP)   { ctx->jump_sel = (uint8_t)((ctx->jump_sel + 6) % 7); car_sfx_blip(900); }
            if (tap & B_DDOWN) { ctx->jump_sel = (uint8_t)((ctx->jump_sel + 1) % 7); car_sfx_blip(900); }
            if (tap & B_A) {
                static const uint8_t first[7] = { 0, 1, 2, 3, 4, 9, 10 };
                ctx->settings_sel = first[ctx->jump_sel];
                ctx->settings_jump = false;
                car_sfx_click();
                input_consume(B_A);
            }
            if ((tap & B_B) || ev_tap(B_X) || ev_tap(B_BACK)) {
                ctx->settings_jump = false;
                car_sfx_click();
                input_consume(B_B);
                input_consume(B_X);
                input_consume(B_BACK);
            }
            break;
        }
        if (ev_tap(B_X)) {
            ctx->settings_jump = true;
            ctx->jump_sel = 0;
            car_sfx_blip(1200);
            input_consume(B_X);
            break;
        }
        if (ctx->settings_confirm) {    /* discard-changes dialog open */
            if (tap & B_A) {            /* confirm: reload + exit */
                settings_draft_load(ctx);
                ctx->settings_dirty = false;
                ctx->settings_confirm = 0;
                car_sfx_click();
                go(ctx, OS_HOME, now);
            } else if (tap & B_B) {     /* cancel: stay in settings */
                ctx->settings_confirm = 0;
                car_sfx_click();
            }
            break;
        }
        if (tap & B_DUP)   { ctx->settings_sel = (uint8_t)((ctx->settings_sel + SET_ITEM_COUNT - 1) % SET_ITEM_COUNT); car_sfx_blip(900); }
        if (tap & B_DDOWN) { ctx->settings_sel = (uint8_t)((ctx->settings_sel + 1) % SET_ITEM_COUNT); car_sfx_blip(900); }
        if (tap & B_A) {
            if (ctx->settings_sel == SET_ITEM_COUNT - 1) {   /* SAVE item */
                settings_save_all(ctx);
                car_sfx_score();
                go(ctx, OS_HOME, now);
            } else {
                settings_apply_item(ctx, ctx->settings_sel);
                car_sfx_click();
                snd_play("ui_toggle_on");
            }
        }
        if (tap & B_START) {                 /* START = save all + exit */
            settings_save_all(ctx);
            car_sfx_score();
            go(ctx, OS_HOME, now);
        }
        if (tap & B_B) {
            if (ctx->settings_dirty) {
                ctx->settings_confirm = 1;
                car_sfx_blip(300);
                snd_play("ui_error");
            } else {
                go(ctx, OS_HOME, now);
            }
        }
        break;

    case OS_OLED_CTRL:                 /* persistent oled_sel (guide 2.2) */
        if (tap & B_DUP)   ctx->oled_sel = (ctx->oled_sel + 3) % 4;
        if (tap & B_DDOWN) ctx->oled_sel = (ctx->oled_sel + 1) % 4;
        if ((tap & (B_DLEFT | B_DRIGHT)) && ctx->oled_sel == 3) {
            static const char *presets[] = { "CAR OS READY", "AZAM ROBOT", "HELLO WORLD", "JAI HIND" };
            static uint8_t pi = 0;
            pi = (uint8_t)((pi + 1) % 4);
            snprintf(ctx->oled_text, sizeof(ctx->oled_text), "%s", presets[pi]);
        }
        if (tap & B_A) {
            ctx->oled_layout = ctx->oled_sel;
            car_sfx_click();
        }
        if (tap & B_B) go(ctx, OS_HOME, now);
        break;

    case OS_ALEXA_LOG:                  /* Alexa voice history */
        if (tap & B_B) go(ctx, OS_HOME, now);
        break;

    case OS_SCORE:                     /* PART 10 Drive Score window */
        if (tap & B_B) { os_win_pop(ctx, now); input_consume(B_B); }
        break;

    case OS_TRIP:                      /* PART 10 Trip computer */
        if (tap & B_A) { trip_reset(); car_sfx_score(); input_consume(B_A); }
        if (tap & B_B) { os_win_pop(ctx, now); input_consume(B_B); }
        break;

    case OS_CONN:                      /* PART 10 Connectivity */
        if (tap & B_B) { os_win_pop(ctx, now); input_consume(B_B); }
        break;

    case OS_NOTIF: {                   /* PART 10.3 Notification Center */
        notif_item_t tmp[24];
        int n = notif_list(tmp, 24);
        if (tap & B_DUP)   { os_notif_scroll(-1, n); car_sfx_blip(900); }
        if (tap & B_DDOWN) { os_notif_scroll(1, n); car_sfx_blip(900); }
        if (tap & B_B) { os_win_pop(ctx, now); input_consume(B_B); }
        break;
    }

    case OS_GAMES_HUB:                 /* persistent game_sel (guide 2.2) */
        if (tap & B_DUP)   { ctx->game_sel = (ctx->game_sel + OS_GAME_COUNT - 1) % OS_GAME_COUNT; car_sfx_blip(900); }
        if (tap & B_DDOWN) { ctx->game_sel = (ctx->game_sel + 1) % OS_GAME_COUNT; car_sfx_blip(900); }
        if (tap & B_A) {
            uint8_t gi = ctx->game_sel;
            if (gi < OS_GAME_COUNT) {
                OS_GAMES[gi].init();
                car_sfx_click();
                go(ctx, (os_state_t)(OS_GAME_1 + gi), now);
            }
        }
        if (tap & B_B) go(ctx, OS_HOME, now);
        break;

    default: /* OS_GAME_* */
        if (tap & B_B) {
            car_sfx_click();
            snd_play("game_over");   /* PART 5: run over on exit */
            go(ctx, OS_GAMES_HUB, now);
        }
        if (ev_tap(B_BACK)) {        /* PART 12.3 BACK = hub in games */
            car_sfx_click();
            go(ctx, OS_GAMES_HUB, now);
            input_consume(B_BACK);
        }
        break;
    }
}

uint16_t os_hist_pos(void) { return s_hist_idx; }
uint16_t os_hist_n(void)  { return s_hist_n;   }

/* ------------------------------ alerts (guide 5.2) ----------------------- */
static const char *s_alert_def[] = {
    "", "E-STOP ACTIVE", "CONTROLLER LOST", "SENSOR FAULT", "FRONT OBSTACLE",
};

void os_alert(os_ctx_t *ctx, os_alert_type_t type, const char *msg, uint32_t now)
{
    if (type == ALERT_NONE) return;
    /* don't let a lower-priority alert override an active one */
    if (ctx->alert_active && ctx->alert_type <= type) return;
    ctx->alert_type     = type;
    ctx->alert_active   = true;
    ctx->alert_start_ms = now;
    snprintf(ctx->alert_message, sizeof(ctx->alert_message), "%s",
             msg ? msg : s_alert_def[type]);
    notif_push(NOTIF_SAFETY, ctx->alert_message, now);   /* PART 10.3 feed */
    ESP_LOGW(TAG, "ALERT: %s", ctx->alert_message);
}

void os_alert_update(os_ctx_t *ctx, uint32_t now)
{
    if (!ctx->alert_active) return;
    /* auto-dismiss after 3 s, except E-STOP which is sticky (guide 5.2) */
    if (ctx->alert_type == ALERT_ESTOP) {
        if (!g.estop) ctx->alert_active = false;    /* cleared with estop */
    } else if (now - ctx->alert_start_ms > 3000) {
        ctx->alert_active = false;
    }
}

/* condition edge-tracker -> alert triggers (called from os_update) */
static void alert_watch(os_ctx_t *ctx, uint32_t now)
{
    static bool s_p_estop = false, s_p_pad = false;
    static bool s_p_fault = false, s_p_obst = false;
    static bool s_init = false;

    bool estop = g.estop;
    bool padok = xbox360_dongle_connected();
    bool fault = !car_us_front_ok();
    uint16_t f = g.dist_avg[1];
    bool obst  = car_us_front_ok() && f != 65535 &&
                 f < car_get_setting(2);

    if (!s_init) {          /* don't fire stale alerts right after boot */
        s_init    = true;
        s_p_estop = estop; s_p_pad = padok;
        s_p_fault = fault; s_p_obst = obst;
        return;
    }

    if (estop && !s_p_estop) os_alert(ctx, ALERT_ESTOP, NULL, now);
    if (!padok && s_p_pad)   os_alert(ctx, ALERT_PAD_LOST, NULL, now);
    if (fault && !s_p_fault) os_alert(ctx, ALERT_SENSOR, NULL, now);
    if (obst && !s_p_obst)   os_alert(ctx, ALERT_OBSTACLE, NULL, now);

    s_p_estop = estop; s_p_pad = padok; s_p_fault = fault; s_p_obst = obst;
    os_alert_update(ctx, now);
}

/* ------------------------------ update ----------------------------------- */
void os_update(os_ctx_t *ctx, uint32_t now)
{
    if (!s_ctx) s_ctx = ctx;

    alexa_apply_pending(ctx, now);   /* validated Alexa action (no-op idle) */

    /* boot splash -> STANDBY clock (car rests locked until START->menu) */
    if (ctx->state == OS_BOOT && now - ctx->state_enter_ms > 1500) {
        go(ctx, OS_STANDBY, now);
        snd_play("sys_ready");   /* PART 5: system ready at boot end */
    }

    /* parked = motors off, effects off (games + launcher menus).
       STANDBY keeps its own mist latch (A-hold idle option) — entry reset
       happens once in car.c, not wiped every frame here. */
    if (os_parked()) {
        g.tgt_l = g.tgt_r = 0;
        if (ctx->state != OS_STANDBY) g.mist = false;
        g.roof_on = false;
        g.braking = false;
        g.reversing = false;
    }

    /* settings: snapshot draft once per entry (guide 5.6) */
    if (ctx->state == OS_SETTINGS) {
        if (!ctx->settings_loaded) {
            settings_draft_load(ctx);
            ctx->settings_loaded = true;
            ctx->settings_dirty = false;
            ctx->settings_confirm = 0;
        }
    } else {
        ctx->settings_loaded = false;
    }

    /* sync UI mirrors */
    ctx->gear = g.gear;
    for (int i = 0; i < 5; i++) ctx->gear_caps[i] = car_get_gear_cap(i);
    ctx->engine_vol = car_get_setting(1);
    ctx->led_bright = car_get_setting(3);
    ctx->obs_cm     = car_get_setting(2);

    /* drive alerts: edge-trigger + 3s auto-dismiss lifecycle (guide 5.2) */
    alert_watch(ctx, now);

    /* MPU-advanced SLOW path (1.md 4.1): 25 Hz UI smoothing (pitch/roll/
       heading). Fast 100 Hz detection runs in imu_adv task. */
    {
        imu_data_t imu;
        if (motion_radar_imu_read(&imu))
            imu_adv_slow_update(imu.pitch_deg, imu.roll_deg, imu.yaw_rate_dps,
                                imu.accel_xg, imu.accel_yg, imu.accel_zg, now);
        imu_adv_set_motion(g.tgt_l, g.tgt_r, g.cur_l, g.cur_r, now);
    }

    /* perf summary log every 5 s, non-blocking (guide 9) */
    if (now - s_perf_last_log >= 5000) {
        s_perf_last_log = now;
        ESP_LOGI(TAG, "PERF: TFT %u FPS (frame %lu us max %lu us, push %lu us max %lu us), skipped %lu, heap %lu KB (min %lu KB), PSRAM %lu KB",
                 (unsigned)g_perf.fps,
                 (unsigned long)g_perf.frame_us, (unsigned long)g_perf.max_frame_us,
                 (unsigned long)g_perf.push_us, (unsigned long)g_perf.max_push_us,
                 (unsigned long)g_perf.skipped_frames,
                 (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned long)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }

    /* analytics history @10Hz */
    if (now - s_hist_last >= 100 && os_h_spd && os_h_front && os_h_yaw) {
        s_hist_last = now;
        int spd = (abs(g.cur_l) + abs(g.cur_r)) / 2;
        /* hold the last valid front sample instead of 0 - a 0 flat-line would
           look like immediate danger on the graph (guide 2.6) */
        static uint16_t s_last_front = 250;
        if (g.dist_avg[1] != 65535) s_last_front = g.dist_avg[1];
        float y = car_get_yaw();
        os_h_spd[s_hist_idx]   = (int16_t)clampi(spd, 0, 100);
        os_h_front[s_hist_idx] = (int16_t)clampi((int)s_last_front, 0, 200);
        os_h_yaw[s_hist_idx]   = (int16_t)clampi((int)y, -180, 180);
        s_hist_idx = (uint16_t)((s_hist_idx + 1) % s_hist_n);
    }
}

/* ------------------------------- draw ------------------------------------ */
void os_draw_tft(os_ctx_t *ctx, uint32_t now)
{
    if (!gfx_fb()) return;
    if (os_game_active()) return;              /* game drew itself */

    /* PART 10.5/10.6: on state change, record history + capture thumbnail
       while the framebuffer still shows the OLD state. */
    static int s_last_seen = -1;
    if ((int)ctx->state != s_last_seen) {
        if (s_last_seen >= 0) {
            os_state_t prev = (os_state_t)s_last_seen;
            hist_record(prev);
            os_thumb_capture(prev);
        }
        s_last_seen = (int)ctx->state;
    }

    /* deadline-based pacing (guide 6.2): no jitter accumulation */
    if ((int32_t)(now - ctx->next_tft_ms) < 0) return;
    ctx->next_tft_ms += 33;                    /* ~30 FPS */
    if ((int32_t)(now - ctx->next_tft_ms) > 100)      /* big jump guard */
        ctx->next_tft_ms = now + 33;

    /* full frame on state entry (guide 6.1 policy) */
    static int s_last_state = -1;
    static bool s_last_qo = false, s_last_al = false;
    if ((int)ctx->state != s_last_state) {
        s_last_state = (int)ctx->state;
        ui_mark_dirty_full();
    }
    if (ctx->quick_open != s_last_qo) {        /* overlay appear/disappear */
        s_last_qo = ctx->quick_open;
        ui_mark_dirty(50, 30, 220, 180);
    }
    if (ctx->alert_active != s_last_al) {      /* alert appear/disappear */
        s_last_al = ctx->alert_active;
        ui_mark_dirty(56, 102, 208, 44);
    }

    int64_t t0 = esp_timer_get_time();

    switch (ctx->state) {
    case OS_BOOT:            ui_mark_dirty_full(); os_scr_boot(ctx, now); break;
    case OS_STANDBY:         os_scr_standby(ctx, now); break;   /* table clock */
    case OS_DRIVE_ARMING: {
        ui_mark_dirty_full();
        os_scr_boot(ctx, now);
        /* arming reason on screen: starting vs waiting-for-neutral */
        if (now - ctx->state_enter_ms >= 1300)
            gfx_text_center_box(0, 224, 320, "CENTER STICKS", UI_FONT_SMALL, UI_WARNING);
        else
            gfx_text_center_box(0, 224, 320, "STARTING...", UI_FONT_SMALL, UI_MUTED);
        break;
    }
    case OS_HOME:            os_scr_home(ctx, now); break;          /* marks cards */
    case OS_DRIVE_MAIN: {
        if (ctx->radar_full) {   /* PART 12 BACK 1.5s Motion Radar full */
            gfx_clear(UI_BG);
            ui_motion_radar_draw(now, false);
            ui_mark_dirty_full();
        } else {
            /* Drive Cockpit (1.md PART 3) + overlays */
            ui_cockpit_draw(ctx, now, true);   /* alerts via L5 safety */
        }
        os_scr_drive_overlays(ctx);
        break;
    }
    case OS_DRIVE_ANALYTICS: ui_mark_dirty(0, 22, 320, 198); os_scr_analytics(ctx, now); break;
    case OS_DIAG:            ui_mark_dirty(0, 22, 320, 198); os_scr_diag(ctx, now); break;
    case OS_SETTINGS:        ui_mark_dirty(0, 22, 320, 198); os_scr_settings(ctx, now); break;
    case OS_OLED_CTRL:       ui_mark_dirty(0, 22, 320, 198); os_scr_oledctrl(ctx, now); break;
    case OS_GAMES_HUB:       ui_mark_dirty(0, 22, 320, 198); os_scr_gameshub(ctx, now); break;
    case OS_ALEXA_LOG:       ui_mark_dirty(0, 22, 320, 198); os_scr_alexa_log(ctx, now); break;
    case OS_SCORE:           ui_mark_dirty(0, 22, 320, 198); os_scr_score(ctx, now); break;
    case OS_TRIP:            ui_mark_dirty(0, 22, 320, 198); os_scr_trip(ctx, now); break;
    case OS_CONN:            ui_mark_dirty(0, 22, 320, 198); os_scr_conn(ctx, now); break;
    case OS_NOTIF:           ui_mark_dirty(0, 22, 320, 198); os_scr_notif(ctx, now); break;
    default: break;                            /* games draw themselves */
    }

    /* Alexa overlays: banner + confirm dialog (self-expiring) */
    bool ov_before = alexa_overlay_active();
    alexa_banner_draw();
    alexa_confirm_draw();
    if (ov_before || alexa_overlay_active()) ui_mark_dirty_full();

    /* PART 10.6 app switcher (L3) + PART 10.1 safety overlay (L5, top) */
    if (ctx->switch_open) {
        os_scr_switcher(ctx, now);
        ui_mark_dirty_full();
    }
    os_scr_safety_overlay(ctx, now);

    if (ui_has_dirty()) {
        /* perf instrumentation (guide 9) */
        uint32_t r_us = (uint32_t)(esp_timer_get_time() - t0);
        g_perf.frame_us = r_us;
        if (r_us > g_perf.max_frame_us) g_perf.max_frame_us = r_us;
        /* FULL-frame push. Region (partial-window) pushes were tested on this
           ST7789/swap_xy panel and produce horizontal corruption regardless of
           DMA sync - so dirty tracking is used only to SKIP unchanged frames
           (guide 6.1 "where practical"). tft_push_fb_region() stays available
           in tft_display.c for future hardware-verified use. */
        gfx_push();
        uint32_t p_us = (uint32_t)(esp_timer_get_time() - t0) - r_us;
        g_perf.push_us = p_us;
        if (p_us > g_perf.max_push_us) g_perf.max_push_us = p_us;
        g_perf.frames++;
        if (s_perf_win_start == 0) s_perf_win_start = now;
        s_perf_win_frames++;
        if (now - s_perf_win_start >= 1000) {
            g_perf.fps = (uint16_t)(s_perf_win_frames * 1000u / (now - s_perf_win_start));
            s_perf_win_start = now;
            s_perf_win_frames = 0;
        }
    } else {
        g_perf.skipped_frames++;               /* nothing changed -> no SPI time */
    }
}

void os_draw_oled(os_ctx_t *ctx, uint32_t now)
{
    os_oled_apply(ctx, now);                   /* internally throttled 80ms */
}

void os_game_frame(os_ctx_t *ctx, const xbox360_pad_t *pad, uint32_t now)
{
    if (!os_game_active() || !gfx_fb()) return;
    static uint32_t s_last = 0;
    if (now - s_last < 33) return;             /* ~30 FPS */
    s_last = now;
    uint8_t gi = (uint8_t)(ctx->state - OS_GAME_1);
    OS_GAMES[gi].update(pad, now);
    OS_GAMES[gi].draw(now);
    gfx_push();
}
