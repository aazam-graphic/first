# 🔍 CODE REVIEW + BUG REPORT — 17 Sep 2026

**Project:** `xbox360_controller` (AZAM CAR OS) — ESP32-S3 N16R8, ESP-IDF v6.0.2
**Scope:** pin conflicts, memory safety, race conditions + general code review
**Build verification:** `idf.py build` (IDF 6.0.2) — **PASS, 0 errors, 0 warnings**
**Binary:** `build/xbox360_controller.bin` = `0x1C85A0` (1,868,192 B) — factory 6 MB ka **70% free**

| | Baseline (review se pehle) | Fixes ke baad |
|---|---|---|
| Build | PASS | PASS |
| Errors | 0 | 0 |
| Warnings | **14** | **0** |
| App size | 0x1C81A0 | 0x1C85A0 (+1 KB: pin guard + fixes) |

> Ye review sirf `main/` ke **active** files par hai (jo `main/CMakeLists.txt` me hain):
> car.c, car_os.c, os_screens_tft.c, os_gfx.c, os_widgets.c, os_assets.c, ns_theme.c,
> tft_display.c, tft_boot_anim.c, input_events.c, roof_light.c, imu_driver.c,
> ui_motion_radar.c, os_oled.c, car_games.c, cargame_neon_convoy.c,
> cargame_neon_serpent.c, net_wifi.c, alexa_bridge.c, xbox360.c, main.c.

---

## 1. Pin conflicts — ❌ koi conflict NAHI (verified)

26 GPIO functions ko script se extract karke duplicate check kiya:

```
0 TFT_DC          1 UART_TX        2 TFT_RST        3 REAR_LIGHT
4 SERVO_PAN       5 SERVO_TILT     6 LED_STRIP      7 US_TRIG_F
8 US_TRIG_R       9 US_ECHO_L     10 US_ECHO_F     11 US_ECHO_R
12 US_TRIG_L     14 BACKLIGHT     15 FRONT_LED_R   16 MIST
17 FRONT_LED_L   18 ROOF_LED      21 TFT_SCLK      38 TFT_MOSI
39 I2S_BCLK      40 I2S_DIN       41 OLED_SCL      42 OLED_SDA
45 TFT_CS        46 HEADLIGHT     47 I2S_LRC
=> NO DUPLICATES
```

**Naya safety net:** `car.c` me `pin_conflict_check()` add kiya hai — boot pe har baar
sab pins verify hote hain aur conflict hone par `ESP_LOGE("PIN CONFLICT: ...")` log
aata hai, warna `pin check: 27 functions, no GPIO conflicts`.
TFT pins ab `tft_display.h` me aur roof pin `roof_light.h` me hain (single source of
truth), isliye guard cross-module conflicts bhi pakadta hai.

### ⚠ Strapping pins (hardware caution — code fix nahi)
ESP-IDF 6.0.2 GPIO docs (`docs/en/api-reference/peripherals/gpio/esp32s3.inc`) ke
mutabiq ESP32-S3 ke strapping pins: **GPIO0, GPIO3, GPIO45, GPIO46** — aur ye chaaron
is car me use ho rahe hain:

| GPIO | Project use | Strap role |
|---|---|---|
| GPIO0 | TFT **DC** | boot mode |
| GPIO3 | **Rear light relay** (active LOW) | JTAG source |
| GPIO45 | TFT **CS** | VDD_SPI voltage |
| GPIO46 | **Headlight relay** (active LOW) | boot mode |

**Kya karna hai:** in 4 lines par aisa external pull-up/pull-down **mat** lagao jo reset
ke waqt galat level de (warna board download-mode ya VDD_SPI 1.8 V me fas sakta hai).
GPIO0 (DC) par 10k pull-up aur GPIO46 (headlight relay input) par relay module ka
idle-high ensure karo. Ye pins GPIO33–37 (octal PSRAM SPIIO4–SPIDQS) se alag hain —
wo already safe hain.


---

## 2. Confirmed bugs (sab FIX kar diye)

### 🔴 BUG-1: Backlight relay (GPIO 14) completely dead
* **Evidence:** compiler warning `car.c:2410: variable 'bl' set but not used`;
  `grep PIN_BACKLIGHT` ka sirf **1** hit (define) — na `gpio_config`, na `gpio_set_level`.
* **Impact:** brake light / reverse light relay kabhi ON nahi hota tha (hw relay floating
  chhoda gaya tha); boot self-test bhi isko test nahi karta tha.
* **Fix:** `hw_init()` ke output mask + boot level (`1` = OFF) + boot click test, aur
  `drive_output()` me `gpio_set_level(PIN_BACKLIGHT, bl ? 0 : 1);`

### 🔴 BUG-2: SETTINGS → "ENGINE VOL" ka koi asar nahi
* **Evidence:** `s_set_engine_vol` sirf NVS load/save + get/set me tha, audio path me
  kahin use nahi (`audio_render()` hardcoded `evol` deta tha).
* **Impact:** UI se volume 0–100 karne par awaaz same rehti thi.
* **Fix:** `audio_render()` me `evol = evol * s_set_engine_vol / 100` (default 100 = no change).

### 🔴 BUG-3: SETTINGS → "LED BRIGHT" ka koi asar nahi
* **Evidence:** `s_set_led_bright` bhi sirf settings/NVS tak limited tha.
* **Impact:** LED brightness slider dead tha.
* **Fix:** naya `led_scale()` helper — `strip_px()`, `under_px()` aur front WS2812 writes
  sab isse scale hote hain (default 100 = pehle jaisa behaviour).

### 🟠 BUG-4: SETTINGS → "OBST DIST" manual mode me ignore ho raha tha
* **Evidence:** `manual_logic()` me hardcoded `f < 15` (hard stop) aur `f < 30` (limit).
* **Impact:** obstacle distance setting sirf Alexa bridge me use hoti thi, driving me nahi.
* **Fix:** `obst_cm = car_get_setting(2)`, hard-stop `obst/2` (min 5 cm), slow-down `obst_cm`.

### 🟠 BUG-5: AUTO mode — AST_SEARCH ka "3 s me give up" **dead code**
* **Evidence:** AST_SEARCH me `if (el > 500) { ... g.ast_t0 = now; }` aur usi case me
  `if (el > 3000) → AST_STUCK`. Kyunki `ast_t0` har 500 ms reset hota hai, `el` (elapsed)
  3000 tak pahunch hi nahi sakta → STUCK/Hazard kabhi engage nahi hota.
* **Impact:** sab directions blocked hone par car infinite rotation me ghoom sakti thi.
* **Fix:** alag timer `s_search_t0` (SEARCH entry pe set, give-up check usi se).

### 🟠 BUG-6: Alexa bridge — `strncpy()` bina guaranteed NUL termination
* **Locations:** `alexa_bridge.c` — stack copies (pehle line 1055/1056, 1080, 1093/1094,
  1188, 1383/1384, 1415) aur struct fields (`result_set()`, `banner_show()`, `s_conf.*`,
  `s_pend.*`).
* **Impact:** 23/39 char lamba value aane par buffer non-terminated → `strcmp()`/`strlen()`
  out-of-bounds read (crash / undefined behaviour).
* **Fix:** stack copies ab `snprintf()` se, struct copies ke baad explicit `[sizeof-1] = 0`.

### 🟡 BUG-7: OLED HUD speed text clip
* **Evidence:** `oled_text(110, 50, "100%")` → 110 + 4×6 = 134 px (screen 128 px) →
  text loop `x > 122` pe return kar deta hai, sirf "10" dikhta tha.
* **Fix:** x = 104.

### 🟡 BUG-8: 14 compiler warnings (dead code)
Sab remove kar diya:
* `car.c`: `adc_init()`, `batt_read()`, `wav_active()`, `strip_set_all()`,
  `strip_set_half()`, `s_menu_names[]`, `MENU_ITEM_COUNT`, unused `b0`.
* `os_screens_tft.c`: `led_name()`, `batt_pct()`, `batt_color()`, `ol_px()`.
* `xbox360.c`: `print_hex_line()`, `btn_name()`.

### 🟡 BUG-9: Stale/misleading pin comments (docs vs code)
Fix kiye: I2S `DIN=48` → **DIN=40**; under-car strip "GPIO 37" → **GPIO 6 (daisy-chain)**;
`hw_init` comment "GPIO48 = HEADLIGHT, BL=13" → **headlight 46 / backlight 14 / BL 3.3 V
hardwired**; "UART1 TX 17" → **GPIO 1**; TFT pins single-source header me.

---

## 3. Report-only issues (code jaan-boojh kar nahi badla — aap decide karo)

| # | Sev | Issue | Detail / Recommendation |
|---|---|---|---|
| R-1 | 🟠 | **xbox360 pad struct: cross-task tearing** | `in_xfer_cb()` (USB host task) `s_drv.pad[]` ke int16 fields likhti hai, `car_task` `xbox360_pad(0)` se wahi struct padhta hai — lock/snapshot nahi. 32-bit `buttons` + 4×int16 tearing possible. Fix: snapshot ya critical section. |
| R-2 | 🟠 | **MQTT inbox flags not volatile** | `s_inbox`/`s_in_w`/`s_in_r`/`s_mqtt_ok` MQTT-client task (producer) + alexa task (consumer) me share hote hain, `volatile` nahi. Aaj kaam kar raha hai, lekin compiler caching se missed wakeup possible. |
| R-3 |  | **MQTT payload fragmentation** | `MQTT_EVENT_DATA` me `e->data_len` sirf ek chunk hai; bada JSON command do events me aaye to `serve_command()` adhoora JSON parse karega. `current_data_offset`/`total_data_len` handle karo. |
| R-4 | 🟡 | **s_pend handshake lock-free** | `rules_eval()` (alexa task) aur `alexa_apply_pending()` (car task) ek hi struct share karte hain; `busy` ordering theek hai par barrier/lock nahi. `portMUX` critical section (jaise `s_conf` me) use karo. |
| R-5 |  | **AUTO me front sensor fail → STUCK detection dead** | `f < g.prog_f - 5` / `f > g.prog_f + 10` uint16 arithmetic hai; `f == 65535` (no echo) pe wraparound se `prog_t` har loop reset hota hai. `f >= 65000` ko explicit "unknown" treat karo. |
| R-6 | 🟡 | **tft_display inflight counter non-atomic** | `s_tx_inflight` plain int + binary semaphore: do completions ek drain window me aayein to ek semaphore count "khoya" ja sakta hai → worst case 100 ms drain timeout (safety net hai, corruption nahi). Counting semaphore + `volatile` behtar. |
| R-7 |  | **sdkconfig deprecated/renamed options** | Build notes: `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` → ab `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`; plus default mismatches: `SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, `ESP_TASK_WDT_TIMEOUT_S`, `MBEDTLS_DYNAMIC_BUFFER`. `idf.py menuconfig` kholke save karne se migrate ho jayega. |
| R-8 | 🟡 | **7 dead .c files disk par pade hain** | `car_input_handle_a.c`, `car_input_handle_b.c`, `car_input_helpers_new.c`, `display_driver.c`, `main_backup_car.c`, `tft_game.c`, `ui_dashboard.c` (+ headers `display_driver.h`, `ui_dashboard.h`, `ui_dashboard_premium.h`, `tft_game.h`, `sensors_adv.h`). `PROJECT_DETAILS.md` inhe "DELETED" kehta hai — **docs galat hain**. Sirf `oled_driver.h` genuinely used hai (car.c + os_oled.c). Inhe delete karo ya docs correct karo. |
| R-9 |  | **`components/voice_ai/` build me nahi** | Component maujood hai (voice_ai.c, mcp_car_tools.c, voice_audio_i2s.c, audio_mutex.c) lekin `MINIMAL_BUILD` component list me nahi (koi use nahi karta). Ya wire karo ya hata do. |
| R-10 | 🟢 | **Manual obstacle raw vs averaged distance** | Manual mode `g.dist[1]` (raw), AUTO `g.dist_avg[1]` — inconsistent (intentional ho sakta hai). |
| R-11 |  | **Git state** | Sirf 1 commit (`14d87e0 Initial commit`), 17 Sep tak ke saare changes **uncommitted**; `.git/AUTO_MERGE` leftover marker (aborted merge) pada hai — clean karke commit/push karo. |
| R-12 |  | **`utils/` style dead helpers aur backup files** | `main_backup_car.c`, `reconfig.py`, `read_serial.ps1`, `run_build.ps1` — build se bahar, rakho ya hatao (koi harm nahi). |

---

## 4. Changed files (is review me)

| File | Kya badla |
|---|---|
| `main/car.c` | BUG-1..5, 7, 8, 9 + `pin_conflict_check()` + `led_scale()` + stale comments |
| `main/alexa_bridge.c` | BUG-6 (snprintf + explicit NUL termination, 9 sites) |
| `main/os_screens_tft.c` | BUG-8 (4 dead functions removed) |
| `main/xbox360.c` | BUG-8 (2 dead functions removed) |
| `main/tft_display.h` / `tft_display.c` | TFT pin defines header me (single source of truth) |
| `main/roof_light.h` / `roof_light.c` | `ROOF_STRIP_GPIO` header me |
| `CODE_REVIEW_2026-09-17.md` | Ye report |

## 5. Verification kaise karein

```powershell
# ESP-IDF 6.0.2 PowerShell
cd C:\Users\aazam\Desktop\last\16-9\xbox360_controller\xbox360_controller
idf.py build            # -> "Project build complete", 0 warning
idf.py -p COMxx flash monitor
```

**Boot log me ye dikhna chahiye (naya guard):**
```
I (xxx) car: pin check: 27 functions, no GPIO conflicts
```
Aur backlight relay boot pe **ek click** karega (mis/headlight/rear ke saath) —
relay wiring ka live proof.

> **Note:** Pin-guard list me naya pin add karne par `car.c` ke `pin_conflict_check()`
> map me bhi entry karo, warna guard us pin ko check nahi karega.
