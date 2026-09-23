/*
 * voice_audio_i2s.h — I2S microphone RX driver for XiaoZhi voice AI.
 *
 * Captures 16-bit mono PCM at 16 kHz from a standard I2S MEMS mic on
 * I2S_NUM_1 and hands completed DMA frames to a consumer task via a
 * lock-free single-producer single-consumer ring buffer in PSRAM.
 *
 * The motor-control / car task NEVER blocks on any of these functions.
 */

#ifndef VOICE_AUDIO_I2S_H
#define VOICE_AUDIO_I2S_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- user-configurable constants ------------------------------------- */

#ifndef VOICE_I2S_BCLK_GPIO
#define VOICE_I2S_BCLK_GPIO   13   /* SCK  (user-confirmed)              */
#endif
#ifndef VOICE_I2S_WS_GPIO
#define VOICE_I2S_WS_GPIO     14   /* Word-select / L-R (user-confirmed) */
#endif
#ifndef VOICE_I2S_SD_GPIO
#define VOICE_I2S_SD_GPIO     48   /* Data-in from mic (user-confirmed)  */
#endif

#define VOICE_I2S_SAMPLE_RATE 16000
#define VOICE_I2S_BITS        16
#define VOICE_I2S_CHANNELS    1      /* mono */

/* DMA: 4 descriptors × 240 frames × 2 bytes = ~1.9 KB of DMA-capable RAM */
#define VOICE_I2S_DMA_DESC    4
#define VOICE_I2S_DMA_FRAMES  240

/* Ring buffer holds ~4 s of audio in PSRAM (16000 × 2 B × 4 s = 128 KB) */
#ifndef VOICE_I2S_RING_SIZE
#define VOICE_I2S_RING_SIZE   131072
#endif

/* ---- diagnostics ----------------------------------------------------- */

typedef struct {
    bool     running;              /* true while capture task is live    */
    uint32_t frames_rx;            /* total DMA frames handed to ring    */
    uint32_t overruns;             /* ring buffer overflow count         */
    int      peak_level;           /* last frame peak sample (0..32767)  */
    int      rms_level;            /* last frame RMS (0..32767)          */
    bool     clipping;             /* true if any sample hit full-scale  */
} voice_i2s_diag_t;

/* ---- public API ------------------------------------------------------ */

/** Initialise I2S_NUM_1, DMA, ring buffer. Does NOT start capture. */
esp_err_t voice_audio_i2s_init(void);

/** Start the background capture task. Safe to call once after init. */
esp_err_t voice_audio_i2s_start(void);

/** Stop capture. Does not free resources. */
void      voice_audio_i2s_stop(void);

/**
 * Read captured PCM from the ring buffer.
 * Returns number of bytes actually copied (0 if none available).
 * Never blocks; returns immediately if 'max_len' bytes are not ready.
 */
size_t    voice_audio_i2s_read(uint8_t *buf, size_t max_len, uint32_t timeout_ms);

/** Read without consuming (peek). Returns bytes available. */
size_t    voice_audio_i2s_peek(uint8_t *buf, size_t max_len);

/** Number of PCM bytes currently buffered. */
size_t    voice_audio_i2s_available(void);

/** Clear the ring buffer. */
void      voice_audio_i2s_flush(void);

/** Populate diagnostic snapshot. */
void      voice_audio_i2s_get_diag(voice_i2s_diag_t *out);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_AUDIO_I2S_H */
