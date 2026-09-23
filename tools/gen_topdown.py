#!/usr/bin/env python3
"""gen_topdown.py - top-down sedans (64x40 RGB565, nose UP) for the rotating
cockpit sprite: silver (dark themes) + dark (light themes). Wheels baked in
(rotation spins whole car). Run: python3 tools/gen_topdown.py
"""
import os, struct

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "spiffs", "img")
os.makedirs(OUT, exist_ok=True)
W, H = 64, 40
MAGENTA = (255, 0, 255)


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def canvas():
    return [[MAGENTA] * W for _ in range(H)]


def setpx(c, x, y, col):
    if 0 <= y < H and 0 <= x < W:
        c[y][x] = col


def rect(c, x0, y0, w, h, col):
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            setpx(c, x, y, col)


def hline(c, x0, x1, y, col):
    for x in range(min(x0, x1), max(x0, x1) + 1):
        setpx(c, x, y, col)


def vline(c, x, y0, y1, col):
    for y in range(min(y0, y1), max(y0, y1) + 1):
        setpx(c, x, y, col)


def save(name, c):
    with open(os.path.join(OUT, name + ".raw"), "wb") as f:
        for row in c:
            f.write(struct.pack("<%dH" % W, *[rgb565(*p) for p in row]))
    with open(os.path.join(OUT, name + ".wh"), "w") as f:
        f.write("%d %d\n" % (W, H))
    print("%-18s %dx%d %7d B" % (name, W, H, W * H * 2))


def draw_top(body_edge, body, body_dark, glass, glass_hi, chrome):
    c = canvas()
    dark = (16, 20, 28)
    # wheels (4 dark blocks on flanks)
    for wx, wy in ((10, 9), (50, 9), (10, 27), (50, 27)):
        rect(c, wx, wy, 4, 6, dark)
        rect(c, wx + 1, wy + 1, 2, 4, (110, 116, 128))
    # main body with rounded nose/tail
    for y in range(3, 37):
        t = (y - 3) / 33.0
        inset = 0
        if y < 7:
            inset = 7 - y          # tapered nose
        elif y > 33:
            inset = y - 33         # tapered tail
        x0, x1 = 14 + inset, 50 - inset
        # vertical gradient: light spine -> darker flanks
        for x in range(max(0, x0), min(W, x1 + 1)):
            d = abs(x - 32) / 18.0
            base = body
            col = tuple(int(base[i] * (0.72 + 0.28 * max(0.0, 1.0 - d))) for i in range(3))
            if y >= 31:
                col = body_dark
            setpx(c, x, y, col)
    # outline
    for y in range(3, 37):
        t = (y - 3) / 33.0
        inset = 0
        if y < 7:
            inset = 7 - y
        elif y > 33:
            inset = y - 33
        setpx(c, 14 + inset, y, body_edge)
        setpx(c, 50 - inset, y, body_edge)
    hline(c, 16, 48, 3, body_edge)
    hline(c, 16, 48, 36, body_edge)
    # windshield (front) + rear glass
    rect(c, 22, 7, 20, 4, glass)
    hline(c, 22, 41, 7, glass_hi)
    rect(c, 24, 28, 16, 4, glass)
    # roof panel + spine highlight
    rect(c, 24, 13, 16, 14, body)
    vline(c, 32, 4, 35, glass_hi)
    # headlights (front corners) + taillight bar
    rect(c, 15, 4, 5, 3, (240, 244, 250))
    rect(c, 44, 4, 5, 3, (240, 244, 250))
    rect(c, 18, 35, 28, 2, (200, 30, 36))
    # door cuts + handles
    vline(c, 20, 16, 26, dark)
    vline(c, 43, 16, 26, dark)
    rect(c, 22, 18, 3, 1, chrome)
    rect(c, 39, 18, 3, 1, chrome)
    return c


silver = draw_top((30, 34, 44), (192, 200, 210), (120, 128, 140),
                  (30, 44, 62), (150, 205, 235), (215, 222, 232))
save("sedan_top_silver", silver)
dark = draw_top((10, 12, 18), (74, 80, 94), (44, 48, 60),
                (56, 76, 100), (140, 180, 210), (150, 158, 170))
save("sedan_top_dark", dark)
print("done.")
