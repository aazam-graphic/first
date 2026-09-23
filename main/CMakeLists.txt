idf_component_register(SRCS "main.c" "xbox360.c" "car.c" "tft_display.c" "tft_boot_anim.c"
                            "car_os.c" "os_gfx.c" "os_assets.c" "os_screens_tft.c" "os_oled.c" "car_games.c" "os_widgets.c" "input_events.c" "roof_light.c" "cargame_neon_convoy.c" "cargame_neon_serpent.c" "ns_theme.c"
                             "imu_driver.c" "imu_adv.c" "ui_motion_radar.c" "ui_drive_cockpit.c"
                             "snd_bank.c" "mic_in.c" "img_bank.c" "notif.c" "mem_diag.c"
                             "net_wifi.c" "alexa_bridge.c"
                    INCLUDE_DIRS "."
                                           PRIV_REQUIRES esp_driver_ledc esp_driver_i2c esp_timer esp_rom esp_driver_gpio led_strip esp_driver_i2s esp_driver_uart nvs_flash spiffs vfs esp_lcd esp_driver_spi esp_ringbuf
                                  esp_wifi esp_netif mqtt cjson mbedtls
                     EMBED_FILES "../spiffs/engine_loop.wav" "../spiffs/hover_loop.wav" "../spiffs/brake.wav" "../spiffs/stop.wav" "../spiffs/gripper.wav" "../spiffs/laser.wav" "../spiffs/yclick.wav" "../spiffs/a_click.wav"
                                 "../spiffs/snd/sys_ready.raw")