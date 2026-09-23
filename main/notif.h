/*
 * notif.h - PART 10.3 Notification Center feed.
 * Merged: Alexa log + safety + rules + sound + MPU events.
 * Ring buffer (24 entries), rendered by OS_NOTIF window.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NOTIF_ALEXA = 0,   /* blue */
    NOTIF_SAFETY,      /* red */
    NOTIF_RULES,       /* orange */
    NOTIF_SOUND,       /* yellow */
    NOTIF_MPU,         /* purple */
    NOTIF_COUNT
} notif_cat_t;

void notif_push(notif_cat_t cat, const char *text, uint32_t now_ms);

/* PART 15: ring lives in PSRAM - call once at boot before any push/list. */
void notif_init(void);

typedef struct {
    notif_cat_t cat;
    char text[48];
    uint32_t ms;
} notif_item_t;

/* newest-first snapshot; returns count (<= max). */
int notif_list(notif_item_t *out, int max);

const char *notif_cat_name(notif_cat_t c);

#ifdef __cplusplus
}
#endif
