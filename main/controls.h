#pragma once

/*
 * What the touch panel can change, as plain calls that are safe on the LVGL task.
 *
 * Every one of these already exists as a voice tool or a BOOT gesture; this is
 * only the touch-shaped entry to it. The rule they all follow is the one
 * boot_button.c states: a flash write runs with the cache off and stalls the
 * audio path, so nothing here writes flash. Each change applies at once, and
 * the save goes to session_ctl's worker -- for volume and mic gain, two seconds
 * after the last press, so a run of presses costs one write.
 */

#include <stdbool.h>
#include <stdint.h>

#include "alarms.h"

/* Speaker volume, AUDIO_VOLUME_MIN..MAX in steps of 10. */
int controls_volume(void);
int controls_volume_step(int dir);       /* +1 / -1; returns the new level */

/* Microphone gain in dB, 3 dB steps (CONFIG_MIC_GAIN_TOOL). */
bool controls_mic_gain_available(void);
int controls_mic_gain(void);
int controls_mic_gain_step(int dir);     /* +1 / -1; returns the new gain */

/* Noise reduction, as the BOOT double-click toggles it (CONFIG_MIC_NS_ENABLE). */
bool controls_ns_available(void);
bool controls_ns_on(void);
void controls_ns_toggle(void);

/* 12/24-hour, as set_clock {hours} sets it (CONFIG_CLOCK). */
bool controls_clock_available(void);
bool controls_clock_12h(void);
void controls_clock_toggle_12h(void);

/* Pending timers and alarms, soonest first, and cancelling one. */
int controls_alarms(alarm_view_t *out, int max);
bool controls_alarm_cancel(uint8_t id);

/*
 * Ask for the save of anything still unsaved once `quiet_ms` has passed since the
 * last change; 0 asks now. Cheap: call it every frame.
 */
void controls_commit(uint32_t now_ms, uint32_t quiet_ms);
