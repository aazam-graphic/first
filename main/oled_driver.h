/*
 * oled_driver.h - non-static wrappers around the SSD1306 driver that lives in
 * car.c. Car OS (os_oled.c) renders through these; control code untouched.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* (declared in car_global.h too; kept here for clarity of ownership) */

#ifdef __cplusplus
}
#endif