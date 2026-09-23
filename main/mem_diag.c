/*
 * mem_diag.c - PART 15/16 memory diagnostics (task context only).
 */
#include "mem_diag.h"

#if MEM_DIAG_ENABLE
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mem";
static uint32_t s_period_ms = 10000;

uint32_t mic_stack_free(void);      /* mic_in.c */
uint32_t imu_adv_stack_free(void);  /* imu_adv.c */

void mem_diag_init(void)
{
    ESP_LOGI(TAG, "mem_diag ready (tick API, task context only)");
}

void mem_diag_start_timer(uint32_t period_ms)
{
    if (period_ms < 1000) period_ms = 1000;
    s_period_ms = period_ms;
    ESP_LOGI(TAG, "mem_diag cadence %lums", (unsigned long)s_period_ms);
}

void mem_diag_tick(uint32_t now_ms)
{
    static uint32_t s_last;
    if (now_ms - s_last < s_period_ms) return;   /* off hot path */
    s_last = now_ms;
    ESP_LOGI(TAG,
             "heap int=%uK(min %uK,maxblk %uK) psram=%uK carstk=%u micstk=%u imustk=%u",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)mic_stack_free(), (unsigned)imu_adv_stack_free());
}
#else
void mem_diag_tick(uint32_t now_ms) { (void)now_ms; }
void mem_diag_init(void) {}
void mem_diag_start_timer(uint32_t period_ms) { (void)period_ms; }
#endif
