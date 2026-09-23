/*
 * mem_diag.h - PART 15/16 memory diagnostics.
 * Enable via MEM_DIAG_ENABLE (dev/soak builds). Logs heap/PSRAM/task
 * stack HWM on a slow cadence. Task context ONLY (car_task loop) - never
 * ISR, never the motor hot path, never the MPU fast task.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MEM_DIAG_ENABLE
#define MEM_DIAG_ENABLE 1   /* dev/soak: 1. Set 0 for final production. */
#endif

void mem_diag_tick(uint32_t now_ms);

/* PART -1.1 blueprint-compat API: init latches on, start_timer sets the
   log cadence (default 10000ms). Tick-based core runs in car_task. */
void mem_diag_init(void);
void mem_diag_start_timer(uint32_t period_ms);

#ifdef __cplusplus
}
#endif
