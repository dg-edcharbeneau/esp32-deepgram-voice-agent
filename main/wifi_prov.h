/*
 * Wi-Fi provisioning over a SoftAP captive portal.
 *
 * The device raises a WPA2 access point called dg-agent-XXXX. Joining it from a
 * phone pops a browser page -- a scan list, a password box, the Deepgram API key
 * -- and all of it lands in NVS. The device then reboots into station mode.
 *
 * WHY A PORTAL AND NOT BLE
 *
 * No app to install and no pairing: anything with a browser can do it, which
 * includes every phone and laptop in the room. The board's factory firmware
 * (a xiaozhi-esp32 build) reached the same conclusion -- its flash image still
 * has the portal HTML in it.
 *
 * WHY THE AP IS ENCRYPTED, HAVING BEEN OPEN
 *
 * It was open, and the argument was sound at the time: a WPA2 AP needs its
 * passphrase shown before anyone can join, which on a 466 px round panel means
 * displaying a passphrase to protect a passphrase. The exposure was one
 * unencrypted local hop, lasting as long as it takes to fill in a form, and the
 * AP is gone after the reboot. That paragraph ended by saying if the trade ever
 * became unacceptable, this was the place to change it.
 *
 * THE API KEY IS WHAT CHANGED IT. A Wi-Fi password crossing that hop is local,
 * rotatable, and worthless to anyone not standing in the building. A Deepgram
 * API key is a bearer credential that works from anywhere on earth and bills to
 * the owner, and the portal gets used in exactly the places where that matters:
 * conferences, other people's offices, rooms full of laptops.
 *
 * The objection the original argument raised is answered rather than ignored:
 * nobody reads the passphrase off the panel, because it reaches the phone inside
 * the WIFI: QR code the setup screen already showed for joining -- see
 * enter_provisioning() in main.c. It is displayed as text underneath purely as
 * the fallback for a camera that will not act on the code.
 *
 * Note the page itself is still plain HTTP. Encrypting the link is what protects
 * the form; a self-signed certificate on a captive portal would break the
 * automatic sign-in sheet and train people to click through warnings.
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
 * Static storage, so it is safe to hand to ui_set_status(), and usable
 * before wifi_prov_start() has run.
 */
const char *wifi_prov_ap_name(void);

/*
 * The AP's WPA2 passphrase -- nine unambiguous alphanumerics, generated on first
 * call and stable for the life of the boot.
 *
 * Random per boot rather than derived from the MAC: the MAC is broadcast, so
 * anything derived from it is guessable by whoever can hear the beacon, which
 * would leave the encryption doing no work.
 *
 * Same static-storage contract as wifi_prov_ap_name(), so it is safe to hand to
 * ui_set_status() and usable before wifi_prov_start() has run.
 */
const char *wifi_prov_ap_password(void);

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
