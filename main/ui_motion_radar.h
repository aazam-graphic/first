/*
 * ui_motion_radar.h - Premium tactical Motion Radar UI for the ESP32-S3 TFT.
 *
 * Draws a large visual-only parking radar + tactical motion display into the
 * existing Car OS DMA framebuffer (via os_gfx primitives). No raw sensor
 * values are shown on the default screen; D-pad Right opens a separate
 * DETAILS page that alone shows raw technical data.
 *
 * Rendering is fully self-contained. Frame pushing is handled by the Car OS
 * render flow (os_draw_tft -> gfx_push), so this module only marks dirty.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Draw one Motion Radar frame into the global TFT framebuffer.
   now_ms  : monotonic ms (drives scan / pulse animations).
   details : true => draw the technical DETAILS page (raw values);
             false => draw the visual-only radar.
   cal_on  : true for ~1.2s after a controller A (CAL) press - shows a brief
             self-check indicator on the radar. */
void ui_motion_radar_draw(uint32_t now_ms, bool details);

#ifdef __cplusplus
}
#endif