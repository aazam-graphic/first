/*
 * mic_in.h - PART 8 MIC SYSTEM (M1/M2/M3).
 *
 * MEMS mic (INMP441-class) on I2S1, 16-bit mono 16kHz, user-confirmed pins:
 *   SCK = GPIO13, WS = GPIO14, SD = GPIO48.
 * (GPIO14 backlight relay was removed in hardware; pin repurposed for mic.)
 *
 * M1: 64KB PSRAM ring, RMS envelope -> roof MUSIC MODE beat flash (<80ms),
 *     DIAG>SENS live VU meter (RMS sampled every 50ms).
 * M2: double-clap (parked only, caller-enforced) -> headlight toggle.
 * M3 (future foundation): 3s listen buffer into PSRAM, no processing yet.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIC_PIN_SCK   13
#define MIC_PIN_WS    14
#define MIC_PIN_SD    48
#define MIC_SAMPLE_HZ 16000

void mic_init(void);                 /* I2S1 + ring + capture task (idempotent) */
bool mic_ready(void);
uint32_t mic_stack_free(void);       /* PART 16 mem_diag */

/* Call every car_task tick. Samples VU at 50ms cadence, runs beat + clap. */
void mic_poll(uint32_t now_ms);

/* M1: live levels */
uint16_t mic_rms(void);              /* last RMS 0..32767 */
uint16_t mic_vu(void);               /* 50ms-cadence VU 0..100 */
uint16_t mic_peak(void);

/* M1 beat: true once per threshold-cross (roof MUSIC flash, <80ms path) */
bool mic_beat(void);

/* M2 clap: true once per double-clap (caller must verify parked) */
bool mic_clap(void);

/* M3 listen buffer (3s future foundation) */
bool mic_listen_start(void);         /* false if already recording */
void mic_listen_stop(void);
bool mic_listening(void);
uint32_t mic_listen_ms(void);        /* recorded duration */
size_t mic_listen_read(uint8_t *buf, size_t max_len);  /* peek-copy, no consume */

#ifdef __cplusplus
}
#endif
