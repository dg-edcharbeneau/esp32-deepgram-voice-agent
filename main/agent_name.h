/*
 * What the agent is called, and the second setting this device remembers.
 *
 * Same shape and the same precedence rule as voices.h: CONFIG_AGENT_NAME is the
 * FACTORY DEFAULT, used on first boot and after an NVS erase, and a name the
 * user has asked for wins on every later boot until reset_name puts it back.
 *
 * The name reaches the model through {{name}} in the persona rather than a
 * function schema, which is why nothing here needs cJSON: it is one string, not
 * a catalog with two audiences.
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

/* Longest name accepted, NUL included. Deliberately short: it is spoken aloud,
 * and it is interpolated into the system prompt from something a stranger said
 * out loud, so the cap is also what keeps a "name" from being a paragraph of
 * instructions. */
#define AGENT_NAME_MAX 32

/* Reads the saved name, falling back to CONFIG_AGENT_NAME. Call once at boot,
 * before the first session. */
void agent_name_init(void);

/* The name in use. Never NULL, never empty. */
const char *agent_name_get(void);

/* The name the device falls back to. */
const char *agent_name_default(void);

/*
 * Adopt a name and persist it.
 *
 * Trims surrounding whitespace, then rejects anything empty, longer than
 * AGENT_NAME_MAX, or carrying a control character -- a newline would let a
 * spoken "name" forge a heading in the assembled prompt. ESP_ERR_INVALID_ARG
 * says the name was refused and NOTHING changed, which is what the agent turns
 * into an explanation.
 *
 * ESP_OK means it is in use. A later NVS failure is logged and still ESP_OK-ish
 * in spirit -- see the note in the implementation -- because losing the name at
 * the next boot is worth a warning, not a refusal.
 */
esp_err_t agent_name_set(const char *name);

/* Forget the saved name and go back to the Kconfig default. */
esp_err_t agent_name_reset(void);
