/*
 * cargame_neon_convoy.h
 * NEON CONVOY: SENTINEL RUN  -  Car OS Games Hub, Game 7
 * Top-down cyberpunk convoy-defense shooter for 320x240 TFT.
 * SCREEN-ONLY: never touches motors/UART/GPIO/NVS/Wi-Fi/estop. Car stays parked.
 */
#ifndef CARGAME_NEON_CONVOY_H
#define CARGAME_NEON_CONVOY_H
#include <stdint.h>
#include <stdbool.h>
#include "xbox360.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NCR_MAX_ENEMIES        12
#define NCR_MAX_PLAYER_BULLETS 20
#define NCR_MAX_ENEMY_BULLETS  20
#define NCR_MAX_PICKUPS         8
#define NCR_MAX_MINES           8
#define NCR_MAX_PARTICLES      20

void ncr_init(void);
void ncr_update(const xbox360_pad_t *pad, uint32_t now_ms);
void ncr_draw(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
#endif /* CARGAME_NEON_CONVOY_H */