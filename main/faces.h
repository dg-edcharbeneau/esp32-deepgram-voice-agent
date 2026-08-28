/*
 * The display face catalog.
 *
 * Two audiences for one table, exactly as voices.h describes for voices: the
 * model gets the names as an enum in a function-call schema so it can pick one
 * out loud, and the device uses the table to validate what comes back and turn a
 * name into an index.
 *
 * WHY THIS IS SEPARATE FROM ui.c
 *
 * dg_agent.c has to build the schema, and ui.c's face table holds function
 * pointers into LVGL drawing code. Keeping the catalog here means the WebSocket
 * side never includes LVGL -- the same split voices.c/voices.h already uses, and
 * the reason the "nothing outside ui.c calls lv_*" rule survives this feature.
 */
#pragma once

#include <stddef.h>

#include "cJSON.h"

typedef struct {
    const char *name;  /* "orb" -- what the model says, and the enum value */
    const char *blurb; /* what it looks like, for the function description */
} face_t;

/* How many faces exist. Indices are stable and match ui.c's table. */

/* Name for an index, or NULL if out of range. */
const char *faces_name(size_t index);

/* Case-insensitive lookup. Returns the index, or -1 if unknown. */
int faces_find(const char *name);

/* Appends a JSON string array of the names, for the function schema's `enum`. */
void faces_add_enum(cJSON *parent, const char *key);

/* Writes "orb (a dotted sphere that ...), spectrum (...)" for the function
 * description, because JSON Schema has nowhere to hang a per-value note.
 * Truncates rather than overflowing. */
void faces_describe(char *out, size_t out_len);
