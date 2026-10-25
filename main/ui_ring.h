#pragma once

/*
 * The ring screen: what the panel shows while a timer or alarm rings -- Ed's
 * "Timer alarm" design. A small dotted orb near the top with rings pulsing out
 * of it, the label, a line with the overtime counting up, and in the bottom
 * half the way to end it: for a timer one big Stop, for an alarm a Stop
 * quarter beside a "+1 min" snooze quarter.
 *
 * SNOOZE: each tap adds a minute and turns the button solid; it then fades to
 * its dark shade over SNOOZE_FADE_MS, a tap meanwhile adding another minute and
 * starting the fade again. Reaching the dark end commits the snooze for the
 * minutes shown.
 *
 * ui.c decides when it shows and hands it the frame and the presses, as it does
 * the touch panel and the clock. LVGL task only.
 */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "ui.h"

void ui_ring_init(lv_obj_t *canvas);

/* Whether `r` still wants the screen: ringing, and not already ended from this
 * screen (a Stop hides it at once, before alarms.c's next tick catches up). */
bool ui_ring_wanted(const ui_ring_t *r);

/* Paint one frame. `h12` for an alarm's time on its line. */
void ui_ring_frame(const ui_ring_t *r, uint32_t now_ms, uint32_t tint_rgb, bool h12);

/* A press while the screen is up. Every press is the ring screen's: one on
 * nothing does nothing. */
void ui_ring_pressed(const ui_ring_t *r, int32_t x, int32_t y, uint32_t now_ms);

/* Black the canvas for whatever takes over. */
void ui_ring_leave(void);
