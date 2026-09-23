/*
 * os_oled.c - Car OS OLED layouts:
 *   0 MINI HUD : classic car HUD (reuses car.c hud_update render)
 *   1 RADAR    : L/F/R distance bars + speed + gear
 *   2 TEXT     : custom message
 * While the OS is "parked" (menus/games), a status screen is shown instead.
 */
#include "os_oled.h"
#include "os_gfx.h"
#include "oled_driver.h"
#include "car_global.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void os_oled_apply(const os_ctx_t *ctx, uint32_t now)
{
    static uint32_t s_last = 0;
    if (now - s_last < 80) return;             /* ~12 FPS */
    s_last = now;

    bool parked = os_parked();

    /* layout 0 + driving = proven legacy HUD straight from car.c */
    if (!parked && ctx->oled_layout == 0) {
        car_oled_hud();
        return;
    }

    char line[24];
    osd_oled_clear();

    if (parked) {
        /* OS status screen */
        const char *name = os_state_name(ctx->state);
        int w = (int)strlen(name) * 12;
        osd_oled_text_big((128 - w) / 2, 8, name, 2);
        if (os_game_active()) {
            int spd = (abs(g.cur_l) + abs(g.cur_r)) / 2;
            snprintf(line, sizeof(line), "SCORE RUN  B:EXIT");
            osd_oled_text(0, 40, line);
            snprintf(line, sizeof(line), "SPD %d%%", spd);
            osd_oled_text(0, 52, line);
        } else {
            osd_oled_text(0, 40, "CAR PAUSED");
            snprintf(line, sizeof(line), "BL:1 NO BATT");
            osd_oled_text(0, 52, line);
        }
        osd_oled_flush();
        return;
    }

    switch (ctx->oled_layout) {
    case 1: { /* RADAR */
        int spd = (abs(g.cur_l) + abs(g.cur_r)) / 2;
        snprintf(line, sizeof(line), "SPD %3d%%  G%u", spd, (unsigned)(g.gear + 1));
        osd_oled_text(0, 0, line);
        static const char *dn[3] = { "L", "F", "R" };
        for (int i = 0; i < 3; i++) {
            uint16_t d = g.dist_avg[i];
            bool ok = d != 65535;
            int v = ok ? (d > 100 ? 100 : d) : 100;
            int y = 12 + i * 14;
            osd_oled_text(0, y, dn[i]);
            osd_oled_rect(10, y, 10 + (100 - 10) * v / 100, y + 8, true);
            osd_oled_rect(10, y, 110, y + 8, false);
            snprintf(line, sizeof(line), ok ? "%3u" : " --", ok ? d : 0);
            osd_oled_text(114, y, line);
        }
        snprintf(line, sizeof(line), "NO BATT");
        osd_oled_text(0, 54, line);
        break;
    }
    case 2: { /* TEXT */
        int w = (int)strlen(ctx->oled_text) * 12;
        int x = (128 - w) / 2;
        if (x < 0) x = 0;
        osd_oled_text_big(x, 12, ctx->oled_text, 2);
        int spd = (abs(g.cur_l) + abs(g.cur_r)) / 2;
        uint16_t f = g.dist_avg[1];
        snprintf(line, sizeof(line), "SPD %3d%%  F %s", spd, f == 65535 ? "--" : "");
        osd_oled_text(0, 44, line);
        if (f != 65535) {
            snprintf(line, sizeof(line), "%u", f);
            osd_oled_text(100, 44, line);
        }
        snprintf(line, sizeof(line), "NO BATT");
        osd_oled_text(0, 54, line);
        break;
    }
    case 3: { /* STATUS */
        osd_oled_text_big(20, 10, "CAR OS", 2);
        snprintf(line, sizeof(line), "STATE %s", os_state_name(ctx->state));
        osd_oled_text(0, 40, line);
        snprintf(line, sizeof(line), "SPD %d%%", (abs(g.cur_l) + abs(g.cur_r)) / 2);
        osd_oled_text(0, 52, line);
        break;
    }
    default: /* MINI HUD fallback if parked==false but layout unknown */
        osd_oled_flush();
        car_oled_hud();
        return;
    }
    osd_oled_flush();
}