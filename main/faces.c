/*
 * The face catalog. See faces.h.
 *
 * ORDER IS THE CONTRACT: these indices are what ui.c's face table uses and what
 * CONFIG_UI_DEFAULT_FACE selects. Adding a face means appending here and
 * appending to s_faces[] in ui.c, in the same order.
 */

#include <strings.h>
#include <stdio.h>

#include "esp_log.h"

#include "faces.h"

static const char *TAG = "faces";

/*
 * The blurbs are written for the model, not for a person reading source. They
 * have to be enough for it to pick correctly when someone says something
 * indirect -- "show me the bars", "go back to the ball" -- rather than naming a
 * face outright.
 */
static const face_t s_faces[] = {
    { "orb",
      "a dotted sphere that gathers inward while you speak and radiates outward "
      "while I reply; calm and abstract" },
    { "spectrum",
      "a radial audio spectrum analyser, coloured bars fanned around the screen "
      "that jump with the sound; technical looking" },
};

#define FACE_COUNT (sizeof(s_faces) / sizeof(s_faces[0]))

size_t faces_count(void)
{
    return FACE_COUNT;
}

const char *faces_name(size_t index)
{
    return (index < FACE_COUNT) ? s_faces[index].name : NULL;
}

int faces_find(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < FACE_COUNT; i++) {
        if (strcasecmp(name, s_faces[i].name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

void faces_add_enum(cJSON *parent, const char *key)
{
    cJSON *arr = cJSON_AddArrayToObject(parent, key);
    if (arr == NULL) {
        return;
    }
    for (size_t i = 0; i < FACE_COUNT; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(s_faces[i].name));
    }
}

void faces_describe(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    size_t used = 0;
    out[0] = '\0';

    for (size_t i = 0; i < FACE_COUNT; i++) {
        int n = snprintf(out + used, out_len - used, "%s%s (%s)",
                         (used > 0) ? ", " : "", s_faces[i].name, s_faces[i].blurb);
        if (n < 0 || (size_t)n >= out_len - used) {
            /* Truncated: better a short catalog than a corrupt one. */
            out[out_len - 1] = '\0';
            ESP_LOGW(TAG, "face description truncated");
            return;
        }
        used += (size_t)n;
    }
}
