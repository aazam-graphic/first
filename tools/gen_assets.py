#!/usr/bin/env python3
"""gen_assets.py - PART 6: procedural RGB565 (.raw + .wh) assets into spiffs/img/.
Run: python3 tools/gen_assets.py. Total ~1.06MB (budget 1.07MB).
Transparent key = magenta (GFX_KEY 0xF81F).
"""
import math, os, struct

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "spiffs", "img")
os.makedirs(OUT, exist_ok=True)

MAGENTA = (255, 0, 255)


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def canvas(w, h, fill):
    return [[fill] * w for _ in range(h)]


def setpx(c, x, y, col):
    if 0 <= y < len(c) and 0 <= x < len(c[0]):
        c[y][x] = col


def rect(c, x0, y0, w, h, col):
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            setpx(c, x, y, col)


def rect_outline(c, x0, y0, w, h, col):
    for x in range(x0, x0 + w):
        setpx(c, x, y0, col); setpx(c, x, y0 + h - 1, col)
    for y in range(y0, y0 + h):
        setpx(c, x0, y, col); setpx(c, x0 + w - 1, y, col)


def circle(c, cx, cy, r, col, fill=False):
    r2 = r * r
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            d = (x - cx) ** 2 + (y - cy) ** 2
            if fill and d <= r2:
                setpx(c, x, y, col)
            elif abs(d - r2) <= r:
                setpx(c, x, y, col)


def line(c, x0, y0, x1, y1, col):
    dx, dy = abs(x1 - x0), abs(y1 - y0)
    sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
    err = dx - dy
    while True:
        setpx(c, x0, y0, col)
        if x0 == x1 and y0 == y1:
            break
        e2 = 2 * err
        if e2 > -dy:
            err -= dy; x0 += sx
        if e2 < dx:
            err += dx; y0 += sy


def ellipse(c, cx, cy, rx, ry, col):
    for y in range(cy - ry, cy + ry + 1):
        for x in range(cx - rx, cx + rx + 1):
            if ((x - cx) / max(rx, 1)) ** 2 + ((y - cy) / max(ry, 1)) ** 2 <= 1:
                setpx(c, x, y, col)


def save(name, c):
    h, w = len(c), len(c[0])
    with open(os.path.join(OUT, name + ".raw"), "wb") as f:
        for row in c:
            f.write(struct.pack("<%dH" % w, *[rgb565(*p) for p in row]))
    with open(os.path.join(OUT, name + ".wh"), "w") as f:
        f.write("%d %d\n" % (w, h))
    print("%-18s %dx%d %7d B" % (name, w, h, w * h * 2))


def lerp(a, b, t):
    return int(a + (b - a) * t)


def wallpaper(name, top, bot, grid=None):
    c = canvas(320, 240, (0, 0, 0))
    for y in range(240):
        t = y / 239
        col = (lerp(top[0], bot[0], t), lerp(top[1], bot[1], t), lerp(top[2], bot[2], t))
        for x in range(320):
            c[y][x] = col
    if grid:
        for y in range(0, 240, 24):
            for x in range(320):
                c[y][x] = grid
        for x in range(0, 320, 24):
            for y in range(240):
                c[y][x] = grid
    # vignette
    for y in range(240):
        for x in range(320):
            dx, dy = (x - 160) / 160, (y - 120) / 120
            v = max(0.0, 1.0 - 0.25 * (dx * dx + dy * dy))
            r, g, b = c[y][x]
            c[y][x] = (int(r * v), int(g * v), int(b * v))
    save(name, c)


# ---- wallpapers x3 (461KB) ----
wallpaper("wall_solar", (214, 226, 238), (168, 182, 198), grid=(190, 200, 212))
wallpaper("wall_night", (16, 26, 46), (7, 12, 24), grid=(22, 34, 56))
wallpaper("wall_boost", (255, 255, 255), (238, 238, 238))

# ---- boot bg + logo (186KB) ----
wallpaper("boot_bg", (10, 16, 30), (5, 8, 18), grid=(16, 26, 44))
logo = canvas(160, 100, MAGENTA)
for i in range(6):  # hexagon emblem
    a0 = i * math.pi / 3 - math.pi / 2
    a1 = (i + 1) * math.pi / 3 - math.pi / 2
    line(logo, 80 + int(38 * math.cos(a0)), 50 + int(38 * math.sin(a0)),
         80 + int(38 * math.cos(a1)), 50 + int(38 * math.sin(a1)), (255, 176, 32))
    line(logo, 80 + int(30 * math.cos(a0)), 50 + int(30 * math.sin(a0)),
         80 + int(30 * math.cos(a1)), 50 + int(30 * math.sin(a1)), (128, 82, 8))
rect(logo, 70, 52, 20, 12, (232, 238, 245))   # car glyph body
rect(logo, 75, 45, 10, 7, (232, 238, 245))    # cabin
rect(logo, 70, 44, 20, 2, (34, 211, 238))     # cyan accent
save("boot_logo", logo)


def draw_car_up(c, body=(192, 200, 210)):
    """Top-down car facing up on 140x90, magenta key bg (caller fills)."""
    dark = (20, 24, 32)
    # wheels
    rect(c, 42, 18, 8, 14, dark); rect(c, 90, 18, 8, 14, dark)
    rect(c, 42, 58, 8, 14, dark); rect(c, 90, 58, 8, 14, dark)
    # body
    rect(c, 48, 10, 44, 70, body)
    rect_outline(c, 48, 10, 44, 70, dark)
    # cabin + glass
    rect(c, 56, 30, 28, 24, (40, 60, 90))
    rect_outline(c, 56, 30, 28, 24, dark)
    rect(c, 58, 26, 24, 5, (140, 200, 230))
    rect(c, 58, 55, 24, 5, (90, 130, 170))
    # headlights / taillights
    rect(c, 50, 8, 9, 4, (255, 240, 160)); rect(c, 81, 8, 9, 4, (255, 240, 160))
    rect(c, 50, 78, 9, 4, (255, 40, 40)); rect(c, 81, 78, 9, 4, (255, 40, 40))
    # nose stripe
    rect(c, 68, 10, 4, 70, (34, 211, 238))


def rotate_canvas(src, deg):
    h, w = len(src), len(src[0])
    dst = canvas(w, h, MAGENTA)
    rad = math.radians(-deg)
    co, si = math.cos(rad), math.sin(rad)
    cx, cy = (w - 1) / 2, (h - 1) / 2
    for y in range(h):
        for x in range(w):
            sx = co * (x - cx) + si * (y - cy) + cx
            sy = -si * (x - cx) + co * (y - cy) + cy
            ix, iy = int(round(sx)), int(round(sy))
            if 0 <= iy < h and 0 <= ix < w:
                dst[y][x] = src[iy][ix]
    return dst


# ---- car 8-direction sprites + shadow (~227KB) ----
base = canvas(140, 90, MAGENTA)
draw_car_up(base)
for i in range(8):
    save("car_%d" % i, base if i == 0 else rotate_canvas(base, i * 45))
sh = canvas(140, 90, MAGENTA)
ellipse(sh, 74, 51, 30, 42, (30, 34, 44))
save("car_shadow", sh)

# ---- gauges (~150KB) ----
top = canvas(240, 80, MAGENTA)
rect(top, 0, 66, 240, 4, (140, 155, 175))
for i, deg in enumerate([-60, -45, -30, -15, 0, 15, 30, 45, 60]):
    rad = math.radians(deg - 90)
    x1, y1 = 120 + int(88 * math.cos(rad)), 70 + int(88 * math.sin(rad))
    x0, y0 = 120 + int(76 * math.cos(rad)), 70 + int(76 * math.sin(rad))
    line(top, x0, y0, x1, y1, (40, 50, 66))
circle(top, 120, 70, 88, (120, 140, 160))
circle(top, 120, 70, 60, (120, 140, 160))
save("gauge_top", top)
side = canvas(40, 160, MAGENTA)
rect_outline(side, 4, 4, 32, 152, (120, 140, 160))
for i in range(9):
    y = 8 + i * 18
    line(side, 4, y, 36, y, (140, 155, 175))
save("gauge_side", side)
ring = canvas(160, 160, MAGENTA)
circle(ring, 80, 80, 76, (120, 140, 160))
circle(ring, 80, 80, 70, (90, 105, 125))
circle(ring, 80, 80, 40, (90, 105, 125))
for deg in (0, 90, 180, 270):
    rad = math.radians(deg)
    line(ring, 80 + int(64 * math.sin(rad)), 80 - int(64 * math.cos(rad)),
         80 + int(76 * math.sin(rad)), 80 - int(76 * math.cos(rad)), (140, 155, 175))
save("ring", ring)
gm = canvas(64, 64, MAGENTA)
circle(gm, 32, 32, 30, (120, 140, 160))
circle(gm, 32, 32, 20, (120, 140, 160))
circle(gm, 32, 32, 10, (120, 140, 160))
line(gm, 2, 32, 62, 32, (120, 140, 160))
line(gm, 32, 2, 32, 62, (120, 140, 160))
save("gmeter", gm)

# ---- icons/chrome (~65KB): 24x24 set + battery + toggles ----
ICONS = {
    "ic_drive": lambda c: (rect(c, 4, 8, 16, 8, (230, 235, 245)), circle(c, 12, 12, 9, (230, 235, 245))),
    "ic_graph": lambda c: (rect(c, 4, 14, 4, 6, (230, 235, 245)), rect(c, 10, 10, 4, 10, (230, 235, 245)), rect(c, 16, 5, 4, 15, (230, 235, 245))),
    "ic_gear": lambda c: (circle(c, 12, 12, 6, (230, 235, 245)), rect(c, 11, 2, 2, 4, (230, 235, 245)), rect(c, 11, 18, 2, 4, (230, 235, 245)), rect(c, 2, 11, 4, 2, (230, 235, 245)), rect(c, 18, 11, 4, 2, (230, 235, 245))),
    "ic_games": lambda c: (rect(c, 3, 8, 18, 9, (230, 235, 245)), rect(c, 6, 10, 3, 5, (10, 16, 30)), rect(c, 15, 10, 3, 5, (10, 16, 30))),
    "ic_diag": lambda c: (circle(c, 9, 9, 5, (230, 235, 245)), line(c, 12, 12, 20, 20, (230, 235, 245))),
    "ic_light": lambda c: (rect(c, 5, 8, 12, 8, (255, 240, 170)), line(c, 17, 6, 22, 4, (255, 240, 170)), line(c, 17, 12, 22, 12, (255, 240, 170)), line(c, 17, 18, 22, 20, (255, 240, 170))),
    "ic_warn": lambda c: (rect(c, 11, 4, 2, 10, (255, 197, 61)), rect(c, 11, 16, 2, 3, (255, 197, 61))),
    "ic_ok": lambda c: (line(c, 6, 12, 11, 18, (53, 208, 127)), line(c, 11, 18, 19, 6, (53, 208, 127))),
    "ic_mist": lambda c: (circle(c, 12, 10, 6, (34, 211, 238)), rect(c, 8, 14, 8, 2, (34, 211, 238))),
    "ic_spark": lambda c: (line(c, 14, 3, 8, 13, (255, 220, 80)), line(c, 8, 13, 16, 13, (255, 220, 80)), line(c, 12, 13, 9, 21, (255, 220, 80))),
    "ic_auto": lambda c: (line(c, 8, 5, 8, 19, (53, 208, 127)), line(c, 8, 5, 18, 12, (53, 208, 127)), line(c, 8, 19, 18, 12, (53, 208, 127))),
    "ic_batt": lambda c: (rect_outline(c, 2, 8, 17, 9, (230, 235, 245)), rect(c, 19, 10, 3, 5, (230, 235, 245)), rect(c, 4, 10, 10, 5, (53, 208, 127))),
}
for name, fn in ICONS.items():
    c = canvas(24, 24, MAGENTA)
    fn(c)
    save(name, c)
for i, nm in enumerate(["tog_off", "tog_on"]):
    c = canvas(40, 24, MAGENTA)
    rect(c, 2, 4, 36, 16, (60, 70, 88) if i == 0 else (53, 208, 127))
    circle(c, 11 if i == 0 else 29, 12, 7, (230, 235, 245), fill=True)
    save(nm, c)
# pad out chrome budget with panel shadows
for nm, ww, hh in [("sh_panel", 120, 40), ("sh_pill", 80, 24), ("sh_card", 96, 80)]:
    c = canvas(ww, hh, (18, 24, 38))
    rect_outline(c, 0, 0, ww, hh, (42, 58, 79))
    save(nm, c)

print("done.")
