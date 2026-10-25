#pragma once

/*
 * The orb clock: what the panel shows while the device sleeps and the time is
 * known. A small dotted sphere with a meridian sweeping it, hour and minute
 * rings that light up to the time, the time itself, and a Wi-Fi / battery row
 * along the top -- Ed's "Orb clock" mockup. clock_face.c has the geometry; this
 * paints it into the face's canvas.
 *
 * ui.c decides when it shows and hands it the whole frame, as it does the touch
 * panel: the face and its overlays stand down until it is gone.
 *
 * LVGL task only, like everything in ui.c.
 */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "stopwatch.h"

typedef struct {
    uint32_t now_ms;
    int hh, mm;            /* local, 24-hour */
    bool h12;              /* show "2:38" + "PM" rather than "14:38" */
    const char *next;      /* the next-alarm line, "" for none */
    uint32_t tint_rgb;     /* the orb colour */
    bool bat_valid, bat_low, bat_charging;
    int bat_percent;
    bool sig_valid;
    int sig_bars;          /* 0..4 */
} ui_clock_state_t;

/* Once, from build_ui(), with the face's canvas. False if its PSRAM is short;
 * the clock then simply never shows and sleep keeps the plain orb. */
bool ui_clock_init(lv_obj_t *canvas);

/* Paint one frame. The first call after ui_clock_leave() starts the motion
 * from t = 0, as the mockup does when it appears. */
void ui_clock_frame(const ui_clock_state_t *st);

/*
 * The stopwatch screen: the clock's parts with the seconds ring, the sphere
 * spinning only while `sw` runs, and two round buttons. `pressed` is the button
 * showing its pressed shade (0 left, 1 right), or -1.
 */
void ui_clock_stopwatch_frame(const ui_clock_state_t *st, const sw_t *sw, int pressed);

/* Which stopwatch button a press at (x, y) is on: 0 the left (reset or lap),
 * 1 the right (play or pause), -1 neither. */
int ui_clock_stopwatch_hit(int32_t x, int32_t y);

/* Forget the clock, and with `blank` black the canvas for the face that takes
 * over. Without it when the touch panel took over: it has already painted the
 * whole canvas this frame. */
void ui_clock_leave(bool blank);
