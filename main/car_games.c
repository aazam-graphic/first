/*
 * car_games.c - Car OS games registry.
 * Purane 5 games + Redline Ops DELETED (user request) - sirf NEON CONVOY.
 */
#include "car_games.h"
#include "cargame_neon_serpent.h"
#include "cargame_neon_convoy.h"

const os_game_t OS_GAMES[OS_GAME_COUNT] = {
    { "NEON CONVOY",  "SENTINEL", ncr_init, ncr_update, ncr_draw },
    { "NEON SERPENT", "SNAKE",   g7_init,  g7_update,  g7_draw  },
};

void car_games_all_init(void)
{
    /* per-game init runs when a game starts from the hub */
}