/* Host stub: the real one pulls in cJSON, which agent_prompt.c never uses. */
#pragma once
#include <stddef.h>
typedef struct { const char *name; const char *model; const char *blurb; int featured; } voice_t;
const voice_t *voices_find(const char *name);
const char *voices_current_model(void);
void voices_describe(char *out, size_t out_len);
