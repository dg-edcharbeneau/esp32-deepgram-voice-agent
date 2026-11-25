#pragma once

/*
 * Network probe and serial commands (CONFIG_BENCH_NET_PROBE, bench only).
 *
 * WHAT IT IS FOR
 *
 * The P4 has a stall where the radio stays associated -- RSSI reads, the bars
 * are full -- and no data moves: DNS fails (getaddrinfo 202), the websocket
 * cannot get its lock, sends back up. The device cannot say WHERE the data
 * stops, and that is the question: the P4 and its C6 over SDIO, the access
 * point, or past it. A probe answers it by asking each hop in turn:
 *
 *   gw     ping the gateway                 dead: the P4/C6 link, or the AP
 *   dns    ping the DNS server              (often the gateway itself)
 *   inet   ping 8.8.8.8, no name involved   dead with gw alive: past the AP
 *   name   resolve the agent's host         dead with inet alive: DNS only
 *
 * One "EVT netprobe" line carries all four. It runs on a transport error and
 * on the "probe" command.
 *
 * The serial commands exist because nobody may be at the bench to tap: "tap"
 * is the BOOT button's click (a toggle), "stop" stops, "probe" probes, and
 * "txpower [dBm]" reads or caps the radio's transmit power, to make the
 * uplink weak without moving the board. "rejoin" drops and re-makes the
 * association and "reboot" resets both chips: the two recoveries, to find
 * which one a stall yields to. One per line on the console UART.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#if CONFIG_BENCH_NET_PROBE

/* Starts the probe task and the console reader. False if either failed. */
bool net_probe_init(void);

/* Non-blocking, any task (the websocket event task included). Probes once,
 * soon; requests landing while one runs, or within the hold-off after it,
 * are dropped. `why` must be a string literal. */
void net_probe_request(const char *why);

/* The protocol a bench "proto" command left for the next boot, kept across a
 * software reset (not power-on), or 0 for none: wifi_sta.c then uses the
 * Kconfig choice. Lets a soak give each arm a freshly reset C6. */
uint8_t net_probe_boot_proto(void);

#else

static inline bool net_probe_init(void) { return true; }
static inline void net_probe_request(const char *why) { (void)why; }
static inline uint8_t net_probe_boot_proto(void) { return 0; }

#endif
