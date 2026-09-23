/*
 * imu_adv.c - MPU-6500 ADVANCED SYSTEM (spec 1.md PART 4).
 *
 * FAST TASK (100 Hz): impact/freefall/rollover/slip/drift/corner/stationary.
 * SLOW PATH (car_task LOOP_MS 40 ms): UI smoothing via imu_adv_slow_update.
 * Fast task never touches I2C: it consumes the motion_radar_imu_read() cache
 * refreshed by imu_driver_poll() in the slow path. Zero bus contention.
 * INT pin not wired on this board -> 100 Hz software polling fallback.
 */
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "imu_adv.h"
#include "imu_driver.h"
#include "car_os.h"     /* standby gate: sensors OFF in CLOCK_IDLE */

static const char *TAG = "imu_adv";

static QueueHandle_t s_q;
static SemaphoreHandle_t s_mtx;
static TaskHandle_t s_task;
static bool s_ready;

/* ------------------------------- state ---------------------------------- */
static struct {
    bool     valid;
    float    heading_rel;      /* -180..180, gyro-integrated RELATIVE */
    float    pitch_sm, roll_sm, yaw_sm;
    float    g_lat, g_lon, g_mag;
    imu_traction_t traction;
    uint32_t slip_low_since;  /* 0 = not in low-traction timing */
    uint32_t stuck_low_since;
    uint32_t slip_ms_total;
    uint32_t stuck_ms_total;
    bool     drift;
    uint32_t drift_high_since;
    uint32_t drift_low_since;
    bool     airborne;
    uint32_t airtime_start;
    uint32_t ff_low_since;    /* freefall <0.3g accumulator */
    char     landing_msg[24];
    uint32_t landing_until;
    uint8_t  tilt_state;      /* 0 ok 1 warn 2 crit 3 roll */
    bool     tilt_lockout;    /* sticky rollover */
    bool     tilt_crit_now;
    uint32_t tilt_warn_cnt;
    uint32_t tilt_warn_ms_total;
    uint32_t tilt_warn_since;
    uint8_t  corner_cap;      /* 0/1/2 */
    uint32_t corner_hold_until;
    uint32_t corner_warn_last;/* debounce */
    uint32_t corner_hard_last;
    uint32_t corner_hard_cnt;
    uint8_t  last_impact;
    uint32_t last_impact_ms;
    uint32_t impact_mod_cnt;  /* moderate+hard count (score) */
    uint32_t impact_hard_cnt; /* PART 9: hard-only session count */
    uint32_t impact_last_ms;  /* debounce */
    uint32_t last_airtime_ms; /* PART 9: most recent airtime for announce */
    uint32_t hard_decel_cnt;
    bool     stationary;
    uint32_t stat_since;
    /* motion intent */
    int16_t  tgt_l, tgt_r, cur_l, cur_r;
    uint32_t motion_ms;
    float    last_speed;
    float    yaw_lp;         /* PART 7.0 §4: low-passed yaw for integration */
    uint32_t last_speed_ms;
    int16_t  speed_ref;       /* for hard-decel window */
    uint32_t speed_ref_ms;
    /* vibration / terrain */
    float    vib_ring[32];
    uint8_t  vib_idx;
    uint8_t  vib_n;
    imu_terrain_t terrain;
    float    vib_var;
    /* speed variance (Welford) */
    double   spd_mean, spd_m2;
    uint32_t spd_n;
    /* g-trace mini graph */
    float    g_hist[64];
    uint8_t  g_hist_idx;
    uint32_t evt_drop;
} s;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

static void emit(imu_adv_evt_type_t t, float mag, uint32_t now)
{
    if (!s_q) return;
    imu_adv_evt_t e = { .type = t, .mag = mag, .now_ms = now };
    if (xQueueSend(s_q, &e, 0) != pdTRUE) s.evt_drop++;
}

static float wrap180(float a)
{
    while (a > 180.0f) a -= 360.0f;
    while (a <= -180.0f) a += 360.0f;
    return a;
}

/* ------------------------------ fast task ------------------------------- */
static void fast_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "fast task 100 Hz (software polling fallback, INT not wired)");
    TickType_t prev = xTaskGetTickCount();
    uint32_t last_ms = now_ms();
    uint8_t div10 = 0;

    while (1) {
        vTaskDelayUntil(&prev, pdMS_TO_TICKS(10));   /* 100 Hz */
        /* CLOCK_IDLE: freeze the pipeline (no integration, no lockouts).
           Exempt while gyro cal runs so boot calibration can finish. */
        if (os_context() == INPUT_CTX_STANDBY &&
            imu_driver_cal_state() != IMU_CAL_RUN) {
            last_ms = now_ms();
            continue;
        }
        uint32_t now = now_ms();
        float dt = (now - last_ms) / 1000.0f;
        if (dt <= 0.0f) dt = 0.01f;
        if (dt > 0.05f) dt = 0.05f;
        last_ms = now;

        imu_data_t imu;
        bool ok = motion_radar_imu_read(&imu);

        if (xSemaphoreTake(s_mtx, 0) != pdTRUE) continue;

        s.valid = ok;
        if (!ok) {
            /* age out transient states without sensor */
            if (s.airborne && now - s.airtime_start > 3000) {
                s.airborne = false;
                snprintf(s.landing_msg, sizeof(s.landing_msg), "LANDING: SMOOTH");
                s.landing_until = now + 1500;
                emit(IMU_EVT_LANDING_SMOOTH, 0, now);
            }
            if (s.corner_cap && (int32_t)(now - s.corner_hold_until) >= 0)
                s.corner_cap = 0;
            s.tilt_crit_now = false;
            xSemaphoreGive(s_mtx);
            continue;
        }

        float ax = imu.accel_xg, ay = imu.accel_yg, az = imu.accel_zg;
        float mag = sqrtf(ax * ax + ay * ay + az * az);
        float pitch = imu.pitch_deg, roll = imu.roll_deg;
        float yaw = imu.yaw_rate_dps;
        float tilt = fabsf(pitch) > fabsf(roll) ? fabsf(pitch) : fabsf(roll);

        /* ---- heading integration (RELATIVE only, gyro, low-passed) ---- */
        s.yaw_lp += (yaw - s.yaw_lp) * 0.3f;
        if (!(s.stationary && fabsf(yaw) < IMU_STAT_GYRO_DPS))
            s.heading_rel = wrap180(s.heading_rel + s.yaw_lp * dt);

        /* ---- stationary: |accel-1g|<0.05 AND |gyro|<2 for >2 s ---- */
        {
            float gyr = fabsf(yaw);
            /* approx other axes via accel stability: use mag only (conservative) */
            bool still = (fabsf(mag - 1.0f) < IMU_STAT_ACC_TOL_G) && (gyr < IMU_STAT_GYRO_DPS);
            if (still) {
                if (!s.stat_since) s.stat_since = now;
                if (!s.stationary && now - s.stat_since >= IMU_STATIONARY_MS) {
                    s.stationary = true;
                    emit(IMU_EVT_STATIONARY, s.heading_rel, now);
                }
            } else {
                s.stat_since = 0;
                s.stationary = false;
            }
        }

        /* ---- tilt state machine (hysteresis 2 deg on clear) ---- */
        {
            uint8_t ns = s.tilt_state;
            if (tilt >= IMU_TILT_ROLLOVER_DEG) ns = 3;
            else if (tilt >= IMU_TILT_CRITICAL_DEG) ns = (s.tilt_state == 3) ? 3 : 2;
            else if (tilt >= IMU_TILT_WARN_DEG) ns = (s.tilt_state >= 2) ? s.tilt_state : 1;
            else if (tilt < IMU_TILT_WARN_DEG - 2.0f) ns = 0;
            else if (s.tilt_state == 3 && tilt < IMU_TILT_ROLLOVER_DEG - 2.0f) ns = 2;
            else if (s.tilt_state == 2 && tilt < IMU_TILT_CRITICAL_DEG - 2.0f) ns = 1;
            if (ns != s.tilt_state) {
                if (ns == 1) {
                    s.tilt_warn_cnt++;
                    s.tilt_warn_since = now;
                    emit(IMU_EVT_TILT_WARN, tilt, now);
                } else if (ns == 2) {
                    emit(IMU_EVT_TILT_CRITICAL, tilt, now);
                } else if (ns == 3) {
                    if (!s.tilt_lockout) {
                        s.tilt_lockout = true;
                        emit(IMU_EVT_TILT_ROLLOVER, tilt, now);
                    }
                } else {
                    if (s.tilt_state == 1 && s.tilt_warn_since)
                        s.tilt_warn_ms_total += now - s.tilt_warn_since;
                    s.tilt_warn_since = 0;
                    emit(IMU_EVT_TILT_CLEAR, tilt, now);
                }
                s.tilt_state = ns;
            }
            s.tilt_crit_now = (s.tilt_state >= 2);
        }

        /* ---- impact (debounce 400 ms) ---- */
        if (now - s.impact_last_ms >= 400) {
            if (mag >= IMU_IMPACT_HARD_G) {
                s.last_impact = 3; s.last_impact_ms = now;
                s.impact_last_ms = now; s.impact_mod_cnt++;
                s.impact_hard_cnt++;   /* PART 9 session counter */
                emit(IMU_EVT_IMPACT_HARD, mag, now);
            } else if (mag >= IMU_IMPACT_MODERATE_G) {
                s.last_impact = 2; s.last_impact_ms = now;
                s.impact_last_ms = now; s.impact_mod_cnt++;
                emit(IMU_EVT_IMPACT_MODERATE, mag, now);
            } else if (mag >= IMU_IMPACT_LIGHT_G) {
                /* light impacts: only when not freefall-airborne (avoid double) */
                if (!s.airborne) {
                    s.last_impact = 1; s.last_impact_ms = now;
                    s.impact_last_ms = now;
                    emit(IMU_EVT_IMPACT_LIGHT, mag, now);
                }
            }
        }

        /* ---- freefall <0.3 g >150 ms; landing spike >2 g ---- */
        if (!s.airborne) {
            if (mag < IMU_FREEFALL_G) {
                if (!s.ff_low_since) s.ff_low_since = now;
                if (now - s.ff_low_since >= IMU_FREEFALL_MS) {
                    s.airborne = true;
                    s.airtime_start = now;
                    s.ff_low_since = 0;
                    emit(IMU_EVT_FREEFALL, mag, now);
                }
            } else {
                s.ff_low_since = 0;
            }
        } else {
            if (mag > IMU_LANDING_G) {
                bool hard = (mag >= IMU_IMPACT_MODERATE_G);
                s.last_airtime_ms = now - s.airtime_start;
                s.airborne = false;
                snprintf(s.landing_msg, sizeof(s.landing_msg),
                         hard ? "LANDING: HARD" : "LANDING: SMOOTH");
                s.landing_until = now + 1500;
                emit(hard ? IMU_EVT_LANDING_HARD : IMU_EVT_LANDING_SMOOTH, mag, now);
            } else if (now - s.airtime_start > 3000) {
                s.last_airtime_ms = now - s.airtime_start;
                s.airborne = false;   /* failsafe: never stuck airborne */
                snprintf(s.landing_msg, sizeof(s.landing_msg), "LANDING: SMOOTH");
                s.landing_until = now + 1500;
                emit(IMU_EVT_LANDING_SMOOTH, mag, now);
            }
        }

        /* ---- corner warn/hard from yaw rate (debounce 1 s, hold 500 ms) ---- */
        {
            float ayy = fabsf(yaw);
            if (ayy >= IMU_YAW_CORNER_HARD && now - s.corner_hard_last >= 1000) {
                s.corner_hard_last = now;
                s.corner_hard_cnt++;
                s.corner_cap = 2;
                s.corner_hold_until = now + 500;
                emit(IMU_EVT_CORNER_HARD, yaw, now);
            } else if (ayy >= IMU_YAW_CORNER_WARN && now - s.corner_warn_last >= 1000) {
                s.corner_warn_last = now;
                if (s.corner_cap < 1) s.corner_cap = 1;
                s.corner_hold_until = now + 500;
                emit(IMU_EVT_CORNER_WARN, yaw, now);
            }
            if (s.corner_cap && (int32_t)(now - s.corner_hold_until) >= 0
                && ayy < IMU_YAW_CORNER_WARN)
                s.corner_cap = 0;
        }

        /* ---- slip/stuck: measured <40% / <10% expected ---- */
        {
            float cmd = (fabsf((float)s.tgt_l) + fabsf((float)s.tgt_r)) * 0.5f; /* 0..100 */
            float expected = cmd / 100.0f * 0.8f;   /* ~0.8 g max longitudinal */
            float measured = fabsf(ax);             /* longitudinal proxy */
            if (cmd >= 30.0f && expected > 0.05f) {
                float ratio = measured / expected;
                if (ratio < IMU_STUCK_RATIO) {
                    if (!s.stuck_low_since) s.stuck_low_since = now;
                    if (!s.slip_low_since) s.slip_low_since = now;
                } else if (ratio < IMU_SLIP_RATIO) {
                    if (!s.slip_low_since) s.slip_low_since = now;
                    s.stuck_low_since = 0;
                } else {
                    /* recovering: accumulate durations */
                    if (s.traction == IMU_TRACTION_STUCK && s.stuck_low_since)
                        s.stuck_ms_total += now - s.stuck_low_since;
                    if (s.traction != IMU_TRACTION_GRIP && s.slip_low_since)
                        s.slip_ms_total += now - s.slip_low_since;
                    s.slip_low_since = 0; s.stuck_low_since = 0;
                    if (s.traction != IMU_TRACTION_GRIP) {
                        s.traction = IMU_TRACTION_GRIP;
                        emit(IMU_EVT_TRACTION_OK, ratio, now);
                    }
                }
                if (s.stuck_low_since && now - s.stuck_low_since >= IMU_STUCK_MS) {
                    if (s.traction != IMU_TRACTION_STUCK) {
                        s.traction = IMU_TRACTION_STUCK;
                        emit(IMU_EVT_STUCK, measured, now);
                    }
                } else if (s.slip_low_since && now - s.slip_low_since >= IMU_SLIP_MS) {
                    if (s.traction == IMU_TRACTION_GRIP) {
                        s.traction = IMU_TRACTION_SLIPPING;
                        emit(IMU_EVT_SLIP, measured, now);
                    }
                }
            } else {
                if (s.traction == IMU_TRACTION_STUCK && s.stuck_low_since)
                    s.stuck_ms_total += now - s.stuck_low_since;
                if (s.traction != IMU_TRACTION_GRIP && s.slip_low_since)
                    s.slip_ms_total += now - s.slip_low_since;
                s.slip_low_since = 0; s.stuck_low_since = 0;
                if (s.traction != IMU_TRACTION_GRIP) {
                    s.traction = IMU_TRACTION_GRIP;
                    emit(IMU_EVT_TRACTION_OK, 1.0f, now);
                }
            }
        }

        /* ---- drift: |actual - steering_implied| >30 dps >250 ms ---- */
        {
            float implied = ((float)s.tgt_r - (float)s.tgt_l) * 0.3f;
            float diff = fabsf(yaw - implied);
            float cmd = (fabsf((float)s.tgt_l) + fabsf((float)s.tgt_r)) * 0.5f;
            if (cmd >= 20.0f && diff > IMU_DRIFT_DPS) {
                if (!s.drift_high_since) s.drift_high_since = now;
                s.drift_low_since = 0;
                if (!s.drift && now - s.drift_high_since >= IMU_DRIFT_MS) {
                    s.drift = true;
                    emit(IMU_EVT_DRIFT_ON, diff, now);
                }
            } else {
                s.drift_high_since = 0;
                if (s.drift) {
                    if (!s.drift_low_since) s.drift_low_since = now;
                    if (now - s.drift_low_since >= 300) {
                        s.drift = false;
                        emit(IMU_EVT_DRIFT_OFF, diff, now);
                    }
                }
            }
        }

        /* ---- 10 Hz helpers: vibration/terrain, g-trace, speed stats ---- */
        if (++div10 >= 10) {
            div10 = 0;
            s.vib_ring[s.vib_idx] = mag;
            s.vib_idx = (s.vib_idx + 1) & 31;
            if (s.vib_n < 32) s.vib_n++;
            if (s.vib_n >= 8) {
                double m = 0;
                for (int i = 0; i < s.vib_n; i++) m += s.vib_ring[i];
                m /= s.vib_n;
                double v = 0;
                for (int i = 0; i < s.vib_n; i++) {
                    double d = s.vib_ring[i] - m;
                    v += d * d;
                }
                v /= s.vib_n;
                s.vib_var = (float)v;
                s.terrain = (v < 0.005) ? IMU_TERRAIN_SMOOTH :
                            (v < 0.020) ? IMU_TERRAIN_MODERATE : IMU_TERRAIN_ROUGH;
            }
            s.g_hist[s.g_hist_idx] = mag;
            s.g_hist_idx = (s.g_hist_idx + 1) & 63;
            /* speed variance + hard decel */
            float spd = (fabsf((float)s.cur_l) + fabsf((float)s.cur_r)) * 0.5f;
            s.spd_n++;
            double d = spd - s.spd_mean;
            s.spd_mean += d / (double)s.spd_n;
            s.spd_m2 += d * (spd - s.spd_mean);
            if (!s.speed_ref_ms) { s.speed_ref = (int16_t)spd; s.speed_ref_ms = now; }
            if (now - s.speed_ref_ms >= 200) {
                if (s.speed_ref - (int16_t)spd >= 30) s.hard_decel_cnt++;
                s.speed_ref = (int16_t)spd;
                s.speed_ref_ms = now;
            }
            s.last_speed = spd; s.last_speed_ms = now;
        }

        /* fast G estimates for UI dot */
        s.g_lat = ay;
        s.g_lon = ax;
        s.g_mag = mag;

        xSemaphoreGive(s_mtx);
    }
}

/* ------------------------------- public --------------------------------- */
void imu_adv_init(void)
{
    if (s_ready) return;
    memset(&s, 0, sizeof(s));
    s.heading_rel = 0;
    s.traction = IMU_TRACTION_GRIP;
    s.terrain = IMU_TERRAIN_SMOOTH;
    s_mtx = xSemaphoreCreateMutex();
    s_q = xQueueCreate(24, sizeof(imu_adv_evt_t));
    if (!s_mtx || !s_q) {
        ESP_LOGE(TAG, "mutex/queue create failed");
        return;
    }
    s_ready = true;
    BaseType_t r = xTaskCreatePinnedToCore(fast_task, "imu_fast", 4096, NULL,
                                           3, &s_task, 1);
    if (r != pdTRUE) {
        ESP_LOGE(TAG, "fast task create failed");
        s_ready = false;
        return;
    }
    ESP_LOGI(TAG, "init done (fast 100 Hz + slow 25 Hz, I2C 400 kHz shared)");
}

bool imu_adv_ready(void) { return s_ready; }

/* PART 16 mem_diag: fast-task stack high-water (0 if not running) */
uint32_t imu_adv_stack_free(void)
{
    if (!s_task) return 0;
    return (uint32_t)uxTaskGetStackHighWaterMark(s_task);
}

void imu_adv_slow_update(float pitch, float roll, float yaw_rate,
                         float ax_g, float ay_g, float az_g, uint32_t now_ms)
{
    (void)now_ms;
    if (!s_ready || !s_mtx) return;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5)) != pdTRUE) return;
    const float a = 0.3f;   /* UI smoothing only, control values untouched */
    s.pitch_sm += (pitch - s.pitch_sm) * a;
    s.roll_sm += (roll - s.roll_sm) * a;
    s.yaw_sm += (yaw_rate - s.yaw_sm) * a;
    if (s.vib_n == 0) { s.g_lat = ay_g; s.g_lon = ax_g; }
    (void)az_g;
    xSemaphoreGive(s_mtx);
}

void imu_adv_set_motion(int16_t tgt_l, int16_t tgt_r,
                        int16_t cur_l, int16_t cur_r, uint32_t now_ms)
{
    if (!s_ready || !s_mtx) return;
    if (xSemaphoreTake(s_mtx, 0) != pdTRUE) return;
    s.tgt_l = tgt_l; s.tgt_r = tgt_r;
    s.cur_l = cur_l; s.cur_r = cur_r;
    s.motion_ms = now_ms;
    xSemaphoreGive(s_mtx);
}

float imu_adv_heading_rel(void)
{
    float h = 0;
    if (s_ready && s_mtx && xSemaphoreTake(s_mtx, 0) == pdTRUE) {
        h = s.heading_rel;
        xSemaphoreGive(s_mtx);
    }
    return h;
}

void imu_adv_reset_heading(void)
{
    if (!s_ready || !s_mtx) return;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(10)) != pdTRUE) return;
    s.heading_rel = 0;
    xSemaphoreGive(s_mtx);
    ESP_LOGI(TAG, "heading reset (manual RS-hold-1s)");
}

bool imu_adv_evt_recv(imu_adv_evt_t *out)
{
    if (!s_ready || !s_q || !out) return false;
    return xQueueReceive(s_q, out, 0) == pdTRUE;
}

static uint8_t shield_score_locked(void)
{
    float tilt = fabsf(s.pitch_sm) > fabsf(s.roll_sm) ? fabsf(s.pitch_sm) : fabsf(s.roll_sm);
    float tilt_pts = 40.0f - (tilt / IMU_TILT_ROLLOVER_DEG) * 40.0f;
    if (tilt_pts < 0) tilt_pts = 0;
    float yaw_pts = 20.0f - fminf(fabsf(s.yaw_sm) / IMU_YAW_CORNER_HARD, 1.0f) * 20.0f;
    float slip_pts = (s.traction == IMU_TRACTION_GRIP) ? 20.0f :
                     (s.traction == IMU_TRACTION_SLIPPING) ? 8.0f : 0.0f;
    uint32_t now = now_ms();
    float imp_pts = 20.0f;
    if (s.last_impact_ms && now - s.last_impact_ms < 10000)
        imp_pts = (s.last_impact >= 3) ? 0.0f : 10.0f;
    float sc = tilt_pts + yaw_pts + slip_pts + imp_pts;
    if (sc < 0) sc = 0;
    if (sc > 100) sc = 100;
    return (uint8_t)sc;
}

/* weighted score + optional 6 components. Caller MUST hold s_mtx. */
static uint16_t score_locked(int comp[6])
{
    int corner = 100 - (int)s.corner_hard_cnt * 15;
    int brake = 100 - (int)s.hard_decel_cnt * 15;
    int bumps = 100 - (int)s.impact_mod_cnt * 20;
    double var = (s.spd_n > 1) ? (s.spd_m2 / (double)(s.spd_n - 1)) : 0;
    int stable = 100 - (int)fmin(50.0, sqrt(var) * 2.0);
    int traction = 100 - (int)(s.slip_ms_total / 1000 * 4 + s.stuck_ms_total / 1000 * 10);
    int tilt = 100 - (int)(s.tilt_warn_cnt * 5 + s.tilt_warn_ms_total / 1000 * 2);
    if (corner < 0) corner = 0;
    if (brake < 0) brake = 0;
    if (bumps < 0) bumps = 0;
    if (stable < 0) stable = 0;
    if (traction < 0) traction = 0;
    if (tilt < 0) tilt = 0;
    if (comp) {
        comp[0] = corner; comp[1] = brake; comp[2] = bumps;
        comp[3] = stable; comp[4] = traction; comp[5] = tilt;
    }
    return (uint16_t)((corner + brake + bumps + stable + traction + tilt) / 6);
}

uint16_t imu_adv_drive_score(void)
{
    if (!s_ready || !s_mtx) return 100;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5)) != pdTRUE) return 100;
    uint16_t sc = score_locked(NULL);
    xSemaphoreGive(s_mtx);
    return sc;
}

char imu_adv_drive_grade(void)
{
    uint16_t sc = imu_adv_drive_score();
    if (sc >= 90) return 'A';
    if (sc >= 75) return 'B';
    if (sc >= 60) return 'C';
    if (sc >= 40) return 'D';
    return 'F';
}

/* PART 10 trip window: session hard-bump count */
uint32_t trip_hard_bumps(void)
{
    uint32_t c = 0;
    if (s_ready && s_mtx && xSemaphoreTake(s_mtx, 0) == pdTRUE) {
        c = s.impact_hard_cnt;
        xSemaphoreGive(s_mtx);
    }
    return c;
}

void imu_adv_snapshot(imu_adv_snap_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_ready || !s_mtx) return;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5)) != pdTRUE) return;
    uint32_t now = now_ms();
    out->valid = s.valid;
    out->pitch_deg = s.pitch_sm;
    out->roll_deg = s.roll_sm;
    out->heading_rel = s.heading_rel;
    out->yaw_rate_dps = s.yaw_sm;
    out->g_lat = s.g_lat;
    out->g_lon = s.g_lon;
    out->g_mag = s.g_mag;
    out->traction = s.traction;
    out->airborne = s.airborne;
    out->airtime_ms = s.airborne ? (now - s.airtime_start) : 0;
    if (!s.airborne && s.landing_until > now) {
        snprintf(out->landing_msg, sizeof(out->landing_msg), "%s", s.landing_msg);
        out->landing_until = s.landing_until;
    }
    out->drift = s.drift;
    out->shield_score = shield_score_locked();
    out->shield = (out->shield_score >= 70) ? IMU_SHIELD_GREEN :
                  (out->shield_score >= 40) ? IMU_SHIELD_AMBER : IMU_SHIELD_RED;
    out->terrain = s.terrain;
    out->vib_var = s.vib_var;
    out->tilt_lockout = s.tilt_lockout;
    out->tilt_critical = s.tilt_crit_now;
    out->corner_cap = s.corner_cap;
    out->last_impact = s.last_impact;
    out->last_impact_ms = s.last_impact_ms;
    out->impact_hard_cnt = s.impact_hard_cnt;
    out->last_airtime_ms = s.last_airtime_ms;
    out->tilt_deg = fabsf(s.pitch_sm) > fabsf(s.roll_sm) ?
                    fabsf(s.pitch_sm) : fabsf(s.roll_sm);
    {
        int comp[6];
        out->drive_score = score_locked(comp);
        out->sc_corner = (uint8_t)comp[0]; out->sc_brake = (uint8_t)comp[1];
        out->sc_bumps = (uint8_t)comp[2]; out->sc_stable = (uint8_t)comp[3];
        out->sc_traction = (uint8_t)comp[4]; out->sc_tilt = (uint8_t)comp[5];
    }
    xSemaphoreGive(s_mtx);
    /* grade from the just-computed score (no second lock round-trip) */
    out->drive_grade = (out->drive_score >= 90) ? 'A' :
                       (out->drive_score >= 75) ? 'B' :
                       (out->drive_score >= 60) ? 'C' :
                       (out->drive_score >= 40) ? 'D' : 'F';
}

bool imu_adv_motors_cut(void)
{
    bool cut = false;
    if (s_ready && s_mtx && xSemaphoreTake(s_mtx, 0) == pdTRUE) {
        cut = s.tilt_crit_now || s.tilt_lockout || s.airborne;
        xSemaphoreGive(s_mtx);
    }
    return cut;
}

bool imu_adv_rollover_latched(void)
{
    bool l = false;
    if (s_ready && s_mtx && xSemaphoreTake(s_mtx, 0) == pdTRUE) {
        l = s.tilt_lockout;
        xSemaphoreGive(s_mtx);
    }
    return l;
}

void imu_adv_ack_rollover(void)
{
    if (!s_ready || !s_mtx) return;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(10)) != pdTRUE) return;
    /* Manual acknowledge only. If still past rollover angle it will re-latch
       on the next fast tick (sticky until truly level). */
    s.tilt_lockout = false;
    if (s.tilt_state == 3) s.tilt_state = 2;
    xSemaphoreGive(s_mtx);
    ESP_LOGW(TAG, "rollover acknowledged manually (Alexa cannot clear)");
}

uint8_t imu_adv_corner_cap(void)
{
    uint8_t c = 0;
    if (s_ready && s_mtx && xSemaphoreTake(s_mtx, 0) == pdTRUE) {
        c = s.corner_cap;
        xSemaphoreGive(s_mtx);
    }
    return c;
}

bool imu_adv_airborne(void)
{
    bool a = false;
    if (s_ready && s_mtx && xSemaphoreTake(s_mtx, 0) == pdTRUE) {
        a = s.airborne;
        xSemaphoreGive(s_mtx);
    }
    return a;
}
