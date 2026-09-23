/*
 * snd_bank.c - PART 5 sound bank loader + LRU PSRAM cache.
 */
#include <stdio.h>
#include <string.h>
#include "snd_bank.h"
#include "car_global.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "snd";

#define SND_MAX_SLOTS  48   /* PART 16: whole bank resident (~370KB PSRAM) */
#define SND_MAX_FILE   (96 * 1024)

typedef struct {
    char     name[24];
    uint8_t *pcm;
    uint32_t len;
    uint32_t use;
    bool     used;
} snd_slot_t;

static snd_slot_t *s_slots;   /* PART 15: PSRAM (was 2KB internal static) */
static uint32_t s_tick;

void snd_bank_init(void)
{
    if (s_slots) return;
    s_slots = heap_caps_malloc(SND_MAX_SLOTS * sizeof(snd_slot_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_slots) memset(s_slots, 0, SND_MAX_SLOTS * sizeof(snd_slot_t));
    else ESP_LOGE(TAG, "slot alloc failed - bank silent");
}

/* find_slot + load run in car_task (play) + late_init (preload): reserve
   marking under a short mux keeps two tasks off the same slot. I/O and
   playback happen outside the lock. 48 slots / 46 sounds: no eviction. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static int slot_lookup(const char *name)
{
    for (int i = 0; i < SND_MAX_SLOTS; i++) {
        if (s_slots[i].used && s_slots[i].pcm &&
            strncmp(s_slots[i].name, name, sizeof(s_slots[i].name)) == 0)
            return i;
    }
    return -1;
}

static int slot_reserve(const char *name)
{
    int idx = -1;
    for (int i = 0; i < SND_MAX_SLOTS; i++) {
        if (!s_slots[i].used) { idx = i; break; }
    }
    if (idx < 0) return -1;   /* full (dead path) */
    s_slots[idx].used = true;   /* reserved: px NULL until published */
    s_slots[idx].pcm = NULL;
    snprintf(s_slots[idx].name, sizeof(s_slots[idx].name), "%s", name);
    return idx;
}

static bool load_raw(const char *name, uint8_t **out_pcm, uint32_t *out_len)
{
    char path[64];
    snprintf(path, sizeof(path), "/spiffs/snd/%s.raw", name);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > SND_MAX_FILE) { fclose(f); return false; }
    uint8_t *buf = heap_caps_malloc((size_t)n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { fclose(f); return false; }
    size_t off = 0;
    while (off < (size_t)n) {   /* chunked: never starve IDLE (task-WDT) */
        size_t chunk = (size_t)n - off > 8192 ? 8192 : (size_t)n - off;
        if (fread(buf + off, 1, chunk, f) != chunk) {
            fclose(f);
            heap_caps_free(buf);
            return false;
        }
        off += chunk;
        vTaskDelay(pdMS_TO_TICKS(20));   /* 100Hz tick: 5ms == 0 == no-op! */
    }
    fclose(f);
    *out_pcm = buf;
    *out_len = (uint32_t)n;
    return true;
}

static snd_slot_t *slot_ensure(const char *name)
{
    int idx;
    portENTER_CRITICAL(&s_mux);
    idx = slot_lookup(name);
    if (idx >= 0) {
        s_slots[idx].use = s_tick;
        portEXIT_CRITICAL(&s_mux);
        return &s_slots[idx];
    }
    idx = slot_reserve(name);
    portEXIT_CRITICAL(&s_mux);
    if (idx < 0) return NULL;
    uint8_t *pcm = NULL;
    uint32_t len = 0;
    if (!load_raw(name, &pcm, &len)) {
        portENTER_CRITICAL(&s_mux);
        s_slots[idx].used = false;
        portEXIT_CRITICAL(&s_mux);
        return NULL;
    }
    portENTER_CRITICAL(&s_mux);
    s_slots[idx].pcm = pcm;
    s_slots[idx].len = len;
    s_slots[idx].use = s_tick;
    portEXIT_CRITICAL(&s_mux);
    return &s_slots[idx];
}

bool snd_play_vol(const char *name, uint16_t vol)
{
    if (!name || !*name || !s_slots) return false;
    s_tick++;
    snd_slot_t *s = slot_ensure(name);
    if (!s || !s->pcm) return false;
    /* master scale = engine volume setting (0..100) */
    uint8_t master = car_get_setting(1);
    uint16_t v = (uint16_t)((uint32_t)vol * master / 100u);
    if (v == 0) return true;
    car_snd_play_pcm(s->pcm, s->len, v);
    return true;
}

bool snd_play(const char *name)
{
    return snd_play_vol(name, 800);
}

void snd_bank_prefetch(void)
{
    static const char *tiny[] = {
        "ui_tap", "ui_back", "ui_nav", "ui_toggle_on", "ui_toggle_off",
        "score_tick", "gear_shift_up", "gear_shift_down",
    };
    for (unsigned i = 0; i < sizeof(tiny) / sizeof(tiny[0]); i++) {
        s_tick++;
        slot_ensure(tiny[i]);
        vTaskDelay(pdMS_TO_TICKS(20));   /* yield: SPIFFS reads must not starve IDLE */
    }
    ESP_LOGI(TAG, "prefetch done");
}

/* PART 16: whole-bank preload (late_init, background, yields per file) */
void snd_bank_preload_all(void)
{
    int n = 0;
    /* fixed inventory (matches tools/gen_sounds.py) - no readdir nondeterminism */
    static const char *all[] = {
        "ui_tap", "ui_back", "ui_nav", "ui_open_sheet", "ui_close_sheet",
        "ui_error", "ui_toggle_on", "ui_toggle_off",
        "gear_shift_up", "gear_shift_down", "gear_limit",
        "sys_boot_chime", "sys_ready", "sys_warning", "sys_estop",
        "sys_connect", "sys_disconnect",
        "voice_listen_start", "voice_listen_end", "alexa_notify", "clap_detected",
        "roof_police_on", "roof_mode_cycle", "light_flash",
        "score_tick", "highscore_fanfare", "game_over", "score_reveal",
        "grade_A", "grade_B", "grade_C",
        "impact_light", "impact_moderate", "impact_hard",
        "freefall_start", "landing_smooth", "landing_hard",
        "stuck_alert", "drift_warn",
        "profile_chime_safe", "profile_chime_night", "profile_chime_perf",
        "profile_chime_park", "profile_chime_demo", "profile_chime_silent",
        "profile_chime_game",
    };
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        s_tick++;
        if (slot_ensure(all[i])) n++;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "preload_all: %d sounds cached", n);
}
