/*
 * roof_light.c - ROOF RGB LIGHT (PART 7 locked spec).
 *
 * Single WS2812 pixel on GPIO18 (1 IC = 3 LEDs same colour).
 * Computed from now_ms (never vTaskDelay). Only energised in the DRIVE
 * context with estop clear and controller connected; everywhere else dark.
 */
#include "roof_light.h"
#include "car_global.h"
#include "car_os.h"            /* os_context() */
#include "xbox360.h"           /* xbox360_dongle_connected() */
#include "mic_in.h"            /* PART 8: MUSIC mode beat */
#include "driver/rmt_tx.h"
#include "led_strip.h"
#include "esp_log.h"

#define ROOF_DEFAULT_PERIOD_MS 400

/* ---- ROOF STRIP CONFIG -----------------------------------------------------
    Wire: DIN -> GPIO18, 5V/GND -> supply. 1 IC controlling 3 LEDs = 1 pixel. */
#define ROOF_STRIP_NUM  1

static const char *TAG = "roof";

static roof_light_state_t s_rl = {
    .mode      = ROOF_LIGHT_POLICE,   /* PART 7: default = POLICE */
    .enabled   = false,               /* always boot OFF */
    .brightness = 70,
    .color_idx = 0,
    .period_ms = ROOF_DEFAULT_PERIOD_MS,
    .changed_ms = 0,
};

static const char *s_labels[] = {
    "OFF", "POLICE", "STEADY", "RAINBOW", "BREATHE", "WARN", "CHASE", "MUSIC",
};

/* steady color dots: R B G C Y W */
static const uint8_t s_colors[ROOF_COLOR_COUNT][3] = {
    {255, 0, 0}, {0, 0, 255}, {0, 255, 0},
    {0, 255, 255}, {255, 255, 0}, {255, 255, 255},
};
static const char *s_color_names[ROOF_COLOR_COUNT] = {
    "RED", "BLUE", "GREEN", "CYAN", "YELLOW", "WHITE",
};

static led_strip_handle_t s_roof_strip = NULL;

const char *roof_light_label(void)
{
    if (s_rl.mode >= ROOF_LIGHT_COUNT) return "OFF";
    return s_labels[s_rl.mode];
}

void roof_light_color_rgb(uint8_t idx, uint8_t *r, uint8_t *g, uint8_t *b)
{
    idx %= ROOF_COLOR_COUNT;
    if (r) *r = s_colors[idx][0];
    if (g) *g = s_colors[idx][1];
    if (b) *b = s_colors[idx][2];
}

const char *roof_light_color_name(uint8_t idx)
{
    return s_color_names[idx % ROOF_COLOR_COUNT];
}

const roof_light_state_t *roof_light_state(void) { return &s_rl; }

void roof_light_init(void)
{
    /* WS2812 roof strip (RMT TX: rear=DMA, 2x front, roof non-DMA) */
    led_strip_config_t sc = {
        .strip_gpio_num   = ROOF_STRIP_GPIO,
        .max_leds         = ROOF_STRIP_NUM,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model        = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rc = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 48,
        .flags.with_dma = false,   /* S3 DMA-TX limited: rear strip owns it */
    };
    esp_err_t err = led_strip_new_rmt_device(&sc, &rc, &s_roof_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "roof strip RMT FAILED (err=%d)", err);
        s_roof_strip = NULL;
    } else {
        led_strip_clear(s_roof_strip);
        led_strip_refresh(s_roof_strip);
    }

    s_rl.enabled = false;                 /* never restore ON at boot  */
    ESP_LOGI(TAG, "roof light init (PART 7: WS2812 GPIO%d, default POLICE)",
             ROOF_STRIP_GPIO);
}

void roof_light_set_enabled(bool enabled) { s_rl.enabled = enabled; }
void roof_light_set_mode(roof_light_mode_t mode)
{
    if (mode >= ROOF_LIGHT_COUNT) mode = ROOF_LIGHT_OFF;
    s_rl.mode = mode;
}
void roof_light_set_brightness(uint8_t pct)
{
    s_rl.brightness = pct > 100 ? 100 : pct;
}
void roof_light_set_color_idx(uint8_t idx)
{
    s_rl.color_idx = (uint8_t)(idx % ROOF_COLOR_COUNT);
}
/* B double-tap quick-cycle (PART 7): OFF skipped while cycling */
void roof_light_cycle_mode(int dir)
{
    uint8_t m = (uint8_t)s_rl.mode;
    if (m == ROOF_LIGHT_OFF) m = ROOF_LIGHT_POLICE;
    else {
        /* cycle POLICE..MUSIC */
        int n = ROOF_LIGHT_COUNT - 1;
        int cur = (int)m - 1;
        cur = (cur + (dir >= 0 ? 1 : n - 1)) % n;
        m = (uint8_t)(cur + 1);
    }
    s_rl.mode = (roof_light_mode_t)m;
}
void roof_light_force_safe_off(void) { s_rl.enabled = false; }

/* push one pattern colour onto the pixel; skips refresh when unchanged
   (no RMT spam on static modes) */
static void roof_strip_show(uint8_t r, uint8_t g, uint8_t b)
{
    static uint8_t p_r = 0xFF, p_g = 0, p_b = 0;
    if (!s_roof_strip) return;
    if (r == p_r && g == p_g && b == p_b) return;
    p_r = r; p_g = g; p_b = b;
    led_strip_set_pixel(s_roof_strip, 0, r, g, b);
    led_strip_refresh(s_roof_strip);
}

static uint8_t scale_br(uint8_t v)   /* pattern 0..255 -> brightness-scaled */
{
    return (uint8_t)(((uint16_t)v * s_rl.brightness) / 100u);
}

/* hue 0..255 -> rgb (RAINBOW) */
static void hue_rgb(uint8_t h, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t region = h / 43;
    uint8_t rem = (uint8_t)((h - region * 43) * 6);
    uint8_t p = 0;
    uint8_t q = (uint8_t)(255 - rem);
    uint8_t t = rem;
    switch (region) {
    case 0: *r = 255; *g = t; *b = p; break;
    case 1: *r = q; *g = 255; *b = p; break;
    case 2: *r = p; *g = 255; *b = t; break;
    case 3: *r = p; *g = q; *b = 255; break;
    case 4: *r = t; *g = p; *b = 255; break;
    default: *r = 255; *g = p; *b = q; break;
    }
}

void roof_light_update(uint32_t now)
{
    /* DRIVE context energises the strip. Drive-screen overlays (roof bottom
       sheet / quick menu) get a LIVE preview while open - real menus, games,
       estop and disconnect stay dark. */
    input_context_t ctx = os_context();
    bool preview = false;
    os_ctx_t *oc = alexa_os_ctx();
    if (oc && (oc->roof_panel || oc->quick_open)) preview = true;
    if (!s_rl.enabled || (ctx != INPUT_CTX_DRIVE && !preview) ||
        g.estop || !xbox360_dongle_connected() || s_rl.brightness == 0) {
        roof_strip_show(0, 0, 0);
        return;
    }

    uint32_t period = s_rl.period_ms ? s_rl.period_ms : ROOF_DEFAULT_PERIOD_MS;
    uint32_t phase  = now % period;
    uint8_t r = 0, g = 0, b = 0;
    uint8_t cr, cg, cb;
    roof_light_color_rgb(s_rl.color_idx, &cr, &cg, &cb);

    switch (s_rl.mode) {
    case ROOF_LIGHT_POLICE: {             /* red/blue strobe (default) */
        uint32_t p = now % 800;
        if      (p <  80)               { r = 255; }
        else if (p >= 140 && p < 220)   { r = 255; }
        else if (p >= 400 && p < 480)   { b = 255; }
        else if (p >= 540 && p < 620)   { b = 255; }
        break;
    }
    case ROOF_LIGHT_STEADY:               /* solid selected color */
        r = cr; g = cg; b = cb;
        break;
    case ROOF_LIGHT_RAINBOW: {            /* hue cycle ~3s */
        uint8_t h = (uint8_t)((now / 12) & 0xFF);
        hue_rgb(h, &r, &g, &b);
        break;
    }
    case ROOF_LIGHT_BREATHE: {            /* pulse of steady color ~2s */
        uint32_t p = now % 2000;
        /* triangle 0..255 */
        uint16_t t = p < 1000 ? p * 255 / 1000 : (2000 - p) * 255 / 1000;
        r = (uint8_t)(cr * t / 255); g = (uint8_t)(cg * t / 255); b = (uint8_t)(cb * t / 255);
        break;
    }
    case ROOF_LIGHT_WARNING:              /* amber strobe */
        if (phase < period / 2) { r = 255; g = 150; }
        break;
    case ROOF_LIGHT_CHASE: {              /* single-pixel chase pulse */
        uint32_t p = now % 1000;
        uint8_t v = (uint8_t)(p * 255 / 1000);
        r = v; g = (uint8_t)(v / 2); b = (uint8_t)(255 - v);
        break;
    }
    case ROOF_LIGHT_MUSIC: {              /* PART 8: mic-reactive beat flash */
        static uint32_t s_flash_until = 0;
        if (mic_beat()) s_flash_until = now + 120;   /* <80ms path */
        if ((int32_t)(now - s_flash_until) < 0) {
            r = 200; g = 220; b = 255;    /* beat flash */
        } else if (mic_ready() && mic_rms() > 200) {
            uint8_t v = mic_rms() > 4000 ? 255 : (uint8_t)(mic_rms() * 255 / 4000);
            b = v; g = (uint8_t)(v / 3);  /* live level glow */
        } else {                          /* silent/absent mic: slow pulse */
            uint32_t p = now % 2400;
            uint8_t v = (uint8_t)((p < 1200 ? p : 2400 - p) * 255 / 1200 / 3);
            b = v;
        }
        break;
    }
    case ROOF_LIGHT_OFF:
    default:
        break;
    }

    roof_strip_show(scale_br(r), scale_br(g), scale_br(b));
}
