#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "nvs.h"

#include "voices.h"

static const char *TAG = "voices";

/* Both inside NVS's 16-char limit for a namespace and a key. */
#define NVS_NAMESPACE "dgagent"
#define NVS_KEY_VOICE "tts_voice"

/* Longest model id is "flux-brittany-en" at 16 chars. */
#define VOICE_MODEL_MAX 24

/*
 * The catalog, from Deepgram's Flux TTS voice list.
 *
 * `featured` marks the thirteen Deepgram calls its strongest all-rounders, and
 * only those are offered to the model -- the enum and its descriptions ride in
 * every Settings message, so the whole list of thirty-six would be a couple of
 * kilobytes on the wire per reconnect and a harder choice for the LLM. The rest
 * stay here because they cost only rodata, they are still selectable as the
 * Kconfig default, and widening the offer later is then a one-flag change.
 *
 * The model id is spelled out rather than derived from the name. It is what
 * goes on the wire, and one voice that ever breaks the flux-<name>-en pattern
 * would turn a derivation into a silent bug.
 */
static const voice_t s_voices[] = {
    /* Featured */
    { "hannah",   "flux-hannah-en",   "American woman, clear and confident",      true  },
    { "kit",      "flux-kit-en",      "British man, friendly and energetic",      true  },
    { "alexis",   "flux-alexis-en",   "American woman, professional and calm",    true  },
    { "cliff",    "flux-cliff-en",    "American man, deep and raspy",             true  },
    { "sienna",   "flux-sienna-en",   "American woman, warm and professional",    true  },
    { "cole",     "flux-cole-en",     "American man, engaging and energetic",     true  },
    { "brooke",   "flux-brooke-en",   "American woman, fast and confident",       true  },
    { "colin",    "flux-colin-en",    "British man, warm and authoritative",      true  },
    { "gemma",    "flux-gemma-en",    "British woman, kind and approachable",     true  },
    { "haley",    "flux-haley-en",    "American woman, calm and empathetic",      true  },
    { "heather",  "flux-heather-en",  "American woman, energetic and engaging",   true  },
    { "miles",    "flux-miles-en",    "American man, calm and sincere",           true  },
    { "sean",     "flux-sean-en",     "British man, kind and calming",            true  },

    /* Catalog only -- selectable as the Kconfig default, not offered to the
     * model. This is where most of the accent range lives. */
    { "bree",     "flux-bree-en",     "American woman, sweet and kind",           false },
    { "brittany", "flux-brittany-en", "American woman, soft and confident",       false },
    { "bruce",    "flux-bruce-en",    "American man, natural and engaged",        false },
    { "conor",    "flux-conor-en",    "British man, deep and relaxed",            false },
    { "donovan",  "flux-donovan-en",  "American man, professional and thoughtful",false },
    { "drew",     "flux-drew-en",     "American man, young and relaxed",          false },
    { "elise",    "flux-elise-en",    "American woman, professional and caring",  false },
    { "jack",     "flux-jack-en",     "British man, clear and professional",      false },
    { "kai",      "flux-kai-en",      "Singaporean man, calm and knowledgeable",  false },
    { "kelsey",   "flux-kelsey-en",   "American woman, calm and empathetic",      false },
    { "maeve",    "flux-maeve-en",    "Irish woman, energetic and gentle",        false },
    { "marcelo",  "flux-marcelo-en",  "Filipino man, calm and knowledgeable",     false },
    { "marcus",   "flux-marcus-en",   "American man, smooth and helpful",         false },
    { "meena",    "flux-meena-en",    "Indian woman, empathetic and reassuring",  false },
    { "meghan",   "flux-meghan-en",   "American woman, energetic and kind",       false },
    { "naveen",   "flux-naveen-en",   "Indian man, clear and knowledgeable",      false },
    { "paige",    "flux-paige-en",    "American woman, calm and comfortable",     false },
    { "priya",    "flux-priya-en",    "Indian woman, confident and reassuring",   false },
    { "rufus",    "flux-rufus-en",    "British man, gentle and enthusiastic",     false },
    { "sharon",   "flux-sharon-en",   "Australian woman, formal and relaxed",     false },
    { "tanner",   "flux-tanner-en",   "British man, professional and calm",       false },
    { "wade",     "flux-wade-en",     "American man, warm and enthusiastic",      false },
    { "wes",      "flux-wes-en",      "American man, thoughtful and warm",        false },
};

#define VOICE_COUNT (sizeof(s_voices) / sizeof(s_voices[0]))

/*
 * A copy, not a pointer. The name arrives inside a cJSON tree that is freed as
 * soon as the message is handled, so the borrowed-literal trick used for the
 * display status string would leave this dangling.
 */
static char s_current[VOICE_MODEL_MAX] = CONFIG_DEEPGRAM_FLUX_VOICE;

const voice_t *voices_find(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < VOICE_COUNT; i++) {
        /* Either spelling: the model is told to use short names, but it has the
         * full ids in front of it too and sometimes echoes those back. */
        if (strcasecmp(name, s_voices[i].name) == 0 ||
            strcasecmp(name, s_voices[i].model) == 0) {
            return &s_voices[i];
        }
    }
    return NULL;
}

const voice_t *voices_default(void)
{
    const voice_t *v = voices_find(CONFIG_DEEPGRAM_FLUX_VOICE);
    /* A Kconfig value outside the catalog is a build-time typo, but falling
     * back to the first entry keeps the device talking. */
    return (v != NULL) ? v : &s_voices[0];
}

const char *voices_current_model(void)
{
    return s_current;
}

void voices_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* NOT_FOUND here means the namespace has never been written, which is
         * every device until the first voice change. */
        ESP_LOGI(TAG, "no saved voice (%s), using default %s",
                 esp_err_to_name(err), voices_default()->model);
        strlcpy(s_current, voices_default()->model, sizeof(s_current));
        return;
    }

    char saved[VOICE_MODEL_MAX];
    size_t len = sizeof(saved);
    err = nvs_get_str(h, NVS_KEY_VOICE, saved, &len);
    nvs_close(h);

    const voice_t *v = (err == ESP_OK) ? voices_find(saved) : NULL;
    if (v == NULL) {
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "reading saved voice failed: %s", esp_err_to_name(err));
        }
        /* Also the path for a stored voice Deepgram has since retired. */
        strlcpy(s_current, voices_default()->model, sizeof(s_current));
        ESP_LOGI(TAG, "voice: %s (default)", s_current);
        return;
    }

    strlcpy(s_current, v->model, sizeof(s_current));
    ESP_LOGI(TAG, "voice: %s (saved)", s_current);
}

esp_err_t voices_set(const voice_t *voice)
{
    if (voice == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_current, voice->model, sizeof(s_current));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(h, NVS_KEY_VOICE, voice->model);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        /* The session keeps the new voice either way -- only the next boot
         * loses it, which is worth a warning but not a failure. */
        ESP_LOGW(TAG, "could not persist voice: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "voice saved: %s", voice->model);
    }
    return err;
}

esp_err_t voices_reset(void)
{
    strlcpy(s_current, voices_default()->model, sizeof(s_current));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, NVS_KEY_VOICE);
    /* Nothing saved is the same outcome as erasing it. */
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    ESP_LOGI(TAG, "voice reset to default: %s", s_current);
    return err;
}

void voices_add_enum(cJSON *parent, const char *key)
{
    cJSON *arr = cJSON_AddArrayToObject(parent, key);
    if (arr == NULL) {
        return;
    }
    for (size_t i = 0; i < VOICE_COUNT; i++) {
        if (s_voices[i].featured) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(s_voices[i].name));
        }
    }
}

void voices_describe(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    size_t used = 0;
    out[0] = '\0';

    for (size_t i = 0; i < VOICE_COUNT; i++) {
        if (!s_voices[i].featured) {
            continue;
        }
        int n = snprintf(out + used, out_len - used, "%s%s (%s)",
                         (used > 0) ? ", " : "", s_voices[i].name, s_voices[i].blurb);
        if (n < 0 || (size_t)n >= out_len - used) {
            /* Truncated: better a short catalog than a corrupt one. */
            out[out_len - 1] = '\0';
            ESP_LOGW(TAG, "voice description truncated");
            return;
        }
        used += (size_t)n;
    }
}
