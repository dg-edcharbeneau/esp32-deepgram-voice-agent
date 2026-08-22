/*
 * Wi-Fi credentials that outlive a reflash.
 *
 * Credentials used to be compile-time constants, which meant moving the device
 * to another network needed a toolchain. They now live in NVS, written by the
 * provisioning portal (wifi_prov.c) and read on every boot.
 *
 * THE PRECEDENCE RULE, because it is the one thing that surprises people:
 * NVS wins. CONFIG_WIFI_SSID / CONFIG_WIFI_PASSWORD are a *first-boot seed*
 * only, applied when NVS has nothing. Once anything has been provisioned,
 * editing sdkconfig and reflashing appears to do nothing at all -- the saved
 * network keeps winning. Erase it with the BOOT button, or wipe NVS, to get the
 * menuconfig values back.
 *
 * Keeping the seed at all is deliberate: it means an existing bench setup that
 * has always used menuconfig does not suddenly boot into a captive portal.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * Buffer sizes for the strings handed around above the driver.
 *
 * wifi_config_t's own fields are 32 and 64 bytes and are explicitly *not*
 * required to be NUL-terminated there -- a 32-character SSID fills the array
 * completely. Everything in this project treats them as C strings, so the
 * buffers here carry the extra byte the driver's do not.
 */
#define WIFI_CREDS_SSID_LEN 33
#define WIFI_CREDS_PASS_LEN 65

/*
 * Fills ssid/pass with the network to join. Returns false when there is nothing
 * to try -- neither a saved network nor a configured seed -- which is the
 * caller's cue to start provisioning.
 *
 * An empty password is a legitimate result: it means an open network.
 */
bool wifi_creds_load(char ssid[WIFI_CREDS_SSID_LEN], char pass[WIFI_CREDS_PASS_LEN]);

/* Persists a network. pass may be NULL or "" for an open network. */
esp_err_t wifi_creds_save(const char *ssid, const char *pass);

/*
 * Forgets the saved network. The next boot falls back to the menuconfig seed if
 * one is set, and to provisioning if not.
 *
 * Erasing something that was never written is success, not an error.
 */
esp_err_t wifi_creds_erase(void);
