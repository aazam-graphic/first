# 🚗 AZAM CAR OS — Complete Project Details

**Project:** Xbox 360 (Redgear Pro) Controller se chalne wala Robot Car + "Car OS" Dashboard
**MCU:** ESP32-S3 (N16R8 module) | **Framework:** ESP-IDF v6.0.2 + FreeRTOS
**Project name:** `xbox360_controller`
**Repo:** https://github.com/yourschoicecollections-hash/first.git (branch: `main`)

> Ye file poore project ka single reference hai — hardware, pins, button maps, games, features, RAM/ROM usage, sab kuch yahan hai. Source code aur build logs se verify kiya gaya hai.
>
> 🔍 **Latest code review (17 Sep 2026):** `CODE_REVIEW_2026-09-17.md` — 9 bugs (sab fixed),
> pin-conflict report, strapping-pin warnings, race conditions. Build: 0 errors / 0 warnings.

---

## 1. Project Overview

Ye ek **ESP32-S3 based robot car** hai jise **Xbox 360 wireless controller (Redgear Pro dongle)** se USB Host ke through control kiya jata hai. Car ke upar ek **"Car OS"** chalta hai — ek custom dashboard/UI jo 2.8" TFT screen par render hota hai, jisme Drive HUD, Analytics, Settings, Games Hub, Diagnostics aur Alexa voice-control bridge sab shamil hain.

**Ek line mein:** Gamepad → ESP32-S3 (USB Host) → Motors (UART slave) + Lights + Servos + Sensors + TFT/OLED UI + WiFi/Alexa.

### Boot Flow
```
app_main() → 4 FreeRTOS tasks create:
  1. usb_host  (Core 0, prio 2, stack 4 KB)  - USB Host library events
  2. xbox360   (Core 0, prio 3, stack 8 KB)  - gamepad driver
  3. car       (Core 1, prio 2, stack 16 KB) - car control + Car OS + HUD
  4. alexa     (Core 1, prio 1, stack 16 KB) - WiFi + MQTT/TLS cloud bridge
```

---

## 2. Hardware Components (Sab Included)

| # | Component | Detail | Connection |
|---|-----------|--------|------------|
| 1 | **ESP32-S3 DevKit (N16R8)** | 16 MB Flash, 8 MB Octal PSRAM, dual-core 240 MHz | Main board |
| 2 | **Xbox 360 Wireless Receiver (Redgear Pro dongle)** | USB Host, 045E:028E / 045E:0719, max 4 controllers | Native USB-OTG port |
| 3 | **2.8" SPI TFT (ILI9341 / ST7789)** | 320×240 landscape, RGB565, 40 MHz SPI | SPI + GPIO |
| 4 | **0.96" OLED SSD1306** | 128×64, I2C addr 0x3C — mini HUD / OS mirror | I2C |
| 5 | **MPU-6500 IMU** | Accel + Gyro (Motion Radar UI ke liye), WHO_AM_I 0x70/0x71 | Same I2C bus (0x68/0x69) |
| 6 | **3× HC-SR04 Ultrasonic** | Left / Front / Right distance (cm) | GPIO trig/echo |
| 7 | **MAX98357A I2S Amplifier + Speaker** | Engine sound, horn, effects, UI clicks | I2S |
| 8 | **D1 Motor Driver Slave (UART)** | Left/Right motor commands @115200 UART (real brake supported) | UART TX |
| 9 | **2× Servo (Pan/Tilt)** | Spark cannon aim — LEDC ch0 (pan), ch1 (tilt) | GPIO 4/5 |
| 10 | **WS2812 Rear LED Strip** | 2 addressable pixels (HW strip = 6 physical LEDs: 3 left + 3 right) | GPIO 6 |
| 11 | **WS2812 Under-car Strip** | 12 addressable LEDs — rear strip ke baad **daisy-chained** (same GPIO 6) | GPIO 6 |
| 12 | **2× Front WS2812 RGB LEDs** | Front Left + Front Right indicator/headlight glow | GPIO 17 / 15 |
| 13 | **Relays (Active-LOW)** | Headlight (GPIO 46), Backlight (GPIO 14), Rear/spoiler light (GPIO 3), Mist maker (GPIO 16) | GPIO |
| 14 | **Roof Police Light (WS2812)** | 1 pixel (1 IC → 3 LEDs same color), RGB patterns — RMT TX driver | GPIO 18 |
| 15 | **Mist Maker** | Fog machine relay, A-hold se toggle + auto mode | GPIO 16 |
| 16 | **WiFi + AWS IoT (Alexa)** | MQTT/TLS over mqtts://, Device Shadow | On-chip radio |

**Removed/optional (code mein handle hai):** Battery monitor (GPIO 1 ab motor UART TX), gripper + spoiler servos (hardware removed, pins freed), rear ultrasonic (removed), buzzer (removed — I2S amp horn karta hai).

---

## 3. Complete Pin Map (Current Code se — `main/car.c` + `main/tft_display.c`)

### 3.1 Motors & Servos
| GPIO | Function | Notes |
|------|----------|-------|
| GPIO 1 | **UART TX → D1 motor slave** | 115200 baud, "L<pwm>,R<pwm>" commands |
| GPIO 4 | **Servo PAN** (spark cannon) | LEDC ch0 |
| GPIO 5 | **Servo TILT** (spark cannon) | LEDC ch1 |

### 3.2 Ultrasonic Sensors (HC-SR04)
| GPIO | Function |
|------|----------|
| GPIO 12 | LEFT Trig |
| GPIO 9  | LEFT Echo |
| GPIO 7  | FRONT Trig |
| GPIO 10 | FRONT Echo |
| GPIO 8  | RIGHT Trig |
| GPIO 11 | RIGHT Echo |

### 3.3 Display — 2.8" TFT (SPI2, 40 MHz)
| GPIO | Function | Notes |
|------|----------|-------|
| GPIO 21 | TFT **SCLK** | was 46 |
| GPIO 38 | TFT **MOSI** | MISO not connected |
| GPIO 45 | TFT **CS** | |
| GPIO 0  | TFT **DC** | |
| GPIO 2  | TFT **RST** | was 16 |
| — (3.3V direct) | TFT **BL** | `TFT_PIN_BL = -1` — backlight hamesha ON, **kabhi GPIO pe mat lagao** |

### 3.4 OLED + IMU (Shared I2C Bus)
| GPIO | Function |
|------|----------|
| GPIO 42 | I2C **SDA** (OLED 0x3C + MPU-6500 0x68/0x69) |
| GPIO 41 | I2C **SCL** |

### 3.5 Audio (I2S — MAX98357A)
| GPIO | Function |
|------|----------|
| GPIO 39 | I2S **BCLK** |
| GPIO 47 | I2S **LRC/WS** |
| GPIO 40 | I2S **DIN** |

### 3.6 Lights & Relays (sab active-LOW relays)
| GPIO | Function | Notes |
|------|----------|-------|
| GPIO 46 | **Headlight relay** | |
| GPIO 14 | **Backlight relay** (car body backlight) | |
| GPIO 3  | **Rear light relay** (spoiler ke neeche) | default ON, D-pad Down toggle (purana roof relay yahan se HAT GAYA) |
| GPIO 16 | **Mist maker relay** | |
| GPIO 6  | **WS2812 rear strip** (2 px → 6 LEDs) + **12-LED under-car strip** (daisy-chain) | single wire |
| GPIO 17 | **Front Left WS2812** | |
| GPIO 15 | **Front Right WS2812** | |
| GPIO 18 | **Roof Police Light WS2812** (1 pixel) | RMT TX ch4, non-DMA (rear strip DMA owns), 10 MHz |

### 3.7 System / Reserved / DO-NOT-USE Pins
| GPIO | Status | Reason |
|------|--------|--------|
| GPIO 19, 20 | **USB-OTG** | Xbox dongle yahan lagta hai |
| GPIO 43, 44 | **UART0 console** | Serial monitor / flash |
| GPIO 26–32 | Flash | Internal SPI flash |
| GPIO 33, 35, 36, 37 | **Octal PSRAM** (N16R8) | ⚠ Inpe kabhi kuch mat lagao |
| GPIO 48 | Board RGB LED | Code se **force OFF** kiya gaya hai |
| GPIO 13 | Free/freed | Rear US removed |
| GPIO 44-area | Free/freed | BL freed, etc. |

---

## 4. Button Map — Kaunsa Button Kya Karta Hai, Kab

### 4.1 Priority System (hamesha is order mein)
1. **GUIDE tap = EMERGENCY STOP (E-STOP)** — sab kuch turant band: motors 0, lights off, roof off, servos center, horn beep, full rumble. Sticky alert — clear hone tak lock.
2. **Controller disconnect = fail-safe** — motors stop, turbo/mist/horn off.
3. **Context routing** — `os_context()` decide karta hai buttons kiske paas hain: `BOOT` / `DRIVE` / `OS (menu)` / `GAME` / `ESTOP`.
4. AUTO mode mein sirf **X** (exit) aur LT/stick (manual takeover) kaam karte hain, baaki sab blocked.

### 4.2 DRIVE Mode (Manual) — Car Controls
| Button | Kaam | Kab |
|--------|------|-----|
| **Left Stick (LY/LX)** | Throttle + Steering (differential drive) | Hamesha drive mein |
| **RT (Right Trigger)** | Throttle / speed limiter | Hamesha |
| **LT (Left Trigger)** | **Brake** (LT 200+ = real motor brake + brake light + spoiler air brake) | Hamesha |
| **A — tap** | Headlight ON/OFF (+ click sound) | Drive |
| **A — hold 700 ms** | **Mist maker ON/OFF** toggle | Drive |
| **B — tap** | **Roof police light** ON/OFF (+ toast) | Drive |
| **B — hold 700 ms** | Roof light **panel** open (mode/brightness select) | Drive |
| **X — tap** | Preview overlay ON/OFF | Drive |
| **X — hold 1 s** | **AUTO mode enter** (agar sensors ready; warna checklist dikhega) | Drive |
| **Y — tap** | Ambient LED preset cycle (off → static → rainbow → chase → siren) | Drive |
| **Y — hold 700 ms** | Static LED **color cycle** (6 colors) | Drive |
| **LB — hold** | **Horn** (continuous, wav via I2S amp) | Drive |
| **RB — hold** | **TURBO** — max 3 s, phir 5 s cooldown (gear cap bypass, 100% speed) | Drive (AUTO mein blocked) |
| **LS click** | MANUAL ↔ CRAWL mode toggle | Drive |
| **RS click** | Spark cannon **recenter** | Drive |
| **D-Up** | **Hazard lights** ON/OFF | Drive |
| **D-Left** | Left indicator (right auto-cancel) | Drive |
| **D-Right** | Right indicator (left auto-cancel) | Drive |
| **D-Down — tap** | Gripper sound action + **rear light toggle** (legacy gripper button) | Drive |
| **D-Down — hold** | Gripper open (legacy) | Drive |
| **START — tap** | **Home launcher** (OS menu) | Drive |
| **START — hold** | **Quick Settings** overlay | Drive |
| **BACK — tap** | HUD **layout cycle** (full/compact/night) | Drive |
| **BACK — hold** | **Night mode** HUD | Drive |
| **GUIDE — tap** | 🚨 **E-STOP** (global, kabhi bhi) | Har jagah |
| **GUIDE — hold** | Kuch nahi (koi reboot action nahi — safety rule) | — |

### 4.3 Menus / OS Screens
| Button | Kaam |
|--------|------|
| D-pad / Left stick | Navigate focus (repeat-on-hold supported) |
| **A** | Confirm / Open |
| **B** | Back / Cancel (Games se bhi exit) |
| **START** | Pause / confirm |
| **BACK** | Exit to Home / back |
| **GUIDE** | Emergency stop (har screen par) |

### 4.4 Games
| Button | Kaam |
|--------|------|
| Left stick + buttons | Game actions (game-dependent) |
| **START** | Pause (release par auto-resume) |
| **BACK** | Games Hub par exit |
| **B** | Exit/pause (framework-owned) |
| **GUIDE** | Emergency stop |

### 4.5 Input Event Engine (`input_events.c`)
Har button ke liye semantic events: **PRESS / RELEASE / TAP (<350 ms) / HOLD (700/1000/1500 ms) / REPEAT (350 ms delay, 120 ms interval) / DOUBLE-TAP / CHORD**. Tap aur hold mutually exclusive hain, aur events **consumable** hain (ek press par do systems kabhi react nahi karenge). Diagnostics mein **Restart / Factory Reset = A hold 1.5 s** (dangerous-confirm timing).

---

## 5. Drive Modes

| Mode | Kaise | Kya hota hai |
|------|-------|--------------|
| **MANUAL** | Default | Tum chalate ho — stick/trigger control |
| **CRAWL** | LS click | Slow precise mode (LS click se toggle) |
| **AUTO** | X hold 1 s | Car khud chalti hai — distance se speed, obstacle avoid, turning, stuck-escape (reverse + torque boost), 360° search, dynamic obstacle pause. Exit: X tap ya LT/stick takeover |

**AUTO state machine:** `CRUISE → SLOW → TURN_L/R → REVERSE → ESCAPE → STUCK → PAUSE → SEARCH`

### Gears (G1–G5)
- Virtual gear system — speed cap per gear (default): **G1=5%, G2=15%, G3=25%, G4=35%, G5=50%**
- Settings se change + NVS mein save hote hain
- **RB (turbo)** gear cap bypass karke 100% deta hai
- Active cap = `min(speed_cap_setting, gear_caps[gear])`

---

## 6. Car OS — Screens aur Features

**Flow:** `BOOT (animation) → HOME launcher → 6 screens + Games`

| Screen | Features |
|--------|----------|
| **BOOT** | "AZAM CAR OS" logo animation, boot checklist (✓ CONTROL ✓ SENSORS ✓ DISPLAY ✓ PAD), TFT boot animation module |
| **DRIVE (HUD)** | Speed, L/F/R distances (color-coded: cyan clear / amber near / red danger), gear pills G1–G5, status icons (headlight, hazard, mist, spark, auto, IMU), alerts overlay (E-STOP, CONTROLLER LOST, OBSTACLE, SENSOR FAULT), 4 sub-views (Radar / Sensor / Performance / HUD) |
| **ANALYTICS** | Ring-buffer graphs (480 samples, PSRAM mein) — speed / front distance / yaw history |
| **OLED CTRL** | OLED layout select: 0=mini HUD, 1=radar, 2=STATUS, 3=text — OLED par mirror |
| **SETTINGS** | 14 items: speed cap, engine volume, obstacle cm, LED brightness, mist max run, TFT brightness, 5 gear caps, OLED layout, HUD layout — **A = apply item, START = NVS save, B = discard** |
| **GAMES HUB** | 2 games (niche dekho) |
| **DIAG** | 5 tabs: SYS / SENS / CTRL / DISPLAY / THEME. SYSTEM mein **Restart** aur **Factory Reset** (A hold 1.5 s confirm) |
| **ALEXA LOG** | Voice command history screen |

### Games (TFT par, car parked — motors kabhi touch nahi hote)
1. **NEON CONVOY** (`cargame_neon_convoy.c`, ~61 KB source) — Top-down cyberpunk convoy-defense shooter, splash/menu/game states, high-score.
2. **NEON SERPENT** (`cargame_neon_serpent.c`, ~25 KB source) — Cyberpunk grid-snake (18×9 grid @ 16 px) with enemies, EMP/dash, mines aur **boss fight**. Rendering `ns_theme` (draw-only theme layer) se hoti hai.

---

## 7. Features Summary (Saare)

- ✅ **Xbox 360 USB Host driver** — wireless receiver + wired-clone support, 4 pads tak, auto-pair (HOME), rumble/haptic feedback, alive-detection (last data timestamp)
- ✅ **Differential drive** — UART motor slave, ramped acceleration (cur → tgt), real motor brake
- ✅ **3× ultrasonic** — moving-average (3 samples per sensor), parking beeps, obstacle speed limit, emergency stop, AUTO avoidance
- ✅ **MPU-6500 IMU** — pitch/roll/yaw-rate, auto gyro-bias calibration at boot + manual CAL (radar par A button), Motion Radar UI
- ✅ **Lighting system** — headlight relay, body backlight relay, rear light relay, roof police light (7 modes — WS2812 RGB, niche detail), WS2812 rear + under-car strip (modes: off/static/rainbow/chase/siren), front indicator LEDs, hazard + indicators, brake light
- ✅ **Roof Light System (UPDATED — GPIO 18 WS2812)** — purana GPIO3 relay remove ho gaya, ab RGB strip par real patterns:
  - **7 modes:** `OFF` / `R/B` (alternate red-blue) / `DOUBLE` (2× red flash + 2× blue flash, 800 ms cycle) / `FAST` (rapid alternation) / `RED` (red pulse) / `BLUE` (blue pulse) / `STEADY` (magenta = red+blue dono ek saath — single IC hai isliye left/right split nahi ho sakta)
  - **Brightness 0–100%** (default 70%) — pattern colors brightness-se scaled hote hain
  - **Default:** mode = DOUBLE FLASH, boot par hamesha **OFF** (kabhi ON restore nahi hota)
  - **Blink period:** 400 ms default (adjustable via `period_ms`)
  - **Safety rules:** sirf **DRIVE context** mein hi jalti hai — estop, controller disconnect, ya menu/game mein strip automatically **dark** ho jati hai (`roof_light_force_safe_off()` estop par)
  - **NVS persistence:** pattern + brightness save hote hain (`car_roof_apply_saved()` boot par load karta hai, par OFF hi rehti hai)
  - **Roof Panel UI (B hold 700 ms):** overlay with 2 rows — `PATTERN` (7 modes cycle) aur `BRIGHTNESS` (progress bar) — **A = apply + close + NVS save**, **B = cancel (purane settings restore)**, **START = close (current rakho)**
  - **Efficient rendering:** color unchanged ho to RMT refresh skip hota hai (no RMT spam on static modes)
  - HUD par label toast: "ROOF LIGHTS ON · DOUBLE" type
- ✅ **Audio (I2S)** — speed-linked engine loop, horn wav, UI clicks/score/error blips, tone sweeps (turbo whoosh/blowoff), reverse beeps, TTS PCM playback API
- ✅ **TFT Car OS UI** — 320×240 framebuffer (single DMA push + dirty-rect pushes), byte-swap RGB565 fix, skip-unchanged-frames, perf counters (frame µs / push µs / FPS)
- ✅ **OLED HUD** — 128×64 secondary display, 4 layouts
- ✅ **WiFi + SNTP** — station mode, NVS credentials (`net` namespace, defaults embedded)
- ✅ **Alexa / AWS IoT bridge** — MQTT over TLS (port 8883), mutual TLS certs (NVS blobs ya built-in fallback), Device Shadow, command TTL + safety gateway, voice history screen. Broker khali = bridge sleeping (zero traffic)
- ✅ **Settings + NVS persistence** — 14 settings, gear caps, roof pattern/brightness, factory reset
- ✅ **Safety** — E-STOP (GUIDE), disconnect fail-safe, neutral-release lock (250 ms) menu/game/estop se wapas aane par, alert priority system, roof safe-off on estop, no-reboot-on-any-hold rule
- ✅ **SPIFFS sounds** — `horn_sound.raw`, `startup_sound.raw`, `intro_test_sound.raw` + 8 embedded WAVs (engine_loop, hover_loop, brake, stop, gripper, laser, yclick, a_click)
- ✅ **Boot animation** TFT par (`tft_boot_anim.c`)

---

## 8. Memory Usage — RAM aur ROM (Flash)

### 8.1 Flash (ROM) — 16 MB total (N16R8)
| Partition | Offset | Size | Used | Free |
|-----------|--------|------|------|------|
| nvs | 0x9000 | 24 KB | settings data | — |
| otadata | 0xF000 | 8 KB | — | — |
| phy_init | 0x11000 | 4 KB | — | — |
| **factory (app)** | 0x20000 | **6 MB** | **1.87 MB** (bin = 1,867,984 bytes) | **~4.2 MB (≈70% FREE)** ✅ |
| **storage (SPIFFS)** | 0x620000 | **9.875 MB** (0x9E0000) | **~628 KB** (643,144 bytes sounds) | **~9.26 MB FREE** ✅ |

- **Bootloader:** 21,056 bytes (0x5240) — apne slot mein 36% free.
- **App binary:** `xbox360_controller.bin` = 1,867,984 bytes ≈ **1.78 MB** — 6 MB factory partition ka sirf ~30% use hua, **~70% khali hai**. OTA slots hata diye the taaki audio/codec space mile (single factory 6 MB layout).
- **8 embedded WAVs** app binary ke andar compile hote hain; badi raw sounds SPIFFS partition par rehti hain.

### 8.2 RAM
| Region | Total | Runtime use |
|--------|-------|-------------|
| **Internal SRAM** | 512 KB (ESP32-S3) | Linker data region `dram0_0_seg` ≈ 339 KB; **runtime free internal heap ≈ 128 KB (min 128 KB)** — serial PERF log se measured |
| **PSRAM (octal)** | **8 MB** | **~8,018 KB free** — sirf explicit `heap_caps_malloc(MALLOC_CAP_SPIRAM)` se use hota hai (analytics buffers 480×3; USB DMA kabhi PSRAM touch nahi karta) |
| **IRAM** | ≈ 359 KB region | ISR / fast code |

**RAM policy (sdkconfig.defaults se):**
- `malloc()` internal SRAM mein rehta hai (`SPIRAM_MALLOC_ALWAYSINTERNAL=16384`)
- Task stacks **hamesha internal** (PSRAM stack par ISR = S3 par crash)
- PSRAM sirf caps-alloc se (`SPIRAM_USE_CAPS_ALLOC`) — USB-OTG DMA PSRAM tak nahi pahunch sakta, isliye stack corruption avoid hota hai
- Flash: 16 MB, DIO mode, **80 MHz**

### 8.3 Big Static Data (app binary ke andar)
| File | Size |
|------|------|
| `startup_sound.h` | 717 KB |
| `intro_test_sound.h` | 467 KB |
| `horn_sound.h` | 112 KB |
| TFT framebuffer | 153,600 bytes (240×320×2, DMA internal RAM) |
| TFT row scratch | 32 KB DMA |

---

## 9. Software Architecture / File Map

```
main/
├── main.c                  - app_main: 4 tasks (usb_host, xbox360, car, alexa)
├── xbox360.c/h             - Xbox 360 USB Host driver (XBOXRECV protocol port, GPL-2.0)
├── car.c                   - CONTROL LOOP: motors, servos, lights, sensors, audio, OLED (116 KB!)
├── car_global.h            - shared car state `g` + button masks B_* + modes
├── car_input_handle_a/b.c  - DRIVE mode button handling (A/B/X/Y/LB/RB/D-pad)
├── input_events.c/h        - semantic event engine (tap/hold/repeat/double-tap)
├── car_os.c/h              - Car OS state machine + navigation + games framework
├── os_screens_tft.c        - TFT screen renderers (Home/Drive/Settings/Diag...)
├── os_gfx.c, os_widgets.c, os_assets.c, ns_theme.c - UI drawing + theme
├── os_oled.c               - OLED HUD renderer
├── tft_display.c/h         - ILI9341/ST7789 SPI driver + framebuffer + DMA
├── tft_boot_anim.c         - boot animation
├── imu_driver.c/h          - MPU-6500 driver + gyro calibration
├── ui_motion_radar.c       - Motion Radar UI (IMU visual)
├── ui_dashboard.c          - dashboard helpers
├── roof_light.c/h          - roof police light module (GPIO 18 WS2812 ka single owner)
├── cargame_neon_convoy.c/h - Game 1: Neon Convoy (shooter)
├── cargame_neon_serpent.c/h- Game 2: Neon Serpent (snake + boss)
├── car_games.c/h           - games registry
├── net_wifi.c/h            - WiFi station + SNTP
├── alexa_bridge.c/h        - MQTT/TLS AWS IoT bridge + safety gateway (51 KB)
├── alexa_config.h          - broker/thing/certs defaults
└── learning_adv.h          - audio/learning helpers (car.c include karta hai)

🗑 DELETED (17 Sep 2026) — dead/legacy files jo build mein NAHI thi (CMakeLists
  mein nahi, koi include nahi karta). Delete ho chuki hain:
  car_input_handle_a/b.c, car_input_helpers_new.c (asli drive button logic
  car.c ke andar inline hai, ~line 1500+), main_backup_car.c,
  display_driver.c/h, ui_dashboard.c/h, ui_dashboard_premium.h,
  tft_game.c/h, sensors_adv.h

spiffs/                     - SPIFFS partition content (raw sounds)
partitions.csv              - custom 16 MB partition table
sdkconfig.defaults          - PSRAM octal, USB host, flash 80M DIO config
CAR_OS_UI_GUIDE_UPDATED.md  - full UI redesign spec (48 KB)
PROJECT_ANALYSIS_AND_BUGS.md- known issues analysis
SESSION_SUMMARY.md          - development session notes
```

**Libraries (ESP-IDF components):** `espressif__usb` (USB Host stack), `led_strip` (WS2812/RMT), plus `esp_lcd`, `esp_wifi`, `esp_netif`, `mqtt`, `cjson`, `mbedtls`, `nvs_flash`, `spiffs`, `esp_ringbuf`, aur sab `esp_driver_*` (LEDC, I2C, I2S, SPI, GPIO, UART).

---

## 10. Build & Flash

```powershell
# ESP-IDF 6.0.2 PowerShell open karo (Start menu: "ESP-IDF 6.0.2 CMD/PowerShell")
cd C:\Users\aazam\Desktop\last\16-9\xbox360_controller\xbox360_controller
idf.py build
idf.py -p COM7 flash monitor      # COM7 = UART bridge port (baud 115200)

# Full manual flash (zaroorat pade to):
python -m esptool --chip esp32s3 -b 460800 write-flash `
  0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin `
  0xf000 build\ota_data_initial.bin 0x20000 build\xbox360_controller.bin `
  0x620000 build\storage.bin
```

- **Board par:** Dongle **USB-OTG port** mein lagta hai (adapter ke saath), Serial monitor **UART/COM port** se.
- **Controller:** HOME dabao = auto-pair; HOME 5 s hold = XInput/DInput toggle.
- ⚠ **Security note:** WiFi credentials (`net_wifi.h`) aur AWS IoT device cert + private key (`alexa_config.h`) abhi source mein embedded hain — repo public karne se pehle rotate/untrack karna.

---

## 11. Performance (Measured — serial PERF logs se)

| Metric | Value |
|--------|-------|
| TFT FPS | **9 FPS** (frame render 3.5–4.9 ms, max 4.9 ms) |
| SPI push | ~30.8 ms per full frame (40 MHz SPI, 153 KB) |
| Skipped frames | 0 (unchanged-frame skip active) |
| Free internal heap | ~128 KB |
| Free PSRAM | ~8,018 KB / 8 MB |
| Control loop | 40 ms tick (`LOOP_MS`) |
| Stick deadzone | ±1500 (typical stick range ±16000) |

---

*Last verified: 17 Sep 2026 — source (`main/*.c/h`), `partitions.csv`, `sdkconfig.defaults`, build outputs (`build/xbox360_controller.bin` = 1,867,984 bytes), linker map aur serial PERF logs se.*
