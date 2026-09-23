# ESP32-S3 Car OS — Complete Premium UI Redesign and Code-Update Guide

> ⚠ **NOTE (17 Sep 2026):** Ye ek DESIGN SPEC hai. Isme kuch references purane hain
> (battery monitor removed hai, roof light ab WS2812 GPIO18 hai, pins badal chuke hain).
> **Latest verified pins/features ke liye `PROJECT_DETAILS.md` dekho.**

## Purpose

This document is a direct implementation specification for updating the current `car_os.c`-based firmware. The goal is a premium, useful, smooth and readable 320×240 Car OS—not blue text on a plain black background. The existing motor, sensor, controller, mist, spark, lighting and audio behavior must remain functionally unchanged unless a safety correction is explicitly required.

The new UI must use large text, clear visual hierarchy, layered dark surfaces, restrained animations, dirty-region rendering where practical, and safe separation between real-time car control and display work. Dark automotive interfaces should use semantic colors and sufficient contrast instead of treating every element as the same blue accent.

---

## 1. Non-negotiable requirements

1. Preserve all existing GPIO assignments and hardware drivers.
2. Preserve motor, ultrasonic, IMU, controller, mist, spark, LEDs, audio and emergency-stop behavior.
3. The OS may observe and configure car state, but it must not block the car-control loop.
4. Menus and games must park the car: target motors zero, mist off, spark off.
5. Emergency stop and controller-disconnect fail-safe must remain available in every OS state.
6. Do not perform blocking animations, file loading, decompression or large allocations inside `car_task`.
7. TFT updates must be throttled independently from control-loop timing.
8. OLED updates must be throttled separately from the TFT.
9. Large assets/history belong in PSRAM; DMA buffers and real-time state remain in internal RAM.
10. All screens must use large primary values and readable labels; tiny text is allowed only for secondary hints.

---

## 2. Current code corrections

Before visual redesign, correct the following issues in the supplied `car_os.c`.

### 2.1 Syntax error

The file ends with:

```c
},
```

It must end with:

```c
}
```

The trailing comma after `os_game_frame()` causes a compilation error.

### 2.2 Shared selection state

`focus_tile` is reused by Home, OLED Control and Games Hub. This causes one screen's selection to leak into another. Replace it with separate fields:

```c
uint8_t home_sel;      // 0..5
uint8_t oled_sel;      // 0..2
uint8_t game_sel;      // 0..OS_GAME_COUNT-1
uint8_t settings_sel;  // settings item
uint8_t settings_category; // settings page/category
```

Do not reset these on every frame. Preserve each screen's last selection so returning to it feels consistent.

### 2.3 Safe enum lookup

Do not assume enum values always match array positions. Replace the range-only lookup with a designated initializer table or a switch:

```c
static const char *state_name(os_state_t st)
{
    switch (st) {
    case OS_BOOT: return "BOOT";
    case OS_HOME: return "HOME";
    case OS_DRIVE_MAIN: return "DRIVE";
    case OS_ANALYTICS: return "ANALYTICS";
    case OS_SETTINGS: return "SETTINGS";
    case OS_OLED_CONTROL: return "OLED";
    case OS_GAMES_HUB: return "GAMES";
    case OS_DIAGNOSTICS: return "DIAGNOSTICS";
    default: return "UNKNOWN";
    }
}
```

This avoids silent label mismatch if enum order changes.

### 2.4 Game ownership and safety - CRITICAL

`os_game_frame()` updates and draws the game, but the main car loop must park the motors BEFORE any game input can reach normal driving logic. Apply the safety gate early in car_task:

```c
// In car_task main loop, AFTER input_handle but BEFORE manual/auto logic:

// Emergency stop ALWAYS available
const xbox360_pad_t *pad = xbox360_pad(0);
uint16_t dig = pad ? (uint16_t)(pad->buttons >> 16) : 0;
if (tap & B_GUIDE) {
    g.estop = true;  // E-STOP fires regardless of UI state
}

// Gate: UI owns drive if parked or game active
bool ui_owns_drive = os_parked() || os_game_active();
if (ui_owns_drive) {
    g.tgt_l = 0;
    g.tgt_r = 0;
    g.mist = false;
    g.spark = false;
} else {
    /* existing manual or auto logic */
}
```

**Timing order:**
1. Read controller input.
2. **Check E-STOP immediately** (B_GUIDE).
3. **Gate motor targets** based on UI state.
4. Only then execute manual/auto drive logic.
5. Apply drive output.

**Critical invariant:** Entering/exiting a game must **never clear** a latched `g.estop`. If the user hit E-STOP while in a menu, returning to Drive must keep motors parked until E-STOP is explicitly cleared via Start button.

### 2.5 Allocation handling

The three history arrays are individually allocated and individually fall back to `malloc()`. This can create mixed placement and partial allocation. Allocate one PSRAM block and split it:

```c
static int16_t *s_hist_block = NULL;

static void os_init_history(void)
{
    size_t one = OS_HIST_N * sizeof(int16_t);
    s_hist_block = heap_caps_calloc(3, one,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    
    if (s_hist_block) {
        os_h_spd   = s_hist_block;
        os_h_front = s_hist_block + OS_HIST_N;
        os_h_yaw   = s_hist_block + OS_HIST_N * 2;
        ESP_LOGI(TAG, "analytics history: %.1f KB in PSRAM",
                 3.0f * one / 1024.0f);
    } else {
        // Reduce history depth and retry with internal RAM
        #define OS_HIST_N_FALLBACK 64
        size_t fallback = OS_HIST_N_FALLBACK * sizeof(int16_t);
        s_hist_block = malloc(3 * fallback);
        
        if (s_hist_block) {
            os_h_spd   = s_hist_block;
            os_h_front = s_hist_block + OS_HIST_N_FALLBACK;
            os_h_yaw   = s_hist_block + OS_HIST_N_FALLBACK * 2;
            ESP_LOGW(TAG, "analytics history reduced to %d samples (internal RAM)",
                     OS_HIST_N_FALLBACK);
            os_ctx->analytics_hist_n = OS_HIST_N_FALLBACK;
        } else {
            ESP_LOGE(TAG, "analytics history allocation failed; disabled");
            os_ctx->analytics_available = false;
        }
    }
}
```

**Never silently consume scarce internal RAM.** Log every fallback decision.

### 2.6 History behavior

Front distance `65535` is currently converted to zero. On a graph, zero looks like immediate danger. Store an invalid marker or hold the last valid sample:

```c
static int16_t last_front = 0;

static void os_update_history(const os_ctx_t *ctx, uint32_t now)
{
    if (!os_ctx->analytics_available) return;
    
    // Update every ~100ms
    if (now - ctx->hist_update_ms < 100) return;
    ctx->hist_update_ms = now;
    
    // Speed: absolute value of max motor
    int16_t spd = abs(g.cur_l) > abs(g.cur_r) ? abs(g.cur_l) : abs(g.cur_r);
    os_h_spd[s_hist_idx] = (int16_t)clampi(spd, 0, 100);
    
    // Front distance: hold last valid, never show 65535
    if (g.dist_avg[1] != 65535) {
        last_front = (int16_t)clampi(g.dist_avg[1], 0, 200);
    }
    os_h_front[s_hist_idx] = last_front;
    
    // Yaw: fixed-point or integer (no float per frame)
    int16_t yaw_int = (int16_t)clampi((int32_t)(g_yaw), -180, 180);
    os_h_yaw[s_hist_idx] = yaw_int;
    
    s_hist_idx = (s_hist_idx + 1) % os_ctx->analytics_hist_n;
}
```

Use **fixed-point or integer yaw** representation to avoid unnecessary float formatting during every graph update.

### 2.7 Input repeat

Settings currently adjust only on D-pad tap, making large changes slow. Add key-repeat:

```c
typedef struct {
    uint32_t key_down_ms;
    uint32_t last_repeat_ms;
    bool repeat_armed;
} os_key_repeat_t;

static os_key_repeat_t repeat_state = {0};

static bool os_check_repeat(uint16_t current_key, uint32_t now)
{
    if (current_key == 0) {
        repeat_state.key_down_ms = 0;
        repeat_state.repeat_armed = false;
        return false;
    }
    
    if (!repeat_state.key_down_ms) {
        repeat_state.key_down_ms = now;
        repeat_state.repeat_armed = true;
        return true;  // Immediate first press
    }
    
    uint32_t held = now - repeat_state.key_down_ms;
    if (held < 350) return false;  // Delay before repeat
    
    if (repeat_state.repeat_armed) {
        repeat_state.repeat_armed = false;
        repeat_state.last_repeat_ms = now;
        return true;
    }
    
    if (now - repeat_state.last_repeat_ms >= 90) {  // Repeat every 90ms
        repeat_state.last_repeat_ms = now;
        return true;
    }
    
    return false;
}
```

Use in settings:
```c
uint16_t adjust_key = dig & (B_DLEFT | B_DRIGHT);
if (os_check_repeat(adjust_key, now)) {
    // Adjust selected setting
}
```

**Do not use `vTaskDelay()` for repeat.** Timer-driven repeat is non-blocking.

### 2.8 Back+Start toggle - CRITICAL

The global Back+Start logic uses elapsed time only. Holding both buttons can retrigger after 800 ms. Use an armed latch that triggers once and rearms only after one button is released:

```c
static uint32_t back_start_armed_ms = 0;
static bool back_start_triggered = false;

static void os_check_back_start(uint16_t dig, uint32_t now)
{
    bool back = (dig & B_BACK) != 0;
    bool start = (dig & B_START) != 0;
    
    if (!back || !start) {
        // At least one released: re-arm
        back_start_armed_ms = 0;
        back_start_triggered = false;
        return;
    }
    
    // Both held
    if (!back_start_armed_ms) {
        back_start_armed_ms = now;
    }
    
    uint32_t held = now - back_start_armed_ms;
    if (held >= 800 && !back_start_triggered) {
        // Trigger once
        back_start_triggered = true;
        ESP_LOGI(TAG, "Back+Start 800ms hold -> toggle settings save");
        // Apply action (e.g., os_ctx->state = OS_SETTINGS)
    }
}
```

---

## 3. Visual design system

### 3.1 Style direction

Use a restrained **robotic automotive** style:

- Deep blue-charcoal background rather than pure black.
- Elevated dark slate panels.
- Warm amber as the main interaction accent.
- Cyan only for live sensor/data visualization.
- White for primary information.
- Green, yellow and red only for status meaning.
- Thin borders, simple geometric icons and minimal glow.
- No rainbow UI, excessive neon, constant blinking or full-screen gradients.

### 3.2 Semantic color tokens

Create `os_theme.h`. No screen may use hard-coded raw colors.

```c
#pragma once
#include <stdint.h>

/* RGB565: 5-bit R, 6-bit G, 5-bit B */
#define RGB565(r,g,b) \
    (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

/* Surface hierarchy: BG < SURFACE < SURFACE_2 < SURFACE_3 */
#define UI_BG          RGB565(7, 10, 15)       /* ~#0A0A0F */
#define UI_SURFACE     RGB565(17, 24, 35)      /* ~#111823 */
#define UI_SURFACE_2   RGB565(27, 37, 52)      /* ~#1B2534 */
#define UI_SURFACE_3   RGB565(36, 49, 67)      /* ~#243143 */
#define UI_BORDER      RGB565(42, 58, 79)      /* ~#2A3A4F */

/* Text contrast */
#define UI_TEXT        RGB565(232, 238, 245)   /* ~#E8EEF5 (primary) */
#define UI_TEXT_2      RGB565(164, 177, 193)   /* ~#A4B1C1 (secondary) */
#define UI_MUTED       RGB565(112, 130, 151)   /* ~#708297 (hint/disabled) */

/* Semantic colors */
#define UI_ACCENT      RGB565(255, 176, 32)    /* ~#FFB020 (amber: focus, action) */
#define UI_ACCENT_DARK RGB565(128, 82, 8)      /* ~#805208 (amber shadow) */
#define UI_DATA        RGB565(34, 211, 238)    /* ~#22D3EE (cyan: sensor/live) */
#define UI_OK          RGB565(53, 208, 127)    /* ~#35D07F (green: success) */
#define UI_WARNING     RGB565(255, 197, 61)    /* ~#FFC53D (yellow: caution) */
#define UI_DANGER      RGB565(255, 59, 48)     /* ~#FF3B30 (red: critical) */
#define UI_DISABLED    RGB565(55, 66, 80)      /* ~#374250 (disabled state) */

/* Test swatches - use in Diagnostics to verify contrast */
#define UI_SWATCH_COUNT 8
static const uint16_t ui_swatches[] = {
    UI_BG, UI_SURFACE, UI_SURFACE_2, UI_SURFACE_3,
    UI_TEXT, UI_ACCENT, UI_DATA, UI_DANGER
};
```

**Verification on hardware:** Add a test screen in Diagnostics that renders color swatches and has user confirm contrast is readable.

### 3.3 Spacing system

Use an 8-pixel grid:

```c
#define UI_GAP_XS      4
#define UI_GAP_S       8
#define UI_GAP_M       12
#define UI_GAP_L       16
#define UI_RADIUS      4
#define UI_BORDER_W    1

/* Screen layout anchors */
#define UI_TOP_H       22      /* status bar height */
#define UI_BOTTOM_H    20      /* action bar height */
#define UI_CONTENT_Y   23      /* content start */
#define UI_CONTENT_H   196     /* content height: 219 - 23 */
#define UI_DIVIDER_Y   22      /* line between top bar and content */
#define UI_ACTION_Y    220     /* action bar start */
```

Every screen uses:
- Content area: y = 23 to 219.
- Top status bar: y = 0 to 21 (22 px total).
- Divider: 1 px at y = 22.
- Bottom action/hint bar: y = 220 to 239 (20 px total).

### 3.4 Typography

The current 5×7 font can remain for labels, but the OS needs a stronger hierarchy.

| Role | Target height | Color | Usage |
|---|---:|---|---|
| Hero value | 40–64 px | `UI_TEXT` | Speed, countdown, score |
| Screen title | 16–20 px | `UI_TEXT` | HOME, SETTINGS, GAMES |
| Card label | 12–16 px | `UI_TEXT` | App names, setting names |
| Secondary value | 12–18 px | `UI_DATA` or `UI_TEXT` | Distance, gear caps |
| Hint | 7–10 px | `UI_MUTED` | Controller help only |

**Implement these renderers:**

```c
/* os_text.h */
typedef enum {
    UI_FONT_TINY,       /* 5×7 existing */
    UI_FONT_SMALL,      /* ~10 px */
    UI_FONT_MEDIUM,     /* ~14 px */
    UI_FONT_LARGE,      /* ~24 px */
    UI_FONT_HERO,       /* ~48 px digits */
} ui_font_t;

void gfx_text(int x, int y, const char *s, ui_font_t font, uint16_t color);
void gfx_text_center(int x, int y, int w, const char *s, ui_font_t font, uint16_t color);

/* 7-segment-style large digits: 0–9, -, : */
void gfx_number_7seg(int x, int y, int value, int digits, 
                     uint16_t fg, uint16_t bg);

/* Example: speed 75 in large digits */
gfx_number_7seg(160 - 20, 100, 75, 2, UI_TEXT, UI_BG);
gfx_text(180, 130, "km/h", UI_FONT_SMALL, UI_TEXT_2);
```

Without large digits, Drive screen speed will look tiny and defeat the premium feel.

### 3.5 Reusable components

Build once and use on every screen:

```c
/* os_widgets.h */

/* Top bar: MODE | TIME | GEAR */
void ui_draw_topbar(const os_ctx_t *ctx, uint32_t now);

/* Bottom bar: left hint | right hint */
void ui_draw_bottombar(const char *left, const char *right);

/* Elevated panel with optional border and selection state */
void ui_panel(int x, int y, int w, int h, 
              uint16_t bg_color, bool selected);

/* Card: icon + label, supports selection highlight */
void ui_card(int x, int y, int w, int h, 
             const char *label, bool selected, uint16_t accent);

/* Progress bar or gauge */
void ui_progress(int x, int y, int w, int h, int value, int max,
                 uint16_t color);

/* Pill/badge: small rounded container */
void ui_pill(int x, int y, int w, const char *text, 
             bool active, uint16_t color);

/* Status icon: small indicator (light on/off) */
void ui_status_icon(int x, int y, uint16_t color, bool active);

/* Modal dialog: blocking overlay */
void ui_dialog(const char *title, const char *message,
               const char *confirm, const char *cancel);

/* Rounded rectangle (approx with straight lines + corner dots) */
void ui_rounded_rect(int x, int y, int w, int h, int radius,
                     uint16_t border_color, bool fill, uint16_t fill_color);
```

**Rounded corners:** Approximate cheaply with:
- Filled rectangle inset by radius.
- Four small corner rectangles or pixels to round the corners.
- Do not calculate curves per frame.

---

## 4. Global interaction model

### 4.1 Controller map

| Input | Drive | Menus | Games |
|---|---|---|---|
| Left stick | Drive/steer | No navigation | Game-dependent |
| Right stick | Car functions | Optional adjustment | Game-dependent |
| D-pad | Safe shortcuts only | Navigate/adjust with repeat | Game-dependent |
| A | Existing car function | Confirm/open | Primary action |
| B | Existing car function | Back/cancel | Exit/pause |
| Start | Quick overlay | Confirm/pause | Pause (auto-resume on release) |
| Back | Home/exit quick overlay | Back/cancel | Exit to Games Hub |
| Guide | Emergency stop | Emergency stop | Emergency stop |

**Input priority:**
1. Emergency stop and disconnect fail-safe.
2. Global safety functions.
3. OS navigation if UI owns input.
4. Game input if active.
5. Normal car controls.

### 4.2 Transitions

Keep transitions short and non-blocking:

- Screen entry: 140–180 ms.
- Focus movement: 80–120 ms.
- Modal open: 120 ms.
- Toast notification: visible 900–1200 ms.
- Warning pulse border: 500–700 ms, only for warnings.

**Integer easing (no float per frame):**

```c
static int ease_out_quad(int from, int to, uint32_t elapsed, uint32_t duration)
{
    if (elapsed >= duration) return to;
    int32_t t = (int32_t)(elapsed * 256 / duration);
    int32_t inv = 256 - t;
    int32_t eased = 256 - (inv * inv / 256);  /* Q14.8 fixed-point */
    return from + (to - from) * eased / 256;
}
```

**Do not animate every element.** Animate screen/card position and key values (speed, distance, mode) only.

---

## 5. Complete screen redesign

### 5.1 Boot screen

**Layout:**
- Background `UI_BG`.
- Center robotic hexagon/car emblem (64×64).
- Large title: `AZAM CAR OS` in `UI_TEXT` at ~20 px.
- Below: `ROBOTICS CONTROL SYSTEM` in `UI_MUTED` at ~10 px.
- Bottom boot checklist:
  - `✓ CONTROL`, `✓ SENSORS`, `✓ DISPLAY`, `✓ PAD` (each turns green on completion).
- Amber progress line from x=40 to x=280 (3 px high).

**Animation (timer-driven, no blocking delays):**
- 0–300 ms: emblem lines draw outward (scale from 0.5 to 1.0).
- 300–800 ms: title fades in.
- 800–1400 ms: subsystem labels turn from `UI_MUTED` to `UI_OK` one by one.
- At 1500 ms: enter Drive screen.

### 5.2 Drive screen

**This is the default screen and must be the clearest.**

**Layout:**
- **Top bar:** Mode (MANUAL/CRAWL/AUTO) left | controller link icon right | gear G1–G5 centered.
- **Center:** Very large speed value (48–64 px) occupying approximately 120×70.
  - Below speed: `POWER` or `KM/H` label (specify which based on measurement).
  - Use 7-segment large digits, not scaled 5×7 font.
- **Left column (y=50–200):**
  - Distance card: `L xxx cm` (cyan if clear, amber if approaching, red if critical).
  - Separator line.
  - Distance card: `F xxx cm`.
  - Separator line.
  - Distance card: `R xxx cm`.
- **Bottom center (y=210–230):**
  - Five gear pills: `G1 G2 G3 G4 G5`.
  - Selected gear highlighted with amber background and white text.
- **Right column (2×3 status icons):**
  - Row 1: Headlight (white circle on/off), Hazard (red H on/off), Mist (vapor icon).
  - Row 2: Spark (bolt icon), Auto mode (radar icon), IMU (gyro icon).

**Sensor colors:**
- Clear: `UI_DATA` (cyan).
- Approaching configured obstacle threshold (e.g., 30 cm): `UI_WARNING` (amber).
- Immediate danger/stop threshold (e.g., 15 cm): `UI_DANGER` (red).
- Invalid sensor (65535): `UI_MUTED` (`--`), never zero.

**Smoothing (displayed values only; control values untouched):**
```c
// In os_update(), not in real-time loop
os_ctx->shown_speed += (target_speed - os_ctx->shown_speed) / 4;
if (abs(os_ctx->shown_speed - target_speed) < 1) 
    os_ctx->shown_speed = target_speed;

os_ctx->shown_front += (target_front - os_ctx->shown_front) / 3;
if (abs(os_ctx->shown_front - target_front) < 2) 
    os_ctx->shown_front = target_front;
```

**Drive alerts (overlaid only when active):**
Show a centered warning card only for:
- `⚠ E-STOP ACTIVE` → pulsing red border.
- `⚠ CONTROLLER LOST` → pulsing amber border.
- `⚠ FRONT OBSTACLE` → pulsing red border.
- `⚠ SENSOR FAULT` → pulsing amber border.

```c
static void os_draw_drive_alert(const os_ctx_t *ctx, uint32_t now)
{
    if (!os_ctx->alert_active) return;
    
    /* Alert auto-dismisses after 3 seconds, except E-STOP */
    if (ctx->alert_type != ALERT_ESTOP && 
        now - ctx->alert_start_ms > 3000) {
        ctx->alert_active = false;
        return;
    }
    
    uint16_t pulse = ((now / 300) & 1) ? UI_DANGER : UI_WARNING;
    int alert_x = 100, alert_y = 100, alert_w = 120, alert_h = 40;
    ui_panel(alert_x, alert_y, alert_w, alert_h, UI_SURFACE_3, false);
    ui_rounded_rect(alert_x, alert_y, alert_w, alert_h, 4, pulse, false, 0);
    gfx_text_center(alert_x, alert_y + 8, alert_w, 
                    ctx->alert_message, UI_FONT_SMALL, UI_TEXT);
}
```

Alerts may pulse their border, never flash the entire screen (causes distraction/epilepsy risk).

### 5.3 Home launcher

**Layout:**
Six cards in a 3×2 grid:
- DRIVE, ANALYTICS, OLED
- SETTINGS, GAMES, DIAGNOSTICS

Suggested geometry:
- Columns at x: 12, 114, 216.
- Rows at y: 36, 126.
- Card size: 92×78 (includes padding).
- Gap: 10 px.

Each card:
- 24×24 geometric icon (simple lines/shapes).
- 12–16 px label below icon.
- Selected: `UI_SURFACE_2` bg, white label, `UI_ACCENT` border (2 px), +2 px upward offset.
- Unselected: `UI_SURFACE` bg, `UI_MUTED` label, `UI_BORDER` (1 px).

**Navigation:**
- Left/right: move one column (wraps).
- Up/down: change row (wraps).
- A: open selected screen.
- B: return to Drive.

### 5.4 Analytics screen

**Layout:**
Three stacked chart cards, each scrolling vertically:

- **SPEED/POWER:** 0–100%.
- **FRONT DISTANCE:** 0–200 cm.
- **YAW:** -180 to +180 degrees.

Each card:
- Title in `UI_TEXT_2` (10 px).
- Large current value (32 px) at top-right.
- 1-pixel light grid (spacing 16–20 px).
- 64–128 historical samples (cyan trace).
- Warning/danger section highlighted in amber/red where thresholds apply.
- Thin `UI_BORDER` around card.

**Redraw strategy:**
Only redraw a graph if a new history sample was added (every ~100 ms), not every frame.

```c
static void os_draw_analytics(const os_ctx_t *ctx, uint32_t now)
{
    if (!os_ctx->analytics_available) {
        gfx_text(40, 100, "ANALYTICS DISABLED", UI_FONT_MEDIUM, UI_MUTED);
        return;
    }
    
    /* Graph 1: Speed (y=40–110) */
    if (os_should_redraw_graph_speed(ctx, now)) {
        ui_draw_graph_speed(ctx);
    }
    
    /* Graph 2: Front distance (y=125–195) */
    if (os_should_redraw_graph_front(ctx, now)) {
        ui_draw_graph_front(ctx);
    }
    
    /* Graph 3: Yaw (y=210–220 indicator bar only) */
    if (os_should_redraw_graph_yaw(ctx, now)) {
        ui_draw_graph_yaw(ctx);
    }
}
```

### 5.5 Diagnostics screen

**Pages (use tabs):**

1. **SYSTEM:**
   - `FREE HEAP: xxx KB`
   - `PSRAM FREE: xxx KB`
   - `HEAP MIN: xxx KB`
   - `UPTIME: HH:MM:SS`

2. **SENSORS:**
   - `LEFT: xxx cm (avg)` in cyan.
   - `FRONT: xxx cm (avg)` in cyan.
   - `RIGHT: xxx cm (avg)` in cyan.
   - `YAW: xxx° (live)` in cyan.
   - `IMU: OK/LOST` in green or red.

3. **CONTROL:**
   - `MODE: MANUAL/CRAWL/AUTO`.
   - `AUTO STATE: CRUISE/SLOW/TURN_L/...` (if applicable).
   - `MOTOR L: tgt=xxx cur=xxx`.
   - `MOTOR R: tgt=xxx cur=xxx`.
   - `GEAR: Gx`.

4. **DISPLAY:**
   - `TFT FPS: xx (target 20–30)`.
   - `TFT AVG TIME: xxx ms`.
   - `OLED FPS: xx (target 10–12.5)`.
   - `OLED AVG TIME: xxx ms`.
   - `DIRTY REGIONS: xx (last frame)`.

Primary values must be **medium/large** (14–20 px). Tiny debug logs do not belong on this screen.

**Optional:** Add a `COPY/LOG` action only if serial logging is already supported; otherwise do not create fake functionality.

### 5.6 Settings screen

**Structure:**
Split settings into categories (use D-pad up/down to navigate):

- DRIVE: speed cap, obstacle distance.
- SAFETY: emergency stop behavior (custom).
- LIGHTS: LED brightness, headlight mode.
- SOUND: engine volume, horn volume.
- DISPLAY: TFT brightness, contrast.
- OLED: OLED brightness, layout selection.
- SYSTEM: reset NVS, reboot.

**Layout:**
- Left 42%: category/item list (5 visible rows max, scrollable).
- Right 58%: large selected value and slider/choice cards.
- Bottom: `A:APPLY`, `B:BACK`, `START:SAVE`.

**Behavior:**
- D-pad up/down: selects item (with repeat).
- D-pad left/right: adjusts value (with repeat).
- A: applies current item (optional; can auto-apply on adjustment).
- Start: saves all settings to NVS.
- B: opens `Discard unsaved changes?` only if values changed.

**Draft management:**
Maintain a draft copy so values can be canceled cleanly:

```c
typedef struct {
    uint8_t speed_cap;
    uint8_t engine_vol;
    uint8_t obstacle_cm;
    uint8_t led_bright;
    uint8_t ui_brightness;
    uint8_t gear_caps[5];
    uint8_t oled_layout;
    bool changed;
} os_settings_draft_t;

// On Settings entry: copy current settings to draft
// On Start: if draft.changed, copy draft to g and save to NVS
// On B: if draft.changed, ask; if yes, discard; else exit
```

### 5.7 OLED Control screen

**Layout:**
- Main: 128×64 OLED preview enlarged 2× (i.e., 256×128) in center.
- Thin light border (UI_BORDER) around preview.
- Render preview from the same model used by physical OLED.

**Layout options (D-pad up/down selects):**
- MINI HUD: current mode / speed / distance.
- RADAR: auto mode radar view (if applicable).
- STATUS: system status (battery, uptime, etc.).
- CUSTOM TEXT: user-entered text (if supported).

**Controls:**
- Up/down: selects layout.
- Left/right: changes preset or option (context-dependent).
- A: applies (saves to NVS).
- Y: previews without saving (for testing).
- B: returns to Home.

**Physical OLED update:**
Remains throttled to approximately 80–100 ms and updates only when its content changes.

### 5.8 Games Hub

**Layout:**
Horizontal carousel with smooth transitions:

- Selected game card centered at (118, 150) — full size.
- Previous/next cards at (-60, 150) and (276, 150) — partially visible, scaled 0.7×.
- Large game title (20 px) below card.
- Simple icon or screenshot preview (64×48).
- High score displayed.
- Control hint (small text).
- `A:PLAY`, `B:BACK`.

**Navigation:**
- Left/right: scroll carousel.
- A: enter selected game.
- B: return to Home.

**Five games:**

#### Game 1: Lane Runner

- Left stick or D-pad changes lane (3–5 lanes).
- RT accelerates game speed progressively.
- LT brakes (optional).
- Obstacles scroll toward the player.
- Large score at top-center.
- Game over: show final score and `START:RETRY` or `A:MENU`.

#### Game 2: Dodge & Collect

- Free horizontal movement (left stick or D-pad).
- Avoid red hazards (descending).
- Collect amber energy cells for points.
- Three large life icons (top-right).
- Game over when lives = 0.

#### Game 3: Reflex Core

- A/B/X/Y symbols appear at center with a shrinking timer ring.
- Player presses the corresponding button before ring disappears.
- Large symbol (48 px).
- Combo counter and average reaction time shown clearly.
- Difficulty increases: shorter timer, faster sequence.

#### Game 4: Pattern Memory

- Four large colored pads in a square (correspond to A/B/X/Y).
- Game shows sequence: pads light up in order.
- Player repeats sequence by pressing pads.
- Sequence grows by one each round.
- Score = sequence length.
- Optional: LED-strip synchronization (pads glow external LEDs), but motors remain parked.

#### Game 5: Sensor Challenge

- Uses L/F/R ultrasonic readings as optional input.
- Example: collect targets by avoiding obstacles; ultrasonic distance gates collection.
- Must provide controller fallback if sensors are invalid (65535).
- On-screen indicator: `[SENSOR]` or `[CTRL]` shows which input is active.
- Pre-game instruction panel explains controls (3–5 seconds, non-blocking).

**Game safety (CRITICAL):**
```c
if (os_game_active()) {
    // Before first game frame:
    g.tgt_l = 0;
    g.tgt_r = 0;
    g.mist = false;
    g.spark = false;
    
    // Keep emergency stop available
    // Do NOT clear g.estop
    
    // Read game input
    os_game_frame(&os_ctx, pad, now);
    
    // Motors remain parked while game is running
}
```

When entering any game:
- Set motor targets to zero before the first game frame.
- Disable mist and spark.
- Keep emergency stop active (do not clear if already latched).
- Keep controller-disconnect handling active.

**Pause overlay (Start opens):**
- RESUME
- RESTART
- EXIT TO GAMES

B should pause first (auto-resume on release) rather than instantly discard running score, unless held for 800 ms.

### 5.9 Quick overlay (Start on Drive screen)

On the Drive screen, Start opens a small quick overlay without entering full Home:

- Gear selector (G1–G5).
- Mode selector (MANUAL / CRAWL / AUTO).
- Headlight toggle.
- Hazard toggle.
- OLED layout preview + selector.
- `EXIT TO HOME` button.

Overlay must not hide emergency warnings (render alerts above overlay).

If car is moving, restrict settings that could distract (e.g., disable mode change until motors stop).

---

## 6. Rendering architecture

### 6.1 Dirty-region rendering

A 320×240 RGB565 frame is 153,600 bytes. At 40 MHz SPI, raw transfer ≈ 30.72 ms. Unconditional full-screen every 33 ms leaves no margin.

**Dirty-region tracking:**

```c
typedef struct {
    int16_t x, y, w, h;
} ui_dirty_rect_t;

#define OS_MAX_DIRTY_RECTS 4

typedef struct {
    ui_dirty_rect_t rects[OS_MAX_DIRTY_RECTS];
    uint8_t count;
    bool full_frame;
} ui_dirty_t;

static ui_dirty_t g_dirty = {0};

void ui_mark_dirty(int x, int y, int w, int h)
{
    if (g_dirty.full_frame) return;  /* Already full */
    
    if (g_dirty.count >= OS_MAX_DIRTY_RECTS) {
        /* Too many regions; fall back to full-frame */
        g_dirty.full_frame = true;
        g_dirty.count = 0;
        return;
    }
    
    g_dirty.rects[g_dirty.count++] = (ui_dirty_rect_t){x, y, w, h};
}

void ui_clear_dirty(void)
{
    g_dirty.count = 0;
    g_dirty.full_frame = false;
}

bool ui_has_dirty(void)
{
    return g_dirty.full_frame || g_dirty.count > 0;
}

void gfx_push_dirty(void)
{
    if (g_dirty.full_frame) {
        /* Push entire frame */
        tft_push_full_frame(framebuffer);
    } else {
        /* Push dirty rectangles */
        for (uint8_t i = 0; i < g_dirty.count; i++) {
            ui_dirty_rect_t *r = &g_dirty.rects[i];
            tft_push_region(framebuffer, r->x, r->y, r->w, r->h);
        }
    }
    ui_clear_dirty();
}
```

**Policy by screen:**
- Boot/game screens: full frame allowed (single frame, then change).
- Drive screen: redraw dynamic regions only (speed, distance, status icons).
- Home: redraw previous and new focused cards.
- Settings: redraw selected rows and value panel.
- Analytics: redraw graphs at 10 Hz (when history updates).
- OLED Control: redraw preview on layout change.

### 6.2 Frame pacing with deadlines

Use deadlines, not `last = now`, to reduce jitter:

```c
typedef struct {
    uint32_t next_tft_ms;
    uint32_t next_oled_ms;
    uint32_t tft_target_fps;  /* typically 20–30 */
    uint32_t oled_target_fps; /* typically 10–12.5 */
} os_frame_pace_t;

static os_frame_pace_t g_pace = {0};

void os_update_render(os_ctx_t *ctx, uint32_t now)
{
    // TFT rendering
    if ((int32_t)(now - g_pace.next_tft_ms) >= 0) {
        g_pace.next_tft_ms += 1000 / ctx->tft_target_fps;
        
        /* Guard against large time jumps (overflow, pause, etc.) */
        if ((int32_t)(now - g_pace.next_tft_ms) > 100) {
            g_pace.next_tft_ms = now + 1000 / ctx->tft_target_fps;
        }
        
        os_render_screen(ctx, now);
        if (ui_has_dirty()) {
            gfx_push_dirty();
        }
    }
    
    // OLED rendering (separate timing)
    if ((int32_t)(now - g_pace.next_oled_ms) >= 0) {
        g_pace.next_oled_ms += 1000 / ctx->oled_target_fps;
        
        if ((int32_t)(now - g_pace.next_oled_ms) > 100) {
            g_pace.next_oled_ms = now + 1000 / ctx->oled_target_fps;
        }
        
        os_render_oled(ctx, now);
    }
}
```

**Suggested rates:**

| Screen | Update target | Dirty regions |
|---|---:|---|
| Drive | 20–30 FPS | Yes (dynamic speed/distance/status) |
| Home | 10–20 FPS (transition) | Yes (focus change) |
| Settings | 10–20 FPS (transition) | Yes (selected row + value) |
| Analytics | 10 FPS (graphs) | Yes (graph update on history sample) |
| Games | 30 FPS | No (full-frame typical) |
| OLED Control | 10 FPS | Yes (layout change) |
| OLED hardware | 10–12.5 FPS | Yes (change-driven) |

### 6.3 Asset storage and allocation

Use PSRAM for decoded assets, sprite sheets and analytics history. Keep raw assets packed in flash and load/decode outside real-time loop.

**PSRAM budget (8 MB total):**

| Use | Suggested budget |
|---|---:|
| UI icons/fonts (decoded) | 0.5–1.0 MB |
| Game sprite sheets (decoded) | 1.5–2.5 MB |
| Decoded backgrounds | 0.5–1.0 MB |
| Game level/state data | 0.5–1.0 MB |
| Analytics history (100 samples × 3 series × 2 bytes) | 0.6 KB |
| Temporary decode buffer | 0.5–1.0 MB |
| Cache/workspace | 0.5–1.0 MB |
| **Unallocated safety reserve** | **≥ 1.0 MB** |

**Do not fill all 8 MB.** Keep a reserve for fragmentation, future features and temporary allocations.

```c
static void *s_asset_cache = NULL;
static size_t s_asset_cache_used = 0;
#define OS_ASSET_CACHE_SIZE (3 * 1024 * 1024)

void os_assets_init(void)
{
    s_asset_cache = heap_caps_malloc(OS_ASSET_CACHE_SIZE, 
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_asset_cache) {
        ESP_LOGE(TAG, "PSRAM asset cache allocation failed");
        // Fallback: reduce features or use internal RAM
    }
}

void *os_asset_alloc(size_t size)
{
    if (!s_asset_cache) return NULL;
    if (s_asset_cache_used + size > OS_ASSET_CACHE_SIZE) {
        ESP_LOGW(TAG, "asset cache full: %u / %u", 
                 s_asset_cache_used, OS_ASSET_CACHE_SIZE);
        return NULL;
    }
    void *ptr = (uint8_t *)s_asset_cache + s_asset_cache_used;
    s_asset_cache_used += size;
    return ptr;
}
```

---

## 7. Recommended file structure

```text
main/
├── car.c                    /* existing real-time control (UNTOUCHED except safety gate) */
├── car_os.c                 /* state, routing, scheduling */
├── car_os.h
├── os_theme.h               /* colors, spacing, typography tokens */
├── os_widgets.c             /* panels, cards, bars, pills, dialogs */
├── os_widgets.h
├── os_text.c                /* font rendering (tiny, small, medium, large, 7-seg) */
├── os_text.h
├── os_screens_tft/          /* screen composition (split into files) */
│   ├── screen_boot.c
│   ├── screen_drive.c
│   ├── screen_home.c
│   ├── screen_settings.c
│   ├── screen_analytics.c
│   ├── screen_diagnostics.c
│   ├── screen_oled_control.c
│   ├── screen_games_hub.c
│   └── os_screens_tft.h
├── os_oled.c                /* physical OLED model/render */
├── os_oled.h
├── os_assets.c              /* flash → PSRAM asset loader/cache */
├── os_assets.h
├── car_games/               /* game registry and lifecycle */
│   ├── car_games.c
│   ├── car_games.h
│   ├── game_lane.c          /* Lane Runner */
│   ├── game_dodge.c         /* Dodge & Collect */
│   ├── game_reflex.c        /* Reflex Core */
│   ├── game_memory.c        /* Pattern Memory */
│   └── game_sensor.c        /* Sensor Challenge */
├── tft_display.c            /* low-level TFT driver ONLY (not composition) */
└── tft_display.h
```

**Important:** `tft_display.c` must remain a display **driver** only (transfer_to_tft, set_window, etc.). Remove all complete screen composition. Place screen logic in `os_screens_tft/*.c`.

---

## 8. OS context redesign

**Suggested context structure:**

```c
typedef struct {
    /* State machine */
    os_state_t state;
    os_state_t prev_state;
    uint32_t state_enter_ms;
    bool transition_active;

    /* Selection per screen */
    uint8_t home_sel;
    uint8_t oled_sel;
    uint8_t game_sel;
    uint8_t settings_sel;
    uint8_t settings_category;

    /* Live display values (smoothed, not car state) */
    int16_t shown_speed;
    int16_t shown_front;
    int16_t shown_left;
    int16_t shown_right;
    int16_t shown_yaw;

    /* Gear and OLED config */
    uint8_t gear;
    uint8_t oled_layout;
    char oled_custom_text[32];

    /* Rendering */
    uint32_t next_tft_ms;
    uint32_t next_oled_ms;
    uint16_t tft_target_fps;
    uint16_t oled_target_fps;
    bool screen_dirty;
    bool topbar_dirty;
    bool oled_dirty;

    /* Input */
    uint32_t last_input_ms;
    os_key_repeat_t repeat_state;

    /* Settings draft */
    os_settings_draft_t draft;
    bool settings_changed;

    /* Analytics */
    bool analytics_available;
    uint16_t analytics_hist_n;
    uint32_t hist_update_ms;

    /* Alerts */
    uint8_t alert_type;
    uint32_t alert_start_ms;
    bool alert_active;
    char alert_message[64];

    /* Performance counters */
    uint32_t frame_us;
    uint32_t push_us;
    uint32_t max_frame_us;
    uint16_t fps;
    uint32_t frames;
    uint32_t skipped_frames;
} os_ctx_t;
```

**Do not duplicate every car value in `os_ctx_t`.** Copy only:
- Values required for interpolation (speed, distance, yaw).
- Draft settings for cancel/apply.
- Change-detection flags (for dirty regions).

---

## 9. Performance instrumentation

Add lightweight diagnostics:

```c
typedef struct {
    uint32_t frame_us;       /* render time */
    uint32_t push_us;        /* SPI transfer time */
    uint32_t max_frame_us;   /* peak render time */
    uint16_t fps;            /* frames per second */
    uint32_t frames;         /* total frame count */
    uint32_t skipped_frames; /* unchanged frames skipped */
    uint32_t internal_free;  /* internal RAM free */
    uint32_t psram_free;     /* PSRAM free */
    uint32_t heap_min;       /* minimum free heap */
    uint32_t uptime_ms;      /* system uptime */
} os_perf_t;

static os_perf_t g_perf = {0};
```

**Measure:**
- Render time (before push).
- SPI push time (after push).
- Frames per second.
- Skipped unchanged frames.
- Internal free heap and minimum free heap.
- PSRAM free heap.
- Car-loop maximum execution time.

**Log summary every 5 seconds (non-blocking):**

```c
if ((now / 5000) != (ctx->last_perf_log_sec / 5000)) {
    ctx->last_perf_log_sec = now;
    ESP_LOGI(TAG, 
        "PERF: TFT %d FPS (max %lu us), OLED %d FPS, "
        "skipped %lu, heap %lu KB (min %lu KB), PSRAM %lu KB",
        g_perf.fps, g_perf.max_frame_us,
        oled_fps, g_perf.skipped_frames,
        esp_get_free_heap_size() / 1024,
        esp_get_minimum_free_heap_size() / 1024,
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
}
```

---

## 10. AI implementation instructions

Give the coding AI the current project plus this document and use this instruction:

> Update the existing ESP-IDF ESP32-S3 Car OS according to this specification. First inspect all headers, enum definitions, `car.c`, TFT driver, OLED functions, game registry, CMake files and sdkconfig. Do not invent APIs that do not exist. Preserve the current pin map and all motor, ultrasonic, IMU, controller, mist, spark, lighting, audio and emergency-stop behavior. Fix the trailing comma syntax error (section 2.1) and shared selection-state bug (section 2.2). CRITICAL: implement the safety gate that parks motors before OS input is processed (section 2.4). Separate UI components, theme, screens, assets and games into modules per section 7 file structure. Implement a premium automotive dark theme with large readable text, amber interaction accent, cyan data color and semantic warning colors (sections 3.2–3.4). Add non-blocking transitions, dirty-region rendering, change-driven OLED updates and safe game parking. Use PSRAM for assets/history and internal DMA-capable RAM for display transfer buffers. Build after each small change per section 10 implementation order. Fix all compiler errors and warnings. Never clear a latched emergency stop when opening or leaving games. Report every changed file and any behavior that could not be verified on hardware.

### Required implementation order

1. **Baseline build:** Build untouched project; save warnings/errors.
2. **Fix syntax & safety (sections 2.1–2.4):** Fix trailing comma, separate state, safe enum lookup, motor-parking gate. Rebuild.
3. **Add allocation fixes (sections 2.5–2.6):** Unified history allocation with fallback. Rebuild.
4. **Add input repeat & Back+Start fix (sections 2.7–2.8):** Implement key-repeat and armed latch. Rebuild.
5. **Create theme & widgets (sections 3.2–3.5):** `os_theme.h`, `os_widgets.c/h`, reusable components. Rebuild.
6. **Add text rendering (section 3.4):** `os_text.c/h` with medium/large fonts and 7-segment digits. Rebuild and **verify on TFT**.
7. **Boot screen (section 5.1):** Redesign boot with emblem, title, progress bar, subsystem checklist. Rebuild.
8. **Top/bottom bars:** Common UI elements used by all screens. Rebuild.
9. **Drive screen (section 5.2):** Redesign with large speed, distance cards, gear pills, status icons. **Verify car input still responsive.**
10. **Home launcher (section 5.3):** Redesign 3×2 grid with navigation. Rebuild.
11. **Settings (section 5.6):** Redesign with categories, draft/save/cancel flow. Rebuild.
12. **Analytics (section 5.4):** Three graphs with history, change-driven redraw. Rebuild.
13. **Diagnostics (section 5.5):** Four tabbed pages (System, Sensors, Control, Display). Rebuild.
14. **OLED Control (section 5.7):** Layout selector with 2× preview. Rebuild.
15. **Games Hub (section 5.8):** Carousel layout. Rebuild.
16. **Games implementation:** Lane Runner, Dodge & Collect, Reflex Core, Pattern Memory, Sensor Challenge (each with safety gate, fallback input). Rebuild and **test each game**.
17. **Quick overlay (section 5.9):** On Drive screen, Start opens gear/mode/light selector. Rebuild.
18. **Dirty-region rendering (section 6.1):** Implement tracking and conditional push. Rebuild.
19. **Frame pacing (section 6.2):** Deadline-based pacing for TFT and OLED. Rebuild.
20. **Performance counters (section 9):** Add diagnostics and logging. Rebuild.
21. **Final validation:** Flash and monitor for heap usage, frame timing, car-loop responsiveness. Report measurements.

Build after each step. Do not skip steps or combine large changes.

---

## 11. Acceptance checklist

### Visual
- [ ] No screen is plain black with only uniform blue text.
- [ ] Primary values (speed, distance, score) are large (≥24 px) and readable at a glance.
- [ ] Every screen uses consistent top/bottom layout (sections 3.3, 5.1–5.9).
- [ ] Amber (`UI_ACCENT`) indicates focus/action; cyan (`UI_DATA`) indicates sensor/live data.
- [ ] Red (`UI_DANGER`) appears only for critical alerts.
- [ ] Selected elements are obvious without excessive glow or pulsing.
- [ ] Settings show no more than 5 visible rows; remaining items scroll.
- [ ] Invalid sensor values display `--`, never zero or `65535`.
- [ ] Color contrast passes visual check: text on surfaces is readable in bright and dim lighting.
- [ ] Rounded corner approximation looks polished (not blocky).

### Interaction
- [ ] Home/OLED/Game selections persist when returning to each screen.
- [ ] D-pad left/right adjust settings with repeat (90 ms interval after 350 ms hold).
- [ ] Back+Start does not retrigger while both held (armed latch).
- [ ] B consistently returns or cancels; A confirms.
- [ ] Emergency stop (B_GUIDE) works in every OS state.
- [ ] Emergency stop state persists across screen transitions.
- [ ] Controller disconnect parks motors immediately (0ms latency).
- [ ] Start on Drive opens quick overlay; Start in quick overlay exits cleanly.

### Performance
- [ ] No blocking delays in OS draw/input functions.
- [ ] No large allocations per frame.
- [ ] Unchanged frames are skipped (dirty regions working).
- [ ] Games maintain approximately 30 FPS.
- [ ] Analytics graphs update at approximately 10 FPS (history sample rate).
- [ ] OLED updates are change-driven or throttled to ≤12.5 FPS.
- [ ] **Measured:** At least 1 MB PSRAM remains free under normal operation.
- [ ] **Measured:** Internal heap has stable floor above 50 KB.
- [ ] **Measured:** Car-control loop maximum time is <10 ms (before and after OS changes).

### Safety
- [ ] Motors are parked (g.tgt_l = g.tgt_r = 0) before game input is processed.
- [ ] Entering any game disables mist and spark.
- [ ] Exiting any game does NOT clear emergency stop.
- [ ] UI failure (e.g., asset alloc fail) does not prevent motor stop commands.
- [ ] Asset allocation failure falls back gracefully without reboot loops.
- [ ] All five games have controller fallback if sensors are invalid (65535).

### Bug fixes
- [ ] Section 2.1: Trailing comma fixed.
- [ ] Section 2.2: Separate home/oled/game/settings selection fields.
- [ ] Section 2.3: Safe enum lookup (switch-based).
- [ ] Section 2.4: Motor-parking safety gate in car_task (CRITICAL).
- [ ] Section 2.5: Unified PSRAM history allocation with logged fallback.
- [ ] Section 2.6: History values hold last valid, never 65535 on graph.
- [ ] Section 2.7: D-pad repeat with 350 ms delay, 90 ms interval.
- [ ] Section 2.8: Back+Start armed latch, no retrigger while held.

---

## Final target

The finished system should feel like a **compact console-style automotive interface**:
- Large speed and sensor values readable at a glance.
- Polished launcher with clear categories.
- Readable settings with draft/apply/cancel flow.
- OLED preview/configurator.
- Clean diagnostics with measurable performance metrics.
- Five playable mini-games with safe motor parking.

The premium effect should come from:
- **Consistency:** every screen follows the same layout rules.
- **Spacing & hierarchy:** proper use of gaps and text sizes.
- **Semantic color:** meaningful colors that guide the eye.
- **Smooth restrained movement:** transitions are 140–180 ms, not jarringly instant.

Do not add heavy effects that compete with real-time car control. The UI is a **tool**, not a distraction.

---

## Summary of changes from original

1. ✅ **Safety gate (2.4):** E-STOP always available; motors parked before game input.
2. ✅ **Separate selections (2.2):** home_sel, oled_sel, game_sel, settings_sel, settings_category.
3. ✅ **History allocation (2.5):** Unified PSRAM block, logged fallback to internal RAM.
4. ✅ **Input repeat (2.7):** 350 ms delay, 90 ms repeat interval, non-blocking.
5. ✅ **Back+Start (2.8):** Armed latch prevents retrigger while held.
6. ✅ **Typography (3.4):** os_text.c/h with small/medium/large fonts and 7-segment digits.
7. ✅ **Theme tokens (3.2):** os_theme.h with semantic colors and verification swatches.
8. ✅ **Reusable widgets (3.5):** os_widgets.c/h with panels, cards, progress, pills, dialogs.
9. ✅ **Screen-specific files (7):** os_screens_tft/screen_*.c instead of one monolithic file.
10. ✅ **Dirty-region rendering (6.1):** Track and conditional push to reduce SPI time.
11. ✅ **Frame pacing (6.2):** Deadline-based, separate TFT/OLED timing.
12. ✅ **Asset cache (6.3):** Unified PSRAM buffer with fallback.
13. ✅ **Performance counters (9):** Instrumentation for heap, FPS, loop time.
14. ✅ **Sensor Challenge game (5.8):** Ultrasonic input with controller fallback.
15. ✅ **Game safety (5.8):** Motors parked, mist/spark off, E-STOP persistent.
16. ✅ **Quick overlay (5.9):** Start on Drive for gear/mode/light without full Home.
17. ✅ **All five games:** Designed with clear controls and visual hierarchy.
18. ✅ **Acceptance checklist (11):** Measurable visual, interaction, performance and safety criteria.

