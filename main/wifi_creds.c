#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "wifi_creds.h"

static const char *TAG = "wifi_creds";

#define NVS_NAMESPACE "wifi"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"

/* Copies the menuconfig seed, if there is one. See the precedence note in the
 * header: this only runs when NVS has nothing to say. */
static bool load_seed(char ssid[WIFI_CREDS_SSID_LEN], char pass[WIFI_CREDS_PASS_LEN])
{
    if (strlen(CONFIG_WIFI_SSID) == 0) {
        return false;
    }
    strlcpy(ssid, CONFIG_WIFI_SSID, WIFI_CREDS_SSID_LEN);
    strlcpy(pass, CONFIG_WIFI_PASSWORD, WIFI_CREDS_PASS_LEN);
    ESP_LOGI(TAG, "using menuconfig seed \"%s\" (nothing saved yet)", ssid);
    return true;
}

bool wifi_creds_load(char ssid[WIFI_CREDS_SSID_LEN], char pass[WIFI_CREDS_PASS_LEN])
{
    ssid[0] = '\0';
    pass[0] = '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* NOT_FOUND means the namespace has never been written, which is every
         * device until the first provision -- not a fault. */
        return load_seed(ssid, pass);
    }

    size_t len = WIFI_CREDS_SSID_LEN;
    err = nvs_get_str(h, NVS_KEY_SSID, ssid, &len);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "reading saved ssid failed: %s", esp_err_to_name(err));
        }
        nvs_close(h);
        ssid[0] = '\0';
        return load_seed(ssid, pass);
    }

    /* A missing password is normal: that is how an open network is stored. */
    len = WIFI_CREDS_PASS_LEN;
    if (nvs_get_str(h, NVS_KEY_PASS, pass, &len) != ESP_OK) {
        pass[0] = '\0';
    }
    nvs_close(h);

    if (ssid[0] == '\0') {
        /* Defensive: a saved-but-empty SSID would otherwise send the driver off
         * to associate with nothing and burn the whole retry budget first. */
        return load_seed(ssid, pass);
    }

    ESP_LOGI(TAG, "saved network \"%s\"", ssid);
    return true;
}

esp_err_t wifi_creds_save(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(h, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_PASS, pass != NULL ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        /* Worth shouting about: unlike a failed voice save, the user cannot tell
         * this went wrong until the device fails to come back after its reboot. */
        ESP_LOGE(TAG, "could not persist credentials: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "saved network \"%s\"", ssid);
    }
    return err;
}

esp_err_t wifi_creds_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(h, NVS_KEY_SSID);
    /* Nothing saved is the same outcome as erasing it. */
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        esp_err_t perr = nvs_erase_key(h, NVS_KEY_PASS);
        if (perr != ESP_OK && perr != ESP_ERR_NVS_NOT_FOUND) {
            err = perr;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    ESP_LOGI(TAG, "credentials erased (%s)", esp_err_to_name(err));
    return err;
}
