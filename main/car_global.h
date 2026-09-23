/*
 * car_global.h - shared car state + defines (Car OS modules read g.* directly)
 *
 * The struct definition moved here from car.c (fields identical + new "gear").
 * car.c defines:  car_state_t g;
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------- buttons -------------------------------- */
/* pad->buttons >> 16 = XInput digital mask */
#define B_DUP    0x0001
#define B_DDOWN  0x0002
#define B_DLEFT  0x0004
#define B_DRIGHT 0x0008
#define B_START  0x0010
#define B_BACK   0x0020
#define B_LS     0x0040
#define B_RS     0x0080
#define B_LB     0x0100
#define B_RB     0x0200
#define B_GUIDE  0x0400
#define B_A      0x1000
#define B_B      0x2000
#define B_X      0x4000
#define B_Y      0x8000

/* -------------------------------- modes --------------------------------- */
#define MODE_MANUAL  0
#define MODE_CRAWL   1
#define MODE_AUTO    2

/* auto state machine */
#define AST_CRUISE   0
#define AST_SLOW     1
#define AST_TURN_L   2
#define AST_TURN_R   3
#define AST_REVERSE  4
#define AST_ESCAPE   5
#define AST_STUCK    6
#define AST_PAUSE    7   /* wait for dynamic obstacle */
#define AST_SEARCH   8   /* 360 scan for path */

#define CAR_GEAR_COUNT 5

/* ----------------------------- global state ----------------------------- */
typedef struct car_state {
    uint8_t mode;               /* MODE_* */
    uint8_t ast;                /* auto state */
    uint32_t ast_t0;            /* auto state entry time (ms) */
    uint8_t esc_turn;           /* escape turn direction 0=left 1=right */
    uint32_t esc_t0;            /* escape phase time */
    uint8_t esc_phase;          /* 0=reverse 1=forward */
    uint16_t prog_f;            /* progress anchor (cm) */
    uint32_t prog_t;            /* progress anchor time */
    uint16_t dist[4];           /* 0=left 1=front 2=right 3=rear (cm, 65535 = clear) */
    uint16_t dist_buf[4][3];    /* moving average buffer (3 samples per sensor) */
    uint8_t dist_idx;           /* circular buffer index */
    uint16_t dist_avg[4];       /* averaged distances for auto decisions */
    uint8_t stuck_cnt;          /* auto-retry counter for STUCK */
    uint8_t search_dir;         /* search rotation direction: 0=left 1=right */

    bool headlight;
    bool hazard;
    bool sig_l, sig_r;          /* manual indicators */
    uint8_t led_mode;           /* 0 off 1 static 2 rainbow 3 chase 4 siren */

    bool gripper_open;           /* legacy - D-pad Down now rear light toggle */
    uint32_t grip_hit_ms;
    bool highbeam;              /* Y = high beam */
    bool snd_mute;              /* X hold = engine sound off/on */
    bool cannon_reset;          /* RS click pending recenter */

    bool turbo, cooldown;
    uint32_t turbo_ms;
    uint32_t cooldown_until;

    bool    mist, roof_on;              /* roof_on: legacy flag (roof light ab WS2812 GPIO18, roof_light.c owner) */
    bool    rear_light_on;          /* rear light relay (GPIO3) - default ON, D-pad Down toggle */
    bool estop;                 /* emergency stop latched */

    int16_t tgt_l, tgt_r;       /* commanded -100..100 */
    int16_t cur_l, cur_r;       /* ramped -100..100 */

    bool reversing;             /* reverse beep + mist */
    bool braking;               /* brake light + spoiler air brake */
    bool brake_on;              /* L298N real brake (manual LT 200+) */

    // battery removed - GPIO1 now for backlight relay
    // uint16_t batt_mv; bool batt_valid; bool batt_warn, batt_crit; REMOVED

    uint32_t last_input_ms;

    uint8_t gear;               /* Car OS: virtual gear 0..4 (gear_caps cap speed) */

} car_state_t;

extern car_state_t g;

/* --------------------- Car OS / external API (car.c) -------------------- */
float    car_get_yaw(void);                        /* gyro yaw degrees */
uint8_t  car_get_setting(uint8_t idx);             /* 0 spd cap 1 vol 2 obst 3 led */
void     car_set_setting(uint8_t idx, uint8_t v);
uint8_t  car_get_gear_cap(uint8_t i);              /* 0..4 */
void     car_set_gear_cap(uint8_t i, uint8_t v);
void     car_settings_save(void);                  /* NVS commit all settings */
uint16_t car_last_dig(void);                       /* last raw button mask (debug) */
void     car_roof_apply_saved(void);               /* apply stored roof pattern/brightness */
void     car_cycle_gear(void);                     /* g.gear = (g.gear+1)%5 */
uint8_t  car_speed_cap_pct(void);                  /* min(speed_cap, gear_caps[gear]) */
void     car_oled_hud(void);                       /* legacy OLED mini HUD render */
void     car_sfx_click(void);                      /* short UI click */
void     car_sfx_score(void);                      /* score blip */
void     car_sfx_bad(void);                        /* error/crash */
void     car_sfx_blip(uint16_t freq);              /* tone blip */
/* PART 5 sound bank voice: 16-bit mono 16kHz PCM, mixer-resampled to out rate */
void     car_snd_play_pcm(const uint8_t *d, uint32_t len_bytes, uint16_t vol);
/* PART 10 trip computer (odometry estimated from wheel commands) */
uint32_t trip_time_s(void);
uint32_t trip_dist_m(void);
uint16_t trip_top_speed(void);
uint16_t trip_avg_speed(void);
float    trip_max_tilt(void);
void     trip_reset(void);
bool     car_us_front_ok(void);                    /* front sensor echo valid */

/* ------------------- OLED driver wrappers (car.c) ----------------------- */
void osd_oled_clear(void);
void osd_oled_rect(int x1, int y1, int x2, int y2, bool fill);
void osd_oled_text(int x, int y, const char *s);
void osd_oled_text_big(int x, int y, const char *s, int scale);
void osd_oled_flush(void);

#ifdef __cplusplus
}
#endif