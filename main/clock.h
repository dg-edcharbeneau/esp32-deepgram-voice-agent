/*
 * Wall-clock time: where it comes from, which zone it is read in, whether it is
 * shown 12 or 24-hour, and the two agent tools that ask about it.
 *
 * WHERE THE TIME COMES FROM
 *
 * SNTP, started the first time the station gets an address. Optionally a
 * PCF85063 RTC (CONFIG_CLOCK_RTC), read at boot and written after every sync --
 * off by default, because the ESP32-S3-Touch-AMOLED-1.75C has none, whatever
 * its board package's comment says. A device with no RTC and no network simply
 * has no time,
 * and everything that asks -- the face, the tools, the alarms -- says so rather
 * than inventing 1970.
 *
 * WHICH ZONE
 *
 * One name in NVS, exactly as it was given ("America/Chicago", or a POSIX rule),
 * resolved through tz_table.c on every boot. Storing the name rather than the
 * rule is deliberate: when the table's rules change -- they do; see the Canadian
 * rows -- a reflash fixes every device without anyone setting the zone again.
 *
 * THREADING
 *
 * The zone is APPLIED (setenv + tzset) and saved only in clock_tick(), on the
 * main loop. newlib re-parses TZ with a full sscanf the first time it is read
 * after a change, which wants over a kilobyte of stack, and the WebSocket task
 * that runs the agent's tools is the last place to spend that. A tool asks;
 * the loop does it within a second.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "cJSON.h"

typedef enum {
    CLOCK_SRC_NONE = 0,
    CLOCK_SRC_RTC  = 1,
    CLOCK_SRC_SNTP = 2,
} clock_source_t;

/*
 * Apply the saved zone, read the RTC, and arm SNTP for when the network comes
 * up. Call after nvs_flash_init(), battery_start() (which brings up the I2C bus)
 * and wifi_stack_init() (which creates the default event loop). No-op when
 * CONFIG_CLOCK is off.
 */
void clock_start(void);

/* Main loop, once per pass: starts SNTP, applies a zone change, writes the RTC
 * after a sync. Everything slow the clock does happens here. */
void clock_tick(void);

/*
 * The time now, if it is known. False -- and *local untouched -- before the
 * RTC or SNTP has supplied one. Either out pointer may be NULL.
 */
bool clock_now(int64_t *epoch, struct tm *local);

/*
 * Hold around local-time conversions made OFF the main loop -- the agent's tools
 * on the WebSocket task. clock_tick() holds it across setenv("TZ") + tzset(), so
 * no other task can call localtime_r()/mktime() between the two and make newlib
 * re-parse the zone (a >1 kB sscanf) on its own stack. Lock order: this first,
 * then any alarms.c lock. No-ops when CONFIG_CLOCK is off.
 */
void clock_lock(void);
void clock_unlock(void);

/* Where the current time came from, for the TLM line. */
clock_source_t clock_source(void);

/*
 * Changes whenever a zone is applied. An alarm "at 7:30" is a wall time, so
 * alarms.c recomputes its instants when this moves -- otherwise moving the
 * device from Chicago to London would leave it ringing at 7:30 Chicago.
 */
uint32_t clock_tz_generation(void);

/*
 * Whether to show and say 12-hour time: what the user asked for by voice if they
 * ever did, otherwise the zone's own convention (tz_default_12h()). Safe from
 * any task.
 */
bool clock_12h(void);

/* The zone as the user named it, for the tools and the portal. */
void clock_tz_name(char *out, size_t cap);

/*
 * For the setup portal, which runs before any of the above matters.
 *
 * clock_tz_saved() copies the zone the user saved, if they ever did -- not the
 * Kconfig default, which the portal would otherwise present as a choice someone
 * made. clock_save_tz() checks a name against tz_table.c and writes it to NVS;
 * the device reboots straight after, and applies it then. False if the name is
 * not one the device knows, and nothing is written.
 */
bool clock_tz_saved(char *out, size_t cap);
bool clock_save_tz(const char *name);

/* Agent tools. Each writes a phrasing instruction for the model into `out`. */
void clock_fn_get_time(char *out, size_t n);
void clock_fn_set_clock(const cJSON *args, char *out, size_t n);
