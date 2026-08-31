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
#include <stdint.h>

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


/*
 * Link quality of the AP the station is associated with.
 *
 * WHY THIS IS NOT A SAMPLER MODULE, unlike battery.h
 *
 * The battery lives behind an I2C transaction on a bus three other peripherals
 * share, so it earns a task of its own and a cached copy. RSSI does not: the
 * Wi-Fi driver already averages the beacons it receives and
 * esp_wifi_sta_get_ap_info() hands over the number it is holding, with no radio
 * activity and no bus. It is cheap enough to call from any reader on any
 * cadence, so there is nothing here but an accessor.
 *
 * The one thing that does need state is the bucket hysteresis -- see bars.
 */
typedef struct {
    /* False whenever there is no association to report: before the first
     * connect, after a disconnect, and for the whole time the provisioning AP
     * owns the radio. Everything below is meaningless while this is false. */
    bool    valid;
    /* Beacon RSSI in dBm, straight from the driver. Diagnostic: it is the only
     * field that tells a log reader whether a bar count is believable. */
    int8_t  rssi;
    /*
     * 0-4, hysteresed. THE ONLY FIELD A DISPLAY SHOULD READ.
     *
     * Beacon RSSI walks several dB between beacons, so bucketing the raw value
     * puts a boundary reading on the wrong side of the line every second or two
     * -- which on screen is a bar blinking in and out for as long as the device
     * sits at that distance. The promotion threshold is offset from the
     * demotion one to stop that; the direction of the asymmetry is explained at
     * the thresholds in wifi_sta.c.
     */
    uint8_t bars;
    /* bars <= 1. Hysteresed with them, and the one reading a caller is expected
     * to act on rather than merely display. */
    bool    weak;
    /* Primary channel. Diagnostic: "it only drops in the evening" is answered by
     * which channel the AP moved to, and nothing else. */
    uint8_t channel;
} wifi_signal_t;

/*
 * Reads the current link quality. Safe from any task; returns false and leaves
 * *out zeroed when the feature is compiled out or the station is not associated.
 *
 * Cheap: no scan, no bus transaction, no blocking. See the note above.
 */
bool wifi_sta_get_signal(wifi_signal_t *out);
