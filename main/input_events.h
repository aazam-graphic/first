/*
 * input_events.h - semantic button event layer (premium Car OS guide §14).
 *
 * Replaces scattered raw dig/tap + per-button timers with a single per-key
 * tracker producing press / release / tap / hold / repeat events. Tap and hold
 * are mutually exclusive (hold fires -> release never produces a tap) and every
 * event is consumable so two subsystems can never both react to one press.
 *
 * Call input_events_update(dig, now) once per loop, then query.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* recommended timings (guide §14) */
#define IE_MAX_TAP_MS     350
#define IE_STD_HOLD_MS    700
#define IE_AUTO_HOLD_MS   1000
#define IE_DANGER_HOLD_MS 1500
#define IE_ESTOP_CHORD_MS 2000
#define IE_REPEAT_DELAY   350
#define IE_REPEAT_INTERVAL 120

typedef enum {
    KEY_PRESS = 0,
    KEY_RELEASE,
    KEY_TAP,
    KEY_HOLD,
    KEY_REPEAT,
    KEY_DOUBLE_TAP,
    KEY_CHORD,
} key_event_type_t;

typedef struct {
    uint16_t      key;
    key_event_type_t type;
    uint32_t      held_ms;
} key_event_t;

/* Feed the raw digital mask (pad->buttons >> 16) every control loop. */
void input_events_update(uint16_t dig, uint32_t now_ms);

/* Edge events (one-shot, consumable - first caller wins). */
bool ev_press(uint16_t key);      /* key went down this frame      */
bool ev_release(uint16_t key);    /* key came up this frame        */

/* One-shot hold: fires exactly once after the key has been held >= ms.
   Once fired, the release will NOT produce a tap (mutual exclusion). */
bool ev_hold(uint16_t key, uint32_t ms);

/* Tap: key pressed and released within IE_MAX_TAP_MS and no hold fired.
   Consumable - cleared once read. */
bool ev_tap(uint16_t key);

/* Double-tap: two taps released within IE_MAX_TAP_MS of each other (guide 14).
   Consumable; query BEFORE ev_tap so the double replaces the second toggle. */
bool ev_double(uint16_t key);

/* Repeat: after held >= delay then every interval (as long as held). */
bool ev_repeat(uint16_t key, uint32_t delay_ms, uint32_t interval_ms);

/* Continuous (not one-shot): true while held >= ms. */
bool ev_held(uint16_t key, uint32_t ms);

/* Currently debounced-down state. */
bool input_pressed(uint16_t key);

/* Consumption: mark a key consumed so its pending tap/hold cannot fire. */
void input_consume(uint16_t key);
void input_consume_all(void);

/* AZAM CAR OS 9.0 Part 3 RULE 2 — context-switch flush: call once when
   os_context() changes. Any button already held at the switch moment had
   its PRESS consumed by the OLD context, so the NEW context must see
   neither its release-as-tap, its hold, its double-tap pairing, nor its
   stale press/release edges — only a fresh RELEASE + next PRESS may act.
   GUIDE is deliberately excluded (9.0 RULE 4: it bypasses the router). */
void input_flush_context_switch(void);

#ifdef __cplusplus
}
#endif
