#pragma once

/*
 * The touch panel: a pull-down set of pages for settings that otherwise change
 * only by voice -- sound (mic gain, volume), the orb's colour, two toggles
 * (noise reduction, 12/24-hour) and pending timers and alarms. Nothing here is a
 * new feature; see controls.h.
 *
 * Internal to the UI: ui.c owns the canvas, the touch events and the frame
 * timer, and calls these on the LVGL task with the LVGL lock held. Swipe down
 * from the top edge to open; sideways to change page; up, or 8 s untouched, to
 * close. While open the panel owns the whole screen and the centre button means
 * nothing.
 */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

void ui_panel_init(lv_obj_t *canvas);

bool ui_panel_is_open(void);

/* The current page, or -1 when closed. For the TLM line. */
int ui_panel_page(void);

/*
 * Touch, in the order LVGL delivers it. pressed() returns true when the panel is
 * open and the press is its own; released() returns true when the whole gesture
 * was the panel's (including the drag that opened it), so ui.c must not also
 * treat it as a tap or hold on the centre button. `can_open` is ui.c's say on
 * whether anything else owns the screen (QR code, display test, alarm).
 */
bool ui_panel_pressed(int32_t x, int32_t y, bool can_open);
void ui_panel_pressing(int32_t x, int32_t y);
bool ui_panel_released(bool lost);

/* Close now, e.g. for an alarm or for sleep. No-op when closed. */
void ui_panel_close(const char *why);

/*
 * Once per frame, always -- the deferred save runs from here even while the
 * panel is closed. Returns true when the panel owns this frame, in which case
 * ui.c draws nothing else. `tint_rgb` is the orb colour in force; `session_live`
 * only paces the self-test (CONFIG_UI_PANEL_SELFTEST).
 */
bool ui_panel_frame(uint32_t now_ms, uint32_t tint_rgb, bool session_live);
