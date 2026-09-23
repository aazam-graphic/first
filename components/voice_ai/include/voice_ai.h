/*
 * voice_ai.h — top-level XiaoZhi voice-AI orchestrator.
 *
 * Brings up Wi-Fi, connects to xiaozhi.me, bridges I2S mic RX to the
 * ASR/LLM/TTS pipeline, plays TTS back through the shared MAX98357A amp,
 * and exposes MCP tools so the LLM can (safely) inspect and drive the car.
 *
 * The voice pipeline runs on Core 1, never blocking the motor loop.
 */

#ifndef VOICE_AI_H
#define VOICE_AI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- public state shown on the TFT chat screen -------------------- */
typedef enum {
    VOICE_STATE_OFFLINE = 0,
    VOICE_STATE_READY,
    VOICE_STATE_LISTENING,
    VOICE_STATE_PROCESSING,
    VOICE_STATE_SPEAKING,
    VOICE_STATE_ERROR,
} voice_ai_state_t;

/* ---- user callbacks (optional, set before voice_ai_init) ----------- */
typedef void (*voice_state_cb_t)(voice_ai_state_t old_st, voice_ai_state_t new_st,
                                 void *user_ctx);
typedef void (*voice_text_cb_t)(const char *text, bool is_tts, void *user_ctx);

/* ---- configuration ------------------------------------------------- */
typedef struct {
    const char        *wifi_ssid;       /* NULL = skip Wi-Fi              */
    const char        *wifi_pass;       /* NULL = open network            */
    const char        *xiaozhi_url;     /* NULL = default xiaozhi.me      */
    bool               use_mqtt;        /* try MQTT+UDP before WS          */
    voice_state_cb_t   on_state;        /* optional state-change hook      */
    void              *on_state_ctx;
    voice_text_cb_t    on_text;         /* optional transcript/TTS text    */
    void              *on_text_ctx;
} voice_ai_config_t;

#define VOICE_AI_DEFAULT_CONFIG() { \
    .wifi_ssid = NULL, .wifi_pass = NULL, .xiaozhi_url = NULL, \
    .use_mqtt = true, .on_state = NULL, .on_state_ctx = NULL, \
    .on_text = NULL, .on_text_ctx = NULL }

/* ---- lifecycle ------------------------------------------------------ */
esp_err_t voice_ai_init(const voice_ai_config_t *cfg);
esp_err_t voice_ai_start(void);
void      voice_ai_stop(void);
void      voice_ai_shutdown(void);

/* ---- query ---------------------------------------------------------- */
voice_ai_state_t voice_ai_get_state(void);
const char      *voice_ai_state_str(voice_ai_state_t st);

/* ---- user-facing triggers (call from ISR-safe context) ------------- */
void voice_ai_request_listen(void);     /* wake word / button             */
void voice_ai_request_stop(void);       /* abort listening / speaking     */
void voice_ai_request_abort_speak(void);/* stop TTS playback              */

/* ---- MCP tool engine (registered with esp_xiaozhi ------------------ */
void *voice_ai_get_mcp_engine(void);

/* ---- shutdown flag used by I2S driver ------------------------------ */
extern volatile bool g_voice_shutdown;

#ifdef __cplusplus
}
#endif

#endif /* VOICE_AI_H */
