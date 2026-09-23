/*
 * imu_adv.h - MPU-6500 ADVANCED SYSTEM (spec 1.md PART 4, from 6.0 full detail).
 *
 * Dual-speed sampling architecture:
 *   FAST TASK (100 Hz, this module) -> impact/freefall/rollover/slip/drift
 *   SLOW TASK (10-30 Hz, existing car_task LOOP_MS + imu_driver_poll) -> UI smoothing
 * Both share one I2C bus (MPU device at 100 kHz, OLED at 400 kHz).
 * Fast task NEVER touches I2C directly: it consumes
 * the cached imu_data_t (motion_radar_imu_read) refreshed by the slow path.
 * This gives zero bus contention by design (verified headroom, no conflict).
 * Hardware free-fall/motion INT register is used if MPU INT pin wired;
 * else 100 Hz software polling fallback (this build: fallback, INT not wired).
 *
 * Event bus: FreeRTOS queue, consumed by car.c / cockpit / alexa_bridge / rumble.
 *
 * SAFETY NOTE: these are ADDITIVE to GUIDE E-STOP, never a replacement.
 * Existing E-STOP clear rule (GUIDE tap + stationary + 250 ms neutral-release
 * lock) remains the master override, unchanged.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------- exact thresholds (4.2) ------------------------ */
#define IMU_TILT_WARN_DEG        15.0f
#define IMU_TILT_CRITICAL_DEG    25.0f
#define IMU_TILT_ROLLOVER_DEG    35.0f
#define IMU_IMPACT_LIGHT_G       1.5f
#define IMU_IMPACT_MODERATE_G    2.5f
#define IMU_IMPACT_HARD_G        4.0f
/* FREEFALL: <0.3 g sustained >150 ms | LANDING: spike >2 g within 100 ms after */
#define IMU_FREEFALL_G           0.3f
#define IMU_FREEFALL_MS          150
#define IMU_LANDING_G            2.0f
#define IMU_LANDING_WINDOW_MS    100
#define IMU_YAW_CORNER_WARN      45.0f
#define IMU_YAW_CORNER_HARD      90.0f
/* SLIP: measured <40% expected sustained >300 ms; STUCK: <10% >800 ms */
#define IMU_SLIP_RATIO           0.40f
#define IMU_SLIP_MS              300
#define IMU_STUCK_RATIO          0.10f
#define IMU_STUCK_MS             800
/* DRIFT: |actual_yaw - steering_implied| >30 deg/s sustained >250 ms */
#define IMU_DRIFT_DPS            30.0f
#define IMU_DRIFT_MS             250
/* STATIONARY: |accel-1g|<0.05 g AND |gyro|<2 deg/s for >2 s -> drift-correct */
#define IMU_STAT_ACC_TOL_G       0.05f
#define IMU_STAT_GYRO_DPS        2.0f
#define IMU_STATIONARY_MS        2000

/* ------------------------------ event bus ------------------------------- */
typedef enum {
    IMU_EVT_NONE = 0,
    IMU_EVT_TILT_WARN,
    IMU_EVT_TILT_CRITICAL,
    IMU_EVT_TILT_ROLLOVER,
    IMU_EVT_TILT_CLEAR,
    IMU_EVT_IMPACT_LIGHT,
    IMU_EVT_IMPACT_MODERATE,
    IMU_EVT_IMPACT_HARD,
    IMU_EVT_FREEFALL,
    IMU_EVT_LANDING_SMOOTH,
    IMU_EVT_LANDING_HARD,
    IMU_EVT_CORNER_WARN,
    IMU_EVT_CORNER_HARD,
    IMU_EVT_SLIP,          /* -> SLIPPING */
    IMU_EVT_STUCK,         /* -> STUCK */
    IMU_EVT_TRACTION_OK,   /* back to GRIP */
    IMU_EVT_DRIFT_ON,
    IMU_EVT_DRIFT_OFF,
    IMU_EVT_STATIONARY,    /* drift-corrected anchor */
} imu_adv_evt_type_t;

typedef struct {
    imu_adv_evt_type_t type;
    float              mag;        /* g / deg / deg/s depending on type */
    uint32_t           now_ms;
} imu_adv_evt_t;

/* traction pill states (3.3) */
typedef enum {
    IMU_TRACTION_GRIP = 0,
    IMU_TRACTION_SLIPPING = 1,
    IMU_TRACTION_STUCK = 2,
} imu_traction_t;

/* stability shield (3.3 upgraded, multi-factor) */
typedef enum {
    IMU_SHIELD_GREEN = 0,
    IMU_SHIELD_AMBER = 1,
    IMU_SHIELD_RED = 2,
} imu_shield_t;

/* terrain bar (Sensor sub-view only) */
typedef enum {
    IMU_TERRAIN_SMOOTH = 0,
    IMU_TERRAIN_MODERATE = 1,
    IMU_TERRAIN_ROUGH = 2,
} imu_terrain_t;

/* cockpit snapshot (single call for UI, no tearing concerns for display) */
typedef struct {
    bool     valid;
    float    pitch_deg;      /* slow-smoothed */
    float    roll_deg;       /* slow-smoothed */
    float    heading_rel;    /* gyro-integrated RELATIVE heading, -180..180 */
    float    yaw_rate_dps;
    float    g_lat;          /* lateral G (yg) */
    float    g_lon;          /* longitudinal G (xg, compensated) */
    float    g_mag;
    imu_traction_t traction;
    bool     airborne;
    uint32_t airtime_ms;
    char     landing_msg[24];/* "LANDING: SMOOTH/HARD" or "" */
    uint32_t landing_until;  /* auto-dismiss deadline (1.5 s) */
    bool     drift;
    imu_shield_t shield;
    uint8_t  shield_score;   /* 0..100 */
    imu_terrain_t terrain;
    float    vib_var;        /* vibration variance (g^2) */
    bool     tilt_lockout;   /* sticky rollover, manual ack required */
    bool     tilt_critical;  /* motors forced 0 while active */
    uint8_t  corner_cap;     /* 0 none, 1 soft, 2 hard (speed-cap during turn) */
    uint8_t  last_impact;    /* 0 none, 1 light, 2 moderate, 3 hard */
    uint32_t last_impact_ms;
    /* drive score (4.4) */
    uint16_t drive_score;    /* 0..100 weighted */
    char     drive_grade;    /* A..F */
    /* PART 9/10: score components + counters for voice queries + windows */
    uint8_t  sc_corner, sc_brake, sc_bumps, sc_stable, sc_traction, sc_tilt;
    uint32_t impact_hard_cnt;   /* session count (proactive + "today" queries) */
    uint32_t last_airtime_ms;   /* most recent airtime (announce text) */
    float    tilt_deg;           /* max(|pitch|,|roll|) smoothed */
} imu_adv_snap_t;

/* lifecycle */
void imu_adv_init(void);                 /* create queue + fast task (idempotent) */
bool imu_adv_ready(void);
uint32_t imu_adv_stack_free(void);       /* PART 16 mem_diag */

/* slow path: called from car_task each LOOP_MS tick (10-30 Hz UI smoothing).
   pitch/roll/yaw_rate/accel come from motion_radar_imu_read cache. */
void imu_adv_slow_update(float pitch, float roll, float yaw_rate,
                         float ax_g, float ay_g, float az_g, uint32_t now_ms);

/* motion intent for slip/stuck/drift model (call each tick from car_task) */
void imu_adv_set_motion(int16_t tgt_l, int16_t tgt_r,
                        int16_t cur_l, int16_t cur_r, uint32_t now_ms);

/* heading: RELATIVE only (no magnetometer, never true north) */
float imu_adv_heading_rel(void);         /* -180..180 */
void  imu_adv_reset_heading(void);       /* RS-hold-1 s manual reset */

/* event bus: non-blocking receive, returns false when empty */
bool imu_adv_evt_recv(imu_adv_evt_t *out);

/* snapshot for cockpit / alexa / score */
void imu_adv_snapshot(imu_adv_snap_t *out);

/* behavior queries for car.c (4.3) */
bool imu_adv_motors_cut(void);           /* tilt critical/rollover/freefall active */
bool imu_adv_rollover_latched(void);
void imu_adv_ack_rollover(void);         /* manual acknowledge (E-STOP-class).
                                            Alexa MUST NOT call this. */
uint8_t imu_adv_corner_cap(void);        /* 0/1/2 */
bool imu_adv_airborne(void);

/* drive score (4.4): weighted average -> grade A-F */
uint16_t imu_adv_drive_score(void);
char     imu_adv_drive_grade(void);
/* PART 10 trip window: session hard-bump count */
uint32_t trip_hard_bumps(void);

#ifdef __cplusplus
}
#endif
