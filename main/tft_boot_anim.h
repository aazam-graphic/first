#pragma once
// Dual ignition boot animation for 2.8" TFT 320x240 landscape
// Uses existing tft_display framebuffer (SPI2: SCLK46 MOSI13 CS45 DC0 RST16 BL18)
// Call after tft_init() - runs ~2.7s together with startup wav
void tft_boot_anim(void);
