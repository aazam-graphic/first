/*
 * os_oled.h - Car OS OLED layout controller (SSD1306 128x64 via car.c driver).
 */
#pragma once
#include <stdint.h>
#include "car_os.h"

#ifdef __cplusplus
extern "C" {
#endif

void os_oled_apply(const os_ctx_t *ctx, uint32_t now);  /* throttled ~12 FPS */

#ifdef __cplusplus
}
#endif