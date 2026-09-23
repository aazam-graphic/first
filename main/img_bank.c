/*
 * img_bank.c - PART 6 asset loader. Each /spiffs/img/<name>.raw is raw
 * RGB565 LE pixels; dimensions come from /spiffs/img/<name>.wh ("W H\n").
 * LRU cache holds up to 8 decoded images in PSRAM.
 */
#include <stdio.h>
#include <string.h>
#include "img_bank.h"
#include "os_gfx.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* img_get runs in car_task (draw) + late_init (preload): mux the whole
   lookup-or-load so two tasks never double-load/evict the same slot. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static const char *TAG = "img";

#define IMG_MAX_SLOTS 20   /* PART 7.0: 16 car sprites + wall + headroom */
#define IMG_MAX_BYTES (200 * 1024)

typedef struct {
    char name[24];
    uint16_t *px;
    uint16_t w, h;
    uint32_t use;
    bool used;
} img_slot_t;

static img_slot_t *s_slots;   /* PART 15: PSRAM (was internal static) */
static uint32_t s_tick;
static img_t *s_views;        /* per-slot stable views (aliasing-safe) */
static char (*s_miss)[24];    /* once-only miss log names */
static int s_miss_n;
#define S_MISS_MAX 16

/* (flash-embed fast path removed: all images load from SPIFFS into the
   PSRAM cache, preloaded at boot so the draw path never blocks.) */

void img_bank_init(void)
{
    if (s_slots) return;
    s_slots = heap_caps_malloc(IMG_MAX_SLOTS * sizeof(img_slot_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_views = heap_caps_malloc(IMG_MAX_SLOTS * sizeof(img_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_miss = heap_caps_malloc(S_MISS_MAX * 24, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_slots) memset(s_slots, 0, IMG_MAX_SLOTS * sizeof(img_slot_t));
    if (s_views) memset(s_views, 0, IMG_MAX_SLOTS * sizeof(img_t));
    if (s_miss) { memset(s_miss, 0, S_MISS_MAX * 24); s_miss_n = 0; }
    else s_miss_n = S_MISS_MAX;   /* no log space: stay silent */
    if (!s_slots || !s_views) ESP_LOGE(TAG, "slot alloc failed");
}

/* log each missing asset once (per-boot diagnosability, no frame spam) */
static void miss_log(const char *name)
{
    if (s_miss) {
        for (int i = 0; i < s_miss_n; i++)
            if (strncmp(s_miss[i], name, 24) == 0) return;
        if (s_miss_n < S_MISS_MAX) {
            snprintf(s_miss[s_miss_n], 24, "%s", name);
            s_miss_n++;
        }
    }
    ESP_LOGW(TAG, "missing asset: %s", name);
}

static bool load_img(const char *name, uint16_t **out_px, uint16_t *out_w,
                     uint16_t *out_h)
{
    char path[64], wpath[64];
    snprintf(path, sizeof(path), "/spiffs/img/%s.raw", name);
    snprintf(wpath, sizeof(wpath), "/spiffs/img/%s.wh", name);
    FILE *wf = fopen(wpath, "r");
    if (!wf) { miss_log(name); return false; }
    int w = 0, h = 0;
    if (fscanf(wf, "%d %d", &w, &h) != 2 || w <= 0 || h <= 0 || w > 320 || h > 240) {
        fclose(wf);
        miss_log(name);
        return false;
    }
    fclose(wf);
    size_t need = (size_t)w * h * 2;
    if (need > IMG_MAX_BYTES) { miss_log(name); return false; }
    FILE *f = fopen(path, "rb");
    if (!f) { miss_log(name); return false; }
    uint16_t *buf = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { fclose(f); return false; }
    size_t off = 0;
    while (off < need) {   /* chunked: never starve IDLE (task-WDT) */
        size_t chunk = need - off > 8192 ? 8192 : need - off;
        if (fread((uint8_t *)buf + off, 1, chunk, f) != chunk) {
            fclose(f);
            heap_caps_free(buf);
            return false;
        }
        off += chunk;
        vTaskDelay(pdMS_TO_TICKS(20));   /* 100Hz tick: 5ms == 0 == no-op! */
    }
    fclose(f);
    ESP_LOGI(TAG, "loaded %s %dx%d", name, w, h);
    *out_px = buf;
    *out_w = (uint16_t)w;
    *out_h = (uint16_t)h;
    return true;
}

const img_t *img_get(const char *name)
{
    if (!name || !*name) return NULL;
    if (!s_slots || !s_views) return NULL;
    /* lookup + reserve under lock (short, no I/O inside) */
    int idx = -1;
    portENTER_CRITICAL(&s_mux);
    s_tick++;
    for (int i = 0; i < IMG_MAX_SLOTS; i++) {
        if (s_slots[i].used && s_slots[i].px &&
            strncmp(s_slots[i].name, name, sizeof(s_slots[i].name)) == 0) {
            s_slots[i].use = s_tick;
            s_views[i].px = s_slots[i].px;
            s_views[i].w = s_slots[i].w;
            s_views[i].h = s_slots[i].h;
            portEXIT_CRITICAL(&s_mux);
            return &s_views[i];
        }
    }
    for (int i = 0; i < IMG_MAX_SLOTS; i++) {
        if (!s_slots[i].used) { idx = i; break; }
    }
    if (idx < 0) {   /* full: evict LRU (dead path: 12 slots, 7 images) */
        uint32_t lu = 0xFFFFFFFF;
        for (int i = 0; i < IMG_MAX_SLOTS; i++) {
            if (s_slots[i].used && s_slots[i].use < lu) {
                lu = s_slots[i].use;
                idx = i;
            }
        }
    }
    if (idx < 0) { portEXIT_CRITICAL(&s_mux); return NULL; }
    /* evict path: steal the old buffer pointer, free AFTER leaving the
       critical section (heap free takes locks — never with IRQs off). */
    uint16_t *old_px = s_slots[idx].px;
    s_slots[idx].px = NULL;
    s_slots[idx].used = true;   /* reserved: other task picks another slot */
    snprintf(s_slots[idx].name, sizeof(s_slots[idx].name), "%s", name);
    portEXIT_CRITICAL(&s_mux);
    if (old_px) heap_caps_free(old_px);
    /* load outside the lock, then publish */
    uint16_t *px = NULL;
    uint16_t w = 0, h = 0;
    if (!load_img(name, &px, &w, &h)) {
        portENTER_CRITICAL(&s_mux);
        s_slots[idx].used = false;   /* release reservation */
        portEXIT_CRITICAL(&s_mux);
        return NULL;
    }
    portENTER_CRITICAL(&s_mux);
    /* dedupe: another task may have published the same name while we loaded
       (reserve-then-load window) — drop our copy, use theirs. */
    for (int i = 0; i < IMG_MAX_SLOTS; i++) {
        if (i != idx && s_slots[i].used && s_slots[i].px &&
            strncmp(s_slots[i].name, name, sizeof(s_slots[i].name)) == 0) {
            s_slots[idx].used = false;
            s_slots[idx].px = NULL;
            portEXIT_CRITICAL(&s_mux);
            heap_caps_free(px);
            portENTER_CRITICAL(&s_mux);
            s_slots[i].use = s_tick;
            s_views[i].px = s_slots[i].px;
            s_views[i].w = s_slots[i].w;
            s_views[i].h = s_slots[i].h;
            portEXIT_CRITICAL(&s_mux);
            return &s_views[i];
        }
    }
    s_slots[idx].px = px;
    s_slots[idx].w = w;
    s_slots[idx].h = h;
    s_slots[idx].use = s_tick;
    s_views[idx].px = px;
    s_views[idx].w = w;
    s_views[idx].h = h;
    portEXIT_CRITICAL(&s_mux);
    return &s_views[idx];
}

void img_blit(const img_t *img, int x, int y)
{
    if (!img || !img->px) return;
    gfx_blit(x, y, img->w, img->h, img->px);
}

void img_blit_half(const img_t *img, int x, int y)
{
    if (!img || !img->px) return;
    for (uint16_t sy = 0; sy < img->h; sy += 2) {
        for (uint16_t sx = 0; sx < img->w; sx += 2) {
            uint16_t c = img->px[sy * img->w + sx];
            if (c == GFX_KEY) continue;
            gfx_px(x + sx / 2, y + sy / 2, c);
        }
    }
}

void img_blit_full(const img_t *img)
{
    if (!img || !img->px) return;
    gfx_blit_full(img->px);
}
