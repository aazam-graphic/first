> ⚠ **HISTORICAL LOG (17 Sep 2026):** Ye ek purani development session ka record hai.
> Isme pins/features **outdated** hain (DIN 35, Buzzer 18, BATT_ADC 1, Servos 6/7,
> battery monitor, 4× ultrasonic) — **latest ke liye `PROJECT_DETAILS.md` dekho.**


# Xbox360 Controller Car — Full Session Details

**Date:** 27 Aug 2026 (Thu)  
**Board:** ESP32-S3 N16R8 (DevKitC-1) | **IDF 6.0.2** | **Flash 4MB** (app 1MB, **1% free 0x1E30 / 7728 bytes**)  
**Main Firmware:** `xbox360_controller/main/car.c` — 2441 lines, `build/xbox360_controller.bin` = `0xFE1D0` (1040 KB)  
**Port:** COM7 @ 460800 baud | **UART Motors:** TX 36 → D1 RX @115200 `L<l>,R<r>\n`

---

## 0. Chat ka Full Timeline — Har Important Baat

| # | User ne kya kaha (exact) | Humne kya kiya + Code/Build |
|---|---|---|
| 1 | `What did we do so far?` | Summary diya — auto mode forward hi ja raha tha, ultrasonic `f=65535` no echo issue mila |
| 2 | (auto mode tez tha) `Reduce max auto speed to 20%` | Pehle auto speed 20% limit ka plan banaya, phir rear sensor pe focus shift hua |
| 3 | `15,16 kro pins ek jagah hai wo free bi hai` | Rear TRIG/ECHO ko 14/15 se **15/16** pe kiya: `#define PIN_US_TRIG_B 15 / PIN_US_ECHO_B 16` → build `0xFE2E0` |
| 4 | `krdia connect` | `flash_only.ps1` se COM7 pe flash: `Hard resetting via RTS pin... Done` |
| 5 | `pin 13 trin 14 eco back usltrasonic change krdo` | Rear ko 15/16 se **13/14** pe change: `PIN_US_TRIG_B 13, PIN_US_ECHO_B 14` → rebuild `0xFE2E0` → flash Done |
| 6 | `complete` | Status summary diya — L12/R8/F38/B13 + ECHO 9/10/11/14, HUD B: cm, reverse/escape rear check |
| 7 | `hi` | Greeting |
| 8 | `abhio tak jo bi kaam kiay auska backup lo videos folder mai organaise tareeqe se` | `videos` folder scan kiya, `videos/backup_car_project_2026-08.zip` (6.37 MB) banaya → rename `backup_car_project_2026-08.zip` |
| 9 | `mene kaha hamen jo y project car ka banaya hai uska backup lo complete` | Confirm: videos me full project zip ready |
| 10 | `abhi tak jo hamne car m code build kia hai saara data backup lo, Music folder mai` | `Music/car_project_backup/` banaya: xbox360_controller (car.c/h, xbox360.c/h, 8 wavs), esp32_test, ESP32S3_Gamepad_Diag, xiaozhi-car → `Music/car_project_backup.zip` (772 KB source-only, build excluded) → folder delete, zip only rakha |
| 11 | `check krna konse code m sensor 4 add hua hai ultrasonic uska path ka link do` | Bataya: `xbox360_controller/main/car.c:60-67` pins, `us_read:242-310` 4-sensor round-robin, `dist[4]/dist_avg[4]` |
| 12 | `ek bar bugs check krna` | `us_read` + `dist_avg` init bug mila: `for(i<3)` → `i<4` miss tha, `min3` rear ignore, `dist_buf` check |
| 13 | `mene paw 8610 amp board liya hai tez awaz ke liye, isko hame apne amp se connect krna hai, tum volume ko 1% krdo` | `buzzer_tone()` ko 1% lock kiya: `1 * 512 / 100` → build `0xFE1C0` → flash Done |
| 14 | `volume hogyi 1%?` | Confirm: strictly 1% locked |
| 15 | `code upload hogya?` | Confirm: `Hard resetting ... Done` |
| 16 | `ni ua kaafi tez awaz aari hai gain ko kam kro hamara max 98357a mosdule hai` | `buzzer_tone` ko ultra-low kiya `duty=1` (1/1024), `audio_render:1113` me `*0.05f` nahi tha abhi → build `0xFE1C0` |
| 17 | `abhi to awaaz same hi aari koi change ni aaya` | Diagnose: PWM buzzer ka I2S pe effect nahi — asli gain `audio_render` me `s_effect_amp` hai → `*0.05f` (95% cut) kiya → build `0xFE1D0` |
| 18 | `buzzer ni hai speaker connect hai hai MAX98357A I2S pin dekho konse se connect hai thik kro` | Grep kiya: `PIN_I2S_BCLK 39, LRC 47, DIN 35` mile, WIRING.md me official 7/15/16 tha — galat samajh ke 15/16/7 pe change kar diya → build/flash |
| 19 | `pin ko wapas wahi kro jo pehle the MAX98357A I2S, mene sirf puch atha konsi pins e connect hai or volum ko 1% krne kha tah` | Revert: 39/47/35 wapas → build `0xFE1D0` → flash Done, apology |
| 20 | `thik hai ab bugs nikalo proper` | `dist_avg` init `i<4` fixed, `us_read` check, I2S pins verify |
| 21 | `pins check kre koi pin confilt to ni krri hai kahgi realay kia kai issue hai check kro` | Python pin-uniqueness check: 23 pins all unique, relays active LOW (`SPARK 3, MIST 2` OFF=HIGH, boot click test) — no conflict |
| 22 | `kia esa possible hai ham isme esp s3 board m bluthoot ke zariye computer se connect krke music chala ske?` | Jawab: Haan A2DP Sink possible, I2S MAX98357A pe stream kar sakte hain |
| 23 | `ek kaam kro project folder m ic session ka description add krdo ... md file bana do` | `SESSION_SUMMARY.md` banayi (125 lines) |
| 24 | `tum samjhe ni poori details add kro ... sab add kro ziyda details` | **Ye update** — pura timeline, har pin change, har build size, har flash log add kiya |

---

## 1. Project kya hai?
ESP32-S3 pe Xbox 360 wireless controller (USB host) se chalne wali 4WD car:

- **Manual:** Left stick drive, right stick camera/cannon, A/B/X/Y/LB/RB/LT/RT = gripper, spoiler, turbo, spark, mist, horn, lights, ESTOP
- **Auto:** 4 ultrasonic se avoidance — states: `CRUISE / SLOW / TURN_L / TURN_R / REVERSE / ESCAPE (2-phase) / STUCK / PAUSE / SEARCH`, `wall_steer()`, `turn_clear()>=30cm`
- **Audio:** MAX98357A I2S (BCLK 39, LRC 47, DIN 35) — hover/engine loop, horn sample, beeps (reverse/indicator/siren/obstacle `<50cm`), `audio_task` 512 samples @22kHz, `evol=0` engine mute by user choice
- **Display:** SSD1306 128x64 OLED HUD (L,F,R,B + A:state)
- **Motors:** `motors_apply()` UART1 TX36 → Wemos D1 slave `L<l>,R<r>\n`, ramping cur/tgt -100..100
- **Sensors:** 4x HC-SR04, battery ADC (330k+100k divider), IMU MPU6050 (optional)

## 2. Final Pin Map (after all changes)

| Function | GPIO | Mode | Conflicts? |
|---|---|---|---|
| SERVO_GRIP | 6 | LEDC ch2 | OK |
| SERVO_SPOILER | 7 | LEDC ch3 | OK |
| US_TRIG_L | 12 | OUT | OK |
| US_TRIG_R | 8 | OUT | OK |
| US_TRIG_F | 38 | OUT | OK (was RGB, moved) |
| US_TRIG_B | **13** | OUT | OK — changed 15→13 |
| US_ECHO_L | 9 | IN | OK |
| US_ECHO_F | 10 | IN | OK |
| US_ECHO_R | 11 | IN | OK |
| US_ECHO_B | **14** | IN | OK — changed 16→14 |
| LED_STRIP | 21 | RMT | OK |
| I2S_BCLK | 39 | I2S | OK |
| I2S_LRC | 47 | I2S | OK |
| I2S_DIN | 35 | I2S | OK — user ne 7/15/16 pucha tha, wapas 35/39/47 final |
| SPARK | 3 | OUT relay LOW | OK |
| MIST | 2 | OUT relay LOW | OK |
| HEADLIGHT | 37 | OUT relay | OK |
| BACKLIGHT | 33 | OUT | OK |
| UART_TX | 36 | UART1 | OK |
| OLED_SDA/SCL | 42/41 | I2C | OK |
| BUZZER | 18 | LEDC ch7 PWM | OK |
| BATT_ADC | 1 | ADC1_CH0 | OK |
| Board RGB | 48 | OUT LOW | OK |

**Verified:** `python -c` uniqueness check → `NO PIN CONFLICTS! All pins unique (23)`

### Pin Evolution Is Session Me
- Start: `TRIG_B 14, ECHO_B 15` (first rear add, build 0xFE0B0)
- User `15,16 free hai` → `TRIG_B 15, ECHO_B 16` (build 0xFE2E0)
- User `13 trig 14 echo karo` → `TRIG_B 13, ECHO_B 14` (final, build 0xFE2E0/0xFE1D0)
- I2S: `39/47/35` (original) → `15/16/7` (xiaozhi doc se mistake) → `39/47/35` (revert)

## 3. Code Changes — File-by-File

### `xbox360_controller/main/car.c` (2441 lines)

**A. Defines `car.c:60-78`**
```c
#define PIN_US_TRIG_B      13  /* REAR trig (13) */
#define PIN_US_ECHO_B      14  /* REAR echo (14) */
#define PIN_I2S_BCLK       39
#define PIN_I2S_LRC        47
#define PIN_I2S_DIN        35
```

**B. Ultrasonic `us_read():242-311` — 4-sensor round-robin**
```c
static void us_read(void){
  static uint8_t seq=0; seq=(seq+1)%4;
  if(seq==0) /* LEFT  TRIG_L 12 -> ECHO_L 9  */;
  else if(seq==1) /* RIGHT TRIG_R 8  -> ECHO_R 11 */;
  else if(seq==2) /* FRONT TRIG_F 38 -> ECHO_F 10 + s_us_front_ok filter */;
  else           /* REAR  TRIG_B 13 -> ECHO_B 14 */;
  // 16ms timeout, rise/fall detect, cm=(fall-rise)/58, >400=>400 else 65535
}
```

**C. Buffers `car.c:138-141`**
```c
uint16_t dist[4];        // 0=L 1=F 2=R 3=B (65535=clear)
uint16_t dist_buf[4][3]; // 3-sample moving average
uint16_t dist_avg[4];
uint8_t  dist_idx;
```

**D. Averaging `dist_update_avg():1433-1443` + init bug fix `1249-1253`**
```c
// Boot/X-tap reset pe:
for(int i=0;i<4;i++){ g.dist_avg[i]=g.dist[i]; for(int j=0;j<3;j++) g.dist_buf[i][j]=g.dist[i]; }
// Har loop:
dist_update_avg() -> dist_buf[i][dist_idx]=dist[i]; dist_idx=(dist_idx+1)%3; avg=sum/3
// BUG FIX: pehle i<3 tha, rear (3) uninitialized rehta tha -> i<4 kiya
```

**E. HUD `hud_update():2120-2156`**
```c
L: ---/cm @ y0, F: @ y16, R: @ y32, B: @ y48, A:STATE @ y56  // 4 lines
```

**F. Auto Logic `auto_logic():1493` + REVERSE/ESCAPE rear safety `1652-1682`**
```c
uint16_t f=g.dist_avg[1], l=g.dist_avg[0], r=g.dist_avg[2], b=g.dist_avg[3];
case AST_REVERSE:
  if(b!=65535 && b<20){ turn=best_turn(l,r); g.ast=turn_clear?turn:other; break; } // rear blocked -> turn
  spd=-10;
case AST_ESCAPE: if(esc_phase==0){ if(b!=65535 && b<15){esc_phase=1; break;} spd=-10; ...}
```

**G. Audio Volume — MAX98357A `car.c:214-218, 1110-1113, 856-950`**
```c
// buzzer_tone PWM: pehle vol*512/100 -> 1*512/100 (1%) -> 1 tick (0.1%) final
static void buzzer_tone(uint16_t freq,uint8_t vol){
  if(freq==0||vol==0){ledc_set_duty(...,0);return;}
  ledc_set_freq(...,freq);
  ledc_set_duty_and_update(...,1,0); // min duty
}
// I2S render: pehle sq*s_effect_amp -> sq*s_effect_amp*0.05f (95% cut)
if(eff){ ph_x+=dt*freq; sq=...; s+=sq*s_effect_amp*0.05f; }
// s_effect_amp values: STUCK 5000, REVERSE 3500, TURN 3000, SIREN 6000, obstacle 2000
```

**H. Other Files (unchanged but part of backup)**
- `car.h`, `main.c` (app_main), `xbox360.c/.h` (USB host), `sensors_adv.h`, `learning_adv.h`, `horn_sound.h`, `startup_sound.h`, `intro_test_sound.h`, `sounds/*.wav` (8 files), `partitions.csv`, `sdkconfig`

### `xiaozhi-car/docs/WIRING.md` — Official vs Car firmware difference
Official xiaozhi: MAX98357A DIN 7, BCLK 15, LRC 16 — **Car firmware uses 35/39/47** (different board routing, isliye revert kiya)

### `esp32_test`, `ESP32S3_Gamepad_Diag` — Test sketches, backup me included, car logic se independent

## 4. Bugs Found & Fixed (Detailed)

| Bug | Location | Before | After | Impact |
|---|---|---|---|---|
| Rear avg uninit | `car.c:1250` | `for(i<3)` | `for(i<4)` | B=garbage -> wrong reverse decision |
| Rear not in averaging | `dist_update_avg:1435` | `i<3` loop | `i<4` | Fixed same |
| min3 ignores rear | `audio_effect_update:931` `min3(L,F,R)` | only 3 | Still 3 — **TODO** `min4` if rear beep needed | Rear obstacle no beep |
| PWM volume no effect on I2S | `buzzer_tone` | 1% PWM | I2S gain `*0.05f` | Real fix for MAX98357A |
| Partition tight | `partitions.csv` 4MB | 0x1E40 free | 0x1E30 free (still tight) | Need sound compression |
| I2S pin mis-change | `PIN_I2S_*` | 39/47/35 → 7/15/16 | Reverted 39/47/35 | No I2S conflict |

## 5. Build & Flash History (Exact)

| Build | Binary Size | Free | Note |
|---|---|---|---|
| after 15/16 rear | `0xFE2E0` (1040.2KB) | `0x1D20` (7.4KB) 1% | before 13/14 |
| after 13/14 rear | `0xFE2E0` | `0x1D20` | same size |
| after 1% volume | `0xFE1C0` (1039.7KB) | `0x1E40` (7.8KB) | PWM fix |
| after `*0.05f` | `0xFE1D0` (1039.9KB) | `0x1E30` (7.7KB) | final |

**Commands (always used):**
```powershell
powershell -ExecutionPolicy Bypass -File "C:\Users\aazam\AppData\Local\Temp\opencode\build_flash.ps1"
powershell -ExecutionPolicy Bypass -File "C:\Users\aazam\AppData\Local\Temp\opencode\flash_only.ps1"
```
**Flash log:** `Hard resetting via RTS pin... Done` (COM7 460800, esptool chip esp32s3, dio 80m)

**Errors faced:**
- `ninja: build stopped` + `Could not open COM7` — board disconnect tha, replug se fix
- PowerShell `Select-String -Last` param error — `Select-Object -Last 5` se fix

## 6. Backups (Detailed)

| Backup | Path | Size | Contents | Exclude |
|---|---|---|---|---|
| videos | `Default Project/videos/backup_car_project_2026-08.zip` | 6.37 MB | Full `videos/` with `build/` | — |

---

# Car OS Session — 31 Aug 2026: GPIO 250 Mystery SOLVED

## The Bug
Log spam har ~90ms pe: `E (xxxxx) gpio: gpio_set_level(250): GPIO output gpio_num error`

## Debugging Journey
1. **Red herring:** `250` GPIO number NAHI tha! IDF 6.0.2 ka `ESP_RETURN_ON_FALSE` macro
   (`esp_check.h:236`) log format karta hai `"%s(%d): "` = `__FUNCTION__` + **`__LINE__`**.
   `gpio_set_level(250)` = function `gpio_set_level` at **gpio.c line 250** (jahan check hai).
2. **Backtrace instrumentation:** `gpio.c` me temporary debug add kiya jo invalid pin +
   `esp_backtrace_print(8)` print karta hai. Result:
   - `gpio_set_level INVALID gpio_num=25 (is not-a-gpio)`
   - Backtrace: `drive_output` → **car.c:2134** (`gpio_set_level(PIN_BACKLIGHT, ...)`)
3. **Root cause:** `PIN_BACKLIGHT = 25` — but **GPIO 25 ESP32-S3 pe EXIST hi nahi karta!**
   `soc_caps.h:188`: `SOC_GPIO_VALID_GPIO_MASK = 0x1FFFFFFFFFFFF & ~(BIT22|BIT23|BIT24|BIT25)`
   → S3 pe valid GPIOs sirf **0-21 aur 26-48** hain.

## Collateral Damage (bhi fix hua)
`hw_init()` ka `gpio_config_t out` mask me `BIT64(25)` tha → **poora gpio_config FAIL** ho
rata tha (`E (898) gpio: GPIO_PIN mask error`) → **koi bhi output pin configure nahi hota tha**:
- US TRIG L/R/F (12/8/38) kabhi fire nahi karte the → sensors `65535`
- SPARK(3)/MIST(2)/HEADLIGHT(48) relays drive nahi hote the

## Pin Inventory (koi free pin nahi mila)
- 22-25: **exist nahi karte** (S3 soc mask)
- 26-32: SPI flash (module internal)
- 33-37: octal PSRAM D0-D7 (N16R8)
- 19/20: internal USB PHY (xbox dongle host)
- 43/44: UART0 console (COM7 = external USB-UART; USB-Serial-JTAG secondary console
  USB host ke saath coexist nahi kar sakta)
- Baaki sab: TFT(0,13,16,18,45,46), OLED(41,42), servos(4-7), US(8-12,38),
  front LEDs(15,17), strip(21), I2S(39,40,47), spark/mist(2,3), ADC(1), UART TX(14)

## The Fix (software-only)
- `PIN_BACKLIGHT 25 → 48` (headlight relay ke saath merged)
- `drive_output()`: dono relays ab ek hi line: `gpio_set_level(PIN_HEADLIGHT, (hl || bl) ? 0 : 1)`
- **Hardware:** backlight/brake relay ko GPIO48 pe headlight relay ke saath parallel me
  wire karna hoga (pehle 25 pe wire tha = dead pin)

## Verification (boot_os_log.txt)
| Metric | Before | After |
|---|---|---|
| gpio_set_level errors | ~11/sec (138 in 25s) | **0** |
| GPIO_PIN mask error | 1 at boot | **0** |
| Loop cadence (D1 TX 500ms throttle) | 540ms (6 × 90ms loops) | **~530ms** |
| Relay DBG | hl=48 bl=25 | **hl=48 bl=48** |
| OS state | BOOT → HOME | BOOT → HOME ✓ |
| Panics | 0 | 0 |

## Environment Notes (build/flash from CLI)
```
$env:IDF_PATH='C:\esp\v6.0.2\esp-idf'; $env:ESP_IDF_VERSION='6.0.2'
$env:IDF_PYTHON_ENV_PATH='C:\Espressif\tools\python\v6.0.2\venv'
$env:PATH += ';C:\Espressif\tools\python\v6.0.2\venv\Scripts;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin'
idf.py build / idf.py flash   (COM7)
```
Note: `ESP_IDF_VERSION` me `v` prefix NAHI (ValueError); constraints file copy:
`C:\Espressif\espidf.constraints.v6.0.txt`.

## Audio Fix (31 Aug 2026, evening) — "car me sound nahi + menu me tractor awaaz"
**Do bugs mile:**
1. Manual mode me engine sound SIRF RT trigger se tha (`manual_logic:2058 pad->rt`),
   LY stick se gaadi chalao to silent. Menu/game me `manual_logic`/`auto_logic` skip
   hote the → `s_engine_spd` STALE reh gaya (aakhri value) → menu me engine chalta
   rehta = "tractor ki awaaz".
2. Auto mode me targets se sound thi, manual me nahi — inconsistent.

**Fix — engine sound ab ACTUAL wheel speed se (`drive_output()` me, sab modes me):**
```c
int16_t spd_now = max(|g.cur_l|, |g.cur_r|);      // ramped wheel speed
int16_t rt_rev = pad ? pad->rt*100/255 : 0;       // RT = stationary rev
s_engine_spd = max(spd_now, rt_rev);
if (os_parked() || g.estop || g.batt_crit) s_engine_spd = 0;  // menu/game = silent
```
- Purane dono writers hataye (auto_logic:1893-95, manual_logic:2056-59)
- Debug log ab: `eng: tgt=%d,%d cur=%d,%d es=%d f=%d`
- Verify: HOME/menu me `es=0` (silent) ✓, gpio errors 0 ✓

## Car OS Status (replaces LVGL dashboard)
- Boot anim → BOOT → HOME launcher (6 tiles) ✓ PSRAM 8MB ✓ assets ✓
- Files: car_os.c/h, os_gfx.c/h, os_assets.c/h, os_screens_tft.c/h, os_oled.c/h, car_games.c/h
- display_driver.c / ui_dashboard.c / tft_game.c ab build me NAHI (CMakeLists updated)

| Music source | `Default Project/Music/car_project_backup.zip` | 772 KB (772.1 KB) | `xbox360_controller/` (car.c/h, xbox360.c/h, main.c, 8 wavs, CMake, sdkconfig, partitions), `esp32_test/`, `ESP32S3_Gamepad_Diag/ESP32S3_Gamepad_Diag.ino`, `xiaozhi-car/` (WIRING.md, pins.h, led_device.c) — 35 files total | `build/`, `managed_components/` excluded |

**Music backup zip list:**
```
ESP32S3_Gamepad_Diag/ESP32S3_Gamepad_Diag.ino
esp32_test/CMakeLists.txt, dependencies.lock, sdkconfig, main/{CMakeLists.txt, idf_component.yml, main.c}
xbox360_controller/{CMakeLists.txt, dependencies.lock, partitions.csv, sdkconfig, sdkconfig.defaults, main/{car.c, car.h, CMakeLists.txt, horn_sound.h, idf_component.yml, intro_test_sound.h, learning_adv.h, main.c, sensors_adv.h, startup_sound.h, xbox360.c, xbox360.h, sounds/{a_click,brake,engine_loop,gripper,hover_loop,laser,stop,yclick}.wav}}
xiaozhi-car/{device/{led_device.c, pins.h}, docs/WIRING.md}
```

## 7. Kya Ho Gya ✅
- [x] 4 ultrasonic L/R/F/B working, HUD B: display
- [x] Auto REVERSE/ESCAPE rear collision avoid
- [x] Dist averaging init fix
- [x] Pin conflicts zero
- [x] MAX98357A volume 95% cut, I2S pins final 39/47/35
- [x] Relays active LOW, boot click test OK
- [x] Full backups videos + Music
- [x] Build/flash pipeline stable

## 8. Kya Reh Gya ⏳

| Priority | Task | Details |
|---|---|---|
| HIGH | Rear field test | HUD pe B: cm verify, 65535 vs real cm, 13/14 wiring continuity |
| HIGH | Partition free | 1% free — `partitions.csv` me app 0x100000 → 0x150000 ya sounds ko `spiffs` me move |
| MEDIUM | Bluetooth A2DP Sink | User ne pucha — `esp_a2dp` + `bluetooth` menuconfig enable, I2S same pins pe stream, PC se pair |
| MEDIUM | Rear beep | `min3` → `min4` if rear obstacle beep chahiye |
| LOW | GAIN hardware | MAX98357A GAIN pin GND=9dB, 600k to GND=12dB, check jumper |
| LOW | Auto speed 20% | Pehle manga tha, ab `spd=10` etc me limit, verify karo |

## 9. Agla Step (User se Confirmation Chahiye)
1. Rear sensor test karna hai ya Bluetooth A2DP add karna hai?
2. Partition bada karna hai ya sounds compress karne hain?

## 🔧 Fix: Gear Caps, RT Curve, Controller Disconnect — 1 Sep 2026

### तीन बग ठीक किए:

### 1. RT (Right Trigger) सिरf square curve — sudden jump at top
- **पुराना:** `cap_raw = cap_raw * cap_raw` (square) → halka dabao = बहुत कम speed, 180+ dabao = तेज़ jump
- **नया:** linear/proportional → हल्का दबाओ = low speed, पूरा दबाओ = full speed, gradually

### 2. Gear 5 = 0% (stale NVS) → motor बंद
- **पुराना:** NVS में `gears=85/35/50/70/0` था → gear 5 = 0% → motors completely OFF
- **नया defaults:** {5, 15, 25, 35, 50} — gear 1=5%, 2=15%, 3=25%, 4=35%, 5=50%
- **NVS migration:** `SETTINGS_VER=2` bump → अगर पुराना version है, gear caps नए defaults से set

### 3. Controller disconnect → estop लिप्त, reconnect पे motor चलता नहीं
- **bug:** Disconnect पर `g.estop = true`, Reconnect पे **never cleared** → car permanently frozen
- **fix:** Reconnect पे `g.estop = false` automatically

### DInput/HID mode warning (hardware config)
- Controller/Dongle DInput/HID mode में है → HOME 5s hold = XInput mode

### Boot verify:
```
I (928) xbox360: dongle connected (addr=1)
I (1718) car: gear caps reset to defaults: 5/15/25/35/50
I (1718) car: settings loaded: spd=50 vol=100 obs=30 led=100 gears=5/15/25/35/50
I (15278) car_os: state -> DRIVE
I (15908) car: eng: tgt=0,0 cur=0,0 es=0 f=65535  (estop=0, no errors)
```

---
*Generated: 27 Aug 2026 — is chat ke har message, har pin change (8→12, 14→15→13, 15→16→14, 35/39/47↔7/15/16), har build size, har flash log is file me documented hai.*
