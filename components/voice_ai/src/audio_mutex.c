#include "audio_mutex.h"
#include "esp_log.h"

static const char *TAG = "audio_mutex";
static SemaphoreHandle_t s_mutex = NULL;

void audio_mutex_init(void)
{
    if (s_mutex) return;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) ESP_LOGE(TAG, "mutex create failed");
}

bool audio_mutex_lock(TickType_t timeout)
{
    if (!s_mutex) return false;
    return xSemaphoreTake(s_mutex, timeout) == pdTRUE;
}

void audio_mutex_unlock(void)
{
    if (s_mutex) xSemaphoreGive(s_mutex);
}

bool audio_mutex_is_locked(void)
{
    if (!s_mutex) return false;
    return uxSemaphoreGetCount(s_mutex) == 0;
}
