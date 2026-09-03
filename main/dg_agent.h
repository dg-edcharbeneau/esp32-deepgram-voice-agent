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
 *
 * That other count is dg_agent_transport_dropped() below. It used to be
 * unreachable, which made this the only one on the TLM line and the line
 * misleading -- a device shedding 7% of its audio at the socket read updrop=0.
 */
uint32_t dg_agent_audio_dropped(void);

/*
 * The other half: frames the TRANSPORT dropped because the socket was not
 * writable inside its deadline, cumulative since boot.
 *
 * Forwarded from transport_ws.c's local patch rather than read directly by
 * main.c, so the telemetry loop keeps taking its uplink numbers from one place
 * and nothing above this layer has to know the transport is patched at all.
 *
 * Read them as a pair. updrop rising means the queue never drained; txdrop
 * rising means it drained into a socket that would not take it. The second is
 * the network, and it is the one that was invisible.
 */
uint32_t dg_agent_transport_dropped(void);

/* Streams captured microphone audio. No-op unless the session is ready. */
esp_err_t dg_agent_send_audio(const void *pcm, size_t len);


/*
 * Forget the conversation so far, in RAM and (once the worker next runs) on
 * flash.
 *
 * dg_agent keeps the turns and replays them into the next session's Settings, so
 * reopening the socket -- to change a setting, after the network drops, or after
 * a reboot -- resumes the conversation instead of starting over.
 *
 * STOPPING A SESSION IS NOT A REASON TO CALL THIS, and it used to be. A tap is
 * how you stop the device streaming, and people reach for it for reasons that
 * have nothing to do with being done talking -- so the gesture that was easiest
 * to hit was also the only one that destroyed something. Every stop keeps the
 * conversation now. The only callers are the two paths where the user asked to
 * forget it AND confirmed: the new_conversation function, and hold-again on a
 * stopped device.
 *
 * Does not block on flash, so it is safe from the LVGL and WebSocket tasks; see
 * the definition for how the empty record actually gets written.
 */
void dg_agent_clear_history(void);

/*
 * Whether there is a conversation to resume.
 *
 * Drives the one word on screen that tells the user their conversation
 * outlived a reboot ("resuming" rather than "connecting"), and gates the
 * hold-again gesture that offers to forget it.
 */
bool dg_agent_has_history(void);

/*
 * Write the conversation to flash if anything has changed since the last write.
 *
 * BLOCKS on a sector erase. Call it ONLY from session_ctl's worker task --
 * session_ctl_request_history_flush() is how every other task asks for this.
 */
void dg_agent_flush_history(void);
