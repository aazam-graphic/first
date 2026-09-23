#!/usr/bin/env python3
"""gen_cockpit_car.py - 8 top-down rotation sprites (96x78 RGB565) for the
band-layout cockpit: car_N/NE/E/SE/S/SW/W/NW per PART 7.0 section 4 sectors.
Base art faces UP (=N); others are clockwise rotations with bilinear sampling
and magenta-key preservation. Run: python3 tools/gen_cockpit_car.py
"""
import math, os, struct

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "spiffs", "img")
os.makedirs(OUT, exist_ok=True)
BW, BH = 64, 40          # base art size
W, H = 96, 78            # sprite canvas (fits all rotations)
MAGENTA = (255, 0, 255)


def setpx(c, W_, H_, x, y, col):
    if 0 <= y < H_ and 0 <= x < W_:
        c[y][x] = col


def rect(c, W_, H_, x0, y0, w, h, col):
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            setpx(c, W_, H_, x, y, col)


def hline(c, W_, H_, x0, x1, y, col):
    for x in range(min(x0, x1), max(x0, x1) + 1):
        setpx(c, W_, H_, x, y, col)


def vline(c, W_, H_, x, y0, y1, col):
    for y in range(min(y0, y1), max(y0, y1) + 1):
        setpx(c, W_, H_, x, y, col)


def base_art(body_edge, body, body_dark, glass, glass_hi, chrome):
    c = [[MAGENTA] * BW for _ in range(BH)]
    dark = (16, 20, 28)
    for wx, wy in ((10, 9), (50, 9), (10, 27), (50, 27)):
        rect(c, BW, BH, wx, wy, 4, 6, dark)
        rect(c, BW, BH, wx + 1, wy + 1, 2, 4, (110, 116, 128))
    # body with rounded nose/tail + flank shading
    for y in range(3, 37):
        inset = 0
        if y < 7:
            inset = 7 - y
        elif y > 33:
            inset = y - 33
        for x in range(14 + inset, 51 - inset):
            d = abs(x - 32) / 18.0
            b = body
            col = tuple(int(b[i] * (0.72 + 0.28 * max(0.0, 1.0 - d))) for i in range(3))
            if y >= 31:
                col = body_dark
            setpx(c, BW, BH, x, y, col)
    for y in range(3, 37):
        inset = 0
        if y < 7:
            inset = 7 - y
        elif y > 33:
            inset = y - 33
        setpx(c, BW, BH, 14 + inset, y, body_edge)
        setpx(c, BW, BH, 50 - inset, y, body_edge)
    hline(c, BW, BH, 16, 48, 3, body_edge)
    hline(c, BW, BH, 16, 48, 36, body_edge)
    rect(c, BW, BH, 22, 7, 20, 4, glass)
    hline(c, BW, BH, 22, 41, 7, glass_hi)
    rect(c, BW, BH, 24, 28, 16, 4, glass)
    rect(c, BW, BH, 24, 13, 16, 14, body)
    vline(c, BW, BH, 32, 4, 35, glass_hi)
    rect(c, BW, BH, 15, 4, 5, 3, (240, 244, 250))
    rect(c, BW, BH, 44, 4, 5, 3, (240, 244, 250))
    rect(c, BW, BH, 18, 35, 28, 2, (200, 30, 36))
    vline(c, BW, BH, 20, 16, 26, dark)
    vline(c, BW, BH, 43, 16, 26, dark)
    rect(c, BW, BH, 22, 18, 3, 1, chrome)
    rect(c, BW, BH, 39, 18, 3, 1, chrome)
    return c


def sample_bilinear(base, x, y):
    x0, y0 = int(math.floor(x)), int(math.floor(y))
    fx, fy = x - x0, y - y0
    acc = [0.0, 0.0, 0.0]
    wsum = 0.0
    for dy in (0, 1):
        for dx in (0, 1):
            xx, yy = x0 + dx, y0 + dy
            if 0 <= xx < BW and 0 <= yy < BH:
                p = base[yy][xx]
                if p == MAGENTA:
                    continue
                w = (1 - abs(dx - fx)) * (1 - abs(dy - fy))
                # key priority: skip key texels entirely
                acc[0] += p[0] * w; acc[1] += p[1] * w; acc[2] += p[2] * w
                wsum += w
    if wsum <= 1e-6:
        return MAGENTA
    return tuple(int(v / wsum) for v in acc)


def rotate_cw(base, deg):
    """Clockwise visual rotation (y-down screen) by deg. Returns 96x78."""
    c = [[MAGENTA] * W for _ in range(H)]
    ox, oy = (W - BW) / 2.0, (H - BH) / 2.0   # base placement offset
    cx, cy = (W - 1) / 2.0, (H - 1) / 2.0
    a = math.radians(deg)
    co, si = math.cos(a), math.sin(a)
    for y in range(H):
        for x in range(W):
            dx, dy = x - cx, y - cy
            # inverse: source point in canvas space, then minus base offset
            sx = co * dx - si * dy + cx - ox
            sy = si * dx + co * dy + cy - oy
            if 0 <= sx < BW - 1 and 0 <= sy < BH - 1:
                c[y][x] = sample_bilinear(base, sx, sy)
    return c


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def save(name, c):
    h, w = len(c), len(c[0])
    with open(os.path.join(OUT, name + ".raw"), "wb") as f:
        for row in c:
            f.write(struct.pack("<%dH" % w, *[rgb565(*p) for p in row]))
    with open(os.path.join(OUT, name + ".wh"), "w") as f:
        f.write("%d %d\n" % (w, h))
    print("%-14s %dx%d %7d B" % (name, w, h, w * h * 2))


def place_centered(base):
    c = [[MAGENTA] * W for _ in range(H)]
    ox, oy = (W - BW) // 2, (H - BH) // 2
    for y in range(BH):
        for x in range(BW):
            c[y + oy][x + ox] = base[y][x]
    return c


base = base_art((30, 34, 44), (192, 200, 210), (120, 128, 140),
                (30, 44, 62), (150, 205, 235), (215, 222, 232))
based = base_art((10, 12, 18), (74, 80, 94), (44, 48, 60),
                 (56, 76, 100), (140, 180, 210), (150, 158, 170))
names = ["car_N", "car_NE", "car_E", "car_SE",
         "car_S", "car_SW", "car_W", "car_NW"]
base96 = place_centered(base)
based96 = place_centered(based)
for i, nm in enumerate(names):
    save(nm, base96 if i == 0 else rotate_cw(base, i * 45))
for i, nm in enumerate(names):
    save(nm + "_d", based96 if i == 0 else rotate_cw(based, i * 45))
print("done.")
