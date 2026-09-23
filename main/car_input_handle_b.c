/* Y: tap = next ambient preset, hold 700ms = static color cycle (§5.2) */
    if (ev_tap(B_Y)) {
        static uint8_t s_rgb = 0;
        const uint8_t cols[6][3] = {{255,0,0},{0,255,0},{0,0,255},{255,255,0},{255,0,255},{255,255,255}};
        if (g.led_mode == 0) { g.led_mode = 1; s_rgb = 0; }
        else if (g.led_mode == 1) { s_rgb = (uint8_t)((s_rgb + 1) % 6); if (s_rgb == 0) g.led_mode = (uint8_t)((g.led_mode + 1) % 5); }
        else { g.led_mode = 1; s_rgb = 0; }
        extern uint8_t g_static_r, g_static_g, g_static_b;
        g_static_r = cols[s_rgb][0]; g_static_g = cols[s_rgb][1]; g_static_b = cols[s_rgb][2];
        play_shot(_binary_yclick_wav_start, _binary_yclick_wav_end, 800);
        rumble(0, 100, 80);
    }
    if (ev_hold(B_Y, IE_STD_HOLD_MS)) {
        static uint8_t s_rgb2 = 0;
        const uint8_t cols2[6][3] = {{255,0,0},{0,255,0},{0,0,255},{255,255,0},{255,0,255},{255,255,255}};
        extern uint8_t g_static_r, g_static_g, g_static_b;
        g_static_r = cols2[s_rgb2][0]; g_static_g = cols2[s_rgb2][1]; g_static_b = cols2[s_rgb2][2];
        s_rgb2 = (uint8_t)((s_rgb2 + 1) % 6);
        g.led_mode = 1;
        input_consume(B_Y);
        play_shot(_binary_yclick_wav_start, _binary_yclick_wav_end, 800);
        rumble(0, 100, 80);
    }

    /* LS/RS click */
    if (tap & B_LS) {
        if (g.mode == MODE_MANUAL)      g.mode = MODE_CRAWL;
        else if (g.mode == MODE_CRAWL)  g.mode = MODE_MANUAL;
        s_blip_until = now + 60;
        s_blip_freq = 1100;
    }
    if (tap & B_RS) g.cannon_reset = true;

    /* D-pad */
    if (tap & B_DUP)    { g.hazard = !g.hazard; s_blip_until = now + 60; s_blip_freq = 1200; }
    if (tap & B_DLEFT)  { g.sig_l = !g.sig_l;  g.sig_r = false; }
    if (tap & B_DRIGHT) { g.sig_r = !g.sig_r;  g.sig_l = false; }
    g.gripper_open = (dig & B_DDOWN) != 0;
    if (tap & B_DDOWN) {
        g.grip_hit_ms = now;
        play_shot(_binary_gripper_wav_start, _binary_gripper_wav_end, 650);
        rumble(0, 255, 120);
    }

    /* LB hold = continuous horn */
    s_horn_on = (dig & B_LB) != 0;

    /* RB hold = turbo, 3s max, phir 5s cooldown */
    {
        bool rb = (dig & B_RB) != 0;
        if (rb && !g.cooldown && g.mode != MODE_AUTO) {
            if (!g.turbo) {
                s_sweep_until = now + 250;
                s_sweep_f0 = 400;
                s_sweep_f1 = 1500;
                s_sweep_amp = 4500;
                rumble(180, 180, 250);
            }
            g.turbo = true;
            g.turbo_ms += dt_ms;
            if (g.turbo_ms >= 3000) {
                g.cooldown = true;
                g.turbo = false;
                g.turbo_ms = 0;
                g.cooldown_until = now + 5000;
            }
        } else {
            g.turbo = false;
            if (!g.cooldown) g.turbo_ms = 0;
        }
        if (!g.turbo && s_was_turbo) {
            s_sweep_until = now + 250;
            s_sweep_f0 = 1400;
            s_sweep_f1 = 350;
            s_sweep_amp = 4000;
        }
        s_was_turbo = g.turbo;
        if (g.cooldown && now >= g.cooldown_until) g.cooldown = false;
    }

    /* START/BACK live in car_os (OS_DRIVE_MAIN): START tap = Home launcher,
       START hold = Quick Settings, BACK tap = HUD layout, BACK hold = night. */

    /* input activity tracker */
    if (tap || (pad && (stick_dz(pad->lx) || stick_dz(pad->ly) ||
                        stick_dz(pad->rx) || stick_dz(pad->ry) ||
                        pad->rt > 5 || pad->lt > 5))) {
        g.last_input_ms = now;
    }
}