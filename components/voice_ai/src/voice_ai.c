/*
 * voice_ai.c — XiaoZhi voice-AI orchestrator for Car OS.
 *
 * Lifecycle: init -> start -> [READY | LISTENING | PROCESSING | SPEAKING | OFFLINE | ERROR]
 * Core 1 only.  Never blocks the motor-control loop.
 */
#include "voice_ai.h"
#include "voice_audio_i2s.h"
#include "audio_mutex.h"
#include "mcp_car_tools.h"
#include "car_os.h"
#include "car.h"
#include "car_global.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include <string.h>
#include <stdio.h>

/* XiaoZhi protocol component */
#include "esp_xiaozhi_chat.h"
#include "esp_xiaozhi_info.h"

static const char *TAG = "voice_ai";

/* ---- shutdown flag (read by I2S driver) ----------------------------- */
volatile bool g_voice_shutdown = false;

/* ---- internals -------------------------------------------------------- */
static voice_ai_config_t   s_cfg;
static voice_ai_state_t    s_state = VOICE_STATE_OFFLINE;
static voice_ai_state_t    s_last_state = VOICE_STATE_OFFLINE;
static TaskHandle_t        s_task = NULL;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static void set_state(voice_ai_state_t st)
{
    if (st == s_state) return;
    s_last_state = s_state;
    s_state = st;
    ESP_LOGI(TAG, "state %d -> %d", s_last_state, st);
    if (s_cfg.on_state) s_cfg.on_state(s_last_state, st, s_cfg.on_state_ctx);
}
const char *voice_ai_state_str(voice_ai_state_t st)
{
    switch (st) {
    case VOICE_STATE_OFFLINE:    return "OFFLINE";
    case VOICE_STATE_READY:      return "READY";
    case VOICE_STATE_LISTENING:  return "LISTENING";
    case VOICE_STATE_PROCESSING: return "PROCESSING";
    case VOICE_STATE_SPEAKING:   return "SPEAKING";
    case VOICE_STATE_ERROR:      return "ERROR";
    }
    return "?";
}
voice_ai_state_t voice_ai_get_state(void) { return s_state; }

static EventGroupHandle_t  s_wifi_events = NULL;
static esp_xiaozhi_chat_handle_t s_chat = NULL;

/* ---- Wi-Fi --------------------------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t id, void *data)
{
    (void)arg; (void)data;
    if (event_base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        set_state(VOICE_STATE_OFFLINE);
    } else if (event_base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_connect(const char *ssid, const char *pass)
{
    if (!ssid || !*ssid) { ESP_LOGW(TAG, "no SSID; offline mode"); return ESP_OK; }
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    wifi_config_t wcfg = { .sta = { .threshold.authmode = WIFI_AUTH_WPA2_PSK } };
    strncpy((char *)wcfg.sta.ssid, ssid, 32);
    if (pass) strncpy((char *)wcfg.sta.password, pass, 64);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT) { ESP_LOGI(TAG, "Wi-Fi connected"); return ESP_OK; }
    ESP_LOGW(TAG, "Wi-Fi connect failed");
    return ESP_ERR_TIMEOUT;
}

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

    ESP_LOGW(TAG, "Wi-Fi connect failed");
    return ESP_ERR_TIMEOUT;
}

/* ---- audio pipeline ------------------------------------------------ */
static esp_err_t voice_i2s0_tx(const uint8_t *buf, size_t len)
{
    extern i2s_chan_handle_t car_i2s0_handle(void); /* car.c exposes this */
    size_t w = 0;
    return i2s_channel_write(car_i2s0_handle(), buf, len, &w, 0);
}

static esp_err_t audio_cb(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    if (!data || !len) return ESP_OK;
    if (!audio_mutex_lock(pdMS_TO_TICKS(500))) {
        ESP_LOGW(TAG, "I2S0 mutex timeout (TTS drop %u B)", (unsigned)len);
        return ESP_OK;
    }
    voice_i2s0_tx(data, len);
    audio_mutex_unlock();
    return ESP_OK;
}

/* ---- chat event callback -------------------------------------------- */
static void chat_cb(esp_xiaozhi_chat_handle_t h, esp_xiaozhi_chat_event_t ev,
                    void *ed, void *ctx)
{
    (void)h; (void)ctx;
    switch (ev) {
    case ESP_XIAOZHI_CHAT_EVENT_CONNECTED:
        set_state(VOICE_STATE_READY); break;
    case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
        set_state(VOICE_STATE_OFFLINE); break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_OPENED:
        set_state(VOICE_STATE_LISTENING); break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_CLOSED:
        set_state(VOICE_STATE_READY); break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE: {
        esp_xiaozhi_chat_tts_state_t *st = (esp_xiaozhi_chat_tts_state_t *)ed;
        if (st && st->state == ESP_XIAOZHI_CHAT_TTS_STATE_START) set_state(VOICE_STATE_SPEAKING);
        else if (st && st->state == ESP_XIAOZHI_CHAT_TTS_STATE_STOP) set_state(VOICE_STATE_READY);
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT: {
        esp_xiaozhi_chat_text_data_t *td = (esp_xiaozhi_chat_text_data_t *)ed;
        if (s_cfg.on_text && td) s_cfg.on_text(td->text, false, s_cfg.on_text_ctx);
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR:
        set_state(VOICE_STATE_ERROR); break;
    default: break;
void *voice_ai_get_mcp_engine(void) { return s_mcp_engine; }

/* ---- main voice task ------------------------------------------------ */
static void voice_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "voice task started (Core %d)", xPortGetCoreID());

    /* 1. Wi-Fi -------------------------------------------------------- */
    if (s_cfg.wifi_ssid) {
        if (wifi_connect(s_cfg.wifi_ssid, s_cfg.wifi_pass) != ESP_OK) {
            set_state(VOICE_STATE_OFFLINE);
        }
    }

    /* 2. XiaoZhi chat init -------------------------------------------- */
    esp_xiaozhi_chat_info_t info = {0};
    if (s_cfg.xiaozhi_url) {
        esp_xiaozhi_chat_get_info(&info);
        /* info.has_mqtt_config / info.has_websocket_config available */
    }
    esp_xiaozhi_chat_config_t cc = ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();
    cc.audio_callback = audio_cb;
    cc.event_callback = chat_cb;
    cc.mcp_engine = s_mcp_engine;
    cc.has_mqtt_config = info.has_mqtt_config;
    cc.has_websocket_config = info.has_websocket_config;
    if (esp_xiaozhi_chat_init(&cc, &s_chat) != ESP_OK) {
        ESP_LOGE(TAG, "xiaozhi init failed");
        set_state(VOICE_STATE_ERROR);
        goto cleanup;
    }
    if (esp_xiaozhi_chat_start(s_chat) != ESP_OK) {
        ESP_LOGE(TAG, "xiaozhi start failed");
        set_state(VOICE_STATE_ERROR);
        goto cleanup;
    }

    /* 3. I2S mic start ------------------------------------------------ */
    if (voice_audio_i2s_init() != ESP_OK) {
        ESP_LOGW(TAG, "I2S mic init failed");
    } else {
        voice_audio_i2s_start();
    }

    /* 4. Register MCP tools ------------------------------------------- */
    if (s_mcp_engine) mcp_car_tools_register(s_mcp_engine);

    /* 5. Main loop: stream mic to xiaozhi ----------------------------- */
    set_state(VOICE_STATE_READY);
    uint8_t pcm[1024];
    while (!g_voice_shutdown) {
        if (s_state == VOICE_STATE_LISTENING && s_chat) {
            size_t n = voice_audio_i2s_read(pcm, sizeof(pcm), 100);
            if (n > 0) esp_xiaozhi_chat_send_audio_data(s_chat, pcm, n);
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

/* ---- safe command layer (used by MCP tools) ------------------------ */

esp_err_t voice_cmd_get_car_status(char *buf, size_t cap)
{
    int fl = (g.dist_avg[0] == 65535) ? -1 : (int)g.dist_avg[0];
    int ff = (g.dist_avg[1] == 65535) ? -1 : (int)g.dist_avg[1];
    int fr = (g.dist_avg[2] == 65535) ? -1 : (int)g.dist_avg[2];
    snprintf(buf, cap,
             "{\"gear\":%u,\"mode\":\"%s\",\"headlight\":%s,\"hazard\":%s,"
             "\"front\":%d,\"left\":%d,\"right\":%d,\"speed_cap\":%d}",
             (unsigned)(g.gear + 1),
             g.mode == MODE_AUTO ? "AUTO" : g.mode == MODE_CRAWL ? "CRAWL" : "MANUAL",
             g.headlight ? "on" : "off", g.hazard ? "on" : "off",
             ff, fl, fr, car_speed_cap_pct());
    return ESP_OK;
}

esp_err_t voice_cmd_get_distance(const char *which, char *buf, size_t cap)
{
    int d = -1;
    if (!strcmp(which, "front")) d = (g.dist_avg[1] == 65535) ? -1 : (int)g.dist_avg[1];
    else if (!strcmp(which, "left"))  d = (g.dist_avg[0] == 65535) ? -1 : (int)g.dist_avg[0];
    else if (!strcmp(which, "right")) d = (g.dist_avg[2] == 65535) ? -1 : (int)g.dist_avg[2];
    snprintf(buf, cap, "{\"%s\":%d}", which, d);
    return ESP_OK;
}

esp_err_t voice_cmd_open_screen(const char *screen)
{
    /* Use existing car_os state machine — parks the car via INPUT_CTX_OS */
    if (!screen) return ESP_ERR_INVALID_ARG;
    /* Transition handled by caller via go(); return true on valid name */
    if (!strcmp(screen, "home") || !strcmp(screen, "drive") ||
        !strcmp(screen, "diagnostics") || !strcmp(screen, "games"))
        return ESP_OK;
    return ESP_ERR_INVALID_ARG;
}

esp_err_t voice_cmd_set_headlight(int on)
{
    if (on) g.headlight = true; else g.headlight = false;
    return ESP_OK;
}
esp_err_t voice_cmd_set_hazard(int on)
{
    if (on) g.hazard = true; else g.hazard = false;
    return ESP_OK;
}
esp_err_t voice_cmd_set_volume(int pct)
{
    if (pct < 0 || pct > 100) return ESP_ERR_INVALID_ARG;
    car_set_setting(1, (uint8_t)pct);
    return ESP_OK;
}
esp_err_t voice_cmd_set_tft_bright(int pct)
{
    if (pct < 0 || pct > 100) return ESP_ERR_INVALID_ARG;
    car_set_setting(5, (uint8_t)pct);
    return ESP_OK;
}
esp_err_t voice_cmd_set_oled(int layout)
{
    if (layout < 0 || layout > 3) return ESP_ERR_INVALID_ARG;
    s_os.oled_layout = (uint8_t)layout;
    return ESP_OK;
}

cleanup:
    ESP_LOGI(TAG, "voice task shutting down");
    if (s_chat) { esp_xiaozhi_chat_stop(s_chat); esp_xiaozhi_chat_deinit(s_chat); s_chat = NULL; }
    voice_audio_i2s_stop();
    mcp_car_tools_unregister();
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---- public API ------------------------------------------------------ */
esp_err_t voice_ai_init(const voice_ai_config_t *cfg)
{
    if (s_task) return ESP_ERR_INVALID_STATE;
    if (cfg) s_cfg = *cfg;
    audio_mutex_init();
    return ESP_OK;
}

esp_err_t voice_ai_start(void)
{
    if (s_task || !s_cfg.wifi_ssid) return ESP_ERR_INVALID_STATE;
    g_voice_shutdown = false;
    xTaskCreatePinnedToCore(voice_task, "voice_ai", 8192, NULL, 3, &s_task, 1);
    return ESP_OK;
}

void voice_ai_stop(void) { g_voice_shutdown = true; }
void voice_ai_shutdown(void) { g_voice_shutdown = true; vTaskDelay(pdMS_TO_TICKS(200)); }

void voice_ai_request_listen(void)
{
    if (s_chat && s_state == VOICE_STATE_READY)
        esp_xiaozhi_chat_open_audio_channel(s_chat, NULL, NULL, 0);
}
void voice_ai_request_stop(void)
{
    if (s_chat && s_state == VOICE_STATE_LISTENING)
        esp_xiaozhi_chat_close_audio_channel(s_chat);
}
void voice_ai_request_abort_speak(void)
{
    if (s_chat && s_state == VOICE_STATE_SPEAKING)
        esp_xiaozhi_chat_send_abort_speaking(s_chat);
}

    }
}

/* ---- MCP engine stub ------------------------------------------------ */
static void *s_mcp_engine = NULL;
void *voice_ai_get_mcp_engine(void) { return s_mcp_engine; }
