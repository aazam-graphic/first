/* SPDX-License-Identifier: Apache-2.0 */
/* alexa_bridge.h - Amazon Alexa cloud bridge + safety gateway (firmware side).
 *
 * Rule zero: Alexa never touches motors, never bypasses E-stop. Every command
 * from MQTT passes the safety gateway; execution happens ONLY via tested
 * Car OS paths in car_task context. Blocked commands are rejected with a
 * reason the Lambda skill turns into a polite refusal.
 *
 * Threading: alexa_task (core 1, prio 1) owns MQTT + JSON + history.
 * Hardware actions are posted to a single pending slot consumed by
 * alexa_apply_pending() inside os_update() (car_task owns globals there).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "car_os.h"   /* os_ctx_t for apply/input/draw hooks */

#ifdef __cplusplus
extern "C" {
#endif

/* Result badge shared by TFT banner + voice history */
typedef enum {
    ALEXA_RES_NONE = 0,
    ALEXA_RES_PROCESSING,     /* yellow: sent to car, awaiting apply */
    ALEXA_RES_DONE,           /* green */
    ALEXA_RES_REJECTED,       /* red: + reason */
    ALEXA_RES_NEEDS_CONFIRM,  /* orange: A/B dialog open on car */
    ALEXA_RES_CANCELLED,      /* grey: confirm timeout / B pressed */
} alexa_result_t;

/* Bridge task: WiFi wait -> MQTT/TLS -> subscribe cmd topic -> serve.
 * Starts net_wifi itself. Safe to create once from app_main. */
void alexa_task(void *arg);

/* car_task context (call ONLY from os_update): execute one validated
 * pending action + confirm-execution, fill result slot for the bridge. */
void alexa_apply_pending(os_ctx_t *ctx, uint32_t now);

/* Input hook (call from os_handle_input after E-stop check, before other
 * handling). Returns true when A/B confirm dialog consumed the input. */
bool alexa_confirm_handle_input(uint16_t dig, uint16_t tap, uint32_t now);

/* TFT overlays (call from os_draw_tft; self-expire, zero cost when idle) */
void alexa_banner_draw(void);    /* live "ALEXA heard/action/result" banner */
void alexa_confirm_draw(void);   /* blocking A/B confirm dialog */
bool alexa_overlay_active(void); /* banner visible OR confirm open */

/* Voice History screen body (OS_ALEXA_LOG state) */
void os_scr_alexa_log(const os_ctx_t *ctx, uint32_t now);

/* MPU-advanced (1.md 4.1/4.3): HARD impact announce. Called from car_task
   via the imu_adv event bus. Never touches motors, never clears lockouts. */
void alexa_impact_note(uint8_t level, float g, uint32_t now_ms);

/* PART 9.5 proactive announcements (MPU events). Quiet-hours 22:00-07:00 UTC
   suppress non-critical texts (blueprint rule); critical always speaks.
   Safe offline (no-op without MQTT). */
void alexa_announce(const char *text, bool critical);

/* PART 10 QCC profile dropdown (same validated path as voice, with NVS). */
bool alexa_request_profile(uint8_t p);

/* PART 14: lifetime odometer meters (NVS, accumulates per drive-end). */
uint32_t alexa_odo_m(void);

/* Helpers for UI / diagnostics */
const char *alexa_profile_name(void);  /* "safe"/"night"/.../"manual" */
bool        alexa_mqtt_connected(void);
const char *alexa_result_str(alexa_result_t r);

#ifdef __cplusplus
}
#endif
