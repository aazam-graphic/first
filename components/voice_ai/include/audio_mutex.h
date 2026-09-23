/*
 * audio_mutex.h — protects the shared I2S0 TX channel (MAX98357A amp)
 * between existing WAV effects (car.c) and XiaoZhi TTS playback.
 */

#ifndef AUDIO_MUTEX_H
#define AUDIO_MUTEX_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_MUTEX_TIMEOUT_MS 2000
#define AUDIO_MUTEX_TIMEOUT     pdMS_TO_TICKS(AUDIO_MUTEX_TIMEOUT_MS)

void     audio_mutex_init(void);
bool     audio_mutex_lock(TickType_t timeout);
void     audio_mutex_unlock(void);
bool     audio_mutex_is_locked(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_MUTEX_H */
