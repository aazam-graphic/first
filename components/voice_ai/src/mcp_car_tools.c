/*
 * mcp_car_tools.c — MCP tools implementation.
 */
#include "mcp_car_tools.h"
#include "voice_ai.h"
#include "voice_audio_i2s.h"
#include "car_os.h"
#include "car.h"
#include "car_global.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "mcp_car";
static void json_int(char *buf, size_t cap, const char *key, int val)
{ snprintf(buf, cap, "{\"%s\":%d}", key, val); }

static esp_err_t tool_get_status(void *engine, const char *args, char *result, size_t cap)
{
    (void)engine; (void)args;
    return voice_cmd_get_car_status(result, cap);
}
static esp_err_t tool_get_distance(void *engine, const char *args, char *result, size_t cap)
{
    (void)engine;
    if (!args || !strcmp(args, "front")) return voice_cmd_get_distance("front", result, cap);
    if (!strcmp(args, "left"))   return voice_cmd_get_distance("left", result, cap);
    if (!strcmp(args, "right"))  return voice_cmd_get_distance("right", result, cap);
    json_str(result, cap, "error", "use front|left|right");
    return ESP_OK;
}
static esp_err_t tool_open_screen(void *engine, const char *args, char *result, size_t cap)
{
    (void)engine;
    if (!args || !voice_cmd_open_screen(args)) {
        json_str(result, cap, "error", "use home|drive|diagnostics|games");
        return ESP_OK;
    json_str(result, cap, "hazard", on ? "on" : "off");
    return ESP_OK;
}
static esp_err_t tool_set_volume(void *engine, const char *args, char *result, size_t cap)
{
    (void)engine;
    int pct = args ? atoi(args) : -1;
    if (pct < 0 || pct > 100) { json_str(result, cap, "error", "volume 0..100"); return ESP_OK; }
    if (voice_cmd_set_volume(pct) != ESP_OK) { json_str(result, cap, "error", "set failed"); return ESP_OK; }
    json_int(result, cap, "volume", pct);
    return ESP_OK;
}
static esp_err_t tool_set_tft(void *engine, const char *args, char *result, size_t cap)
{
    (void)engine;
    int pct = args ? atoi(args) : -1;
    if (pct < 0 || pct > 100) { json_str(result, cap, "error", "brightness 0..100"); return ESP_OK; }
    if (voice_cmd_set_tft_bright(pct) != ESP_OK) { json_str(result, cap, "error", "set failed"); return ESP_OK; }
    json_int(result, cap, "tft_brightness", pct);
    return ESP_OK;
}
static esp_err_t tool_set_oled(void *engine, const char *args, char *result, size_t cap)
{
    (void)engine;
    int layout = args ? atoi(args) : -1;
    if (layout < 0 || layout > 3) { json_str(result, cap, "error", "layout 0..3"); return ESP_OK; }
    if (voice_cmd_set_oled(layout) != ESP_OK) { json_str(result, cap, "error", "set failed"); return ESP_OK; }
    json_int(result, cap, "oled_layout", layout);
    return ESP_OK;
}

typedef struct {
    const char *name;
    const char *desc;
    esp_err_t (*handler)(void *engine, const char *args, char *result, size_t cap);
} mcp_tool_t;

static const mcp_tool_t s_tools[] = {
    { "get_car_status",     "Read-only car status",          tool_get_status  },
    { "get_front_distance", "Front distance cm",             tool_get_distance },
    { "get_left_distance",  "Left distance cm",              tool_get_distance },
    { "get_right_distance", "Right distance cm",             tool_get_distance },
    { "open_home",          "Open Home",                     tool_open_screen },
    { "open_drive",         "Open Drive",                    tool_open_screen },
    { "open_diagnostics",   "Open Diagnostics",              tool_open_screen },
    { "open_games",         "Open Games",                    tool_open_screen },
    { "set_headlight",      "Headlight on/off",              tool_set_headlight },
    { "set_hazard",         "Hazard on/off",                 tool_set_hazard },
    { "set_engine_volume",  "Volume 0..100",                 tool_set_volume },
    { "set_tft_brightness", "TFT brightness 0..100",         tool_set_tft },
    { "set_oled_layout",    "OLED layout 0..3",              tool_set_oled },
};

static bool s_registered = false;

esp_err_t mcp_car_tools_register(void *mcp_engine)
{
    if (!mcp_engine || s_registered) return ESP_ERR_INVALID_STATE;
    /* Adapt loop to your mcp-c-sdk API. */
    (void)s_tools;
    s_registered = true;
    ESP_LOGI(TAG, "%u car tools registered", (unsigned)(sizeof(s_tools)/sizeof(s_tools[0])));
    return ESP_OK;
}
void mcp_car_tools_unregister(void) { s_registered = false; }

    }
    json_str(result, cap, "ok", args);
    return ESP_OK;
}
static esp_err_t tool_set_headlight(void *engine, const char *args, char *result, size_t cap)
{
    (void)engine;
    int on = (!args || strcmp(args, "off") == 0 || strcmp(args, "false") == 0 || strcmp(args, "0") == 0) ? 0 : 1;
    if (voice_cmd_set_headlight(on) != ESP_OK) { json_str(result, cap, "error", "set failed"); return ESP_OK; }
    json_str(result, cap, "headlight", on ? "on" : "off");
    return ESP_OK;
}
static esp_err_t tool_set_hazard(void *engine, const char *args, char *result, size_t cap)
{
    (void)engine;
    int on = (!args || strcmp(args, "off") == 0 || strcmp(args, "false") == 0 || strcmp(args, "0") == 0) ? 0 : 1;
    if (voice_cmd_set_hazard(on) != ESP_OK) { json_str(result, cap, "error", "set failed"); return ESP_OK; }
    json_str(result, cap, "hazard", on ? "on" : "off");
    return ESP_OK;
}


extern esp_err_t voice_cmd_get_car_status(char *buf, size_t cap);
extern esp_err_t voice_cmd_get_distance(const char *which, char *buf, size_t cap);
extern esp_err_t voice_cmd_open_screen(const char *screen);
extern esp_err_t voice_cmd_set_headlight(int on);
extern esp_err_t voice_cmd_set_hazard(int on);
extern esp_err_t voice_cmd_set_volume(int pct);
extern esp_err_t voice_cmd_set_tft_bright(int pct);
extern esp_err_t voice_cmd_set_oled(int layout);

static void json_str(char *buf, size_t cap, const char *key, const char *val)
{ snprintf(buf, cap, "{\"%s\":\"%s\"}", key, val); }
static void json_int(char *buf, size_t cap, const char *key, int val)
{ snprintf(buf, cap, "{\"%s\":%d}", key, val); }
