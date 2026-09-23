#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t display_driver_init(void); // init LVGL + SPI DMA ST7789 320x240
void display_driver_deinit(void);
#ifdef __cplusplus
}
#endif
