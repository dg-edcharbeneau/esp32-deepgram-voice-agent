/* See stopwatch.h. */
#include "stopwatch.h"

#include <stdio.h>

void sw_toggle(sw_t *sw, uint32_t now_ms)
{
    switch (sw->state) {
    case SW_READY:
    case SW_PAUSED:
        sw->since_ms = now_ms;
        sw->state = SW_RUNNING;
        break;
    case SW_RUNNING:
        sw->banked_ms += now_ms - sw->since_ms;
        sw->state = SW_PAUSED;
        break;
    }
}

bool sw_lap(sw_t *sw, uint32_t now_ms)
{
    if (sw->state != SW_RUNNING) {
        return false;
    }
    sw->lap_ms = sw_elapsed(sw, now_ms);
    if (sw->laps < UINT16_MAX) {
        sw->laps++;
    }
    return true;
}

bool sw_reset(sw_t *sw)
{
    if (sw->state != SW_PAUSED) {
        return false;
    }
    *sw = (sw_t){.state = SW_READY};
    return true;
}

uint32_t sw_elapsed(const sw_t *sw, uint32_t now_ms)
{
    return sw->state == SW_RUNNING ? sw->banked_ms + (now_ms - sw->since_ms) : sw->banked_ms;
}

void sw_fmt(uint32_t ms, char *main, size_t main_cap, char *frac, size_t frac_cap)
{
    const uint32_t s = ms / 1000;
    if (s >= 3600) {
        snprintf(main, main_cap, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)((s / 60) % 60),
                 (unsigned)(s % 60));
        if (frac_cap > 0) {
            frac[0] = '\0';
        }
    } else {
        snprintf(main, main_cap, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
        snprintf(frac, frac_cap, ".%02u", (unsigned)((ms / 10) % 100));
    }
}
