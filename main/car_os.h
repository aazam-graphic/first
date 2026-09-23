/*
 * car_os.h - Car OS state machine + navigation (raw s_fb architecture).
 *
 * Screens: BOOT -> HOME launcher -> DRIVE / ANALYTICS / OLED-CTRL / SETTINGS /
 * GAMES-HUB / DIAG + GAME_1..5. Car control loop stays untouched; the OS only
 * reads g.* (car_global.h) and draws into the TFT framebuffer.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "xbox360.h"
#include "car_global.h"
#include "roof_light.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OS_GAME_COUNT 2
#define OS_HIST_N     480          /* analytics ring buffers (PSRAM) */
#define SET_ITEM_COUNT 14         /* settings items incl. SAVE (guide 5.6/11) */

/* drive alerts (guide 5.2) - enum order = priority (lower value wins) */
typedef enum {
    ALERT_NONE = 0,
    ALERT_ESTOP,        /* sticky: never auto-dismisses, cleared on estop clear */
    ALERT_PAD_LOST,     /* controller disconnected */
    ALERT_SENSOR,       /* front sensor fault */
    ALERT_OBSTACLE,     /* front obstacle below threshold */
} os_alert_type_t;

typedef enum {
    OS_BOOT = 0,
    OS_HOME,
    OS_DRIVE_MAIN,
    OS_DRIVE_ANALYTICS,
    OS_DIAG,
    OS_SETTINGS,
    OS_OLED_CTRL,
    OS_GAMES_HUB,
    OS_GAME_1,
    OS_GAME_2,
    OS_GAME_3,
    OS_GAME_4,
    OS_GAME_5,
    OS_GAME_6,
    OS_ALEXA_LOG,      /* Alexa voice history (blueprint §9B) */
    OS_SCORE,          /* PART 10: Drive Score window */
    OS_TRIP,           /* PART 10: Trip computer */
    OS_CONN,           /* PART 10: Connectivity */
    OS_NOTIF,          /* PART 10: Notification Center */
    OS_STANDBY,        /* CLOCK/STANDBY: table clock, parked, START-only */
    OS_DRIVE_ARMING,   /* DRIVE pre-stage: 1.3 s anim+chime, neutral gate */
} os_state_t;

/* unified input context (premium guide Â§3) - single source of truth. car.c
   derives motor safety + button capture from os_context(); the old separate
   ui_capture/menu_active flags are gone so they can never disagree. */
typedef enum {
    INPUT_CTX_BOOT = 0,   /* boot: everything locked except Guide       */
    INPUT_CTX_DRIVE,      /* drive HUD: car control enabled              */
    INPUT_CTX_OS,         /* any OS menu: parked, buttons = UI only      */
    INPUT_CTX_GAME,       /* games: parked, buttons = game actions       */
    INPUT_CTX_ESTOP,      /* emergency stop: hard locked                 */
    INPUT_CTX_STANDBY,    /* clock/standby: parked, START-only (9.0)     */
    INPUT_CTX_ARMING,     /* drive arming: parked, B-cancel only         */
} input_context_t;

/* settings draft (guide 5.6): copy of live settings so edits can be
   applied item-by-item (A), saved to NVS (START), or discarded (B). */
typedef struct {
    uint8_t spd_cap;        /* setting 0 */
    uint8_t engine_vol;     /* setting 1 */
    uint8_t obstacle_cm;    /* setting 2 */
    uint8_t led_bright;     /* setting 3 */
    uint8_t mist_max;       /* setting 4: mist max run x10s, 0 = unlimited */
    uint8_t tft_bright;     /* setting 5: TFT backlight % */
    uint8_t gear_caps[5];   /* gear 1..5 caps */
    uint8_t oled_layout;    /* 0..3 */
    uint8_t hud_layout;     /* 0=full 1=compact 2=night (guide 11 Display) */
} os_settings_draft_t;

typedef struct {
    os_state_t state;
    os_state_t prev_state;
    uint8_t  home_sel;          /* HOME focus 0..8 (persistent per screen) */
    uint8_t  home_page;         /* PART 10: HOME pages 0..1 */
    uint8_t  oled_sel;          /* OLED control focus 0..2    (persistent per screen) */
    uint8_t  game_sel;          /* GAMES HUB focus 0..4       (persistent per screen) */
    uint8_t  settings_sel;      /* SETTINGS focus 0..10       (persistent per screen) */
    bool     settings_jump;     /* PART 10: jump-list overlay open */
    uint8_t  jump_sel;          /* PART 10: jump-list focus */
    uint8_t  diag_tab;          /* DIAG tab 0=SYS 1=SENS 2=CTRL 3=DISPLAY 4=THEME */
    uint8_t  gear;              /* mirror of g.gear for UI */
    uint8_t  gear_caps[5];      /* mirror for UI */
    uint8_t  engine_vol;
    uint8_t  led_bright;
    uint8_t  obs_cm;
    uint8_t  oled_layout;       /* 0=mini HUD 1=radar 2=STATUS 3=text */
    char     oled_text[32];
    uint32_t next_tft_ms;           /* next TFT frame deadline (guide 6.2) */
    uint32_t next_oled_ms;          /* next OLED frame deadline */
    uint32_t state_enter_ms;

    /* settings draft/save/cancel model (guide 5.6) */
    os_settings_draft_t draft;
    bool settings_dirty;        /* draft differs from live settings */
    bool settings_loaded;       /* draft snapshot taken for this entry */
    uint8_t settings_confirm;   /* 0 none, 1 = discard-changes dialog */

    /* analytics history (guide 2.5) */
    bool     analytics_available;  /* history buffers allocated */
    uint16_t analytics_hist_n;     /* active ring size: 480 PSRAM / 64 fallback */

    /* drive alerts (guide 5.2): event lifecycle, 3s auto-dismiss except ESTOP */
    os_alert_type_t alert_type;
    uint32_t        alert_start_ms;
    bool            alert_active;
    char            alert_message[48];

    /* quick overlay (guide 5.9 / premium §10): START hold on Drive screen */
    bool    quick_open;
    uint8_t quick_row;              /* PART 10 QCC: 0=CHIPS 1=BRIGHT 2=VOL 3=THEME 4=PROFILE 5=EXIT */
    uint8_t quick_col;              /* PART 10 QCC chips: 0=HEAD 1=HAZ 2=ROOF 3=MUTE */

    /* premium guide extras */
    bool    roof_panel;             /* B hold : roof light quick panel open   */
    uint8_t roof_row;               /* 0=PATTERN 1=BRIGHTNESS                */
    bool    auto_prev;              /* X tap : AUTO preview overlay open     */
    uint8_t hud_layout;             /* 0=full 1=compact 2=minimal/night HUD  */
    roof_light_mode_t roof_prev_mode; /* roof panel cancel restore           */
    uint8_t roof_prev_br;
    uint8_t roof_prev_col;            /* PART 7: steady color cancel restore */
    char    toast[40];              /* drive HUD toast (1.2 s)               */
    uint32_t toast_until;

    /* system actions in DIAG > SYSTEM (premium guide 4/11): Restart and
       Factory reset both need A hold 1.5 s (dangerous-confirm timing) */
    uint8_t  diag_item;             /* 0=none 1=RESTART 2=FACTORY RESET      */
    uint32_t sys_hold_t0;           /* A-press timestamp while confirming    */

    /* ambient color picker (premium guide 5.2): Y hold 700 ms on Drive */
    bool    picker_open;
    uint8_t picker_sel;             /* 0..5 swatch index                     */
    uint8_t picker_prev_mode;       /* g.led_mode before opening (B cancel)  */
    uint8_t picker_prev_r, picker_prev_g, picker_prev_b;
    uint8_t drive_sub_view;         /* cockpit VIEW 0..3 (1.md 3.4, BACK tap cycles) */
    uint8_t cockpit_theme;          /* 0 NIGHT 1 SOLAR (default) 2 SUNLIGHT (BACK hold cycles) */
    uint8_t radar_view;             /* 0=visual radar 1=details page          */
    uint32_t radar_cal_ms;          /* monotonic ms of last A (CAL) action     */
    uint8_t gear_flash;             /* gear change flash counter (guide §10) */
    uint32_t gear_flash_until;      /* flash expiry */
    char     gear_pop[8];           /* PART 12 paddle popup: "G4"/"MIN"/"MAX" */
    uint32_t mic_vu_until;          /* PART 12: X double-tap VU overlay deadline */
    bool     radar_full;            /* PART 12: BACK 1.5s Motion Radar screen */

    /* PART 10 window stack (max depth 3) + app switcher */
    os_state_t win_st[3];
    uint8_t win_depth;
    bool switch_open;               /* START double-tap app switcher overlay */
    uint8_t switch_sel;             /* switcher focus 0..3 */
    uint32_t start_home_at;         /* PART 12: deferred START-tap HOME stamp */
} os_ctx_t;

/* PART 10 window stack helpers */
void os_win_push(os_ctx_t *ctx, os_state_t st);  /* push current, cap depth 3 */
bool os_win_pop(os_ctx_t *ctx, uint32_t now);    /* pop to previous, false if empty */
void os_win_breadcrumb(const os_ctx_t *ctx, char *out, size_t n);
/* app switcher thumbnails (160x120 RGB565 PSRAM, last 4 windows) */
void os_thumb_capture(os_state_t st);
const uint16_t *os_thumb_get(os_state_t st, uint16_t *w, uint16_t *h);
int os_hist_win_list(os_state_t *out, int max);

/* analytics history (allocated in PSRAM by os_init) */
extern int16_t *os_h_spd;
extern int16_t *os_h_front;
extern int16_t *os_h_yaw;
uint16_t os_hist_pos(void);      /* current write index of the ring buffers */
uint16_t os_hist_n(void);        /* active ring size (480 PSRAM / 64 fallback) */

/* drive alerts (guide 5.2) */
void os_alert(os_ctx_t *ctx, os_alert_type_t type, const char *msg, uint32_t now);
void os_alert_update(os_ctx_t *ctx, uint32_t now);   /* auto-dismiss + housekeeping */

/* performance counters (guide 9) */
typedef struct {
    uint32_t frame_us;        /* last render time */
    uint32_t max_frame_us;    /* peak render time */
    uint32_t push_us;         /* last SPI push time */
    uint32_t max_push_us;     /* peak push time */
    uint16_t fps;             /* measured TFT FPS (1s window) */
    uint32_t frames;          /* total frames pushed */
    uint32_t skipped_frames;  /* unchanged frames skipped */
} os_perf_t;
const os_perf_t *os_perf(void);

/* neutral-release lock (premium guide 2.6/12): true while drive inputs are
   ignored after re-entering Drive from OS/Game/E-stop (250 ms neutral rule) */
bool car_drive_locked(void);

/* system actions (premium guide 4/11) - called from DIAG > SYSTEM with
   A hold 1.5 s confirmation. Factory erases NVS + defaults; restart reboots. */
void car_settings_factory_reset(void);
void car_system_restart(void);

/* ambient color picker palette (premium guide 5.2) */
extern const uint8_t os_ambient_palette[6][3];
const char *os_ambient_name(uint8_t idx);

void os_init(os_ctx_t *ctx);
void os_handle_input(os_ctx_t *ctx, const xbox360_pad_t *pad, uint16_t dig, uint16_t tap, uint32_t now);
void os_update(os_ctx_t *ctx, uint32_t now);

/* ---- Alexa bridge entry points (called from alexa_bridge, car_task ctx) -- */
void      os_request_screen(os_ctx_t *ctx, int st, uint32_t now);
void      os_alexa_save_settings(os_ctx_t *ctx);
os_ctx_t *alexa_os_ctx(void);

void os_draw_tft(os_ctx_t *ctx, uint32_t now);
void os_draw_oled(os_ctx_t *ctx, uint32_t now);
void os_game_frame(os_ctx_t *ctx, const xbox360_pad_t *pad, uint32_t now);

bool os_input_captured(void);   /* true: OS owns buttons (menus/games)  */
input_context_t os_context(void); /* active input context (guide Â§3)     */
bool os_parked(void);           /* true: motors must stay 0             */
bool os_game_active(void);
const char *os_state_name(os_state_t st);
const char *os_state_hint(os_state_t st);

#ifdef __cplusplus
}
#endif
