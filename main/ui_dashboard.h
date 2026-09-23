#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void ui_dashboard_init(void);
void ui_dashboard_update(int speed_kmh, int batt_pct, float batt_v, int rssi, int latency_ms, const char *mode, bool hazard, int rt_pct, int lt_pct, int steer_deg, bool btn_a, bool btn_b, bool btn_x, bool btn_y);
void ui_dashboard_set_battery(float v);
#ifdef __cplusplus
}
#endif
