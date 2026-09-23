/*
 * car_games.h - Car OS multi-game framework (5 games).
 * Each game owns its init/update/draw; drawing uses os_gfx into the shared
 * framebuffer. B always exits to the Games Hub (handled by car_os.c).
 */
#pragma once
#include <stdint.h>
#include "xbox360.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef OS_GAME_COUNT
#define OS_GAME_COUNT 2      /* 2 games: Neon Convoy, Neon Serpent */
#endif

typedef struct {
    const char *name;
    const char *desc;
    void (*init)(void);
    void (*update)(const xbox360_pad_t *pad, uint32_t now);
    void (*draw)(uint32_t now);
} os_game_t;

extern const os_game_t OS_GAMES[OS_GAME_COUNT];

void car_games_all_init(void);   /* one-time setup (currently a no-op hook) */

#ifdef __cplusplus
}
#endif