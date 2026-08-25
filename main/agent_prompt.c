/*
 * Assembles the blocks in main/prompt/ into one system prompt. See
 * agent_prompt.h for why the words live in files and why this runs in PSRAM.
 */
#include "agent_prompt.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "agent_name.h"
#include "faces.h"
#include "orb_colors.h"
#include "voices.h"

static const char *TAG = "prompt";

/*
 * EMBED_TXTFILES names each symbol after the FILE NAME with every
 * non-alphanumeric character turned into an underscore, and appends a NUL that
 * `_end` points past -- hence the -1 in section_len().
 */
#define PROMPT_BLOCK(sym)                                                \
    extern const char sym##_start[] __asm__("_binary_" #sym "_start"); \
    extern const char sym##_end[] __asm__("_binary_" #sym "_end")

PROMPT_BLOCK(formatting_md);
PROMPT_BLOCK(identity_md);
PROMPT_BLOCK(conversation_md);
PROMPT_BLOCK(speaking_md);
PROMPT_BLOCK(substance_md);
PROMPT_BLOCK(substance_flux_md);
PROMPT_BLOCK(half_duplex_md);
PROMPT_BLOCK(barge_in_md);
PROMPT_BLOCK(boundaries_md);
PROMPT_BLOCK(session_md);

typedef struct {
    const char *start;
    const char *end;
    /* Joined to the block above with a single newline instead of a blank line,
     * which is what lets a gated block continue the list it belongs to rather
     * than starting a stray one-bullet section. */
    bool continues;
} block_t;

/*
 * THE ORDER OF THE PROMPT. Filenames carry none of it on purpose -- see the
 * header. Blocks gated on Kconfig are gated for the same reason send_settings()
 * gates its Flux fields: a prompt that describes a build you did not make is
 * worse than a shorter one, because the model states it confidently.
 */
static const block_t s_blocks[] = {
    { formatting_md_start,   formatting_md_end,   false },
    { identity_md_start,     identity_md_end,     false },
    { conversation_md_start, conversation_md_end, false },
    { speaking_md_start,     speaking_md_end,     false },
    { substance_md_start,    substance_md_end,    false },
#if CONFIG_SPEECH_STACK_FLUX
    /* Model-integrated end-of-turn detection is a Flux property; Nova-3 gets it
     * from server-side VAD, which is a different claim. Continues the list of
     * true things substance.md ends with. */
    { substance_flux_md_start, substance_flux_md_end, true },
#endif
#if CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS
    { half_duplex_md_start,  half_duplex_md_end,  false },
#else
    { barge_in_md_start,     barge_in_md_end,     false },
#endif
    { boundaries_md_start,   boundaries_md_end,   false },
    { session_md_start,      session_md_end,      false },
};

/* ---------------- {{placeholders}} ---------------- */

/* Same shape as voices_describe() and friends, so the catalogs need no wrapper:
 * write into `out`, never past `cap`, always NUL-terminated. */
typedef void (*expand_fn)(char *out, size_t cap);

static void expand_name(char *out, size_t cap)
{
    snprintf(out, cap, "%s", agent_name_get());
}

static void expand_voice(char *out, size_t cap)
{
    /* voices_find() is tolerant of the model id, so this turns "flux-hannah-en"
     * back into the short name the user would actually say. */
    const voice_t *v = voices_find(voices_current_model());
    snprintf(out, cap, "%s", (v != NULL) ? v->name : voices_current_model());
}

static void expand_listen_model(char *out, size_t cap)
{
#if CONFIG_SPEECH_STACK_FLUX
    snprintf(out, cap, "Flux");
#else
    snprintf(out, cap, "Nova-3");
#endif
}

static void expand_speak_model(char *out, size_t cap)
{
#if CONFIG_SPEECH_STACK_FLUX
    snprintf(out, cap, "Flux TTS");
#else
    snprintf(out, cap, "Aura-2");
#endif
}

static const struct {
    const char *key;
    expand_fn   fn;
} s_vars[] = {
    { "name",         expand_name },
    { "voice",        expand_voice },
    { "listen_model", expand_listen_model },
    { "speak_model",  expand_speak_model },
    /* Unused by the blocks as written, and deliberately kept: these are the
     * catalogs the function schemas already carry, here for a prompt that ever
     * wants to talk about them in prose. */
    { "voices",       voices_describe },
    { "faces",        faces_describe },
    { "colors",       orb_colors_describe },
};

/* Room for the expansions, which are not known until they run. The catalogs are
 * the big ones at a few hundred bytes each; this covers every placeholder in the
 * table firing at once, and the assembled length is logged so growth is visible
 * long before it matters. */
#define EXPANSION_SLACK 4096
/* No single placeholder may eat the whole slack. */
#define EXPANSION_MAX   1024

static expand_fn find_var(const char *key, size_t key_len)
{
    for (size_t i = 0; i < sizeof(s_vars) / sizeof(s_vars[0]); i++) {
        if (strlen(s_vars[i].key) == key_len &&
            strncmp(s_vars[i].key, key, key_len) == 0) {
            return s_vars[i].fn;
        }
    }
    return NULL;
}

/* ---------------- assembly ---------------- */

/* Trailing newlines are trimmed so the joins below decide the spacing, not
 * whichever editor last saved the file. */
static size_t block_len(const block_t *b)
{
    size_t n = (size_t)(b->end - b->start) - 1; /* the embedded NUL */
    while (n > 0 && b->start[n - 1] == '\n') {
        n--;
    }
    return n;
}

static size_t append(char *out, size_t cap, size_t len, const char *src, size_t n)
{
    if (len + 1 >= cap) {
        return len;
    }
    size_t room = cap - 1 - len;
    if (n > room) {
        n = room;
    }
    memcpy(out + len, src, n);
    out[len + n] = '\0';
    return len + n;
}

/* Copies one block, expanding {{name}} as it goes. */
static size_t append_block(char *out, size_t cap, size_t len, const block_t *b)
{
    const char *p = b->start;
    const char *stop = b->start + block_len(b);

    while (p < stop) {
        const char *open = strstr(p, "{{");
        if (open == NULL || open >= stop) {
            return append(out, cap, len, p, (size_t)(stop - p));
        }
        len = append(out, cap, len, p, (size_t)(open - p));

        const char *close = strstr(open, "}}");
        if (close == NULL || close >= stop) {
            /* Unterminated. Copy the rest verbatim rather than guessing. */
            return append(out, cap, len, open, (size_t)(stop - open));
        }

        size_t key_len = (size_t)(close - open) - 2;
        expand_fn fn = find_var(open + 2, key_len);
        if (fn != NULL && len + 1 < cap) {
            size_t room = cap - 1 - len;
            fn(out + len, (room < EXPANSION_MAX) ? room : EXPANSION_MAX);
            len += strlen(out + len);
        } else {
            /* A typo should show up in the wire log, not vanish silently. */
            ESP_LOGW(TAG, "unknown placeholder {{%.*s}}", (int)key_len, open + 2);
            len = append(out, cap, len, open, (size_t)(close + 2 - open));
        }
        p = close + 2;
    }
    return len;
}

char *agent_prompt_build(const agent_prompt_ctx_t *ctx)
{
    /* The escape hatch wins outright, so a one-line experiment in menuconfig
     * does not need a file edit -- and an empty value, the default, means the
     * files are in charge. */
    if (strlen(CONFIG_DEEPGRAM_AGENT_PROMPT) > 0) {
        ESP_LOGW(TAG, "CONFIG_DEEPGRAM_AGENT_PROMPT is set; main/prompt is ignored");
        return strdup(CONFIG_DEEPGRAM_AGENT_PROMPT);
    }

    const size_t nblocks = sizeof(s_blocks) / sizeof(s_blocks[0]);
    size_t cap = EXPANSION_SLACK + 1;
    for (size_t i = 0; i < nblocks; i++) {
        cap += block_len(&s_blocks[i]) + 2; /* + the join above it */
    }
    if (ctx != NULL && ctx->notes != NULL) {
        cap += strlen(ctx->notes) + 2;
    }

    char *out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (out == NULL) {
        ESP_LOGE(TAG, "no PSRAM for a %u byte prompt", (unsigned)cap);
        return NULL;
    }
    out[0] = '\0';

    size_t len = 0;
    for (size_t i = 0; i < nblocks; i++) {
        if (len > 0) {
            len = append(out, cap, len, "\n\n", s_blocks[i].continues ? 1 : 2);
        }
        len = append_block(out, cap, len, &s_blocks[i]);
    }
    if (ctx != NULL && ctx->notes != NULL) {
        len = append(out, cap, len, "\n\n", 2);
        len = append(out, cap, len, ctx->notes, strlen(ctx->notes));
    }

    ESP_LOGI(TAG, "system prompt: %u bytes from %u blocks (%u allocated)",
             (unsigned)len, (unsigned)nblocks, (unsigned)cap);
    return out;
}
