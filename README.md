# 🚗 xbox360_controller — ESP32-S3 Robot Car + Car OS

Xbox 360 wireless controller (Redgear Pro dongle) se chalne wala robot car,
jis par custom **"Car OS" dashboard** chalta hai (TFT + OLED), sath me
WiFi / AWS IoT (Alexa) voice-control bridge.

- **MCU:** ESP32-S3 (N16R8 — 16 MB Flash, 8 MB Octal PSRAM)
- **Framework:** ESP-IDF v6.0 + FreeRTOS
- **Control:** Xbox 360 gamepad via USB Host (native USB-OTG)
- **UI:** 2.8" SPI TFT (Car OS) + 0.96" OLED (mini HUD)
- **Cloud (optional):** MQTT/TLS → AWS IoT → Alexa Smart Home skill

> 🔐 **Security note:** is repo me koi WiFi password, private key ya device
> certificate **nahi** hai. Ye sab NVS provisioning se device me dalte hain
> (neeche dekho). Purani history se secrets hata diye gaye hain.

## ✨ Features

- Gamepad drive (dual-stick, triggers, rumble-style feedback via lights/sound)
- Car OS: Drive HUD, Analytics, Settings, Games Hub (Neon Convoy / Neon Serpent),
  Diagnostics, standby clock
- Sensors: MPU-6500 IMU (Motion Radar), 3× HC-SR04 ultrasonic
- Sound: MAX98357A I2S amp — engine loop, horn, UI clicks (SPIFFS `storage` partition)
- Lights: WS2812 strips (rear + under-car + roof police light), relay headlight/backlight
- Alexa bridge: MQTT/TLS Device Shadow, safety gateway (cloud kabhi direct motor
  command nahi bhejta)
- Voice AI component (`components/voice_ai`) — offline mode jab WiFi na ho

## 🧰 Hardware (summary)

| Part | Detail |
|---|---|
| ESP32-S3 DevKit N16R8 | Main board, USB-OTG = gamepad host, UART0 = console |
| Xbox 360 wireless receiver | Redgear Pro dongle, 045E:028E / 045E:0719 |
| 2.8" TFT ILI9341/ST7789 | 320×240 landscape, 40 MHz SPI |
| OLED SSD1306 128×64 | I2C 0x3C, mini HUD |
| MPU-6500, HC-SR04 ×3 | Motion + distance |
| MAX98357A + speaker | I2S audio |
| D1 motor driver (UART slave) | Motors @115200 |
| Servos, WS2812 LEDs, relays, mist maker | GPIO (detail: `PROJECT_DETAILS.md` §3) |

Full pin map, button map, RAM/ROM report: **`PROJECT_DETAILS.md`**
(old reference, code se verify kiya gaya).

## 📁 Project structure

```
CMakeLists.txt          # IDF project (target esp32s3, 16 MB flash)
partitions.csv          # nvs / otadata / phy / factory 6M / storage (SPIFFS 9.8M)
sdkconfig.defaults      # target + PSRAM + partition defaults (no secrets)
main/                   # firmware source (car, Car OS, drivers, alexa_bridge, net_wifi)
components/voice_ai/    # voice/AI component
spiffs/                 # storage partition assets (snd/*.raw, img/*)
tools/*.py              # asset generators (cars, sounds, topdown)
cloud/alexa/            # Lambda skill (Node), IoT policy docs, NVS provisioning script
build/                  # (ignored) IDF build output
managed_components/     # (ignored) IDF component manager deps
```

Docs: `PROJECT_DETAILS.md` · `CAR_OS_UI_GUIDE_UPDATED.md` ·
`CODE_REVIEW_2026-09-17.md` · `PROJECT_ANALYSIS_AND_BUGS.md` · `SESSION_SUMMARY.md` ·
`cloud/alexa/README_ALEXA_SETUP.md`

## 🚀 Build & flash (ESP-IDF v6.0)

```bash
# IDF setup (ek baar)
~/esp/esp-idf/export.sh        # ya: source ~/esp/esp-idf/export.sh

# Build
idf.py build

# Flash + monitor (board USB-UART bridge par)
idf.py -p /dev/ttyUSB0 flash monitor
```

Flash layout (16 MB, `build/flasher_args.json` se):

| Offset | File |
|---|---|
| 0x0 | `build/bootloader/bootloader.bin` |
| 0x8000 | `build/partition_table/partition-table.bin` |
| 0xf000 | `build/ota_data_initial.bin` |
| 0x20000 | `build/xbox360_controller.bin` |
| 0x620000 | `build/storage.bin` (SPIFFS assets) |

Manual esptool example:

```bash
esptool.py --chip esp32s3 --baud 460800 write-flash \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0xf000 build/ota_data_initial.bin \
  0x20000 build/xbox360_controller.bin \
  0x620000 build/storage.bin
```

> 📦 **Ready firmware:** GitHub **Releases** me `.zip` milta hai
> (bootloader + partition-table + app + storage + flash script) —
> build karne ki zaroorat nahi.

## 🔐 Provisioning (WiFi + Alexa — NVS, code me secret nahi)

Firmware me koi default password/key nahi hai. Pehli baar NVS me dalo:

**WiFi** — NVS namespace `net`:
- `wifi_ssid`, `wifi_pass`

**Alexa / AWS IoT** — NVS namespace `alexa`:
- `broker` (e.g. `mqtts://<endpoint>.iot.<region>.amazonaws.com:8883`)
- `user`, `thing`, `profile`
- blobs `ca` / `cert` / `key` (PEM) — script se:

```bash
python3 cloud/alexa/write_pems_nvs.py \
  --port /dev/ttyUSB0 \
  --cert cloud/alexa/device_cert.pem \
  --key cloud/alexa/private_key.pem
```

`*.pem`, `*_key`, `nvs_pems.csv`, `nvs_certs_only.csv` sab `.gitignore` me hain —
kabhi commit nahi hote. `nvs_strings_only.csv` (broker/user/thing, no secrets)
reference ke liye repo me hai.

Alexa skill setup: **`cloud/alexa/README_ALEXA_SETUP.md`**

## 📦 Releases

Har release me:
- `firmware-<version>.zip` — upar wali 5 `.bin` files + `flash.sh`/`flash.ps1`
- `flasher_args.json` — offsets ka source of truth

Flash karne ke baad NVS provisioning (upar) zaroor karo, warna WiFi/Alexa
disabled rahega aur car offline mode me chalegi (gamepad + Car OS kaam karega).

## 🛟 Troubleshooting

- **Monitor garbage / no boot:** `idf.py -p /dev/ttyUSB0 monitor`, baud 115200; USB-UART bridge wali port use karo (OTG port gamepad ke liye hai).
- **WiFi connect nahi:** NVS `net` keys check karo; `net_wifi` tag me `using SSID ...` log dekho.
- **Alexa bridge silent:** `broker` empty = disabled (by design). NVS `alexa` keys + `ca/cert/key` blobs check karo.
- **Storage (sound/img) missing:** `storage.bin` 0x620000 par flash hua? `spiffs/` se `idf.py build` dobara banata hai.
- **Gamepad nahi:** OTG port par dongle, console UART0 par logs — `xbox360` tag dekho.

## 📄 License

SPDX headers ke mutabiq (Apache-2.0) jahan mentioned hai. Baqi code is repo ke
sath as-is hai — apne risk par use karo, pehle test bench par chalao.
