#pragma once

/*
 * The P4's Wi-Fi wedge watchdog (CONFIG_WIFI_WEDGE_RESET).
 *
 * WHAT IT IS FOR
 *
 * On the P4 the radio is the board's ESP32-C6, reached over SDIO through
 * esp_hosted. A few hours into uptime, with no log line on either chip, the
 * C6 can stop passing Wi-Fi traffic while still answering control calls. The
 * bench caught it twice on 2026-11-18, in two forms:
 *
 *   dead     still associated at full signal, but nothing reaches even the
 *            gateway; no recovery in 5 min
 *   refuse   traffic fades out, the association drops, and every rejoin then
 *            fails (reasons 2 AUTH_EXPIRE and 205) -- 637 tries over 6 h --
 *            while the access point serves everyone else
 *
 * Rejoining does not clear it: the reconnect loop in wifi_sta.c IS a rejoin,
 * and that is what failed 637 times. A ROM reset of both chips (boot pulses
 * the C6's reset line) rejoined in 5 s, same AP, same MAC. So this watchdog
 * detects either form and does that reset, with a backoff so a fault the reset
 * does not cure cannot become a reboot loop.
 *
 *   refuse   a run of disconnects whose reason says the AP was there, lasting
 *            CONFIG_WIFI_WEDGE_REFUSE_S. A router that is off gives 201
 *            (NO_AP_FOUND) instead, which ends the run: no reset for that.
 *   dead     three pings to the gateway, every CONFIG_WIFI_WEDGE_CHECK_S and
 *            at once on a websocket error; 0/3 twice, 10 s apart. Armed only
 *            once this association has proven itself -- the gateway answered
 *            a ping, or the session reached its server ("check armed") -- so
 *            a network that drops ICMP cannot trip it on its own.
 *
 * Every step logs an "EVT wifi wedge" line, and the TLM line carries the reset
 * count, so a field log says which form happened and whether the reset cured
 * it.
 *
 * NOT HERE: asking the C6 for its firmware version. The factory C6 does not
 * implement that RPC; the request fails after 5 s and the data path is dead
 * within seconds of it, on every boot tried (2026-11-19). Leave it out until
 * the C6 runs firmware that answers it.
 */

#include <stdbool.h>

#include "sdkconfig.h"

#if CONFIG_WIFI_WEDGE_RESET

/* Starts the watchdog task. Call once, after
 * the first join. False if the task could not be made (the device then runs
 * as before, unwatched). */
bool wifi_wedge_init(void);

/* From wifi_sta.c's event handler, while the station is meant to be joined. */
void wifi_wedge_note_disconnect(int reason);
void wifi_wedge_note_got_ip(void);

/* From dg_agent.c when the session's socket opens: proof this association
 * carried traffic, which arms the gateway check even if the gateway has not
 * answered a ping yet. */
void wifi_wedge_note_link_ok(void);

/* Non-blocking, any task: check the link now rather than at the next tick.
 * `why` must be a string literal. */
void wifi_wedge_check_now(const char *why);

/* Wedge resets since the last power-on or healthy half hour. For TLM. */
int wifi_wedge_resets(void);

/* Bench fault injection, for net_probe.c's "wdtest" command. Only the bench
 * build can set it; without CONFIG_BENCH_NET_PROBE it stays OFF. */
#define WIFI_WEDGE_INJECT_OFF    0
#define WIFI_WEDGE_INJECT_DEAD   1   /* every gateway ping reports no reply */
#define WIFI_WEDGE_INJECT_REFUSE 2   /* a refusal run, without touching the radio */
#if CONFIG_BENCH_NET_PROBE
void wifi_wedge_inject(int what);
#endif

#else

static inline bool wifi_wedge_init(void) { return true; }
static inline void wifi_wedge_note_disconnect(int reason) { (void)reason; }
static inline void wifi_wedge_note_got_ip(void) {}
static inline void wifi_wedge_note_link_ok(void) {}
static inline void wifi_wedge_check_now(const char *why) { (void)why; }
static inline int wifi_wedge_resets(void) { return 0; }

#endif
