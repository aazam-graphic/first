/*
 * notif.c - PART 10.3 Notification Center ring buffer.
 */
#include <string.h>
#include <stdio.h>
#include "notif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#define NOTIF_N 24

/* writers: car_task (alerts, IMU, sound) + alexa_task (banner, rules).
   portMUX keeps ring indices + text tear-free across tasks. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static notif_item_t *s_ring;   /* PART 15: PSRAM (was 1.3KB internal static) */
static uint8_t s_w;
static uint8_t s_n;

void notif_init(void)
{
    if (s_ring) return;
    s_ring = heap_caps_malloc(NOTIF_N * sizeof(notif_item_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ring) memset(s_ring, 0, NOTIF_N * sizeof(notif_item_t));
    else ESP_LOGE("notif", "ring alloc failed");
}

void notif_push(notif_cat_t cat, const char *text, uint32_t now_ms)
{
    if (cat >= NOTIF_COUNT || !text || !*text || !s_ring) return;
    portENTER_CRITICAL(&s_mux);
    /* coalesce: same text as newest -> refresh timestamp only */
    if (s_n) {
        uint8_t newest = (uint8_t)((s_w + NOTIF_N - 1) % NOTIF_N);
        if (s_ring[newest].cat == cat &&
            strncmp(s_ring[newest].text, text, sizeof(s_ring[newest].text)) == 0) {
            s_ring[newest].ms = now_ms;
            portEXIT_CRITICAL(&s_mux);
            return;
        }
    }
    s_ring[s_w].cat = cat;
    snprintf(s_ring[s_w].text, sizeof(s_ring[s_w].text), "%s", text);
    s_ring[s_w].ms = now_ms;
    s_w = (uint8_t)((s_w + 1) % NOTIF_N);
    if (s_n < NOTIF_N) s_n++;
    portEXIT_CRITICAL(&s_mux);
}

int notif_list(notif_item_t *out, int max)
{
    if (!out || max <= 0 || !s_ring) return 0;
    portENTER_CRITICAL(&s_mux);
    int n = s_n < max ? s_n : max;
    for (int i = 0; i < n; i++) {
        uint8_t idx = (uint8_t)((s_w + NOTIF_N - 1 - i) % NOTIF_N);
        out[i] = s_ring[idx];
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}

const char *notif_cat_name(notif_cat_t c)
{
    switch (c) {
    case NOTIF_ALEXA: return "ALEXA";
    case NOTIF_SAFETY: return "SAFETY";
    case NOTIF_RULES: return "RULES";
    case NOTIF_SOUND: return "SOUND";
    case NOTIF_MPU: return "MPU";
    default: return "?";
    }
}
