/*
 * Deepgram Agent API client for ESP-IDF.
 *
 *   wss://agent.deepgram.com/v1/agent/converse
 *
 * The Agent endpoint takes no query parameters at all: every knob lives in a
 * JSON `Settings` message that MUST be the first thing sent after the socket
 * opens. The server ignores anything -- audio included -- that arrives before
 * it has acknowledged Settings with `SettingsApplied`, which is why this module
 * exposes a distinct "ready" state rather than just "connected".
 *
 * Message flow for a session:
 *
 *   client                              server
 *   ------                              ------
 *   (upgrade w/ Authorization header)
 *                                  <--  Welcome            {request_id}
 *   Settings {...}                 -->
 *                                  <--  SettingsApplied
 *                                  <--  ConversationText   {role:"assistant"}  (greeting)
 *                                  <--  <binary audio>                          (greeting)
 *                                  <--  AgentAudioDone
 *   <binary mic audio>             -->
 *   KeepAlive (during silence)     -->
 *                                  <--  UserStartedSpeaking / ConversationText / audio ...
 *
 * Audio in and audio out are raw binary frames -- no JSON envelope, no base64.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    DG_AGENT_DISCONNECTED,  /* socket down; the client is retrying on its own */
    DG_AGENT_CONNECTED,     /* socket up, Settings sent, not acknowledged yet */
    DG_AGENT_READY,         /* SettingsApplied received: safe to send audio */
    DG_AGENT_ERROR,         /* transport or handshake failure */
} dg_agent_state_t;

/*
 * All callbacks run on the WebSocket client's own task. Keep them short and do
 * not call dg_agent_stop() from inside one -- hand the work to another task.
 */
typedef struct {
    void (*on_state)(dg_agent_state_t state, void *ctx);

    /* One turn of the conversation. role is "user" or "assistant". */
    void (*on_conversation_text)(const char *role, const char *content, void *ctx);

    /*
     * A slice of the agent's TTS output: mono DG_AUDIO_ENCODING at
     * DG_AUDIO_SAMPLE_RATE. Large frames arrive as several calls; there is no
     * frame boundary to honour, it is a continuous PCM stream.
     */
    void (*on_audio)(const uint8_t *data, size_t len, void *ctx);

    /* The agent has finished speaking this turn. */
    void (*on_agent_audio_done)(void *ctx);

    /*
     * Barge-in. Deepgram stops generating when this fires, so any agent audio
     * still queued for playback is a reply the user has already talked over --
     * drop it here or the device keeps speaking over them.
     */
    void (*on_user_started_speaking)(void *ctx);

    void *ctx;
} dg_agent_callbacks_t;

/*
 * Audio contract, shared with whatever drives the codec.
 *
 * One rate for both directions, and it is not a stylistic choice: the ES7210
 * mic and ES8311 speaker share a single duplex I2S peripheral, so they cannot
 * be clocked differently. The Agent API would happily send 24 kHz out while
 * taking 16 kHz in; this hardware would not. 16 kHz is the rate
 * spec_analyzer_radial proved on this board.
 */
#define DG_AUDIO_ENCODING    "linear16"
#define DG_AUDIO_SAMPLE_RATE 16000

/* Opens the socket and starts the session. Returns as soon as the client task
 * is running; watch on_state (or dg_agent_is_ready) for progress. */
esp_err_t dg_agent_start(const dg_agent_callbacks_t *callbacks);

esp_err_t dg_agent_stop(void);

/* True once SettingsApplied has been received. */
bool dg_agent_is_ready(void);

/* Streams captured microphone audio. No-op unless the session is ready. */
esp_err_t dg_agent_send_audio(const void *pcm, size_t len);

/* Talks to the agent without a microphone -- useful for bring-up. */
esp_err_t dg_agent_inject_user_message(const char *text);
