/*
 * Flux TTS voice catalog, and the one setting this device remembers.
 *
 * Two audiences for the same table. The model gets the `featured` subset as an
 * enum in a function-call schema, so it can pick a voice by name. The device
 * uses the whole table to validate what comes back and to turn a short name
 * into the model id that goes on the wire.
 *
 * WHY THE VOICE IS NOT JUST A KCONFIG STRING ANY MORE
 *
 * Changing it costs nothing at runtime: the Agent API takes an `UpdateSpeak`
 * message mid-session, so no socket is torn down and no conversation is lost.
 * And because dg_agent re-sends Settings on every WebSocket connect, a runtime
 * value survives reconnects and session restarts on its own. NVS is only needed
 * to carry it across a reboot.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

typedef struct {
    const char *name;   /* "hannah" -- the short name the model chooses by */
    const char *model;  /* "flux-hannah-en" -- what goes on the wire */
    const char *blurb;  /* "American woman, clear and confident" */
    bool featured;      /* offered to the model, vs catalog-only */
} voice_t;

/* Reads the saved voice, falling back to CONFIG_DEEPGRAM_FLUX_VOICE. Call once
 * at boot, before the first session. */
void voices_init(void);

/* Case-insensitive lookup by short name, and tolerant of the model id too, so
 * a model that says "flux-hannah-en" instead of "hannah" still works. NULL if
 * the name is not in the catalog. */
const voice_t *voices_find(const char *name);

/* The model id for the active voice -- what send_settings() and UpdateSpeak
 * both put on the wire. Never NULL. */
const char *voices_current_model(void);

/* Adopt a voice and persist it. */
esp_err_t voices_set(const voice_t *voice);

/* Forget the saved voice and go back to the Kconfig default. */
esp_err_t voices_reset(void);

/* The default the device falls back to. */
const voice_t *voices_default(void);

/* Appends a JSON string array of the featured short names, for the function
 * schema's `enum`. */
void voices_add_enum(cJSON *parent, const char *key);

/* Writes "hannah (American woman, clear and confident), kit (...), ..." for the
 * function description, so the model knows what each voice sounds like.
 * Truncates rather than overflowing. */
void voices_describe(char *out, size_t out_len);
