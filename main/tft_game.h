#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "xbox360.h"

void game_init(void);
bool game_is_active(void);
void game_toggle(void); // BACK+START to toggle
void game_update(const xbox360_pad_t *pad, uint32_t now_ms); // call at 30Hz from car_task
void game_draw(void); // draws to TFT (call after update)
