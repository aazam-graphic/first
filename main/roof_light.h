/*
 * roof_light.h - ROOF RGB LIGHT (PART 7 locked spec).
 *
 * Hardware: WS2812 RGB strip on GPIO18 (RMT TX, non-DMA) — 1 pixel
 * (1 IC controlling 3 LEDs, all three show the same colour).
 *
 * Modes: OFF / POLICE (default, red/blue strobe) / STEADY (selected color) /
 * RAINBOW (hue cycle) / BREATHE (pulse of steady color) / WARNING (amber
 * strobe) / CHASE (brightness chase pulse) / MUSIC (mic-reactive beat flash,
 * slow-pulse fallback when the mic is silent/absent).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Roof strip GPIO (single source of truth - car.c pin guard ise bhi check
   karta hai). WS2812 DIN -> GPIO18, 1 IC / 3 LEDs = 1 pixel. */
#define ROOF_STRIP_GPIO 18

typedef enum {
    ROOF_LIGHT_OFF = 0,
    ROOF_LIGHT_POLICE,     /* red/blue strobe (B-tap default) */
    ROOF_LIGHT_STEADY,     /* solid selected color */
    ROOF_LIGHT_RAINBOW,    /* hue cycle */
    ROOF_LIGHT_BREATHE,    /* brightness pulse of steady color */
    ROOF_LIGHT_WARNING,    /* amber strobe */
    ROOF_LIGHT_CHASE,      /* brightness chase pulse */
    ROOF_LIGHT_MUSIC,      /* mic-reactive beat flash */
    ROOF_LIGHT_COUNT
} roof_light_mode_t;

/* steady-mode color dots (PART 7 bottom sheet COLOR row, steady only) */
#define ROOF_COLOR_COUNT 6   /* R B G C Y W */

typedef struct {
    roof_light_mode_t mode;
    bool    enabled;          /* user wants the roof light on  */
    uint8_t brightness;       /* 0..100 (WS2812 brightness scaling) */
    uint8_t color_idx;        /* 0..5 steady color dot */
    uint16_t period_ms;       /* base blink period             */
    uint32_t changed_ms;      /* when settings last changed    */
} roof_light_state_t;

/* Public API (PART 7) */
void roof_light_init(void);
void roof_light_set_enabled(bool enabled);
void roof_light_set_mode(roof_light_mode_t mode);
void roof_light_set_brightness(uint8_t pct);       /* 0..100 */
void roof_light_set_color_idx(uint8_t idx);        /* 0..5, steady only */
void roof_light_cycle_mode(int dir);               /* B double-tap quick-cycle */
void roof_light_force_safe_off(void);              /* estop/disconnect */
void roof_light_update(uint32_t now_ms);           /* call every loop */
const roof_light_state_t *roof_light_state(void);

/* short human label for the drive HUD badge */
const char *roof_light_label(void);
/* steady color RGB for a dot index */
void roof_light_color_rgb(uint8_t idx, uint8_t *r, uint8_t *g, uint8_t *b);
const char *roof_light_color_name(uint8_t idx);

#ifdef __cplusplus
}
#endif
