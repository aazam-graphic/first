#pragma once

#include <stdint.h>

/* Robot car control task (created after USB host + xbox360 tasks) */
void car_task(void *arg);

/* Shared I2S0 TX channel (mutex-protected, engine audio owner: car.c) */
#include "driver/i2s_std.h"
i2s_chan_handle_t car_i2s0_handle(void);

/* Engine sound pause (speaker exclusive use) */
void car_audio_pause(bool pause);

/* I2S0 TX sample-rate reconfig. Caller: disable -> reconfig -> enable */
esp_err_t car_i2s0_reconfig_rate(uint32_t hz);
/* PCM (16k mono int16) -> I2S0 speaker; caller pauses engine around this */
void car_audio_play_tts_pcm(const int16_t *pcm, int samples);
/* Raw 16k PCM write (no clock reconfig) */
void car_audio_write_pcm(const int16_t *pcm, int samples);