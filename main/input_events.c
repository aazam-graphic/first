/*
 * input_events.c - per-key semantic event tracker (premium Car OS guide §14).
 *
 * 15 digital buttons tracked (B_* masks). Sticks/triggers are analog and are
 * handled by the drive logic, not here.
 */
#include "input_events.h"
#include "car_global.h"       /* B_* button masks */

#define IE_NUM_KEYS 16        /* masks 0x0001 .. 0x8000 */

typedef struct {
    bool     down;            /* debounced current state       */
    bool     prev;            /* state before this update      */
    bool     edge_down;       /* press edge this frame         */
    bool     edge_up;         /* release edge this frame       */
    uint32_t press_ms;
    bool     hold_done;       /* hold event already emitted    */
    bool     consumed;        /* event consumed -> no tap      */
    bool     has_tap;         /* pending tap waiting to be read*/
    bool     has_double;      /* two taps within IE_MAX_TAP_MS */
    uint32_t last_tap_ms;     /* release time of previous tap  */
    uint32_t rep_next;        /* next repeat timestamp         */
} ie_key_t;

static ie_key_t s_keys[IE_NUM_KEYS];
static uint32_t s_now_ms;         /* timestamp of the last input_events_update */

static ie_key_t *key_for(uint16_t bit)
{
    int idx = 0;
    while (bit && !(bit & 1u)) { bit >>= 1; idx++; }
    if (idx >= IE_NUM_KEYS) return NULL;
    return &s_keys[idx];
}

void input_events_update(uint16_t dig, uint32_t now)
{
    s_now_ms = now;
    for (uint16_t bit = 1, i = 0; i < IE_NUM_KEYS; i++, bit <<= 1) {
        ie_key_t *k = &s_keys[i];
        bool raw = (dig & bit) != 0;
        k->edge_down = false;
        k->edge_up   = false;
        k->prev = k->down;
        if (raw && !k->prev) {
            /* fresh press */
            k->down = true;
            k->press_ms = now;
            k->hold_done = false;
            k->consumed = false;
            k->has_tap = false;
            k->has_double = false;
            k->rep_next = now + IE_REPEAT_DELAY;
            k->edge_down = true;
        } else if (!raw && k->prev) {
            /* release */
            k->down = false;
            k->edge_up = true;
            /* tap is eligible only when no hold fired and nothing consumed */
            if ((now - k->press_ms) <= IE_MAX_TAP_MS &&
                !k->hold_done && !k->consumed) {
                /* double-tap: this tap released within IE_MAX_TAP_MS of the
                   previous tap's release (guide 14) */
                if (k->last_tap_ms && (now - k->last_tap_ms) <= IE_MAX_TAP_MS)
                    k->has_double = true;
                k->last_tap_ms = now;
                k->has_tap = true;
            } else {
                k->has_tap = false;
                k->last_tap_ms = 0;
            }
        }
    }
}

bool ev_press(uint16_t key)
{
    ie_key_t *k = key_for(key);
    if (!k) return false;
    if (k->edge_down) { k->edge_down = false; return true; }
    return false;
}

bool ev_release(uint16_t key)
{
    ie_key_t *k = key_for(key);
    if (!k) return false;
    if (k->edge_up) { k->edge_up = false; return true; }
    return false;
}

bool ev_hold(uint16_t key, uint32_t ms)
{
    ie_key_t *k = key_for(key);
    if (!k) return false;
    if (k->down && !k->hold_done && (s_now_ms - k->press_ms) >= ms) {
        k->hold_done = true;
        return true;
    }
    return false;
}

bool ev_tap(uint16_t key)
{
    ie_key_t *k = key_for(key);
    if (!k) return false;
    if (k->has_tap) {
        k->has_tap = false;
        k->consumed = true;      /* tap claimed this whole press */
        return true;
    }
    return false;
}

/* Double-tap: two taps released within IE_MAX_TAP_MS of each other (guide 14).
   Consumable; check BEFORE ev_tap so the burst replaces the second toggle. */
bool ev_double(uint16_t key)
{
    ie_key_t *k = key_for(key);
    if (!k) return false;
    if (k->has_double) {
        k->has_double = false;
        k->has_tap    = false;
        k->consumed   = true;
        k->last_tap_ms = 0;      /* re-arm: next pair starts fresh */
        return true;
    }
    return false;
}

bool ev_repeat(uint16_t key, uint32_t delay_ms, uint32_t interval_ms)
{
    ie_key_t *k = key_for(key);
    if (!k) return false;
    if (!k->down) return false;
    if (k->rep_next == 0) k->rep_next = k->press_ms + delay_ms;
    if ((s_now_ms - k->press_ms) < delay_ms) return false;
    if ((int32_t)(s_now_ms - k->rep_next) >= 0) {
        k->rep_next = s_now_ms + interval_ms;
        return true;
    }
    return false;
}

bool ev_held(uint16_t key, uint32_t ms)
{
    ie_key_t *k = key_for(key);
    if (!k) return false;
    return k->down && (s_now_ms - k->press_ms) >= ms;
}

bool input_pressed(uint16_t key)
{
    ie_key_t *k = key_for(key);
    return k ? k->down : false;
}

void input_consume(uint16_t key)
{
    ie_key_t *k = key_for(key);
    if (!k) return;
    k->consumed   = true;
    k->has_tap    = false;
    k->has_double = false;
}

void input_consume_all(void)
{
    for (int i = 0; i < IE_NUM_KEYS; i++) {
        /* GUIDE bypasses the router (9.0 RULE 4): an in-flight GUIDE press
           must survive menu navigation, exactly like the context flush. */
        if (((uint16_t)(1u << i) & (uint16_t)B_GUIDE) != 0)
            continue;
        s_keys[i].consumed   = true;
        s_keys[i].has_tap    = false;
        s_keys[i].has_double = false;
    }
}

/* AZAM CAR OS 9.0 Part 3 RULE 2: the router (car.c input_handle) calls this
   exactly once per os_context() change. GUIDE (0x0400) is skipped — it
   bypasses the router entirely (RULE 4), so an in-flight GUIDE hold/tap
   must never be disturbed by a context switch it may itself have caused. */
void input_flush_context_switch(void)
{
    for (int i = 0; i < IE_NUM_KEYS; i++) {
        if (((uint16_t)(1u << i) & (uint16_t)B_GUIDE) != 0)
            continue;                       /* GUIDE: router never owns it */
        ie_key_t *k = &s_keys[i];
        k->edge_down  = false;              /* stale MENU press invisible   */
        k->edge_up    = false;              /* stray release is not an event*/
        k->has_tap    = false;              /* release must not become tap  */
        k->has_double = false;
        k->last_tap_ms = 0;                 /* no pairing across contexts   */
        k->rep_next    = 0;
        k->consumed    = true;              /* release discarded, not a tap */
        if (k->down)
            k->hold_done = true;            /* held-through gets no hold    */
    }
}
