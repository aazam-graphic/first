/*
 * imu_driver.h - MPU-6500 accelerometer + gyroscope driver (I2C, shared bus).
 *
 * The MPU-6500 shares the I2C bus with the OLED (SDA = GPIO42, SCL = GPIO41).
 * car.c creates the master bus and passes the handle to imu_driver_init(),
 * then calls imu_driver_poll(dt_ms) from the main loop.
 *
 * The Motion Radar TFT interface reads IMU state through the read-only
 * motion_radar_imu_read() / motion_radar_imu_is_valid() API so the UI never
 * touches the I2C bus directly.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Shared IMU state consumed by the Motion Radar UI.                   */
/* ------------------------------------------------------------------ */
typedef struct {
    bool    valid;             /* false => IMU absent/fault -> grey UI    */
    float   pitch_deg;         /* from accel gravity vector, -90..90      */
    float   roll_deg;          /* from accel gravity vector, -180..180    */
    float   yaw_rate_dps;      /* gyro Z, deg/sec (+ = turning right)     */
    float   accel_xg, accel_yg, accel_zg;   /* -16..16 g (scaled)         */
    float   gyro_xdps, gyro_ydps, gyro_zdps;/* deg/sec                    */
    int16_t temp_c;            /* die temperature (deg C, coarse)         */
    uint8_t who_am_i;          /* expected 0x70 (MPU6500) / 0x71          */
    uint8_t i2c_addr;          /* 0x68 or 0x69 detected                   */
    uint8_t cal_state;         /* 0 none, 1 calibrating, 2 done           */
    uint32_t last_update_ms;   /* monotonic ms of last successful read    */
    uint16_t err_count;        /* consecutive/accumulated I2C errors       */
} imu_data_t;

/* ---- ORIENTATION TRIM (flip a sign if an arrow/bubble points the wrong
   way on the real car - no UI code needs to change) ----
   IMU_YAW_SIGN   +1: gyro +Z (CCW from above) renders as a LEFT turn arrow.
   IMU_PITCH_SIGN +1: nose-up gives an upward bubble move.
   IMU_ROLL_SIGN  +1: right-side-down gives a rightward bubble move.      */
#define IMU_YAW_SIGN    (+1.0f)
#define IMU_PITCH_SIGN  (+1.0f)
#define IMU_ROLL_SIGN   (+1.0f)

/* Attach the driver to an existing I2C master bus. Call once after the
   bus is created in car.c. Falls back 0x68 -> 0x69 automatically. */
void imu_driver_init(i2c_master_bus_handle_t bus);

/* Lightweight runtime re-attach (no delays): recovers boot probe misses. */
void imu_driver_retry(void);

/* Periodic poll. Non-blocking, safe to call every main-loop tick.
   dt_ms is only used to integrate nothing sensor-related yet (kept for
   future fusion). Updates internal state + data age. */
void imu_driver_poll(uint32_t dt_ms);

/* Copy current IMU state into *out. Returns true if *out is populated.
   Returns false (and fills with zeros) if the IMU has never been valid. */
bool motion_radar_imu_read(imu_data_t *out);

/* 1 when the IMU produced a successful reading recently (short timeout). */
bool motion_radar_imu_is_valid(void);

/* ---- gyro bias calibration (A = CAL on the radar) ----
   Collects ~1 s of gyro samples at rest, stores the bias, applies it to
   yaw_rate_dps. Aborts with state 3 if the car moves during collection.
   Also runs automatically once at boot (car is at rest). */
#define IMU_CAL_IDLE   0
#define IMU_CAL_RUN    1
#define IMU_CAL_DONE   2
#define IMU_CAL_FAIL   3
void    imu_driver_cal_start(void);   /* no-op when IMU absent            */
uint8_t imu_driver_cal_state(void);   /* IMU_CAL_*                        */
uint8_t imu_driver_cal_pct(void);     /* 0..100 while running             */
float   imu_driver_gyro_bias_z(void); /* applied bias (dps)               */

#ifdef __cplusplus
}
#endif