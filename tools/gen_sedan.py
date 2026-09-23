#!/usr/bin/env python3
"""gen_sedan.py - 3/4-view sedan sprites (150x85 RGB565) for the image-faithful
cockpit: silver (dark themes) + dark (light themes). Wheel wells are magenta
key (procedural spinning wheels show through). Run: python3 tools/gen_sedan.py
"""
import os, struct

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "spiffs", "img")
os.makedirs(OUT, exist_ok=True)
W, H = 150, 85
MAGENTA = (255, 0, 255)


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def canvas(fill=MAGENTA):
    return [[fill] * W for _ in range(H)]


def setpx(c, x, y, col):
    if 0 <= y < H and 0 <= x < W:
        c[y][x] = col


def hline(c, x0, x1, y, col):
    for x in range(min(x0, x1), max(x0, x1) + 1):
        setpx(c, x, y, col)


def vline(c, x, y0, y1, col):
    for y in range(min(y0, y1), max(y0, y1) + 1):
        setpx(c, x, y, col)


def line(c, x0, y0, x1, y1, col, th=1):
    dx, dy = abs(x1 - x0), abs(y1 - y0)
    sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
    err = dx - dy
    while True:
        for t in range(-(th // 2), th // 2 + 1):
            setpx(c, x0 + t, y0, col)
            setpx(c, x0, y0 + t, col)
        if x0 == x1 and y0 == y1:
            break
        e2 = 2 * err
        if e2 > -dy:
            err -= dy
            x0 += sx
        if e2 < dx:
            err += dx
            y0 += sy


def rect(c, x0, y0, w, h, col):
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            setpx(c, x, y, col)


def ellipse(c, cx, cy, rx, ry, col):
    for y in range(cy - ry, cy + ry + 1):
        for x in range(cx - rx, cx + rx + 1):
            if ((x - cx) / max(rx, 1)) ** 2 + ((y - cy) / max(ry, 1)) ** 2 <= 1:
                setpx(c, x, y, col)


def circle(c, cx, cy, r, col, fill=False):
    r2 = r * r
    for y in range(cy - r - 1, cy + r + 2):
        for x in range(cx - r - 1, cx + r + 2):
            d = (x - cx) ** 2 + (y - cy) ** 2
            if fill and d <= r2:
                setpx(c, x, y, col)
            elif abs(d - r2) <= r:
                setpx(c, x, y, col)


def fill_poly(c, pts, col):
    xs = [p[0] for p in pts]
    for y in range(max(0, min(p[1] for p in pts)), min(H, max(p[1] for p in pts) + 1)):
        inter = []
        n = len(pts)
        for i in range(n):
            x0, y0 = pts[i]
            x1, y1 = pts[(i + 1) % n]
            if (y0 <= y < y1) or (y1 <= y < y0):
                dy = y1 - y0
                x = x0 + (y - y0) * (x1 - x0) / (dy if dy != 0 else 1e-6)
                inter.append(x)
        inter.sort()
        for i in range(0, len(inter) - 1, 2):
            for x in range(max(0, int(inter[i])), min(W, int(inter[i + 1]) + 1)):
                setpx(c, x, y, col)


def shade(c, x0, y0, w, h, top, bot):
    for y in range(y0, y0 + h):
        if not (0 <= y < H):
            continue
        t = (y - y0) / max(1, h - 1)
        col = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3))
        for x in range(x0, x0 + w):
            if 0 <= x < W and c[y][x] != MAGENTA:
                c[y][x] = col


def save(name, c):
    with open(os.path.join(OUT, name + ".raw"), "wb") as f:
        for row in c:
            f.write(struct.pack("<%dH" % W, *[rgb565(*p) for p in row]))
    with open(os.path.join(OUT, name + ".wh"), "w") as f:
        f.write("%d %d\n" % (W, H))
    print("%-16s %dx%d %7d B" % (name, W, H, W * H * 2))


WELL_F = (40, 64, 13)
WELL_R = (110, 62, 13)


def draw_sedan(body_top, body_mid, body_low, skirt, glass, glass_hi, chrome):
    c = canvas()
    dark = (18, 22, 30)
    # wheel wells (transparent -> procedural wheels)
    circle(c, WELL_F[0], WELL_F[1], WELL_F[2], MAGENTA, fill=True)
    circle(c, WELL_R[0], WELL_R[1], WELL_R[2], MAGENTA, fill=True)
    # main body silhouette
    body = [(10, 66), (12, 46), (20, 42), (34, 40), (52, 36),
            (72, 20), (108, 20), (128, 34), (140, 40), (140, 64),
            (120, 68), (20, 68)]
    fill_poly(c, body, body_mid)
    # vertical shading bands (skip wells/glass by overdraw order: bands first)
    shade(c, 8, 40, 134, 10, body_top, body_mid)
    shade(c, 8, 50, 134, 10, body_mid, body_low)
    shade(c, 8, 60, 134, 8, body_low, skirt)
    # re-cut wells (shade painted over them)
    circle(c, WELL_F[0], WELL_F[1], WELL_F[2], MAGENTA, fill=True)
    circle(c, WELL_R[0], WELL_R[1], WELL_R[2], MAGENTA, fill=True)
    # glasshouse
    fill_poly(c, [(52, 36), (70, 21), (78, 21), (64, 36)], glass)
    fill_poly(c, [(80, 22), (102, 22), (102, 34), (78, 34)], glass)
    fill_poly(c, [(105, 22), (122, 26), (126, 34), (105, 34)], glass)
    line(c, 56, 33, 72, 23, glass_hi)
    line(c, 82, 24, 100, 24, glass_hi)
    # pillars + chrome trim
    vline(c, 78, 22, 34, body_mid)
    vline(c, 103, 22, 34, body_mid)
    line(c, 52, 36, 70, 21, chrome)
    line(c, 80, 22, 102, 22, chrome)
    line(c, 80, 34, 102, 34, chrome)
    # roof highlight
    hline(c, 74, 106, 19, body_top)
    # hood highlight + nose
    line(c, 22, 41, 50, 37, body_top)
    # grille + slats
    rect(c, 12, 50, 14, 12, dark)
    for gx in (15, 18, 21):
        vline(c, gx, 51, 61, (90, 96, 108))
    # headlight (white core, amber lower edge)
    fill_poly(c, [(26, 44), (44, 42), (42, 48), (26, 50)], (238, 243, 250))
    line(c, 26, 50, 42, 48, (255, 170, 60))
    # bumper intake
    rect(c, 14, 60, 20, 6, dark)
    # taillight
    rect(c, 136, 42, 4, 12, (200, 30, 36))
    vline(c, 135, 42, 54, dark)
    # doors + handles
    vline(c, 76, 36, 62, dark)
    vline(c, 104, 34, 62, dark)
    rect(c, 68, 44, 6, 2, chrome)
    rect(c, 96, 42, 6, 2, chrome)
    # side skirt + marker
    rect(c, 20, 64, 112, 4, skirt)
    rect(c, 58, 56, 4, 3, (255, 170, 60))
    # wheel-arch trim
    circle(c, WELL_F[0], WELL_F[1], 15, dark)
    circle(c, WELL_R[0], WELL_R[1], 15, dark)
    # mirror
    fill_poly(c, [(62, 30), (68, 28), (68, 33), (62, 34)], body_mid)
    return c


silver = draw_sedan((228, 234, 242), (188, 195, 205), (140, 148, 160),
                    (72, 78, 90), (30, 42, 60), (150, 205, 235), (215, 222, 232))
save("sedan_silver", silver)
dark = draw_sedan((92, 98, 112), (60, 66, 80), (38, 43, 55),
                  (22, 26, 34), (58, 80, 104), (140, 180, 210), (150, 158, 170))
save("sedan_dark", dark)
print("done.")
