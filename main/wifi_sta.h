/*
 * Minimal Wi-Fi station bring-up.
 *
 * The reference project called esp_wifi_connect() and then slept 3 seconds,
 * which silently races: on a slow AP the WebSocket then opens with no route and
 * fails the DNS lookup. This module blocks on the real GOT_IP event instead.
 *
 * The credentials are no longer compile-time constants -- see wifi_creds.h for
 * where they come from and wifi_prov.h for how they get there.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * Brings up netif, the default event loop and the Wi-Fi driver, and registers
 * the event handlers. Does not touch the radio.
 *
 * Separate from wifi_sta_start() because the provisioning AP needs exactly the
 * same stack underneath it, and the pieces here are all one-shot:
 * esp_event_loop_create_default() returns ESP_ERR_INVALID_STATE the second time.
 * Calling this twice is harmless -- it returns ESP_OK and does nothing.
 */
esp_err_t wifi_stack_init(void);

/*
 * Starts associating with the given network. Non-blocking.
 *
 * pass may be NULL or "" for an open network, which also selects the open
 * authmode threshold -- a WPA2 threshold would reject open APs outright.
 */
esp_err_t wifi_sta_start(const char *ssid, const char *pass);

/*
 * Blocks until the station has an IPv4 address, or until the driver has burned
 * through CONFIG_WIFI_MAX_RETRY association attempts.
 *
 * Returns ESP_OK once connected, ESP_ERR_TIMEOUT if timeout_ms elapsed first,
 * ESP_FAIL if the retry budget ran out (wrong SSID, wrong password, no AP).
 */
esp_err_t wifi_sta_wait_connected(int timeout_ms);

/*
 * Stops the station so something else can have the radio.
 *
 * Also clears the "we are trying to associate" intent, which is what keeps the
 * reconnect logic from fighting the provisioning AP: esp_wifi_start() in AP
 * mode still raises WIFI_EVENT_STA_START, and without this the handler would
 * cheerfully call esp_wifi_connect() underneath the portal.
 */
esp_err_t wifi_sta_stop(void);

/*
 * Turn Wi-Fi modem sleep on or off.
 *
 * On while the device is asleep, off while a session is live -- modem sleep adds
 * tens of ms of jitter, which matters to a 16 kHz audio stream and to nothing
 * else. Safe from any task; failures are logged rather than returned, because
 * there is no useful way for a caller to react.
 */
void wifi_sta_set_power_save(bool enabled);

