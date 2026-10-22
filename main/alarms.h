/*
 * Timers and alarms: the table, keeping it across a reboot, and what happens
 * when one goes off.
 *
 * WHAT RINGING DOES
 *
 * Someone set this by voice, so it ends in a voice -- but the network may be
 * down and a session takes seconds to open, so it starts with a sound that
 * needs neither:
 *   1. The chime, made locally (audio_io_chime_start()), with the label as a
 *      caption and the panel held awake.
 *   2. After CONFIG_ALARM_CHIME_S, or a tap anywhere on the panel, the chime
 *      stops and the speaker drains.
 *   3. Only then a session opens, and the agent says what it was ("your pasta
 *      timer is done"). Only then, because the microphone opens with the
 *      session, and a tone is the worst thing the echo canceller could warm up
 *      on -- see audio_io_chime_start().
 * With a session already up there is no chime at all: the agent just says it.
 *
 * WHO TOUCHES WHAT
 *
 * The agent's tools edit the table on the WebSocket task, under a mutex, and
 * mark it dirty. Everything slow -- the NVS write, mktime() across the week,
 * firing -- is alarms_tick() on the main loop, which already runs once a second
 * and is the task this firmware agrees may be slow. No task of its own, so no
 * internal RAM spent on a stack.
 *
 * Only DEFINITIONS reach flash. A recurring alarm's next instant is computed on
 * load and after each ring, never stored as truth, so an alarm that rings every
 * weekday for a year never writes the flash once.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/* Load the saved table. Call after nvs_flash_init(). No-op when CONFIG_CLOCK is
 * off. Nothing is scheduled until alarms_tick() first sees a valid clock. */
void alarms_init(void);

/* Main loop, once per pass. */
void alarms_tick(void);

/* The chime phase is under way: the panel stays awake and a touch anywhere
 * dismisses rather than doing what it would normally do. */
bool alarms_ringing(void);

/* Stop the chime. The announcement still follows -- it is how the user learns
 * which alarm it was. Safe from any task. */
void alarms_dismiss(void);

/* Pending entries, for the TLM line. */
int alarms_count(void);

/* The soonest pending alarm's instant, or false if there is none with a known
 * wall-clock time. For the face. */
bool alarms_next(int64_t *epoch);

/*
 * Agent tools. Each writes a phrasing instruction for the model into `out`.
 * Two schemas carry the four: dg_agent.c sends set_alarm to set_timer when
 * `seconds` is given, and `alarms` to cancel when `cancel` is.
 */
void alarms_fn_set_timer(const cJSON *args, char *out, size_t n);
void alarms_fn_set_alarm(const cJSON *args, char *out, size_t n);
void alarms_fn_list(char *out, size_t n);
void alarms_fn_cancel(const cJSON *args, char *out, size_t n);
