#!/usr/bin/env python3
"""gen_sounds.py - synthesize PART 5 sound bank: 16-bit PCM mono 16kHz .raw
into spiffs/snd/. Run: python3 tools/gen_sounds.py. Total ~0.5MB (budget 1.25MB).
"""
import math, os, random, struct

SR = 16000
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "spiffs", "snd")
os.makedirs(OUT, exist_ok=True)
random.seed(0xC0FFEE)


def env(n, attack=0.005, decay_pow=2.2):
    a = max(1, int(n * attack / (n / SR)) if False else int(SR * attack))
    out = []
    for i in range(n):
        x = i / n
        e = min(1.0, i / a) * ((1.0 - x) ** decay_pow)
        out.append(e)
    return out


def tone(freq, dur, vol=0.7, slide_to=None, square=False):
    n = int(SR * dur)
    e = env(n)
    s = []
    ph = 0.0
    for i in range(n):
        f = freq if slide_to is None else freq + (slide_to - freq) * i / n
        ph += 2 * math.pi * f / SR
        v = math.sin(ph)
        if square:
            v = 1.0 if v >= 0 else -1.0
            v = v * 0.6 + math.sin(ph) * 0.4
        s.append(v * vol * e[i])
    return s


def noise(dur, vol=0.5, lowpass=0.2):
    n = int(SR * dur)
    e = env(n)
    s = []
    last = 0.0
    for i in range(n):
        w = random.uniform(-1, 1)
        last += lowpass * (w - last)
        s.append(last * vol * e[i] * 2.0)
    return s


def mix(*parts):
    n = max(len(p) for p in parts)
    out = [0.0] * n
    for p in parts:
        for i, v in enumerate(p):
            out[i] += v
    peak = max(1e-6, max(abs(v) for v in out))
    if peak > 0.95:
        out = [v * 0.95 / peak for v in out]
    return out


def seq(notes, gap=0.02):
    """notes: list of (freq, dur, vol). Concatenate with small gaps."""
    out = []
    g = [0.0] * int(SR * gap)
    for idx, (f, d, v) in enumerate(notes):
        out += tone(f, d, v)
        if idx < len(notes) - 1:
            out += g
    return out


def save(name, samples):
    pcm = struct.pack("<%dh" % len(samples),
                      *[max(-32768, min(32767, int(v * 32767))) for v in samples])
    with open(os.path.join(OUT, name + ".raw"), "wb") as f:
        f.write(pcm)
    print("%-22s %6d B" % (name, len(pcm)))


S = {}
# NEW UI
S["ui_tap"] = tone(1200, 0.06)
S["ui_back"] = tone(800, 0.08)
S["ui_nav"] = tone(1000, 0.04, vol=0.5)
S["ui_open_sheet"] = tone(600, 0.12, slide_to=1200)
S["ui_close_sheet"] = tone(1200, 0.12, slide_to=600)
S["ui_error"] = mix(tone(220, 0.2, vol=0.6, square=True), tone(180, 0.2, vol=0.5))
S["ui_toggle_on"] = tone(700, 0.08, slide_to=1050)
S["ui_toggle_off"] = tone(1050, 0.08, slide_to=700)
S["gear_shift_up"] = tone(500, 0.1, slide_to=900)
S["gear_shift_down"] = tone(900, 0.1, slide_to=500)
S["gear_limit"] = tone(300, 0.15, vol=0.6, square=True)
# NEW SYSTEM
S["sys_boot_chime"] = seq([(523, 0.12, 0.7), (659, 0.12, 0.7), (784, 0.2, 0.7)])
S["sys_ready"] = seq([(784, 0.12, 0.7), (1047, 0.2, 0.7)])
S["sys_warning"] = seq([(660, 0.09, 0.7), (660, 0.09, 0.7), (660, 0.14, 0.7)], gap=0.05)
S["sys_estop"] = tone(440, 0.6, vol=0.7, slide_to=110)
S["sys_connect"] = tone(880, 0.1, slide_to=1320)
S["sys_disconnect"] = tone(1320, 0.12, slide_to=880)
# NEW VOICE
S["voice_listen_start"] = tone(700, 0.1, slide_to=1400, vol=0.6)
S["voice_listen_end"] = tone(1400, 0.1, slide_to=700, vol=0.6)
S["alexa_notify"] = mix(tone(990, 0.15), tone(1320, 0.15, vol=0.5))
S["clap_detected"] = noise(0.05, vol=0.7)
# NEW ROOF
S["roof_police_on"] = tone(600, 0.4, slide_to=1200, vol=0.6) + tone(1200, 0.3, slide_to=600, vol=0.6)
S["roof_mode_cycle"] = tone(1500, 0.05)
S["light_flash"] = noise(0.1, vol=0.6)
# NEW GAMES/SCORE
S["score_tick"] = tone(2000, 0.03, vol=0.6)
S["highscore_fanfare"] = seq([(523, 0.1, 0.7), (659, 0.1, 0.7), (784, 0.1, 0.7), (1047, 0.25, 0.7)])
S["game_over"] = seq([(392, 0.15, 0.7), (330, 0.15, 0.7), (262, 0.3, 0.7)])
S["score_reveal"] = tone(800, 0.3, slide_to=2400, vol=0.6)
S["grade_A"] = seq([(659, 0.12, 0.7), (784, 0.12, 0.7), (1047, 0.25, 0.75)])
S["grade_B"] = seq([(587, 0.12, 0.7), (880, 0.2, 0.7)])
S["grade_C"] = tone(440, 0.2, vol=0.7)
# NEW MPU EVENTS
S["impact_light"] = tone(150, 0.15, vol=0.7)
S["impact_moderate"] = mix(tone(120, 0.25, vol=0.7), noise(0.2, vol=0.4))
S["impact_hard"] = mix(tone(90, 0.4, vol=0.8), noise(0.35, vol=0.5))
S["freefall_start"] = tone(1200, 0.4, slide_to=300, vol=0.6)
S["landing_smooth"] = tone(180, 0.12, vol=0.6)
S["landing_hard"] = mix(tone(100, 0.3, vol=0.8), noise(0.25, vol=0.4))
S["stuck_alert"] = seq([(440, 0.09, 0.7), (440, 0.09, 0.7), (440, 0.14, 0.7)], gap=0.05)
S["drift_warn"] = noise(0.3, vol=0.45, lowpass=0.5)
# NEW CAR PERSONALITY (0.3s stings)
S["profile_chime_safe"] = seq([(523, 0.12, 0.65), (784, 0.15, 0.65)])
S["profile_chime_night"] = seq([(392, 0.12, 0.65), (587, 0.15, 0.65)])
S["profile_chime_perf"] = seq([(659, 0.1, 0.7), (988, 0.15, 0.7)])
S["profile_chime_park"] = seq([(440, 0.12, 0.6), (440, 0.15, 0.6)])
S["profile_chime_demo"] = seq([(523, 0.08, 0.65), (659, 0.08, 0.65), (784, 0.12, 0.65)])
S["profile_chime_silent"] = tone(300, 0.25, vol=0.4)
S["profile_chime_game"] = seq([(784, 0.08, 0.7), (1047, 0.08, 0.7), (1319, 0.12, 0.7)])

total = 0
for name, samples in S.items():
    save(name, samples)
    total += len(samples) * 2
print("TOTAL %d files, %d bytes (budget 1.25MB)" % (len(S), total))
