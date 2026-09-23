/* ============================== input/keys ================================ */

static void input_handle(const xbox360_pad_t *pad, uint16_t dig, uint16_t tap, uint32_t now)
{
    /* premium guide §14: unified semantic event layer first. */
    input_events_update(dig, now);

    static bool s_was_turbo;                /* RB turbo edge for blowoff */
    static bool s_mist_tog;                 /* A hold = mist toggle      */
    static uint32_t last_now;
    uint32_t dt_ms = last_now ? now - last_now : LOOP_MS;
    last_now = now;
    if (dt_ms > 100) dt_ms = 100;           /* clamp */

    /* ==== PRIORITY 1 (guide §15): GUIDE tap = global e-stop, never Home ==== */
    if (ev_tap(B_GUIDE)) {
        g.estop = true;
        g.mode = MODE_MANUAL;
        g.ast = AST_CRUISE;
        g.tgt_l = g.tgt_r = 0; g.cur_l = g.cur_r = 0;
        g.headlight = false;
        g.hazard = false;
        g.sig_l = g.sig_r = false;
        g.led_mode = 0;
        g.mist = false; s_mist_tog = false; g.braking = true;
        g.gripper_open = true;
        roof_light_force_safe_off();
        servo_deg(LEDC_CHANNEL_0, 90);
        servo_deg(LEDC_CHANNEL_1, 90);
        s_horn_until = now + 200;
        s_os.roof_panel = false; s_os.quick_open = false; s_os.auto_prev = false;
        rumble(255, 255, 500);
        input_consume_all();
        return;
    }
    /* GUIDE hold = NO action (premium guide §4: no reboot on any hold) */

    /* ===== PRIORITY 2 (guide §15): controller disconnected = safe ======== */
    if (!pad || !pad->present) {
        s_horn_on = false;
        s_y_held  = false;
        g.turbo   = false;
        g.mist    = false;
        s_mist_tog = false;
        return;                      /* safe outputs forced in drive_output */
    }

    /* ==== PRIORITY 3 (guide §15): route by the unified context =========== */
    switch (os_context()) {
    case INPUT_CTX_ESTOP:
        estop_input_handle(pad, now);
        return;
    case INPUT_CTX_OS:
    case INPUT_CTX_BOOT:
    case INPUT_CTX_GAME:
        s_horn_on = false;
        s_y_held  = false;
        g.turbo   = false;
        g.mist    = false;
        s_mist_tog = false;
        if (tap || (pad && (stick_dz(pad->lx) || stick_dz(pad->ly) ||
                            stick_dz(pad->rx) || stick_dz(pad->ry) ||
                            pad->rt > 5 || pad->lt > 5))) {
            g.last_input_ms = now;
        }
        return;
    default:
        break;
    }

    /* ================= DRIVE context: premium controls ===================== */

    /* AUTO restricted sub-context (guide §13): only X exits AUTO here; LT and
       stick manual takeover live in auto_logic. Everything else blocked. */
    if (g.mode == MODE_AUTO) {
        if (ev_tap(B_X)) {
            auto_exit(now);
            s_os.auto_prev = false;
        }
        if (tap || (pad && (stick_dz(pad->lx) || stick_dz(pad->ly) ||
                            stick_dz(pad->rx) || stick_dz(pad->ry) ||
                            pad->rt > 5 || pad->lt > 5))) {
            g.last_input_ms = now;
        }
        return;
    }

    /* X: tap = drive-mode preview overlay, hold 1s = AUTO request (§5.2/§13) */
    if (ev_tap(B_X)) {
        s_os.auto_prev = !s_os.auto_prev;
        s_blip_until = now + 60;
        s_blip_freq = s_os.auto_prev ? 1300 : 900;
    }
    if (ev_hold(B_X, IE_AUTO_HOLD_MS)) {
        if (auto_can_enter(pad)) {
            auto_enter(now);
            s_os.auto_prev = false;
            car_sfx_score();
        } else {
            s_os.auto_prev = true;          /* show requirements checklist */
            s_blip_until = now + 200;
            s_blip_freq = 250;
            rumble(60, 60, 200);
        }
        input_consume(B_X);
    }

    /* A: tap = headlight, hold 700ms = mist toggle (§5.2) */
    if (ev_tap(B_A)) {
        g.headlight = !g.headlight;
        play_shot(_binary_a_click_wav_start, _binary_a_click_wav_end, 800);
    }
    if (ev_hold(B_A, IE_STD_HOLD_MS)) {
        s_mist_tog = !s_mist_tog;
        input_consume(B_A);
        s_blip_until = now + 60;
        s_blip_freq = 900;
    }
    g.mist = s_mist_tog;

    /* B: tap = roof light toggle, hold 700ms = roof panel (§5.2/§6) */
    if (ev_hold(B_B, IE_STD_HOLD_MS)) {
        s_os.roof_prev_mode = roof_light_state()->mode;
        s_os.roof_prev_br   = roof_light_state()->brightness;
        s_os.roof_panel     = true;
        s_os.roof_row       = 0;
        car_sfx_click();
        input_consume(B_B);
        return;                              /* panel owns buttons now */
    }
    if (ev_tap(B_B)) {
        bool on = !roof_light_state()->enabled;
        roof_light_set_enabled(on);
        snprintf(s_os.toast, sizeof(s_os.toast), "ROOF LIGHTS %s · %s",
                 on ? "ON" : "OFF", roof_light_label());
        s_os.toast_until = now + 1200;
        car_sfx_click();
        rumble(50, 50, 70);
    }