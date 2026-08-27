/*
 * The agent's persona -- the system prompt, kept OUT of dg_agent.c.
 *
 * WHY ITS OWN MODULE
 *
 *   1. dg_agent.c is about transport: opening the socket, reassembling JSON,
 *      answering function calls, keeping audio moving. A persona is product
 *      copy. Mixing the two makes both harder to read and to review.
 *   2. Prompts get iterated on constantly. A directory of text files keeps the
 *      diffs -- and the git history -- about the words, not about the client.
 *   3. Prompts for VOICE are their own genre. Every reply here is read aloud by
 *      a TTS model, so the formatting rules in prompt/formatting.md are not
 *      style preferences: they are the difference between "sure, one moment"
 *      and "star star sure star star, one moment".
 *
 * WHERE THE WORDS ARE
 *
 * main/prompt holds one .md file per named block, embedded into flash rodata by
 * EMBED_TXTFILES in main/CMakeLists.txt. They cost no RAM until assembled and
 * editing one triggers a rebuild. The ORDER is the table in agent_prompt.c, not
 * the filenames -- CMake's symbol mangling puts an extra underscore in front of
 * any name starting with a digit, so "10-formatting.md" would be a trap.
 *
 * WHY IT IS ASSEMBLED AT RUNTIME RATHER THAN CONCATENATED AT BUILD TIME
 *
 * Two of the blocks depend on things only known at runtime: the voice the user
 * last asked for, and any per-session note dg_agent wants to fold in. Blocks
 * carry {{placeholders}} for those; see s_vars in agent_prompt.c for the list.
 *
 * STACK. The result is built in PSRAM and never touched on the stack, which is
 * not a style choice -- send_settings() runs on the WebSocket task's 6144 bytes
 * and a third pair of description buffers once put the board in a boot loop.
 * See .claude/skills/esp-stack-budget/SKILL.md. agent_prompt_build()'s own frame
 * is a handful of pointers whatever the prompt grows to.
 */
#pragma once

/* Per-session facts folded into the THIS SESSION block. All fields optional;
 * NULL everywhere gives the plain persona. */
typedef struct {
    /* Extra context, already written as plain spoken-safe prose -- e.g.
     * "You have been talking for a while already; this is the same
     * conversation." Appended verbatim, so no markdown and no brackets. */
    const char *notes;
} agent_prompt_ctx_t;

/*
 * Assembles the full system prompt.
 *
 * Returns a NUL-terminated string in PSRAM that the CALLER MUST free(), or NULL
 * if the allocation failed -- callers should fall back rather than abandon the
 * session, because a session with no persona still works.
 *
 * ctx may be NULL. The files are always in charge -- the one-line Kconfig
 * override that used to pre-empt them is gone, because a forgotten override
 * looked exactly like a prompt with no effect.
 */
char *agent_prompt_build(const agent_prompt_ctx_t *ctx);
