/*
 * snd_bank.h - PART 5 sound bank: 16-bit PCM mono 16kHz .raw files
 * in SPIFFS (/spiffs/snd/<name>.raw), PSRAM LRU cache, played through
 * the car mixer (car_snd_play_pcm, 16k->22.05k resampled).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Play a bank sound by name (e.g. "ui_tap"). Volume follows the engine-vol
   setting. Returns false if the file is missing (caller keeps legacy beep). */
bool snd_play(const char *name);
bool snd_play_vol(const char *name, uint16_t vol);

/* PART 15: slot table lives in PSRAM - call once at boot before any play. */
void snd_bank_init(void);

/* Preload tiny UI sounds at boot so first taps have zero file latency. */
void snd_bank_prefetch(void);

/* PART 16: preload the WHOLE bank once at boot (late_init) so the control
   loop never mallocs/reads on a first-play. ~370KB PSRAM. */
void snd_bank_preload_all(void);

#ifdef __cplusplus
}
#endif
