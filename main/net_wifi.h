/* SPDX-License-Identifier: Apache-2.0 */
/* net_wifi.h - generic Wi-Fi station + SNTP time sync (no chatbot dependency).
 *
 * Alexa bridge (MQTT/TLS) needs: (1) station IP, (2) valid wall-clock for
 * certificate validation + command TTL. Both are managed here, non-blocking.
 * Credentials: NVS namespace "net" keys wifi_ssid/wifi_pass, else defaults.
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_WIFI_DOWN = 0,      /* not started */
    NET_WIFI_CONNECTING,    /* connecting / retrying */
    NET_WIFI_UP,            /* connected, has IP */
} net_wifi_state_t;

/* Default credentials - NEVER hardcode real SSID/PASS in git.
 * Provision via NVS keys "wifi_ssid" / "wifi_pass" in namespace "net".
 * Empty = do not auto-connect with a baked-in password. */
#define NET_WIFI_DEFAULT_SSID  ""
#define NET_WIFI_DEFAULT_PASS  ""

/* Start station mode + connect. Idempotent, non-blocking (event-driven). */
esp_err_t net_wifi_start(void);

net_wifi_state_t net_wifi_get_state(void);
bool             net_wifi_up(void);
/* True once SNTP has set a valid epoch (needed for MQTT TLS + cmd TTL). */
bool             net_wifi_time_synced(void);

#ifdef __cplusplus
}
#endif
