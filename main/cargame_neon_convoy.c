#include "cargame_neon_convoy.h"
#include "snd_bank.h"          /* PART 5: highscore fanfare */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
/* ---- PORT BINDINGS (adapted to Car OS) ---- */
#include "os_gfx.h"
#include "xbox360.h"
/* xbox360pad_t -> xbox360_pad_t alias (source body ise use karta hai) */
typedef xbox360_pad_t xbox360pad_t;
/* Car OS button bit masks (XBOXRECV report layout: buttons>>16) */
#define NCR_BTN_A      (1u << 0)   /* tiles: A=0x1 */
#define NCR_BTN_X      (1u << 2)   /* X=0x4 */
#define NCR_BTN_LB     (1u << 4)
#define NCR_BTN_RB     (1u << 5)
#define NCR_BTN_START  (1u << 6)
#define NCR_BTN_BACK   (1u << 7)
#define NCR_BTN_GUIDE  (1u << 8)

/* ---- TFT primitives (os_gfx framebuffer) ---- */
#define NCR_FB_FILL_SCREEN(c)   gfx_clear((c))
#define NCR_FB_FILL_RECT(x,y,w,h,c) gfx_rect((x),(y),(w),(h),(c))
#define NCR_FB_FILL_RECT_O(x,y,w,h,c) gfx_rect_outline((x),(y),(w),(h),(c))
#define NCR_FB_HLINE(x,y,w,c)   gfx_hline((x),(y),(w),(c))
#define NCR_FB_VLINE(x,y,h,c)   gfx_vline((x),(y),(h),(c))
#define NCR_FB_PX(x,y,c)        gfx_px((x),(y),(c))
#define NCR_FB_LINE(x0,y0,x1,y1,c) gfx_line((x0),(y0),(x1),(y1),(c))
#define NCR_FB_TEXT(x,y,s,c,sc) gfx_text((x),(y),(s),(c),0,(sc))
#define NCR_FB_DRAW_TEXT(x,y,s,c,sc) gfx_text((x),(y),(s),(c),0,(sc))
#define NCR_FB_TEXT_CENTER(y,s,c,sc) gfx_text_center((y),(s),(c),0,(sc))
#define NCR_FONT_W 6
#define NCR_FONT_H 8

/* pad bindings - xbox360_pad_t (Car OS driver) */
#define NCR_PAD_CONNECTED(p)   ((p)->present)
#define NCR_PAD_LX(p)          ((int32_t)(p)->lx)
#define NCR_PAD_LT(p)          ((int32_t)(p)->lt)
#define NCR_PAD_RT(p)          ((int32_t)(p)->rt)
#define NCR_PAD_BTN(p, m)      ((((int32_t)((p)->buttons)) & (m)) != 0)
#define NCR_PAD_EDGE(p, m)     (((p)->present) ? (((int32_t)((p)->buttons)) & (m)) : 0)
#include "os_gfx.h"

/* Optional audio hook. Bind to the existing audio-event API, e.g.
 *   #define NCR_AUDIO_EVENT(e) audio_fx_play(ncr_sfx_map[(e)])
 * Default is a no-op; never invent low-level I2S access here. */
typedef enum {
    NCR_SFX_SHOT = 0, NCR_SFX_HIT, NCR_SFX_EXPLODE, NCR_SFX_PICKUP,
    NCR_SFX_EMP, NCR_SFX_ALERT, NCR_SFX_SELECT, NCR_SFX_WIN, NCR_SFX_LOSE
} ncr_sfx_t;
#ifndef NCR_AUDIO_EVENT
#define NCR_AUDIO_EVENT(evt) do { (void)(evt); } while (0)
#endif

/* =========================================================================
 *  Tunables
 * ========================================================================= */
#define NCR_TICK_MS            33u
#define NCR_SCREEN_W           320
#define NCR_SCREEN_H           240
#define NCR_ROAD_L             40
#define NCR_ROAD_R             280
#define NCR_ROAD_W             (NCR_ROAD_R - NCR_ROAD_L)

#define NCR_FP(v)              ((int32_t)(v) * 16)   /* px  -> fixed */
#define NCR_PX(v)              ((int32_t)(v) / 16)   /* fixed -> px */

#define NCR_PLAYER_W           18
#define NCR_PLAYER_H           22
#define NCR_PLAYER_Y           196
#define NCR_PLAYER_MIN_X       (NCR_ROAD_L + 6)
#define NCR_PLAYER_MAX_X       (NCR_ROAD_R - 6 - NCR_PLAYER_W)

#define NCR_DEADZONE           1800
#define NCR_STICK_SPAN         (32767 - NCR_DEADZONE)
#define NCR_FIRE_CD_MS         110u
#define NCR_TURBO_CD_MS        3000u
#define NCR_EMP_CD_MS          8000u
#define NCR_OVERHEAT_TICKS     36      /* 1.2 s at 30 Hz */
#define NCR_METER_MAX          1000
#define NCR_TURBO_COST         150
#define NCR_EMP_COST           500
#define NCR_EMP_RADIUS_PX      120
#define NCR_WAVE_COUNT         5

/* RGB565 palette */
#define NCR_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define C_BG        NCR_RGB(6, 8, 24)
#define C_ROAD      NCR_RGB(28, 30, 36)
#define C_LANE      NCR_RGB(0, 220, 255)
#define C_LANE_DIM  NCR_RGB(0, 90, 110)
#define C_CYAN      NCR_RGB(0, 255, 255)
#define C_WHITE     0xFFFFu
#define C_BLACK     0x0000u
#define C_RED       NCR_RGB(255, 40, 40)
#define C_DKRED     NCR_RGB(150, 24, 40)
#define C_ORANGE    NCR_RGB(255, 140, 0)
#define C_PURPLE    NCR_RGB(170, 60, 255)
#define C_GREEN     NCR_RGB(40, 255, 90)
#define C_YELLOW    NCR_RGB(255, 230, 0)
#define C_MAGENTA   NCR_RGB(255, 0, 255)
#define C_DKMAG     NCR_RGB(110, 0, 110)
#define C_GRAY      NCR_RGB(90, 90, 100)
#define C_DARK      NCR_RGB(14, 14, 22)
#define C_PANEL     NCR_RGB(10, 12, 30)

/* Per-game button bits (edge tracked locally, reset on every ncr_init). */
#define NCR_IN_A      0x01u
#define NCR_IN_X      0x02u
#define NCR_IN_LB     0x04u
#define NCR_IN_RB     0x08u
#define NCR_IN_START  0x10u

/* =========================================================================
 *  Types
 * ========================================================================= */
typedef enum {
    NCR_STATE_SPLASH = 0,
    NCR_STATE_MENU,
    NCR_STATE_COUNTDOWN,
    NCR_STATE_PLAY,
    NCR_STATE_BOSS_WARNING,
    NCR_STATE_BOSS,
    NCR_STATE_PAUSED,
    NCR_STATE_WIN,
    NCR_STATE_GAME_OVER,
    NCR_STATE_CONTROLLER_LOST
} ncr_state_t;

typedef enum { NCR_WPN_LASER = 0, NCR_WPN_SPREAD, NCR_WPN_MISSILE, NCR_WPN_COUNT } ncr_weapon_t;

typedef enum {
    NCR_EN_BIKE = 0, NCR_EN_JEEP, NCR_EN_DRONE, NCR_EN_MINETRUCK, NCR_EN_TURRET, NCR_EN_BOSS,
    NCR_EN_COUNT
} ncr_enemy_kind_t;

typedef enum { NCR_PK_ARMOR = 0, NCR_PK_SHIELD, NCR_PK_COOLANT, NCR_PK_EMP, NCR_PK_COUNT } ncr_pickup_kind_t;

typedef struct {
    bool     active;
    uint8_t  kind;
    uint8_t  w, h;
    int32_t  x, y;          /* top-left, fixed point */
    int32_t  vx, vy;        /* fixed px per tick */
    int16_t  hp, hp_max;
    uint16_t timer;         /* attack / zig-zag timer */
    uint16_t timer2;        /* burst counter */
    uint8_t  flash;         /* hit-flash ticks */
} ncr_enemy_t;

typedef struct {
    bool    active;
    uint8_t kind;           /* 0 laser, 1 spread, 2 missile */
    uint8_t w, h;
    int16_t dmg;
    int32_t x, y, vx, vy;
} ncr_pbullet_t;

typedef struct {
    bool     active;
    uint8_t  kind;          /* 0 normal, 1 homing, 2 heavy, 3 boss missile */
    uint8_t  w, h;
    uint16_t life;
    int32_t  x, y, vx, vy;
} ncr_ebullet_t;

typedef struct { bool active; uint8_t kind; int32_t x, y; } ncr_pickup_t;
typedef struct { bool active; bool armed; uint8_t fuse; int32_t x, y; } ncr_mine_t;
typedef struct { bool active; uint8_t life; uint16_t color; int32_t x, y, vx, vy; } ncr_particle_t;

typedef struct { uint8_t w, h; int16_t hp; int16_t vy; uint16_t score; } ncr_enemy_def_t;
typedef struct { uint8_t kills_needed; uint8_t max_active; uint8_t spawn_interval; } ncr_wave_def_t;

typedef struct {
    /* state machine */
    ncr_state_t state;
    ncr_state_t resume_state;
    bool        clock_ready;
    uint32_t    last_tick_ms;
    uint32_t    state_ticks;
    uint32_t    tick;
    uint8_t     prev_btn;       /* per-game edge tracking */
    int8_t      shake_dx;
    uint8_t     shake_ticks;
    uint32_t    rng;
    uint32_t    road_scroll;

    /* player */
    int32_t  px, target_px;     /* fixed */
    int8_t   armor;
    int16_t  shield, heat, special;
    bool     shield_on;
    uint8_t  shield_delay;
    uint8_t  cool_delay;
    uint8_t  overheat_ticks;
    uint8_t  turbo_ticks;
    bool     turbo_used, emp_used, fired_once;
    uint32_t turbo_ms, emp_ms, last_fire_ms;
    uint8_t  emp_slow_ticks, emp_flash_ticks;
    uint8_t  invuln_ticks;
    uint8_t  weapon;

    /* score / combo */
    uint32_t score;
    uint16_t kills;
    uint8_t  streak, mult, max_mult;
    uint8_t  combo_ticks, combo_popup_ticks;

    /* waves */
    uint8_t  wave, wave_kills, spawn_ticks, wave_banner_ticks;

    /* boss */
    int8_t   boss_idx;
    uint8_t  boss_phase;
    bool     core_open;
    uint16_t core_ticks;
    uint8_t  boss_atk_ticks, boss_aux_ticks, boss_drone_ticks, boss_msl_ticks, boss_move_ticks;
    int32_t  boss_target_x;
    uint8_t  missile_warn_ticks;
    int16_t  missile_warn_x;
    uint8_t  phase_banner_ticks;

    /* alerts */
    uint8_t  pickup_msg_ticks, pickup_msg_kind;

    /* pools */
    ncr_enemy_t    enemies[NCR_MAX_ENEMIES];
    ncr_pbullet_t  pbullets[NCR_MAX_PLAYER_BULLETS];
    ncr_ebullet_t  ebullets[NCR_MAX_ENEMY_BULLETS];
    ncr_pickup_t   pickups[NCR_MAX_PICKUPS];
    ncr_mine_t     mines[NCR_MAX_MINES];
    ncr_particle_t particles[NCR_MAX_PARTICLES];
} ncr_game_t;

/* =========================================================================
 *  Static data
 * ========================================================================= */
static const char *TAG = "NCR";
static ncr_game_t  g;                 /* single BSS instance (~3 KB) */
static uint32_t    s_session_best;    /* RAM-only best score; survives ncr_init, no NVS */

static const ncr_enemy_def_t k_enemy_def[NCR_EN_COUNT] = {
    /* w   h   hp   vy(fixed) score */
    { 10, 14,   1,  44,  100 },   /* BIKE      */
    { 16, 16,   3,  22,  200 },   /* JEEP      */
    { 14, 10,   2,  18,  250 },   /* DRONE     */
    { 18, 24,   4,  16,  300 },   /* MINETRUCK */
    { 22, 22,   8,   8,  500 },   /* TURRET    */
    { 64, 40, 150,  12, 5000 },   /* BOSS      */
};

static const ncr_wave_def_t k_waves[NCR_WAVE_COUNT] = {
    {  8, 4, 40 },  /* wave 1: bikes                 */
    { 12, 5, 35 },  /* wave 2: bikes + jeeps         */
    { 12, 5, 40 },  /* wave 3: drones + mine trucks  */
    {  6, 3, 60 },  /* wave 4: heavy turrets         */
    { 16, 6, 30 },  /* wave 5: mixed, then boss      */
};

static const char *k_weapon_names[NCR_WPN_COUNT] = { "LASER", "SPREAD", "MISSILE" };
static const char *k_pickup_names[NCR_PK_COUNT]  = { "ARMOR +1", "SHIELD +", "COOLANT", "EMP +" };
static const uint16_t k_pickup_colors[NCR_PK_COUNT] = { C_GREEN, C_CYAN, C_YELLOW, C_MAGENTA };

/* =========================================================================
 *  Prototypes
 * ========================================================================= */
static bool ncr_aabb_overlap(int32_t ax, int32_t ay, int32_t aw, int32_t ah,
                             int32_t bx, int32_t by, int32_t bw, int32_t bh);
static int8_t ncr_spawn_enemy(uint8_t kind, int32_t x_px);
static void ncr_update_player(const xbox360pad_t *pad);
static void ncr_update_weapons(const xbox360pad_t *pad, uint8_t pressed, uint32_t now_ms);
static void ncr_update_enemies(void);
static void ncr_update_bullets(void);
static void ncr_update_pickups(void);
static void ncr_update_boss(void);
static void ncr_resolve_collisions(void);
static void ncr_draw_background(void);
static void ncr_draw_player(void);
static void ncr_draw_enemy(const ncr_enemy_t *e);
static void ncr_draw_hud(void);
static void ncr_draw_overlay(void);

/* =========================================================================
 *  Small helpers
 * ========================================================================= */
static inline int32_t ncr_clamp(int32_t v, int32_t lo, int32_t hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}
static inline int32_t ncr_abs(int32_t v) { return (v < 0) ? -v : v; }

static uint32_t ncr_rand(void)
{
    uint32_t x = g.rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g.rng = x ? x : 0x9E3779B9u;
    return g.rng;
}
static int32_t ncr_rand_range(int32_t lo, int32_t hi)
{
    if (hi <= lo) return lo;
    return lo + (int32_t)(ncr_rand() % (uint32_t)(hi - lo + 1));
}

static void ncr_set_state(ncr_state_t s)
{
    g.state = s;
    g.state_ticks = 0;
}

static void ncr_update_best(void)
{
    /* PART 5: first new session record plays the fanfare once per run */
    static bool s_fanfare_done = false;
    if (g.state == NCR_STATE_SPLASH || g.state == NCR_STATE_MENU) s_fanfare_done = false;
    if (g.score > s_session_best) {
        s_session_best = g.score;
        if (!s_fanfare_done && g.score > 0) {
            s_fanfare_done = true;
            snd_play("highscore_fanfare");
        }
    }
}

static bool ncr_aabb_overlap(int32_t ax, int32_t ay, int32_t aw, int32_t ah,
                             int32_t bx, int32_t by, int32_t bw, int32_t bh)
{
    return (ax < bx + bw) && (ax + aw > bx) && (ay < by + bh) && (ay + ah > by);
}

/* ---- clipped drawing wrappers (all framebuffer access goes through here) ---- */
static void ncr_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    x += g.shake_dx;
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= NCR_SCREEN_W || y >= NCR_SCREEN_H) return;
    if (x + w > NCR_SCREEN_W) w = NCR_SCREEN_W - x;
    if (y + h > NCR_SCREEN_H) h = NCR_SCREEN_H - y;
    if (w <= 0 || h <= 0) return;
    NCR_FB_FILL_RECT((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, c);
}
static void ncr_frame(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    ncr_rect(x, y, w, 1, c);
    ncr_rect(x, y + h - 1, w, 1, c);
    ncr_rect(x, y, 1, h, c);
    ncr_rect(x + w - 1, y, 1, h, c);
}
static void ncr_text(int32_t x, int32_t y, const char *s, uint16_t c, uint8_t scale)
{
    if (!s || !scale) return;
    int32_t w = (int32_t)strlen(s) * NCR_FONT_W * scale;
    int32_t h = NCR_FONT_H * scale;
    x += g.shake_dx;
    if (x + w <= 0 || x >= NCR_SCREEN_W || y + h <= 0 || y >= NCR_SCREEN_H) return;
    NCR_FB_DRAW_TEXT((int16_t)x, (int16_t)y, s, c, scale);
}
static void ncr_text_center(int32_t y, const char *s, uint16_t c, uint8_t scale)
{
    int32_t w = (int32_t)strlen(s) * NCR_FONT_W * scale;
    ncr_text((NCR_SCREEN_W - w) / 2, y, s, c, scale);
}
static void ncr_banner(const char *s, uint16_t fg, uint16_t bg)
{
    int32_t w = (int32_t)strlen(s) * NCR_FONT_W * 2 + 16;
    ncr_rect((NCR_SCREEN_W - w) / 2, 96, w, 24, bg);
    ncr_frame((NCR_SCREEN_W - w) / 2, 96, w, 24, fg);
    ncr_text_center(100, s, fg, 2);
}

/* =========================================================================
 *  Spawners
 * ========================================================================= */
static void ncr_spawn_particles(int32_t cx, int32_t cy, uint8_t n, uint16_t color)
{
    for (int i = 0; i < NCR_MAX_PARTICLES && n > 0; i++) {
        ncr_particle_t *p = &g.particles[i];
        if (p->active) continue;
        p->active = true;
        p->x = cx; p->y = cy;
        p->vx = ncr_rand_range(-40, 40);
        p->vy = ncr_rand_range(-40, 40);
        p->life = (uint8_t)ncr_rand_range(8, 16);
        p->color = color;
        n--;
    }
}

static bool ncr_spawn_pbullet(int32_t x, int32_t y, int32_t vx, int32_t vy,
                              uint8_t kind, int16_t dmg, uint8_t w, uint8_t h)
{
    for (int i = 0; i < NCR_MAX_PLAYER_BULLETS; i++) {
        ncr_pbullet_t *b = &g.pbullets[i];
        if (b->active) continue;
        b->active = true; b->kind = kind; b->dmg = dmg; b->w = w; b->h = h;
        b->x = x; b->y = y; b->vx = vx; b->vy = vy;
        return true;
    }
    return false;
}

static bool ncr_spawn_ebullet(int32_t x, int32_t y, int32_t vx, int32_t vy,
                              uint8_t kind, uint8_t w, uint8_t h)
{
    for (int i = 0; i < NCR_MAX_ENEMY_BULLETS; i++) {
        ncr_ebullet_t *b = &g.ebullets[i];
        if (b->active) continue;
        b->active = true; b->kind = kind; b->w = w; b->h = h; b->life = 220;
        b->x = x; b->y = y; b->vx = vx; b->vy = vy;
        return true;
    }
    return false;
}

static void ncr_spawn_mine(int32_t x, int32_t y)
{
    for (int i = 0; i < NCR_MAX_MINES; i++) {
        ncr_mine_t *m = &g.mines[i];
        if (m->active) continue;
        m->active = true; m->armed = false; m->fuse = 15;
        m->x = ncr_clamp(x, NCR_FP(NCR_ROAD_L + 2), NCR_FP(NCR_ROAD_R - 12));
        m->y = y;
        return;
    }
}

static void ncr_spawn_pickup(int32_t x, int32_t y, uint8_t kind)
{
    if (kind >= NCR_PK_COUNT) return;
    for (int i = 0; i < NCR_MAX_PICKUPS; i++) {
        ncr_pickup_t *p = &g.pickups[i];
        if (p->active) continue;
        p->active = true; p->kind = kind;
        p->x = ncr_clamp(x, NCR_FP(NCR_ROAD_L + 4), NCR_FP(NCR_ROAD_R - 14));
        p->y = y;
        return;
    }
}

static int ncr_active_enemies(void)   /* excludes the boss */
{
    int n = 0;
    for (int i = 0; i < NCR_MAX_ENEMIES; i++)
        if (g.enemies[i].active && g.enemies[i].kind != NCR_EN_BOSS) n++;
    return n;
}

/* Returns slot index or -1. x_px < 0 => choose a fair random lane position. */
static int8_t ncr_spawn_enemy(uint8_t kind, int32_t x_px)
{
    if (kind >= NCR_EN_COUNT) return -1;
    int slot = -1;
    for (int i = 0; i < NCR_MAX_ENEMIES; i++) if (!g.enemies[i].active) { slot = i; break; }
    if (slot < 0) return -1;

    const ncr_enemy_def_t *d = &k_enemy_def[kind];
    const int32_t min_x = NCR_ROAD_L + 6;
    const int32_t max_x = NCR_ROAD_R - 6 - d->w;

    if (x_px < 0) {
        bool ok = false;
        for (int attempt = 0; attempt < 5 && !ok; attempt++) {
            x_px = ncr_rand_range(min_x, max_x);
            ok = true;
            /* safe spawn gap against anything still in the entry strip */
            for (int j = 0; j < NCR_MAX_ENEMIES; j++) {
                const ncr_enemy_t *o = &g.enemies[j];
                if (!o->active || o->y > NCR_FP(60)) continue;
                if (ncr_abs(x_px - NCR_PX(o->x)) < 34) { ok = false; break; }
            }
            /* fairness: slow heavies never spawn straight above the player */
            if (ok && (kind == NCR_EN_TURRET || kind == NCR_EN_MINETRUCK) &&
                ncr_abs(x_px - NCR_PX(g.px)) < 28) ok = false;
        }
        if (!ok) return -1;   /* retry next tick */
    }
    x_px = ncr_clamp(x_px, min_x, max_x);

    ncr_enemy_t *e = &g.enemies[slot];
    memset(e, 0, sizeof(*e));
    e->active = true; e->kind = kind; e->w = d->w; e->h = d->h;
    e->x = NCR_FP(x_px); e->y = NCR_FP(-(int32_t)d->h);
    e->vy = d->vy;
    e->hp = e->hp_max = d->hp;
    e->timer = (uint16_t)ncr_rand_range(20, 50);
    if (kind == NCR_EN_BIKE)  e->vx = (ncr_rand() & 1u) ? 32 : -32;
    if (kind == NCR_EN_DRONE) e->vx = (ncr_rand() & 1u) ? 24 : -24;
    return (int8_t)slot;
}

/* =========================================================================
 *  Damage / scoring
 * ========================================================================= */
static void ncr_damage_player(void)
{
    if (g.invuln_ticks) return;
    int32_t cx = g.px + NCR_FP(NCR_PLAYER_W / 2), cy = NCR_FP(NCR_PLAYER_Y + NCR_PLAYER_H / 2);
    if (g.shield_on && g.shield > 0) {
        g.shield -= 300; if (g.shield < 0) g.shield = 0;
        g.invuln_ticks = 8;
        ncr_spawn_particles(cx, cy, 4, C_CYAN);
        NCR_AUDIO_EVENT(NCR_SFX_HIT);
        return;
    }
    g.armor--;
    g.invuln_ticks = 45;
    g.streak = 0;
    if (g.mult != 1) { g.mult = 1; g.combo_popup_ticks = 40; }
    g.shake_ticks = 8;
    ncr_spawn_particles(cx, cy, 6, C_RED);
    NCR_AUDIO_EVENT(NCR_SFX_HIT);
    if (g.armor <= 0) {
        g.armor = 0;
        ncr_update_best();
        ncr_set_state(NCR_STATE_GAME_OVER);
        NCR_AUDIO_EVENT(NCR_SFX_LOSE);
    }
}

static void ncr_kill_enemy(int i)
{
    ncr_enemy_t *e = &g.enemies[i];
    const ncr_enemy_def_t *d = &k_enemy_def[e->kind < NCR_EN_COUNT ? e->kind : 0];
    int32_t cx = e->x + NCR_FP(e->w / 2), cy = e->y + NCR_FP(e->h / 2);
    e->active = false;
    g.kills++;
    if (e->kind != NCR_EN_BOSS && g.wave_kills < 255) g.wave_kills++;

    if (g.streak < 200) g.streak++;
    uint8_t m = (uint8_t)(1 + g.streak / 3);
    if (m > 8) m = 8;
    if (m != g.mult) { g.mult = m; g.combo_popup_ticks = 40; }
    if (m > g.max_mult) g.max_mult = m;
    g.combo_ticks = 90;

    g.score += (uint32_t)d->score * g.mult;
    g.special += 60; if (g.special > NCR_METER_MAX) g.special = NCR_METER_MAX;
    ncr_spawn_particles(cx, cy, e->kind == NCR_EN_BOSS ? 12 : 6, C_ORANGE);
    NCR_AUDIO_EVENT(NCR_SFX_EXPLODE);

    if (e->kind == NCR_EN_BOSS) {
        g.boss_idx = -1;
        g.score += 1000u * (uint32_t)g.armor;
        g.shake_ticks = 20;
        ncr_update_best();
        ncr_set_state(NCR_STATE_WIN);
        NCR_AUDIO_EVENT(NCR_SFX_WIN);
    } else if ((ncr_rand() % 100u) < 28u) {
        ncr_spawn_pickup(cx - NCR_FP(5), cy, (uint8_t)(ncr_rand() % NCR_PK_COUNT));
    }
}

static void ncr_damage_enemy(int i, int16_t dmg)
{
    ncr_enemy_t *e = &g.enemies[i];
    if (e->kind == NCR_EN_BOSS) dmg = g.core_open ? (int16_t)(dmg * 3) : (int16_t)((dmg + 1) / 2);
    e->hp = (int16_t)(e->hp - dmg);
    e->flash = 3;
    if (e->hp <= 0) ncr_kill_enemy(i);
}

/* =========================================================================
 *  Run reset
 * ========================================================================= */
static void ncr_reset_run(void)
{
    memset(g.enemies,   0, sizeof(g.enemies));
    memset(g.pbullets,  0, sizeof(g.pbullets));
    memset(g.ebullets,  0, sizeof(g.ebullets));
    memset(g.pickups,   0, sizeof(g.pickups));
    memset(g.mines,     0, sizeof(g.mines));
    memset(g.particles, 0, sizeof(g.particles));

    g.px = g.target_px = NCR_FP((NCR_ROAD_L + NCR_ROAD_R) / 2 - NCR_PLAYER_W / 2);
    g.armor = 3;
    g.shield = NCR_METER_MAX; g.heat = 0; g.special = 200;
    g.shield_on = false; g.shield_delay = 0; g.cool_delay = 0; g.overheat_ticks = 0;
    g.turbo_ticks = 0; g.turbo_used = false; g.emp_used = false; g.fired_once = false;
    g.emp_slow_ticks = 0; g.emp_flash_ticks = 0; g.invuln_ticks = 0;
    g.weapon = NCR_WPN_LASER;

    g.score = 0; g.kills = 0; g.streak = 0; g.mult = 1; g.max_mult = 1;
    g.combo_ticks = 0; g.combo_popup_ticks = 0;

    g.wave = 1; g.wave_kills = 0; g.spawn_ticks = 30; g.wave_banner_ticks = 60;

    g.boss_idx = -1; g.boss_phase = 0; g.core_open = false; g.core_ticks = 0;
    g.boss_atk_ticks = 60; g.boss_aux_ticks = 40; g.boss_drone_ticks = 90; g.boss_msl_ticks = 60;
    g.boss_move_ticks = 0; g.boss_target_x = 0;
    g.missile_warn_ticks = 0; g.missile_warn_x = 0; g.phase_banner_ticks = 0;

    g.pickup_msg_ticks = 0; g.pickup_msg_kind = 0;
    g.shake_ticks = 0; g.shake_dx = 0;
}

/* =========================================================================
 *  Per-tick update helpers
 * ========================================================================= */
static uint8_t ncr_read_buttons(const xbox360pad_t *pad)
{
    uint8_t b = 0;
    if (NCR_PAD_BTN(pad, NCR_BTN_A))     b |= NCR_IN_A;
    if (NCR_PAD_BTN(pad, NCR_BTN_X))     b |= NCR_IN_X;
    if (NCR_PAD_BTN(pad, NCR_BTN_LB))    b |= NCR_IN_LB;
    if (NCR_PAD_BTN(pad, NCR_BTN_RB))    b |= NCR_IN_RB;
    if (NCR_PAD_BTN(pad, NCR_BTN_START)) b |= NCR_IN_START;
    return b;
}

static void ncr_update_player(const xbox360pad_t *pad)
{
    /* --- left stick with dead zone -> velocity applied to a TARGET x --- */
    int32_t lx = NCR_PAD_LX(pad);
    if (lx > -NCR_DEADZONE && lx < NCR_DEADZONE) lx = 0;
    else lx = (lx < 0) ? (lx + NCR_DEADZONE) : (lx - NCR_DEADZONE);
    lx = ncr_clamp(lx, -NCR_STICK_SPAN, NCR_STICK_SPAN);

    int32_t speed_px = 5;                  /* px per tick at full deflection */
    if (g.shield_on)   speed_px = 3;       /* precision-dodge mode */
    if (g.turbo_ticks) speed_px = 12;      /* turbo dash */
    g.target_px += (lx * NCR_FP(speed_px)) / NCR_STICK_SPAN;
    g.target_px  = ncr_clamp(g.target_px, NCR_FP(NCR_PLAYER_MIN_X), NCR_FP(NCR_PLAYER_MAX_X));

    /* interpolate towards target (never set directly from stick) */
    g.px += ((g.target_px - g.px) * 6) / 16;
    g.px  = ncr_clamp(g.px, NCR_FP(NCR_PLAYER_MIN_X), NCR_FP(NCR_PLAYER_MAX_X));

    /* --- LT shield: drain while held, recharge after a short delay --- */
    bool lt = NCR_PAD_LT(pad) > 80;
    if (lt && g.shield > 0) {
        g.shield_on = true;
        g.shield -= 6;
        if (g.shield <= 0) { g.shield = 0; g.shield_on = false; }
        g.shield_delay = 30;
    } else {
        g.shield_on = false;
        if (g.shield_delay) g.shield_delay--;
        else if (g.shield < NCR_METER_MAX) { g.shield += 3; if (g.shield > NCR_METER_MAX) g.shield = NCR_METER_MAX; }
    }
}

static void ncr_fire_weapon(void)
{
    int32_t cx  = g.px + NCR_FP(NCR_PLAYER_W / 2);
    int32_t top = NCR_FP(NCR_PLAYER_Y);
    switch (g.weapon) {
    default:
    case NCR_WPN_LASER:
        ncr_spawn_pbullet(cx - NCR_FP(1), top - NCR_FP(10), 0, -NCR_FP(9), 0, 2, 3, 10);
        g.heat += 60;
        break;
    case NCR_WPN_SPREAD:
        for (int k = -1; k <= 1; k++)
            ncr_spawn_pbullet(cx - NCR_FP(1), top - NCR_FP(6), k * NCR_FP(2), -NCR_FP(7), 1, 1, 3, 6);
        g.heat += 85;
        break;
    case NCR_WPN_MISSILE:
        ncr_spawn_pbullet(cx - NCR_FP(2), top - NCR_FP(12), 0, -NCR_FP(5), 2, 6, 5, 12);
        g.heat += 120;
        break;
    }
    NCR_AUDIO_EVENT(NCR_SFX_SHOT);
}

static void ncr_update_weapons(const xbox360pad_t *pad, uint8_t pressed, uint32_t now_ms)
{
    /* X cycles weapons */
    if (pressed & NCR_IN_X) {
        g.weapon = (uint8_t)((g.weapon + 1) % NCR_WPN_COUNT);
        NCR_AUDIO_EVENT(NCR_SFX_SELECT);
    }

    /* RB turbo dash (3 s cooldown, overflow-safe) */
    if ((pressed & NCR_IN_RB) && g.special >= NCR_TURBO_COST &&
        (!g.turbo_used || (uint32_t)(now_ms - g.turbo_ms) >= NCR_TURBO_CD_MS)) {
        g.turbo_used = true; g.turbo_ms = now_ms;
        g.turbo_ticks = 12;
        if (g.invuln_ticks < 12) g.invuln_ticks = 12;
        g.special -= NCR_TURBO_COST;
        NCR_AUDIO_EVENT(NCR_SFX_SELECT);
    }

    /* LB EMP blast (8 s cooldown, overflow-safe) */
    if ((pressed & NCR_IN_LB) && g.special >= NCR_EMP_COST &&
        (!g.emp_used || (uint32_t)(now_ms - g.emp_ms) >= NCR_EMP_CD_MS)) {
        g.emp_used = true; g.emp_ms = now_ms;
        g.special -= NCR_EMP_COST;
        g.emp_slow_ticks = 90;
        g.emp_flash_ticks = 10;
        g.shake_ticks = 6;
        int32_t cx = g.px + NCR_FP(NCR_PLAYER_W / 2), cy = NCR_FP(NCR_PLAYER_Y + NCR_PLAYER_H / 2);
        int32_t r  = NCR_FP(NCR_EMP_RADIUS_PX);
        for (int i = 0; i < NCR_MAX_ENEMY_BULLETS; i++) {
            ncr_ebullet_t *b = &g.ebullets[i];
            if (b->active && ncr_abs(b->x - cx) < r && ncr_abs(b->y - cy) < r) {
                b->active = false; ncr_spawn_particles(b->x, b->y, 1, C_MAGENTA);
            }
        }
        for (int i = 0; i < NCR_MAX_MINES; i++) {
            ncr_mine_t *m = &g.mines[i];
            if (m->active && ncr_abs(m->x - cx) < r && ncr_abs(m->y - cy) < r) {
                m->active = false; ncr_spawn_particles(m->x, m->y, 2, C_MAGENTA);
            }
        }
        NCR_AUDIO_EVENT(NCR_SFX_EMP);
    }

    /* RT primary fire with 110 ms cooldown + heat / overheat */
    bool rt = NCR_PAD_RT(pad) > 60;
    if (g.overheat_ticks) {
        g.overheat_ticks--;
        g.heat -= 25; if (g.heat < 0) g.heat = 0;
        g.cool_delay = 0;
        return;
    }
    if (rt && (!g.fired_once || (uint32_t)(now_ms - g.last_fire_ms) >= NCR_FIRE_CD_MS)) {
        g.fired_once = true;
        g.last_fire_ms = now_ms;
        g.cool_delay = 8;
        ncr_fire_weapon();
        if (g.heat >= NCR_METER_MAX) {
            g.heat = NCR_METER_MAX;
            g.overheat_ticks = NCR_OVERHEAT_TICKS;
            NCR_AUDIO_EVENT(NCR_SFX_ALERT);
        }
    }
    if (g.cool_delay) g.cool_delay--;
    else { g.heat -= 9; if (g.heat < 0) g.heat = 0; }
}

static void ncr_update_enemies(void)
{
    const int32_t slow = g.emp_slow_ticks ? 2 : 1;
    const int32_t pcx  = g.px + NCR_FP(NCR_PLAYER_W / 2);

    for (int i = 0; i < NCR_MAX_ENEMIES; i++) {
        ncr_enemy_t *e = &g.enemies[i];
        if (!e->active || e->kind == NCR_EN_BOSS) continue;
        if (e->flash) e->flash--;

        const bool on_screen = e->y > 0;
        const int32_t ecx = e->x + NCR_FP(e->w / 2);
        const int32_t eby = e->y + NCR_FP(e->h);

        switch (e->kind) {
        case NCR_EN_BIKE:
            if (++e->timer >= 14) { e->timer = 0; e->vx = -e->vx; }
            break;
        case NCR_EN_JEEP:
            if (e->timer) e->timer--; else { e->timer = 72; e->timer2 = 3; }
            if (e->timer2 && (e->timer % 6) == 0 && on_screen) {
                e->timer2--;
                ncr_spawn_ebullet(ecx - NCR_FP(2), eby, 0, NCR_FP(4), 0, 4, 4);
            }
            break;
        case NCR_EN_DRONE:
            if (e->timer) e->timer--;
            else {
                e->timer = 100;
                if (on_screen)
                    ncr_spawn_ebullet(ecx - NCR_FP(2), eby, (pcx > ecx) ? 16 : -16, NCR_FP(3), 1, 5, 5);
            }
            break;
        case NCR_EN_MINETRUCK:
            if (e->timer) e->timer--;
            else { e->timer = 55; if (on_screen) ncr_spawn_mine(ecx - NCR_FP(5), eby); }
            break;
        case NCR_EN_TURRET:
            if (e->timer) e->timer--;
            else {
                e->timer = 80;
                if (on_screen) {
                    int32_t dx = NCR_PX(pcx - ecx);
                    int32_t vx = ncr_clamp((dx * 16) / 60, -40, 40);
                    ncr_spawn_ebullet(ecx - NCR_FP(3), eby, vx, NCR_FP(3), 2, 7, 7);
                }
            }
            break;
        default:
            break;
        }

        e->x += e->vx / slow;
        e->y += e->vy / slow;

        /* keep on the road; bounce lateral movers */
        if (e->x < NCR_FP(NCR_ROAD_L + 2)) { e->x = NCR_FP(NCR_ROAD_L + 2); if (e->vx < 0) e->vx = -e->vx; }
        if (e->x > NCR_FP(NCR_ROAD_R - 2 - e->w)) { e->x = NCR_FP(NCR_ROAD_R - 2 - e->w); if (e->vx > 0) e->vx = -e->vx; }
        if (e->y > NCR_FP(NCR_SCREEN_H)) e->active = false;
    }
}

static void ncr_update_boss(void)
{
    if (g.boss_idx < 0 || g.boss_idx >= NCR_MAX_ENEMIES) return;
    ncr_enemy_t *e = &g.enemies[g.boss_idx];
    if (!e->active || e->kind != NCR_EN_BOSS) { g.boss_idx = -1; return; }
    if (e->flash) e->flash--;
    const int32_t slow = g.emp_slow_ticks ? 2 : 1;

    /* entrance */
    if (e->y < NCR_FP(28)) { e->y += e->vy; return; }

    /* phases: 100-67 / 66-34 / 33-0 */
    int32_t pct = (e->hp_max > 0) ? ((int32_t)e->hp * 100) / e->hp_max : 0;
    uint8_t phase = (pct >= 67) ? 1 : ((pct >= 34) ? 2 : 3);
    if (phase != g.boss_phase) {
        g.boss_phase = phase;
        g.phase_banner_ticks = 45;
        g.shake_ticks = 10;
        g.core_open = false; g.core_ticks = 0;
        NCR_AUDIO_EVENT(NCR_SFX_ALERT);
    }

    /* lateral movement toward a wandering target */
    if (g.boss_move_ticks) g.boss_move_ticks--;
    else {
        g.boss_move_ticks = (uint8_t)ncr_rand_range(50, 90);
        g.boss_target_x = NCR_FP(ncr_rand_range(NCR_ROAD_L + 8, NCR_ROAD_R - 8 - e->w));
    }
    int32_t spd = (NCR_FP(1) + (phase - 1) * 8) / slow;
    if (e->x < g.boss_target_x - spd)      e->x += spd;
    else if (e->x > g.boss_target_x + spd) e->x -= spd;
    else                                   e->x  = g.boss_target_x;

    /* vulnerable core cycle */
    g.core_ticks++;
    uint16_t closed_len = (uint16_t)(200 - phase * 20);
    if (!g.core_open && g.core_ticks >= closed_len) { g.core_open = true;  g.core_ticks = 0; }
    else if (g.core_open && g.core_ticks >= 90)     { g.core_open = false; g.core_ticks = 0; }

    const int32_t bx = e->x + NCR_FP(e->w / 2);
    const int32_t by = e->y + NCR_FP(e->h);

    /* bullet fans */
    if (g.boss_atk_ticks) g.boss_atk_ticks--;
    else {
        g.boss_atk_ticks = (phase == 1) ? 50 : ((phase == 2) ? 40 : 30);
        int n = (phase == 3) ? 7 : 5;
        for (int k = 0; k < n; k++) {
            int32_t vx = (phase == 3) ? (k - 3) * 12 : (k - 2) * 16;
            ncr_spawn_ebullet(bx - NCR_FP(2), by, vx, NCR_FP(2) + phase * 6, 0, 4, 4);
        }
    }
    /* phase 2+: mines + escort drones */
    if (phase >= 2) {
        if (g.boss_aux_ticks) g.boss_aux_ticks--;
        else {
            g.boss_aux_ticks = 75;
            ncr_spawn_mine(e->x + NCR_FP(8), by);
            ncr_spawn_mine(e->x + NCR_FP(e->w - 18), by);
        }
        if (g.boss_drone_ticks) g.boss_drone_ticks--;
        else {
            g.boss_drone_ticks = 140;
            if (ncr_active_enemies() < 2) (void)ncr_spawn_enemy(NCR_EN_DRONE, -1);
        }
    }
    /* phase 3: missile lock with visible warning column, 1 s to dodge */
    if (phase == 3) {
        if (g.missile_warn_ticks) {
            g.missile_warn_ticks--;
            if (g.missile_warn_ticks == 0)
                ncr_spawn_ebullet(NCR_FP(g.missile_warn_x - 3), by, 0, NCR_FP(7), 3, 6, 14);
        } else if (g.boss_msl_ticks) {
            g.boss_msl_ticks--;
        } else {
            g.boss_msl_ticks = 90;
            g.missile_warn_ticks = 30;
            g.missile_warn_x = (int16_t)(NCR_PX(g.px) + NCR_PLAYER_W / 2);
            NCR_AUDIO_EVENT(NCR_SFX_ALERT);
        }
    }
}

static void ncr_update_bullets(void)
{
    const int32_t slow = g.emp_slow_ticks ? 2 : 1;
    const int32_t pcx  = g.px + NCR_FP(NCR_PLAYER_W / 2);

    /* player bullets (missiles steer toward nearest enemy) */
    for (int i = 0; i < NCR_MAX_PLAYER_BULLETS; i++) {
        ncr_pbullet_t *b = &g.pbullets[i];
        if (!b->active) continue;
        if (b->kind == 2) {
            int32_t best = 0x7FFFFFFF, tx = 0; bool found = false;
            for (int j = 0; j < NCR_MAX_ENEMIES; j++) {
                const ncr_enemy_t *e = &g.enemies[j];
                if (!e->active || e->y < 0) continue;
                int32_t ecx = e->x + NCR_FP(e->w / 2);
                int32_t d = ncr_abs(ecx - b->x) + ncr_abs(e->y - b->y);
                if (d < best) { best = d; tx = ecx; found = true; }
            }
            if (found) {
                if (tx > b->x + NCR_FP(2))      b->vx += 6;
                else if (tx < b->x - NCR_FP(2)) b->vx -= 6;
                b->vx = ncr_clamp(b->vx, -48, 48);
            }
            if (b->vy > -NCR_FP(9)) b->vy -= 4;   /* missile accelerates */
        }
        b->x += b->vx; b->y += b->vy;
        if (b->y < -NCR_FP(16) || b->x < 0 || b->x > NCR_FP(NCR_SCREEN_W)) b->active = false;
    }

    /* enemy bullets */
    for (int i = 0; i < NCR_MAX_ENEMY_BULLETS; i++) {
        ncr_ebullet_t *b = &g.ebullets[i];
        if (!b->active) continue;
        if (b->kind == 1 && b->y < NCR_FP(NCR_PLAYER_Y - 10)) {
            if (pcx > b->x + NCR_FP(2))      b->vx += 3;
            else if (pcx < b->x - NCR_FP(2)) b->vx -= 3;
            b->vx = ncr_clamp(b->vx, -40, 40);
        }
        b->x += b->vx / slow; b->y += b->vy / slow;
        if (b->life) b->life--;
        if (!b->life || b->y > NCR_FP(NCR_SCREEN_H) || b->y < -NCR_FP(20) ||
            b->x < NCR_FP(NCR_ROAD_L - 8) || b->x > NCR_FP(NCR_ROAD_R + 8)) b->active = false;
    }

    /* road mines scroll with the road */
    for (int i = 0; i < NCR_MAX_MINES; i++) {
        ncr_mine_t *m = &g.mines[i];
        if (!m->active) continue;
        m->y += NCR_FP(2);
        if (m->fuse) m->fuse--; else m->armed = true;
        if (m->y > NCR_FP(NCR_SCREEN_H)) m->active = false;
    }

    /* particles */
    for (int i = 0; i < NCR_MAX_PARTICLES; i++) {
        ncr_particle_t *p = &g.particles[i];
        if (!p->active) continue;
        p->x += p->vx; p->y += p->vy;
        if (p->life) p->life--;
        if (!p->life) p->active = false;
    }
}

static void ncr_apply_pickup(uint8_t kind)
{
    switch (kind) {
    case NCR_PK_ARMOR:   if (g.armor < 3) g.armor++; break;
    case NCR_PK_SHIELD:  g.shield += 400; if (g.shield > NCR_METER_MAX) g.shield = NCR_METER_MAX; break;
    case NCR_PK_COOLANT: g.heat = 0; g.overheat_ticks = 0; break;
    case NCR_PK_EMP:     g.special += 350; if (g.special > NCR_METER_MAX) g.special = NCR_METER_MAX; break;
    default: return;
    }
    g.score += 50;
    g.pickup_msg_kind = kind;
    g.pickup_msg_ticks = 30;
    NCR_AUDIO_EVENT(NCR_SFX_PICKUP);
}

static void ncr_update_pickups(void)
{
    const int32_t pw = NCR_FP(NCR_PLAYER_W), ph = NCR_FP(NCR_PLAYER_H), py = NCR_FP(NCR_PLAYER_Y);
    for (int i = 0; i < NCR_MAX_PICKUPS; i++) {
        ncr_pickup_t *p = &g.pickups[i];
        if (!p->active) continue;
        p->y += 24;   /* 1.5 px per tick */
        if (p->y > NCR_FP(NCR_SCREEN_H)) { p->active = false; continue; }
        if (ncr_aabb_overlap(p->x, p->y, NCR_FP(10), NCR_FP(10), g.px, py, pw, ph)) {
            p->active = false;
            ncr_apply_pickup(p->kind);
        }
    }
}

static void ncr_resolve_collisions(void)
{
    const int32_t pw = NCR_FP(NCR_PLAYER_W), ph = NCR_FP(NCR_PLAYER_H), py = NCR_FP(NCR_PLAYER_Y);

    /* player bullets -> enemies, mines */
    for (int i = 0; i < NCR_MAX_PLAYER_BULLETS; i++) {
        ncr_pbullet_t *b = &g.pbullets[i];
        if (!b->active) continue;
        const int32_t bw = NCR_FP(b->w), bh = NCR_FP(b->h);
        for (int j = 0; j < NCR_MAX_ENEMIES && b->active; j++) {
            ncr_enemy_t *e = &g.enemies[j];
            if (!e->active || e->y < -NCR_FP(4)) continue;
            if (ncr_aabb_overlap(b->x, b->y, bw, bh, e->x, e->y, NCR_FP(e->w), NCR_FP(e->h))) {
                b->active = false;
                ncr_spawn_particles(b->x, b->y, 1, C_WHITE);
                ncr_damage_enemy(j, b->dmg);
            }
        }
        for (int j = 0; j < NCR_MAX_MINES && b->active; j++) {
            ncr_mine_t *m = &g.mines[j];
            if (!m->active) continue;
            if (ncr_aabb_overlap(b->x, b->y, bw, bh, m->x, m->y, NCR_FP(10), NCR_FP(10))) {
                b->active = false; m->active = false;
                g.score += 50;
                ncr_spawn_particles(m->x, m->y, 3, C_ORANGE);
                NCR_AUDIO_EVENT(NCR_SFX_EXPLODE);
            }
        }
    }
    if (g.state == NCR_STATE_WIN) return;

    /* enemy bullets -> player */
    for (int i = 0; i < NCR_MAX_ENEMY_BULLETS; i++) {
        ncr_ebullet_t *b = &g.ebullets[i];
        if (!b->active) continue;
        if (ncr_aabb_overlap(b->x, b->y, NCR_FP(b->w), NCR_FP(b->h), g.px, py, pw, ph)) {
            b->active = false;
            ncr_damage_player();
        }
    }
    /* mines -> player */
    for (int i = 0; i < NCR_MAX_MINES; i++) {
        ncr_mine_t *m = &g.mines[i];
        if (!m->active || !m->armed) continue;
        if (ncr_aabb_overlap(m->x, m->y, NCR_FP(10), NCR_FP(10), g.px, py, pw, ph)) {
            m->active = false;
            ncr_spawn_particles(m->x, m->y, 4, C_ORANGE);
            ncr_damage_player();
        }
    }
    /* enemy bodies -> player */
    for (int i = 0; i < NCR_MAX_ENEMIES; i++) {
        ncr_enemy_t *e = &g.enemies[i];
        if (!e->active || e->kind == NCR_EN_BOSS) continue;
        if (ncr_aabb_overlap(e->x, e->y, NCR_FP(e->w), NCR_FP(e->h), g.px, py, pw, ph)) {
            ncr_damage_player();
            ncr_damage_enemy(i, 3);
        }
    }
}

static uint8_t ncr_pick_kind_for_wave(uint8_t wave)
{
    uint32_t r = ncr_rand() % 10u;
    switch (wave) {
    case 1:  return NCR_EN_BIKE;
    case 2:  return (r < 4) ? NCR_EN_JEEP : NCR_EN_BIKE;
    case 3:  return (r < 5) ? NCR_EN_DRONE : NCR_EN_MINETRUCK;
    case 4:  return NCR_EN_TURRET;
    default:
        if (r < 3) return NCR_EN_BIKE;
        if (r < 5) return NCR_EN_JEEP;
        if (r < 7) return NCR_EN_DRONE;
        if (r < 9) return NCR_EN_MINETRUCK;
        return NCR_EN_TURRET;
    }
}

static void ncr_update_wave(void)
{
    if (g.wave < 1 || g.wave > NCR_WAVE_COUNT) { g.wave = 1; }
    if (g.wave_banner_ticks) { g.wave_banner_ticks--; return; }
    const ncr_wave_def_t *d = &k_waves[g.wave - 1];

    if (g.wave_kills >= d->kills_needed) {
        if (ncr_active_enemies() == 0) {
            if (g.wave >= NCR_WAVE_COUNT) {
                ncr_set_state(NCR_STATE_BOSS_WARNING);
                NCR_AUDIO_EVENT(NCR_SFX_ALERT);
            } else {
                g.wave++; g.wave_kills = 0;
                g.wave_banner_ticks = 60; g.spawn_ticks = 20;
            }
        }
        return;
    }
    if (g.spawn_ticks) { g.spawn_ticks--; return; }
    if (ncr_active_enemies() < d->max_active) {
        if (ncr_spawn_enemy(ncr_pick_kind_for_wave(g.wave), -1) >= 0) g.spawn_ticks = d->spawn_interval;
        else g.spawn_ticks = 4;   /* no fair slot this tick, retry shortly */
    }
}

static void ncr_tick_timers(void)
{
    if (g.invuln_ticks)       g.invuln_ticks--;
    if (g.turbo_ticks)        g.turbo_ticks--;
    if (g.emp_slow_ticks)     g.emp_slow_ticks--;
    if (g.emp_flash_ticks)    g.emp_flash_ticks--;
    if (g.shake_ticks)        g.shake_ticks--;
    if (g.combo_popup_ticks)  g.combo_popup_ticks--;
    if (g.pickup_msg_ticks)   g.pickup_msg_ticks--;
    if (g.phase_banner_ticks) g.phase_banner_ticks--;
    if (g.combo_ticks) {
        g.combo_ticks--;
        if (!g.combo_ticks && g.streak) {           /* long delay -> combo decays */
            g.streak = 0;
            if (g.mult != 1) { g.mult = 1; g.combo_popup_ticks = 40; }
        }
    }
    g.road_scroll += g.turbo_ticks ? 8u : 4u;
}

static void ncr_tick_world(const xbox360pad_t *pad, uint8_t pressed, uint32_t now_ms)
{
    ncr_tick_timers();
    ncr_update_player(pad);
    ncr_update_weapons(pad, pressed, now_ms);
    ncr_update_enemies();
    ncr_update_boss();
    ncr_update_bullets();
    ncr_update_pickups();
    ncr_resolve_collisions();

    if (g.state == NCR_STATE_PLAY) {
        ncr_update_wave();
    } else if (g.state == NCR_STATE_BOSS_WARNING) {
        if (g.state_ticks >= 60) {
            int8_t idx = ncr_spawn_enemy(NCR_EN_BOSS, (NCR_ROAD_L + NCR_ROAD_R) / 2 - 32);
            if (idx >= 0) {
                g.boss_idx = idx; g.boss_phase = 0; g.core_open = false; g.core_ticks = 0;
                g.boss_atk_ticks = 60; g.boss_aux_ticks = 40; g.boss_drone_ticks = 90; g.boss_msl_ticks = 60;
                g.boss_target_x = g.enemies[idx].x;
                ncr_set_state(NCR_STATE_BOSS);
            } else {
                ESP_LOGW(TAG, "boss spawn deferred: pool full");
            }
        }
    }
}

/* =========================================================================
 *  PUBLIC: init / update
 * ========================================================================= */
void ncr_init(void)
{
    memset(&g, 0, sizeof(g));
    g.rng = 0xA5C3F00Du;
    g.prev_btn = 0xFF;           /* swallow buttons still held from the Games Hub */
    g.clock_ready = false;
    g.boss_idx = -1;
    g.mult = 1;
    ncr_reset_run();
    g.state = NCR_STATE_SPLASH;
    g.resume_state = NCR_STATE_MENU;
    ESP_LOGI(TAG, "NEON CONVOY init (%u bytes state)", (unsigned)sizeof(g));
}

void ncr_update(const xbox360pad_t *pad, uint32_t now_ms)
{
    /* ---- controller guard: never dereference NULL, never restart ---- */
    if (pad == NULL || !NCR_PAD_CONNECTED(pad)) {
        if (g.state != NCR_STATE_CONTROLLER_LOST) {
            g.resume_state = g.state;
            ncr_set_state(NCR_STATE_CONTROLLER_LOST);
            ESP_LOGW(TAG, "controller lost -> gameplay frozen");
        }
        g.last_tick_ms = now_ms;
        return;
    }
    if (!g.clock_ready) { g.clock_ready = true; g.last_tick_ms = now_ms; }

    if (g.state == NCR_STATE_CONTROLLER_LOST) {
        ncr_state_t r = g.resume_state;
        if (r == NCR_STATE_PLAY || r == NCR_STATE_BOSS || r == NCR_STATE_BOSS_WARNING || r == NCR_STATE_COUNTDOWN) {
            g.resume_state = r;
            ncr_set_state(NCR_STATE_PAUSED);          /* resume safely via pause */
        } else if (r == NCR_STATE_CONTROLLER_LOST) {
            ncr_set_state(NCR_STATE_MENU);
        } else {
            ncr_set_state(r);
        }
        g.prev_btn = 0xFF;
        g.last_tick_ms = now_ms;
        ESP_LOGI(TAG, "controller back");
        return;
    }

    /* ---- fixed 33 ms logic tick, overflow-safe ---- */
    if ((uint32_t)(now_ms - g.last_tick_ms) < NCR_TICK_MS) return;
    g.last_tick_ms = now_ms;
    g.tick++;
    g.state_ticks++;

    uint8_t cur = ncr_read_buttons(pad);
    uint8_t pressed = (uint8_t)(cur & (uint8_t)~g.prev_btn);
    g.prev_btn = cur;

    switch (g.state) {
    case NCR_STATE_SPLASH:
        g.road_scroll += 4;
        if (g.state_ticks >= 60 || (pressed & NCR_IN_A)) ncr_set_state(NCR_STATE_MENU);
        break;

    case NCR_STATE_MENU:
        g.road_scroll += 4;
        if (pressed & NCR_IN_X) g.weapon = (uint8_t)((g.weapon + 1) % NCR_WPN_COUNT);
        if (pressed & NCR_IN_A) {
            uint8_t keep = g.weapon;
            ncr_reset_run();
            g.weapon = keep;
            ncr_set_state(NCR_STATE_COUNTDOWN);
            NCR_AUDIO_EVENT(NCR_SFX_SELECT);
        }
        break;

    case NCR_STATE_COUNTDOWN:
        g.road_scroll += 4;
        ncr_update_player(pad);                 /* allow positioning during 3-2-1 */
        if (pressed & NCR_IN_START) { g.resume_state = g.state; ncr_set_state(NCR_STATE_PAUSED); break; }
        if (g.state_ticks >= 90) { ncr_set_state(NCR_STATE_PLAY); g.wave_banner_ticks = 45; }
        break;

    case NCR_STATE_PLAY:
    case NCR_STATE_BOSS_WARNING:
    case NCR_STATE_BOSS:
        if (pressed & NCR_IN_START) { g.resume_state = g.state; ncr_set_state(NCR_STATE_PAUSED); break; }
        ncr_tick_world(pad, pressed, now_ms);
        break;

    case NCR_STATE_PAUSED:
        if (pressed & (NCR_IN_START | NCR_IN_A)) {
            ncr_state_t r = g.resume_state;
            if (r != NCR_STATE_PLAY && r != NCR_STATE_BOSS && r != NCR_STATE_BOSS_WARNING && r != NCR_STATE_COUNTDOWN)
                r = NCR_STATE_PLAY;
            ncr_set_state(r);
            g.last_fire_ms = now_ms;           /* no instant burst after resume */
        }
        break;

    case NCR_STATE_WIN:
    case NCR_STATE_GAME_OVER:
        g.road_scroll += 2;
        ncr_update_bullets();                  /* let particles finish */
        if (g.state_ticks > 20 && (pressed & NCR_IN_A)) {
            uint8_t keep = g.weapon;
            ncr_reset_run();
            g.weapon = keep;
            ncr_set_state(NCR_STATE_COUNTDOWN);
        }
        break;

    case NCR_STATE_CONTROLLER_LOST:
    default:
        break;
    }
}

/* =========================================================================
 *  Drawing
 * ========================================================================= */
static void ncr_draw_background(void)
{
    NCR_FB_FILL_SCREEN(C_BG);

    /* scrolling side structures (cheap rects) */
    int32_t off = (int32_t)(g.road_scroll % 48u);
    for (int32_t y = -48 + off; y < NCR_SCREEN_H; y += 48) {
        ncr_rect(8,   y,      22, 20, C_DARK);
        ncr_rect(12,  y + 4,   3,  3, C_DKMAG);
        ncr_rect(20,  y + 10,  3,  3, C_LANE_DIM);
        ncr_rect(290, y + 16, 22, 26, C_DARK);
        ncr_rect(296, y + 20,  3,  3, C_LANE_DIM);
        ncr_rect(304, y + 30,  3,  3, C_DKMAG);
    }

    /* road + neon edges */
    ncr_rect(NCR_ROAD_L, 0, NCR_ROAD_W, NCR_SCREEN_H, C_ROAD);
    ncr_rect(NCR_ROAD_L - 2, 0, 2, NCR_SCREEN_H, C_CYAN);
    ncr_rect(NCR_ROAD_R,     0, 2, NCR_SCREEN_H, C_MAGENTA);

    /* scrolling lane dashes */
    int32_t d = (int32_t)(g.road_scroll % 32u);
    for (int lane = 1; lane <= 2; lane++) {
        int32_t x = NCR_ROAD_L + lane * 80 - 1;
        for (int32_t y = -32 + d; y < NCR_SCREEN_H; y += 32) ncr_rect(x, y, 2, 16, C_LANE);
    }

    /* EMP shockwave ring */
    if (g.emp_flash_ticks) {
        int32_t r = (10 - g.emp_flash_ticks) * 14;
        int32_t cx = NCR_PX(g.px) + NCR_PLAYER_W / 2, cy = NCR_PLAYER_Y + NCR_PLAYER_H / 2;
        ncr_frame(cx - r, cy - r, r * 2, r * 2, (g.tick & 1u) ? C_WHITE : C_MAGENTA);
    }
    /* EMP slow tint on lane edges */
    if (g.emp_slow_ticks) ncr_frame(NCR_ROAD_L, 22, NCR_ROAD_W, NCR_SCREEN_H - 44, C_DKMAG);
}

static void ncr_draw_player(void)
{
    if (g.invuln_ticks && !g.turbo_ticks && !g.shield_on && (g.tick & 2u)) return;   /* blink */
    int32_t x = NCR_PX(g.px), y = NCR_PLAYER_Y;
    uint16_t tc = (g.tick & 1u) ? C_ORANGE : C_YELLOW;

    if (g.turbo_ticks) ncr_rect(x + 4, y + 24, 10, 14, C_LANE_DIM);
    ncr_rect(x + 3, y + 22, 3, g.turbo_ticks ? 8 : 4, tc);
    ncr_rect(x + 12, y + 22, 3, g.turbo_ticks ? 8 : 4, tc);
    ncr_rect(x + 2, y,      14, 22, C_CYAN);       /* hull   */
    ncr_rect(x,     y + 6,  18, 12, C_CYAN);       /* wings  */
    ncr_rect(x + 6, y + 3,   6,  7, C_WHITE);      /* canopy */
    ncr_rect(x + 7, y + 13,  4,  5, C_DARK);       /* intake */
    ncr_rect(x,     y + 8,   2,  8, C_WHITE);
    ncr_rect(x + 16, y + 8,  2,  8, C_WHITE);

    if (g.shield_on) {
        ncr_frame(x - 4, y - 4, NCR_PLAYER_W + 8, NCR_PLAYER_H + 8, C_CYAN);
        if (g.tick & 1u) ncr_frame(x - 5, y - 5, NCR_PLAYER_W + 10, NCR_PLAYER_H + 10, C_WHITE);
    }
}

static void ncr_draw_enemy(const ncr_enemy_t *e)
{
    int32_t x = NCR_PX(e->x), y = NCR_PX(e->y);
    if (e->flash && e->kind != NCR_EN_BOSS) { ncr_rect(x, y, e->w, e->h, C_WHITE); return; }

    switch (e->kind) {
    case NCR_EN_BIKE:
        ncr_rect(x + 3, y,     4, 14, C_RED);
        ncr_rect(x,     y + 4, 10, 5, C_ORANGE);
        ncr_rect(x + 4, y + 1, 2,  3, C_YELLOW);
        break;
    case NCR_EN_JEEP:
        ncr_rect(x,     y,      16, 16, C_ORANGE);
        ncr_rect(x + 3, y + 3,  10,  6, C_DARK);
        ncr_rect(x + 6, y + 12,  4,  4, C_RED);
        break;
    case NCR_EN_DRONE:
        ncr_rect(x + 4, y + 2, 6, 6, C_PURPLE);
        ncr_rect(x,     y + 4, 14, 2, C_MAGENTA);
        if (g.tick & 1u) { ncr_rect(x, y, 4, 2, C_PURPLE); ncr_rect(x + 10, y + 8, 4, 2, C_PURPLE); }
        else             { ncr_rect(x + 10, y, 4, 2, C_PURPLE); ncr_rect(x, y + 8, 4, 2, C_PURPLE); }
        break;
    case NCR_EN_MINETRUCK:
        ncr_rect(x,     y,      18, 24, C_DKRED);
        ncr_rect(x + 2, y + 2,  14,  8, C_DARK);
        ncr_rect(x + 5, y + 14,  8,  6, C_YELLOW);
        ncr_rect(x + 7, y + 16,  4,  2, C_BLACK);
        break;
    case NCR_EN_TURRET:
        ncr_rect(x,     y,      22, 22, C_PURPLE);
        ncr_rect(x + 5, y + 5,  12, 12, C_DARK);
        ncr_rect(x + 8, y + 8,   6,  6, C_MAGENTA);
        ncr_rect(x + 9, y + 16,  4, 10, C_MAGENTA);
        break;
    case NCR_EN_BOSS: {
        uint16_t body = e->flash ? C_WHITE : C_DKRED;
        ncr_rect(x,      y,      64, 40, body);
        ncr_rect(x + 4,  y + 4,  56, 32, C_RED);
        ncr_rect(x + 8,  y + 8,  48,  6, C_DKRED);
        ncr_rect(x + 6,  y + 36,  8,  8, C_GRAY);
        ncr_rect(x + 28, y + 36,  8,  8, C_GRAY);
        ncr_rect(x + 50, y + 36,  8,  8, C_GRAY);
        ncr_rect(x + 10, y + 18,  8,  4, C_YELLOW);
        ncr_rect(x + 46, y + 18,  8,  4, C_YELLOW);
        uint16_t core = g.core_open ? ((g.tick & 1u) ? C_YELLOW : C_WHITE) : C_DARK;
        ncr_rect(x + 26, y + 14, 12, 12, core);
        ncr_frame(x + 24, y + 12, 16, 16, g.core_open ? C_YELLOW : C_MAGENTA);
        break;
    }
    default: break;
    }
}

static void ncr_draw_world(void)
{
    /* missile lock column */
    if (g.missile_warn_ticks) {
        int32_t x = g.missile_warn_x - 1;
        for (int32_t y = 30; y < NCR_PLAYER_Y + 24; y += 12)
            ncr_rect(x, y + ((g.tick & 1u) ? 6 : 0), 3, 6, C_YELLOW);
    }
    for (int i = 0; i < NCR_MAX_MINES; i++) {
        const ncr_mine_t *m = &g.mines[i];
        if (!m->active) continue;
        int32_t x = NCR_PX(m->x), y = NCR_PX(m->y);
        ncr_rect(x, y, 10, 10, m->armed ? C_RED : C_GRAY);
        ncr_rect(x + 3, y + 3, 4, 4, (m->armed && (g.tick & 2u)) ? C_YELLOW : C_BLACK);
    }
    for (int i = 0; i < NCR_MAX_PICKUPS; i++) {
        const ncr_pickup_t *p = &g.pickups[i];
        if (!p->active || p->kind >= NCR_PK_COUNT) continue;
        int32_t x = NCR_PX(p->x), y = NCR_PX(p->y);
        ncr_rect(x, y, 10, 10, k_pickup_colors[p->kind]);
        ncr_rect(x + 4, y + 2, 2, 6, C_BLACK);
        ncr_rect(x + 2, y + 4, 6, 2, C_BLACK);
        if (g.tick & 1u) ncr_frame(x - 1, y - 1, 12, 12, C_WHITE);
    }
    for (int i = 0; i < NCR_MAX_ENEMIES; i++)
        if (g.enemies[i].active) ncr_draw_enemy(&g.enemies[i]);
    for (int i = 0; i < NCR_MAX_ENEMY_BULLETS; i++) {
        const ncr_ebullet_t *b = &g.ebullets[i];
        if (!b->active) continue;
        uint16_t c = (b->kind == 1) ? C_MAGENTA : (b->kind == 2) ? C_PURPLE : (b->kind == 3) ? C_YELLOW : C_ORANGE;
        ncr_rect(NCR_PX(b->x), NCR_PX(b->y), b->w, b->h, c);
        if (b->kind == 3) ncr_rect(NCR_PX(b->x) + 1, NCR_PX(b->y) - 4, 4, 4, (g.tick & 1u) ? C_ORANGE : C_RED);
    }
    for (int i = 0; i < NCR_MAX_PLAYER_BULLETS; i++) {
        const ncr_pbullet_t *b = &g.pbullets[i];
        if (!b->active) continue;
        uint16_t c = (b->kind == 2) ? C_WHITE : C_CYAN;
        ncr_rect(NCR_PX(b->x), NCR_PX(b->y), b->w, b->h, c);
        if (b->kind == 2) ncr_rect(NCR_PX(b->x) + 1, NCR_PX(b->y) + b->h, 3, 4, C_ORANGE);
    }
    ncr_draw_player();
    for (int i = 0; i < NCR_MAX_PARTICLES; i++) {
        const ncr_particle_t *p = &g.particles[i];
        if (p->active) ncr_rect(NCR_PX(p->x), NCR_PX(p->y), 2, 2, p->color);
    }
}

static void ncr_draw_hud(void)
{
    char buf[24];

    /* ---- top bar ---- */
    ncr_rect(0, 0, NCR_SCREEN_W, 22, C_BLACK);
    for (int i = 0; i < 3; i++) {
        ncr_rect(4 + i * 16, 5, 13, 12, (i < g.armor) ? C_GREEN : C_DARK);
        ncr_frame(4 + i * 16, 5, 13, 12, C_GRAY);
    }
    snprintf(buf, sizeof(buf), "%06lu", (unsigned long)g.score);
    ncr_text_center(3, buf, C_WHITE, 2);
    if (g.mult > 1) {
        snprintf(buf, sizeof(buf), "x%u", (unsigned)g.mult);
        ncr_text(200, 7, buf, C_YELLOW, 1);
    }
    const char *wn = k_weapon_names[g.weapon < NCR_WPN_COUNT ? g.weapon : 0];
    ncr_text(NCR_SCREEN_W - 4 - (int32_t)strlen(wn) * 12, 3, wn, C_CYAN, 2);

    /* ---- shield bar (left, vertical) ---- */
    ncr_text(1, 26, "SHD", C_CYAN, 1);
    ncr_frame(4, 38, 10, 144, C_GRAY);
    int32_t h = (g.shield * 140) / NCR_METER_MAX;
    ncr_rect(6, 180 - h, 6, h, g.shield_on ? C_WHITE : C_CYAN);

    /* ---- heat bar (right, vertical) ---- */
    ncr_text(295, 26, "HEAT", C_ORANGE, 1);
    ncr_frame(306, 38, 10, 144, C_GRAY);
    h = (g.heat * 140) / NCR_METER_MAX;
    uint16_t hc = g.overheat_ticks ? ((g.tick & 1u) ? C_WHITE : C_RED) : (g.heat > 700 ? C_ORANGE : C_YELLOW);
    ncr_rect(308, 180 - h, 6, h, hc);

    /* ---- bottom bar ---- */
    ncr_rect(0, 220, NCR_SCREEN_W, 20, C_BLACK);
    if (g.state == NCR_STATE_BOSS || g.state == NCR_STATE_BOSS_WARNING) snprintf(buf, sizeof(buf), "BOSS");
    else snprintf(buf, sizeof(buf), "WAVE %u", (unsigned)g.wave);
    ncr_text(4, 224, buf, C_WHITE, 2);

    ncr_text(98, 226, "EMP", C_MAGENTA, 1);
    ncr_frame(118, 225, 90, 10, C_GRAY);
    int32_t w = (g.special * 86) / NCR_METER_MAX;
    ncr_rect(120, 227, w, 6, (g.special >= NCR_EMP_COST) ? C_MAGENTA : C_DKMAG);

    ncr_text(NCR_SCREEN_W - 4 - 9 * 6, 226, "BACK:EXIT", C_GRAY, 1);

    /* ---- boss health bar ---- */
    if (g.state == NCR_STATE_BOSS && g.boss_idx >= 0 && g.boss_idx < NCR_MAX_ENEMIES) {
        const ncr_enemy_t *b = &g.enemies[g.boss_idx];
        if (b->active && b->hp_max > 0) {
            int32_t bw = ((int32_t)b->hp * 200) / b->hp_max;
            ncr_rect(60, 23, 200, 7, C_DARK);
            ncr_rect(60, 23, bw, 7, g.core_open ? C_YELLOW : C_RED);
            ncr_frame(60, 23, 200, 7, C_GRAY);
            ncr_rect(60 + 66, 23, 1, 7, C_WHITE);
            ncr_rect(60 + 133, 23, 1, 7, C_WHITE);
            if (g.core_open) ncr_text_center(31, "CORE EXPOSED", C_YELLOW, 1);
        }
    }
}

static void ncr_draw_summary(const char *title, uint16_t color)
{
    char buf[24];
    ncr_rect(40, 44, 240, 152, C_PANEL);
    ncr_frame(40, 44, 240, 152, color);
    ncr_text_center(52, title, color, 2);
    snprintf(buf, sizeof(buf), "SCORE  %06lu", (unsigned long)g.score);
    ncr_text_center(80, buf, C_WHITE, 2);
    snprintf(buf, sizeof(buf), "KILLS %u  MAX x%u", (unsigned)g.kills, (unsigned)g.max_mult);
    ncr_text_center(104, buf, C_CYAN, 1);
    snprintf(buf, sizeof(buf), "WAVE %u  BEST %06lu", (unsigned)g.wave, (unsigned long)s_session_best);
    ncr_text_center(118, buf, C_YELLOW, 1);
    if (g.state_ticks > 20 && (g.tick & 8u)) ncr_text_center(150, "A RESTART", C_GREEN, 2);
    ncr_text_center(176, "BACK EXIT TO HUB", C_GRAY, 1);
}

static void ncr_draw_overlay(void)
{
    char buf[24];
    switch (g.state) {
    case NCR_STATE_SPLASH:
        ncr_rect(20, 60, 280, 110, C_PANEL);
        ncr_frame(20, 60, 280, 110, C_CYAN);
        ncr_text_center(72, "NEON CONVOY", C_CYAN, 3);
        ncr_text_center(102, "SENTINEL RUN", C_MAGENTA, 2);
        if (g.tick & 8u) ncr_text_center(140, "PRESS A", C_WHITE, 2);
        return;

    case NCR_STATE_MENU:
        ncr_rect(20, 30, 280, 180, C_PANEL);
        ncr_frame(20, 30, 280, 180, C_MAGENTA);
        ncr_text_center(36, "NEON CONVOY", C_CYAN, 3);
        ncr_text_center(64, "SENTINEL RUN", C_MAGENTA, 2);
        ncr_text_center(90,  "LS MOVE   RT FIRE   LT SHIELD", C_WHITE, 1);
        ncr_text_center(102, "RB TURBO  LB EMP    X WEAPON",  C_WHITE, 1);
        ncr_text_center(114, "START PAUSE      BACK EXIT",    C_GRAY, 1);
        snprintf(buf, sizeof(buf), "WEAPON: %s", k_weapon_names[g.weapon < NCR_WPN_COUNT ? g.weapon : 0]);
        ncr_text_center(132, buf, C_YELLOW, 1);
        snprintf(buf, sizeof(buf), "BEST %06lu", (unsigned long)s_session_best);
        ncr_text_center(148, buf, C_GREEN, 2);
        if (g.tick & 8u) ncr_text_center(180, "A  START", C_WHITE, 2);
        return;

    case NCR_STATE_COUNTDOWN: {
        uint32_t n = 3 - (g.state_ticks / 30);
        if (n < 1) n = 1;
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)n);
        ncr_rect(140, 84, 40, 48, C_PANEL);
        ncr_text_center(92, buf, C_CYAN, 4);
        ncr_text_center(140, "GET READY", C_WHITE, 2);
        return;
    }

    case NCR_STATE_PAUSED:
        ncr_rect(60, 76, 200, 84, C_PANEL);
        ncr_frame(60, 76, 200, 84, C_CYAN);
        ncr_text_center(88, "PAUSED", C_WHITE, 3);
        ncr_text_center(122, "START RESUME", C_CYAN, 1);
        ncr_text_center(136, "BACK EXIT TO HUB", C_GRAY, 1);
        return;

    case NCR_STATE_CONTROLLER_LOST:
        ncr_rect(10, 60, 300, 120, C_BLACK);
        ncr_frame(10, 60, 300, 120, C_RED);
        ncr_frame(12, 62, 296, 116, C_RED);
        ncr_text_center(76, "CONTROLLER", C_RED, 3);
        ncr_text_center(104, "LOST", C_RED, 3);
        ncr_text_center(140, "RECONNECT PAD  -  CAR PARKED", C_WHITE, 1);
        ncr_text_center(156, "GAME FROZEN", C_GRAY, 1);
        return;

    case NCR_STATE_WIN:
        ncr_draw_summary("CONVOY SAVED", C_GREEN);
        return;

    case NCR_STATE_GAME_OVER:
        ncr_draw_summary("CONVOY LOST", C_RED);
        return;

    case NCR_STATE_BOSS_WARNING:
        ncr_rect(0, 90, NCR_SCREEN_W, 44, (g.tick & 2u) ? C_DKRED : C_BLACK);
        ncr_text_center(100, "BOSS INCOMING", (g.tick & 2u) ? C_WHITE : C_RED, 3);
        return;

    default:
        break;
    }

    /* ---- in-play: exactly ONE banner, by priority ---- */
    if (g.wave_banner_ticks && g.state == NCR_STATE_PLAY) {
        snprintf(buf, sizeof(buf), "WAVE %u", (unsigned)g.wave);
        ncr_banner(buf, C_CYAN, C_PANEL);
    } else if (g.phase_banner_ticks) {
        snprintf(buf, sizeof(buf), "BOSS PHASE %u", (unsigned)g.boss_phase);
        ncr_banner(buf, C_RED, C_BLACK);
    } else if (g.overheat_ticks) {
        ncr_banner("OVERHEAT", (g.tick & 1u) ? C_WHITE : C_RED, C_DKRED);
    } else if (g.missile_warn_ticks) {
        ncr_banner("MISSILE LOCK", C_YELLOW, C_BLACK);
    } else if (g.pickup_msg_ticks) {
        uint8_t k = g.pickup_msg_kind < NCR_PK_COUNT ? g.pickup_msg_kind : 0;
        ncr_banner(k_pickup_names[k], k_pickup_colors[k], C_PANEL);
    } else if (g.combo_popup_ticks) {
        snprintf(buf, sizeof(buf), "COMBO x%u", (unsigned)g.mult);
        ncr_banner(buf, g.mult > 1 ? C_YELLOW : C_GRAY, C_PANEL);
    }
}

/* =========================================================================
 *  PUBLIC: draw (exactly once per Car OS frame; never pushes the TFT itself)
 * ========================================================================= */
void ncr_draw(uint32_t now_ms)
{
    (void)now_ms;   /* rendering is tick-driven; Car OS owns the TFT push */
    g.shake_dx = g.shake_ticks ? ((g.tick & 1u) ? 2 : -2) : 0;

    ncr_draw_background();
    if (g.state != NCR_STATE_SPLASH && g.state != NCR_STATE_MENU) ncr_draw_world();
    ncr_draw_hud();
    ncr_draw_overlay();

    g.shake_dx = 0;
}
