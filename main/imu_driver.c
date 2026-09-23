/*
 * imu_driver.c - MPU-6500 accelerometer + gyroscope driver (I2C).
 *
 * Shares the I2C bus with the SSD1306 OLED (SDA = GPIO42, SCL = GPIO41).
 * Registers: accel (0x3B..0x42), gyro (0x43..0x48), temp (0x41..0x42),
 * WHO_AM_I (0x75). Pitch/roll derived from the 3-D accel gravity vector,
 * yaw rate read directly from gyro Z. Failures are non-fatal: if the
 * WHO_AM_I check / reads time out, valid goes false and the UI shows
 * grey "no IMU" tiles. No I2C bus reset is attempted on this shared line
 * (would disturb the OLED), so a hard failure simply degrades the IMU.
 */
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "imu_driver.h"

static const char *TAG = "imu";

/* MPU-6500 registers */
#define MPU_REG_WHO_AM_I     0x75
#define MPU_REG_PWR_MGMT_1   0x6B
#define MPU_REG_ACCEL_XOUT_H 0x3B
#define MPU_REG_TEMP_OUT_H   0x41
#define MPU_REG_GYRO_XOUT_H  0x43
#define MPU_REG_CONFIG       0x1A
#define MPU_REG_GYRO_CFG     0x1B
#define MPU_REG_ACCEL_CFG    0x1C

#define MPU6500_WHOAI_EXPECT 0x70   /* MPU-6500; 0x68=6050, 0x71=9250 also OK */

#define ACCEL_FS_G  8.0f            /* ±8 g full scale (impact measurable) */
#define ACCEL_SCALE (ACCEL_FS_G / 32768.0f)
#define GYRO_FS_DPS 500.0f          /* ±500 dps (fast spins measurable)   */
#define GYRO_SCALE  (GYRO_FS_DPS / 32768.0f)
#define TEMP_SCALE  (1.0f / 340.0f)
#define TEMP_OFFSET 36.53f

#define IMU_VALID_TIMEOUT_MS 600

static i2c_master_dev_handle_t s_imu_dev;
static i2c_master_bus_handle_t s_bus;
static bool    s_attached = false;
static bool    s_stale_valid = false;
static uint8_t s_addr = 0;

static imu_data_t s_imu;

/* ---- gyro bias / calibration state machine ---- */
#define CAL_SAMPLES          50      /* ~1 s at the 20 ms main-loop tick */
#define CAL_MOTION_ABORT_DPS 60.0f   /* car must be still while sampling */
static uint8_t  s_cal_state = 0;     /* IMU_CAL_*                        */
static uint32_t s_cal_t0 = 0;
static int      s_cal_n = 0;
static float    s_cal_sx, s_cal_sy, s_cal_sz;
static float    s_bias_x, s_bias_y, s_bias_z;

static esp_err_t imu_write(uint8_t reg, const uint8_t *data, size_t len)
{
    if (!s_attached || !s_imu_dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[16];
    if (len + 1 > sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    buf[0] = reg;
    for (size_t i = 0; i < len; i++) buf[1 + i] = data[i];
    return i2c_master_transmit(s_imu_dev, buf, len + 1, 20);
}

static esp_err_t imu_read(uint8_t reg, uint8_t *out, size_t len)
{
    if (!s_attached || !s_imu_dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_imu_dev, &reg, 1, out, len, 20);
}

static uint16_t rd16(const uint8_t *b)
{
    return (uint16_t)((((uint16_t)b[0]) << 8) | b[1]);
}

static int16_t rd_i16(const uint8_t *b) { return (int16_t)rd16(b); }

static void imu_mark_invalid(const char *why, esp_err_t er)
{
    s_imu.err_count++;
    ESP_LOGW(TAG, "%s: %s (err=%u)", why, esp_err_to_name(er),
             (unsigned)s_imu.err_count);
    if (s_imu.err_count > 20000) s_imu.err_count = 0;
}
/* Attach to one MPU address: probe WHO_AM_I, then wake + configure.
   do_reset=true only at boot (100 ms delay); runtime failover skips reset
   so the car loop never hitches. Old handle is removed only after the new
   one answers, so we never end up handle-less. */
static bool imu_attach_addr(uint8_t addr, bool do_reset)
{
    if (!s_bus) return false;
    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        /* MPU at 100 kHz (OLED keeps 400 kHz on the same bus). */
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(s_bus, &dcfg, &dev) != ESP_OK || !dev)
        return false;

    uint8_t who = 0;
    uint8_t reg = MPU_REG_WHO_AM_I;
    esp_err_t er = i2c_master_transmit_receive(dev, &reg, 1, &who, 1, 20);
    /* register-map compatible family: 6050/6500/9250/9255 */
    bool known = (er == ESP_OK) &&
                 ((who == 0x68) || (who == 0x70) ||
                  (who == 0x71) || (who == 0x73));
    if (!known) {
        i2c_master_bus_rm_device(dev);
        return false;
    }

    i2c_master_dev_handle_t old = s_attached ? s_imu_dev : NULL;
    s_imu_dev = dev;
    s_attached = true;
    if (do_reset) {
        uint8_t r = 0x80;
        imu_write(MPU_REG_PWR_MGMT_1, &r, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    {
        uint8_t r = 0x01;   /* auto-select best clock */
        imu_write(MPU_REG_PWR_MGMT_1, &r, 1);
        r = 0x03;           /* DLPF ~44Hz */
        imu_write(MPU_REG_CONFIG, &r, 1);
        r = 0x08;           /* gyro +-500dps */
        imu_write(MPU_REG_GYRO_CFG, &r, 1);
        r = 0x10;           /* accel +-8g */
        imu_write(MPU_REG_ACCEL_CFG, &r, 1);
    }
    s_addr = addr;
    s_imu.i2c_addr = addr;
    s_imu.who_am_i = who;
    if (old) i2c_master_bus_rm_device(old);
    return true;
}

void imu_driver_init(i2c_master_bus_handle_t bus)
{
    s_bus = bus;
    s_attached = false;
    s_stale_valid = false;
    s_addr = 0;
    memset(&s_imu, 0, sizeof(s_imu));
    s_imu.who_am_i = 0;
    s_imu.cal_state = 0;

    if (!s_bus) {
        ESP_LOGW(TAG, "no bus handle - IMU disabled");
        return;
    }

    /* Try MPU6500 default address then the AD0-high variant.
       Retry a few times: the part can need >100ms after power-up before it
       ACKs (marginal rail), and instant-boot probes earlier than before. */
    static const uint8_t addrs[2] = { 0x68, 0x69 };
    for (int attempt = 0; attempt < 4 && !s_attached; attempt++) {
        for (int i = 0; i < 2; i++) {
            if (imu_attach_addr(addrs[i], true)) {
                ESP_LOGI(TAG, "MPU found at 0x%02X WHO_AM_I=0x%02X",
                         s_addr, (unsigned)s_imu.who_am_i);
                break;
            }
        }
        if (!s_attached) vTaskDelay(pdMS_TO_TICKS(150));
    }

    if (!s_attached) {
        ESP_LOGW(TAG, "MPU-6500 not detected (SCL=41 SDA=42) - IMU grey");
        return;
    }

    s_imu.cal_state = 2;
    ESP_LOGI(TAG, "IMU ready");
    /* auto-bias at boot: car is assumed at rest right after power-up */
    imu_driver_cal_start();
}

void imu_driver_cal_start(void)
{
    if (!s_attached) return;
    s_cal_state = IMU_CAL_RUN;
    s_cal_n = 0;
    s_cal_sx = s_cal_sy = s_cal_sz = 0.0f;
    s_cal_t0 = (uint32_t)esp_timer_get_time() / 1000u;
    ESP_LOGI(TAG, "gyro cal started - keep the car still");
}

/* Runtime re-attach (light: single pass, no delays). Called every ~10s
   while the IMU never produces data - recovers boot-time probe misses
   and mid-run dropouts without stalling the control loop. */
void imu_driver_retry(void)
{
    if (!s_bus || s_attached) return;
    uint8_t cur = s_addr ? s_addr : 0x68;
    if (imu_attach_addr(cur, false) ||
        imu_attach_addr(cur == 0x68 ? 0x69 : 0x68, false)) {
        ESP_LOGI(TAG, "MPU re-attached at 0x%02X WHO_AM_I=0x%02X",
                 s_addr, (unsigned)s_imu.who_am_i);
        s_imu.cal_state = 2;
        imu_driver_cal_start();
        return;
    }
}

uint8_t imu_driver_cal_state(void) { return s_cal_state; }

uint8_t imu_driver_cal_pct(void)
{
    if (s_cal_state == IMU_CAL_RUN)  return (uint8_t)(s_cal_n * 100 / CAL_SAMPLES);
    if (s_cal_state == IMU_CAL_DONE) return 100;
    return 0;
}

float imu_driver_gyro_bias_z(void) { return s_bias_z; }

void imu_driver_poll(uint32_t dt_ms)
{
    (void)dt_ms;
    if (!s_attached) return;

    uint8_t d[14];   /* accel(6)+temp(2)+gyro(6) from 0x3B */
    esp_err_t er = imu_read(MPU_REG_ACCEL_XOUT_H, d, sizeof(d));
    if (er != ESP_OK) {
        imu_mark_invalid("IMU read fail", er);
        s_stale_valid = false;
        /* recovery: every ~4 s of continuous failure, try the OTHER address
           (floating AD0 flips 0x68<->0x69 mid-run on some modules) with a
           fresh wake+config; old handle is kept unless the new one answers. */
        if (s_imu.err_count % 25 == 0) {
            uint8_t other = (s_addr == 0x68) ? 0x69 : 0x68;
            if (imu_attach_addr(other, false)) {
                ESP_LOGW(TAG, "MPU failover to 0x%02X (WHO=0x%02X)",
                         (unsigned)s_addr, (unsigned)s_imu.who_am_i);
                s_imu.err_count = 0;
            } else {
                ESP_LOGW(TAG, "MPU probe fail: %s", esp_err_to_name(er));
            }
        }
        return;
    }

    int16_t ax = rd_i16(&d[0]);
    int16_t ay = rd_i16(&d[2]);
    int16_t az = rd_i16(&d[4]);
    int16_t ot = rd_i16(&d[6]);
    int16_t gx = rd_i16(&d[8]);
    int16_t gy = rd_i16(&d[10]);
    int16_t gz = rd_i16(&d[12]);

    s_imu.accel_xg = ax * ACCEL_SCALE;
    s_imu.accel_yg = ay * ACCEL_SCALE;
    s_imu.accel_zg = az * ACCEL_SCALE;
    s_imu.gyro_xdps = gx * GYRO_SCALE;
    s_imu.gyro_ydps = gy * GYRO_SCALE;
    s_imu.gyro_zdps = gz * GYRO_SCALE;   /* + = CCW viewed down */

    /* ---- gyro bias calibration collector (advance once per poll) ---- */
    if (s_cal_state == IMU_CAL_RUN) {
        if (fabsf(s_imu.gyro_xdps) > CAL_MOTION_ABORT_DPS ||
            fabsf(s_imu.gyro_ydps) > CAL_MOTION_ABORT_DPS ||
            fabsf(s_imu.gyro_zdps) > CAL_MOTION_ABORT_DPS) {
            s_cal_state = IMU_CAL_FAIL;
            ESP_LOGW(TAG, "gyro cal aborted (car moved)");
        } else {
            s_cal_sx += s_imu.gyro_xdps;
            s_cal_sy += s_imu.gyro_ydps;
            s_cal_sz += s_imu.gyro_zdps;
            if (++s_cal_n >= CAL_SAMPLES) {
                s_bias_x = s_cal_sx / CAL_SAMPLES;
                s_bias_y = s_cal_sy / CAL_SAMPLES;
                s_bias_z = s_cal_sz / CAL_SAMPLES;
                s_cal_state = IMU_CAL_DONE;
                ESP_LOGI(TAG, "gyro cal done (bias z %.2f dps)",
                         (double)s_bias_z);
            }
        }
    }

    s_imu.temp_c = (int16_t)(((float)ot * TEMP_SCALE + TEMP_OFFSET));

    /* Tilt from gravity vector (landscape: X across, Y toward front). */
    float axx = s_imu.accel_xg;
    float ayy = s_imu.accel_yg;
    float azz = s_imu.accel_zg;
    float mag = sqrtf(axx * axx + ayy * ayy + azz * azz);
    if (mag < 0.1f) mag = 0.1f;
    s_imu.pitch_deg = IMU_PITCH_SIGN * asinf(axx / mag) * 57.2958f;
    s_imu.roll_deg  = IMU_ROLL_SIGN  * atan2f(ayy, azz) * 57.2958f;
    /* yaw rate: bias-corrected, IMU_YAW_SIGN makes the UI turn arrow
       match the real car */
    s_imu.yaw_rate_dps = IMU_YAW_SIGN * (s_imu.gyro_zdps - s_bias_z);

    s_imu.last_update_ms = (uint32_t)esp_timer_get_time() / 1000u;
    s_stale_valid = true;
}

bool motion_radar_imu_read(imu_data_t *out)
{
    if (!out) return false;
    uint32_t now_ms = (uint32_t)esp_timer_get_time() / 1000u;
    bool fresh = s_attached && s_stale_valid &&
                 (now_ms - s_imu.last_update_ms) < IMU_VALID_TIMEOUT_MS;
    if (fresh) {
        *out = s_imu;
        out->valid = true;
        return true;
    }
    memset(out, 0, sizeof(*out));
    out->valid = false;
    return false;
}

bool motion_radar_imu_is_valid(void)
{
    uint32_t now_ms = (uint32_t)esp_timer_get_time() / 1000u;
    return s_attached && s_stale_valid &&
           (now_ms - s_imu.last_update_ms) < IMU_VALID_TIMEOUT_MS;
}