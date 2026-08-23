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
 * The Vira palette's accent colours. Names are the palette's, lowercased to match
 * the other catalogs.
 *
 * WHITE IS FIRST, NOT WHERE THE PALETTE PUTS IT. Index 0 is the boot default, and
 * 0xFFFFFF is the rasteriser's exact identity -- so keeping it first is what makes
 * the device's out-of-box look unchanged and makes "put it back to normal" a real
 * return rather than an approximation. The remaining thirteen are in palette order.
 *
 * NAMES ARE NOT ALL THE PALETTE'S. Its "acid lime" is plain lime here, its "lime"
 * is green, its "bright teal" is teal, and its muted #80cbc4 teal is dropped
 * entirely. The palette names three greens and two teals by relative intensity,
 * which is fine to read off a swatch and poor to say out loud: "lime" and "acid
 * lime" are a coin toss spoken aloud, and the pair only existed to tell each other
 * apart. One name per colour a person would actually ask for.
 *
 * BLURBS ONLY WHERE THE NAME DOES NOT CARRY IT. All thirteen go into every
 * session's function schema, so describing "pink" as pink is tokens for nothing.
 * They are kept for the brand name the model cannot know, and where a colour is
 * not quite what its name suggests -- lime here is yellow-green, and teal sits
 * close enough to cyan to be worth separating.
 */
static const orb_color_t s_colors[] = {
    { "white",       0xFFFFFF, "the plain white dots it normally shows; the "
                               "default, and the way back from any other colour" },
    { "vira",        0xE9A581, "a soft peach-coral" },
    { "tomato",      0xF85044, "a warm red" },
    { "orange",      0xFF7042, NULL },
    { "yellow",      0xFFCF3D, NULL },
    { "lime",        0xC6FF00, "a sharp yellow-green" },
    { "green",       0x39EA5F, NULL },
    { "teal",        0x64FFDA, "a vivid aqua" },
    { "cyan",        0x57D7FF, NULL },
    { "blue",        0x5393FF, NULL },
    { "indigo",      0x758AFF, "a blue-violet" },
    { "purple",      0xB54DFF, NULL },
    { "pink",        0xFF669E, NULL },
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
        const char *sep = (used > 0) ? ", " : "";
        int n = (s_colors[i].blurb != NULL)
                    ? snprintf(out + used, out_len - used, "%s%s (%s)",
                               sep, s_colors[i].name, s_colors[i].blurb)
                    : snprintf(out + used, out_len - used, "%s%s",
                               sep, s_colors[i].name);
        if (n < 0 || (size_t)n >= out_len - used) {
            /* Truncated: better a short catalog than a corrupt one. */
            out[out_len - 1] = '\0';
            ESP_LOGW(TAG, "colour description truncated");
            return;
        }
        used += (size_t)n;
    }
}
