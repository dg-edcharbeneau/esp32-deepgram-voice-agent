/*
 * Minimal Wi-Fi station bring-up.
 *
 * The reference project called esp_wifi_connect() and then slept 3 seconds,
 * which silently races: on a slow AP the WebSocket then opens with no route and
 * fails the DNS lookup. This module blocks on the real GOT_IP event instead.
 */
#pragma once

#include "esp_err.h"

/* Brings up the netif/event/wifi stack and starts connecting. Non-blocking. */
esp_err_t wifi_sta_start(void);

/*
 * Blocks until the station has an IPv4 address, or until the driver has burned
 * through CONFIG_WIFI_MAX_RETRY association attempts.
 *
 * Returns ESP_OK once connected, ESP_ERR_TIMEOUT if timeout_ms elapsed first,
 * ESP_FAIL if the retry budget ran out (wrong SSID, wrong password, no AP).
 */
esp_err_t wifi_sta_wait_connected(int timeout_ms);
