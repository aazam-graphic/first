/*
 * xbox360.h - Xbox 360 (wireless receiver / wired-style clone) USB host driver
 *
 * Protocol ported from felis/USB_Host_Shield_2.0 (XBOXRECV / XBOXUSB) by
 * Kristian Lauszus (GPL-2.0). Handles the Redgear Pro dongle family that
 * enumerates as 045E:028E / 045E:0719 vendor-class interrupt devices.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define XBOX360_MAX_PADS 4

typedef struct {
    bool present;         /* controller currently connected in this slot */
    uint32_t buttons;     /* XBOXRECV-compatible:
                             bits 16..31 = digital buttons (XInput order)
                             bits 8..15  = left trigger (LT)
                             bits 0..7   = right trigger (RT) */
    uint8_t lt, rt;       /* trigger values 0..255 */
    int16_t lx, ly;       /* left stick, signed 16-bit */
    int16_t rx, ry;       /* right stick, signed 16-bit */
    uint8_t battery;      /* 0..3 */
    bool battery_valid;   /* true once a real battery report arrived
                             (level 0 at fresh pairing = UNKNOWN, not low) */
} xbox360_pad_t;

typedef struct {
    uint8_t slot;               /* 0..3 */
    bool connect_change;        /* presence changed this report */
    xbox360_pad_t pad;          /* parsed state AFTER this report */
    uint8_t raw[64];            /* raw report bytes (diagnostic) */
    uint8_t raw_len;
} xbox360_evt_t;

/* Client task entry (USB Host Library must be installed first) */
void xbox360_task(void *arg);

/* Latest state accessor for application code (motor control, etc.) */
const xbox360_pad_t *xbox360_pad(uint8_t slot);
bool xbox360_dongle_connected(void);
uint8_t xbox360_layout(void);   /* 1 = wired-style, 4 = wireless receiver */

/* Haptic feedback — 0..255 each motor (left=low freq, right=high freq) */
void xbox360_rumble(uint8_t slot, uint8_t left, uint8_t right);

/* LED ring (PART 13.1, wireless receiver only - wired clones ignore).
   Wire format {00 00 08 V} on the command channel (same path as rumble).
   V table: 0x00 = off (standard). Quadrant-ON values follow the
   connect-proven 0x0C+8k formula (slot quadrant lights at connect);
   LED_ALL extrapolates it - VERIFY visually on hardware, one-line fix
   if a pattern shows wrong. App flashes/pulses via timed ON/OFF (no
   pattern-byte risk for blink rates). */
typedef enum {
    XBOX_LED_OFF = 0,
    XBOX_LED_Q1,
    XBOX_LED_Q2,
    XBOX_LED_Q3,
    XBOX_LED_Q4,
    XBOX_LED_ALL,
    XBOX_LED_COUNT
} xbox360_led_pattern_t;
void xbox360_set_led(uint8_t slot, xbox360_led_pattern_t pattern);

/* Chauffeur Mode (PART 13.5, FUTURE - documented only, not implemented):
   xbox360.c already supports 4 pads. Slot 0 = Driver, full control
   (unchanged). Slot 1, if paired = Passenger: horn + lights + roof ONLY,
   no throttle/steer/brake access, driver inputs always override. */

/* Timestamp (ms) of last successful USB data from controller — 0 if none yet.
   Use this for "controller alive" detection; it updates on every report,
   even idle ones (unlike button/stick activity which needs actual input). */
uint32_t xbox360_last_data_ms(void);