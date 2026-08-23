/*
 * The orb's colour catalog.
 *
 * Two audiences for one table, exactly as faces.h and voices.h describe: the
 * model gets the names as an enum in a function-call schema so it can pick one
 * out loud, and the device uses the table to validate what comes back and turn a
 * name into an index.
 *
 * WHY COLOUR IS A uint32_t AND NOT AN lv_color_t
 *
 * dg_agent.c includes this header to build the schema, and the rule that keeps
 * "nothing outside ui.c calls lv_*" true is that the catalog headers stay free of
 * LVGL. So a colour is a plain 0xRRGGBB here and stays one until orb_raster.c,
 * which is the only file that has any business knowing about pixel formats.
 *
 * ORB ONLY. The spectrum face colours its bands by frequency and by which half of
 * the conversation is live, so a single tint would destroy information rather than
 * restyle it. The setting is stored whichever face is up and shows on the orb.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

typedef struct {
    const char *name;  /* "orange" -- what the model says, and the enum value */
    uint32_t rgb;      /* 0xRRGGBB, multiplicative on the orb's ink */
    const char *blurb; /* what it looks like, for the function description, or
                        * NULL when the name already says it -- every colour goes
                        * into every session's schema, so "pink (pink)" is waste */
} orb_color_t;

/* How many colours exist. Index 0 is the boot default. */
size_t orb_colors_count(void);

/* Name for an index, or NULL if out of range. */
const char *orb_colors_name(size_t index);

/* 0xRRGGBB for an index. Out of range returns white, so a bad index degrades to
 * the default look rather than to an invisible black-on-black orb. */
uint32_t orb_colors_rgb(size_t index);

/* Case-insensitive lookup. Returns the index, or -1 if unknown. */
int orb_colors_find(const char *name);

/* Appends a JSON string array of the names, for the function schema's `enum`. */
void orb_colors_add_enum(cJSON *parent, const char *key);

/* Writes "white (the plain ...), orange (...)" for the function description,
 * because JSON Schema has nowhere to hang a per-value note. Truncates rather
 * than overflowing. */
void orb_colors_describe(char *out, size_t out_len);
