/* SPDX-License-Identifier: Apache-2.0 */
/* alexa_bridge.c - Alexa cloud bridge + safety gateway (single-S3 build).
 *
 * ARCHITECTURE (adapted from blueprint §2 for one ESP32-S3):
 *   Echo -> Alexa cloud -> Lambda -> AWS IoT MQTT -> THIS board ->
 *   safety gateway -> Car OS. No second S3, no ESP-NOW, no Echo mods.
 *
 * THREADS: alexa_task owns WiFi-wait/MQTT/JSON/history. Hardware actions go
 * through ONE pending slot executed by alexa_apply_pending() in car_task.
 * Results flow back through ONE result slot -> bridge publishes resp.
 *
 * SAFETY GATEWAY (every ALEXA cmd):
 *   1. userId match  2. TTL 5 s  3. requestId dedupe  4. allow-list only
 *   5. E-stop blocks all actions  6. confirm-gated cmds need physical A/B
 *   7. args clamped  8. execute via Car OS paths only (car_task)
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"

#include "alexa_bridge.h"
#include "alexa_config.h"
#include "net_wifi.h"
#include "car_global.h"
#include "roof_light.h"
#include "tft_display.h"
#include "imu_driver.h"
#include "imu_adv.h"
#include "snd_bank.h"
#include "notif.h"   /* PART 10.3 Notification Center feed */
#include "xbox360.h"
#include "os_gfx.h"
#include "os_theme.h"
#include "os_widgets.h"

static const char *TAG = "alexa";

/* ------------------------------ config ---------------------------------- */
static char s_broker[128] = ALEXA_DEFAULT_BROKER;
static char s_user[64]    = ALEXA_DEFAULT_USER;
static char s_thing[64]   = ALEXA_DEFAULT_THING;
static char *s_ca = NULL, *s_cert = NULL, *s_key = NULL;  /* NVS blobs */

static char s_t_cmd[96], s_t_resp[96], s_t_state[96], s_t_event[96];
static char s_t_shadow[128];

static void load_str(nvs_handle_t h, const char *key, char *out, size_t n,
                     const char *dflt)
{
    size_t l = n;
    if (nvs_get_str(h, key, out, &l) != ESP_OK) {
        strncpy(out, dflt, n - 1);
        out[n - 1] = 0;
    }
}

static char *load_blob(nvs_handle_t h, const char *key)
{
    size_t l = 0;
    if (nvs_get_blob(h, key, NULL, &l) != ESP_OK || l == 0 || l > 8192)
        return NULL;
    char *b = malloc(l + 1);
    if (!b) return NULL;
    if (nvs_get_blob(h, key, b, &l) != ESP_OK) { free(b); return NULL; }
    b[l] = 0;
    return b;
}

static void config_load(void)
{
    /* Start with build-time defaults, then override from NVS if present */
    strncpy(s_broker, ALEXA_DEFAULT_BROKER, sizeof(s_broker) - 1);
    s_broker[sizeof(s_broker) - 1] = 0;
    strncpy(s_user,   ALEXA_DEFAULT_USER,   sizeof(s_user) - 1);
    s_user[sizeof(s_user) - 1] = 0;
    strncpy(s_thing,  ALEXA_DEFAULT_THING,  sizeof(s_thing) - 1);
    s_thing[sizeof(s_thing) - 1] = 0;

    nvs_handle_t h;
    if (nvs_open("alexa", NVS_READONLY, &h) != ESP_OK) h = 0;
    if (h) {
        load_str(h, "broker", s_broker, sizeof(s_broker), ALEXA_DEFAULT_BROKER);
        load_str(h, "user",   s_user,   sizeof(s_user),   ALEXA_DEFAULT_USER);
        load_str(h, "thing",  s_thing,  sizeof(s_thing),  ALEXA_DEFAULT_THING);
        s_ca   = load_blob(h, "ca");
        s_cert = load_blob(h, "cert");
        s_key  = load_blob(h, "key");
        nvs_close(h);
    }
    /* Fallback: use build-time PEMs if NVS blobs are empty */
    if (!s_ca && ALEXA_DEFAULT_CA[0]) {
        size_t len = strlen(ALEXA_DEFAULT_CA);
        s_ca = malloc(len + 1);
        if (s_ca) { memcpy(s_ca, ALEXA_DEFAULT_CA, len + 1); }
    }
    if (!s_cert && ALEXA_DEFAULT_CERT[0]) {
        size_t len = strlen(ALEXA_DEFAULT_CERT);
        s_cert = malloc(len + 1);
        if (s_cert) { memcpy(s_cert, ALEXA_DEFAULT_CERT, len + 1); }
    }
    if (!s_key && ALEXA_DEFAULT_KEY[0]) {
        size_t len = strlen(ALEXA_DEFAULT_KEY);
        s_key = malloc(len + 1);
        if (s_key) { memcpy(s_key, ALEXA_DEFAULT_KEY, len + 1); }
    }
    snprintf(s_t_cmd,   sizeof(s_t_cmd),   "azamcar/cmd/%s",   s_user);
    snprintf(s_t_resp,  sizeof(s_t_resp),  "azamcar/resp/%s",  s_user);
    snprintf(s_t_state, sizeof(s_t_state), "azamcar/state/%s", s_user);
    snprintf(s_t_event, sizeof(s_t_event), "azamcar/event/%s", s_user);
    snprintf(s_t_shadow, sizeof(s_t_shadow),
             "$aws/things/%s/shadow/update", s_thing);
    ESP_LOGI(TAG, "broker=%s user=%s thing=%s tls_certs=%c%c%c",
             s_broker[0] ? s_broker : "(disabled)", s_user, s_thing,
             s_ca ? 'Y' : 'n', s_cert ? 'Y' : 'n', s_key ? 'Y' : 'n');
}

/* --------------------------- shared state ------------------------------- */
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }
static long now_epoch(void)
{
    return net_wifi_time_synced() ? (long)time(NULL) : 0;
}

/* Voice history: 20-event ring (blueprint §9B). PART 15: PSRAM. */
#define HIST_N 20
typedef struct { uint32_t t_ms; char text[56]; uint8_t res; } hist_t;
static hist_t *s_hist;
static uint8_t s_hist_n = 0, s_hist_head = 0;

static void hist_push(const char *text, alexa_result_t r)
{
    if (!s_hist) return;
    hist_t *e = &s_hist[s_hist_head];
    e->t_ms = now_ms();
    strncpy(e->text, text ? text : "", sizeof(e->text) - 1);
    e->text[sizeof(e->text) - 1] = 0;
    e->res = (uint8_t)r;
    s_hist_head = (uint8_t)((s_hist_head + 1) % HIST_N);
    if (s_hist_n < HIST_N) s_hist_n++;
}

/* Live banner (3 s auto-dismiss, blueprint §9A) */
typedef struct {
    volatile bool active;
    char heard[64];
    char action[40];
    volatile uint8_t res;
    uint32_t until_ms;
} banner_t;
static banner_t s_banner;
static uint32_t s_last_rx_ms;   /* PART 9.3: last MQTT cmd RX (voice latency) */

static void banner_show(const char *heard, const char *action, alexa_result_t r)
{
    strncpy(s_banner.heard, heard ? heard : "", sizeof(s_banner.heard) - 1);
    s_banner.heard[sizeof(s_banner.heard) - 1] = 0;   /* FIX: guaranteed NUL */
    strncpy(s_banner.action, action ? action : "", sizeof(s_banner.action) - 1);
    s_banner.action[sizeof(s_banner.action) - 1] = 0; /* FIX: guaranteed NUL */
    s_banner.res = (uint8_t)r;
    s_banner.until_ms = now_ms() + 3000;
    s_banner.active = true;
    notif_push(NOTIF_ALEXA, action && *action ? action : "Alexa", now_ms());
    /* PART 9.3: voice command -> TFT banner latency (target <500ms).
       Banner still shows PROCESSING state if exceeded. */
    if (s_last_rx_ms) {
        unsigned long d = (unsigned long)(now_ms() - s_last_rx_ms);
        ESP_LOGI(TAG, "voice->banner %lums%s", d, d > 500 ? " OVER BUDGET" : "");
    }
}

/* Pending action: bridge validates, car_task executes (ONE slot) */
typedef enum {
    AA_NONE = 0,
    AA_HEADLIGHT, AA_HAZARD, AA_REAR,
    AA_ROOF_COLOR, AA_ROOF_MODE, AA_ROOF_BRIGHT,
    AA_ALL_OFF, AA_WARN,
    AA_SCREEN, AA_TFT_BRIGHT, AA_THEME, AA_MUTE, AA_VOLUME, AA_PROFILE,
    AA_SCENE,   /* PART 9.6 ambient scene bundle */
} aa_kind_t;

typedef struct {
    volatile bool busy;
    uint8_t kind;
    int32_t iarg;
    char sarg[24];
    char req_id[40];
} pending_t;
static pending_t s_pend;

/* Result: car_task fills, bridge publishes */
typedef struct {
    volatile bool ready;
    char req_id[40];
    uint8_t status;          /* alexa_result_t */
    char reason[32];
} result_t;
static result_t s_res;

static void result_set(const char *req_id, alexa_result_t st, const char *reason)
{
    strncpy(s_res.req_id, req_id ? req_id : "", sizeof(s_res.req_id) - 1);
    s_res.req_id[sizeof(s_res.req_id) - 1] = 0;      /* FIX: guaranteed NUL */
    s_res.status = (uint8_t)st;
    strncpy(s_res.reason, reason ? reason : "", sizeof(s_res.reason) - 1);
    s_res.reason[sizeof(s_res.reason) - 1] = 0;      /* FIX: guaranteed NUL */
    s_res.ready = true;      /* publish last */
}

/* Confirm dialog: physical A/B on the car (blueprint §E) */
typedef struct {
    bool active;
    char text[40];
    uint8_t kind;            /* aa_kind_t to execute on A */
    int32_t iarg;
    char sarg[24];
    char req_id[40];
    uint32_t expiry_ms;
} confirm_t;
static confirm_t s_conf;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* requestId dedupe ring (replay protection). PART 15: PSRAM. */
#define DEDUP_N 16
static char (*s_dedup)[40];
static uint8_t s_dedup_idx = 0;

static bool dedup_seen(const char *id)
{
    if (!id || !*id || !s_dedup) return false;
    for (int i = 0; i < DEDUP_N; i++)
        if (strcmp(s_dedup[i], id) == 0) return true;
    strncpy(s_dedup[s_dedup_idx], id, sizeof(s_dedup[0]) - 1);
    s_dedup_idx = (uint8_t)((s_dedup_idx + 1) % DEDUP_N);
    return false;
}

/* Counters (NVS "alexa", survive reboot for day reports) */
static uint32_t s_c_estop = 0, s_c_pad = 0, s_c_front = 0, s_c_stuck = 0;
static uint32_t s_boot_ms = 0;
static uint32_t s_last_drive_min = 0;
static char s_last_alert[64] = "none yet";
/* MPU-advanced impact note (defined near publish_event, fwd here because
   state_json() reads it and is defined earlier in the file) */
static char s_last_impact[48] = "none";
static uint32_t s_impact_ms;
static uint8_t s_profile = 7;        /* 0..6 profile, 7 = manual */
static uint8_t s_tft_bright = 80;    /* local mirror (no getter in TFT drv) */
static bool s_warn_active = false;
static uint32_t s_warn_until = 0;
static bool s_moving_prev = false;
static uint32_t s_move_t0 = 0;
/* PART 9.6: yesterday-score (prev boot's last score) + personality speech.
   s_speech/s_last_impact are written in car_task, read in alexa_task:
   portMUX keeps cross-task string reads tear-free. */
static uint16_t s_prev_score;
static char s_speech[80];
static portMUX_TYPE s_str_mux = portMUX_INITIALIZER_UNLOCKED;

static void speech_set(const char *s)
{
    if (!s) return;
    portENTER_CRITICAL(&s_str_mux);
    snprintf(s_speech, sizeof(s_speech), "%s", s);
    portEXIT_CRITICAL(&s_str_mux);
}

static void impact_note_set(uint8_t level, float g)
{
    portENTER_CRITICAL(&s_str_mux);
    snprintf(s_last_impact, sizeof(s_last_impact), "L%u %.1fg", level, (double)g);
    portEXIT_CRITICAL(&s_str_mux);
}

static void counters_load(void)
{
    nvs_handle_t h;
    if (nvs_open("alexa", NVS_READONLY, &h) != ESP_OK) return;
    uint32_t v = 0;
    if (nvs_get_u32(h, "c_estop", &v) == ESP_OK) s_c_estop = v;
    if (nvs_get_u32(h, "c_pad", &v) == ESP_OK)   s_c_pad = v;
    if (nvs_get_u32(h, "c_front", &v) == ESP_OK) s_c_front = v;
    if (nvs_get_u32(h, "c_stuck", &v) == ESP_OK) s_c_stuck = v;
    if (nvs_get_u32(h, "last_score", &v) == ESP_OK && v <= 100)
        s_prev_score = (uint16_t)v;   /* previous boot = "yesterday" */
    uint8_t p = 7;
    size_t l = sizeof(p);
    if (nvs_get_blob(h, "profile", NULL, &l) != ESP_OK) {
        uint32_t pv = 7;
        if (nvs_get_u32(h, "profile", &pv) == ESP_OK && pv <= 7) p = (uint8_t)pv;
    }
    s_profile = p;
    nvs_close(h);
}

static void counter_bump(const char *key, uint32_t *c)
{
    (*c)++;
    nvs_handle_t h;
    if (nvs_open("alexa", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, key, *c);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* Local rules (Phase J stub, blueprint §F): obstacle-flash + idle-dim.
 * Stored NVS "alexa_rules": u8 n + u32 r0..r3. Bits: 0 valid,1 enabled,
 * 2-3 trigger (0 obstacle,1 idle), 4-5 action (0 flash HL,1 dim TFT). */
#define RULE_MAX 4
typedef struct { bool valid, enabled; uint8_t trig, act; } rule_t;
static rule_t s_rules[RULE_MAX];
static bool s_rule_lat[RULE_MAX];
static uint8_t s_dim_saved = 80;

static void rules_load(void)
{
    nvs_handle_t h;
    if (nvs_open("alexa_rules", NVS_READONLY, &h) != ESP_OK) return;
    for (int i = 0; i < RULE_MAX; i++) {
        char k[4];
        snprintf(k, sizeof(k), "r%d", i);
        uint32_t v = 0;
        if (nvs_get_u32(h, k, &v) == ESP_OK && (v & 1)) {
            s_rules[i].valid   = true;
            s_rules[i].enabled = (v >> 1) & 1;
            s_rules[i].trig    = (v >> 2) & 3;
            s_rules[i].act     = (v >> 4) & 3;
        }
    }
    nvs_close(h);
}

static void rules_save(void)
{
    nvs_handle_t h;
    if (nvs_open("alexa_rules", NVS_READWRITE, &h) != ESP_OK) return;
    for (int i = 0; i < RULE_MAX; i++) {
        char k[4];
        snprintf(k, sizeof(k), "r%d", i);
        uint32_t v = s_rules[i].valid
            ? (1 | (s_rules[i].enabled ? 2 : 0) |
               ((uint32_t)s_rules[i].trig << 2) | ((uint32_t)s_rules[i].act << 4))
            : 0;
        nvs_set_u32(h, k, v);
    }
    nvs_commit(h);
    nvs_close(h);
}

/* ------------------------------ MQTT ------------------------------------ */
static esp_mqtt_client_handle_t s_mqtt = NULL;
static volatile bool s_mqtt_ok = false;
bool alexa_mqtt_connected(void) { return s_mqtt_ok; }

/* inbox: MQTT event ctx -> bridge loop (parse in task, never in callback) */
#define INBOX_N 2
#define INBOX_SZ 1536
static char (*s_inbox)[INBOX_SZ];   /* PART 15: PSRAM (was 3KB internal) */
static volatile uint8_t s_in_w = 0, s_in_r = 0;

/* PART 15: PSRAM tables - called once from alexa_task startup (owner). */
static void alexa_psram_init(void)
{
    if (!s_hist) {
        s_hist = heap_caps_malloc(HIST_N * sizeof(hist_t),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_hist) memset(s_hist, 0, HIST_N * sizeof(hist_t));
    }
    if (!s_dedup) {
        s_dedup = heap_caps_malloc(DEDUP_N * 40, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_dedup) memset(s_dedup, 0, DEDUP_N * 40);
    }
    if (!s_inbox) {
        s_inbox = heap_caps_malloc(INBOX_N * INBOX_SZ,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_inbox) memset(s_inbox, 0, INBOX_N * INBOX_SZ);
    }
    if (!s_hist || !s_dedup || !s_inbox)
        ESP_LOGE(TAG, "psram tables failed - voice history/inbox degraded");
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    (void)arg; (void)base;
    switch (id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_ok = true;
        esp_mqtt_client_subscribe(s_mqtt, s_t_cmd, 1);
        ESP_LOGI(TAG, "mqtt up, sub %s", s_t_cmd);
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "mqtt sub ack msg_id=%d", e->msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_ok = false;
        ESP_LOGW(TAG, "mqtt down");
        break;
    case MQTT_EVENT_ERROR:
        if (e->error_handle) {
            ESP_LOGW(TAG, "mqtt err type=%d transport=0x%x tls=0x%x stack=%d",
                     (int)e->error_handle->error_type,
                     (unsigned)e->error_handle->esp_transport_sock_errno,
                     (unsigned)e->error_handle->esp_tls_last_esp_err,
                     (int)e->error_handle->esp_tls_stack_err);
        } else {
            ESP_LOGW(TAG, "mqtt err (no handle)");
        }
        break;
    case MQTT_EVENT_DATA:
        if (s_inbox && e->topic_len == (int)strlen(s_t_cmd) &&
            memcmp(e->topic, s_t_cmd, e->topic_len) == 0 &&
            e->data_len > 0 && e->data_len < INBOX_SZ) {
            uint8_t w = s_in_w;
            if ((uint8_t)(w + 1) % INBOX_N != s_in_r) {  /* drop if full */
                memcpy(s_inbox[w], e->data, e->data_len);
                s_inbox[w][e->data_len] = 0;
                s_in_w = (uint8_t)((w + 1) % INBOX_N);
                s_last_rx_ms = now_ms();   /* PART 9.3 latency anchor */
            }
        }
        break;
    default: break;
    }
}

static void mqtt_pub(const char *topic, const char *json, int qos, int retain)
{
    if (s_mqtt && s_mqtt_ok)
        esp_mqtt_client_publish(s_mqtt, topic, json, 0, qos, retain);
}

/* --------------------------- state snapshot ----------------------------- */
static const char *mode_name(void)
{
    return (g.mode == MODE_AUTO) ? "AUTO" : (g.mode == MODE_CRAWL) ? "CRAWL" : "MANUAL";
}

static const char *profile_names[] = {
    "park", "safe", "night", "demo", "performance", "silent", "game", "manual"
};
const char *alexa_profile_name(void)
{
    return profile_names[s_profile <= 7 ? s_profile : 7];
}

const char *alexa_result_str(alexa_result_t r)
{
    switch (r) {
    case ALEXA_RES_PROCESSING:    return "PROCESSING";
    case ALEXA_RES_DONE:          return "DONE";
    case ALEXA_RES_REJECTED:      return "REJECTED";
    case ALEXA_RES_NEEDS_CONFIRM: return "NEEDS_CONFIRM";
    case ALEXA_RES_CANCELLED:     return "CANCELLED";
    default:                      return "NONE";
    }
}

static int dist_cm(uint16_t raw) { return (raw == 65535) ? -1 : (int)raw; }
static bool moving(void) { return (g.cur_l != 0 || g.cur_r != 0); }

/* Full telemetry object for Lambda phrasing + shadow. Caller frees. */
static cJSON *state_json(const char *query_id, const char *cmd_id,
                         const char *cmd_res, const char *cmd_reason)
{
    imu_data_t imu = {0};
    motion_radar_imu_read(&imu);
    uint32_t now = now_ms();
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "mode", mode_name());
    cJSON_AddStringToObject(s, "profile", alexa_profile_name());
    cJSON_AddNumberToObject(s, "gear", (int)g.gear + 1);
    cJSON_AddBoolToObject(s, "estop", g.estop);
    cJSON_AddBoolToObject(s, "moving", moving());
    cJSON_AddNumberToObject(s, "front_cm", dist_cm(g.dist_avg[1]));
    cJSON_AddNumberToObject(s, "left_cm",  dist_cm(g.dist_avg[0]));
    cJSON_AddNumberToObject(s, "right_cm", dist_cm(g.dist_avg[2]));
    cJSON_AddNumberToObject(s, "obstacle_cm", car_get_setting(2));
    cJSON_AddBoolToObject(s, "headlight", g.headlight);
    cJSON_AddBoolToObject(s, "hazard", g.hazard);
    cJSON_AddBoolToObject(s, "rear_light", g.rear_light_on);
    const roof_light_state_t *rl = roof_light_state();
    cJSON_AddNumberToObject(s, "roof_mode", (int)rl->mode);
    cJSON_AddNumberToObject(s, "roof_bright", (int)rl->brightness);
    cJSON_AddBoolToObject(s, "roof_on", rl->enabled);
    cJSON_AddNumberToObject(s, "tft_bright", s_tft_bright);
    cJSON_AddNumberToObject(s, "volume", car_get_setting(1));
    cJSON_AddBoolToObject(s, "muted", g.snd_mute);
    cJSON_AddNumberToObject(s, "heap_free", (int)esp_get_free_heap_size());
    cJSON_AddNumberToObject(s, "psram_free",
        (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddBoolToObject(s, "pad_connected", xbox360_dongle_connected());
    cJSON_AddNumberToObject(s, "pad_age_ms",
        (int)(now - xbox360_last_data_ms()));
    cJSON_AddBoolToObject(s, "imu_valid", motion_radar_imu_is_valid());
    cJSON_AddNumberToObject(s, "imu_cal", (int)imu_driver_cal_state());
    cJSON_AddBoolToObject(s, "front_sensor_ok", car_us_front_ok());
    {   /* MPU-advanced snapshot (1.md 4.1: alexa_bridge consumes event bus) */
        imu_adv_snap_t adv;
        imu_adv_snapshot(&adv);
        cJSON_AddNumberToObject(s, "heading_rel", adv.heading_rel);
        cJSON_AddNumberToObject(s, "traction", (int)adv.traction);
        cJSON_AddBoolToObject(s, "airborne", adv.airborne);
        cJSON_AddBoolToObject(s, "drift", adv.drift);
        cJSON_AddNumberToObject(s, "shield", (int)adv.shield);
        cJSON_AddBoolToObject(s, "tilt_lockout", adv.tilt_lockout);
        cJSON_AddNumberToObject(s, "drive_score", (int)adv.drive_score);
        cJSON_AddNumberToObject(s, "last_impact", (int)adv.last_impact);
        {   /* locked copies: written in car_task, read here (alexa_task) */
            char note[48], speech[80];
            portENTER_CRITICAL(&s_str_mux);
            snprintf(note, sizeof(note), "%s", s_last_impact);
            snprintf(speech, sizeof(speech), "%s", s_speech);
            portEXIT_CRITICAL(&s_str_mux);
            cJSON_AddStringToObject(s, "last_impact_note", note);
        }
        /* PART 9.2/9.4 voice-query fields */
        cJSON_AddNumberToObject(s, "tilt_deg", adv.tilt_deg);
        cJSON_AddNumberToObject(s, "stability", (int)adv.shield_score);
        cJSON_AddNumberToObject(s, "terrain", (int)adv.terrain);
        cJSON_AddNumberToObject(s, "hard_bumps", (int)adv.impact_hard_cnt);
        uint32_t age_s = 0;
        if (adv.last_impact_ms) {
            uint32_t n = now_ms();
            age_s = (n > adv.last_impact_ms) ? (n - adv.last_impact_ms) / 1000 : 0;
        }
        cJSON_AddNumberToObject(s, "impact_age_s", (int)age_s);
    }
    {   /* PART 9.2/9.6 shadow additions: roof color, theme, scores, battery */
        const roof_light_state_t *rl2 = roof_light_state();
        cJSON_AddNumberToObject(s, "roof_color", (int)rl2->color_idx);
        os_ctx_t *octx = alexa_os_ctx();
        cJSON_AddNumberToObject(s, "theme", octx ? (int)(octx->cockpit_theme % 3) : 1);
        cJSON_AddNumberToObject(s, "prev_score", (int)s_prev_score);
        {   /* locked copy (see above) */
            char speech[80];
            portENTER_CRITICAL(&s_str_mux);
            snprintf(speech, sizeof(speech), "%s", s_speech);
            portEXIT_CRITICAL(&s_str_mux);
            cJSON_AddStringToObject(s, "speech", speech);
        }
        const xbox360_pad_t *pad = xbox360_pad(0);
        int batt = -1;
        if (pad && pad->present && pad->battery_valid) {
            static const int pct[4] = { 15, 45, 78, 100 };
            batt = pct[pad->battery & 3];
        }
        cJSON_AddNumberToObject(s, "pad_batt", batt);
    }
    cJSON_AddNumberToObject(s, "uptime_min", (int)((now - s_boot_ms) / 60000));
    cJSON_AddNumberToObject(s, "last_drive_min", (int)s_last_drive_min);
    cJSON_AddNumberToObject(s, "cnt_estop", (int)s_c_estop);
    cJSON_AddNumberToObject(s, "cnt_pad_drop", (int)s_c_pad);
    cJSON_AddNumberToObject(s, "cnt_front_stop", (int)s_c_front);
    cJSON_AddNumberToObject(s, "cnt_stuck", (int)s_c_stuck);
    cJSON_AddStringToObject(s, "last_alert", s_last_alert);
    cJSON_AddBoolToObject(s, "cal_due",
        imu_driver_cal_state() != IMU_CAL_DONE);
    cJSON *rules = cJSON_CreateArray();
    for (int i = 0; i < RULE_MAX; i++) {
        if (!s_rules[i].valid) continue;
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "id", i);
        cJSON_AddBoolToObject(r, "enabled", s_rules[i].enabled);
        cJSON_AddNumberToObject(r, "trigger", s_rules[i].trig);
        cJSON_AddNumberToObject(r, "action", s_rules[i].act);
        cJSON_AddItemToArray(rules, r);
    }
    cJSON_AddItemToObject(s, "rules", rules);
    if (query_id && *query_id) cJSON_AddStringToObject(s, "queryId", query_id);
    if (cmd_id && *cmd_id) {
        cJSON_AddStringToObject(s, "cmdId", cmd_id);
        cJSON_AddStringToObject(s, "cmdResult", cmd_res ? cmd_res : "");
        cJSON_AddStringToObject(s, "cmdReason", cmd_reason ? cmd_reason : "");
    }
    cJSON_AddNumberToObject(s, "epoch", now_epoch());
    return s;
}

static void publish_resp(const char *req_id, alexa_result_t st,
                         const char *reason, const char *action_label,
                         const char *heard)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "requestId", req_id ? req_id : "");
    cJSON_AddStringToObject(o, "status", alexa_result_str(st));
    cJSON_AddStringToObject(o, "reason", reason ? reason : "");
    cJSON_AddItemToObject(o, "state",
        state_json(NULL, req_id, alexa_result_str(st), reason));
    char *j = cJSON_PrintUnformatted(o);
    if (j) { mqtt_pub(s_t_resp, j, 1, 0); free(j); }
    cJSON_Delete(o);

    char h[56];
    snprintf(h, sizeof(h), "%s", action_label ? action_label : "cmd");
    hist_push(h, st);
    banner_show(heard, action_label, st);
}

static void publish_shadow(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *st = cJSON_CreateObject();
    cJSON *rep = state_json(NULL, NULL, NULL, NULL);
    cJSON_AddItemToObject(st, "reported", rep);
    cJSON_AddItemToObject(o, "state", st);
    char *j = cJSON_PrintUnformatted(o);
    if (j) { mqtt_pub(s_t_shadow, j, 1, 0); free(j); }
    cJSON_Delete(o);

    /* mirror on plain state topic (routines without shadow support) */
    cJSON *p = state_json(NULL, NULL, NULL, NULL);
    char *k = cJSON_PrintUnformatted(p);
    if (k) { mqtt_pub(s_t_state, k, 0, 1); free(k); }
    cJSON_Delete(p);
}

static void publish_event(const char *type, const char *text)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", type);
    cJSON_AddStringToObject(o, "text", text);
    cJSON_AddNumberToObject(o, "epoch", now_epoch());
    cJSON_AddItemToObject(o, "state", state_json(NULL, NULL, NULL, NULL));
    char *j = cJSON_PrintUnformatted(o);
    if (j) { mqtt_pub(s_t_event, j, 1, 0); free(j); }
    cJSON_Delete(o);
    strncpy(s_last_alert, text, sizeof(s_last_alert) - 1);
}

/* MPU-advanced HARD impact announce (1.md 4.3: IMPACT always logged, HARD
   also triggers Alexa announce). Runs in car_task context via event bus. */
void alexa_impact_note(uint8_t level, float g, uint32_t now_ms)
{
    impact_note_set(level, g);
    s_impact_ms = now_ms;
    if (level >= 3) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Hard impact detected (%.1fg)", (double)g);
        publish_event("impact_hard", msg);
        alexa_announce("Attention - Azam Car detected a hard impact.", true);
    }
}

/* PART 9.5 proactive announcements with quiet-hours rule */
void alexa_announce(const char *text, bool critical)
{
    if (!text || !*text) return;
    if (!critical) {
        long ep = now_epoch();
        if (ep > 1700000000L) {   /* SNTP synced? else announce always */
            int hour = (int)((ep % 86400) / 3600);   /* UTC */
            if (hour >= 22 || hour < 7) {
                ESP_LOGI(TAG, "announce suppressed (quiet hours): %s", text);
                return;
            }
        }
    }
    publish_event("announce", text);
}

/* Dirty snapshot: meaningful state only (no heap/psram/pad_age/uptime/
 * live-cm/epoch). Idle car -> memcmp equal -> zero MQTT traffic. */
typedef struct {
    uint8_t mode, profile, gear;
    bool estop, moving, headlight, hazard, rear;
    uint8_t roof_mode, roof_bright, roof_color;
    bool roof_on;
    uint8_t tft_bright, theme;
    int vol, obst, cal;
    bool muted, pad, front_ok;
    uint32_t c_estop, c_pad, c_front, c_stuck, drive_min;
    uint8_t rules;
    char alert[64];
    /* PART 9.2: MPU/query fields in dirty set (rounded for traffic) */
    uint8_t traction, shield, terrain;
    bool tilt_lockout;
    uint8_t drive_score5;   /* score / 5 */
    uint8_t hard_bumps;     /* capped at 255 */
} snap_t;
static snap_t s_snap;
static bool s_snap_init = false;

static void snap_take(snap_t *s)
{
    const roof_light_state_t *rl = roof_light_state();
    memset(s, 0, sizeof(*s));
    s->mode = g.mode; s->profile = s_profile; s->gear = g.gear;
    s->estop = g.estop; s->moving = moving();
    s->headlight = g.headlight; s->hazard = g.hazard;
    s->rear = g.rear_light_on;
    s->roof_mode = (uint8_t)rl->mode;
    s->roof_bright = (uint8_t)rl->brightness;
    s->roof_color = rl->color_idx;
    s->roof_on = rl->enabled;
    os_ctx_t *octx = alexa_os_ctx();
    s->theme = octx ? (uint8_t)(octx->cockpit_theme % 3) : 1;
    s->tft_bright = s_tft_bright;
    s->vol = car_get_setting(1); s->obst = car_get_setting(2);
    s->cal = (int)imu_driver_cal_state();
    s->muted = g.snd_mute;
    s->pad = xbox360_dongle_connected();
    s->front_ok = car_us_front_ok();
    s->c_estop = s_c_estop; s->c_pad = s_c_pad;
    s->c_front = s_c_front; s->c_stuck = s_c_stuck;
    s->drive_min = s_last_drive_min;
    {   /* PART 9.2 MPU dirty fields (score rounded to /5 for traffic) */
        imu_adv_snap_t adv;
        imu_adv_snapshot(&adv);
        s->traction = (uint8_t)adv.traction;
        s->shield = (uint8_t)adv.shield;
        s->terrain = (uint8_t)adv.terrain;
        s->tilt_lockout = adv.tilt_lockout;
        s->drive_score5 = (uint8_t)(adv.drive_score / 5);
        uint32_t hb = adv.impact_hard_cnt;
        s->hard_bumps = (uint8_t)(hb > 255 ? 255 : hb);
    }
    for (int i = 0; i < RULE_MAX; i++)
        if (s_rules[i].valid)
            s->rules |= (uint8_t)((1u << i) |
                          (s_rules[i].enabled ? (1u << (i + 4)) : 0));
    strncpy(s->alert, s_last_alert, sizeof(s->alert) - 1);
}

/* Compare only (does NOT store): dirty stays visible until published. */
static bool state_dirty(void)
{
    snap_t cur;
    snap_take(&cur);
    return !s_snap_init || memcmp(&cur, &s_snap, sizeof(cur)) != 0;
}

/* Store current as last-published. Call right after publish_shadow(). */
static void state_snap(void)
{
    snap_take(&s_snap);
    s_snap_init = true;
}

/* ------------------------- command vocabulary --------------------------- */
static const char *READONLY_CMDS[] = {
    "STATUS", "ZONE", "MODE_Q", "MOTION_Q", "SENSOR_HEALTH", "LINK_Q",
    "LAST_ALERT", "DAY_REPORT", "MAINTENANCE", "MEM_STATUS", "SESSION_Q",
    "LIST_RULES", NULL
};

/* Explicitly dangerous names -> LOCKED (double layer with Lambda).
   PART 9.2: motor/estop/lockout clears can NEVER pass - Alexa never
   bypasses the car-side Safety Gateway (imu_adv_ack_rollover has no
   alexa path at all). */
static const char *BLOCKED_SUB[] = {
    "MOVE", "DRIVE", "THROTTLE", "STEER", "MOTOR", "SPARK", "MIST",
    "REBOOT", "RESTART", "RESET", "FACTORY", "ESTOP_CLEAR", "CLEAR_ESTOP",
    "SAFETY_OFF", "DISABLE_SAFETY",
    "TILT_CRITICAL", "ROLLOVER", "LOCKOUT", "CLEAR_LOCKOUT", "ACK_ROLLOVER",
    NULL
};

static bool is_readonly(const char *cmd)
{
    for (int i = 0; READONLY_CMDS[i]; i++)
        if (strcmp(cmd, READONLY_CMDS[i]) == 0) return true;
    return false;
}

static bool is_blocked(const char *cmd)
{
    for (int i = 0; BLOCKED_SUB[i]; i++)
        if (strstr(cmd, BLOCKED_SUB[i]) != NULL) return true;
    return false;
}

/* Confirm-gated: NEVER execute from MQTT; physical A/B only (blueprint §E) */
static bool needs_confirm(const char *cmd)
{
    return strcmp(cmd, "SET_AUTO") == 0 || strcmp(cmd, "SET_GEAR") == 0 ||
           strcmp(cmd, "GEAR_SET") == 0 ||   /* PART 9.2 alias, stationary-only */
           strcmp(cmd, "CALIBRATE") == 0 || strcmp(cmd, "START_GAME") == 0 ||
           strcmp(cmd, "SAVE_SETTINGS") == 0 || strcmp(cmd, "RULE_DELETE") == 0;
}

static const char *confirm_text(const char *cmd, cJSON *args)
{
    static char t[40];
    if (strcmp(cmd, "SET_AUTO") == 0)      snprintf(t, sizeof(t), "AUTO MODE");
    else if (strcmp(cmd, "SET_GEAR") == 0 || strcmp(cmd, "GEAR_SET") == 0)
        snprintf(t, sizeof(t), "GEAR %d",
        args ? (int)cJSON_GetNumberValue(
            cJSON_GetObjectItemCaseSensitive(args, "gear")) : 0);
    else if (strcmp(cmd, "CALIBRATE") == 0)     snprintf(t, sizeof(t), "GYRO CALIBRATION");
    else if (strcmp(cmd, "START_GAME") == 0)    snprintf(t, sizeof(t), "START GAME");
    else if (strcmp(cmd, "SAVE_SETTINGS") == 0) snprintf(t, sizeof(t), "SAVE SETTINGS");
    else if (strcmp(cmd, "RULE_DELETE") == 0)   snprintf(t, sizeof(t), "DELETE RULES");
    else snprintf(t, sizeof(t), "ALEXA REQUEST");
    return t;
}

/* Map validated action -> pending slot kind. Returns false if unknown. */
static bool map_action(const char *cmd, cJSON *args, uint8_t *kind,
                       int32_t *iarg, char *sarg)
{
    *iarg = 0; sarg[0] = 0;
    cJSON *a = args;
    int bval = 0;
    if (a) {
        cJSON *on = cJSON_GetObjectItemCaseSensitive(a, "on");
        if (cJSON_IsBool(on)) bval = cJSON_IsTrue(on);
        else if (cJSON_IsNumber(on)) bval = (on->valueint != 0);
    }
    if (strcmp(cmd, "SET_HEADLIGHT") == 0)      { *kind = AA_HEADLIGHT; *iarg = bval; }
    else if (strcmp(cmd, "SET_HAZARD") == 0)    { *kind = AA_HAZARD; *iarg = bval; }
    else if (strcmp(cmd, "SET_REAR") == 0)      { *kind = AA_REAR; *iarg = bval; }
    else if (strcmp(cmd, "SET_ROOF_COLOR") == 0) {
        *kind = AA_ROOF_COLOR;
        cJSON *c = a ? cJSON_GetObjectItemCaseSensitive(a, "color") : NULL;
        strncpy(sarg, (c && cJSON_IsString(c)) ? c->valuestring : "blue",
                23);
    } else if (strcmp(cmd, "SET_ROOF_MODE") == 0) {
        *kind = AA_ROOF_MODE;
        cJSON *m = a ? cJSON_GetObjectItemCaseSensitive(a, "mode") : NULL;
        strncpy(sarg, (m && cJSON_IsString(m)) ? m->valuestring : "steady",
                23);
    } else if (strcmp(cmd, "SET_ROOF_BRIGHT") == 0) {
        *kind = AA_ROOF_BRIGHT;
        cJSON *p = a ? cJSON_GetObjectItemCaseSensitive(a, "pct") : NULL;
        *iarg = p ? (int)cJSON_GetNumberValue(p) : 50;
        if (*iarg < 0) *iarg = 0;
        if (*iarg > 100) *iarg = 100;
    } else if (strcmp(cmd, "ALL_LIGHTS_OFF") == 0) { *kind = AA_ALL_OFF; }
    else if (strcmp(cmd, "WARN_BURST") == 0)    { *kind = AA_WARN; }
    else if (strcmp(cmd, "OPEN_SCREEN") == 0) {
        *kind = AA_SCREEN;
        cJSON *s = a ? cJSON_GetObjectItemCaseSensitive(a, "screen") : NULL;
        strncpy(sarg, (s && cJSON_IsString(s)) ? s->valuestring : "drive",
                23);
    } else if (strcmp(cmd, "SET_TFT_BRIGHT") == 0) {
        *kind = AA_TFT_BRIGHT;
        cJSON *p = a ? cJSON_GetObjectItemCaseSensitive(a, "pct") : NULL;
        *iarg = p ? (int)cJSON_GetNumberValue(p) : 80;
        if (*iarg < 5) *iarg = 5;
        if (*iarg > 100) *iarg = 100;
    } else if (strcmp(cmd, "SET_THEME") == 0 || strcmp(cmd, "THEME_SET") == 0) {
        *kind = AA_THEME;
        cJSON *t = a ? cJSON_GetObjectItemCaseSensitive(a, "theme") : NULL;
        strncpy(sarg, (t && cJSON_IsString(t)) ? t->valuestring : "day",
                23);
    } else if (strcmp(cmd, "RADAR_OPEN") == 0) {
        *kind = AA_SCREEN;
        snprintf(sarg, 24, "radar");
    } else if (strcmp(cmd, "HOME_OPEN") == 0) {
        *kind = AA_SCREEN;
        snprintf(sarg, 24, "home");
    } else if (strcmp(cmd, "SCENE_SET") == 0) {
        *kind = AA_SCENE;
        cJSON *s = a ? cJSON_GetObjectItemCaseSensitive(a, "scene") : NULL;
        strncpy(sarg, (s && cJSON_IsString(s)) ? s->valuestring : "",
                23);
    } else if (strcmp(cmd, "SET_MUTE") == 0)   { *kind = AA_MUTE; *iarg = bval; }
    else if (strcmp(cmd, "SET_VOLUME") == 0) {
        *kind = AA_VOLUME;
        cJSON *p = a ? cJSON_GetObjectItemCaseSensitive(a, "pct") : NULL;
        *iarg = p ? (int)cJSON_GetNumberValue(p) : 50;
        if (*iarg < 0) *iarg = 0;
        if (*iarg > 100) *iarg = 100;
    } else if (strcmp(cmd, "SET_PROFILE") == 0) {
        *kind = AA_PROFILE;
        cJSON *p = a ? cJSON_GetObjectItemCaseSensitive(a, "profile") : NULL;
        strncpy(sarg, (p && cJSON_IsString(p)) ? p->valuestring : "safe",
                23);
    } else {
        return false;
    }
    return true;
}

/* --------------------------- inbox serving ------------------------------ */
static void serve_command(const char *payload)
{
    cJSON *o = cJSON_Parse(payload);
    if (!o) { ESP_LOGW(TAG, "bad json len=%d head=%.64s", (int)strlen(payload), payload); return; }

    cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(o, "cmd");
    const char *cmd = (jcmd && cJSON_IsString(jcmd)) ? jcmd->valuestring : "";
    cJSON *args = cJSON_GetObjectItemCaseSensitive(o, "args");
    cJSON *juid = cJSON_GetObjectItemCaseSensitive(o, "userId");
    cJSON *jrid = cJSON_GetObjectItemCaseSensitive(o, "requestId");
    cJSON *jheard = cJSON_GetObjectItemCaseSensitive(o, "heard");
    cJSON *jts = cJSON_GetObjectItemCaseSensitive(o, "timestamp");
    const char *uid = (juid && cJSON_IsString(juid)) ? juid->valuestring : "";
    const char *rid = (jrid && cJSON_IsString(jrid)) ? jrid->valuestring : "";
    const char *heard = (jheard && cJSON_IsString(jheard)) ? jheard->valuestring : cmd;
    long ts = jts ? (long)cJSON_GetNumberValue(jts) : 0;

    /* 1. identity */
    if (strcmp(uid, s_user) != 0) { cJSON_Delete(o); return; }
    /* 2. TTL 5 s (only when clock valid) */
    long ep = now_epoch();
    if (ep > 0 && ts > 0 && (ep - ts) > 5) {
        publish_resp(rid, ALEXA_RES_REJECTED, "EXPIRED", cmd, heard);
        cJSON_Delete(o);
        return;
    }
    /* 3. replay dedupe */
    if (dedup_seen(rid)) {
        publish_resp(rid, ALEXA_RES_REJECTED, "DUPLICATE", cmd, heard);
        cJSON_Delete(o);
        return;
    }
    /* 4. blocked forever (double layer with Lambda) */
    if (is_blocked(cmd)) {
        ESP_LOGW(TAG, "blocked cmd %s", cmd);
        publish_resp(rid, ALEXA_RES_REJECTED, "LOCKED", cmd, heard);
        cJSON_Delete(o);
        return;
    }

    /* read-only queries: answer immediately (safe even in E-stop) */
    if (is_readonly(cmd)) {
        publish_resp(rid, ALEXA_RES_DONE, "", cmd, heard);
        cJSON_Delete(o);
        return;
    }

    /* 5. E-stop blocks every action */
    if (g.estop) {
        publish_resp(rid, ALEXA_RES_REJECTED, "ESTOP", cmd, heard);
        cJSON_Delete(o);
        return;
    }

    /* 6. confirm-gated: dialog on car, NEVER direct execute */
    if (needs_confirm(cmd)) {
        if (moving() && strcmp(cmd, "SAVE_SETTINGS") != 0) {
            publish_resp(rid, ALEXA_RES_REJECTED, "CAR_MOVING", cmd, heard);
        } else {
            portENTER_CRITICAL(&s_mux);
            s_conf.active = true;
            strncpy(s_conf.text, confirm_text(cmd, args), sizeof(s_conf.text) - 1);
            s_conf.text[sizeof(s_conf.text) - 1] = 0;     /* FIX: guaranteed NUL */
            /* stash ints now (args freed below) */
            if (strcmp(cmd, "SET_GEAR") == 0 || strcmp(cmd, "GEAR_SET") == 0) {
                cJSON *gg = args ? cJSON_GetObjectItemCaseSensitive(args, "gear") : NULL;
                int gv = gg ? (int)cJSON_GetNumberValue(gg) : 1;
                if (gv < 1) gv = 1;
                if (gv > 5) gv = 5;
                s_conf.kind = AA_NONE; s_conf.iarg = gv;  /* gear handled inline */
                strncpy(s_conf.sarg, "GEAR", sizeof(s_conf.sarg) - 1);
            } else if (strcmp(cmd, "SET_AUTO") == 0) {
                strncpy(s_conf.sarg, "AUTO", sizeof(s_conf.sarg) - 1);
            } else if (strcmp(cmd, "CALIBRATE") == 0) {
                strncpy(s_conf.sarg, "CAL", sizeof(s_conf.sarg) - 1);
            } else if (strcmp(cmd, "START_GAME") == 0) {
                cJSON *gi = args ? cJSON_GetObjectItemCaseSensitive(args, "game") : NULL;
                int idx = gi ? (int)cJSON_GetNumberValue(gi) : 0;
                if (idx < 0) idx = 0;
                if (idx > 1) idx = 1;
                s_conf.iarg = idx;
                strncpy(s_conf.sarg, "GAME", sizeof(s_conf.sarg) - 1);
            } else {  /* SAVE_SETTINGS / RULE_DELETE */
                strncpy(s_conf.sarg, cmd, sizeof(s_conf.sarg) - 1);
            }
            s_conf.sarg[sizeof(s_conf.sarg) - 1] = 0;      /* FIX: guaranteed NUL */
            strncpy(s_conf.req_id, rid, sizeof(s_conf.req_id) - 1);
            s_conf.req_id[sizeof(s_conf.req_id) - 1] = 0;  /* FIX: guaranteed NUL */
            s_conf.expiry_ms = now_ms() + 30000;
            portEXIT_CRITICAL(&s_mux);
            publish_resp(rid, ALEXA_RES_NEEDS_CONFIRM, "CONFIRM_ON_CAR",
                         cmd, heard);
        }
        cJSON_Delete(o);
        return;
    }

    /* rules management (MAKE/RULE_ENABLE/RULE_DELETE-all is direct) */
    if (strcmp(cmd, "MAKE_RULE") == 0 || strcmp(cmd, "RULE_ENABLE") == 0) {
        const char *rr = "UNKNOWN";
        if (strcmp(cmd, "MAKE_RULE") == 0) {
            cJSON *tr = args ? cJSON_GetObjectItemCaseSensitive(args, "trigger") : NULL;
            cJSON *ac = args ? cJSON_GetObjectItemCaseSensitive(args, "action") : NULL;
            const char *ts2 = (tr && cJSON_IsString(tr)) ? tr->valuestring : "";
            const char *as2 = (ac && cJSON_IsString(ac)) ? ac->valuestring : "";
            uint8_t ti = (strstr(ts2, "idle") != NULL) ? 1 : 0;
            uint8_t ai = (strstr(as2, "dim") != NULL) ? 1 : 0;
            int slot = -1;
            for (int i = 0; i < RULE_MAX; i++)
                if (!s_rules[i].valid) { slot = i; break; }
            if (slot >= 0) {
                s_rules[slot].valid = true;
                s_rules[slot].enabled = true;
                s_rules[slot].trig = ti;
                s_rules[slot].act = ai;
                s_rule_lat[slot] = false;
                rules_save();
                rr = "";
            } else rr = "RULES_FULL";
        } else {
            cJSON *ji = args ? cJSON_GetObjectItemCaseSensitive(args, "id") : NULL;
            int id = ji ? (int)cJSON_GetNumberValue(ji) : -1;
            cJSON *on = args ? cJSON_GetObjectItemCaseSensitive(args, "on") : NULL;
            bool en = on ? (cJSON_IsTrue(on) || (cJSON_IsNumber(on) && on->valueint)) : true;
            if (id >= 0 && id < RULE_MAX && s_rules[id].valid) {
                s_rules[id].enabled = en;
                s_rule_lat[id] = false;
                rules_save();
                rr = "";
            } else rr = "NO_SUCH_RULE";
        }
        publish_resp(rid, rr[0] ? ALEXA_RES_REJECTED : ALEXA_RES_DONE,
                     rr[0] ? rr : "", cmd, heard);
        cJSON_Delete(o);
        return;
    }

    /* validated action -> pending slot for car_task */
    uint8_t kind = AA_NONE;
    int32_t iarg = 0;
    char sarg[24] = {0};
    if (!map_action(cmd, args, &kind, &iarg, sarg)) {
        publish_resp(rid, ALEXA_RES_REJECTED, "UNKNOWN", cmd, heard);
        cJSON_Delete(o);
        return;
    }
    /* parked/moving rule: games screen + auto need a stationary car */
    if (moving() && ((kind == AA_SCREEN &&
         (strcmp(sarg, "games") == 0)) || strcmp(cmd, "SET_AUTO") == 0)) {
        publish_resp(rid, ALEXA_RES_REJECTED, "CAR_MOVING", cmd, heard);
        cJSON_Delete(o);
        return;
    }
    if (s_pend.busy) {
        publish_resp(rid, ALEXA_RES_REJECTED, "BUSY", cmd, heard);
        cJSON_Delete(o);
        return;
    }
    s_pend.kind = kind;
    s_pend.iarg = iarg;
    strncpy(s_pend.sarg, sarg, sizeof(s_pend.sarg) - 1);
    s_pend.sarg[sizeof(s_pend.sarg) - 1] = 0;        /* FIX: guaranteed NUL */
    strncpy(s_pend.req_id, rid, sizeof(s_pend.req_id) - 1);
    s_pend.req_id[sizeof(s_pend.req_id) - 1] = 0;    /* FIX: guaranteed NUL */
    s_pend.busy = true;          /* publish last */
    banner_show(heard, cmd, ALEXA_RES_PROCESSING);
    cJSON_Delete(o);
}

/* --------------------- car_task: apply pending -------------------------- */
static roof_light_mode_t roof_mode_from(const char *m)
{
    if (strcmp(m, "police") == 0)    return ROOF_LIGHT_POLICE;
    if (strcmp(m, "steady") == 0)    return ROOF_LIGHT_STEADY;
    if (strcmp(m, "rainbow") == 0)   return ROOF_LIGHT_RAINBOW;
    if (strcmp(m, "breathing") == 0) return ROOF_LIGHT_BREATHE;
    if (strcmp(m, "breathe") == 0)   return ROOF_LIGHT_BREATHE;
    if (strcmp(m, "warning") == 0)   return ROOF_LIGHT_WARNING;
    if (strcmp(m, "chase") == 0)     return ROOF_LIGHT_CHASE;
    if (strcmp(m, "speed") == 0)     return ROOF_LIGHT_CHASE;
    if (strcmp(m, "music") == 0)     return ROOF_LIGHT_MUSIC;
    if (strcmp(m, "off") == 0)       return ROOF_LIGHT_OFF;
    return ROOF_LIGHT_STEADY;
}

static uint8_t roof_color_idx_from(const char *c)
{
    if (strcmp(c, "red") == 0)    return 0;
    if (strcmp(c, "blue") == 0)   return 1;
    if (strcmp(c, "green") == 0)  return 2;
    if (strcmp(c, "cyan") == 0)   return 3;
    if (strcmp(c, "yellow") == 0) return 4;
    if (strcmp(c, "white") == 0)  return 5;
    return 0;
}

static int screen_from(const char *s)
{
    if (strcmp(s, "radar") == 0 || strcmp(s, "drive") == 0) return OS_DRIVE_MAIN;
    if (strcmp(s, "diagnostics") == 0) return OS_DIAG;
    if (strcmp(s, "games") == 0)      return OS_GAMES_HUB;
    if (strcmp(s, "voice_history") == 0) return OS_ALEXA_LOG;
    if (strcmp(s, "settings") == 0)   return OS_SETTINGS;
    if (strcmp(s, "home") == 0)       return OS_HOME;
    return -1;
}

static int profile_from(const char *p)
{
    if (strcmp(p, "park") == 0)        return 0;
    if (strcmp(p, "safe") == 0)        return 1;
    if (strcmp(p, "night") == 0)       return 2;
    if (strcmp(p, "demo") == 0)        return 3;
    if (strcmp(p, "performance") == 0) return 4;
    if (strcmp(p, "silent") == 0)      return 5;
    if (strcmp(p, "game") == 0)        return 6;
    return -1;
}

static void profile_save(uint8_t p)
{
    if (s_profile != p) {
        /* PART 5: personality sting on profile change (Attractiveness P9) */
        static const char *chimes[] = {
            "profile_chime_park", "profile_chime_safe", "profile_chime_night",
            "profile_chime_demo", "profile_chime_perf",
            "profile_chime_silent", "profile_chime_game", NULL,
        };
        if (p < 7 && chimes[p]) snd_play(chimes[p]);
    }
    s_profile = p;
    nvs_handle_t h;
    if (nvs_open("alexa", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "profile", p);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* PART 14: lifetime odometer meters (NVS, accumulates per drive-end).
   Cached 2s: TRIP window draws at 30fps, NVS open per frame is wasteful. */
uint32_t alexa_odo_m(void)
{
    static uint32_t s_cache, s_cache_ms;
    uint32_t now = now_ms();
    if (s_cache_ms && now - s_cache_ms < 2000) return s_cache;
    s_cache_ms = now;
    nvs_handle_t h;
    uint32_t odo = 0;
    if (nvs_open("alexa", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "odo_m", &odo);
        nvs_close(h);
    }
    s_cache = odo;
    return odo;
}

/* PART 10 QCC profile dropdown: inject a validated profile action into the
   pending slot (executes in car_task via alexa_apply_pending, WITH NVS +
   chime - a profile choice should stick). Safe from os_handle_input. */
bool alexa_request_profile(uint8_t p)
{
    if (p > 6 || s_pend.busy) return false;
    if (p == 6 && moving()) return false;   /* game profile needs stationary car */
    snprintf(s_pend.sarg, sizeof(s_pend.sarg), "%s", profile_names[p]);
    snprintf(s_pend.req_id, sizeof(s_pend.req_id), "QCC");
    s_pend.kind = AA_PROFILE;
    s_pend.iarg = 0;
    s_pend.busy = true;
    return true;
}

/* Execute ONE validated action. Runs in car_task (owns g/ctx). */
static void apply_action(os_ctx_t *ctx, uint8_t kind, int32_t iarg,
                         const char *sarg, const char *req_id)
{
    char reason[32] = {0};
    bool ok = true;

    switch (kind) {
    case AA_HEADLIGHT: g.headlight = (iarg != 0); break;
    case AA_HAZARD:    g.hazard = (iarg != 0); break;
    case AA_REAR:      g.rear_light_on = (iarg != 0); break;
    case AA_ROOF_COLOR:
        roof_light_set_color_idx(roof_color_idx_from(sarg));
        roof_light_set_mode(ROOF_LIGHT_STEADY);
        roof_light_set_enabled(true);
        break;
    case AA_ROOF_MODE:
        if (strcmp(sarg, "off") == 0) roof_light_set_enabled(false);
        else {
            roof_light_set_mode(roof_mode_from(sarg));
            roof_light_set_enabled(true);
        }
        break;
    case AA_ROOF_BRIGHT:
        roof_light_set_brightness((uint8_t)iarg);
        roof_light_set_enabled(iarg > 0);
        break;
    case AA_ALL_OFF:
        g.headlight = false; g.hazard = false; g.rear_light_on = false;
        roof_light_set_enabled(false);
        break;
    case AA_WARN:
        g.hazard = true;
        s_warn_active = true;
        s_warn_until = now_ms() + 3000;
        break;
    case AA_SCREEN: {
        int st = screen_from(sarg);
        if (st < 0) { ok = false; strncpy(reason, "BAD_SCREEN", 31); break; }
        if (st == OS_GAMES_HUB && moving()) {
            ok = false; strncpy(reason, "CAR_MOVING", 31); break;
        }
        if (st == OS_DRIVE_MAIN) {
            ctx->drive_sub_view = 0; ctx->radar_view = 0;
        }
        os_request_screen(ctx, st, now_ms());
        break;
    }
    case AA_TFT_BRIGHT:
        s_tft_bright = (uint8_t)iarg;
        tft_set_brightness((uint8_t)iarg);
        break;
    case AA_THEME:
        /* PART 9.2 THEME_SET: cockpit palette LUT + legacy hud layout */
        if (strcmp(sarg, "night") == 0) {
            ctx->cockpit_theme = 0; ctx->hud_layout = 2;
        } else if (strcmp(sarg, "boost") == 0 || strcmp(sarg, "sun") == 0) {
            ctx->cockpit_theme = 2; ctx->hud_layout = 0;
        } else {
            ctx->cockpit_theme = 1; ctx->hud_layout = 0;
        }
        break;
    case AA_MUTE:   g.snd_mute = (iarg != 0); break;
    case AA_VOLUME:
        car_set_setting(1, (uint8_t)iarg);
        if (iarg > 0) g.snd_mute = false;
        break;
    case AA_PROFILE: {
        int p = profile_from(sarg);
        if (p < 0) { ok = false; strncpy(reason, "BAD_PROFILE", 31); break; }
        if (p == 6 && moving()) {
            ok = false; strncpy(reason, "CAR_MOVING", 31); break;
        }
        switch (p) {
    case 0:  /* park */
            os_request_screen(ctx, OS_HOME, now_ms());
            g.headlight = false; g.hazard = false;
            roof_light_set_mode(ROOF_LIGHT_POLICE);
            roof_light_set_brightness(40);
            roof_light_set_enabled(true);
            s_tft_bright = 50; tft_set_brightness(50);
            g.snd_mute = true;
            speech_set("Parked. Systems resting.");
            break;
        case 1:  /* safe */
            car_set_setting(0, 40); car_set_setting(2, 30);
            g.headlight = true;
            roof_light_set_mode(ROOF_LIGHT_STEADY);
            roof_light_set_brightness(60);
            roof_light_set_enabled(true);
            s_tft_bright = 80; tft_set_brightness(80);
            g.snd_mute = false;
            speech_set("Safe profile. Taking it easy.");
            break;
        case 2:  /* night */
            g.headlight = true;
            s_tft_bright = 30; tft_set_brightness(30);
            ctx->hud_layout = 2;
            ctx->cockpit_theme = 0;
            roof_light_set_mode(ROOF_LIGHT_STEADY);
            roof_light_set_brightness(25);
            roof_light_set_enabled(true);
            car_set_setting(1, 30);
            speech_set("Night Drive activated. Lights out, systems quiet.");
            break;
        case 3:  /* demo */
            roof_light_set_mode(ROOF_LIGHT_RAINBOW);
            roof_light_set_brightness(100);
            roof_light_set_enabled(true);
            ctx->drive_sub_view = 0; ctx->radar_view = 0;
            os_request_screen(ctx, OS_DRIVE_MAIN, now_ms());
            speech_set("Demo mode. Watch this.");
            break;
        case 4:  /* performance */
            car_set_setting(0, 100);
            g.gear = 4; ctx->gear = 4;
            roof_light_set_mode(ROOF_LIGHT_STEADY);
            roof_light_set_brightness(100);
            roof_light_set_enabled(true);
            car_set_setting(1, 80); g.snd_mute = false;
            speech_set("Performance unlocked. Hold on.");
            break;
        case 5:  /* silent */
            g.snd_mute = true;
            g.headlight = false; g.hazard = false;
            roof_light_set_enabled(false);
            speech_set("Silent running.");
            break;
        case 6:  /* game */
            os_request_screen(ctx, OS_GAMES_HUB, now_ms());
            speech_set("Game time. Have fun.");
            break;
        }
        car_settings_save();
        profile_save((uint8_t)p);
        break;
    }
    case AA_SCENE: {   /* PART 9.6 ambient scene bundles */
        if (strcmp(sarg, "midnight_cruise") == 0) {
            roof_light_set_mode(ROOF_LIGHT_BREATHE);
            roof_light_set_color_idx(1);   /* blue */
            roof_light_set_brightness(60);
            roof_light_set_enabled(true);
            g.headlight = true;
            car_set_setting(1, 40);
            ctx->cockpit_theme = 0;        /* night */
            speech_set("Midnight Cruise set. Easy riding.");
        } else if (strcmp(sarg, "party_mode") == 0) {
            /* roof MUSIC (mic-reactive show) + under-car chase + full volume */
            roof_light_set_mode(ROOF_LIGHT_MUSIC);
            roof_light_set_brightness(100);
            roof_light_set_enabled(true);
            g.led_mode = 3;                /* under-car chase */
            car_set_setting(1, 100); g.snd_mute = false;
            g.headlight = true;
            speech_set("Party mode on. Let's move.");
        } else { ok = false; strncpy(reason, "BAD_SCENE", 31); break; }
        break;
    }
    default: ok = false; strncpy(reason, "UNKNOWN", 31); break;
    }

    result_set(req_id, ok ? ALEXA_RES_DONE : ALEXA_RES_REJECTED,
               ok ? "" : reason);
}

void alexa_apply_pending(os_ctx_t *ctx, uint32_t now)
{
    (void)now;
    if (!s_pend.busy) return;
    uint8_t kind = s_pend.kind;
    int32_t iarg = s_pend.iarg;
    /* FIX 2026-09-17: pehle strncpy() bina guaranteed NUL-termination ke stack
       buffers me copy karta tha -> agar source 23/39 chars ka ho to strcmp()
       OOB read karta tha. Ab snprintf (always terminated, truncating). */
    char sarg[24], req[40];
    snprintf(sarg, sizeof(sarg), "%s", s_pend.sarg);
    snprintf(req, sizeof(req), "%s", s_pend.req_id);
    s_pend.busy = false;
    if (kind == AA_NONE) {
        result_set(req, ALEXA_RES_REJECTED, "UNKNOWN");
        return;
    }
    apply_action(ctx, kind, iarg, sarg, req);
}

/* ------------------- car_task: confirm A/B input ------------------------ */
bool alexa_confirm_handle_input(uint16_t dig, uint16_t tap, uint32_t now)
{
    (void)dig;
    bool have;
    portENTER_CRITICAL(&s_mux);
    have = s_conf.active;
    portEXIT_CRITICAL(&s_mux);
    if (!have) return false;

    /* expiry check */
    portENTER_CRITICAL(&s_mux);
    bool expired = (int32_t)(now - s_conf.expiry_ms) >= 0;
    if (expired) {
        char req[40];
        snprintf(req, sizeof(req), "%s", s_conf.req_id);
        s_conf.active = false;
        portEXIT_CRITICAL(&s_mux);
        result_set(req, ALEXA_RES_CANCELLED, "TIMEOUT");
        return false;   /* let normal input through */
    }
    bool do_a = (tap & B_A) != 0;
    bool do_b = (tap & B_B) != 0;
    if (!do_a && !do_b) { portEXIT_CRITICAL(&s_mux); return false; }

    /* consume: copy + clear under lock, execute outside */
    char kind_s[24], req[40];
    int32_t iarg = s_conf.iarg;
    snprintf(kind_s, sizeof(kind_s), "%s", s_conf.sarg);
    snprintf(req, sizeof(req), "%s", s_conf.req_id);
    s_conf.active = false;
    portEXIT_CRITICAL(&s_mux);

    extern os_ctx_t *alexa_os_ctx(void);  /* car_os exposes current ctx */
    if (do_b) {
        result_set(req, ALEXA_RES_CANCELLED, "USER_CANCEL");
        return true;
    }
    /* A = confirm: execute now (we ARE car_task via os_handle_input) */
    os_ctx_t *ctx = alexa_os_ctx();
    if (!ctx) { result_set(req, ALEXA_RES_REJECTED, "NO_CTX"); return true; }
    if (g.estop || moving()) {
        result_set(req, ALEXA_RES_REJECTED,
                   g.estop ? "ESTOP" : "CAR_MOVING");
        return true;
    }
    if (strcmp(kind_s, "AUTO") == 0) {
        g.mode = MODE_AUTO;
        result_set(req, ALEXA_RES_DONE, "");
    } else if (strcmp(kind_s, "GEAR") == 0) {
        g.gear = (uint8_t)(iarg - 1); ctx->gear = (uint8_t)(iarg - 1);
        result_set(req, ALEXA_RES_DONE, "");
    } else if (strcmp(kind_s, "CAL") == 0) {
        imu_driver_cal_start();
        result_set(req, ALEXA_RES_DONE, "");
    } else if (strcmp(kind_s, "GAME") == 0) {
       extern void os_request_screen(os_ctx_t *c, int st, uint32_t now);
        os_request_screen(ctx, OS_GAME_1 + (int)iarg, now);
        result_set(req, ALEXA_RES_DONE, "");
    } else if (strcmp(kind_s, "SAVE_SETTINGS") == 0) {
        extern void os_alexa_save_settings(os_ctx_t *c);
        os_alexa_save_settings(ctx);
        result_set(req, ALEXA_RES_DONE, "");
    } else if (strcmp(kind_s, "RULE_DELETE") == 0) {
        for (int i = 0; i < RULE_MAX; i++) {
            s_rules[i].valid = false; s_rule_lat[i] = false;
        }
        rules_save();
        result_set(req, ALEXA_RES_DONE, "");
    } else {
        result_set(req, ALEXA_RES_REJECTED, "UNKNOWN");
    }
    return true;
}

/* ------------------------------ TFT ------------------------------------- */
void alexa_banner_draw(void)
{
    /* PART 5: notify sting on banner rising edge */
    static bool s_was_banner = false;
    bool show = s_banner.active &&
                (int32_t)(now_ms() - s_banner.until_ms) < 0;
    if (show && !s_was_banner) {
        snd_play("alexa_notify");
        /* PART 9.3: voice command -> banner VISIBLE latency (target <500ms) */
        if (s_last_rx_ms)
            ESP_LOGI(TAG, "voice->visible %lums%s",
                     (unsigned long)(now_ms() - s_last_rx_ms),
                     (now_ms() - s_last_rx_ms > 500) ? " OVER BUDGET" : "");
    }
    s_was_banner = show;
    if (!s_banner.active) return;
    if ((int32_t)(now_ms() - s_banner.until_ms) >= 0) {
        s_banner.active = false;
        return;
    }
    uint16_t badge = UI_WARNING;
    const char *bl = "...";
    switch ((alexa_result_t)s_banner.res) {
    case ALEXA_RES_DONE:          badge = UI_OK;      bl = "DONE"; break;
    case ALEXA_RES_REJECTED:      badge = UI_DANGER;  bl = "REJECTED"; break;
    case ALEXA_RES_NEEDS_CONFIRM: badge = UI_WARNING; bl = "CONFIRM ON CAR"; break;
    case ALEXA_RES_CANCELLED:     badge = UI_TEXT_2;  bl = "CANCELLED"; break;
    default:                      badge = UI_DATA;    bl = "..."; break;
    }
    gfx_rect(8, 24, UI_W - 16, 66, UI_BORDER);
    gfx_rect(10, 26, UI_W - 20, 62, UI_SURFACE);
    gfx_text_center_box(10, 30, UI_W - 20, "ALEXA", UI_FONT_SMALL, UI_ACCENT);
    char hb[70];
    snprintf(hb, sizeof(hb), "\"%.52s\"", s_banner.heard);
    gfx_text_center_box(10, 44, UI_W - 20, hb, UI_FONT_SMALL, UI_TEXT);
    char ab[64];
    snprintf(ab, sizeof(ab), "%.30s  [%s]", s_banner.action, bl);
    gfx_text_center_box(10, 60, UI_W - 20, ab, UI_FONT_SMALL, badge);
}

bool alexa_overlay_active(void)
{
    if (s_banner.active &&
        (int32_t)(now_ms() - s_banner.until_ms) < 0) return true;
    bool c;
    portENTER_CRITICAL(&s_mux);
    c = s_conf.active;
    portEXIT_CRITICAL(&s_mux);
    return c;
}

void alexa_confirm_draw(void)
{
    bool have;
    portENTER_CRITICAL(&s_mux);
    have = s_conf.active;
    portEXIT_CRITICAL(&s_mux);
    if (!have) return;
    char text[40];
    portENTER_CRITICAL(&s_mux);
    snprintf(text, sizeof(text), "%s", s_conf.text);
    portEXIT_CRITICAL(&s_mux);
    const int W = 280, H = 104, X = (UI_W - W) / 2, Y = 60;
    gfx_rect(X, Y, W, H, UI_WARNING);
    gfx_rect(X + 2, Y + 2, W - 4, H - 4, UI_SURFACE);
    gfx_text_center_box(X, Y + 10, W, "ALEXA REQUEST", UI_FONT_MEDIUM, UI_WARNING);
    gfx_text_center_box(X, Y + 36, W, text, UI_FONT_MEDIUM, UI_TEXT);
    gfx_text_center_box(X, Y + 66, W, "A CONFIRM   B CANCEL",
                        UI_FONT_SMALL, UI_TEXT_2);
}

void os_scr_alexa_log(const os_ctx_t *ctx, uint32_t now)
{
    (void)ctx; (void)now;
    gfx_clear(UI_BG);
    ui_draw_topbar(ctx);
    gfx_text_small(10, 26, "VOICE HISTORY (ALEXA)", UI_ACCENT);
    uint16_t col;
    char line[64];
    int shown = 0;
    if (!s_hist) {
        gfx_text_center_box(0, 110, UI_W, "STARTING...",
                            UI_FONT_SMALL, UI_MUTED);
    }
    for (int i = 0; s_hist && i < s_hist_n && shown < 8; i++) {
        int idx = (s_hist_head - 1 - i + HIST_N * 2) % HIST_N;
        hist_t *e = &s_hist[idx];
        switch ((alexa_result_t)e->res) {
        case ALEXA_RES_DONE:          col = UI_OK;      break;
        case ALEXA_RES_REJECTED:      col = UI_DANGER;  break;
        case ALEXA_RES_NEEDS_CONFIRM: col = UI_WARNING; break;
        default:                      col = UI_TEXT_2;  break;
        }
        unsigned sec = e->t_ms / 1000;
        snprintf(line, sizeof(line), "%02u:%02u %.46s",
                 (sec / 60) % 100, sec % 60, e->text);
        gfx_text_small(10, 44 + shown * 18, line, col);
        shown++;
    }
    if (!shown && s_hist)
        gfx_text_center_box(0, 110, UI_W, "no alexa commands yet",
                            UI_FONT_SMALL, UI_MUTED);
    ui_draw_bottombar("B BACK", "ALEXA LOG");
}

/* ------------------------- bridge task proper --------------------------- */
static void mqtt_start(void)
{
    if (!s_broker[0]) return;   /* disabled */
    ESP_LOGI(TAG, "mqtt_start: broker=%s ca=%d cert=%d key=%d",
             s_broker, (int)(s_ca != NULL), (int)(s_cert != NULL), (int)(s_key != NULL));
    esp_mqtt_client_config_t cfg = {0};
    cfg.broker.address.uri = s_broker;
    cfg.credentials.client_id = s_thing;
    cfg.session.keepalive = 30;
    cfg.network.disable_auto_reconnect = false;
    /* Persistent session: broker queues QoS1 cmd while offline, delivers
     * on reconnect. TTL(5s)+dedupe in serve_command make redelivery safe. */
    cfg.session.disable_clean_session = true;
    bool tls = (strncmp(s_broker, "mqtts", 5) == 0 ||
                strncmp(s_broker, "ssl", 3) == 0);
    if (tls) {
        /* Always use cert bundle (includes intermediate CAs) for server verification */
        cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        if (s_cert) cfg.credentials.authentication.certificate = s_cert;
        if (s_key)  cfg.credentials.authentication.key = s_key;
    }
    s_mqtt = esp_mqtt_client_init(&cfg);
    if (!s_mqtt) { ESP_LOGE(TAG, "mqtt init fail"); return; }
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    esp_mqtt_client_start(s_mqtt);
}

/* edge-detect critical events -> event topic (Phase I, anti-spam 60 s) */
static void events_poll(void)
{
    static bool p_estop = false, p_pad = true, p_fault = false;
    static uint8_t p_stuck = 0;
    static uint32_t t_pad = 0, t_fault = 0, t_stuck = 0;
    static uint32_t last_estop_rep = 0;
    uint32_t now = now_ms();

    if (g.estop && !p_estop) {
        counter_bump("c_estop", &s_c_estop);
        publish_event("estop", "Emergency stop activated");
        last_estop_rep = now;
    } else if (g.estop && now - last_estop_rep > 300000) {
        publish_event("estop", "Emergency stop still active");
        last_estop_rep = now;
    }
    p_estop = g.estop;

    bool pad = xbox360_dongle_connected();
    if (!pad && p_pad && now - t_pad > 60000) {
        counter_bump("c_pad", &s_c_pad);
        publish_event("pad_lost", "Controller disconnected");
        t_pad = now;
    }
    p_pad = pad;

    bool fault = !car_us_front_ok();
    if (fault && !p_fault && now - t_fault > 60000) {
        publish_event("sensor_fault", "Front sensor fault");
        t_fault = now;
    }
    p_fault = fault;

    if (g.stuck_cnt > p_stuck && now - t_stuck > 60000) {
        counter_bump("c_stuck", &s_c_stuck);
        publish_event("stuck", "Car stuck, retries exhausted");
        t_stuck = now;
    }
    p_stuck = g.stuck_cnt;

    /* front-blocked-while-moving edge -> front stop counter */
    bool blocked = moving() && g.dist_avg[1] != 65535 &&
                   g.dist_avg[1] < car_get_setting(2);
    static bool p_blocked = false;
    if (blocked && !p_blocked) counter_bump("c_front", &s_c_front);
    p_blocked = blocked;

    /* drive session length tracking */
    if (moving() && !s_moving_prev) { s_move_t0 = now; }
    if (!moving() && s_moving_prev && s_move_t0) {
        s_last_drive_min = (now - s_move_t0) / 60000;
        s_move_t0 = 0;
        /* PART 9.6: persist score at drive end for tomorrow's greeting.
           PART 14: lifetime odometer accumulates session distance. */
        {
            imu_adv_snap_t adv;
            imu_adv_snapshot(&adv);
            nvs_handle_t h;
            if (nvs_open("alexa", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u32(h, "last_score", adv.drive_score);
                uint32_t odo = 0;
                nvs_get_u32(h, "odo_m", &odo);
                odo += trip_dist_m();
                nvs_set_u32(h, "odo_m", odo);
                nvs_commit(h);
                nvs_close(h);
            }
        }
    }
    s_moving_prev = moving();
}

/* Phase J stub evaluation (1 s tick): level-triggered, edge-fired */
static void rules_eval(void)
{
    uint32_t now = now_ms();
    bool idle = (now - g.last_input_ms) > 60000;
    uint16_t f = g.dist_avg[1];
    bool obst = (f != 65535 && f < car_get_setting(2) && !g.estop);

    for (int i = 0; i < RULE_MAX; i++) {
        if (!s_rules[i].valid || !s_rules[i].enabled) {
            s_rule_lat[i] = false;
            continue;
        }
        bool cond = (s_rules[i].trig == 0) ? obst : idle;
        if (cond && !s_rule_lat[i] && !s_pend.busy) {
            s_rule_lat[i] = true;
            if (s_rules[i].act == 0) {          /* flash/follow headlights */
                s_pend.kind = AA_HEADLIGHT; s_pend.iarg = 1;
            } else {                             /* idle dim */
                s_dim_saved = s_tft_bright;
                s_pend.kind = AA_TFT_BRIGHT; s_pend.iarg = 20;
            }
            s_pend.sarg[0] = 0; s_pend.req_id[0] = 0;  /* autonomous */
            s_pend.busy = true;
        } else if (!cond && s_rule_lat[i] && !s_pend.busy) {
            s_rule_lat[i] = false;
            if (s_rules[i].act == 0) {
                s_pend.kind = AA_HEADLIGHT; s_pend.iarg = 0;
            } else {
                s_pend.kind = AA_TFT_BRIGHT; s_pend.iarg = s_dim_saved;
            }
            s_pend.sarg[0] = 0; s_pend.req_id[0] = 0;
            s_pend.busy = true;
        }
    }
}

void alexa_task(void *arg)
{
    (void)arg;
    s_boot_ms = now_ms();
    counters_load();
    alexa_psram_init();   /* PART 15: voice hist/dedupe/inbox to PSRAM */
    rules_load();
    config_load();

    ESP_LOGI(TAG, "alexa bridge starting");
    net_wifi_start();

    /* wait for IP (or run disabled-loop if broker empty) */
    bool mqtt_started = false;
    uint32_t last_shadow = 0, last_rules = 0, last_ev = 0;

    while (1) {
        if (!mqtt_started && net_wifi_up() && s_broker[0]) {
            mqtt_start();
            mqtt_started = true;
        }

        /* inbox */
        while (s_inbox && s_in_r != s_in_w) {
            uint8_t r = s_in_r;
            s_in_r = (uint8_t)((r + 1) % INBOX_N);
            serve_command(s_inbox[r]);
        }

        /* results -> resp topic (+ shadow + history handled in publish) */
        if (s_res.ready) {
            char req[40], reason[32];
            uint8_t st;
            snprintf(req, sizeof(req), "%s", s_res.req_id);
            snprintf(reason, sizeof(reason), "%s", s_res.reason);
            st = s_res.status;
            s_res.ready = false;
            if (req[0]) {
                publish_resp(req, (alexa_result_t)st, reason,
                             reason[0] ? reason : "ok", "");
                publish_shadow();
                state_snap();
            } else {
                /* autonomous (rule/warn): event, no requestId */
                publish_event("rule_fired", reason[0] ? reason : "rule action");
                notif_push(NOTIF_RULES, reason[0] ? reason : "rule action", now_ms());
            }
        }

        uint32_t now = now_ms();

        /* warn-burst expiry -> restore hazard off */
        if (s_warn_active && (int32_t)(now - s_warn_until) >= 0) {
            s_warn_active = false;
            if (!s_pend.busy) {
                s_pend.kind = AA_HAZARD; s_pend.iarg = 0;
                s_pend.sarg[0] = 0; s_pend.req_id[0] = 0;
                s_pend.busy = true;
            }
        }

        /* confirm auto-expiry (bridge side; car_task also checks) */
        portENTER_CRITICAL(&s_mux);
        bool cexp = s_conf.active &&
                    (int32_t)(now - s_conf.expiry_ms) >= 0;
        char creq[40] = {0};
        if (cexp) { snprintf(creq, sizeof(creq), "%s", s_conf.req_id); s_conf.active = false; }
        portEXIT_CRITICAL(&s_mux);
        if (cexp && creq[0]) publish_resp(creq, ALEXA_RES_CANCELLED, "TIMEOUT",
                                          "confirm", "");

        if (s_mqtt_ok && now - last_ev > 500) {
            last_ev = now;
            events_poll();
        }
        if (s_mqtt_ok && now - last_rules > 1000) {
            last_rules = now;
            rules_eval();
        }
        /* event-driven shadow: publish only on real state change
         * (idle car = silent), 1.5 s rate limit for drive transitions.
         * First connected loop always publishes (snapshot not init). */
        if (s_mqtt_ok && now - last_shadow > 1500 && state_dirty()) {
            last_shadow = now;
            state_snap();
            publish_shadow();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
