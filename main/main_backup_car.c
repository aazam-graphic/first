/*
 * main.c - ESP32-S3 USB Host: Xbox 360 wireless receiver (Redgear Pro)
 *
 * Build/flash:
 *   ESP-IDF 6.0.2 PowerShell open karo (Start menu: "ESP-IDF 6.0.2 CMD/PowerShell")
 *   cd C:\Users\aazam\OneDrive\Documents\Default Project\xbox360_controller
 *   idf.py build
 *   idf.py -p COMxx flash monitor     (COMxx = UART bridge wala port)
 *
 * Board par:
 *   - Dongle native USB-OTG port mein (OTG adapter ke saath)
 *   - Serial Monitor UART/COM port se (115200 default)
 *   - Controller: HOME dabao, auto-pair; HOME 5s hold = XInput/DInput toggle
 *
 * Tasks:
 *   usb_host (core0, prio2)  - USB Host library events
 *   xbox360  (core0, prio3)  - gamepad driver
 *   car      (core1, prio2)  - car control (drive/auto/servos/lights/HUD)
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "usb/usb_host.h"
#include "xbox360.h"
#include "car.h"

static const char *TAG = "usb_host";

static void usb_host_lib_task(void *arg)
{
    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
        .peripheral_map = BIT0,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_LOGI(TAG, "USB Host installed (peripheral_map=0x%x)", host_config.peripheral_map);
    xTaskNotifyGive(arg);

    while (1) {
        uint32_t event_flags;
        ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &event_flags));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "xbox360_controller starting");

    TaskHandle_t host_task_hdl = NULL;
    BaseType_t created = xTaskCreatePinnedToCore(usb_host_lib_task,
                                                 "usb_host",
                                                 4096,
                                                 xTaskGetCurrentTaskHandle(),
                                                 2,
                                                 &host_task_hdl,
                                                 0);
    assert(created == pdTRUE);

    /* wait until the host library is installed (2s timeout, fail-safe) */
    if (ulTaskNotifyTake(false, pdMS_TO_TICKS(2000)) == 0) {
        ESP_LOGE(TAG, "USB Host install timeout - continuing anyway, xbox360 may fail");
    } else {
        ESP_LOGI(TAG, "USB Host ready");
    }

    created = xTaskCreatePinnedToCore(xbox360_task,
                                      "xbox360",
                                      8192,
                                      NULL,
                                      3,
                                      NULL,
                                      0);
    assert(created == pdTRUE);

    created = xTaskCreatePinnedToCore(car_task,
                                      "car",
                                      8192,
                                      NULL,
                                      2,
                                      NULL,
                                      1);
    assert(created == pdTRUE);
}