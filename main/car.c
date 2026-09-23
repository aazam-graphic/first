/*
 * car.c - Redgear Pro gamepad controlled robot car (ESP32-S3)
 *
 * Features:
 *  - Differential drive: LY throttle, LX steering, RT throttle,
 *    LT brake (200+ = real motor brake), LS click = crawl mode
 *  - Motors via UART D1 slave (GPIO1 TX @115200, "L<l>,R<r>\n")
 *  - Servos: 1/2 = spark cannon pan/tilt (right stick);
 *    gripper/spoiler servos REMOVED (hardware removed)
 *  - Lights: headlight relay (46), backlight relay (14), rear light relay (3),
 *    roof WS2812 police light (18, roof_light.c), WS2812 rear+under strip (6),
 *    front WS2812 LEDs (17/15) — modes: off/static/rainbow/chase/siren,
 *    indicators, hazard
 *  - 3x HC-SR04 ultrasonic (left/front/right): parking beeps, obstacle
 *    speed limit, emergency stop, auto mode avoidance + stuck escape
 *  - Auto mode (X hold): speed decided by distance, stuck escape with torque
 *    boost, everything automatic (lights/indicators/mist/spark aim)
 *  - Mist maker (A hold + auto), roof light (B), horn (LB),
 *    headlight (A tap), ambient LED colors (Y), hazard (D-up),
 *    turbo (RB, 3s max + 5s cooldown)
 *  - MPU-6500 IMU (Motion Radar UI) + 0.96" SSD1306 OLED HUD
 *  - MAX98357A I2S sound: engine sound (speed linked) + effect tones
 *  - Battery monitor REMOVED (GPIO1 ab motor UART hai)
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "nvs_flash.h"
#include "nvs.h"
// battery ADC removed - no longer needed
#include "led_strip.h"
#include "xbox360.h"
#include "car.h"
#include "car_global.h"
#include "car_os.h"
#include "input_events.h"
#include "roof_light.h"
#include "tft_display.h"
#include "tft_boot_anim.h"
#include "os_gfx.h"   /* early boot splash via framebuffer */
#include "os_screens_tft.h"   /* PART 16: L5 safety overlay over games */
#include "oled_driver.h"
#include "imu_driver.h"
#include "imu_adv.h"
#include "alexa_bridge.h"
#include "snd_bank.h"
#include "mic_in.h"
#include "img_bank.h"   /* PART 6: late-init image preload */
#include "notif.h"      /* PART 10.3 Notification Center feed */
#include "mem_diag.h"   /* PART 15/16 memory diagnostics */

static const char *TAG = "car";

/* ---------------------------------- pins --------------------------------- */

#define PIN_SERVO_PAN      4   /* LEDC ch0 */
#define PIN_SERVO_TILT     5   /* LEDC ch1 */
/* gripper + spoiler servos REMOVED (hardware removed, pins freed):
   GPIO 6 = rear WS2812 strip, GPIO 7 = front US trig.
   servo_deg() ignores ch2/ch3 so old calls become safe no-ops. */
/* PIN_M_IN1..ENB removed — motors now via UART D1 slave (pin 36)
   Pins 15,17 reused for front WS2812 LEDs */
#define PIN_HEADLIGHT      46  /* was 48 - relay active LOW - headlight (46 free after TFT SCLK -> 21) */
#define PIN_BACKLIGHT_UNUSED 14  /* PART 8: body-backlight relay REMOVED in hardware;
                                    GPIO14 repurposed as MIC WS (I2S1). Do not drive. */
/* NOTE: PIN_BATT_ADC was removed long ago (GPIO1 = motor UART TX now). */
#define PIN_US_TRIG_L      12  /* LEFT trig (moved 8→12) */
#define PIN_US_TRIG_R      8   /* RIGHT trig (keep on 8) */
#define PIN_US_TRIG_F      7   /* FRONT trig (was 38 - 38 now TFT MOSI) */
// REAR US removed - pins 44/40 + 14 freed (user request)
#define PIN_US_ECHO_L      9
#define PIN_US_ECHO_F      10
#define PIN_US_ECHO_R      11
#define PIN_LED_STRIP      6   /* rear indicator (was 21 - 21 now TFT SCLK) */
/* Under-car strip: DAISY-CHAINED after rear strip on GPIO 6 (rear DOUT -> under DIN).
   No dedicated GPIO — board pe free pin nahi bacha (22-25 module pe exist nahi,
   26-37 flash/PSRAM, 43/44 console). UNDER_STRIP_NUM = 12 under LEDs. */
#define PIN_I2S_BCLK       39  /* revert original */
#define PIN_I2S_LRC        47
#define PIN_I2S_DIN        40  /* moved 35->40 (35 = octal PSRAM pin, N16R8) */
#define PIN_REAR_LIGHT     3   /* rear light under spoiler air brake - active LOW relay (was roof relay, user request) */
#define PIN_MIST           16  /* was 2 - 2 now TFT RST */
// #define PIN_BATT_ADC removed - GPIO1 is motor UART TX (battery function removed)
#define PIN_UART_TX        1   /* D1 motor slave -> D1 RX @115200 (was GPIO 14; 14 = backlight relay) */
#define PIN_OLED_SDA       42
// #define PIN_BUZZER removed - amp (MAX98357 I2S) does horn via wav (GPIO 18 = roof WS2812)
#define PIN_FRONT_LED_L    17  /* Front Left WS2812 RGB LED */
#define PIN_FRONT_LED_R    15  /* Front Right WS2812 RGB LED */
#define PIN_OLED_SCL       41

#define OLED_ADDR          0x3C
// MPU_ADDR removed - gyro disabled
#define OLED_W             128
#define OLED_H             64

#define LED_STRIP_NUM      2   /* rear strip: 2 addressable pixels, HW strip triples each pixel to 3 physical LEDs = 6 total (3 left + 3 right) */
#define UNDER_STRIP_NUM    12  /* under-car strip: 12 addressable LEDs (4 groups x 3) */
// BATT_DIV removed - battery function removed

#define LOOP_MS            40
#define STICK_MAX          16000 /* typical stick ~±16000, not 32767 */

/* buttons B_* and modes MODE_* / AST_* now live in car_global.h (shared with Car OS) */

/* ----------------------------- global state ----------------------------- */

/* definition (declared in car_global.h) */
car_state_t g;
static os_ctx_t s_os;   /* Car OS context (single instance) - defined here so
                           input_handle (drive controls) can set overlays */

/* gear 1..5 speed caps % (Car OS settings screen adjusts + persists in NVS).
   RB (turbo) button bypasses gear cap and gives 100%. */
static uint8_t s_gear_caps[CAR_GEAR_COUNT] = {5, 15, 25, 35, 50};

/* ------------------------------- helpers -------------------------------- */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int16_t stick_dz(int16_t v)
{
    if (v > -1500 && v < 1500) {
        return 0;
    }
    return v;
}

static uint16_t min3(uint16_t a, uint16_t b, uint16_t c)
{
    uint16_t m = a < b ? a : b;
    return m < c ? m : c;
}

/* ---------------------------------------------------------------------------
 * PIN UNIQUENESS GUARD (added 2026-09-17 review)
 * Boot pe ek baar check karta hai ki koi GPIO do functions me nahi diya gaya.
 * Naya pin add karte waqt ye list update karo — duplicate hone par boot log
 * me clear ESP_LOGE milega (silent pin conflict se bachne ke liye).
 * NOTE: ESP32-S3 strapping pins jo is project me use ho rahe hain:
 *   GPIO0  (TFT DC)          - boot mode strap
 *   GPIO3  (rear light relay)- JTAG source strap
 *   GPIO45 (TFT CS)          - VDD_SPI voltage strap
 *   GPIO46 (headlight relay) - boot mode strap
 * Inpe aise external pull/pulldown mat lagao jo reset ke waqt ulta level de,
 * warna board download-mode/VDD_SPI 1.8V me fas sakta hai.
 * ------------------------------------------------------------------------- */
static void pin_conflict_check(void)
{
    static const struct { const char *name; int pin; } map[] = {
        { "SERVO_PAN",  PIN_SERVO_PAN },  { "SERVO_TILT", PIN_SERVO_TILT },
        { "US_TRIG_L",  PIN_US_TRIG_L },  { "US_ECHO_L",  PIN_US_ECHO_L },
        { "US_TRIG_F",  PIN_US_TRIG_F },  { "US_ECHO_F",  PIN_US_ECHO_F },
        { "US_TRIG_R",  PIN_US_TRIG_R },  { "US_ECHO_R",  PIN_US_ECHO_R },
        { "LED_STRIP",  PIN_LED_STRIP },  { "FRONT_LED_L", PIN_FRONT_LED_L },
        { "FRONT_LED_R", PIN_FRONT_LED_R },
        { "I2S_BCLK",   PIN_I2S_BCLK },   { "I2S_LRC",    PIN_I2S_LRC },
        { "I2S_DIN",    PIN_I2S_DIN },
        { "HEADLIGHT",  PIN_HEADLIGHT },
        { "MIC_SCK",    MIC_PIN_SCK },    { "MIC_WS",     MIC_PIN_WS },
        { "MIC_SD",     MIC_PIN_SD },
        { "REAR_LIGHT", PIN_REAR_LIGHT }, { "MIST",       PIN_MIST },
        { "UART_TX",    PIN_UART_TX },    { "OLED_SDA",   PIN_OLED_SDA },
        { "OLED_SCL",   PIN_OLED_SCL },
        { "TFT_SCLK",   TFT_PIN_SCLK },   { "TFT_MOSI",   TFT_PIN_MOSI },
        { "TFT_CS",     TFT_PIN_CS },     { "TFT_DC",     TFT_PIN_DC },
        { "TFT_RST",    TFT_PIN_RST },    { "ROOF_LED",   ROOF_STRIP_GPIO },
    };
    const int n = (int)(sizeof(map) / sizeof(map[0]));
    int conflicts = 0;
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (map[i].pin == map[j].pin) {
                conflicts++;
                ESP_LOGE("car", "PIN CONFLICT: %s and %s both on GPIO%d",
                         map[i].name, map[j].name, map[i].pin);
            }
        }
    }
    if (conflicts == 0) {
        ESP_LOGI("car", "pin check: %d functions, no GPIO conflicts", n);
    }
}

/* -------------------------- adjustable live settings --------------------- */
static uint8_t  s_set_speed_cap   = 50;   /* manual speed cap % (replaces hardcoded 50) */
static uint8_t  s_set_engine_vol  = 100;  /* engine volume % scale */
static uint8_t  s_set_obstacle_cm = 30;   /* obstacle slow-down distance (cm) */
static uint8_t  s_set_led_bright  = 100;  /* static LED brightness % */
static uint8_t  s_set_tft_bright  = 100;  /* TFT backlight brightness % (guide 10/11) */
static uint8_t  s_set_mist_max    = 6;    /* mist max run time x10s, 0 = unlimited (guide 11) */

/* roof light persisted settings (PART 7): mode + brightness + steady color
   are stored (same keys roof_m/roof_b + new roof_c), the enabled flag is
   NEVER stored - roof always boots OFF. Default = POLICE. */
static uint8_t s_roof_mode  = ROOF_LIGHT_POLICE;
static uint8_t s_roof_bright = 70;
static uint8_t s_roof_col = 0;

/* snapshot current roof-module state into the persisted vars (called before
   every NVS save so panel changes get captured by any save path) */
static void roof_sync_to_storage(void)
{
    const roof_light_state_t *rl = roof_light_state();
    s_roof_mode  = (uint8_t)rl->mode;
    s_roof_bright = rl->brightness;
    s_roof_col = rl->color_idx;
}

/* settings schema version — bump when defaults change so stale NVS values
   get replaced instead of persisted. v4: PART 7 roof modes + POLICE default. */
#define SETTINGS_VER  4

static void settings_nvs_load(void)
{
    /* start from compile-time defaults */
    uint8_t defaults[CAR_GEAR_COUNT] = {5, 15, 25, 35, 50};

    nvs_handle_t h;
    if (nvs_open("car_set", NVS_READONLY, &h) != ESP_OK) {
        memcpy(s_gear_caps, defaults, sizeof(defaults));
        return;
    }
    uint32_t v;
    if (nvs_get_u32(h, "spd", &v) == ESP_OK && v <= 100) s_set_speed_cap = (uint8_t)v;
    if (nvs_get_u32(h, "vol", &v) == ESP_OK && v <= 100) s_set_engine_vol = (uint8_t)v;
    if (nvs_get_u32(h, "obs", &v) == ESP_OK && v <= 255) s_set_obstacle_cm = (uint8_t)v;
    if (nvs_get_u32(h, "led", &v) == ESP_OK && v <= 100) s_set_led_bright = (uint8_t)v;
    if (nvs_get_u32(h, "tftb", &v) == ESP_OK && v <= 100) s_set_tft_bright = (uint8_t)v;
    if (nvs_get_u32(h, "mist", &v) == ESP_OK && v <= 12) s_set_mist_max = (uint8_t)v;
    if (nvs_get_u32(h, "roof_m", &v) == ESP_OK && v < ROOF_LIGHT_COUNT) s_roof_mode = (uint8_t)v;
    if (nvs_get_u32(h, "roof_b", &v) == ESP_OK && v <= 100) s_roof_bright = (uint8_t)v;
    if (nvs_get_u32(h, "roof_c", &v) == ESP_OK && v < ROOF_COLOR_COUNT) s_roof_col = (uint8_t)v;

    uint32_t ver = 0;
    bool gears_valid = false;
    if (nvs_get_u32(h, "ver", &ver) != ESP_OK || ver != SETTINGS_VER) {
        ESP_LOGI(TAG, "settings schema v%u != NVS v%u — applying new defaults", SETTINGS_VER, ver);
        s_roof_mode = ROOF_LIGHT_POLICE;   /* v4 migration: PART 7 default */
        s_roof_col = 0;
    } else if (nvs_get_u32(h, "gears", &v) == ESP_OK) {
        uint8_t tmp[CAR_GEAR_COUNT];
        bool ascending = true;
        for (int i = 0; i < CAR_GEAR_COUNT; i++) {
            tmp[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
            if (tmp[i] < 10) tmp[i] = 10;            /* min 10% */
            if (i > 0 && tmp[i] < tmp[i - 1]) ascending = false;
        }
        if (ascending) {
            memcpy(s_gear_caps, tmp, sizeof(tmp));
            gears_valid = true;
        }
    }
    if (!gears_valid) {
        memcpy(s_gear_caps, defaults, sizeof(defaults));
        ESP_LOGI(TAG, "gear caps reset to defaults: %d/%d/%d/%d/%d",
                 s_gear_caps[0], s_gear_caps[1], s_gear_caps[2],
                 s_gear_caps[3], s_gear_caps[4]);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "settings loaded: spd=%d vol=%d obs=%d led=%d gears=%d/%d/%d/%d/%d",
             s_set_speed_cap, s_set_engine_vol, s_set_obstacle_cm, s_set_led_bright,
             s_gear_caps[0], s_gear_caps[1], s_gear_caps[2], s_gear_caps[3], s_gear_caps[4]);
}

static void settings_nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open("car_set", NVS_READWRITE, &h) != ESP_OK) return;
    roof_sync_to_storage();
    nvs_set_u32(h, "spd", s_set_speed_cap);
    nvs_set_u32(h, "vol", s_set_engine_vol);
    nvs_set_u32(h, "obs", s_set_obstacle_cm);
    nvs_set_u32(h, "led", s_set_led_bright);
    nvs_set_u32(h, "tftb", s_set_tft_bright);
    nvs_set_u32(h, "mist", s_set_mist_max);
    nvs_set_u32(h, "roof_m", s_roof_mode);
    nvs_set_u32(h, "roof_b", s_roof_bright);
    nvs_set_u32(h, "roof_c", s_roof_col);
    nvs_set_u32(h, "ver", SETTINGS_VER);          /* schema version for migration */
    uint32_t gp = 0;
    for (int i = 0; i < CAR_GEAR_COUNT; i++) gp |= ((uint32_t)s_gear_caps[i] & 0xFF) << (8 * i);
    nvs_set_u32(h, "gears", gp);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "settings saved");
}

/* apply persisted roof pattern/brightness/color to the roof module (boot only;
   the enabled flag is deliberately NOT restored - roof boots OFF). */
void car_roof_apply_saved(void)
{
    roof_light_set_mode((roof_light_mode_t)s_roof_mode);
    roof_light_set_brightness(s_roof_bright);
    roof_light_set_color_idx(s_roof_col);
    ESP_LOGI(TAG, "roof settings applied: mode=%u bright=%u color=%u (OFF at boot)",
             s_roof_mode, s_roof_bright, s_roof_col);
}

/* factory reset (premium guide 11): erase the settings namespace, reload
   compile-time defaults into RAM. Caller decides when to restart. */
void car_settings_factory_reset(void)
{
    nvs_handle_t h;
    if (nvs_open("car_set", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    uint8_t defaults[CAR_GEAR_COUNT] = {5, 15, 25, 35, 50};
    memcpy(s_gear_caps, defaults, sizeof(defaults));
    s_set_speed_cap   = 50;
    s_set_engine_vol  = 100;
    s_set_obstacle_cm = 30;
    s_set_led_bright  = 100;
    s_set_tft_bright  = 100;
    s_set_mist_max    = 6;
    s_roof_mode       = ROOF_LIGHT_POLICE;
    s_roof_bright     = 70;
    s_roof_col        = 0;
    tft_set_brightness(100);
    roof_light_set_mode(ROOF_LIGHT_OFF);
    roof_light_set_enabled(false);
    ESP_LOGW(TAG, "factory reset: NVS erased, defaults applied");
}

/* system restart (premium guide 4/11): safe shutdown then reboot. Called only
   from Diagnostics > System with A hold 1.5 s - never from a button long-press. */
static void motors_apply(void);   /* forward decl: defined below */
void car_system_restart(void)
{
    ESP_LOGW(TAG, "system restart requested (safe shutdown)");
    roof_light_force_safe_off();
    g.tgt_l = 0; g.tgt_r = 0;
    motors_apply();
    vTaskDelay(pdMS_TO_TICKS(150));   /* let the zero-command reach the D1 slave */
    esp_restart();
}

/* ------------------------------- servos -------------------------------- */

static void servo_us(ledc_channel_t ch, uint16_t us)
{
    uint32_t duty = (uint32_t)us * 16384 / 20000; /* 14-bit @ 50Hz */
    ledc_set_duty_and_update(LEDC_LOW_SPEED_MODE, ch, duty, 0);
}

static void servo_deg(ledc_channel_t ch, uint16_t deg)
{
    if (ch == LEDC_CHANNEL_2 || ch == LEDC_CHANNEL_3) return; /* gripper/spoiler servos removed */
    if (deg > 180) {
        deg = 180;
    }
    servo_us(ch, (uint16_t)(500 + deg * 2000 / 180));
}

static void buzzer_tone(uint16_t freq, uint8_t vol) // DISABLED - amp does horn, TFT BL now on GPIO18
{
    (void)freq; (void)vol; // no buzzer hardware
}

/* -------------------------------- motors -------------------------------- */
/* D1 motor slave: S3 -> UART TX GPIO1 @115200 -> D1 RX  "L<l>,R<r>\n" */

static void motors_apply(void)
{
    char buf[32];
    int l = g.cur_l, r = g.cur_r;
    if (g.brake_on) {
        l = 0; r = 0;
    }
    int n = snprintf(buf, sizeof(buf), "L%d,R%d\n", l, r);
    int wrc = uart_write_bytes(UART_NUM_1, buf, n);
    static uint32_t s_last_log, s_last_ulen;
    uint32_t now = now_ms();
    if (wrc != n && (int32_t)(now - s_last_ulen) >= 0) {
        s_last_ulen = now + 5000;
        ESP_LOGW(TAG, "D1 TX fail (%d/%d) - slave may hold last speed", wrc, n);
    }
    if (now - s_last_log > 5000) { s_last_log = now; ESP_LOGI(TAG, "D1 TX %s", buf); }
}

/* ----------------------------- ultrasonic ------------------------------- */

static bool s_us_front_ok; /* front sensor ne real echo diya? (noise ignore) */
static bool s_us_beep_mute = true; /* default silent */

static void us_read(void)
{
    /* Round-robin 3 sensors: L,R,F -> ~5ms max per call (was 16ms blocking) */
    static uint8_t seq = 0;
    static uint16_t s_us_last = 0;
    static uint8_t s_us_cnt = 0;
    static uint32_t s_us_ok_since = 0;
    seq = (seq + 1) % 3;

    // Helper: reduced timeout 12000us ( ~200cm) and lighter yield (every 60us)
    if (seq == 0) { /* LEFT */
        gpio_set_level(PIN_US_TRIG_L, 1); esp_rom_delay_us(10); gpio_set_level(PIN_US_TRIG_L, 0);
        int64_t rise = 0, fall = 0;
        int64_t t0 = esp_timer_get_time();
        while (esp_timer_get_time() - t0 < 12000) {
            if (!rise && gpio_get_level(PIN_US_ECHO_L)) rise = esp_timer_get_time();
            else if (rise && !fall && !gpio_get_level(PIN_US_ECHO_L)) fall = esp_timer_get_time();
            if (rise && fall) break;
            esp_rom_delay_us(10);
            if ((esp_timer_get_time() - t0) % 200 < 10) taskYIELD();
        }
        if (rise && fall) {
            uint16_t cm = (uint16_t)((fall - rise) / 58);
            g.dist[0] = cm > 400 ? 400 : cm;
        } else g.dist[0] = 65535;
    } else if (seq == 1) { /* RIGHT */
        gpio_set_level(PIN_US_TRIG_R, 1); esp_rom_delay_us(10); gpio_set_level(PIN_US_TRIG_R, 0);
        int64_t rise = 0, fall = 0;
        int64_t t0 = esp_timer_get_time();
        while (esp_timer_get_time() - t0 < 12000) {
            if (!rise && gpio_get_level(PIN_US_ECHO_R)) rise = esp_timer_get_time();
            else if (rise && !fall && !gpio_get_level(PIN_US_ECHO_R)) fall = esp_timer_get_time();
            if (rise && fall) break;
            esp_rom_delay_us(10);
            if ((esp_timer_get_time() - t0) % 200 < 10) taskYIELD();
        }
        if (rise && fall) {
            uint16_t cm = (uint16_t)((fall - rise) / 58);
            g.dist[2] = cm > 400 ? 400 : cm;
        } else g.dist[2] = 65535;
    } else if (seq == 2) { /* FRONT */
        gpio_set_level(PIN_US_TRIG_F, 1); esp_rom_delay_us(10); gpio_set_level(PIN_US_TRIG_F, 0);
        int64_t rise = 0, fall = 0;
        int64_t t0 = esp_timer_get_time();
        while (esp_timer_get_time() - t0 < 12000) {
            if (!rise && gpio_get_level(PIN_US_ECHO_F)) rise = esp_timer_get_time();
            else if (rise && !fall && !gpio_get_level(PIN_US_ECHO_F)) fall = esp_timer_get_time();
            if (rise && fall) break;
            esp_rom_delay_us(10);
            if ((esp_timer_get_time() - t0) % 200 < 10) taskYIELD();
        }
        if (rise && fall) {
            uint16_t cm = (uint16_t)((fall - rise) / 58);
            g.dist[1] = cm > 400 ? 400 : cm;
            if (cm > 2 && cm < 400) {
                if (s_us_last && (cm > s_us_last + s_us_last / 4 || cm < s_us_last - s_us_last / 4)) {
                    s_us_cnt = 0;
                    if (s_us_front_ok && s_us_ok_since && now_ms() - s_us_ok_since > 2000) s_us_front_ok = false;
                } else if (++s_us_cnt >= 3) {
                    s_us_front_ok = true;
                    s_us_ok_since = now_ms();
                }
                s_us_last = cm;
                if (s_us_ok_since && now_ms() - s_us_ok_since > 3000) {
                    s_us_front_ok = false;
                    s_us_cnt = 0;
                }
            }
        } else {
            g.dist[1] = 65535;
            // if no echo for 3s, invalidate front_ok
            if (s_us_ok_since && now_ms() - s_us_ok_since > 3000) {
                s_us_front_ok = false;
                s_us_cnt = 0;
            }
        }
    }
    g.dist[3] = 65535; // rear removed
    // global timeout: if front_ok but no success for 3s, clear
    if (s_us_front_ok && s_us_ok_since && now_ms() - s_us_ok_since > 3500) {
        s_us_front_ok = false;
    }
}

/* -------------------------------- battery ------------------------------- */
// Battery function REMOVED - GPIO1 now used for motor UART TX, GPIO14 = backlight relay

/* --------------------------------- OLED --------------------------------- */

static i2c_master_dev_handle_t s_oled;
static i2c_master_bus_handle_t s_bus;
static uint8_t s_fb[1024];
static uint8_t s_txb[1025];

static void oled_cmd(uint8_t c)
{
    uint8_t b[2] = { 0x00, c };
    i2c_master_transmit(s_oled, b, 2, 50);
}

static void oled_flush(void)
{
    s_txb[0] = 0x40;
    memcpy(s_txb + 1, s_fb, 1024);
    i2c_master_transmit(s_oled, s_txb, 1025, 100);
}

static void oled_px(int x, int y, bool on)
{
    if (x < 0 || x > 127 || y < 0 || y > 63) {
        return;
    }
    uint8_t *p = &s_fb[(y >> 3) * 128 + x];
    if (on) {
        *p |= (uint8_t)(1u << (y & 7));
    } else {
        *p &= (uint8_t)~(1u << (y & 7));
    }
}

static void oled_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

static void oled_rect(int x1, int y1, int x2, int y2, bool fill)
{
    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            if (fill || x == x1 || x == x2 || y == y1 || y == y2) {
                oled_px(x, y, true);
            }
        }
    }
}

static const uint8_t FONT5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},
    {0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},
    {0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},
    {0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},
    {0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},
    {0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},
    {0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x41,0x51,0x32},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},
    {0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},
    {0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},
    {0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},
    {0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},
    {0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},
    {0x20,0x40,0x44,0x3D,0x00},{0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},
    {0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},
    {0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},
    {0x08,0x04,0x08,0x10,0x08},
};

static void oled_text(int x, int y, const char *s)
{
    for (; *s; s++) {
        if (*s < 32 || *s > 126) {
            continue;
        }
        const uint8_t *gr = FONT5x7[*s - 32];
        for (int c = 0; c < 5; c++) {
            for (int r = 0; r < 7; r++) {
                if (gr[c] & (1u << r)) {
                    oled_px(x + c, y + r, true);
                }
            }
        }
        x += 6;
        if (x > 122) {
            return;
        }
    }
}
static void oled_text_big(int x, int y, const char *s, int scale)
{
    for (; *s; s++) {
        if (*s < 32 || *s > 126) continue;
        const uint8_t *gr = FONT5x7[*s - 32];
        for (int c = 0; c < 5; c++) for(int r=0;r<7;r++) if(gr[c]&(1u<<r)){
            for(int dy=0;dy<scale;dy++) for(int dx=0;dx<scale;dx++) oled_px(x+c*scale+dx, y+r*scale+dy, true);
        }
        // gap
        x += 6*scale;
        if (x > 128-6*scale) return;
    }
}

static void oled_init(void)
{
    static const uint8_t seq[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14,
        0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1,
        0xDB, 0x40, 0xA4, 0xA6, 0x2E, 0xAF,
    };
    for (size_t i = 0; i < sizeof(seq); i++) {
        oled_cmd(seq[i]);
    }
    oled_clear();
    oled_flush();
}

/* -------------------------------- gyro ---------------------------------- */
/* MPU-6500 driver lives in imu_driver.c (imu_driver_init/imu_driver_poll).
   Attached to the shared I2C bus in car_app_main; polled in the main loop. */

/* ------------------------------- LED strip ------------------------------ */

static led_strip_handle_t s_strip;
static led_strip_handle_t s_front_l;
static led_strip_handle_t s_front_r;
static led_strip_handle_t s_under;
/* PART 12: pass-light flash window + under-car solo state (used by
   strip_update/drive_output above their original declaration point) */
static uint32_t s_pass_until;
static bool s_under_solo;
uint8_t g_static_r=255,g_static_g=160,g_static_b=40; // Y button cycles

/* strip color order: agar colors galat dikhe to ye macro badlo.
   Magenta dikha => RBG (byte order swap). Hamesha 1 hi ON rakho */
#define STRIP_ORDER_RBG 1   /* 1 = RBG, 0 = normal RGB */
/* BUG-fix 2026-09-17: SETTINGS > "LED BRIGHT" (s_set_led_bright) ka koi asar
   nahi tha. Ab har pixel write (rear/under strip + front LEDs) isi setting se
   scale hoti hai. Default 100 -> behaviour pehle jaisa. */
static uint8_t led_scale(uint8_t v)
{
    if (s_set_led_bright >= 100) return v;
    return (uint8_t)((uint16_t)v * s_set_led_bright / 100u);
}

static void strip_px(uint32_t i, uint8_t r, uint8_t g, uint8_t b)
{
#if STRIP_ORDER_RBG
    led_strip_set_pixel(s_strip, i, led_scale(r), led_scale(b), led_scale(g));
#else
    led_strip_set_pixel(s_strip, i, led_scale(r), led_scale(g), led_scale(b));
#endif
}

/* ---- under-car strip (DAISY-CHAINED after rear strip, same GPIO 6 line) ----
   Pixels [LED_STRIP_NUM .. LED_STRIP_NUM+UNDER_STRIP_NUM) = under LEDs. */
static void under_px(uint32_t i, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_under) return;
#if STRIP_ORDER_RBG
    led_strip_set_pixel(s_under, LED_STRIP_NUM + i, led_scale(r), led_scale(b), led_scale(g));
#else
    led_strip_set_pixel(s_under, LED_STRIP_NUM + i, led_scale(r), led_scale(g), led_scale(b));
#endif
}

static void under_set_all(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_under) return;
    for (int i = 0; i < UNDER_STRIP_NUM; i++) under_px(i, r, g, b);
    led_strip_refresh(s_under);
}

static void hsv2rgb(uint16_t h, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t region = (uint8_t)(h / 43);
    uint8_t rem = (uint8_t)((h - region * 43) * 6);
    uint8_t p = 0, q = 255 - rem, t = rem;
    switch (region) {
    case 0: *r = 255; *g = t; *b = p; break;
    case 1: *r = q; *g = 255; *b = p; break;
    case 2: *r = p; *g = 255; *b = t; break;
    case 3: *r = p; *g = q; *b = 255; break;
    case 4: *r = t; *g = p; *b = 255; break;
    default: *r = 255; *g = p; *b = q; break;
    }
}

static void strip_update(uint32_t now)
{
    uint16_t blink = (now / 500) & 1;          /* indicator blink 1Hz */
    uint16_t fast = (now / 150) & 1;           /* siren/strobe 3.3Hz */
    bool sig_l = g.sig_l, sig_r = g.sig_r;
    /* auto indicators already set by auto_logic() — no override needed */

    /* Front LEDs control (dedicated separate pins 17 and 15) */
    {
        uint8_t fr = 0, fg = 0, fb = 0;
        uint8_t fl_r = 0, fl_g = 0, fl_b = 0;

        if (g.hazard) {
            if (blink) {
                fl_r = 255; fl_g = 0; fl_b = 0; // Red blink
                fr = 255; fg = 0; fb = 0;
            }
        } else {
            /* base color: headlight white, otherwise off */
            if (g.headlight) {
                fl_r = 255; fl_g = 255; fl_b = 255;
                fr = 255; fg = 255; fb = 255;
            }
            /* indicator overrides: orange blink (ON phase) / off (OFF phase) */
            if (sig_l) {
                if (blink) {
                    fl_r = 255; fl_g = 120; fl_b = 0; // Orange ON
                } else {
                    fl_r = 0; fl_g = 0; fl_b = 0;     // OFF phase (dark)
                }
            }
            if (sig_r) {
                if (blink) {
                    fr = 255; fg = 120; fb = 0; // Orange ON
                } else {
                    fr = 0; fg = 0; fb = 0;     // OFF phase (dark)
                }
            }
        }

        if (s_front_l) { led_strip_set_pixel(s_front_l, 0, led_scale(fl_r), led_scale(fl_g), led_scale(fl_b)); led_strip_refresh(s_front_l); }
        else { gpio_set_level(PIN_FRONT_LED_L, (fl_r || fl_g || fl_b) ? 1 : 0); }

        if (s_front_r) { led_strip_set_pixel(s_front_r, 0, led_scale(fr), led_scale(fg), led_scale(fb)); led_strip_refresh(s_front_r); }
        else { gpio_set_level(PIN_FRONT_LED_R, (fr || fg || fb) ? 1 : 0); }
    }

    if (g.hazard) {
        if (fast) {
            strip_px(0, 255, 0, 0); strip_px(1, 255, 0, 0);
        } else {
            strip_px(0, 0, 0, 0); strip_px(1, 0, 0, 0);
        }
        led_strip_refresh(s_strip);
    } else if (g.cooldown) {
        if (blink) {
            strip_px(0, 255, 0, 0); strip_px(1, 255, 0, 0);
        } else {
            strip_px(0, 0, 0, 0); strip_px(1, 0, 0, 0);
        }
        led_strip_refresh(s_strip);
    } else if (sig_l || sig_r) {
        strip_px(0, 0, 0, 0); strip_px(1, 0, 0, 0); // Clear rear strip first
        if (sig_l && !sig_r) {
            /* LEFT ONLY: pixel 0 (hw triples to 3 physical LEDs) */
            if (blink) {
                strip_px(0, 255, 120, 0);
            }
        }
        if (sig_r && !sig_l) {
            /* RIGHT ONLY: pixel 1 (hw triples to 3 physical LEDs) */
            if (blink) {
                strip_px(1, 255, 120, 0);
            }
        }
        if (sig_l && sig_r) {
            /* BOTH: pixel 0+1 orange (auto REVERSE/ESCAPE/SLOW/PAUSE) */
            if (blink) {
                strip_px(0, 255, 120, 0); strip_px(1, 255, 120, 0);
            }
        }
        led_strip_refresh(s_strip);
    } else {
        switch (g.led_mode) {
        case 0:
            strip_px(0, 0, 0, 0); strip_px(1, 0, 0, 0);
            led_strip_refresh(s_strip);
            break;
        case 1:
            strip_px(0, g_static_r, g_static_g, g_static_b); strip_px(1, g_static_r, g_static_g, g_static_b);
            led_strip_refresh(s_strip);
            break;
        case 2: {
            for (int i = 0; i < LED_STRIP_NUM; i++) {
                uint8_t r, g2, b;
                hsv2rgb((uint16_t)(((now / 20) + i * 26) % 256), &r, &g2, &b);
                strip_px(i, r, g2, b);
            }
            led_strip_refresh(s_strip);
            break;
        }
        case 3: {
            uint8_t pos = (uint8_t)((now / 80) % (LED_STRIP_NUM + 4));
            for (int i = 0; i < LED_STRIP_NUM; i++) {
                uint8_t d = (uint8_t)(pos > i ? pos - i : i - pos);
                uint8_t v = d <= 3 ? (uint8_t)(60 - d * 20) : 0;
                strip_px(i, v, v, v);
            }
            led_strip_refresh(s_strip);
            break;
        }
        default: /* siren */
            if (fast) {
                strip_px(0, 255, 0, 0); strip_px(1, 255, 0, 0);
            } else {
                strip_px(0, 0, 0, 255); strip_px(1, 0, 0, 255);
            }
            led_strip_refresh(s_strip);
            break;
        }
    }

    /* ---- under-car RGB strip (GPIO 6 pe daisy-chained, 12 LEDs) ----
       PART 12: solo toggle forces static white independent of mode.
       1) hazard  -> all red blink
       2) indicator L/R -> left/right 6 LEDs orange blink
       3) solo   -> static white
       4) else    -> ambient static color when led_mode==1, rainbow chase when
                      led_mode==2, off otherwise */
    if (s_under) {
        if (g.hazard) {
            under_set_all(blink ? 255 : 0, 0, 0);
        } else if (sig_l || sig_r) {
            uint8_t r0 = 0, g0 = 0, b0 = 0;
            if (blink) { r0 = 255; g0 = 120; b0 = 0; }
            for (int i = 0; i < UNDER_STRIP_NUM; i++) {
                uint8_t r = (sig_l && i < 6) ? r0 : 0;
                uint8_t g = (sig_r && i >= 6) ? g0 : 0;
                uint8_t b = (sig_l || sig_r) ? b0 : 0;
                under_px(i, r, g, b);
            }
            led_strip_refresh(s_under);
        } else if (s_under_solo) {
            under_set_all(255, 255, 255);
        } else if (g.led_mode == 1) {
            under_set_all(g_static_r, g_static_g, g_static_b);
        } else if (g.led_mode == 2) {
            for (int i = 0; i < UNDER_STRIP_NUM; i++) {
                uint8_t r, g2, b;
                hsv2rgb((uint16_t)(((now / 20) + i * 22) % 256), &r, &g2, &b);
                under_px(i, r, g2, b);
            }
            led_strip_refresh(s_under);
        } else {
            under_set_all(0, 0, 0);
        }
    }
}

/* ---------------------------- audio (MAX98357A) -------------------------- */
/* I2S 22050Hz mono 16-bit. Engine sound speed ke hisaab se, effect tones
   (horn/siren/beep) on top. State car_task set karta hai, audio task render
   karta hai. BCLK=39 LRC=47 DIN=40 (GPIO35 = octal PSRAM pin, N16R8), SD -> 3V3. */

static i2s_chan_handle_t s_i2s_tx;
static volatile bool     s_audio_paused = false;   /* pause ke dauraan engine silent */

/* Shared I2S0 TX channel (mutex-protected, engine audio owner) */
i2s_chan_handle_t car_i2s0_handle(void) { return s_i2s_tx; }

/* Engine sound pause (I2S0 ek waqt pe ek writer) */
void car_audio_pause(bool pause) { s_audio_paused = pause; }

static volatile uint32_t s_effect_until;
static volatile uint16_t s_effect_freq;
static volatile uint16_t s_effect_amp;   /* effect volume (reverse beep halka) */
static volatile int16_t s_engine_spd;   /* signed -100..100 */
static volatile bool s_horn_on;                 /* LB hold = continuous horn (audio task reads) */
static volatile bool s_y_held;            /* Y hold = intro-test (audio task reads) */
static uint32_t s_horn_until;           /* oneshot horn (Guide) */
static uint32_t s_rumble_until;         /* haptic auto-stop */
static uint8_t s_rumble_l, s_rumble_r;
static uint32_t s_sweep_until;          /* turbo whoosh (rising sweep) */
static uint16_t s_sweep_f0, s_sweep_f1;
static uint16_t s_sweep_amp;            /* sweep volume */

/* optional siren (premium guide 6.5): LB+B hold 1s toggles. Audio only sounds
   while roof light is enabled in Drive - OFF on menu/estop/disconnect. */
static bool     s_siren_on   = false;

/* mode-change blip (synth, instant) */
static uint32_t s_blip_until;           /* mode-change blip */
static uint16_t s_blip_freq;

/* ------------------------ embedded WAV sounds ---------------------------- */
extern const uint8_t _binary_engine_loop_wav_start[];
extern const uint8_t _binary_engine_loop_wav_end[];
extern const uint8_t _binary_hover_loop_wav_start[];
extern const uint8_t _binary_hover_loop_wav_end[];
extern const uint8_t _binary_brake_wav_start[];
extern const uint8_t _binary_brake_wav_end[];
extern const uint8_t _binary_stop_wav_start[];
extern const uint8_t _binary_stop_wav_end[];
extern const uint8_t _binary_gripper_wav_start[];
extern const uint8_t _binary_gripper_wav_end[];
extern const uint8_t _binary_laser_wav_start[];
extern const uint8_t _binary_laser_wav_end[];
extern const uint8_t _binary_yclick_wav_start[];
extern const uint8_t _binary_yclick_wav_end[];
extern const uint8_t _binary_a_click_wav_start[];
extern const uint8_t _binary_a_click_wav_end[];
#include "learning_adv.h"
#include "esp_spiffs.h"   /* esp_vfs_spiffs_register + esp_spiffs_info (IDF6) */

/* PART 5 boot trim: startup/horn/intro moved from embedded .h (~1.3MB flash)
   to SPIFFS .raw, loaded into PSRAM at boot. Sample rates unchanged. */
#define STARTUP_SOUND_RATE 8000
#define INTRO_TEST_SOUND_RATE 8000
static uint8_t *s_boot_horn;    static uint32_t s_boot_horn_len;
static uint8_t *s_boot_intro;   static uint32_t s_boot_intro_len;

static void spiffs_mount(void)
{
    static bool s_mounted;
    if (s_mounted) return;
    esp_vfs_spiffs_conf_t c = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 8,
        .format_if_mount_failed = false,
    };
    esp_err_t e = esp_vfs_spiffs_register(&c);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "spiffs mount fail: %s", esp_err_to_name(e));
        return;
    }
    s_mounted = true;
    size_t t = 0, u = 0;
    if (esp_spiffs_info("storage", &t, &u) == ESP_OK)
        ESP_LOGI(TAG, "spiffs %u/%u KB used", (unsigned)(u / 1024), (unsigned)(t / 1024));
}

static bool boot_snd_load(const char *path, uint8_t **out, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "missing %s", path); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (long)(512 * 1024)) { fclose(f); return false; }
    uint8_t *buf = heap_caps_malloc((size_t)n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { fclose(f); return false; }
    /* chunked with yields: blocking SPIFFS reads never yield by themselves
       and would starve IDLE1 into a task-WDT trigger. NOTE: 100Hz tick, so
       minimum real delay is 10ms (pdMS_TO_TICKS(5) == 0 == pure yield!). */
    size_t off = 0;
    while (off < (size_t)n) {
        size_t chunk = (size_t)n - off > 8192 ? 8192 : (size_t)n - off;
        if (fread(buf + off, 1, chunk, f) != chunk) {
            fclose(f);
            heap_caps_free(buf);
            return false;
        }
        off += chunk;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    fclose(f);
    *out = buf;
    *len = (uint32_t)n;
    ESP_LOGI(TAG, "loaded %s (%lu B PSRAM)", path, (unsigned long)n);
    return true;
}

static void boot_sounds_init(void)
{
    spiffs_mount();
    /* NOTE: startup_sound.raw is NOT loaded (instant-boot: 14s boot jingle
       dropped; saves 228KB PSRAM). Horn/intro still load for LB/Y-hold. */
    boot_snd_load("/spiffs/horn_sound.raw", &s_boot_horn, &s_boot_horn_len);
    boot_snd_load("/spiffs/intro_test_sound.raw", &s_boot_intro, &s_boot_intro_len);
}

/* simple loop/one-shot player, position 16.16 fixed point, len = bytes */
typedef struct {
    const uint8_t *d;
    uint32_t len;
    uint32_t pos;
    uint8_t loop;
    uint16_t vol;   /* 0..1000 */
} wav_t;

static wav_t s_eng, s_hov, s_brk, s_stp, s_shot, s_startup, s_horn_wav, s_intro_wav;
static uint32_t s_startup_out, s_intro_out;
/* PART 5 sound-bank voice (16kHz PCM, resampled in mixer like intro) */
static wav_t s_snd;
static uint32_t s_snd_out;
#define SND_BANK_RATE 16000
static portMUX_TYPE s_wav_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t s_ry_tick_until; /* RY (tilt) par indicator-style tick */
static bool s_mist_tog;          /* A hold = mist toggle (ON rehta hai) */

static void wav_play(wav_t *w, const uint8_t *d, uint32_t len, uint8_t loop, uint16_t vol)
{
    portENTER_CRITICAL(&s_wav_mux);
    w->d = d;
    w->len = len;
    w->pos = 0;
    w->loop = loop;
    w->vol = vol;
    portEXIT_CRITICAL(&s_wav_mux);
}

static void wav_stop(wav_t *w)
{
    portENTER_CRITICAL(&s_wav_mux);
    w->d = NULL;
    portEXIT_CRITICAL(&s_wav_mux);
}

void car_snd_play_pcm(const uint8_t *d, uint32_t len, uint16_t vol)
{
    if (!d || !len) return;
    portENTER_CRITICAL(&s_wav_mux);
    s_snd.d = d;
    s_snd.len = len;
    s_snd.pos = 0;
    s_snd.loop = 0;
    s_snd.vol = vol;
    s_snd_out = 0;
    portEXIT_CRITICAL(&s_wav_mux);
}

static void play_shot(const uint8_t *s, const uint8_t *e, uint16_t vol)
{
    if (!s || !e || e <= s + 44) return;
    wav_play(&s_shot, s + 44, (uint32_t)(e - s) - 44, 0, vol);
}

static uint32_t s_boot_ms;
static void cockpit_toast(const char *msg, uint32_t now);   /* fwd (defined below) */
/* PART 13.2 rumble presets + sequence drain. All rumble originates in
   car_task (UI + MPU event handlers); the drain runs in rumble_update()
   on the same task - NEVER from ISR or mpu_fast_task (Part 4.1 note).
   (A dedicated drain task would race the shared USB OUT pipe; the
   car_task drain gives the same guarantee with zero extra RAM.) */
typedef struct { uint8_t l, r; uint16_t ms; uint16_t gap; } rumble_step_t;
#define RUMBLE_MAX_STEPS 4
static rumble_step_t s_seq[RUMBLE_MAX_STEPS];
static uint8_t s_seq_n, s_seq_i;
static uint32_t s_seq_gap_until;
static bool s_seq_gap;

typedef enum {
    RUMBLE_TICK,      /* right=60, 60ms - generic confirm */
    RUMBLE_GEAR,      /* right=80, 60ms - gear shift tick */
    RUMBLE_ERROR,     /* right=100, double-pulse 80/80 - invalid action */
    RUMBLE_ESTOP,     /* L+R=255, 400ms - emergency */
    RUMBLE_TURBO,     /* 180/180, 250ms - turbo kick */
    RUMBLE_REV,       /* low 100, 200ms - reverse beep feel */
    RUMBLE_PANIC,     /* 150/150, 200ms - panic hold */
    RUMBLE_TILT,      /* 255/255, 350ms - tilt critical */
    RUMBLE_ROLLOVER,  /* 255/255, 600ms - rollover continuous */
    RUMBLE_IMPACT_L,  /* 80/80, 100ms */
    RUMBLE_IMPACT_M,  /* 150/150, 200ms */
    RUMBLE_IMPACT_H,  /* 255/255, 400ms */
    RUMBLE_FREEFALL,  /* 60/60, 150ms */
    RUMBLE_LAND_S,    /* 150/150, 150ms - smooth touchdown */
    RUMBLE_LAND_H,    /* 255/255, 350ms - hard touchdown */
    RUMBLE_SLIP,      /* stutter 2x 120/80 */
    RUMBLE_STUCK,     /* stutter 2x 200/100 */
    RUMBLE_CORNER_L,  /* left 80, 180ms - turning left */
    RUMBLE_CORNER_R,  /* right 80, 180ms - turning right */
    RUMBLE_DRIFT      /* 100/100, 120ms */
} rumble_preset_t;

static void rumble_start_step(uint8_t i)
{
    uint8_t l = (uint8_t)(s_seq[i].l * 40 / 100);
    uint8_t r = (uint8_t)(s_seq[i].r * 40 / 100);
    uint32_t ms = (uint32_t)s_seq[i].ms * 70 / 100;
    if (l < 20 && r < 20) {   /* too light - skip to next */
        s_seq_i = i + 1;
        s_seq_gap_until = now_ms();
        s_seq_gap = true;
        s_rumble_until = 0;
        return;
    }
    xbox360_rumble(0, l, r);
    s_rumble_l = l; s_rumble_r = r;
    s_rumble_until = now_ms() + ms;
    s_seq_i = (uint8_t)(i + 1);
    s_seq_gap = false;
}

/* haptic: trigger rumble L/R (0-255) for ms duration — only if controller connected */
static void rumble(uint8_t l, uint8_t r, uint32_t ms)
{
    rumble_step_t one = { l, r, (uint16_t)ms, 0 };
    const xbox360_pad_t *p = xbox360_pad(0);
    if (!p || !p->present) return;
    if (now_ms() - s_boot_ms < 2000) return; /* boot pe 2s tak no rumble */
    s_seq[0] = one;
    s_seq_n = 1;
    rumble_start_step(0);
}

static void rumble_seq(const rumble_step_t *steps, uint8_t n)
{
    const xbox360_pad_t *p = xbox360_pad(0);
    if (!p || !p->present) return;
    if (now_ms() - s_boot_ms < 2000) return;
    if (!steps || !n) return;
    if (n > RUMBLE_MAX_STEPS) n = RUMBLE_MAX_STEPS;
    for (uint8_t i = 0; i < n; i++) s_seq[i] = steps[i];
    s_seq_n = n;
    rumble_start_step(0);
}

static void rumble_preset(rumble_preset_t p)
{
    switch (p) {
    case RUMBLE_TICK:   { rumble_step_t s[] = {{0, 60, 60, 0}}; rumble_seq(s, 1); break; }
    case RUMBLE_GEAR:   { rumble_step_t s[] = {{0, 80, 60, 0}}; rumble_seq(s, 1); break; }
    case RUMBLE_ERROR:  { rumble_step_t s[] = {{0, 100, 80, 80}, {0, 100, 80, 0}}; rumble_seq(s, 2); break; }
    case RUMBLE_ESTOP:  { rumble_step_t s[] = {{255, 255, 400, 0}}; rumble_seq(s, 1); break; }
    case RUMBLE_TURBO:  { rumble_step_t s[] = {{180, 180, 250, 0}}; rumble_seq(s, 1); break; }
    case RUMBLE_REV:    { rumble_step_t s[] = {{100, 0, 200, 0}}; rumble_seq(s, 1); break; }
    case RUMBLE_PANIC:  { rumble_step_t s[] = {{150, 150, 200, 0}}; rumble_seq(s, 1); break; }
    case RUMBLE_TILT:   { rumble_step_t s[] = {{255, 255, 350, 0}}; rumble_seq(s, 1); break; }
    case RUMBLE_ROLLOVER: { rumble_step_t s[] = {{255, 255, 600, 0}}; rumble_seq(s, 1); break; }
    case RUMBLE_IMPACT_L: { rumble_step_t s[] = {{80, 80, 100, 0}}; rumble_seq(s, 1); break; }
    case RUMBLE_IMPACT_M: { rumble_step_t s[] = {{150, 150, 200, 0}}; rumble_seq(s, 1); break; }
    case RUMBLE_IMPACT_H: { rumble_step_t s[] = {{255, 255, 400, 0}}; rumble_seq(s, 1); break; }
    case RUMBLE_FREEFALL: { rumble_step_t s[] = {{60, 60, 150, 0}}; rumble_seq(s, 1); break; }
    case RUMBLE_LAND_S: { rumble_step_t s[] = {{150, 150, 150, 0}}; rumble_seq(s, 1); break; }
    case RUMBLE_LAND_H: { rumble_step_t s[] = {{255, 255, 350, 0}}; rumble_seq(s, 1); break; }
    case RUMBLE_SLIP:   { rumble_step_t s[] = {{120, 120, 80, 80}, {120, 120, 80, 0}}; rumble_seq(s, 2); break; }
    case RUMBLE_STUCK:  { rumble_step_t s[] = {{200, 200, 150, 100}, {200, 200, 150, 0}}; rumble_seq(s, 2); break; }
    case RUMBLE_CORNER_L: { rumble_step_t s[] = {{80, 0, 180, 0}}; rumble_seq(s, 1); break; }
    case RUMBLE_CORNER_R: { rumble_step_t s[] = {{0, 80, 180, 0}}; rumble_seq(s, 1); break; }
    case RUMBLE_DRIFT:  { rumble_step_t s[] = {{100, 100, 120, 0}}; rumble_seq(s, 1); break; }
    default: break;
    }
}

static void rumble_update(uint32_t now)
{
    const xbox360_pad_t *p = xbox360_pad(0);
    if (!p || !p->present) {
        if (s_rumble_until) { xbox360_rumble(0,0,0); s_rumble_until=0; }
        s_seq_n = 0; s_seq_i = 0; s_seq_gap = false;
        return;
    }
    /* gap between sequence steps: start next when it elapses */
    if (s_seq_gap) {
        if ((int32_t)(now - s_seq_gap_until) >= 0) {
            s_seq_gap = false;
            if (s_seq_i < s_seq_n) rumble_start_step(s_seq_i);
            else s_seq_n = 0;
        }
        return;
    }
    if (s_rumble_until && now >= s_rumble_until) {
        xbox360_rumble(0, 0, 0);
        s_rumble_until = 0;
        s_rumble_l = s_rumble_r = 0;
        if (s_seq_i < s_seq_n) {
            /* gap stored on the step that just finished */
            uint16_t gap = (s_seq_i > 0) ? s_seq[s_seq_i - 1].gap : 0;
            if (gap) {
                s_seq_gap_until = now + gap;
                s_seq_gap = true;
            } else {
                rumble_start_step(s_seq_i);
            }
        } else {
            s_seq_n = 0;
        }
    }
}

/* PART 13.1 controller LED ring: drive status on the pad.
   Priority: E-STOP/lockout fast-flash > pairing rotate > AUTO slow-flash >
   low-batt pulse > gear quadrants (moving) > all-4 parked-healthy.
   Flashing is app-timed ON/OFF (no pattern-byte risk); driver rate-limits. */
static void led_tick(uint32_t now)
{
    /* no receiver = nothing to drive (also covers pre-pair searching) */
    if (!xbox360_dongle_connected()) return;
    const xbox360_pad_t *p0 = xbox360_pad(0);
    bool pad = p0 && p0->present;
    static uint32_t s_next;
    static uint8_t s_phase;
    if ((int32_t)(now - s_next) < 0) return;
    s_next = now + 150;   /* 150ms cadence */
    s_phase++;
    bool lockout = g.estop || imu_adv_rollover_latched();
    xbox360_led_pattern_t want;
    bool flashing = false;
    if (lockout) {
        want = ((s_phase & 1) ? XBOX_LED_ALL : XBOX_LED_OFF);
        flashing = true;
    } else if (!pad) {
        want = (xbox360_led_pattern_t)(XBOX_LED_Q1 + (s_phase % 4));
        flashing = true;
    } else if (g.mode == MODE_AUTO) {
        want = ((s_phase % 4 < 2) ? XBOX_LED_ALL : XBOX_LED_OFF);
        flashing = true;
    } else if (p0->battery_valid && p0->battery == 0) {
        want = ((s_phase % 4 < 2) ? XBOX_LED_Q1 : XBOX_LED_OFF);
        flashing = true;
    } else if (g.cur_l || g.cur_r || g.tgt_l || g.tgt_r) {
        want = (g.gear >= CAR_GEAR_COUNT - 1) ? XBOX_LED_ALL :
               (xbox360_led_pattern_t)(XBOX_LED_Q1 + g.gear);
    } else {
        want = XBOX_LED_ALL;
    }
    /* steady patterns: send once (USB OUT pipe stays free for rumble).
       Re-send after any flashing phase so the ring settles correctly. */
    static xbox360_led_pattern_t s_sent = XBOX_LED_COUNT;
    static bool s_was_flashing = true;
    if (!flashing && want == s_sent && !s_was_flashing) return;
    s_sent = want;
    s_was_flashing = flashing;
    xbox360_set_led(0, want);
}

/* PART 13.3 LOW BATTERY (<15%): banner toast on edge + 60s repeat.
   Requires a REAL battery report (battery_valid) - a fresh pairing reads
   level 0 until the first 3s poll answers, which is UNKNOWN, not low. */
static void lowbatt_watch(uint32_t now, const xbox360_pad_t *pad)
{
    static uint32_t s_last;
    bool low = pad && pad->present && pad->battery_valid && pad->battery == 0;
    if (low && (s_last == 0 || now - s_last > 60000)) {
        s_last = now;
        cockpit_toast("CONTROLLER LOW BATTERY", now);
        snd_play("sys_warning");
    }
    if (!low) s_last = 0;
}

/* PART 10 trip computer: odometry estimated from wheel commands
   (no wheel encoders; VMAX_EST ~2 m/s at 100%). Reset from TRIP window. */
#define TRIP_VMAX_EST_MS 2.0f
static uint32_t s_trip_ms;
static double s_trip_dist_m;
static uint16_t s_trip_top;
static uint32_t s_trip_spd_acc, s_trip_spd_n;
static float s_trip_max_tilt;

uint32_t trip_time_s(void) { return s_trip_ms / 1000; }
uint32_t trip_dist_m(void) { return (uint32_t)s_trip_dist_m; }
uint16_t trip_top_speed(void) { return s_trip_top; }
uint16_t trip_avg_speed(void)
{
    return s_trip_spd_n ? (uint16_t)(s_trip_spd_acc / s_trip_spd_n) : 0;
}
float trip_max_tilt(void) { return s_trip_max_tilt; }
void trip_reset(void)
{
    s_trip_ms = 0; s_trip_dist_m = 0; s_trip_top = 0;
    s_trip_spd_acc = 0; s_trip_spd_n = 0; s_trip_max_tilt = 0;
}

static void trip_update(int16_t cl, int16_t cr, float tilt, uint32_t dt_ms)
{
    float frac = ((cl < 0 ? -cl : cl) + (cr < 0 ? -cr : cr)) * 0.5f / 100.0f;
    if (frac > 1.0f) frac = 1.0f;
    uint16_t pct = (uint16_t)(frac * 100.0f);
    if (frac > 0.02f) {
        s_trip_ms += dt_ms;
        s_trip_dist_m += (double)frac * TRIP_VMAX_EST_MS * dt_ms / 1000.0;
        s_trip_spd_acc += pct;
        s_trip_spd_n++;
        if (pct > s_trip_top) s_trip_top = pct;
    }
    float at = tilt < 0 ? -tilt : tilt;
    if (at > s_trip_max_tilt) s_trip_max_tilt = at;
}

/* ---- MPU-advanced car behavior (1.md 4.3, ADDITIVE to GUIDE E-STOP) ---- */
static uint32_t s_land_hold_until;   /* motors stay 0 until 200 ms after landing */
static uint32_t s_land_ramp_until;   /* gentle recovery ramp 500 ms (no jerk) */

static void cockpit_toast(const char *msg, uint32_t now)
{
    snprintf(s_os.toast, sizeof(s_os.toast), "%s", msg);
    s_os.toast_until = now + 1200;
}

/* PART 12 dashcam snapshot: freeze the cockpit framebuffer into PSRAM, then
   stream it to SPIFFS chunked (task-WDT safe). Overwrites dashcam.raw. */
static void dashcam_save(void)
{
    uint32_t now = now_ms();
    uint16_t *fb = gfx_fb();
    if (!fb) return;
    uint8_t *copy = heap_caps_malloc(320u * 240u * 2u,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        cockpit_toast("SNAP NO MEM", now);
        return;
    }
    memcpy(copy, fb, 320u * 240u * 2u);
    FILE *f = fopen("/spiffs/dashcam.raw", "wb");
    if (!f) {
        heap_caps_free(copy);
        cockpit_toast("SNAP FAIL", now);
        return;
    }
    size_t off = 0, total = 320u * 240u * 2u;
    bool ok = true;
    while (off < total) {
        size_t chunk = total - off > 8192 ? 8192 : total - off;
        if (fwrite(copy + off, 1, chunk, f) != chunk) { ok = false; break; }
        off += chunk;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    fclose(f);
    heap_caps_free(copy);
    cockpit_toast(ok ? "SNAPSHOT SAVED" : "SNAP FAIL", now);
    if (ok) {
        snd_play("light_flash");
        rumble_preset(RUMBLE_TICK);
    }
    ESP_LOGI(TAG, "dashcam snapshot %s", ok ? "saved" : "FAILED");
}

/* Consume the imu_adv event bus: car actions + cockpit toasts/alerts +
   rumble. Called every LOOP_MS tick from car_task. */
static void imu_events_handle(uint32_t now)
{
    imu_adv_evt_t e;
    while (imu_adv_evt_recv(&e)) {
        switch (e.type) {
        case IMU_EVT_TILT_WARN:
            cockpit_toast("TILT WARN - SLOW DOWN", now);
            ESP_LOGW(TAG, "TILT_WARN %.1f deg (speed-cap suggestion)", (double)e.mag);
            break;
        case IMU_EVT_TILT_CRITICAL:
            /* motors forced to 0 immediately (gating below); clears on
               throttle-neutral via TILT_CLEAR + neutral-release lock */
            g.tgt_l = g.tgt_r = 0;
            os_alert(&s_os, ALERT_OBSTACLE, "TILT CRITICAL", now);
            if (!snd_play("sys_warning")) car_sfx_bad();
            notif_push(NOTIF_MPU, "TILT CRITICAL - motors cut", now);
            alexa_announce("Attention - car exceeded critical tilt, motors stopped automatically.", true);
            rumble_preset(RUMBLE_TILT);
            ESP_LOGE(TAG, "TILT_CRITICAL %.1f deg - motors cut", (double)e.mag);
            break;
        case IMU_EVT_TILT_ROLLOVER:
            /* sticky lockout - manual GUIDE ack required, Alexa CANNOT clear */
            g.tgt_l = g.tgt_r = 0;
            os_alert(&s_os, ALERT_SENSOR, "ROLLOVER - GUIDE TO ACK", now);
            notif_push(NOTIF_MPU, "ROLLOVER - ack required", now);
            snd_play("sys_estop");
            rumble_preset(RUMBLE_ROLLOVER);
            ESP_LOGE(TAG, "TILT_ROLLOVER %.1f deg - sticky lockout", (double)e.mag);
            break;
        case IMU_EVT_TILT_CLEAR:
            break;
        case IMU_EVT_IMPACT_LIGHT:
            ESP_LOGI(TAG, "impact light %.2fg (logged)", (double)e.mag);
            snd_play("impact_light");
            rumble_preset(RUMBLE_IMPACT_L);
            break;
        case IMU_EVT_IMPACT_MODERATE:
            cockpit_toast("BUMP LOGGED", now);
            snd_play("impact_moderate");
            rumble_preset(RUMBLE_IMPACT_M);
            ESP_LOGW(TAG, "impact moderate %.2fg (logged)", (double)e.mag);
            break;
        case IMU_EVT_IMPACT_HARD:
            /* always logged; HARD also triggers Alexa announce */
            os_alert(&s_os, ALERT_OBSTACLE, "HARD IMPACT", now);
            snd_play("impact_hard");
            notif_push(NOTIF_MPU, "HARD IMPACT", now);
            rumble_preset(RUMBLE_IMPACT_H);
            alexa_impact_note(3, e.mag, now);
            ESP_LOGE(TAG, "impact HARD %.2fg (logged + Alexa announce)", (double)e.mag);
            break;
        case IMU_EVT_FREEFALL:
            /* motors cut during airtime (gating via imu_adv_motors_cut) */
            snd_play("freefall_start");
            rumble_preset(RUMBLE_FREEFALL);
            ESP_LOGW(TAG, "FREEFALL confirmed - motors cut");
            break;
        case IMU_EVT_LANDING_SMOOTH:
        case IMU_EVT_LANDING_HARD: {
            bool hard = (e.type == IMU_EVT_LANDING_HARD);
            /* log to Drive Score (counters already in imu_adv) + gentle ramp */
            s_land_hold_until = now + 200;   /* resume 200 ms after landing */
            s_land_ramp_until = now + 500;   /* no power jerk */
            if (hard) {
                os_alert(&s_os, ALERT_OBSTACLE, "HARD LANDING", now);
                if (!snd_play("landing_hard")) car_sfx_bad();
                notif_push(NOTIF_MPU, "HARD LANDING", now);
                rumble_preset(RUMBLE_LAND_H);
            } else {
                if (!snd_play("landing_smooth")) car_sfx_score();
                rumble_preset(RUMBLE_LAND_S);
            }
            {   /* PART 9.5: airborne announce with measured airtime */
                imu_adv_snap_t asnap;
                imu_adv_snapshot(&asnap);
                if (asnap.last_airtime_ms) {
                    char msg[80];
                    snprintf(msg, sizeof(msg),
                             "Attention - car went airborne for %lu milliseconds, landed safely.",
                             (unsigned long)asnap.last_airtime_ms);
                    alexa_announce(msg, false);
                }
            }
            ESP_LOGI(TAG, "landing %s %.2fg (score logged)",
                     hard ? "HARD" : "SMOOTH", (double)e.mag);
            break;
        }
        case IMU_EVT_CORNER_WARN:
            break;   /* soft speed-cap applied silently in gating */
        case IMU_EVT_CORNER_HARD:
            cockpit_toast("TIGHT CORNER - SLOW", now);
            /* alternating side buzz, sign of yaw picks the side */
            rumble_preset(e.mag > 0 ? RUMBLE_CORNER_R : RUMBLE_CORNER_L);
            break;
        case IMU_EVT_SLIP:
            cockpit_toast("SLIPPING", now);
            rumble_preset(RUMBLE_SLIP);
            ESP_LOGW(TAG, "SLIP: measured <40%% expected - AUTO escape feeds faster");
            if (g.mode == MODE_AUTO && g.ast != AST_ESCAPE && g.ast != AST_STUCK) {
                g.ast = AST_ESCAPE; g.ast_t0 = now;
                g.esc_phase = 0; g.esc_t0 = now;
            }
            break;
        case IMU_EVT_STUCK:
            cockpit_toast("STUCK - ESCAPING", now);
            snd_play("stuck_alert");
            notif_push(NOTIF_MPU, "STUCK - escaping", now);
            alexa_announce("Attention - wheels are stuck, car is attempting to escape.", false);
            rumble_preset(RUMBLE_STUCK);
            ESP_LOGE(TAG, "STUCK: measured <10%% expected - AUTO escape now");
            if (g.mode == MODE_AUTO && g.ast != AST_STUCK) {
                g.ast = AST_ESCAPE; g.ast_t0 = now;
                g.esc_phase = 0; g.esc_t0 = now;
            }
            break;
        case IMU_EVT_TRACTION_OK:
            break;
        case IMU_EVT_DRIFT_ON:
            /* manual: alert only */
            if (g.mode != MODE_AUTO) {
                cockpit_toast("DRIFT", now);
                snd_play("drift_warn");
                rumble_preset(RUMBLE_DRIFT);
            }
            ESP_LOGW(TAG, "DRIFT on (%.1f dps)", (double)e.mag);
            break;
        case IMU_EVT_DRIFT_OFF:
            break;
        case IMU_EVT_STATIONARY:
            break;   /* silent drift-correct */
        default:
            break;
        }
    }
}

static void set_loop(wav_t *w, const uint8_t *s, const uint8_t *e, uint16_t vol)
{
    if (!s || !e || e <= s + 44) { w->d = NULL; w->len = 0; w->pos = 0; return; }
    w->d = s + 44;
    w->len = (uint32_t)(e - s) - 44;
    w->pos = 0;
    w->loop = 1;
    w->vol = vol;
}

static void audio_effect_update(uint32_t now)
{
    uint16_t freq = 0;
    uint32_t until = now;
    uint16_t amp = 5000;

    if (false && s_horn_on) {
        /* old tone horn disabled — custom horn sample used */
        freq = ((now / 130) & 1) ? 620 : 880;
        until = now + 200;
        amp = 6000;
    } else if (false && now < s_horn_until) {
        freq = 880;
        until = s_horn_until;
        amp = 6000;
    } else if (now < s_blip_until) {
        freq = s_blip_freq;
        until = s_blip_until;
        amp = 4500;
    } else if (now < s_sweep_until) {
        /* sweep: turbo whoosh / blowoff / ignition crank */
        uint32_t t = s_sweep_until - now;
        if (t > 250) {
            t = 250;
        }
        freq = (uint16_t)(s_sweep_f0 + (s_sweep_f1 - s_sweep_f0) * (250 - t) / 250);
        until = now + 40;
        amp = s_sweep_amp;
    } else if (s_siren_on && roof_light_state()->enabled &&
               os_context() == INPUT_CTX_DRIVE && !g.estop) {
        /* optional siren (guide 6.5): sounds only with roof light in Drive.
           Forced OFF on menu entry, e-stop and controller disconnect. */
        freq = (uint16_t)(((now / 350) & 1) ? 700 : 1050);
        until = now + 350;
        amp = 5500;
    } else if (g.mode == MODE_AUTO) {
        /* auto mode: different beeps per state */
        if (g.ast == AST_STUCK) {
            freq = (now % 400) < 200 ? 1000 : 0;
            until = now + 200;
            amp = 5000;
        } else if (g.ast == AST_REVERSE || g.ast == AST_ESCAPE) {
            freq = (now % 500) < 250 ? 1500 : 0;
            until = now + 250;
            amp = 3500;
        } else if (g.ast == AST_TURN_L || g.ast == AST_TURN_R) {
            freq = 1300;
            until = now + 100;
            amp = 3000;
        } else if (g.ast == AST_SLOW || g.ast == AST_PAUSE) {
            freq = (now % 800) < 200 ? 1000 : 0;
            until = now + 200;
            amp = 2500;
        } else if (g.ast == AST_SEARCH) {
            freq = (now % 300) < 150 ? 2000 : 0;
            until = now + 150;
            amp = 3000;
        }
    } else if (now < s_ry_tick_until) {
        freq = 1300; /* RX/RY tick - halki */
        until = now + 80;
        amp = 2200;
    } else if (g.reversing) {
        freq = (now % 500) < 250 ? 1500 : 0;
        until = now + 250;
        amp = 3500; /* halka beep */
    } else if ((g.sig_l || g.sig_r || g.hazard) && (now % 800) < 90) {
        freq = 1300; /* indicator tick */
        until = now + 80;
        amp = 3500;
    } else if (g.led_mode == 4) {
        freq = (now % 400) < 200 ? 1500 : 2500; /* siren two-tone */
        until = now + 200;
        amp = 6000;
    } else if (!s_us_beep_mute && g.mode != MODE_AUTO && s_us_front_ok) {
        /* obstacle beep - sirf jab front sensor ne real echo diya ho
           (floating pins noise => lagatar beep => wo band) */
        uint16_t d = min3(g.dist[0], g.dist[1], g.dist[2]);
        if (d < 50) {
            if (d < 10) {
                freq = 2000;
                until = now + 4000;
            } else {
                uint32_t iv_calc = (uint32_t)d * 8;
                uint16_t iv = (uint16_t)iv_calc;
                if (iv < 40) {
                    iv = 40;
                }
                // Protect against division by zero
                if (iv == 0) {
                    iv = 1;
                }
                freq = (now % iv) < iv / 2 ? 2000 : 0;
                until = now + iv / 2;
            }
        }
    }

    /* master silence: MUTE chip or E-STOP kills scheduled tones
       (bank voices like sys_estop still play — they bypass this) */
    if (g.snd_mute || g.estop) { freq = 0; until = now; amp = 0; }

    s_effect_freq = freq;
    s_effect_until = until;
    s_effect_amp = amp;
}

static void audio_render(int16_t *buf, size_t n)
{
    const float dt = 1.0f / 22050.0f;
    static float ph_x = 0.0f;
    static uint32_t lcg = 12345;
    uint32_t now = now_ms();
    bool eff = s_effect_freq != 0 && now < s_effect_until;
    int16_t spd = s_engine_spd;
    int16_t aspd = spd < 0 ? (int16_t)-spd : spd;

    /* engine wav: pitch = RPM, volume = throttle. Reverse = silent (sirf beep).
       Auto mode = hover, loud + deep pitch. Idle base 300 (acchi sunai de) */
    float espeed;
    uint16_t evol;
    /* idle mein band, throttle dene par chalega */
    if (aspd < 5) {
        evol = 0;
        espeed = 1.0f;
    } else {
        if (g.mode == MODE_AUTO) {
            espeed = 0.95f + (float)aspd * 0.012f;
            evol = 360 + aspd * 5;
        } else {
            espeed = 1.0f + (float)aspd * 0.012f;
            evol = 300 + aspd * 6;
        }
        if (evol > 950) evol = 950;
        /* BUG-fix 2026-09-17: SETTINGS > "ENGINE VOL" (s_set_engine_vol)
           pehle kahin apply nahi hoti thi - ab engine/hover loop volume
           usi setting se scale hoti hai (default 100 = no change). */
        evol = (uint16_t)((uint32_t)evol * (uint32_t)s_set_engine_vol / 100u);
    }
    /* auto mode = futuristic hover loop, manual = engine loop */
    wav_t *e = (g.mode == MODE_AUTO) ? &s_hov : &s_eng;
    e->vol = evol;

    /* custom horn sample: LB hold = loop, release = stop */
    {
        bool active;
        portENTER_CRITICAL(&s_wav_mux);
        active = s_horn_wav.d != NULL;
        portEXIT_CRITICAL(&s_wav_mux);
        if (s_horn_on && !g.snd_mute) {
            if (!active && s_boot_horn && s_boot_horn_len)
                wav_play(&s_horn_wav, s_boot_horn, s_boot_horn_len, 1, 1000);
        } else {
            if (active) wav_stop(&s_horn_wav);
        }
    }
    /* intro-test sample: Y hold = loop (11025Hz), release = stop */
    if (s_y_held) {
        bool active;
        portENTER_CRITICAL(&s_wav_mux);
        active = s_intro_wav.d != NULL;
        portEXIT_CRITICAL(&s_wav_mux);
        if (!active) {
            portENTER_CRITICAL(&s_wav_mux);
            s_intro_out = 0;
            portEXIT_CRITICAL(&s_wav_mux);
            if (s_boot_intro && s_boot_intro_len)
                wav_play(&s_intro_wav, s_boot_intro, s_boot_intro_len, 1, 1000);
        }
    } else {
        bool active;
        portENTER_CRITICAL(&s_wav_mux);
        active = s_intro_wav.d != NULL;
        portEXIT_CRITICAL(&s_wav_mux);
        if (active) {
            wav_stop(&s_intro_wav);
            portENTER_CRITICAL(&s_wav_mux);
            s_intro_out = 0;
            portEXIT_CRITICAL(&s_wav_mux);
        }
    }

    // Snapshot wav states under single critical (was 7 per sample -> ISR starve fix)
    wav_t loc_eng, loc_hov, loc_brk, loc_stp, loc_shot, loc_horn, loc_intro, loc_startup, loc_snd;
    uint32_t loc_startup_out, loc_intro_out, loc_snd_out;
    portENTER_CRITICAL(&s_wav_mux);
    loc_eng = s_eng; loc_hov = s_hov; loc_brk = s_brk; loc_stp = s_stp;
    loc_shot = s_shot; loc_horn = s_horn_wav; loc_intro = s_intro_wav; loc_startup = s_startup;
    loc_snd = s_snd;
    loc_startup_out = s_startup_out; loc_intro_out = s_intro_out; loc_snd_out = s_snd_out;
    // Use local engine wav (auto/manual) - need to handle e->vol update already done
    // We'll keep e as pointer to snapshot copy
    wav_t *loc_e = (g.mode == MODE_AUTO) ? &loc_hov : &loc_eng;
    // Ensure vol is current evol (already set via e->vol, so copy)
    loc_e->vol = evol;
    if (g.mode == MODE_AUTO) loc_eng.vol = loc_hov.vol; else loc_hov.vol = loc_eng.vol; // keep both in sync for writeback
    portEXIT_CRITICAL(&s_wav_mux);

    uint32_t loc_s_intro_out = loc_intro_out;
    uint32_t loc_s_startup_out = loc_startup_out;
    uint32_t loc_s_snd_out = loc_snd_out;

    for (size_t i = 0; i < n; i++) {
        int32_t s = 0;
        if (loc_e->d) {
            uint32_t nsp = loc_e->len / 2;
            uint32_t ip = loc_e->pos >> 16;
            if (ip >= nsp) {
                if (loc_e->loop) loc_e->pos = 0;
                else loc_e->d = NULL;
            } else {
                int16_t a = ((int16_t *)loc_e->d)[ip];
                int16_t b = (ip + 1 < nsp) ? ((int16_t *)loc_e->d)[ip + 1] : a;
                uint32_t fr = loc_e->pos & 0xFFFF;
                int32_t smp = a + (int32_t)(((int64_t)(b - a) * fr) >> 16);
                s += smp * loc_e->vol / 1000;
                loc_e->pos += (uint32_t)(espeed * 65536.0f);
            }
        }
        if (loc_brk.d) {
            uint32_t ip = loc_brk.pos >> 16;
            if (ip >= loc_brk.len / 2) loc_brk.d = NULL;
            else { s += ((int16_t *)loc_brk.d)[ip] * loc_brk.vol / 1000; loc_brk.pos += 65536; }
        }
        if (loc_stp.d) {
            uint32_t ip = loc_stp.pos >> 16;
            if (ip >= loc_stp.len / 2) loc_stp.d = NULL;
            else { s += ((int16_t *)loc_stp.d)[ip] * loc_stp.vol / 1000; loc_stp.pos += 65536; }
        }
        if (loc_shot.d) {
            uint32_t ip = loc_shot.pos >> 16;
            if (ip >= loc_shot.len / 2) {
                loc_shot.d = NULL;
                loc_shot.pos = 0;
            } else {
                s += ((int16_t *)loc_shot.d)[ip] * loc_shot.vol / 1000;
                loc_shot.pos += 65536;
            }
        }
        if (loc_horn.d) {
            uint32_t ip = loc_horn.pos >> 16;
            if (ip >= loc_horn.len / 2) {
                if (loc_horn.loop) { loc_horn.pos = 0; ip = 0; }
                else loc_horn.d = NULL;
            }
            if (loc_horn.d) { s += ((int16_t *)loc_horn.d)[ip] * loc_horn.vol * 2 / 1000; loc_horn.pos += 65536; }
        }
        if (loc_intro.d) {
            uint32_t out_total = (uint32_t)((uint64_t)loc_intro.len / 2 * 22050 / INTRO_TEST_SOUND_RATE);
            if (loc_s_intro_out >= out_total) {
                if (loc_intro.loop) loc_s_intro_out = 0;
                else { loc_intro.d = NULL; loc_s_intro_out = 0; }
            }
            if (loc_intro.d) {
                uint32_t nsp_i = loc_intro.len / 2;
                uint32_t ip = (uint32_t)((uint64_t)loc_s_intro_out * INTRO_TEST_SOUND_RATE / 22050);
                if (ip >= nsp_i) ip = nsp_i ? nsp_i - 1 : 0; /* clamp: rounding can push ip == nsp on last sample */
                int32_t smp = ((int16_t *)loc_intro.d)[ip];
                s += smp * loc_intro.vol * 2 / 1000;
                loc_s_intro_out++;
            }
        }
        if (loc_startup.d) {
            uint32_t out_total = (uint32_t)((uint64_t)loc_startup.len / 2 * 22050 / STARTUP_SOUND_RATE);
            if (loc_s_startup_out >= out_total) { loc_startup.d = NULL; loc_s_startup_out = 0; }
            else {
                uint32_t nsp_s = loc_startup.len / 2;
                uint32_t ip = (uint32_t)((uint64_t)loc_s_startup_out * STARTUP_SOUND_RATE / 22050);
                if (ip >= nsp_s) ip = nsp_s ? nsp_s - 1 : 0; /* clamp: rounding can push ip == nsp on last sample */
                int32_t smp = ((int16_t *)loc_startup.d)[ip];
                s += smp * loc_startup.vol * 2 / 1000;
                loc_s_startup_out++;
            }
        }
        bool startup_active = (loc_startup.d != NULL);
        if (loc_snd.d) {   /* PART 5 bank voice: 16kHz -> 22050 resample */
            uint32_t out_total = (uint32_t)((uint64_t)loc_snd.len / 2 * 22050 / SND_BANK_RATE);
            if (loc_s_snd_out >= out_total) { loc_snd.d = NULL; loc_s_snd_out = 0; }
            else {
                uint32_t nsp_b = loc_snd.len / 2;
                uint32_t ip = (uint32_t)((uint64_t)loc_s_snd_out * SND_BANK_RATE / 22050);
                if (ip >= nsp_b) ip = nsp_b ? nsp_b - 1 : 0;
                int32_t smp = ((int16_t *)loc_snd.d)[ip];
                s += smp * loc_snd.vol * 2 / 1000;
                loc_s_snd_out++;
            }
        }
        if (g.mist && !g.reversing && !startup_active) {
            lcg = lcg * 1664525u + 1013904223u;
            s += (int32_t)(((int16_t)(lcg >> 16)) * 0.15f);
        }
        if (eff) {
            ph_x += dt * (float)s_effect_freq;
            float sq = (ph_x - floorf(ph_x)) < 0.5f ? 1.0f : -1.0f;
            s += (int32_t)(sq * (float)s_effect_amp);
        }
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        buf[i] = (int16_t)s;
    }
    // Write back updated positions (single critical)
    portENTER_CRITICAL(&s_wav_mux);
    // Write back engine/hover depending on mode that was active during render
    if (g.mode == MODE_AUTO) {
        s_hov.pos = loc_hov.pos; s_hov.d = loc_hov.d; s_hov.vol = loc_hov.vol;
        s_eng.pos = loc_eng.pos; s_eng.d = loc_eng.d;
    } else {
        s_eng.pos = loc_eng.pos; s_eng.d = loc_eng.d; s_eng.vol = loc_eng.vol;
        s_hov.pos = loc_hov.pos; s_hov.d = loc_hov.d;
    }
    s_brk.pos = loc_brk.pos; s_brk.d = loc_brk.d;
    s_stp.pos = loc_stp.pos; s_stp.d = loc_stp.d;
    s_shot.pos = loc_shot.pos; s_shot.d = loc_shot.d;
    s_horn_wav.pos = loc_horn.pos; s_horn_wav.d = loc_horn.d;
    s_intro_wav.pos = loc_intro.pos; s_intro_wav.d = loc_intro.d;
    s_intro_out = loc_s_intro_out;
    s_snd.pos = loc_snd.pos; s_snd.d = loc_snd.d; s_snd.vol = loc_snd.vol;
    s_snd_out = loc_s_snd_out;
    s_startup.pos = loc_startup.pos; s_startup.d = loc_startup.d;
    s_startup_out = loc_s_startup_out;
    portEXIT_CRITICAL(&s_wav_mux);
}

static void audio_task(void *arg)
{
    int16_t buf[512];
    ESP_LOGI(TAG, "audio task: eng_len=%d eng_d=%p brk_d=%p", s_eng.len, s_eng.d, s_brk.d);
    uint32_t hb = 0;
    while (1) {
        /* pause ke dauraan I2S0 dusre writer ka hai - hum write NAHI karte
           (warna DMA queue full rahega aur writes fail honge) */
        if (s_audio_paused) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        audio_render(buf, 512);
        size_t wr = 0;
        esp_err_t werr = i2s_channel_write(s_i2s_tx, buf, sizeof(buf), &wr, portMAX_DELAY);
        if (werr != ESP_OK || wr != sizeof(buf)) {
            ESP_LOGE(TAG, "I2S0 write err=%s wr=%u/%u", esp_err_to_name(werr), (unsigned)wr, (unsigned)sizeof(buf));
        }
        if (++hb >= 215) {   /* ~5 s @ 512 samples / 22050 Hz */
            hb = 0;
            ESP_LOGI(TAG, "AUDIO hb: paused=%d wr_ok=%d heap=%u",
                     (int)s_audio_paused, (werr == ESP_OK), (unsigned)esp_get_free_heap_size());
        }
    }
}

/* I2S0 sample-rate change (16k <-> engine 22050). Channel disabled
   hona chahiye - caller disable/enable karega. */
esp_err_t car_i2s0_reconfig_rate(uint32_t hz)
{
    if (!s_i2s_tx) return ESP_ERR_INVALID_STATE;
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(hz);
    return i2s_channel_reconfig_std_clock(s_i2s_tx, &clk);
}

/* PCM (16k mono int16) -> I2S0. Caller pauses engine around this.
   Non-blocking-ish: bounded write with 300 ms timeout. */
void car_audio_play_tts_pcm(const int16_t *pcm, int samples)
{
    if (!pcm || samples <= 0 || !s_i2s_tx) return;
    size_t bytes = (size_t)samples * 2u;
    size_t written = 0;
    /* NO clock reconfig here - caller clock manage karta hai.
       Har-chunk reconfig = glitch/pop. */
    esp_err_t e = i2s_channel_write(s_i2s_tx, pcm, bytes, &written, pdMS_TO_TICKS(300));
    static uint32_t tts_logs = 0;
    if (tts_logs < 8) {
        tts_logs++;
        ESP_LOGI("tts", "write %uB -> %uB err=%s",
                 (unsigned)bytes, (unsigned)written, esp_err_to_name(e));
    }
}

/* Raw PCM write (clock caller manage karta hai). */
void car_audio_write_pcm(const int16_t *pcm, int samples)
{
    if (!pcm || samples <= 0 || !s_i2s_tx) return;
    size_t bytes = (size_t)samples * 2u;
    size_t written = 0;
    i2s_channel_write(s_i2s_tx, pcm, bytes, &written, pdMS_TO_TICKS(400));
}

static void audio_init(void)
{
    i2s_chan_config_t ac = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 8,
        .dma_frame_num = 512,
        .auto_clear = true,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&ac, &s_i2s_tx, NULL));

    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(22050),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)PIN_I2S_BCLK,
            .ws = (gpio_num_t)PIN_I2S_LRC,
            .dout = (gpio_num_t)PIN_I2S_DIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_tx, &sc));
    ESP_ERROR_CHECK(i2s_channel_enable(s_i2s_tx));

    /* embedded wav loops (44-byte RIFF header skip) */
    set_loop(&s_eng, _binary_engine_loop_wav_start, _binary_engine_loop_wav_end, 220);
    set_loop(&s_hov, _binary_hover_loop_wav_start, _binary_hover_loop_wav_end, 220);

    ESP_LOGI(TAG, "eng wav: len=%d nsmp=%d v0=%d v1=%d v2=%d",
             s_eng.len, s_eng.len / 2,
             ((int16_t *)(s_eng.d))[0], ((int16_t *)(s_eng.d))[1], ((int16_t *)(s_eng.d))[2]);

    /* prio 4: USB/xbox360 (prio 2/3) audio ko starve na kare - gaps/latency fix */
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "MAX98357A audio ready");
}

/* ------------------------------ input/keys ------------------------------ */

/* ------------------------- AUTO helpers (guide §13) ----------------------- */
static bool auto_can_enter(const xbox360_pad_t *pad)
{
    if (g.estop) return false;
    if (!car_us_front_ok()) return false;                 /* sensors valid  */
    if (g.cur_l || g.cur_r || g.tgt_l || g.tgt_r) return false; /* stationary */
    if (!pad) return false;
    if (stick_dz(pad->lx) || stick_dz(pad->ly) ||
        stick_dz(pad->rx) || stick_dz(pad->ry)) return false;  /* neutral    */
    if (pad->rt > 5 || pad->lt > 5) return false;
    return true;
}

static void auto_enter(uint32_t now)
{
    g.mode = MODE_AUTO;
    g.ast  = AST_CRUISE;
    g.ast_t0 = now;
    g.prog_f = 65535;
    g.prog_t = now;
    g.esc_phase = 0;
    g.hazard = false;
    g.headlight = true;
    /* AUTO restricted sub-context (guide 13): roof/ambient/mist/turbo/gripper
       activation is blocked inside AUTO - force the shared outputs quiet. */
    roof_light_set_enabled(false);
    g.led_mode = 0;
    g.mist = false;
    g.turbo = false;
    g.gripper_open = false;
    g.stuck_cnt = 0;
    g.search_dir = 0;
    g.dist_idx = 0;
    for (int i = 0; i < 3; i++) {
        g.dist_avg[i] = g.dist[i];
        for (int j = 0; j < 3; j++) g.dist_buf[i][j] = g.dist[i];
    }
    s_blip_until = now + 60;
    s_blip_freq = 1400;
    rumble_preset(RUMBLE_TICK);
}

static void auto_exit(uint32_t now)
{
    g.mode = MODE_MANUAL;
    g.ast  = AST_CRUISE;
    g.hazard = false;
    s_blip_until = now + 60;
    s_blip_freq = 1000;
    rumble_preset(RUMBLE_TICK);
    ESP_LOGI(TAG, "AUTO exited (manual)");
}

/* premium guide §4: clear e-stop only via START+BACK hold 2s + LT held +
   stationary. No single-button clear. */
static void estop_input_handle(const xbox360_pad_t *pad, uint32_t now)
{
    static uint32_t chord_t0 = 0;
    static bool need_fresh = true;   /* entry: require release + fresh press */
    static bool s_in = false;        /* latch: reset fresh-press on entry */
    bool lt_held    = pad && pad->lt > 200;
    bool chord      = input_pressed(B_START) && input_pressed(B_BACK);
    bool stationary = (g.cur_l == 0 && g.cur_r == 0 && g.tgt_l == 0 && g.tgt_r == 0);

    if (!s_in) { s_in = true; need_fresh = true; chord_t0 = 0; }
    if (!chord) need_fresh = false;   /* a release re-arms the chord */
    if (chord && lt_held && !need_fresh) {
        if (chord_t0 == 0) {
            chord_t0 = now;
        } else if (now - chord_t0 >= IE_ESTOP_CHORD_MS && stationary) {
            g.estop = false;
            ESP_LOGI(TAG, "ESTOP cleared (START+BACK 2s + LT + stationary)");
            rumble_preset(RUMBLE_TICK);
            chord_t0 = 0;
            s_in = false;
            input_consume_all();
        }
    } else {
        chord_t0 = 0;
    }
}
/* ============================== input/keys ================================ */

/* Neutral-release lock (premium guide �2.6/�12): after leaving OS/Game/E-stop
   the car stays drive-locked until BOTH sticks and BOTH triggers read neutral
   continuously for 250 ms. Motors are forced to 0 while locked. */
static bool     s_drive_locked  = false;
static uint32_t s_lock_nt0      = 0;     /* neutral-since timestamp        */
static bool     s_ctx_was_drive = true;  /* previous-loop context tracker  */
static uint32_t s_arm_nt0       = 0;     /* DRIVE_ARMING neutral-since     */
/* engine fade state (file scope: reconnect failsafe also resets it) */
static int16_t  s_last_es       = 0;
static uint32_t s_fade_t0       = 0;
static int16_t  s_fade_from     = -1;

bool car_drive_locked(void) { return s_drive_locked; }
static uint16_t s_last_dig = 0;      /* last nonzero raw button mask (debug) */
uint16_t car_last_dig(void) { return s_last_dig; }

static void input_handle(const xbox360_pad_t *pad, uint16_t dig, uint16_t tap, uint32_t now)
{
    /* premium guide §14: unified semantic event layer first. */
    input_events_update(dig, now);

    /* AZAM CAR OS 9.0 Part 1 — A/B meaning is 100% context-determined:
       CLOCK_STANDBY: A/B dead (no-op) | DRIVE: A=headlight/mist/pass,
       B=roof/panel/cycle | OS/MENU: A=confirm, B=back | GAME: game-defined,
       B=exit | MODAL: A=confirm, B=cancel | ESTOP: A/B dead (GUIDE only).
       9.0 Part 3 RULE 1: this switch is the SINGLE router — exactly one
       handler below ever sees a frame's events, so DRIVE logic can never
       observe a press made inside OS/GAME/BOOT/ESTOP (or future STANDBY). */
    /* arm the lock on every DRIVE re-entry (from OS/Game/Estop) + 9.0 RULE 2:
       ANY context change flushes in-flight button events (sticks already
       covered by the 250ms neutral-release lock below; this extends the
       same principle to A/B/X/Y/D-pad/LB/RB — a held-through button gets
       no tap/hold/double/edge in the new context, only a fresh press). */
    {
        input_context_t ctx_now = os_context();
        static input_context_t s_ctx_prev = INPUT_CTX_DRIVE;
        input_context_t ctx_prev = s_ctx_prev;
        if (ctx_now != ctx_prev) {
            input_flush_context_switch();
            ESP_LOGI(TAG, "ctx %d -> %d: button events flushed (9.0 RULE 2)",
                     (int)ctx_prev, (int)ctx_now);
            s_ctx_prev = ctx_now;
        }
        /* entering CLOCK_IDLE: lock everything visibly OFF — lights,
           signals, roof, mist, horn, siren. (Motors/engine die via the
           parked gate.) The user re-enables lights with A/B/Y/D-pad. */
        if (ctx_now == INPUT_CTX_STANDBY && ctx_prev != INPUT_CTX_STANDBY) {
            g.headlight = false;
            g.hazard = false;
            g.sig_l = g.sig_r = false;
            g.mist = false;
            roof_light_set_enabled(false);
            s_horn_on = false;
            s_siren_on = false;
            if (g.led_mode == 4) g.led_mode = 0;   /* siren wail can't enter idle */
        }
        bool in_drive = (ctx_now == INPUT_CTX_DRIVE);
        if (in_drive && !s_ctx_was_drive) {
            s_drive_locked = true;
            s_lock_nt0     = 0;
        }
        s_ctx_was_drive = in_drive;
    }

    static bool s_was_turbo;                /* RB turbo edge for blowoff */
    /* s_mist_tog used here is the FILE-scope one (line ~808) - local shadow
       was killing the A-hold toggle (manual_logic reads the file-scope copy). */
    static uint32_t last_now;
    uint32_t dt_ms = last_now ? now - last_now : LOOP_MS;
    last_now = now;
    if (dt_ms > 100) dt_ms = 100;           /* clamp */

    /* ==== PRIORITY 1 (guide §15): GUIDE tap = global e-stop toggle ========= */
    if (ev_tap(B_GUIDE)) {
        /* MPU rollover sticky lockout (1.md 4.3): GUIDE tap acks it when
           stationary - E-STOP-class, manual only. Alexa CANNOT clear
           (no alexa path ever calls imu_adv_ack_rollover). */
        if (!g.estop && imu_adv_rollover_latched()) {
            bool stationary = (g.cur_l == 0 && g.cur_r == 0 &&
                               g.tgt_l == 0 && g.tgt_r == 0);
            if (stationary) {
                imu_adv_ack_rollover();
                cockpit_toast("ROLLOVER ACK", now);
                ESP_LOGW(TAG, "rollover lockout manually acknowledged");
                rumble_preset(RUMBLE_TICK);
                input_consume_all();
                return;
            }
            /* else: moving -> fall through to normal E-STOP set (safety) */
        }
        if (g.estop) {
            /* E-stop hard-locked: GUIDE tap is the ONLY single-button exit.
               PART 16: clear needs stationary (neutral-release lock gates
               the motors after any clear, chord path checks stationary too). */
            bool stationary = (g.cur_l == 0 && g.cur_r == 0 &&
                               g.tgt_l == 0 && g.tgt_r == 0);
            if (!stationary) {
                cockpit_toast("STOP FIRST", now);
                car_sfx_blip(250);
                input_consume_all();
                return;
            }
            g.estop = false;
            g.braking = false;
            ESP_LOGI(TAG, "ESTOP cleared (GUIDE tap)");
            snd_play("ui_toggle_off");
            rumble_preset(RUMBLE_TICK);
            input_consume_all();
            return;
        }
        s_siren_on   = false;                /* siren OFF on e-stop (guide 6.5) */
        g.estop = true;
        g.mode = MODE_MANUAL;
        g.ast = AST_CRUISE;
        g.tgt_l = g.tgt_r = 0; g.cur_l = g.cur_r = 0;
        g.headlight = false;
        g.hazard = false;
        g.sig_l = g.sig_r = false;
        g.led_mode = 0;
        g.mist = false; s_mist_tog = false; g.braking = true;
        g.gripper_open = true;
        s_horn_on = false;   /* held-LB must not stick through E-STOP */
        roof_light_force_safe_off();
        servo_deg(LEDC_CHANNEL_0, 90);
        servo_deg(LEDC_CHANNEL_1, 90);
        s_horn_until = now + 200;
        s_os.roof_panel = false; s_os.quick_open = false; s_os.auto_prev = false;
        s_os.switch_open = false; s_os.start_home_at = 0;   /* drop overlays + deferred nav */
        snd_play("sys_estop");
        rumble_preset(RUMBLE_ESTOP);
        input_consume_all();
        return;
    }
    /* GUIDE hold = NO action (premium guide §4: no reboot on any hold) */

    /* ===== PRIORITY 2 (guide §15): controller disconnected = safe ======== */
    if (!pad || !pad->present) {
        s_horn_on = false;
        s_y_held  = false;
        g.turbo   = false;
        g.mist    = false;
        s_mist_tog = false;
        s_siren_on   = false;                /* siren OFF on disconnect */
        return;                      /* safe outputs forced in drive_output */
    }

    /* ==== PRIORITY 3 (guide §15): route by the unified context =========== */
    switch (os_context()) {
    case INPUT_CTX_ESTOP:
        estop_input_handle(pad, now);
        return;
    case INPUT_CTX_OS:
    case INPUT_CTX_BOOT:
    case INPUT_CTX_GAME:
    case INPUT_CTX_ARMING:    /* drive pre-stage: parked, B-cancel only */
        s_horn_on = false;
        s_y_held  = false;
        g.turbo   = false;
        g.mist    = false;
        s_mist_tog = false;
        s_siren_on   = false;                /* siren OFF on menu entry (guide 6.5) */
        if (tap || (pad && (stick_dz(pad->lx) || stick_dz(pad->ly) ||
                            stick_dz(pad->rx) || stick_dz(pad->ry) ||
                            pad->rt > 5 || pad->lt > 5))) {
            g.last_input_ms = now;
        }
        return;
    case INPUT_CTX_STANDBY:   /* clock/idle: parked, but keeps its own
                                 mist latch (A-hold). Entry reset is once
                                 in the context-change block above. */
        s_horn_on = false;
        s_y_held  = false;
        g.turbo   = false;
        s_mist_tog = false;
        s_siren_on   = false;
        if (tap || (pad && (stick_dz(pad->lx) || stick_dz(pad->ly) ||
                            stick_dz(pad->rx) || stick_dz(pad->ry) ||
                            pad->rt > 5 || pad->lt > 5))) {
            g.last_input_ms = now;
        }
        return;
    default:
        break;
    }

    /* ================= DRIVE context: premium controls ===================== */

    /* Neutral-release lock evaluation (guide �2.6): 250 ms continuous neutral */
    if (s_drive_locked) {
        bool neutral = pad &&
            stick_dz(pad->lx) == 0 && stick_dz(pad->ly) == 0 &&
            stick_dz(pad->rx) == 0 && stick_dz(pad->ry) == 0 &&
            pad->rt < 5 && pad->lt < 5;
        if (!neutral) {
            s_lock_nt0 = 0;
        } else if (s_lock_nt0 == 0) {
            s_lock_nt0 = now;
        } else if (now - s_lock_nt0 >= 250) {
            s_drive_locked = false;
            s_lock_nt0     = 0;
            ESP_LOGI(TAG, "drive unlocked (controls neutral 250ms)");
        }
    }

    /* AUTO restricted sub-context (guide §13): only X exits AUTO here; LT and
       stick manual takeover live in auto_logic. Everything else blocked. */
    if (g.mode == MODE_AUTO) {
        if (ev_tap(B_X)) {
            auto_exit(now);
            s_os.auto_prev = false;
        }
        if (tap || (pad && (stick_dz(pad->lx) || stick_dz(pad->ly) ||
                            stick_dz(pad->rx) || stick_dz(pad->ry) ||
                            pad->rt > 5 || pad->lt > 5))) {
            g.last_input_ms = now;
        }
        return;
    }

    /* X: tap = drive-mode preview overlay, hold 1s = AUTO request (§5.2/§13) */
    /* PART 8 M3 (future foundation): X double-tap = 3s listen buffer */
    if (ev_double(B_X)) {
        if (mic_listen_start()) {
            snprintf(s_os.toast, sizeof(s_os.toast), "LISTENING 3s");
            s_os.toast_until = now + 2500;
            s_os.mic_vu_until = now + 3000;   /* PART 12: VU overlay */
            notif_push(NOTIF_SOUND, "voice listen 3s", now);
        }
        input_consume(B_X);
    }
    if (ev_tap(B_X)) {
        s_os.auto_prev = !s_os.auto_prev;
        s_blip_until = now + 60;
        s_blip_freq = s_os.auto_prev ? 1300 : 900;
    }
    if (ev_hold(B_X, IE_AUTO_HOLD_MS)) {
        if (auto_can_enter(pad)) {
            auto_enter(now);
            s_os.auto_prev = false;
            car_sfx_score();
        } else {
            s_os.auto_prev = true;          /* show requirements checklist */
            s_blip_until = now + 200;
            s_blip_freq = 250;
            rumble_preset(RUMBLE_ERROR);
        }
        input_consume(B_X);
    }

    /* A: tap = headlight, hold 700ms = mist toggle (§5.2) */
    if (ev_tap(B_A)) {
        g.headlight = !g.headlight;
        play_shot(_binary_a_click_wav_start, _binary_a_click_wav_end, 800);
    }
    /* PART 12: A double-tap = pass light (2x flash window; the two taps net
       zero headlight change, so no state compensation needed) */
    if (ev_double(B_A)) {
        s_pass_until = now + 600;
        snd_play("light_flash");
        snprintf(s_os.toast, sizeof(s_os.toast), "PASS");
        s_os.toast_until = now + 800;
        input_consume(B_A);
    }
    if (ev_hold(B_A, IE_STD_HOLD_MS)) {
        s_mist_tog = !s_mist_tog;
        input_consume(B_A);
        s_blip_until = now + 60;
        s_blip_freq = 900;
        snprintf(s_os.toast, sizeof(s_os.toast), "MIST %s", s_mist_tog ? "ON" : "OFF");
        s_os.toast_until = now + 1200;
    }
    g.mist = s_mist_tog;

    /* mist max run time (guide 11 Safety): cap the manual mist so a forgotten
       toggle cannot run the reservoir dry. 0 = unlimited. */
    {
        static uint32_t s_mist_on_t0 = 0;
        if (g.mist && s_mist_on_t0 == 0) s_mist_on_t0 = now;
        if (!g.mist) s_mist_on_t0 = 0;
        uint8_t mmax = s_set_mist_max;
        if (g.mist && mmax && now - s_mist_on_t0 >= (uint32_t)mmax * 10000u) {
            g.mist = false;
            s_mist_tog = false;
            s_blip_until = now + 60;
            s_blip_freq = 300;               /* low blip = auto cut-off */
        }
    }

    /* B: tap = roof toggle (default POLICE), double-tap = quick-cycle modes,
       hold 700ms = roof bottom sheet (PART 7). LB+B hold 1s = siren toggle. */
    {
        /* siren chord: LB+B together 1s (checked first so the roof-panel
           hold is suppressed while the chord is in progress) */
        static uint32_t s_sir_t0 = 0;
        if (input_pressed(B_LB) && input_pressed(B_B)) {
            if (s_sir_t0 == 0) s_sir_t0 = now;
            else if (now - s_sir_t0 >= 1000) {
                s_siren_on = !s_siren_on;
                s_sir_t0   = 0;
                snprintf(s_os.toast, sizeof(s_os.toast), "SIREN %s",
                         s_siren_on ? "ON" : "OFF");
                s_os.toast_until = now + 1200;
                rumble_preset(RUMBLE_TICK);
                input_consume(B_B);
                input_consume(B_LB);
            }
        } else {
            s_sir_t0 = 0;
        }
    }
    if (ev_hold(B_B, IE_STD_HOLD_MS) && !input_pressed(B_LB)) {
        s_os.roof_prev_mode = roof_light_state()->mode;
        s_os.roof_prev_br   = roof_light_state()->brightness;
        s_os.roof_prev_col  = roof_light_state()->color_idx;
        s_os.roof_panel     = true;
        s_os.roof_row       = 0;
        car_sfx_click();
        snd_play("ui_open_sheet");
        input_consume(B_B);
        return;                              /* panel owns buttons now */
    }
    if (ev_double(B_B)) {
        /* PART 7: quick-cycle modes without opening panel (replaces burst) */
        roof_light_cycle_mode(1);
        if (roof_light_state()->brightness == 0) roof_light_set_brightness(70);
        roof_light_set_enabled(true);
        snprintf(s_os.toast, sizeof(s_os.toast), "ROOF %s",
                 roof_light_label());
        s_os.toast_until = now + 1200;
        snd_play("roof_mode_cycle");
        rumble_preset(RUMBLE_TICK);
        input_consume(B_B);
    } else if (ev_tap(B_B) && !input_pressed(B_LB)) {
        bool on = !roof_light_state()->enabled;
        if (on) {
            if (roof_light_state()->mode == ROOF_LIGHT_OFF)
                roof_light_set_mode(ROOF_LIGHT_POLICE);  /* PART 7 default */
            if (roof_light_state()->brightness == 0) roof_light_set_brightness(70);
            roof_light_set_enabled(true);
            snd_play("roof_police_on");
        } else {
            roof_light_set_enabled(false);
            car_sfx_click();
        }
        snprintf(s_os.toast, sizeof(s_os.toast), "ROOF LIGHTS %s - %s",
                 on ? "ON" : "OFF", roof_light_label());
        s_os.toast_until = now + 1200;
        rumble_preset(RUMBLE_TICK);
    }
    if (ev_tap(B_Y)) {
        static uint8_t s_rgb = 0;
        const uint8_t cols[6][3] = {{255,0,0},{0,255,0},{0,0,255},{255,255,0},{255,0,255},{255,255,255}};
        if (g.led_mode == 0) { g.led_mode = 1; s_rgb = 0; }
        else if (g.led_mode == 1) { s_rgb = (uint8_t)((s_rgb + 1) % 6); if (s_rgb == 0) g.led_mode = (uint8_t)((g.led_mode + 1) % 5); }
        else { g.led_mode = 1; s_rgb = 0; }
        extern uint8_t g_static_r, g_static_g, g_static_b;
        g_static_r = cols[s_rgb][0]; g_static_g = cols[s_rgb][1]; g_static_b = cols[s_rgb][2];
        play_shot(_binary_yclick_wav_start, _binary_yclick_wav_end, 800);
        rumble_preset(RUMBLE_TICK);
    }
    if (ev_hold(B_Y, IE_STD_HOLD_MS)) {
        /* ambient color picker overlay (premium guide 5.2): opens the parked
           picker; selection/apply/cancel handled by the OS router. */
        s_os.picker_open     = true;
        s_os.picker_sel      = 0;
        s_os.picker_prev_mode = g.led_mode;
        s_os.picker_prev_r   = g_static_r;
        s_os.picker_prev_g   = g_static_g;
        s_os.picker_prev_b   = g_static_b;
        input_consume(B_Y);
        car_sfx_blip(1200);
    }
    /* PART 12: Y double-tap = under-car strip solo toggle (preset cycles
       twice as a side effect - accepted) */
    if (ev_double(B_Y)) {
        s_under_solo = !s_under_solo;
        snprintf(s_os.toast, sizeof(s_os.toast), "UNDER %s",
                 s_under_solo ? "SOLO" : "AUTO");
        s_os.toast_until = now + 1200;
        snd_play("light_flash");
        rumble_preset(RUMBLE_TICK);
        input_consume(B_Y);
    }

    /* LS/RS click */
    if (tap & B_LS) {
        if (g.mode == MODE_MANUAL)      g.mode = MODE_CRAWL;
        else if (g.mode == MODE_CRAWL)  g.mode = MODE_MANUAL;
        s_blip_until = now + 60;
        s_blip_freq = 1100;
    }
    if (ev_tap(B_RS)) g.cannon_reset = true;   /* tap only: RS-hold is CAL */

    /* D-pad */
    if (tap & B_DUP)    { g.hazard = !g.hazard; s_blip_until = now + 60; s_blip_freq = 1200; }
    /* D-pad UP hold 1 s = Quick settings sheet (BACK alternative).
       The press edge already toggled hazard above — undo it so a hold
       never flips hazard as a side effect. */
    if (ev_hold(B_DUP, 1000)) {
        g.hazard = !g.hazard;
        if (!s_os.quick_open) {
            s_os.quick_open = true;
            s_os.quick_row = 0;
            snd_play("ui_open_sheet");
        }
        input_consume(B_DUP);
    }
    if (tap & B_DLEFT)  { g.sig_l = !g.sig_l;  g.sig_r = false; }
    if (tap & B_DRIGHT) { g.sig_r = !g.sig_r;  g.sig_l = false; }
    /* D-pad Down = rear light toggle (ON/OFF) */
    if (tap & B_DDOWN) {
        g.rear_light_on = !g.rear_light_on;
        s_blip_until = now + 60;
        s_blip_freq = g.rear_light_on ? 1200 : 600;
    }
    /* PART 12: D-Down hold = ALL LIGHTS master off/on.
       The press edge above already toggled rear_light — exclude it from
       the decision, else "all were off + hold" computes ON then turns OFF. */
    if (ev_hold(B_DDOWN, IE_STD_HOLD_MS)) {
        bool any_on = g.headlight || g.hazard || g.sig_l || g.sig_r ||
                      roof_light_state()->enabled || g.led_mode != 0;
        if (any_on) {
            g.headlight = false; g.hazard = false;
            g.sig_l = g.sig_r = false;
            roof_light_force_safe_off();
            g.led_mode = 0;
            g.rear_light_on = false;
            snprintf(s_os.toast, sizeof(s_os.toast), "ALL LIGHTS OFF");
        } else {
            g.headlight = true;
            g.rear_light_on = true;
            snprintf(s_os.toast, sizeof(s_os.toast), "ALL LIGHTS ON");
        }
        s_os.toast_until = now + 1200;
        snd_play("light_flash");
        rumble_preset(RUMBLE_TICK);
        input_consume(B_DDOWN);
    }

    /* PART 12 paddle shifters (tap) + horn/turbo (hold >=250ms).
       Tap/hold are mutually exclusive in the input engine: ev_hold arms
       suppression, ev_held sustains - zero crossfire by construction. */
    ev_hold(B_LB, 250);
    ev_hold(B_RB, 250);
    s_horn_on = ev_held(B_LB, 250) && !input_pressed(B_RB);

    /* RB hold = turbo, 3s max, phir 5s cooldown (hold-gated: taps shift) */
    {
        bool rb = ev_held(B_RB, 250) && !input_pressed(B_LB);
        if (rb && !g.cooldown && g.mode != MODE_AUTO) {
            if (!g.turbo) {
                s_sweep_until = now + 250;
                s_sweep_f0 = 400;
                s_sweep_f1 = 1500;
                s_sweep_amp = 4500;
                rumble_preset(RUMBLE_TURBO);
            }
            g.turbo = true;
            g.turbo_ms += dt_ms;
            if (g.turbo_ms >= 3000) {
                g.cooldown = true;
                g.turbo = false;
                g.turbo_ms = 0;
                g.cooldown_until = now + 5000;
            }
        } else {
            g.turbo = false;
            if (!g.cooldown) g.turbo_ms = 0;
        }
        if (!g.turbo && s_was_turbo) {
            s_sweep_until = now + 250;
            s_sweep_f0 = 1400;
            s_sweep_f1 = 350;
            s_sweep_amp = 4000;
        }
        s_was_turbo = g.turbo;
        if (g.cooldown && (int32_t)(now - g.cooldown_until) >= 0) g.cooldown = false;
    }

    /* PART 12 LB+RB chord = dashcam snapshot (cockpit frame -> SPIFFS).
       Consumes both keys so paddle taps never crossfire on release.
       Refused while moving: the save stalls the drive loop ~400 ms. */
    {
        static bool s_cam_armed = true;
        if (input_pressed(B_LB) && input_pressed(B_RB)) {
            if (s_cam_armed) {
                s_cam_armed = false;
                if (g.cur_l || g.cur_r || g.tgt_l || g.tgt_r) {
                    cockpit_toast("STOP FIRST", now);
                    car_sfx_blip(250);
                } else {
                    dashcam_save();
                }
                input_consume(B_LB);
                input_consume(B_RB);
            }
        } else {
            s_cam_armed = true;
        }
    }

    /* PART 12 paddle shifters: LB tap = GEAR DOWN, RB tap = GEAR UP.
       Center G-popup 1s + shift sound + rumble tick; at limits MIN/MAX +
       gear_limit, no rumble. Chord peers held = aborted chord, stay quiet. */
    if (ev_tap(B_LB) && !input_pressed(B_RB) && !input_pressed(B_B)) {
        if (g.gear == 0) {
            snprintf(s_os.gear_pop, sizeof(s_os.gear_pop), "MIN");
            s_os.gear_flash = 1;
            s_os.gear_flash_until = now + 1000;
            snd_play("gear_limit");
        } else {
            g.gear--;
            snprintf(s_os.gear_pop, sizeof(s_os.gear_pop), "G%u", (unsigned)(g.gear + 1));
            s_os.gear_flash = 1;
            s_os.gear_flash_until = now + 1000;
            snd_play("gear_shift_down");
            rumble_preset(RUMBLE_GEAR);
        }
        input_consume(B_LB);
    }
    if (ev_tap(B_RB) && !input_pressed(B_LB)) {
        if (g.gear >= CAR_GEAR_COUNT - 1) {
            snprintf(s_os.gear_pop, sizeof(s_os.gear_pop), "MAX");
            s_os.gear_flash = 1;
            s_os.gear_flash_until = now + 1000;
            snd_play("gear_limit");
        } else {
            g.gear++;
            snprintf(s_os.gear_pop, sizeof(s_os.gear_pop), "G%u", (unsigned)(g.gear + 1));
            s_os.gear_flash = 1;
            s_os.gear_flash_until = now + 1000;
            snd_play("gear_shift_up");
            rumble_preset(RUMBLE_GEAR);
        }
        input_consume(B_RB);
    }

    /* START/BACK live in car_os (OS_DRIVE_MAIN): PART 12 map -
       START tap = HOME, START hold = QCC, BACK tap = view cycle,
       BACK hold = Night HUD / 1.5s radar. */

    /* input activity tracker */
    if (tap || (pad && (stick_dz(pad->lx) || stick_dz(pad->ly) ||
                        stick_dz(pad->rx) || stick_dz(pad->ry) ||
                        pad->rt > 5 || pad->lt > 5))) {
        g.last_input_ms = now;
    }
}

/* ------------------------------- drive logic ---------------------------- */

static void ramp_to(int16_t *cur, int16_t tgt)
{
    int16_t lim = (g.mode == MODE_AUTO) ? 5 : 20; /* auto dheere, manual tez */
    int16_t d = (int16_t)(tgt - *cur);
    if (d > lim) d = lim;
    if (d < -lim) d = -lim;
    *cur = (int16_t)(*cur + d);
}

/* ------------------------- sensor moving average ------------------------- */
static void dist_update_avg(void)
{
    for (int i = 0; i < 3; i++) {
        g.dist_buf[i][g.dist_idx] = g.dist[i];
    }
    // Rear (index 3) is not a real sensor - keep it 65535, do not average
    g.dist_buf[3][g.dist_idx] = 65535;
    g.dist_idx = (g.dist_idx + 1) % 3;
    for (int i = 0; i < 3; i++) {
        // Median-filtered average that ignores 65535 (no-echo) values
        uint32_t sum = 0; uint8_t cnt = 0;
        for (int j = 0; j < 3; j++) if (g.dist_buf[i][j] != 65535) { sum += g.dist_buf[i][j]; cnt++; }
        if (cnt == 0) g.dist_avg[i] = 65535;
        else g.dist_avg[i] = (uint16_t)(sum / cnt);
    }
    g.dist_avg[3] = 65535;
}

/* --------------------------- auto state name ----------------------------- */
static const char *ast_name(uint8_t ast)
{
    switch (ast) {
    case AST_CRUISE:  return "CRUISE";
    case AST_SLOW:    return "SLOW";
    case AST_TURN_L:  return "TURN_L";
    case AST_TURN_R:  return "TURN_R";
    case AST_REVERSE: return "REVERSE";
    case AST_ESCAPE:  return "ESCAPE";
    case AST_STUCK:   return "STUCK";
    case AST_PAUSE:   return "PAUSE";
    case AST_SEARCH:  return "SEARCH";
    default:          return "?";
    }
}

/* proportional wall steer: distance → steering correction */
static int16_t wall_steer(uint16_t side_dist)
{
    if (side_dist < 15)  return 50;   /* very close: strong push */
    if (side_dist < 30)  return 30;   /* close: medium correction */
    if (side_dist < 60)  return 15;   /* moderate: gentle nudge */
    return 0;                          /* far: no correction */
}

/* check if turn direction has enough clearance (>=30cm) */
static bool turn_clear(uint8_t ast, uint16_t l, uint16_t r)
{
    if (ast == AST_TURN_L) return l >= 30;
    if (ast == AST_TURN_R) return r >= 30;
    return false;
}

/* find best turn direction based on side distances */
static uint8_t best_turn(uint16_t l, uint16_t r)
{
    if (l >= r && l >= 30) return AST_TURN_L;
    if (r > l && r >= 30)  return AST_TURN_R;
    if (l >= r) return AST_TURN_L;  /* fallback: pick more open side */
    return AST_TURN_R;
}

/* BUG-fix 2026-09-17: AST_SEARCH ka "3s me give up" branch kabhi reach nahi
   hota tha — us hi case me g.ast_t0 har 500ms reset hota hai, isliye el
   (now - ast_t0) 3000 tak pahunch hi nahi sakta tha. Ab alag timer. */
static uint32_t s_search_t0;

/* ----------------------------- auto logic --------------------------------- */
static void auto_logic(const xbox360_pad_t *pad, uint32_t now)
{
    /* use averaged distances for decisions (smooth out noise) */
    uint16_t f = g.dist_avg[1], l = g.dist_avg[0], r = g.dist_avg[2], b = g.dist_avg[3];
    int16_t spd = 0, st = 0;
    uint32_t el = now - g.ast_t0;

    switch (g.ast) {

    /* ======================== CRUISE: full speed forward ======================== */
    case AST_CRUISE:
        /* socho 600ms: kaha jagah zyada hai */
        if (el < 600) {
            spd = 0; st = 0;
            break; /* stand still, sensors scan */
        }
        /* distance-proportional speed (max 12, cap 20%) */
        if (f > 200)      spd = 12;
        else if (f > 120) spd = 10;
        else if (f > 80)  spd = 8;
        else if (f > 40)  spd = 6;
        else              spd = 5;

        /* proportional wall following */
        if (l < 60) st += wall_steer(l);   /* wall on left → steer right */
        if (r < 60) st -= wall_steer(r);   /* wall on right → steer left */

        /* critical: front too close → reverse */
        if (f < 35) {
            g.ast = AST_REVERSE;
            g.ast_t0 = now;
            g.prog_f = 65535;
            rumble(255, 255, 350); /* critical: strong pulse */
        }
        /* turn: Q-learning + safety mask */
        else if (f < 55) {
            uint8_t state = advStateOf(f,l,r);
            uint8_t qact = advPickAction(state,f,l,r);
            uint8_t turn;
            if(qact==ADV_PIVOT_L || qact==ADV_SOFT_L) turn=AST_TURN_L;
            else if(qact==ADV_PIVOT_R || qact==ADV_SOFT_R) turn=AST_TURN_R;
            else turn=best_turn(l,r);
            if (turn_clear(turn, l, r)) {
                g.ast = turn;
            } else if (turn_clear(turn == AST_TURN_L ? AST_TURN_R : AST_TURN_L, l, r)) {
                g.ast = (turn == AST_TURN_L) ? AST_TURN_R : AST_TURN_L;
            } else {
                g.ast = AST_REVERSE;
                rumble(255, 255, 300);
            }
            if (g.ast == AST_TURN_L || g.ast == AST_TURN_R) rumble(0, 180, 200);
            g.ast_t0 = now;
            g.prog_f = 65535;
            // Q update with small reward
            static uint8_t lastSt=0, lastAct=0; static uint32_t lastMs=0;
            if(lastMs){
                float rew = (f>30?0.5f:-0.5f) -0.25f;
                advUpdateQ(lastSt, lastAct, rew, state);
            }
            lastSt=state; lastAct=qact; lastMs=now;
        }
        /* slow: obstacle approaching */
        else if (f < 120) {
            g.ast = AST_SLOW;
            g.ast_t0 = now;
            g.prog_f = 65535;
            rumble(60, 60, 120); /* approaching */
        }

        /* stuck detection: no progress for 2s */
        if (f < g.prog_f - 5) {
            g.prog_f = f;
            g.prog_t = now;
        }
        if (f > g.prog_f + 10) {
            g.prog_f = f;
            g.prog_t = now;
        }
        if (f < 80 && now - g.prog_t > 2000) {
            g.ast = AST_ESCAPE;
            g.ast_t0 = now;
            g.esc_phase = 0;
            g.esc_turn = 0;
            rumble(200, 200, 400);
        }
        break;

    /* ======================== SLOW: decelerated approach ======================== */
    case AST_SLOW:
        /* proportional speed based on distance (max 15) */
        if (f > 80)      spd = 15;
        else if (f > 60) spd = 12;
        else if (f > 40) spd = 10;
        else             spd = 8;

        /* steer toward more open side */
        if (l < r) {
            st = -20;
        } else {
            st = 20;
        }

        if (f < 35) {
            g.ast = AST_REVERSE;
            g.ast_t0 = now;
            g.prog_f = 65535;
            rumble(255, 255, 350);
        } else if (f < 55) {
            uint8_t state = advStateOf(f,l,r);
            uint8_t qact = advPickAction(state,f,l,r);
            uint8_t turn;
            if(qact==ADV_PIVOT_L || qact==ADV_SOFT_L) turn=AST_TURN_L;
            else if(qact==ADV_PIVOT_R || qact==ADV_SOFT_R) turn=AST_TURN_R;
            else turn=best_turn(l,r);
            if (turn_clear(turn, l, r)) g.ast = turn;
            else if (turn_clear(turn == AST_TURN_L ? AST_TURN_R : AST_TURN_L, l, r)) g.ast = (turn == AST_TURN_L) ? AST_TURN_R : AST_TURN_L;
            else { g.ast = AST_REVERSE; rumble(255,255,300); }
            if (g.ast == AST_TURN_L || g.ast == AST_TURN_R) rumble(0,180,200);
            g.ast_t0 = now; g.prog_f = 65535;
        } else if (f > 100) {
            g.ast = AST_CRUISE;
            g.ast_t0 = now;
            g.prog_f = 65535;
        }

        /* stuck detection in SLOW too */
        if (f < g.prog_f - 5) {
            g.prog_f = f;
            g.prog_t = now;
        }
        if (f < 80 && now - g.prog_t > 2000) {
            g.ast = AST_ESCAPE;
            g.ast_t0 = now;
            g.esc_phase = 0;
            g.esc_turn = 0;
            rumble(200, 200, 400);
        }
        break;

    /* ======================== TURN: pivot in place ======================== */
    case AST_TURN_L:
    case AST_TURN_R:
        spd = 15;
        st = (g.ast == AST_TURN_L) ? -100 : 100;
        /* light continuous rumble while turning */
        if (el < 100) rumble(s_rumble_l, s_rumble_r, 150); /* keep pulse */
        if (el == 0 || el < 40) {
            /* trigger once at entry */
            if (g.ast == AST_TURN_L) rumble(80, 0, 180);
            else rumble(0, 80, 180);
        }
        if (el > 500) {
            g.ast = AST_CRUISE;
            g.ast_t0 = now;
            g.prog_f = 65535;
            g.prog_t = now;
        }
        break;

    /* ======================== REVERSE: back up ======================== */
    case AST_REVERSE:
        /* rear blocked → don't reverse, turn instead */
        if (b != 65535 && b < 20) {
            uint8_t turn = best_turn(l, r);
            if (turn_clear(turn, l, r)) g.ast = turn; else g.ast = (turn==AST_TURN_L?AST_TURN_R:AST_TURN_L);
            g.ast_t0 = now; rumble(180,180,250); break;
        }
        spd = -10;
        if (el < 40) rumble(120, 0, 350); /* low rumble on reverse start */
        else if ((el % 800) < 40) rumble(60, 0, 120); /* periodic */
        if (el > 900) {
            /* turn toward more open side with clearance check */
            uint8_t turn = best_turn(l, r);
            if (turn_clear(turn, l, r)) {
                g.ast = turn;
            } else if (turn_clear(turn == AST_TURN_L ? AST_TURN_R : AST_TURN_L, l, r)) {
                g.ast = (turn == AST_TURN_L) ? AST_TURN_R : AST_TURN_L;
            } else {
                g.ast = AST_REVERSE;  /* both blocked: keep reversing */
                g.ast_t0 = now;
            }
            g.prog_f = 65535;
        }
        break;

    /* ======================== ESCAPE: multi-phase stuck recovery ============ */
    case AST_ESCAPE:
        if (el < 40) rumble(200, 200, 300);
        if (g.esc_phase == 0) {
            /* phase 0: reverse — check rear */
            if (b != 65535 && b < 15) {
                g.esc_phase = 1; g.esc_t0 = now; rumble(150,150,200);
            }
            spd = -10;
            if (el > 1200) {
                g.esc_phase = 1;
                g.esc_t0 = now;
            }
        } else {
            /* phase 1: forward + turn */
            spd = 10;
            st = g.esc_turn ? 100 : -100;
            if (el > 2200) {
                if (f < 25) {
                    if (g.esc_turn == 0) {
                        g.esc_turn = 1;  /* try right */
                        g.ast_t0 = now;
                        g.esc_phase = 0;
                    } else {
                        g.ast = AST_STUCK;
                        g.ast_t0 = now;
                        g.stuck_cnt++;
                        g.hazard = true;
                    }
                } else {
                    g.ast = AST_CRUISE;
                    g.ast_t0 = now;
                    g.prog_f = 65535;
                    g.prog_t = now;
                }
            }
        }
        break;

    /* ======================== STUCK: terminal (auto-retry after 3s) ======== */
    case AST_STUCK:
        spd = 0;
        if (el < 40) rumble(255, 0, 600); /* heavy low rumble */
        else if ((el % 1000) < 40) rumble(180, 180, 250); /* periodic */
        /* auto-retry after 3s (max 3 retries) */
        if (el > 3000 && g.stuck_cnt < 3) {
            g.ast = AST_REVERSE;
            g.ast_t0 = now;
            g.prog_f = 65535;
            g.hazard = false;
            rumble(150, 150, 300);
        } else if (el > 3000 && g.stuck_cnt >= 3) {
            /* permanent stuck: stay stopped, hazard on */
            g.hazard = true;
        }
        break;

    /* ======================== PAUSE: wait for dynamic obstacle ============= */
    case AST_PAUSE:
        spd = 0;
        /* obstacle cleared? resume cruise */
        if (f > 40) {
            g.ast = AST_CRUISE;
            g.ast_t0 = now;
            g.prog_f = 65535;
            g.prog_t = now;
        }
        /* timeout 2s: obstacle didn't move, turn away */
        else if (el > 2000) {
            uint8_t turn = best_turn(l, r);
            if (turn_clear(turn, l, r)) {
                g.ast = turn;
            } else if (turn_clear(turn == AST_TURN_L ? AST_TURN_R : AST_TURN_L, l, r)) {
                g.ast = (turn == AST_TURN_L) ? AST_TURN_R : AST_TURN_L;
            } else {
                g.ast = AST_SEARCH;
                s_search_t0 = now;      /* SEARCH give-up timer start */
            }
            g.ast_t0 = now;
        }
        break;

    /* ======================== SEARCH: 360 scan for path ==================== */
    case AST_SEARCH:
        /* slow rotation to find clear path */
        spd = 10;
        st = g.search_dir ? 60 : -60;

        /* check every 500ms if any sensor shows clear path */
        if (el > 500) {
            if (f > 80 || l > 80 || r > 80) {
                /* found clear path: face that direction */
                g.ast = AST_CRUISE;
                g.ast_t0 = now;
                g.prog_f = 65535;
                g.prog_t = now;
            }
            g.ast_t0 = now;  /* reset timer for next check */
        }
        /* gave up after 3s rotation (BUG-fix: alag timer, upar dekho) */
        if (now - s_search_t0 > 3000) {
            g.ast = AST_STUCK;
            g.ast_t0 = now;
            g.hazard = true;
        }
        break;
    }

    // cap 20% max
    if(spd>20) spd=20;
    if(spd<-20) spd=-20;
    if(st>20) st=20;
    if(st<-20) st=-20;
    // adaptive + NVS (bina encoder ke bhi)
    advAdaptiveTick(now, (uint32_t)f, f<15, g.ast==AST_STUCK);
    if(now % 60000 < 40) advNvsSave(now);

    g.reversing = (spd < 0);

    /* auto indicators */
    if (g.ast == AST_TURN_L) {
        g.sig_l = true;
        g.sig_r = false;
    } else if (g.ast == AST_TURN_R) {
        g.sig_r = true;
        g.sig_l = false;
    } else if (g.ast == AST_REVERSE || g.ast == AST_ESCAPE) {
        /* reverse/escape: both indicators flash */
        g.sig_l = (now / 400) & 1;
        g.sig_r = g.sig_l;
    } else if (g.ast == AST_SLOW || g.ast == AST_PAUSE) {
        /* slow/pause: both indicators on (warning) */
        g.sig_l = true;
        g.sig_r = true;
    } else {
        g.sig_l = false;
        g.sig_r = false;
    }

    /* spark cannon aim at closest obstacle */
    uint16_t m = min3(l, f, r);
    if (m != 65535) {
        if (m == l) {
            servo_deg(LEDC_CHANNEL_0, 135);
        } else if (m == r) {
            servo_deg(LEDC_CHANNEL_0, 45);
        } else {
            servo_deg(LEDC_CHANNEL_0, 90);
        }
        servo_deg(LEDC_CHANNEL_1, 90);
    }

    g.tgt_l = (int16_t)(spd + st);
    g.tgt_r = (int16_t)(spd - st);
    if (g.tgt_l > 100)  g.tgt_l = 100;
    if (g.tgt_r > 100)  g.tgt_r = 100;
    if (g.tgt_l < -100) g.tgt_l = -100;
    if (g.tgt_r < -100) g.tgt_r = -100;

    /* debug: auto state + distances */
    static uint32_t s_auto_dbg;
    if (now - s_auto_dbg > 1000) {
        s_auto_dbg = now;
        ESP_LOGI(TAG, "AUTO %s: L=%u F=%u R=%u spd=%d st=%d tgtL=%d tgtR=%d",
                 ast_name(g.ast), l, f, r, spd, st, g.tgt_l, g.tgt_r);
    }

    /* auto: mist on reverse/stuck */
    g.mist = g.reversing || g.ast == AST_STUCK || (g.turbo && g.mode == MODE_AUTO);
    /* engine sound ab drive_output() me actual wheel speed se set hota hai */
}

static void manual_logic(const xbox360_pad_t *pad, uint16_t dig, uint32_t now)
{
    int16_t ly = stick_dz(pad->ly);
    int16_t lx = stick_dz(pad->lx);
    /* halka smoothing (0.45): jerk hataye, latency na aaye */
    static float s_th_s, s_st_s, s_cap_s;
    float th_raw = (float)ly / STICK_MAX;
    if (th_raw > 1.0f) th_raw = 1.0f;
    if (th_raw < -1.0f) th_raw = -1.0f;
    float st_raw = (float)lx / STICK_MAX;
    if (st_raw > 1.0f) st_raw = 1.0f;
    if (st_raw < -1.0f) st_raw = -1.0f;
    s_th_s += (th_raw - s_th_s) * 0.45f;
    s_st_s += (st_raw - s_st_s) * 0.35f;
    float th = s_th_s;
    float st = s_st_s;
    st = st * st * st; /* steering curve: halka = soft */

    /* RT (Right Trigger): PROPORTIONAL to gear cap — halka dabao = low % of gear,
       poore dabao = full gear %. Gear1(5%) me 50% RT = 2.5% speed, 100% RT = 5% */
    float cap_raw = pad->rt / 255.0f;
    if (cap_raw < 0.05f) cap_raw = 0;            // tiny deadzone
    /* PART 13.4 trigger zones: 0-20% crawl precision (half gain), 20-70%
       linear, 70-100% aggressive (4/3 slope). Continuous at both knots. */
    if (cap_raw < 0.20f) cap_raw *= 0.5f;
    else if (cap_raw < 0.70f) cap_raw = 0.10f + (cap_raw - 0.20f);
    else cap_raw = 0.60f + (cap_raw - 0.70f) * 1.3333f;
    s_cap_s += (cap_raw - s_cap_s) * 0.35f;       // smooth
    float cap = s_cap_s;
    if (g.turbo) {
        cap = 1.0f; // turbo state (RB hold >=250ms) = 100% instant
    } else {
        /* Car OS: settings SPEED CAP + virtual gear cap — PROPORTIONAL */
        float capmax = car_speed_cap_pct() / 100.0f;
        cap = cap * capmax;  // proportional: RT% × gear%
        if (g.mode == MODE_CRAWL) cap *= 0.5f; // crawl = half again
    }

    /* LT progressive brake: 30-200 = speed cap, 200+ = hard brake */
    uint8_t lt = pad->lt;
    if (lt > 30 && lt <= 200) {
        cap *= (200.0f - lt) / 170.0f;
    }

    bool fwd = th > 0.02f;
    /* ultrasonic obstacle limiting (front) - sirf tab jab sensor ne
       real echo diya ho (floating pin noise ignore) */
    static uint8_t s_f_cnt = 0;
    uint16_t f = g.dist[1];
    /* BUG-fix 2026-09-17: SETTINGS > "OBST DIST" (car_get_setting(2)) manual
       mode me ignore ho raha tha (15/30 hardcoded). Ab setting hi lagi hai:
       hard-stop = obst/2 (min 5 cm), slow-down = obst cm. */
    uint16_t obst_cm = car_get_setting(2);
    if (obst_cm < 10) obst_cm = 10;
    uint16_t hard_cm = (uint16_t)(obst_cm / 2);
    if (hard_cm < 5) hard_cm = 5;
    if (fwd && s_us_front_ok && f != 65535 && f < hard_cm) {
        if (s_f_cnt < 3) {
            s_f_cnt++;
        } else {
            cap = 0.0f; /* emergency hard stop (3 baar consistently) */
        }
    } else {
        s_f_cnt = 0;
        if (fwd && s_us_front_ok && f != 65535 && f < obst_cm) {
            cap = cap < 0.30f ? cap : 0.30f; /* obstacle limit */
        }
    }

    float tL = th + st;
    float tR = th - st;
    if (tL > 1.0f) {
        tL = 1.0f;
    }
    if (tL < -1.0f) {
        tL = -1.0f;
    }
    if (tR > 1.0f) {
        tR = 1.0f;
    }
    if (tR < -1.0f) {
        tR = -1.0f;
    }

    g.tgt_l = (int16_t)(tL * cap * 100.0f);
    g.tgt_r = (int16_t)(tR * cap * 100.0f);

    /* LT hard brake (200+) */
    g.braking = lt > 200;
    if (g.braking) {
        g.tgt_l = 0;
        g.tgt_r = 0;
    }

    // gyro assist removed (MPU6050 not used)

    g.reversing = th < -0.02f && cap > 0.01f;

    /* mist: A hold = toggle ON rehta hai; reverse me auto */
    g.mist = s_mist_tog || g.reversing;

    /* cannon: right stick pan/tilt (RS click se recenter) + RX/RY par halki tick */
    static uint32_t s_cannon_snd_t;
    if (now - s_cannon_snd_t > 350 &&
        (stick_dz(pad->rx) > 5000 || stick_dz(pad->rx) < -5000 ||
         stick_dz(pad->ry) > 5000 || stick_dz(pad->ry) < -5000)) {
        s_cannon_snd_t = now;
        s_ry_tick_until = now + 80; /* tick RX/RY dono par */
    }
    if (g.cannon_reset) {
        servo_deg(LEDC_CHANNEL_0, 90);
        servo_deg(LEDC_CHANNEL_1, 90);
        g.cannon_reset = false;
    } else {
        /* right stick ko full rotation do: typical stick ~±16000 hota hai,
           32767 nahi - isliye amplify + clamp (0..180) + smoothing */
        static float s_rx_s, s_ry_s;
        s_rx_s += ((float)stick_dz(pad->rx) - s_rx_s) * 0.25f;
        s_ry_s += ((float)stick_dz(pad->ry) - s_ry_s) * 0.25f;
        int16_t a1 = (int16_t)(90 + (int32_t)s_rx_s * 110 / STICK_MAX);
        int16_t a2 = (int16_t)(90 + (int32_t)s_ry_s * 110 / STICK_MAX);
        if (a1 < 0) {
            a1 = 0;
        }
        if (a1 > 180) {
            a1 = 180;
        }
        if (a2 < 0) {
            a2 = 0;
        }
        if (a2 > 180) {
            a2 = 180;
        }
        servo_deg(LEDC_CHANNEL_0, (uint16_t)a1);
        servo_deg(LEDC_CHANNEL_1, (uint16_t)a2);
    }

    /* idle saver: 10s no USB data -> motors off, lights dim (true disconnect) */
    if (now - xbox360_last_data_ms() > 10000) {
        g.tgt_l = 0;
        g.tgt_r = 0;
        g.mist = false;
    }
    /* engine sound ab drive_output() me actual wheel speed (+RT stationary rev) se set hota hai */
}

static void drive_output(uint32_t now, const xbox360_pad_t *pad)
{
    // gyro removed

    /* Controller disconnected? IMMEDIATE motor stop (no ramp) for safety */
    if (!pad || !pad->present) {
        g.cur_l = 0; g.cur_r = 0; g.tgt_l = 0; g.tgt_r = 0;
        g.mist = false;
        g.braking = true;
        motors_apply();
        s_engine_spd = 0;
        return;
    }

    ramp_to(&g.cur_l, g.tgt_l);
    ramp_to(&g.cur_r, g.tgt_r);

    /* 200ms safety cutoff: if no controller signal, IMMEDIATE motor stop.
       Uses USB-level timestamp (xbox360_last_data_ms) so idle controller
       reports also keep the timeout from triggering — only true disconnect
       or controller-off stops the motors. */
    if (now - xbox360_last_data_ms() > 200) {
        g.tgt_l = 0;
        g.tgt_r = 0;
        g.cur_l = 0;
        g.cur_r = 0;
        g.mist = false;
    }

    if (g.estop) {
        g.cur_l = 0; g.cur_r = 0; g.tgt_l = 0; g.tgt_r = 0;
        g.mist = false; g.braking = true;
    }
    /* manual LT hard brake = real brake; auto mode me kabhi wheel lock nahi */
    g.brake_on = g.braking && g.mode == MODE_MANUAL;
    motors_apply();

    /* engine sound: ACTUAL wheel speed se (manual LY + auto dono me chalega),
       RT trigger = stationary rev (proportional, dead-zone applied).
       Menu/game (parked) ya estop me engine silent. */
    {
        /* absolute wheel speed — pick larger magnitude */
        int16_t a = g.cur_l, b = g.cur_r;
        if (a < 0) a = (int16_t)-a;
        if (b < 0) b = (int16_t)-b;
        int16_t spd_now = a > b ? a : b;

        /* RT trigger: dead-zone (ignore <20), then proportional 0..200 → 0..100 */
        int16_t rt_rev = 0;
        if (pad && !g.estop && pad->rt > 20) {
            uint32_t rt = pad->rt;
            if (rt > 200) rt = 200;          /* clamp to usable range */
            rt_rev = (int16_t)(rt * 100 / 200); /* proportional 0..100 */
        }

        int16_t es = spd_now > rt_rev ? spd_now : rt_rev;
        /* engine lifecycle: the loop runs forever in audio_task — only this
           speed word moves, so the loop is never restarted from here.
           IDLE/HOME/menus/games/ARMING (parked) = 0; DRIVE entry ramps an
           audible idle in over ~600 ms; leaving DRIVE fades out over
           400 ms. E-STOP cuts instantly (safety). */
        if (g.estop) {
            es = 0;
            s_fade_from = -1;
        } else if (!os_parked()) {
            s_fade_from = -1;
            uint32_t dt = now - s_os.state_enter_ms;
            int16_t idle = dt >= 600 ? 8 : (int16_t)(dt * 8 / 600);
            if (es < idle) es = idle;
        } else if (s_last_es > 0) {
            if (s_fade_from < 0) { s_fade_from = s_last_es; s_fade_t0 = now; }
            uint32_t dt = now - s_fade_t0;
            es = dt >= 400 ? 0 : (int16_t)(s_fade_from * (400 - dt) / 400);
        } else {
            es = 0;   /* parked at rest: RT rev must never reach the engine */
            s_fade_from = -1;
        }
        if (g.snd_mute) es = 0;   /* MUTE chip = master silence (engine too) */
        s_engine_spd = es;
        s_last_es = es;
    }

    /* spoiler / air brake: normal = 90 deg, progressive brake = 90 down to 0 deg */
    uint16_t ang = 90;
    uint8_t lt = pad ? pad->lt : 0;
    if (g.mode != MODE_AUTO) {
        if (g.reversing) {
            ang = 90;
        } else if (lt > 10) {
            // Proportional: LT 0..255 maps to angle 90..0
            ang = (uint16_t)(90 - (uint32_t)lt * 90 / 255);
        } else {
            int16_t spd = g.cur_l > g.cur_r ? g.cur_l : g.cur_r;
            if (spd < 0) spd = -spd;
            if (spd > 70) ang = 45;
            else if (spd > 30) ang = 70;
            else ang = 90;
        }
    } else {
        ang = 90;
    }
    servo_deg(LEDC_CHANNEL_3, ang);

    /* gripper: hold=180, tap=180hit 150ms then 90, default=90 */
    {
        uint8_t deg = 90;
        if (g.gripper_open) {
            deg = 180;  /* hold = stay 180 */
        } else if (g.grip_hit_ms && (now_ms() - g.grip_hit_ms < 150)) {
            deg = 180;  /* tap = 180 for 150ms */
        } else {
            g.grip_hit_ms = 0;
        }
        servo_deg(LEDC_CHANNEL_2, deg);
    }

    /* headlight relay + brake indicator.
       NOTE (PART 8): body-backlight relay on GPIO14 was removed in hardware;
       GPIO14 is now MIC WS (I2S1 output). Brake/reverse still shows on the
       rear-light relay + LED strips via g.braking. */
    uint32_t hl = 0;
    if (g.hazard) {
        hl = (now / 400) & 1 ? 100 : 0;
    } else {
        if (g.headlight) {
            hl = g.highbeam ? 100 : 60;
        }
        if (g.mode == MODE_AUTO) {
            hl = g.highbeam ? 100 : 60;
        }
        if (now - g.last_input_ms > 10000 && g.mode != MODE_AUTO) {
            hl = 20; /* idle dim */
        }
    }
    /* headlight relay active LOW (PART 12 pass-light forces ON in window) */
    if ((int32_t)(now - s_pass_until) < 0) hl = 100;
    gpio_set_level(PIN_HEADLIGHT, hl ? 0 : 1);

    /* rear light (under spoiler air brake): ON at boot, toggle with D-pad Down */
    gpio_set_level(PIN_REAR_LIGHT, g.rear_light_on ? 0 : 1);

    /* startup engine sound -> mist sync: two cranks 2-26% and 48-89% of 14.28s file (8000Hz->22050).
       Only sync when NOT parked (menu/game) — prevents relay clicking in menus. */
    if (!os_parked()) {
        bool sa=false; float prog=0;
        portENTER_CRITICAL(&s_wav_mux);
        if(s_startup.d && s_startup.len) {
            sa=true;
            uint32_t out_total = (uint32_t)((uint64_t)s_startup.len / 2 * 22050 / STARTUP_SOUND_RATE);
            if(out_total) prog=(float)s_startup_out/(float)out_total;
        }
        portEXIT_CRITICAL(&s_wav_mux);
        if(sa){
            bool crank = (prog>=0.02f && prog<=0.26f) || (prog>=0.48f && prog<=0.89f);
            g.mist = crank || g.mist;   /* user A-hold toggle survives; crank adds puffs */
        }
    }
    /* spark + mist MOSFETs */
    /* roof relay is owned by roof_light module (roof_light_update) */
    gpio_set_level(PIN_MIST, g.mist ? 0 : 1); // relay active LOW
    // buzzer horn: LB horn, reverse beep, indicator tick - volume 100%
    // master silence on MUTE or E-STOP (no confusion in safety state)
    if (g.snd_mute || g.estop) buzzer_tone(0,0);
    else if (s_horn_on || now < s_horn_until) buzzer_tone(880, 100);
    else if (g.reversing) buzzer_tone((now%500)<250?1500:0, 100);
    else if ((g.sig_l || g.sig_r || g.hazard) && (now%800)<90) buzzer_tone(1300, 100);
    else if (g.led_mode==4) buzzer_tone((now%400)<200?1500:2500, 100);
    else buzzer_tone(0,0);

    /* engine sound LY-input se direct manual_logic me set hota hai */

    /* brake screech: LT hard brake lagate hi */
    static bool s_was_braking;
    if (g.braking && !s_was_braking) {
        wav_play(&s_brk, _binary_brake_wav_start + 44,
                 (uint32_t)(_binary_brake_wav_end - _binary_brake_wav_start) - 44,
                 0, 850);
    } else if (!g.braking && s_was_braking) {
        int16_t spd_ = g.cur_l > g.cur_r ? g.cur_l : g.cur_r;
        if (spd_ < 0) {
            spd_ = (int16_t)-spd_;
        }
        if (spd_ > 30) {
            wav_play(&s_stp, _binary_stop_wav_start + 44,
                     (uint32_t)(_binary_stop_wav_end - _binary_stop_wav_start) - 44,
                     0, 700);
        }
    }
    s_was_braking = g.braking;

    static uint32_t dbg_t;
    if (now - dbg_t > 5000) {
        dbg_t = now;
        ESP_LOGI(TAG, "eng: tgt=%d,%d cur=%d,%d es=%d f=%d", g.tgt_l, g.tgt_r, g.cur_l, g.cur_r, (int)s_engine_spd, g.dist[1]);
    }
}

/* --------------------------------- HUD ---------------------------------- */

static void hud_update(void)
{
    char line[32];
    oled_clear();
    uint16_t l = g.dist[0], f = g.dist[1], r = g.dist[2];
    // Top bar: MODE (battery removed, GPIO1 now for backlight)
    const char *mstr = g.mode==MODE_AUTO?"AUTO": g.mode==MODE_CRAWL?"CRAWL":"MANUAL";
    snprintf(line,sizeof(line),"%s",mstr);
    oled_text(0, 0, line);
    // battery removed - show speed hint instead
    snprintf(line,sizeof(line),"%d%%", (abs(g.cur_l)+abs(g.cur_r))/2);
    oled_text(90, 0, line);
    // signal icons small
    if(g.hazard) oled_text(112, 0, "H");
    else if(g.sig_l) oled_text(112,0,"<");
    else if(g.sig_r) oled_text(112,0,">");

    // BIG FRONT distance centered
    if(f==65535) snprintf(line,sizeof(line),"---");
    else if(f<15) snprintf(line,sizeof(line),"STOP");
    else snprintf(line,sizeof(line),"%3u", f);
    // center big
    int len=strlen(line);
    int w=len*6*3;
    int x=(128-w)/2;
    if(f<15) oled_text_big(x, 14, line, 2); // STOP smaller to fit
    else oled_text_big(x, 14, line, 3);
    oled_text(48, 38, "cm");
    if(f<30 && f!=65535) oled_rect(0,12,128,32,false);

    // Bottom: L and R small + speed
    if(l==65535) snprintf(line,sizeof(line),"L --"); else snprintf(line,sizeof(line),"L%3u",l);
    oled_text(0, 50, line);
    if(r==65535) snprintf(line,sizeof(line),"R --"); else snprintf(line,sizeof(line),"R%3u",r);
    oled_text(70, 50, line);
    int spd=(abs(g.cur_l)+abs(g.cur_r))/2;
    snprintf(line,sizeof(line),"%d%%", spd);
    oled_text(104, 50, line);   /* BUG-fix: x=110 pe "100%" clip hota tha (128px screen) */

    oled_flush();
}

/* ------------------------- Car OS external API --------------------------- */

/* RELATIVE heading only (1.md 3.2): gyro-integrated by imu_adv fast task,
   drift-corrected when stationary >2 s. No magnetometer - never true north. */
float car_get_yaw(void) { return imu_adv_ready() ? imu_adv_heading_rel() : 0.0f; }

bool car_us_front_ok(void) { return s_us_front_ok; }

uint8_t car_get_setting(uint8_t idx)
{
    switch (idx) {
    case 0: return s_set_speed_cap;
    case 1: return s_set_engine_vol;
    case 2: return s_set_obstacle_cm;
    case 3: return s_set_led_bright;
    case 4: return s_set_mist_max;      /* x10s, 0 = unlimited (guide 11) */
    case 5: return s_set_tft_bright;    /* TFT backlight % (guide 10/11) */
    default: return 0;
    }
}

void car_set_setting(uint8_t idx, uint8_t v)
{
    switch (idx) {
    case 0: s_set_speed_cap = v; break;
    case 1: s_set_engine_vol = v; break;
    case 2: s_set_obstacle_cm = v; break;
    case 3: s_set_led_bright = v; break;
    case 4: s_set_mist_max = v; break;
    case 5:
        s_set_tft_bright = v;
        tft_set_brightness(v);          /* apply to hardware immediately */
        break;
    default: break;
    }
}

uint8_t car_get_gear_cap(uint8_t i)
{
    return (i < CAR_GEAR_COUNT) ? s_gear_caps[i] : 0;
}

void car_set_gear_cap(uint8_t i, uint8_t v)
{
    if (i < CAR_GEAR_COUNT) s_gear_caps[i] = v;
}

void car_settings_save(void) { settings_nvs_save(); }

void car_cycle_gear(void) { g.gear = (uint8_t)((g.gear + 1) % CAR_GEAR_COUNT); }

uint8_t car_speed_cap_pct(void)
{
    if (g.gear >= CAR_GEAR_COUNT) g.gear = 0;
    uint8_t gc = s_gear_caps[g.gear];
    return s_set_speed_cap < gc ? s_set_speed_cap : gc;
}

/* OLED wrappers (os_oled.c renders through these) */
void osd_oled_clear(void) { oled_clear(); }

void osd_oled_rect(int x1, int y1, int x2, int y2, bool fill) { oled_rect(x1, y1, x2, y2, fill); }

void osd_oled_text(int x, int y, const char *s) { oled_text(x, y, s); }

void osd_oled_text_big(int x, int y, const char *s, int scale) { oled_text_big(x, y, s, scale); }

void osd_oled_flush(void) { oled_flush(); }

void car_oled_hud(void) { hud_update(); }

/* game SFX (embedded WAVs + synth blips) */
void car_sfx_click(void)
{
    /* PART 5 funnel: UI tap prefers the bank sample, embedded tick fallback */
    if (!snd_play("ui_tap"))
        play_shot(_binary_a_click_wav_start, _binary_a_click_wav_end, 800);
}

void car_sfx_score(void)
{
    /* PART 5 funnel: all scoring (games, saves, rewards) plays score_tick */
    if (!snd_play("score_tick"))
        play_shot(_binary_yclick_wav_start, _binary_yclick_wav_end, 900);
}

void car_sfx_bad(void)
{
    /* PART 5 funnel: errors play ui_error */
    if (!snd_play("ui_error"))
        play_shot(_binary_laser_wav_start, _binary_laser_wav_end, 900);
}

void car_sfx_blip(uint16_t freq)
{
    s_blip_until = now_ms() + 60;
    s_blip_freq = freq;
}

/* ------------------------------ self test ------------------------------- */

/* Instant-boot: self_test removed from the boot path (servo sweep + beeps +
   LED tests cost seconds). Servos are already centered in hw_init. Kept for
   manual diagnostics; call it explicitly if needed. */
static void __attribute__((unused)) self_test(void)
{
    oled_clear();
    oled_text(0, 0, "SELF TEST");
    oled_text(0, 16, "SERVO SWEEP...");
    oled_flush();

    /* AUDIO SELF-TEST: loud 1s 800Hz beep.
       Agar ye Nahi sunai de -> amp ki DIN wire abhi bhi purane GPIO35 par hai.
       Firmware ab DIN = GPIO 40 par output karta hai (35 = octal PSRAM). */
    ESP_LOGI(TAG, "AUDIO SELF-TEST: 1s beep — sunai de to I2S OK (DIN=GPIO40)");
    s_sweep_until = now_ms() + 1000;
    s_sweep_f0 = 800;
    s_sweep_f1 = 800;
    s_sweep_amp = 9000;
    for (int i = 0; i < 100; i++) {
        audio_effect_update(now_ms());
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_sweep_until = now_ms(); /* beep off */

    for (int d = 0; d <= 180; d += 30) {
        servo_deg(LEDC_CHANNEL_0, d);
        servo_deg(LEDC_CHANNEL_1, 180 - d);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    servo_deg(LEDC_CHANNEL_0, 90);
    servo_deg(LEDC_CHANNEL_1, 90);
    for (int d = 0; d <= 90; d += 30) {
        servo_deg(LEDC_CHANNEL_3, d);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    servo_deg(LEDC_CHANNEL_3, 0);

    /* ignition crank (MAX98357A): starter motor jaisi gaddar awaaz */
    s_sweep_until = now_ms() + 250;
    s_sweep_f0 = 140;
    s_sweep_f1 = 45;
    s_sweep_amp = 3500;
    for (int i = 0; i < 26; i++) { audio_effect_update(now_ms()); vTaskDelay(pdMS_TO_TICKS(10)); }

    /* DEDICATED STRIP & FRONT LED HARDWARE TEST AT BOOT */
    ESP_LOGI(TAG, "RUNNING LED HARDWARE TEST...");
    
    /* 1. Test Rear Strip pixels (0=left,1=right; hw triples each to 3 physical LEDs) */
    for (int i = 0; i < LED_STRIP_NUM; i++) {
        led_strip_clear(s_strip);
        strip_px(i, 255, 255, 255); // White test on each rear LED
        led_strip_refresh(s_strip);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);

    /* 2. Test Front Left LED (WS2812, pin 17) via RMT - NOT plain GPIO */
    if (s_front_l) {
        led_strip_set_pixel(s_front_l, 0, 255, 255, 255);
        led_strip_refresh(s_front_l);
        vTaskDelay(pdMS_TO_TICKS(80));
        led_strip_clear(s_front_l);
        led_strip_refresh(s_front_l);
        ESP_LOGI(TAG, "Front Left LED test done");
    } else {
        ESP_LOGE(TAG, "Front Left LED NULL - RMT init failed, skipping test");
    }

    /* 3. Test Front Right LED (WS2812, pin 15) via RMT - NOT plain GPIO */
    if (s_front_r) {
        led_strip_set_pixel(s_front_r, 0, 255, 255, 255);
        led_strip_refresh(s_front_r);
        vTaskDelay(pdMS_TO_TICKS(80));
        led_strip_clear(s_front_r);
        led_strip_refresh(s_front_r);
        ESP_LOGI(TAG, "Front Right LED test done");
    } else {
        ESP_LOGE(TAG, "Front Right LED NULL - RMT init failed, skipping test");
    }

    /* 4. Test Under-car strip (GPIO 6 daisy-chain, 12 LEDs): quick white blink */
    if (s_under) {
        under_set_all(255, 255, 255);
        vTaskDelay(pdMS_TO_TICKS(80));
        under_set_all(0, 0, 0);
        ESP_LOGI(TAG, "Under strip test done (%d LEDs)", UNDER_STRIP_NUM);
    } else {
        ESP_LOGW(TAG, "Under strip NULL - RMT init failed/skipped");
    }

    oled_clear();
    oled_text(0, 0, "SELF TEST OK");
    oled_text(0, 16, "DONGLE PLUG KARO");
    oled_text(0, 32, "X = AUTO MODE");
    oled_flush();
}

/* Early boot splash (instant-boot): shown once after TFT init while hardware
   comes up (~1s). The drive loop takes over immediately after - no anim. */
static void boot_splash_stage(const char *stage)
{
    if (!tft_is_ready() || !gfx_fb()) return;
    gfx_clear(RGB565(10, 14, 24));
    gfx_rect_outline(10, 60, 300, 120, RGB565(255, 176, 32));
    gfx_text_center_box(0, 84, 320, "AZAM CAR OS", UI_FONT_LARGE, RGB565(232, 238, 245));
    gfx_text_center_box(0, 118, 320, "ROBOTICS CONTROL SYSTEM", UI_FONT_SMALL, RGB565(112, 130, 151));
    gfx_text_center_box(0, 150, 320, stage, UI_FONT_SMALL, RGB565(34, 211, 238));
    gfx_push();
}

/* ------------------------------- hardware ------------------------------- */

static void hw_init(void)
{
    // NOTE: headlight relay = GPIO46, rear light = GPIO3, mist = GPIO16
    // (sab active LOW). GPIO14 backlight relay HARDWARE SE HATAYA gaya hai;
    // GPIO14 ab MIC WS hai (PART 8, mic_init owns it). GPIO48 board-RGB ab
    // use NAHI hota (MIC SD input hai); TFT BL 3.3V pe hardwired hai.
    // --- TFT init (SPI2: SCLK=21 MOSI=38 CS=45 DC=0 RST=2, BL = TFT_PIN_BL -1) ---
    ESP_LOGI(TAG, "hw_init: tft_init start");
    pin_conflict_check();       /* GPIO uniqueness sanity check (review 2026-09-17) */
    tft_init();
    boot_splash_stage("STARTING");   /* audit: no more black screen during init */
    /* BUG-13 (historical): headlight/rear/mist are relays (active LOW digital),
       not PWM LEDs. Body-backlight relay (was GPIO14) removed in hardware;
       GPIO14 = MIC WS now (PART 8). */
    gpio_config_t out = {
        .pin_bit_mask = BIT64(PIN_US_TRIG_L) | BIT64(PIN_US_TRIG_R) | BIT64(PIN_US_TRIG_F) | BIT64(PIN_HEADLIGHT) |
                        BIT64(PIN_REAR_LIGHT) | BIT64(PIN_MIST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out);
     gpio_set_level(PIN_REAR_LIGHT, 0); gpio_set_level(PIN_MIST, 1); gpio_set_level(PIN_HEADLIGHT, 1); gpio_set_level(PIN_US_TRIG_L, 0); gpio_set_level(PIN_US_TRIG_R, 0); gpio_set_level(PIN_US_TRIG_F, 0); // rear light ON at boot, all TRIG idle
    g.rear_light_on = true;
    // self-test relays at boot: click to verify hardware
    vTaskDelay(pdMS_TO_TICKS(80)); gpio_set_level(PIN_REAR_LIGHT, 1); vTaskDelay(pdMS_TO_TICKS(80)); gpio_set_level(PIN_REAR_LIGHT, 0);
    vTaskDelay(pdMS_TO_TICKS(200)); gpio_set_level(PIN_MIST, 0); vTaskDelay(pdMS_TO_TICKS(80)); gpio_set_level(PIN_MIST, 1);
    vTaskDelay(pdMS_TO_TICKS(200)); gpio_set_level(PIN_HEADLIGHT, 0); vTaskDelay(pdMS_TO_TICKS(80)); gpio_set_level(PIN_HEADLIGHT, 1);

    gpio_config_t in = {
        .pin_bit_mask = BIT64(PIN_US_ECHO_L) | BIT64(PIN_US_ECHO_F) | BIT64(PIN_US_ECHO_R),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);

    /* LEDC: timer0 = servos 50Hz 14-bit, timer1 = motors+lights 20kHz 10-bit,
       timer2 = buzzer tones */

    ledc_timer_config_t t0 = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t0));

    ledc_timer_config_t t1 = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_1,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t1));

    ledc_timer_config_t t2 = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_2,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 4000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t2));

    ledc_fade_func_install(0);

    struct { gpio_num_t pin; ledc_channel_t ch; ledc_timer_t timer; } chans[] = {
        { PIN_SERVO_PAN, LEDC_CHANNEL_0, LEDC_TIMER_0 },
        { PIN_SERVO_TILT, LEDC_CHANNEL_1, LEDC_TIMER_0 },
        /* ch2 (gripper) + ch3 (spoiler) removed - servos removed */
        // PIN_BUZZER removed - amp does horn via I2S
    };
    for (size_t i = 0; i < sizeof(chans) / sizeof(chans[0]); i++) {
        ledc_channel_config_t cc = {
            .gpio_num = chans[i].pin,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = chans[i].ch,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = chans[i].timer,
            .duty = 0,
            .hpoint = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&cc));
    }

    servo_deg(LEDC_CHANNEL_0, 90);
    servo_deg(LEDC_CHANNEL_1, 90);

    /* UART1 -> D1 motor slave TX GPIO1 @115200 ("L<l>,R<r>\n") */
    uart_config_t uc = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uc));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, PIN_UART_TX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 256, 0, 0, NULL, 0));

    /* I2C + OLED (gyro removed - only OLED on bus) */
    i2c_master_bus_config_t bcfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_OLED_SDA,
        .scl_io_num = PIN_OLED_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bcfg, &s_bus));
    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dcfg, &s_oled));
    oled_init();
    /* MPU-6500 shares the same I2C bus (SDA = GPIO 42, SCL = GPIO 41). */
    imu_driver_init(s_bus);

    /* LED strip (rear indicator GPIO 6) + UNDER-CAR strip DAISY-CHAINED on the
       same data line: rear strip DOUT -> under strip DIN.
       Pixel map: 0..1 = rear (L/R), 2..13 = under-car (12 LEDs).
       Board pe koi free GPIO nahi bacha (19/20 USB, 26-37 flash+PSRAM,
       43/44 console UART) — isliye chain approach. */
    led_strip_config_t sc = {
        .strip_gpio_num = PIN_LED_STRIP,
        .max_leds = LED_STRIP_NUM + UNDER_STRIP_NUM,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rc = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 48,
        .flags.with_dma = true,  /* DMA ON: 14-LED chain needs stable timing under CPU load */
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&sc, &rc, &s_strip));
    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);
    s_under = s_strip;  /* alias — under_px() offset LED_STRIP_NUM se likhta hai */
    ESP_LOGI(TAG, "Rear strip (pin %d, %d px) + under strip daisy-chain (%d px, total %d)",
             PIN_LED_STRIP, LED_STRIP_NUM, UNDER_STRIP_NUM, LED_STRIP_NUM + UNDER_STRIP_NUM);


    /* Front LEDs (GPIO 17, 15) - WS2812 needs RMT, GPIO bitbang wont light them */
    led_strip_config_t sc_fl = { .strip_gpio_num = PIN_FRONT_LED_L, .max_leds = 1, .led_pixel_format = LED_PIXEL_FORMAT_GRB, .led_model = LED_MODEL_WS2812 };
    led_strip_rmt_config_t rc_fl = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 48,
        .flags.with_dma = false,
    };
    esp_err_t err_fl = led_strip_new_rmt_device(&sc_fl, &rc_fl, &s_front_l);
    if (err_fl != ESP_OK) {
        ESP_LOGE(TAG, "Front Left RMT FAILED (err=%d) - LED off!", err_fl);
        s_front_l = NULL;
    } else {
        led_strip_clear(s_front_l);
        led_strip_refresh(s_front_l);
        ESP_LOGI(TAG, "Front Left LED (pin %d) RMT OK", PIN_FRONT_LED_L);
    }

    led_strip_config_t sc_fr = { .strip_gpio_num = PIN_FRONT_LED_R, .max_leds = 1, .led_pixel_format = LED_PIXEL_FORMAT_GRB, .led_model = LED_MODEL_WS2812 };
    led_strip_rmt_config_t rc_fr = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 48,
        .flags.with_dma = false,
    };
    esp_err_t err_fr = led_strip_new_rmt_device(&sc_fr, &rc_fr, &s_front_r);
    if (err_fr != ESP_OK) {
        ESP_LOGE(TAG, "Front Right RMT FAILED (err=%d) - LED off!", err_fr);
        s_front_r = NULL;
    } else {
        led_strip_clear(s_front_r);
        led_strip_refresh(s_front_r);
        ESP_LOGI(TAG, "Front Right LED (pin %d) RMT OK", PIN_FRONT_LED_R);
    }

    /* Under-car strip: alag RMT channel NAHI — rear strip ke saath daisy-chained
       (upar dekho). Board pe free GPIO nahi bacha tha. */

    /* battery ADC */
    // adc_init removed - battery function removed (GPIO1 now backlight)

    /* MAX98357A sound */
    audio_init();

    /* roof light WS2812 (GPIO18) - single owner: roof_light.c */
    roof_light_init();

    ESP_LOGI(TAG, "car hardware ready");
}

/* --------------------------------- task --------------------------------- */

/* Late init (instant-boot): slow SPIFFS sound loads + prefetch + mic run in a
   background task AFTER the drive loop starts, so the car is drivable in
   ~2s. All consumers are NULL/absent-guarded until assets arrive.
   Yields between steps are MANDATORY: blocking SPIFFS flash reads never
   yield by themselves and would starve IDLE1 into a task-WDT trigger. */
static void late_init_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(200));   /* let drive loop + IDLE settle first */
    /* PART 8 first (small + fast): mic live in ~3s, not ~45s */
    mic_init();             /* PART 8: I2S1 mic (SCK13/WS14/SD48) + PSRAM ring */
    vTaskDelay(pdMS_TO_TICKS(100));
    boot_sounds_init();     /* PART 5: mount SPIFFS + horn/intro to PSRAM */
    vTaskDelay(pdMS_TO_TICKS(100));
    /* PART 6/16: preload cockpit image set (theme wall + 8+8 rotation
       sprites; draw path never touches SPIFFS after boot) */
    {
        static const char *imgs[] = {
            "wall_night",
            "car_N", "car_NE", "car_E", "car_SE",
            "car_S", "car_SW", "car_W", "car_NW",
            "car_N_d", "car_NE_d", "car_E_d", "car_SE_d",
            "car_S_d", "car_SW_d", "car_W_d", "car_NW_d",
        };
        for (unsigned i = 0; i < sizeof(imgs) / sizeof(imgs[0]); i++) {
            img_get(imgs[i]);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    snd_bank_preload_all(); /* PART 16: whole bank last (slow lookups, background) */
    /* NOTE: no gfx_push here - the TFT belongs to car_task only; pushing from
       a second task races the in-flight SPI transfer and deadlocks. */
    snd_play("sys_boot_chime");   /* PART 5: all-systems-ready chime */
    ESP_LOGI(TAG, "late init done - all systems ready");
    vTaskDelete(NULL);
}

void car_task(void *arg)
{
    s_boot_ms = now_ms();
    /* FIX (audit): os_init() was never called - analytics/assets/defaults
       were dead and BOOT state relied on zero-init luck. */
    os_init(&s_os);
    hw_init();
    // Init distance buffers to invalid (65535) so first avg is not 0
    for (int i = 0; i < 4; i++) {
        g.dist[i] = 65535;
        g.dist_avg[i] = 65535;
        for (int j = 0; j < 3; j++) g.dist_buf[i][j] = 65535;
    }
    g.dist_idx = 0;
    advBegin();
    settings_nvs_load();   /* NVS init ho chuka (advBegin) - saved settings wapas */
    car_roof_apply_saved(); /* roof pattern/brightness from NVS (stays OFF) */
    imu_adv_init();         /* MPU-advanced fast task 100 Hz (1.md PART 4) */
    /* PART 15: PSRAM tables now (snd/img/notif) - zero internal statics */
    snd_bank_init();
    img_bank_init();
    notif_init();
    tft_set_brightness(s_set_tft_bright);  /* saved backlight brightness (guide 10/11) */
    settings_nvs_save();   /* persist new defaults if schema changed */

    /* CLOCK/STANDBY first: table clock, motors locked. START opens the
       menu, DRIVE is picked from there. (Instant-boot to DRIVE removed.) */
    s_os.state = OS_STANDBY;
    s_os.prev_state = OS_BOOT;
    s_os.state_enter_ms = now_ms();
    /* PART 5: sys_ready plays from flash embed (zero-copy, no mount yet);
       SPIFFS mount + bulk loads stay in late_init_task. */
    {
        extern const uint8_t _binary_sys_ready_raw_start[];
        extern const uint8_t _binary_sys_ready_raw_end[];
        size_t n = (size_t)(_binary_sys_ready_raw_end - _binary_sys_ready_raw_start);
        uint16_t v = (uint16_t)(800u * car_get_setting(1) / 100u);
        if (n > 0 && v) car_snd_play_pcm(_binary_sys_ready_raw_start, (uint32_t)n, v);
    }
    xTaskCreatePinnedToCore(late_init_task, "late_init", 4096, NULL, 1, NULL, 1);
    ESP_LOGI(TAG, "Car OS ready - state %s", os_state_name(s_os.state));
    ESP_LOGI(TAG, "FW: cockpit instant-boot (B=roof, BACK=view, BACK-hold=theme, START-hold=home)");
    ESP_LOGI(TAG, "Car OS ready - state %s", os_state_name(s_os.state));
    ESP_LOGI(TAG, "PSRAM free: %u KB | internal free: %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    uint16_t prev_dig = 0;
    bool prev_present = false;
    uint32_t last_loop_ms = 0;

    while (1) {
        uint32_t now = now_ms();
        uint32_t dt_ms = last_loop_ms ? now - last_loop_ms : LOOP_MS;
        if (dt_ms > 100) dt_ms = 100;
        last_loop_ms = now;
        const xbox360_pad_t *p0 = xbox360_pad(0);
        const xbox360_pad_t *pad = (p0 && p0->present && xbox360_dongle_connected()) ? p0 : NULL;
        bool cur_present = pad != NULL;
        if (cur_present && !prev_present) {
            /* just reconnected — failsafe so car moves again.
               E-STOP stays latched (GUIDE-tap + stationary clears it):
               auto-clearing here would let deflected sticks drive off. */
            xbox360_rumble(0, 0, 0);
            s_rumble_until = 0;
            s_drive_locked = true;   /* neutral 250 ms before any motion */
            s_lock_nt0 = 0;
            s_fade_from = -1;        /* no stale engine fade blip */
            s_last_es = 0;
            snd_play("sys_connect");
            ESP_LOGI(TAG, "controller reconnected (mode=%d)", g.mode);
        }
        if (!cur_present && prev_present) {
            /* just disconnected — failsafe: motors off */
            g.tgt_l = g.tgt_r = 0;
            g.estop = true;
            roof_light_force_safe_off();
            s_rumble_until = 0;
            xbox360_rumble(0, 0, 0);
            snd_play("sys_disconnect");
        }
        prev_present = cur_present;
        uint16_t dig = pad ? (uint16_t)(pad->buttons >> 16) : 0;
        uint16_t tap = (uint16_t)(dig & ~prev_dig);
        if (pad && pad->present && dig && dig != prev_dig) s_last_dig = dig;
        prev_dig = dig;

        /* Sensors OFF in CLOCK_IDLE: no ultrasonic pinging, no MPU reads,
           no IMU event/lockout processing. Exempt while gyro cal is still
           running so the boot calibration in standby can complete. The
           imu_adv fast task freezes separately (no drift integration). */
        bool sensors_off = (os_context() == INPUT_CTX_STANDBY) &&
                           imu_driver_cal_state() != IMU_CAL_RUN;
        if (!sensors_off) {
            us_read();
            dist_update_avg();
            imu_driver_poll(dt_ms);
            imu_events_handle(now);   /* MPU-advanced event bus (1.md 4.1/4.3) */
        }
        /* Flaky-MPU safety net: a boot-time probe miss leaves s_attached
           false forever (poll returns early). Re-attach every 10s while
           the IMU has never produced data. Zero cost when healthy. */
        {
            static uint32_t s_imu_retry;
            if (!motion_radar_imu_is_valid() &&
                (int32_t)(now - s_imu_retry) >= 0) {
                s_imu_retry = now + 10000;
                imu_driver_retry();
            }
        }
        mic_poll(now);            /* PART 8 M1: VU 50ms + beat + clap detect */
        /* PART 8 M2: double-clap (parked only) toggles headlight */
        if (mic_clap()) {
            bool parked = (g.cur_l == 0 && g.cur_r == 0 &&
                           g.tgt_l == 0 && g.tgt_r == 0);
            if (parked) {
                g.headlight = !g.headlight;
                snd_play("clap_detected");
                notif_push(NOTIF_SOUND, g.headlight ? "CLAP lights ON" : "CLAP lights OFF", now);
                cockpit_toast(g.headlight ? "CLAP: LIGHTS ON" : "CLAP: LIGHTS OFF", now);
                ESP_LOGI(TAG, "double-clap -> headlight %s", g.headlight ? "ON" : "OFF");
            }
        }

        input_handle(pad, dig, tap, now);

        if (os_parked()) {
            /* Car OS owns the car (launcher menus / games): motors stay 0 */
            g.tgt_l = 0;
            g.tgt_r = 0;
        } else if (g.mode == MODE_AUTO) {
            if (pad) {
                auto_logic(pad, now);
            } else {
                g.tgt_l = 0;
                g.tgt_r = 0;
            }
        } else if (pad && !s_drive_locked) {
            manual_logic(pad, dig, now);
        } else {
            /* controller-lost OR neutral-release lock (guide 2.6): motors 0 */
            g.tgt_l = 0;
            g.tgt_r = 0;
            g.mist = false;
            g.braking = false;
            g.reversing = false;
        }

        /* ---- MPU-advanced safety gating (1.md 4.3, ADDITIVE to E-STOP) ---- */
        if (imu_adv_motors_cut()) {
            /* TILT_CRITICAL / ROLLOVER latch / FREEFALL airtime */
            g.tgt_l = 0;
            g.tgt_r = 0;
        } else if ((int32_t)(now - s_land_hold_until) < 0) {
            /* resume 200 ms after landing detect */
            g.tgt_l = 0;
            g.tgt_r = 0;
        } else if ((int32_t)(now - s_land_ramp_until) < 0) {
            /* gentle recovery ramp (no power jerk) */
            g.tgt_l /= 2;
            g.tgt_r /= 2;
        }
        {   /* CORNER soft/hard speed-cap during turn only */
            uint8_t cc = imu_adv_corner_cap();
            if (cc == 2) { g.tgt_l /= 4; g.tgt_r /= 4; }
            else if (cc == 1) { g.tgt_l = g.tgt_l * 2 / 5; g.tgt_r = g.tgt_r * 2 / 5; }
        }
        {   /* DRIFT in AUTO: reduce speed + straighten (manual: alert only) */            imu_adv_snap_t dsnap;
            imu_adv_snapshot(&dsnap);
            if (dsnap.drift && g.mode == MODE_AUTO) {
                int16_t avg = (int16_t)(((int32_t)g.tgt_l + (int32_t)g.tgt_r) / 2 * 3 / 5);
                g.tgt_l = avg;
                g.tgt_r = avg;
            }
            /* PART 5: drive-grade improvement fanfare (F->..->A) */            static char s_last_grade = 0;
            if (s_last_grade && dsnap.drive_grade != s_last_grade) {
                int rank = (dsnap.drive_grade == 'A') ? 5 :
                           (dsnap.drive_grade == 'B') ? 4 :
                           (dsnap.drive_grade == 'C') ? 3 :
                           (dsnap.drive_grade == 'D') ? 2 : 1;
                int old = (s_last_grade == 'A') ? 5 :
                          (s_last_grade == 'B') ? 4 :
                          (s_last_grade == 'C') ? 3 :
                          (s_last_grade == 'D') ? 2 : 1;
                if (rank > old) {
                    if (dsnap.drive_grade == 'A') snd_play("grade_A");
                    else if (dsnap.drive_grade == 'B') snd_play("grade_B");
                    else if (dsnap.drive_grade == 'C') snd_play("grade_C");
                }
            }
            s_last_grade = dsnap.drive_grade;
            /* PART 10 trip computer (uses gated targets + live tilt) */
            {
                float tilt = dsnap.pitch_deg < 0 ? -dsnap.pitch_deg : dsnap.pitch_deg;
                float rl = dsnap.roll_deg < 0 ? -dsnap.roll_deg : dsnap.roll_deg;
                trip_update(g.tgt_l, g.tgt_r, tilt > rl ? tilt : rl, dt_ms);
            }
        }

        /* PART 12 LT+RT full chord = panic brake-and-hold (soft stop via
           normal ramp, NO lockout unlike E-STOP - release resumes). */
        {
            static bool s_panic_was = false;
            bool panic = (pad && pad->lt > 200 && pad->rt > 200);
            if (panic) {
                g.tgt_l = 0;
                g.tgt_r = 0;
                g.braking = true;
                if (!s_panic_was) {
                    ESP_LOGI(TAG, "panic-hold: lt=%d rt=%d ctx=%d",
                             pad ? pad->lt : -1, pad ? pad->rt : -1,
                             (int)os_context());
                    cockpit_toast("PANIC HOLD", now);
                    snd_play("sys_warning");
                    rumble_preset(RUMBLE_PANIC);
                }
            }
            s_panic_was = panic;
        }

        drive_output(now, pad);
        strip_update(now);
        roof_light_update(now);
        audio_effect_update(now);
        led_tick(now);   /* PART 13.1 controller LED ring status */
        lowbatt_watch(now, pad);   /* PART 13.3 low-battery banner */
        /* continuous haptics while held */
        if (s_horn_on && !s_rumble_until) rumble(200, 255, 250);
        else if (s_y_held && !s_rumble_until) rumble(120, 180, 250);
        else if (g.reversing && g.mode != MODE_AUTO && !s_rumble_until && (now % 600 < 40)) rumble(90, 0, 150);
        rumble_update(now);

        /* ------- Car OS: navigation, state machine, rendering ------- */
        os_handle_input(&s_os, pad, dig, tap, now);
        /* DRIVE_ARMING exit: ~1.3 s elapsed AND sticks/triggers neutral for
           250 ms continuous. Until then motors stay 0 (parked) and the boot
           anim keeps playing. B-cancel is handled in os_handle_input. */
        if (s_os.state == OS_DRIVE_ARMING && !g.estop) {
            bool neutral = pad && pad->present &&
                stick_dz(pad->lx) == 0 && stick_dz(pad->ly) == 0 &&
                stick_dz(pad->rx) == 0 && stick_dz(pad->ry) == 0 &&
                pad->rt < 5 && pad->lt < 5;
            if (!neutral) s_arm_nt0 = 0;
            else if (!s_arm_nt0) s_arm_nt0 = now;
            /* 1 s cadence diagnostics: proves WHY arming waits */
            {
                static uint32_t s_arm_log = 0;
                if ((int32_t)(now - s_arm_log) >= 0) {
                    s_arm_log = now + 1000;
                    ESP_LOGI(TAG, "arming t=%lu neutral=%d nt=%lu es=%d "
                             "lx=%d ly=%d rx=%d ry=%d lt=%d rt=%d",
                             (unsigned long)(now - s_os.state_enter_ms),
                             neutral, (unsigned long)(s_arm_nt0 ? now - s_arm_nt0 : 0),
                             g.estop, pad ? pad->lx : 0, pad ? pad->ly : 0,
                             pad ? pad->rx : 0, pad ? pad->ry : 0,
                             pad ? pad->lt : 0, pad ? pad->rt : 0);
                }
            }
            if (now - s_os.state_enter_ms >= 1300 &&
                s_arm_nt0 && now - s_arm_nt0 >= 250) {
                s_arm_nt0 = 0;
                os_request_screen(&s_os, OS_DRIVE_MAIN, now);
            } else if (now - s_os.state_enter_ms >= 8000) {
                /* safety valve: never freeze forever — back to HOME */
                ESP_LOGW(TAG, "arming abort (>8s) -> HOME");
                s_arm_nt0 = 0;
                os_request_screen(&s_os, OS_HOME, now);
            }
        } else {
            s_arm_nt0 = 0;   /* keep the neutral timer fresh across sessions */
        }
        os_update(&s_os, now);
        if (os_game_active()) os_game_frame(&s_os, pad, now);
        os_draw_tft(&s_os, now);    /* ~30 FPS, internal DMA fb push */
        /* PART 16: E-STOP renders above EVERY layer incl. games (L5).
           os_draw_tft skips games; overlay the alert card on top here. */
        if (os_game_active() && s_os.alert_active) {
            os_scr_safety_overlay(&s_os, now);
            gfx_push();
        }
        mem_diag_tick(now);         /* PART 15/16: 10s cadence, task ctx */
        os_draw_oled(&s_os, now);   /* ~12 FPS, per oled_layout */

        /* games use a faster loop for smooth 30 FPS frame pacing */
        vTaskDelay(pdMS_TO_TICKS(os_game_active() ? 20 : LOOP_MS));
    }
}
