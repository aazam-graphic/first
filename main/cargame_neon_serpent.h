/*
 * cargame_neon_serpent.h - NEON SERPENT: GRID BREACH (Car OS game 7)
 * Cyberpunk grid-snake with enemies, EMP/dash, mines and a boss.
 * Rendering by ns_theme (draw-only theme). B = exit (framework-owned).
 */
#pragma once
#include <stdint.h>
#include "xbox360.h"

#ifdef __cplusplus
extern "C" {
#endif

void g7_init(void);
void g7_update(const xbox360_pad_t *pad, uint32_t now);
void g7_draw(uint32_t now);

#ifdef __cplusplus
}
#endif
