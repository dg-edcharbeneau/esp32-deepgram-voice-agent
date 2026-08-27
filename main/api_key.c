#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "api_key.h"

static const char *TAG = "api_key";

/* Its own namespace rather than sharing "wifi": the two are erased on different
 * occasions -- forgetting a network must not cost the key. */
#define NVS_NAMESPACE "deepgram"
#define NVS_KEY_API   "apikey"

/* Copies the menuconfig seed, if there is one. See the precedence note in the
 * header: this only runs when NVS has nothing to say. */
static bool load_seed(char out[DG_API_KEY_LEN])
{
    if (strlen(CONFIG_DEEPGRAM_API_KEY) == 0) {
        return false;
    }
    strlcpy(out, CONFIG_DEEPGRAM_API_KEY, DG_API_KEY_LEN);
    ESP_LOGI(TAG, "using the menuconfig seed (nothing saved yet)");
    return true;
}

bool api_key_load(char out[DG_API_KEY_LEN])
{
    out[0] = '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* NOT_FOUND means the namespace has never been written, which is every
         * device until the first provision -- not a fault. */
        return load_seed(out);
    }

    size_t len = DG_API_KEY_LEN;
    err = nvs_get_str(h, NVS_KEY_API, out, &len);
    nvs_close(h);

    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            /* Worth a word: a key that is present but unreadable looks exactly
             * like no key at all from here, and the two want different fixes. */
            ESP_LOGW(TAG, "reading the saved key failed: %s", esp_err_to_name(err));
        }
        out[0] = '\0';
        return load_seed(out);
    }

    if (out[0] == '\0') {
        /* Defensive, mirroring wifi_creds_load(): a saved-but-empty key would
         * otherwise be handed to the client as a valid credential and fail
         * authentication instead of falling back to the seed. */
        return load_seed(out);
    }

    /* The LENGTH, never the key -- it is the one number that distinguishes a
     * truncated paste from a rejected one, and it reveals nothing. */
    ESP_LOGI(TAG, "using the saved key (%u characters)", (unsigned)strlen(out));
    return true;
}

esp_err_t api_key_save(const char *key)
{
    if (key == NULL || key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(key) >= DG_API_KEY_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(h, NVS_KEY_API, key);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        /* Same reasoning as wifi_creds_save(): the user cannot tell this went
         * wrong until the device fails to authenticate after its reboot. */
        ESP_LOGE(TAG, "could not persist the api key: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "api key saved (%u characters)", (unsigned)strlen(key));
    }
    return err;
}

bool api_key_is_stored(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    /* Size query only -- a NULL destination asks nvs how big the value is
     * without copying the key anywhere it does not need to be. */
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, NVS_KEY_API, NULL, &len);
    nvs_close(h);

    /* len counts the NUL, so a stored empty string is 1 and does not count. */
    return err == ESP_OK && len > 1;
}
