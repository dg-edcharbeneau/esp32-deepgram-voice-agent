#pragma once

/*
 * The stopwatch, as arithmetic: ready, running or paused, the time it has run,
 * and the latest lap. Plain C with the time passed in, so host/alarms.sh drives
 * it on a laptop; controls.c keeps the device's one instance behind a lock, and
 * ui_clock.c draws it.
 *
 * Milliseconds are the esp_timer's, truncated to 32 bits like every other ms
 * stamp in this firmware; differences are taken unsigned, so a stopwatch
 * running across the 49.7-day wrap reads right.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SW_READY = 0,    /* at zero, never started since the last reset */
    SW_RUNNING,
    SW_PAUSED,
} sw_state_t;

typedef struct {
    sw_state_t state;
    uint32_t banked_ms;  /* time run before the current stretch */
    uint32_t since_ms;   /* when the current stretch began; RUNNING only */
    uint16_t laps;       /* how many laps have been taken */
    uint32_t lap_ms;     /* the latest lap's split: total time when it was taken */
} sw_t;

/* Ready -> running, running -> paused, paused -> running. */
void sw_toggle(sw_t *sw, uint32_t now_ms);

/* Running only: count a lap at the time now. False otherwise. */
bool sw_lap(sw_t *sw, uint32_t now_ms);

/* Paused only: back to ready, laps cleared. False otherwise -- a running
 * stopwatch is paused first, and a ready one has nothing to clear. */
bool sw_reset(sw_t *sw);

/* Total time run, at `now_ms`. */
uint32_t sw_elapsed(const sw_t *sw, uint32_t now_ms);

/*
 * `ms` as the display shows it: "12:34" and ".56" under an hour, "1:02:03" and
 * "" past it -- an hour-long readout in centiseconds is noise. Each buffer
 * should hold 12.
 */
void sw_fmt(uint32_t ms, char *main, size_t main_cap, char *frac, size_t frac_cap);
