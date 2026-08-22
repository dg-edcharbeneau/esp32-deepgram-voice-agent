/*
 * Wi-Fi provisioning over a SoftAP captive portal.
 *
 * The device raises an open access point called dg-agent-XXXX. Joining it from
 * a phone pops a browser page -- a scan list, a password box, Save -- and the
 * credentials land in NVS. The device then reboots into station mode.
 *
 * WHY A PORTAL AND NOT BLE
 *
 * No app to install and no pairing: anything with a browser can do it, which
 * includes every phone and laptop in the room. The board's factory firmware
 * (a xiaozhi-esp32 build) reached the same conclusion -- its flash image still
 * has the portal HTML in it.
 *
 * WHY THE AP IS OPEN
 *
 * A WPA2 provisioning AP would need its passphrase shown somewhere before the
 * user could join, which on a 466 px round panel means displaying a passphrase
 * to protect a passphrase. The exposure is one unencrypted local hop, lasting
 * as long as it takes to fill in a form, and the AP is gone after the reboot.
 * If that trade is ever unacceptable, this is the place to change it.
 *
 * WHY IT ALWAYS ENDS IN A REBOOT
 *
 * Re-plumbing a live device from AP to STA means tearing down the portal, the
 * DNS responder and the AP netif underneath an HTTP request that is still being
 * answered. A reboot costs ~2 seconds and is impossible to get subtly wrong.
 * It also guarantees the portal and a TLS agent session never hold internal RAM
 * at the same time.
 */
#pragma once

#include "esp_err.h"

/*
 * The AP's name, e.g. "dg-agent-A1B2" -- derived from the SoftAP MAC, so it is
 * stable for a given board and distinct between two of them on one bench.
 *
 * Static storage, so it is safe to hand to spectrum_ui_set_status(), and usable
 * before wifi_prov_start() has run.
 */
const char *wifi_prov_ap_name(void);

/*
 * Brings up the AP, the HTTP server and the DNS responder. Non-blocking, so the
 * caller can get the panel up while it settles.
 *
 * Requires wifi_stack_init(). If a station was running, stop it first with
 * wifi_sta_stop() -- this takes the radio.
 */
esp_err_t wifi_prov_start(void);

/*
 * Parks the calling task while the portal is in use. Does not return.
 *
 * Ends in esp_restart(), either because credentials were saved or because the
 * portal sat idle for five minutes. The idle path matters: without it, a device
 * whose router was merely rebooting would sit in AP mode forever instead of
 * retrying the network it already knows.
 */
void wifi_prov_run(void) __attribute__((noreturn));
