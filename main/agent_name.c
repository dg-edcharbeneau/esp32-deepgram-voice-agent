#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "agent_name.h"

static const char *TAG = "agent_name";

/* The namespace voices.c already owns; both are settings of the same device. */
#define NVS_NAMESPACE "dgagent"
#define NVS_KEY_NAME  "agent_name"

/* A copy, not a pointer, for the reason voices.c spells out: the name arrives
 * inside a cJSON tree that is freed as soon as the message is handled. */
static char s_current[AGENT_NAME_MAX] = CONFIG_AGENT_NAME;

const char *agent_name_default(void)
{
    /* An empty Kconfig value is a build-time typo, but a nameless agent would
     * put "Your name is ." in the prompt, so it never gets that far. */
    return (CONFIG_AGENT_NAME[0] != '\0') ? CONFIG_AGENT_NAME : "Grammer";
}

const char *agent_name_get(void)
{
    return s_current;
}

/*
 * Copies `in` into `out` with the surrounding whitespace gone, and says whether
 * what is left is usable as a name.
 *
 * Speech-to-text is why this is stricter than it looks: the model is passing on
 * something it HEARD, so it arrives with stray punctuation, a leading "uh", or
 * the whole sentence when the model gets lazy. Length is what catches the last
 * one, and it fails loudly rather than storing a paragraph.
 */
static bool sanitise(const char *in, char *out, size_t out_len)
{
    if (in == NULL) {
        return false;
    }
    while (*in != '\0' && isspace((unsigned char)*in)) {
        in++;
    }
    size_t len = strlen(in);
    while (len > 0 && isspace((unsigned char)in[len - 1])) {
        len--;
    }
    if (len == 0 || len >= out_len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        /* Control characters only. Anything printable is somebody's name
         * somewhere, and refusing punctuation would refuse half of them. */
        if (iscntrl((unsigned char)in[i])) {
            return false;
        }
    }
    memcpy(out, in, len);
    out[len] = '\0';
    return true;
}

void agent_name_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* NOT_FOUND means the namespace has never been written, which is every
         * device until the first voice or name change. */
        strlcpy(s_current, agent_name_default(), sizeof(s_current));
        ESP_LOGI(TAG, "no saved name (%s), using default %s",
                 esp_err_to_name(err), s_current);
        return;
    }

    char saved[AGENT_NAME_MAX];
    size_t len = sizeof(saved);
    err = nvs_get_str(h, NVS_KEY_NAME, saved, &len);
    nvs_close(h);

    char clean[AGENT_NAME_MAX];
    if (err != ESP_OK || !sanitise(saved, clean, sizeof(clean))) {
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "reading saved name failed: %s", esp_err_to_name(err));
        }
        /* Also the path for a stored name written by an older, laxer build. */
        strlcpy(s_current, agent_name_default(), sizeof(s_current));
        ESP_LOGI(TAG, "name: %s (default)", s_current);
        return;
    }

    strlcpy(s_current, clean, sizeof(s_current));
    ESP_LOGI(TAG, "name: %s (saved)", s_current);
}

esp_err_t agent_name_set(const char *name)
{
    char clean[AGENT_NAME_MAX];
    if (!sanitise(name, clean, sizeof(clean))) {
        ESP_LOGW(TAG, "refused name \"%s\"", (name != NULL) ? name : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_current, clean, sizeof(s_current));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_NAME, clean);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }

    if (err != ESP_OK) {
        /* The session keeps the new name either way -- only the next boot loses
         * it, which is worth a warning but not a refusal. Same call voices.c
         * makes, and the agent has already been told the change worked. */
        ESP_LOGW(TAG, "could not persist name: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "name saved: %s", clean);
    }
    return ESP_OK;
}

esp_err_t agent_name_reset(void)
{
    strlcpy(s_current, agent_name_default(), sizeof(s_current));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, NVS_KEY_NAME);
    /* Nothing saved is the same outcome as erasing it. */
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    ESP_LOGI(TAG, "name reset to default: %s", s_current);
    return err;
}
