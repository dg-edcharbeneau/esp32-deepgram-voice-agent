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
    /*
     * The server rejected the API key (HTTP 401 on the upgrade). Distinct from
     * DG_AGENT_ERROR because it is the one failure retrying cannot fix: the
     * client would otherwise reconnect every 5 s against a key that will never
     * work, and the panel would say "error" as though the network were at fault.
     *
     * Whoever observes this is expected to stop the session -- from another
     * task, per the note below.
     */
    DG_AGENT_BAD_KEY,
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

    /*
     * The session must be reopened to pick up a setting change.
     *
     * UpdateSpeak is the documented way to change the voice in place, but it
     * returns SpeakUpdated and then does nothing, so the only mechanism that
     * actually works is a new Settings message -- which means a new session.
     * Runs on the WebSocket task, which cannot stop the client itself, so the
     * handler must hand the work to another task.
     */
    void (*on_reload_required)(void *ctx);

    /*
     * The user asked for the display test and the agent has finished saying so.
     *
     * Deferred to AgentAudioDone for the same reason the reload is: the session
     * is about to be closed, and closing it mid-sentence cuts the confirmation
     * off. Runs on the WebSocket task, which cannot stop the client itself, so
     * the handler must hand the work to another task.
     */
    void (*on_display_test_required)(void *ctx);

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

/*
 * One-time setup: allocates the client and its reassembly buffer, registers the
 * event handler, and starts the keepalive task. Call once at boot.
 *
 * Split from dg_agent_start() so that stopping and restarting a conversation
 * reuses one long-lived client handle. That is a supported path -- the client
 * re-initialises its transport list on every start -- and it means `s_client` is
 * never dangling, which is what makes dg_agent_send_audio() safe to call from
 * the capture task without a lock. Tearing the client down and rebuilding it per
 * session would need one, and any lock ordered outside the client's own mutex
 * deadlocks against the WebSocket task.
 */
esp_err_t dg_agent_init(const dg_agent_callbacks_t *callbacks);

/* Opens the socket and starts a session. Returns as soon as the client task is
 * running; watch on_state (or dg_agent_is_ready) for progress. Safe to call
 * again after dg_agent_stop() -- the new session is a genuinely new one, so the
 * agent has no memory of the previous conversation. */
esp_err_t dg_agent_start(void);

/*
 * Closes the session. The client handle survives for the next dg_agent_start().
 *
 * Do not call from a WebSocket callback: the client compares the calling task
 * against its own and refuses. Hand the work to another task.
 */
esp_err_t dg_agent_stop(void);

/*
 * Silences on_state while a teardown is in progress.
 *
 * Closing the socket raises DISCONNECTED, which would otherwise overwrite a
 * "stopping"/"stopped" message on the display with "disconnected". Keeping the
 * suppression here rather than latching it in the caller puts the policy where
 * the events are generated, and removes an ordering race between the two.
 */
void dg_agent_suppress_state_events(bool suppress);

/* True once SettingsApplied has been received. */
bool dg_agent_is_ready(void);

/*
 * Frames the uplink queue had to drop because the socket could not keep up,
 * cumulative since boot.
 *
 * Distinct from transport_ws.c's own "send queue full" count one layer down:
 * that one is a frame the socket refused, this one is a frame that never got
 * offered because the queue ahead of it was still full. Both mean the same
 * thing about the uplink; this is the earlier and cheaper signal.
 */
uint32_t dg_agent_audio_dropped(void);

/* Streams captured microphone audio. No-op unless the session is ready. */
esp_err_t dg_agent_send_audio(const void *pcm, size_t len);

/* Talks to the agent without a microphone -- useful for bring-up. */
esp_err_t dg_agent_inject_user_message(const char *text);

/*
 * Forget the conversation so far.
 *
 * dg_agent keeps the last few turns and replays them into the next session's
 * Settings, so reopening the socket -- to change a setting, or after the
 * network drops -- resumes the conversation instead of starting over. Call this
 * when the user deliberately ends a conversation, and only then.
 */
void dg_agent_clear_history(void);
