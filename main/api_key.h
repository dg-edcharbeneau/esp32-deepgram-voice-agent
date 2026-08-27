/*
 * The Deepgram API key, which outlives a reflash.
 *
 * The key used to be a compile-time constant concatenated straight into the
 * WebSocket upgrade header, which meant putting this device in someone else's
 * hands required handing over a build environment or your own key. It now lives
 * in NVS, written by the provisioning portal (wifi_prov.c) and read when the
 * agent client is initialised.
 *
 * THE PRECEDENCE RULE, which is the same one wifi_creds.h states and the same
 * one that surprises people: NVS wins. CONFIG_DEEPGRAM_API_KEY is a *first-boot
 * seed* only, applied when NVS has nothing. Once a key has been saved through
 * the portal, editing sdkconfig and reflashing appears to do nothing at all --
 * the saved key keeps winning. Wipe NVS to get the menuconfig value back.
 *
 * Keeping the seed at all is deliberate, for the same reason the Wi-Fi seed was
 * kept: an existing bench setup that has always used menuconfig does not
 * suddenly stop working.
 *
 * NOTHING HERE EVER LOGS THE KEY. Not a prefix, not a suffix. It is a bearer
 * credential with billing attached, it works from anywhere, and a serial capture
 * is the easiest thing in this project to paste into a bug report.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * Deliberately generous. Deepgram has issued keys in more than one shape, and a
 * buffer sized to today's 40-character form would silently truncate a longer one
 * into a key that fails authentication for no visible reason.
 */
#define DG_API_KEY_LEN 129

/*
 * Fills out with the key to authenticate with. Returns false when there is
 * nothing to try -- neither a saved key nor a configured seed -- which is the
 * caller's cue to refuse to open a session and say why.
 */
bool api_key_load(char out[DG_API_KEY_LEN]);

/*
 * Persists a key. Rejects NULL and empty; the portal is responsible for the
 * user-facing checks, because it is the one that can put a message on screen.
 */
esp_err_t api_key_save(const char *key);

/*
 * Whether NVS holds a key, without reading it out.
 *
 * This exists for the portal's "a key is stored -- leave blank to keep it"
 * affordance: blank has to be visibly safe before anyone will trust it.
 */
bool api_key_is_stored(void);
