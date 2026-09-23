/* ------------------------- AUTO helpers (guide §13) ----------------------- */
static bool auto_can_enter(const xbox360_pad_t *pad)
{
    if (g.estop) return false;
    if (!car_us_front_ok()) return false;                 /* sensors valid  */
    if (g.cur_l || g.cur_r || g.tgt_l || g.tgt_r) return false; /* stationary */
    if (!pad) return false;
    if (stick_dz(pad->lx) || stick_dz(pad->ly) ||
        stick_dz(pad->rx) || stick_dz(pad->ry)) return false;  /* neutral    */
    if (pad->rt > 5 || pad->lt > 5) return false;
    return true;
}

static void auto_enter(uint32_t now)
{
    g.mode = MODE_AUTO;
    g.ast  = AST_CRUISE;
    g.ast_t0 = now;
    g.prog_f = 65535;
    g.prog_t = now;
    g.esc_phase = 0;
    g.hazard = false;
    g.headlight = true;
    g.stuck_cnt = 0;
    g.search_dir = 0;
    g.dist_idx = 0;
    for (int i = 0; i < 3; i++) {
        g.dist_avg[i] = g.dist[i];
        for (int j = 0; j < 3; j++) g.dist_buf[i][j] = g.dist[i];
    }
    s_blip_until = now + 60;
    s_blip_freq = 1400;
    rumble(40, 80, 80);
}

static void auto_exit(uint32_t now)
{
    g.mode = MODE_MANUAL;
    g.ast  = AST_CRUISE;
    g.hazard = false;
    s_blip_until = now + 60;
    s_blip_freq = 1000;
    rumble(100, 100, 150);
    ESP_LOGI(TAG, "AUTO exited (manual)");
}

/* premium guide §4: clear e-stop only via START+BACK hold 2s + LT held +
   stationary. No single-button clear. */
static void estop_input_handle(const xbox360_pad_t *pad, uint32_t now)
{
    static uint32_t chord_t0 = 0;
    bool lt_held    = pad && pad->lt > 200;
    bool chord      = input_pressed(B_START) && input_pressed(B_BACK);
    bool stationary = (g.cur_l == 0 && g.cur_r == 0 && g.tgt_l == 0 && g.tgt_r == 0);

    if (chord && lt_held) {
        if (chord_t0 == 0) {
            chord_t0 = now;
        } else if (now - chord_t0 >= IE_ESTOP_CHORD_MS && stationary) {
            g.estop = false;
            ESP_LOGI(TAG, "ESTOP cleared (START+BACK 2s + LT + stationary)");
            rumble(80, 80, 100);
            chord_t0 = 0;
            input_consume_all();
        }
    } else {
        chord_t0 = 0;
    }
}