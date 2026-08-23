/*
 * The orb's colour catalog. See orb_colors.h.
 *
 * ORDER IS THE CONTRACT: index 0 is the boot default and what ui.c starts with.
 * Adding a colour means appending here; nothing else has a parallel table to keep
 * in step, which is the one way this is simpler than faces.
 */

#include <strings.h>
#include <stdio.h>

#include "esp_log.h"

#include "orb_colors.h"

static const char *TAG = "orb_colors";

/*
 * The blurbs are written for the model, not for a person reading source. They
 * have to carry enough for it to resolve something indirect -- "make it warmer",
 * "put it back to normal" -- rather than only matching a colour named outright.
 *
 * White is first because it is the shell's original monochrome look, and
 * 0xFFFFFF is the rasteriser's exact identity: selecting it is genuinely a
 * return to the default, not an approximation of one.
 */
static const orb_color_t s_colors[] = {
    { "white", 0xFFFFFF,
      "the plain white dots it normally shows; the default, and the way back "
      "from any other colour" },
    { "orange", 0xFFA700,
      "a warm amber, like a filament bulb" },
    { "blue", 0x00FFFF,
      "a bright cyan, cool and electric" },
};

#define COLOR_COUNT (sizeof(s_colors) / sizeof(s_colors[0]))

size_t orb_colors_count(void)
{
    return COLOR_COUNT;
}

const char *orb_colors_name(size_t index)
{
    return (index < COLOR_COUNT) ? s_colors[index].name : NULL;
}

uint32_t orb_colors_rgb(size_t index)
{
    /* White rather than 0 on a bad index: black ink on the black ground would be
     * an invisible orb, which reads as a dead device rather than as a bug. */
    return (index < COLOR_COUNT) ? s_colors[index].rgb : 0xFFFFFFu;
}

int orb_colors_find(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < COLOR_COUNT; i++) {
        if (strcasecmp(name, s_colors[i].name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

void orb_colors_add_enum(cJSON *parent, const char *key)
{
    cJSON *arr = cJSON_AddArrayToObject(parent, key);
    if (arr == NULL) {
        return;
    }
    for (size_t i = 0; i < COLOR_COUNT; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(s_colors[i].name));
    }
}

void orb_colors_describe(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    size_t used = 0;
    out[0] = '\0';

    for (size_t i = 0; i < COLOR_COUNT; i++) {
        int n = snprintf(out + used, out_len - used, "%s%s (%s)",
                         (used > 0) ? ", " : "", s_colors[i].name, s_colors[i].blurb);
        if (n < 0 || (size_t)n >= out_len - used) {
            /* Truncated: better a short catalog than a corrupt one. */
            out[out_len - 1] = '\0';
            ESP_LOGW(TAG, "colour description truncated");
            return;
        }
        used += (size_t)n;
    }
}
