/*
 * cargame_neon_serpent.c - NEON SERPENT: GRID BREACH - Car OS game 7
 *
 * Grid snake (18x9 @ 16 px) on the ns_theme cyberpunk renderer.
 * Systems: armor blocks, shield/EMP modes, cores per stage, drones/hunters/
 * warden-minis, mines, dash (RT), EMP blast (RS), GRID WARDEN boss with
 * weak windows. Pooled objects, integer math, no heap use.
 *
 * Controls: D-pad steer | A fire | RT dash | Y/LB/RB mode | RS EMP blast
 *           LT precision (slow tick) | Start pause | B exit (framework)
 */
#include "cargame_neon_serpent.h"
#include "snd_bank.h"          /* PART 5: highscore fanfare */
#include "ns_theme.h"
#include "os_gfx.h"          /* gfx_rect / RGB565 for boss drawing */
#include "car_global.h"
#include "esp_random.h"
#include <string.h>
#include <stdio.h>

/* ------------------------------- tunables -------------------------------- */
#define NS7_COLS        18
#define NS7_ROWS        9
#define NS7_MAX_SEG     64                 /* ring capacity, len <= 63    */
#define NS7_MAX_EN      6
#define NS7_MAX_PK      4
#define NS7_MAX_MINE    6
#define NS7_MAX_SHOT    12

#define NS7_CORES_PER_STAGE 10
#define NS7_STAGE_MAX   3
#define NS7_BOSS_HP     45

#define NS7_TICK_BASE   150                /* ms per cell, stage-scaled   */
#define NS7_FIRE_MS     350
#define NS7_DASH_MS     2500
#define NS7_COMBO_MS    2500
#define NS7_INVULN_MS   1200
#define NS7_PK_LIFE_MS  12000
#define NS7_FRAME_MS    33

#define NS7_ARMOR_MAX   8
#define NS7_SHIELD_HIT  25                 /* shield consumed per hit     */

typedef struct { int x, y; } ns7_cell_t;   /* col,row                     */

typedef struct {                           /* kind: 0 drone 1 hunter 2 mini */
    int x, y, hp, kind, move_cd;
    bool alive;
} ns7_enemy_t;

typedef struct {                           /* pixel-space projectile       */
    int x, y, dx, dy;
    bool alive, hostile;
} ns7_shot_t;

typedef struct {
    int x, y, kind;                        /* 0 repair 1 shield 2 emp      */
    unsigned born_ms;
    bool alive;
} ns7_pk_t;

typedef struct { int x, y, hp; bool alive; } ns7_mine_t;

typedef struct {
    /* snake */
    ns7_cell_t ring[NS7_MAX_SEG];
    int  head_i, len, dir_x, dir_y;
    ns7_cell_t pend;
    bool has_pend;
    int  grow_pending;
    /* pacing */
    uint32_t next_tick;
    int  tick_ms;
    /* vitals */
    int  mode;                             /* 0 SHIELD 1 EMP               */
    int  armor, shield, heat, emp;
    int  dash_cd, fire_cd;
    bool overheated, shield_on;
    /* progress */
    int  score, stage, stage_cores, cores_total, kills;
    int  combo, combo_ms, max_combo;
    uint32_t best, elapsed_ms;
    bool paused, over, win, lost_pad, new_best;
    /* entities */
    ns7_enemy_t en[NS7_MAX_EN];
    ns7_pk_t    pk[NS7_MAX_PK];
    ns7_mine_t  mine[NS7_MAX_MINE];
    ns7_shot_t  shot[NS7_MAX_SHOT];
    ns7_cell_t  core[2];
    bool        core_alive[2];
    /* boss */
    bool boss_active, boss_warn;
    int  boss_hp, boss_x, boss_dir, boss_fire_cd, weak_ms, weak_cd;
    uint32_t warn_until;
    /* fx */
    uint32_t damage_until;
    int  toast_kind;                       /* 0 none 1 pickup 2 combo      */
    char toast[20];
    uint32_t toast_until;
    int  scroll;
    uint16_t prevdig;
    char objective[32];
} ns7_t;

static ns7_t N7;

/* ------------------------------- helpers --------------------------------- */
static int ns7_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int ns7_cell_px(int c) { return NS_FIELD_IN_X + c * NS_GRID_STEP; }
static int ns7_cell_py(int r) { return NS_FIELD_IN_Y + r * NS_GRID_STEP; }
static int ns7_seg_idx(int k) { return (N7.head_i - k + NS7_MAX_SEG) % NS7_MAX_SEG; }
static int ns7_mult(void) { return N7.combo < 1 ? 1 : (N7.combo > 9 ? 9 : N7.combo); }

static bool ns7_on_snake(int c, int r)
{
    for (int k = 0; k < N7.len; k++) {
        ns7_cell_t *s = &N7.ring[ns7_seg_idx(k)];
        if (s->x == c && s->y == r) return true;
    }
    return false;
}

static bool ns7_cell_free(int c, int r)
{
    if (c < 0 || r < 0 || c >= NS7_COLS || r >= NS7_ROWS) return false;
    if (ns7_on_snake(c, r)) return false;
    for (int i = 0; i < NS7_MAX_EN; i++)
        if (N7.en[i].alive && N7.en[i].x == c && N7.en[i].y == r) return false;
    for (int i = 0; i < NS7_MAX_MINE; i++)
        if (N7.mine[i].alive && N7.mine[i].x == c && N7.mine[i].y == r) return false;
    for (int i = 0; i < 2; i++)
        if (N7.core_alive[i] && N7.core[i].x == c && N7.core[i].y == r) return false;
    return true;
}

static bool ns7_rand_free(int *c, int *r, int min_row)
{
    for (int tries = 0; tries < 40; tries++) {
        int cc = (int)(esp_random() % NS7_COLS);
        int rr = min_row + (int)(esp_random() % (NS7_ROWS - min_row));
        if (ns7_cell_free(cc, rr)) { *c = cc; *r = rr; return true; }
    }
    return false;
}
/* ------------------------------- spawners -------------------------------- */
static void ns7_spawn_core(void)
{
    for (int i = 0; i < 2; i++) {
        if (N7.core_alive[i]) continue;
        int c, r;
        if (ns7_rand_free(&c, &r, N7.boss_active ? 3 : 0)) {
            N7.core[i] = (ns7_cell_t){ c, r };
            N7.core_alive[i] = true;
        }
        return;
    }
}

static void ns7_spawn_enemy(int kind)
{
    for (int i = 0; i < NS7_MAX_EN; i++) {
        if (N7.en[i].alive) continue;
        int c, r;
        if (!ns7_rand_free(&c, &r, 0)) return;
        ns7_enemy_t *e = &N7.en[i];
        e->alive = true;
        e->kind  = kind;
        e->x = c; e->y = r;
        e->hp = (kind == 2) ? 3 : 1;
        e->move_cd = (kind == 1) ? 1 : (kind == 2) ? 3 : 2;
        return;
    }
}

static void ns7_spawn_pickup(int kind)
{
    for (int i = 0; i < NS7_MAX_PK; i++) {
        if (N7.pk[i].alive) continue;
        int c, r;
        if (!ns7_rand_free(&c, &r, 0)) return;
        N7.pk[i].alive   = true;
        N7.pk[i].kind    = kind;
        N7.pk[i].x       = c;
        N7.pk[i].y       = r;
        N7.pk[i].born_ms = N7.elapsed_ms;
        return;
    }
}

static void ns7_spawn_mine(void)
{
    for (int i = 0; i < NS7_MAX_MINE; i++) {
        if (N7.mine[i].alive) continue;
        int c, r;
        if (!ns7_rand_free(&c, &r, 0)) return;
        N7.mine[i].alive = true;
        N7.mine[i].x = c;
        N7.mine[i].y = r;
        N7.mine[i].hp = 1;
        return;
    }
}

/* --------------------------- snake reposition ----------------------------- */
static void ns7_reposition_snake(void)
{
    int r = NS7_ROWS / 2;
    N7.dir_x = 1; N7.dir_y = 0;
    N7.has_pend = false;
    N7.head_i = 0;
    if (N7.len > 10) N7.len = 10;
    for (int k = 0; k < N7.len; k++) {
        N7.ring[k].x = 4 - k;
        if (N7.ring[k].x < 0) N7.ring[k].x = 0;
        N7.ring[k].y = r;
    }
    /* drop any enemy/ mine sitting on the new snake path */
    for (int i = 0; i < NS7_MAX_EN; i++)
        if (N7.en[i].alive && N7.en[i].y == r && N7.en[i].x < 8)
            N7.en[i].alive = false;
    for (int i = 0; i < NS7_MAX_MINE; i++)
        if (N7.mine[i].alive && N7.mine[i].y == r && N7.mine[i].x < 8)
            N7.mine[i].alive = false;
}

/* ---------------------------- stage / boss ------------------------------- */
static void ns7_stage_up(void)
{
    N7.stage++;
    N7.stage_cores = 0;
    N7.score += 500;
    N7.tick_ms = NS7_TICK_BASE - (N7.stage - 1) * 20;
    if (N7.tick_ms < 100) N7.tick_ms = 100;
    if (N7.stage >= NS7_STAGE_MAX) {
        N7.boss_warn = true;
        N7.warn_until = N7.elapsed_ms + 2500;
        car_sfx_blip(300);
    } else {
        int mines = 2 + N7.stage;
        for (int i = 0; i < mines; i++) ns7_spawn_mine();
        ns7_spawn_enemy(0);
        car_sfx_score();
    }
}

static void ns7_boss_spawn(void)
{
    N7.boss_active = true;
    N7.boss_hp     = NS7_BOSS_HP;
    N7.boss_x      = NS_FIELD_IN_X + 8;
    N7.boss_dir    = 1;
    N7.boss_fire_cd = 60;
    N7.weak_cd     = 150;
    N7.weak_ms     = 0;
    N7.boss_warn   = false;
}

static void ns7_boss_down(void)
{
    N7.boss_active = false;
    N7.win       = true;
    N7.score    += 3000;
    N7.new_best  = (N7.score > N7.best);
    if (N7.new_best) { N7.best = N7.score; snd_play("highscore_fanfare"); }
    car_sfx_score();
}

/* -------------------------------- init ----------------------------------- */
void g7_init(void)
{
    memset(&N7, 0, sizeof(N7));
    N7.armor   = 3;
    N7.shield  = 50;
    N7.mode    = 0;
    N7.stage   = 1;
    N7.len     = 3;
    N7.tick_ms = NS7_TICK_BASE;
    N7.grow_pending = 0;
    ns7_reposition_snake();
    ns7_spawn_core();
    ns7_spawn_core();
    ns7_spawn_enemy(0);
    ns7_spawn_pickup(0);
    N7.next_tick = 0;
}

/* --------------------------- damage / death ------------------------------ */
static void ns7_hit_player(uint32_t now)
{
    if (now < N7.damage_until) return;
    if (N7.mode == 0 && N7.shield >= NS7_SHIELD_HIT) {
        N7.shield -= NS7_SHIELD_HIT;
        N7.damage_until = now + 600;
        car_sfx_blip(500);
        return;
    }
    N7.armor--;
    N7.damage_until = now + NS7_INVULN_MS;
    N7.combo = 0; N7.combo_ms = 0;
    ns7_reposition_snake();
    car_sfx_bad();
    if (N7.armor <= 0) {
        N7.over = true;
        N7.new_best = (N7.score > N7.best);
        if (N7.new_best) N7.best = N7.score;
    }
}

/* ----------------------------- game tick --------------------------------- */
static void ns7_step(uint32_t now)
{
    if (N7.has_pend && !(N7.pend.x == -N7.dir_x && N7.pend.y == -N7.dir_y)) {
        N7.dir_x = N7.pend.x; N7.dir_y = N7.pend.y;
    }
    N7.has_pend = false;
    int hx = N7.ring[N7.head_i].x + N7.dir_x;
    int hy = N7.ring[N7.head_i].y + N7.dir_y;
    if (N7.boss_active) hx = ns7_clamp(hx, 0, NS7_COLS - 1);
    if (hy < 0 || hy >= NS7_ROWS) hy = ns7_clamp(hy, 0, NS7_ROWS - 1);

    if (hx < 0 || hx >= NS7_COLS || ns7_on_snake(hx, hy)) {
        ns7_hit_player(now);
        if (!N7.over) ns7_reposition_snake();
        return;
    }
    int new_head = (N7.head_i + 1) % NS7_MAX_SEG;
    N7.ring[new_head] = (ns7_cell_t){ hx, hy };
    N7.head_i = new_head;
    if (N7.grow_pending > 0) { N7.grow_pending--; N7.len++; }
    if (N7.len < 3) N7.len = 3;

    for (int i = 0; i < 2; i++)
        if (N7.core_alive[i] && N7.core[i].x == hx && N7.core[i].y == hy) {
            N7.core_alive[i] = false;
            N7.score += 100 * ns7_mult();
            N7.stage_cores++; N7.cores_total++;
            N7.grow_pending += 2;
            N7.combo++; N7.combo_ms = NS7_COMBO_MS;
            if (N7.combo > N7.max_combo) N7.max_combo = N7.combo;
            car_sfx_score();
            ns7_spawn_core();
            if (N7.stage_cores >= NS7_CORES_PER_STAGE && !N7.boss_active && !N7.boss_warn)
                ns7_stage_up();
        }
    for (int i = 0; i < NS7_MAX_MINE; i++)
        if (N7.mine[i].alive && N7.mine[i].x == hx && N7.mine[i].y == hy) {
            N7.mine[i].alive = false; ns7_hit_player(now);
        }
    for (int i = 0; i < NS7_MAX_EN; i++)
        if (N7.en[i].alive && N7.en[i].x == hx && N7.en[i].y == hy) {
            N7.en[i].alive = false; N7.kills++;
            N7.score += (N7.en[i].kind == 2) ? 300 : 150 * ns7_mult();
            N7.combo++; N7.combo_ms = NS7_COMBO_MS;
            if (N7.combo > N7.max_combo) N7.max_combo = N7.combo;
            car_sfx_blip(900);
        }
    if (N7.boss_active) {
        int by0 = NS_FIELD_TOP + 2, by1 = NS_FIELD_TOP + 48;
        int bx0 = N7.boss_x, bx1 = N7.boss_x + (NS7_COLS - 4) * NS_GRID_STEP;
        int pcx = ns7_cell_px(hx) + NS_GRID_STEP / 2;
        int pcy = ns7_cell_py(hy) + NS_GRID_STEP / 2;
        if (pcx >= bx0 && pcx <= bx1 && pcy >= by0 && pcy <= by1) ns7_hit_player(now);
    }
    if (N7.over) return;

    for (int i = 0; i < NS7_MAX_EN; i++) {
        ns7_enemy_t *e = &N7.en[i];
        if (!e->alive || --e->move_cd > 0) continue;
        e->move_cd = (e->kind == 1) ? 1 : (e->kind == 2) ? 3 : 2;
        int dx = hx - e->x, dy = hy - e->y;
        int tx = e->x, ty = e->y;
        if (dx < 0) tx--; else if (dx > 0) tx++;
        if (dy < 0) ty--; else if (dy > 0) ty++;
        if (tx >= 0 && tx < NS7_COLS && ty >= 0 && ty < NS7_ROWS && !ns7_on_snake(tx, ty))
            e->x = tx, e->y = ty;
    }
}

/* game logic forward declarations */
static void ns7_update_shots(void);

/* -------------------------------- update --------------------------------- */
void g7_update(const xbox360_pad_t *pad, uint32_t now)
{
    ns7_update_shots();
    uint16_t dig = pad ? (uint16_t)(pad->buttons >> 16) : 0;
    uint16_t tap = (uint16_t)(dig & ~N7.prevdig);
    N7.prevdig = dig;

    if (!pad) { N7.lost_pad = true; N7.paused = false; return; }
    N7.lost_pad = false;

    if (tap & B_START && !N7.over && !N7.win) N7.paused = !N7.paused;
    if ((N7.over || N7.win) && (tap & B_A)) { g7_init(); return; }
    if (N7.paused || N7.over || N7.win) return;

    N7.elapsed_ms += NS7_FRAME_MS;
    N7.scroll += 2;

    if (tap & B_DUP)    { N7.pend = (ns7_cell_t){ 0, -1 }; N7.has_pend = true; }
    if (tap & B_DDOWN)  { N7.pend = (ns7_cell_t){ 0,  1 }; N7.has_pend = true; }
    if (tap & B_DLEFT)  { N7.pend = (ns7_cell_t){-1,  0 }; N7.has_pend = true; }
    if (tap & B_DRIGHT) { N7.pend = (ns7_cell_t){ 1,  0 }; N7.has_pend = true; }

    if (tap & B_Y) { N7.mode = !N7.mode; car_sfx_blip(700); }
    if (N7.fire_cd > 0) N7.fire_cd--;
    if ((dig & B_A) && N7.fire_cd == 0 && !N7.overheated) {
        for (int i = 0; i < NS7_MAX_SHOT; i++) {
            if (N7.shot[i].alive) continue;
            int px = ns7_cell_px(N7.ring[N7.head_i].x) + NS_GRID_STEP / 2;
            int py = ns7_cell_py(N7.ring[N7.head_i].y) + NS_GRID_STEP / 2;
            N7.shot[i] = (ns7_shot_t){ px, py, N7.dir_x * 3, N7.dir_y * 3, true, false };
            N7.fire_cd = NS7_FIRE_MS;
            N7.heat = ns7_clamp(N7.heat + 6, 0, 100);
            car_sfx_blip(1500);
            break;
        }
    }
    if (N7.dash_cd > 0) N7.dash_cd -= NS7_FRAME_MS;
    if ((tap & B_X) && N7.dash_cd == 0 && !N7.overheated) {
        N7.dash_cd = NS7_DASH_MS;
        N7.heat = ns7_clamp(N7.heat + 10, 0, 100);
        ns7_step(now); ns7_step(now);
        car_sfx_blip(1200);
    }
    if (tap & B_RS && N7.emp >= 100 && N7.mode == 1) {
        N7.emp = 0;
        for (int i = 0; i < NS7_MAX_EN; i++)
            if (N7.en[i].alive) { N7.en[i].alive = false; N7.kills++; N7.score += 200; }
        for (int i = 0; i < NS7_MAX_MINE; i++) N7.mine[i].alive = false;
        for (int i = 0; i < NS7_MAX_SHOT; i++) N7.shot[i].alive = false;
        N7.score += 500;
        car_sfx_score();
    }
    if (N7.combo_ms > 0) { N7.combo_ms -= NS7_FRAME_MS; if (N7.combo_ms <= 0) N7.combo = 0; }

    /* pickup timers + collisions */
    for (int i = 0; i < NS7_MAX_PK; i++) {
        ns7_pk_t *p = &N7.pk[i];
        if (!p->alive) continue;
        if (N7.elapsed_ms - p->born_ms > NS7_PK_LIFE_MS) { p->alive = false; continue; }
        ns7_cell_t hd = N7.ring[N7.head_i];
        if (p->x == hd.x && p->y == hd.y) {
            p->alive = false;
            N7.combo++; N7.combo_ms = NS7_COMBO_MS;
            if (N7.combo > N7.max_combo) N7.max_combo = N7.combo;
            if (p->kind == 0)      { if (N7.armor < NS7_ARMOR_MAX) N7.armor++; }
            else if (p->kind == 1) { N7.shield = ns7_clamp(N7.shield + 30, 0, 100); }
            else                   { N7.emp = ns7_clamp(N7.emp + 35, 0, 100); }
            N7.toast_kind = 1;
            if (p->kind == 0) snprintf(N7.toast, sizeof(N7.toast), "+ARMOR");
            else if (p->kind == 1) snprintf(N7.toast, sizeof(N7.toast), "+SHIELD");
            else snprintf(N7.toast, sizeof(N7.toast), "+EMP");
            N7.toast_until = N7.elapsed_ms + 900;
            car_sfx_score();
        }
    }
    /* periodic pickup/enemy spawning */
    static uint32_t s_next_spawn = 0;
    if (N7.elapsed_ms > s_next_spawn) {
        s_next_spawn = N7.elapsed_ms + 7000;
        int pk_slot = -1;
        for (int i = 0; i < NS7_MAX_PK; i++) if (!N7.pk[i].alive) { pk_slot = i; break; }
        if (pk_slot >= 0) {
            int pk = (int)(esp_random() % 3);
            ns7_spawn_pickup(pk);
        }
        if (!N7.boss_active && !N7.boss_warn) {
            int ec = 0;
            for (int i = 0; i < NS7_MAX_EN; i++) if (N7.en[i].alive) ec++;
            if (ec < (2 + N7.stage)) {
                int kind = (N7.stage >= 2 && (esp_random() & 1)) ? 1 : 0;
                if (N7.stage >= 3 && (esp_random() % 5) == 0) kind = 2;
                ns7_spawn_enemy(kind);
            }
        }
    }

    /* boss state transitions */
    if (N7.boss_warn && N7.elapsed_ms >= N7.warn_until && !N7.boss_active) ns7_boss_spawn();
    if (N7.boss_active) {
        N7.boss_x += N7.boss_dir * 2;
        int max_x = NS_FIELD_IN_X + NS_FIELD_IN_W - (NS7_COLS - 4) * NS_GRID_STEP;
        if (N7.boss_x < NS_FIELD_IN_X) { N7.boss_x = NS_FIELD_IN_X; N7.boss_dir = 1; }
        if (N7.boss_x > max_x)         { N7.boss_x = max_x;         N7.boss_dir = -1; }
        if (N7.weak_ms > 0) N7.weak_ms--;
        else if (--N7.weak_cd <= 0) { N7.weak_ms = 80; N7.weak_cd = 150; }
        if (--N7.boss_fire_cd <= 0) {
            N7.boss_fire_cd = 40;
            int px = N7.boss_x + (NS7_COLS - 4) * NS_GRID_STEP / 2;
            int py = NS_FIELD_TOP + 48;
            int hx = ns7_cell_px(N7.ring[N7.head_i].x) + NS_GRID_STEP / 2;
            int dx = (hx > px) ? 2 : (hx < px) ? -2 : 0;
            for (int i = 0; i < NS7_MAX_SHOT; i++)
                if (!N7.shot[i].alive) {
                    N7.shot[i] = (ns7_shot_t){ px, py, dx, 3, true, true };
                    break;
                }
        }
    }

    /* run snake ticks */
    if (N7.next_tick == 0) N7.next_tick = now + N7.tick_ms;
    int guard = 0;
    while (now >= N7.next_tick && guard++ < 3) {
        ns7_step(now);
        N7.next_tick += N7.tick_ms;
        if (N7.over) break;
    }
}

/* ------------------------------ projectiles ------------------------------ */
static void ns7_update_shots(void)
{
    for (int i = 0; i < NS7_MAX_SHOT; i++) {
        ns7_shot_t *s = &N7.shot[i];
        if (!s->alive) continue;
        s->x += s->dx; s->y += s->dy;
        if (s->x < NS_FIELD_IN_X - 4 || s->x > NS_FIELD_IN_X + NS_FIELD_IN_W + 4 ||
            s->y < NS_FIELD_IN_Y - 4 || s->y > NS_FIELD_IN_Y + NS_FIELD_IN_H + 4) {
            s->alive = false; continue;
        }
        int cx = (s->x - NS_FIELD_IN_X) / NS_GRID_STEP;
        int cy = (s->y - NS_FIELD_IN_Y) / NS_GRID_STEP;
        if (s->hostile) continue;
        for (int m = 0; m < NS7_MAX_MINE; m++)
            if (N7.mine[m].alive && N7.mine[m].x == cx && N7.mine[m].y == cy) {
                N7.mine[m].alive = false; s->alive = false; N7.score += 50;
                car_sfx_blip(1200);
            }
        for (int e = 0; e < NS7_MAX_EN; e++)
            if (N7.en[e].alive && N7.en[e].x == cx && N7.en[e].y == cy) {
                ns7_enemy_t *en = &N7.en[e];
                en->hp--; s->alive = false;
                if (en->hp <= 0) {
                    en->alive = false; N7.kills++;
                    N7.score += (en->kind == 2) ? 300 : 150 * ns7_mult();
                    N7.combo++; N7.combo_ms = NS7_COMBO_MS;
                    if (N7.combo > N7.max_combo) N7.max_combo = N7.combo;
                    car_sfx_blip(900);
                }
                break;
            }
        if (s->alive && N7.boss_active) {
            int bx0 = N7.boss_x, bx1 = N7.boss_x + (NS7_COLS - 4) * NS_GRID_STEP;
            int by0 = NS_FIELD_TOP + 2, by1 = NS_FIELD_TOP + 48;
            if (s->x >= bx0 && s->x <= bx1 && s->y >= by0 && s->y <= by1) {
                s->alive = false;
                N7.boss_hp -= (N7.weak_ms > 0) ? 3 : 1;
                N7.score += (N7.weak_ms > 0) ? 75 : 25;
                car_sfx_blip(1100);
                if (N7.boss_hp <= 0) ns7_boss_down();
            }
        }
    }
}

/* -------------------------------- draw ----------------------------------- */
/* -------------------------------- draw ----------------------------------- */
void g7_draw(uint32_t now)
{
    ns_view_t v;
    memset(&v, 0, sizeof(v));
    v.score      = N7.score;
    v.armor      = N7.armor;
    v.armor_max  = NS7_ARMOR_MAX;
    v.shield     = N7.shield;
    v.shield_max = 100;
    v.heat       = N7.heat;
    v.heat_max   = 100;
    v.mode_name  = (N7.mode == 0) ? "SHIELD" : "EMP";
    v.combo      = N7.combo;
    v.stage      = N7.stage;
    v.stage_max  = NS7_STAGE_MAX;
    v.emp        = N7.emp;
    v.emp_max    = 100;
    v.dash       = (NS7_DASH_MS - N7.dash_cd) / ((NS7_DASH_MS / 100) + 1);
    v.dash_max   = 100;
    v.dash_recharging = (N7.dash_cd > 0);
    v.boss_active = N7.boss_active;
    v.boss_name   = "GRID WARDEN";
    v.boss_hp     = N7.boss_hp;
    v.boss_hp_max = NS7_BOSS_HP;
    v.grid_scroll = N7.scroll;
    v.time_s      = N7.elapsed_ms / 1000;
    v.cores       = N7.cores_total;
    v.enemies     = N7.kills;
    v.max_combo   = N7.max_combo;
    v.new_best    = N7.new_best;

    if (N7.lost_pad)           v.overlay = NS_OVERLAY_CONTROLLER_LOST;
    else if (N7.paused)        v.overlay = NS_OVERLAY_PAUSE;
    else if (N7.win)           v.overlay = NS_OVERLAY_WIN;
    else if (N7.over)          v.overlay = NS_OVERLAY_GAME_OVER;

    if (N7.boss_warn && !N7.boss_active && v.overlay == NS_OVERLAY_NONE)
        v.banner = NS_BANNER_BOSS_WARNING;
    else if (N7.toast_kind == 1 && N7.elapsed_ms < N7.toast_until)
        { v.banner = NS_BANNER_PICKUP; v.banner_text = N7.toast; }

    if (!N7.boss_active) {
        if (N7.stage_cores < NS7_CORES_PER_STAGE)
            snprintf(N7.objective, sizeof(N7.objective), "CORES %02d/%02d",
                     N7.stage_cores, NS7_CORES_PER_STAGE);
        else
            snprintf(N7.objective, sizeof(N7.objective), "STAGE CLEAR!");
    } else {
        snprintf(N7.objective, sizeof(N7.objective), "DEFEAT GRID WARDEN");
    }
    v.objective = N7.objective;

    ns_begin_frame(&v);

    if (v.overlay == NS_OVERLAY_NONE) {
        for (int i = 0; i < 2; i++)
            if (N7.core_alive[i])
                ns_draw_pickup(ns7_cell_px(N7.core[i].x) + 2,
                               ns7_cell_py(N7.core[i].y) + 2, 11, NS_PICKUP_CORE, now);
        for (int i = 0; i < NS7_MAX_PK; i++)
            if (N7.pk[i].alive)
                ns_draw_pickup(ns7_cell_px(N7.pk[i].x) + 2,
                               ns7_cell_py(N7.pk[i].y) + 2, 11,
                               (ns_pickup_kind_t)N7.pk[i].kind, now);
        for (int i = 0; i < NS7_MAX_MINE; i++)
            if (N7.mine[i].alive)
                ns_draw_mine(ns7_cell_px(N7.mine[i].x) + 2,
                             ns7_cell_py(N7.mine[i].y) + 2, 11, now);
        for (int i = 0; i < NS7_MAX_EN; i++)
            if (N7.en[i].alive)
                ns_draw_enemy(ns7_cell_px(N7.en[i].x) + 2,
                              ns7_cell_py(N7.en[i].y) + 2, 13,
                              (ns_enemy_kind_t)N7.en[i].kind);
        if (N7.boss_active) {
            int bw = (NS7_COLS - 4) * NS_GRID_STEP;
            int bx0 = N7.boss_x;
            bool weak = N7.weak_ms > 0;
            gfx_rect(bx0, NS_FIELD_TOP + 2, bw, 46, NS_ENEMY_PURPLE);
            gfx_rect(bx0 + 2, NS_FIELD_TOP + 4, bw - 4, 42, NS_MAGENTA);
            gfx_rect(bx0 + 4, NS_FIELD_TOP + 6, bw - 8, 38, NS_PANEL_DARK);
            if (weak) gfx_rect(bx0 + 4, NS_FIELD_TOP + 6, bw - 8, 38, NS_GREEN);
            int eye = (now / 100) % 20;
            gfx_rect(bx0 + bw / 2 - eye, NS_FIELD_TOP + 20, 2, 6, NS_WHITE);
            gfx_rect(bx0 + bw / 2 + eye, NS_FIELD_TOP + 20, 2, 6, NS_WHITE);
        }
        for (int i = 0; i < NS7_MAX_SHOT; i++)
            if (N7.shot[i].alive)
                ns_draw_projectile(N7.shot[i].x, N7.shot[i].y,
                                   N7.shot[i].dx, N7.shot[i].dy,
                                   N7.shot[i].hostile ? NS_RED : NS_CYAN);
        for (int k = N7.len - 1; k >= 0; k--) {
            ns7_cell_t *s = &N7.ring[ns7_seg_idx(k)];
            ns_draw_snake_segment(ns7_cell_px(s->x) + 2, ns7_cell_py(s->y) + 2,
                                  NS_GRID_STEP - 4, k, (k == 0), N7.dir_x, N7.dir_y);
        }
    }

    ns_end_frame(&v, now);
}
