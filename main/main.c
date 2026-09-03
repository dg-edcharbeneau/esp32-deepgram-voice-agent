/*
 * Deepgram Agent API device -- Waveshare ESP32-S3-Touch-AMOLED-1.75C.
 *
 * Scope is deliberately the two things underneath a voice device:
 *
 *   1. join Wi-Fi
 *   2. hold an authenticated Deepgram Agent API session over a WebSocket
 *
 * Full loop: mic -> Deepgram (STT/LLM/TTS) -> speaker, over one WebSocket.
 *
 * The greeting still proves the output half on its own -- Deepgram speaks
 * `agent.greeting` as soon as it applies Settings, before the mic has said
 * anything -- so a boot that plays the greeting but never answers you narrows
 * the problem to capture.
 *
 * Note that capture is gated while the agent speaks; see audio_io.c for why
 * (no echo cancellation on this board) and what it costs.
 *
 * Wi-Fi credentials are chosen at runtime: with nothing saved, or when the saved
 * network cannot be reached, the device raises a captive portal (wifi_prov.c)
 * and waits to be told which network to join. The menuconfig values are only a
 * first-boot seed -- see wifi_creds.h for the precedence rule.
 *
 * The Deepgram API key is still `idf.py menuconfig` under "Deepgram Agent
 * Device". It lands in sdkconfig, which is gitignored.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "multi_heap.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "agent_name.h"
#include "audio_io.h"
#include "battery.h"
#include "boot_button.h"
#include "heap_probe.h"
#include "dg_agent.h"
#include "session_ctl.h"
#include "ui.h"
#include "voices.h"
#include "wifi_creds.h"
#include "wifi_prov.h"
#include "wifi_sta.h"

static const char *TAG = "main";

#define WIFI_CONNECT_TIMEOUT_MS 30000

/* Written by the WebSocket task's callback, read by app_main's status loop. A
 * torn read of a counter only misreports one line of logging. */
static volatile uint32_t s_audio_bytes;
static volatile uint32_t s_turns;

/*
 * IDLE TIMEOUT
 *
 * The device uplinks 16 kHz mono for as long as a session is open -- about
 * 32 kB/s -- and reconnects by itself if the socket drops. A board left powered
 * on a desk therefore streams, and bills, indefinitely. This stops the session
 * once nothing has happened for a while; a tap or the BOOT button starts it again.
 *
 * The cost is reconnect latency on the next word: CONNECTING measured 1.1-6.0 s
 * plus 0.5 s of BUFFERING, so a short timeout trades responsiveness for spend.
 *
 * Activity is taken from signals that exist whether or not the display came up --
 * Deepgram's own end-of-turn and start-of-speech messages, plus what the speaker
 * is doing. Deliberately NOT the local VAD in ui.c: tying session lifetime to the
 * display would mean a headless boot never times out, or never stays up.
 */
/*
 * 32-bit ms, not 64-bit us, and THREE TASKS WRITE IT.
 *
 * note_activity() is called from the WebSocket task (the agent's own messages),
 * the LVGL task (an interrupt, the end of a display test) and this one. A
 * 64-bit stamp is stored in two halves on this target, so a reader landing
 * between them saw a time that never existed -- and this clock decides whether
 * to stop a live session, so a tear either killed one mid-conversation or
 * disabled the timeout for the next 71 minutes.
 *
 * Three writers RACING EACH OTHER is fine and stays fine: every one of them
 * means "something happened", so last-write-wins is the correct rule and no
 * ownership needs assigning. What was never fine was a torn read, and 32 bits
 * is what removes it.
 */
static volatile uint32_t s_activity_ms;

/*
 * Set when the server rejects the API key, cleared when a socket comes up.
 *
 * It exists because the stop CANNOT happen where the rejection is noticed:
 * on_state() runs on the WebSocket task and dg_agent_stop() only returns once
 * that task has halted, so calling it there would be the same lock-ordering
 * deadlock sdkconfig.defaults records as wedging this client permanently. The
 * telemetry loop picks it up on its next pass instead -- the same trick the idle
 * timeout uses, and dg_agent.h asks for explicitly.
 */
static volatile bool s_bad_key;

/*
 * Held while the panel should read "check api key".
 *
 * Separate from s_bad_key because they answer different questions: s_bad_key
 * means "a stop is owed", and this means "the reason is still worth showing".
 * Cleared when a socket comes up, or when the user touches the device -- at that
 * point they have seen it and are acting on it.
 */
static volatile bool s_bad_key_notice;

/* Follow-up interval after a face switch -- long enough for a frame or two to
 * land in the window, short enough not to smear the transition. */
#define TELEMETRY_SWITCH_MS 200

/* While waiting for the speaker to drain before the display test. */
#define TEST_ENTRY_POLL_MS 100

/*
 * How long the device stays stopped before it sleeps.
 *
 * Measured either side of this: with a session up the board runs the panel at
 * full brightness and ~29 fps, Wi-Fi with modem sleep disabled, and both codecs
 * clocked -- and it stays that way when the session ends, which is 88% of its
 * life. That is where the heat was going.
 *
 * Thirty seconds AFTER the session stops, and the session itself stops after
 * CONFIG_SESSION_IDLE_TIMEOUT_S of quiet, so this lands about 45 s after anyone
 * last spoke. Long enough that a pause in a conversation never reaches it.
 *
 * PROVISIONING NEEDS NO GUARD, though it looks like it should: enter_provisioning()
 * is noreturn and runs before the loop below, so the timer can never engage while
 * the setup QR is on screen.
 */
#define SLEEP_AFTER_STOPPED_MS 30000

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void note_activity(void)
{
    s_activity_ms = now_ms();
}

/*
 * INTERRUPT STATE
 *
 * Above the callbacks because on_state() and on_audio() both need it, not as a
 * statement about where it belongs.
 *
 * An interrupted turn's mute, and the deadline that guarantees it ends.
 *
 * 32-bit ms rather than the int64_t microseconds used elsewhere in this file,
 * because three tasks touch it -- the LVGL task sets it on the tap, the WebSocket
 * task clears it when the user speaks, this loop clears it on the backstop -- and
 * a 32-bit store is indivisible where a 64-bit one is two halves. Same reasoning
 * as the split in audio_io.c. The clear/set race between them is benign: the
 * worst outcome is a mute that lasts until the deadline.
 *
 * ENDING IT IS THE HARD PART, and two obvious signals were tried on the device
 * and both failed. Recorded here so neither gets tried a third time:
 *
 *   AgentAudioDone -- DOES NOT ARRIVE. Deepgram sent it 0 times across a
 *     12-minute run and once each in two earlier ones. dg_agent parses it
 *     correctly; it simply is not sent for most turns. Waiting on it parks the
 *     mute on the backstop every time.
 *
 *   A gap in the inbound audio -- CANNOT BE DISTINGUISHED FROM A STALL. This
 *     link stalls for seconds at a time; that is the bug the transport patch
 *     exists for. A 1.5 s quiet window released the mute mid-reply and the agent
 *     resumed talking: "I interrupted and it started talking again on its own."
 *
 * What actually means "that turn is over" is THE USER SPEAKING AGAIN. It is the
 * intent behind the tap, it comes from Deepgram's own VAD rather than from
 * timing, and it cannot be forged by a network stall.
 *
 * The consequence is that the mute must NOT gate the microphone -- a deaf device
 * can never hear its own release. See the gate comment in audio_io.c: the
 * full-duplex session death that motivated gating is handled in transport_ws.c
 * now, so the cost of listening through the tail is dropped frames.
 */

/*
 * Last resort, for the case where the user taps and then never speaks. Long,
 * because until it fires the agent's remaining audio is being discarded, which is
 * exactly what was asked for -- there is no rush. It no longer risks deafness.
 */
#define MUTE_MAX_MS 30000
static volatile uint32_t s_mute_deadline_ms;

/*
 * True while agent audio is still ARRIVING, as distinct from still playing.
 *
 * The interrupt needs this and AgentAudioDone alone cannot give it: done means
 * the agent finished SENDING, and the ring holds 12.3 s behind that, so a tap in
 * the tail -- the case the feature exists for -- lands after its own clearing
 * event and latches the mute until the deadline. With this flag the tail tap
 * takes the flush and skips the mute entirely, because nothing more is coming.
 *
 * Set on the WebSocket task, read on the LVGL task, one bool store either way.
 */
static volatile bool s_turn_inbound;

static void end_interrupt(const char *why)
{
    if (s_mute_deadline_ms == 0) {
        return;
    }
    s_mute_deadline_ms = 0;
    audio_io_mute_playback(false);
    ESP_LOGI(TAG, "EVT interrupt-end (%s)", why);
}

/*
 * How long after an interrupt a tap still refuses to toggle the session.
 *
 * The interrupt and the toggle now share the button, and the branch between them
 * is "is the speaker busy" -- which the interrupt itself makes false, within
 * PLAYBACK_TAIL_MS of the flush. So the second tap of a double-tap arrives at a
 * quiet device and reads as "end the conversation". One impatient finger, and the
 * gesture that means "stop talking" has hung up instead.
 *
 * 1500 ms, matching session_ctl's COOLDOWN_MS, and for the same reason it gives:
 * with a touch panel as the only control, ignoring a press someone meant costs
 * far less than acting on one they did not. It delays nothing real -- the tap
 * that ends a conversation is a considered one, and it still works a beat later.
 */
#define INTERRUPT_GRACE_MS 1500
static volatile uint32_t s_interrupt_grace_ms;

static bool in_interrupt_grace(void)
{
    if (s_interrupt_grace_ms == 0) {
        return false;
    }
    /* Wrap-safe, like the mute deadline below: the difference is compared against
     * half the range rather than the deadline against now. */
    return (now_ms() - s_interrupt_grace_ms) >= (UINT32_MAX / 2);
}

/*
 * Silence the reply, and stop the rest of it from arriving.
 *
 * The flush silences the ring. Whether that is the whole job depends on something
 * the flush cannot see: is the reply still ARRIVING?
 *
 * If it is, the mute is the other half -- Deepgram has no interrupt message and
 * keeps sending, so a flush alone just makes the agent pause and resume mid-word a
 * moment later. Measured: 410 kB of story kept arriving over the 14 s after one
 * tap, every byte of it discarded.
 *
 * If it is not -- a tap after the last byte has landed, with only the ring left to
 * empty -- there is nothing to mute. Flush and stop.
 *
 * Either way the microphone stays open and the user can talk immediately, which is
 * both what the feature promised and what releases the mute.
 *
 * Deliberately NOT routed through session_ctl: request() drops anything arriving
 * while busy or inside COOLDOWN_MS, which is exactly when someone interrupts.
 * Every call here is a single flag store, safe on the LVGL task.
 *
 * The caller has already established that playback is active, which is what makes
 * this the interrupt arm rather than the toggle arm.
 */
static void do_interrupt(void)
{
    audio_io_flush();
    note_activity();
    s_interrupt_grace_ms = now_ms() + INTERRUPT_GRACE_MS;

    if (s_turn_inbound) {
        ESP_LOGI(TAG, "EVT interrupt");
        audio_io_mute_playback(true);
        s_mute_deadline_ms = now_ms() + MUTE_MAX_MS;
    } else {
        /* Distinct from the line above so a serial capture shows which path ran
         * -- the tail tap arms no mute and gets no end event. */
        ESP_LOGI(TAG, "EVT interrupt (tail)");
    }
}

static const char *state_name(dg_agent_state_t state)
{
    switch (state) {
    case DG_AGENT_DISCONNECTED: return "disconnected";
    case DG_AGENT_CONNECTED:    return "connected";
    case DG_AGENT_READY:        return "ready";
    case DG_AGENT_ERROR:        return "error";
    case DG_AGENT_BAD_KEY:      return "check api key";
    }
    return "?";
}

static void on_state(dg_agent_state_t state, void *ctx)
{
    ESP_LOGI(TAG, "agent session %s", state_name(state));
    if (state == DG_AGENT_CONNECTED) {
        /* A socket that opened is a key the server accepted, so the warning has
         * nothing left to warn about. */
        s_bad_key_notice = false;
        /* A new socket is a new conversation, so the status line should describe
         * this one rather than every session since boot. */
        s_audio_bytes = 0;
        s_turns = 0;
        /* No turn can be in flight on a socket that has just opened. The
         * disconnect paths below already clear this, so it is belt and braces --
         * but a stale true would arm a mute on the first tap of a session that
         * has nothing to mute. */
        s_turn_inbound = false;
        /* A socket that opened is a key the server accepted. */
        s_bad_key = false;
        /*
         * And the same for a pending mute, which is NOT belt and braces: a
         * deliberate stop goes through session_ctl and logs "session stopped"
         * without ever reaching the DISCONNECTED case below, so the deadline
         * outlives its session and fires inside the next one -- observed as an
         * "EVT interrupt-end (deadline)" four seconds into a fresh session that
         * had never been interrupted. audio_io_reset() already clears the mute
         * itself on that path; this clears the timer that chases it.
         */
        end_interrupt("new session");
    }
    /* Runs on the WebSocket task, so this must not touch LVGL -- it stores the
     * literal and the frame timer picks it up. */
    ui_set_status(state_name(state), state == DG_AGENT_READY);

    /*
     * The connection ladder. A session walks CONNECTING -> BUFFERING ->
     * INITIALIZING in sequence and each rung is drawn visibly fuller and
     * brighter than the last, so someone watching sees progress without reading
     * the label -- which is the whole reason the display distinguishes them
     * rather than showing one "connecting" state throughout.
     */
    switch (state) {
    case DG_AGENT_CONNECTED:
        /* Socket open and Settings sent; waiting for SettingsApplied. */
        ui_set_failed(false);
        ui_set_behaviour(UI_BEHAVIOUR_BUFFERING);
        break;
    case DG_AGENT_READY:
        ui_set_failed(false);
        ui_set_behaviour(UI_BEHAVIOUR_INITIALIZING);
        break;
    case DG_AGENT_ERROR:
        /* The stream died mid-turn, so the next bytes to arrive belong to a
         * different one. Says so before anything can be stitched onto them. */
        audio_io_note_stream_gap();
        /* And nothing more is coming down it, so an interruption still waiting for
         * the user to speak would otherwise discard the whole of the NEXT turn
         * before the backstop caught it. */
        s_turn_inbound = false;
        end_interrupt("stream gone");
        /* Frozen, not merely dim: this one has stopped trying. */
        ui_set_failed(true);
        ui_set_behaviour(UI_BEHAVIOUR_DISCONNECTED);
        break;
    case DG_AGENT_BAD_KEY:
        /* Same teardown as ERROR -- the stream is just as gone -- but the panel
         * is already saying "check api key" from state_name(), and the flag is
         * what gets the retry actually stopped on a task that may do it. */
        audio_io_note_stream_gap();
        s_turn_inbound = false;
        end_interrupt("key rejected");
        ui_set_failed(true);
        ui_set_behaviour(UI_BEHAVIOUR_DISCONNECTED);
        s_bad_key = true;
        break;
    case DG_AGENT_DISCONNECTED:
        /* Between attempts. session_ctl reports a deliberate stop separately, so
         * arriving here means the socket went away on its own -- and THAT is the
         * path audio_io_reset() never covered, because the auto-reconnect does not
         * go through session_ctl. */
        audio_io_note_stream_gap();
        /* Same as the error path: the socket is gone, so the inbound turn it was
         * carrying is over whether or not it said so. */
        s_turn_inbound = false;
        end_interrupt("stream gone");
        ui_set_behaviour(UI_BEHAVIOUR_CONNECTING);
        /*
         * The socket going away on its own is the leading indicator of the
         * failures this persistence exists for -- a link that is about to stay
         * down, a board about to be power-cycled to fix it. Get the conversation
         * onto flash now rather than waiting out the debounce.
         *
         * Via the request, never inline: this runs on the WebSocket task, and
         * the erase behind it would block that task for tens of milliseconds.
         */
        session_ctl_request_history_flush();
        break;
    }
}

static void on_conversation_text(const char *role, const char *content, void *ctx)
{
    s_turns++;
    note_activity();
}

static void on_audio(const uint8_t *data, size_t len, void *ctx)
{
    s_audio_bytes += len;
    /* Bytes are on the wire right now, which is what the interrupt has to know:
     * a tap while this is set has a reply still arriving to mute, a tap after it
     * clears has only the ring to flush. */
    s_turn_inbound = true;
    /* Non-blocking by contract -- this is the WebSocket task. */
    audio_io_play(data, len);
}

static void on_user_started_speaking(void *ctx)
{
    /* Deepgram's own speech detection, which is better evidence than the local
     * level and arrives even when the display is not running. */
    note_activity();
    /* And it is what puts the orb into LISTENING -- measured to separate speech
     * from a quiet room cleanly where the local band gate could not. */
    ui_note_user_speech();
    /* Stop mid-sentence rather than talk over the user. */
    audio_io_flush();
    /*
     * And this ends an interruption. The user talking is the only trustworthy
     * "that turn is over" this API offers -- see the note above MUTE_MAX_MS for
     * the two signals that were tried and failed. The flush above runs first so
     * nothing the mute was holding back can be heard on the way out.
     */
    end_interrupt("user speaking");
}

/* Runs on the capture task. dg_agent_send_audio() no-ops until the session is
 * ready, so this needs no gating of its own. */
static void mic_to_agent(const uint8_t *pcm, size_t len)
{
    dg_agent_send_audio(pcm, len);
}

/* Runs on the WebSocket task, which cannot tear down its own client -- so this
 * only posts, exactly like the touch gestures do. */
static void on_reload_required(void *ctx)
{
    session_ctl_request_reload();
}

/*
 * Set when the agent has asked for the display test, cleared once the speaker has
 * actually finished the sentence announcing it. See enter_display_test().
 */
/* volatile, like every other cross-task flag in this file: set on the WebSocket
 * task, read and cleared on this one. */
static volatile bool s_test_entry_pending;
/* 32-bit ms, same reason as s_activity_ms -- written by the WebSocket task and
 * read here. */
static volatile uint32_t s_test_entry_deadline_ms;

/* How long to wait for the speaker to drain before starting anyway. Long enough
 * for a sentence the agent has already finished sending, short enough that a
 * playback path stuck busy cannot swallow the feature.
 *
 * NOTE the ring holds 12.3 s, not the ~6 s four places in this tree used to
 * claim, so this 8 s is NOT the "cannot possibly still be playing" bound it reads
 * as. A reply longer than 8 s of audio still gets cut off mid-word -- the exact
 * fault the comment above enter_display_test() says this deferral exists to
 * prevent. Unmeasured: no logged reply has been long enough to trip it. */
#define TEST_ENTRY_WAIT_MS (8 * 1000)

static void enter_display_test(void)
{
    /*
     * ORDER MATTERS. session_ctl's stop path calls
     * audio_io_capture_set_enabled(false), so the monitor flag has to go up
     * after the session comes down or the stop clears it -- and then the orb
     * would sit at zero amplitude for the whole test, which is most of the point
     * of it gone.
     */
    session_ctl_request_stop();
    audio_io_capture_set_monitor(true);
    ui_start_display_test();
}

static void on_display_test_required(void *ctx)
{
    /*
     * AgentAudioDone is NOT the speaker finishing. It means the agent has
     * finished SENDING, and between that and the last sample leaving the codec
     * sits the playback ring -- 384 kB, 12.3 s of mono. Entering here
     * stops the session, which drops the queue, and the announcement is cut off
     * mid-word. Measured on the device, not theorised.
     *
     * So this only arms the entry; the main loop below waits for
     * audio_io_playback_active() to go false, which is tied to what the speaker
     * is actually doing rather than to what the agent last claimed.
     */
    /* Deadline before the flag, so the loop cannot see the request with a stale
     * deadline behind it. */
    s_test_entry_deadline_ms = now_ms() + TEST_ENTRY_WAIT_MS;
    s_test_entry_pending = true;
}

static void on_agent_audio_done(void *ctx)
{
    note_activity();
    /* The agent has stopped SENDING, so nothing more can arrive to discard. Worth
     * having when it comes, but do not build on it: measured across a 12-minute
     * run, Deepgram sent this zero times. The release that actually fires is the
     * user speaking. */
    s_turn_inbound = false;
    end_interrupt("turn done");
    ESP_LOGI(TAG, "turn complete, %" PRIu32 " audio bytes received", s_audio_bytes);
}

/*
 * The "forget this conversation?" window, armed by a hold on a stopped device
 * and answered by a second one. 0 when nothing is offered.
 *
 * Lives here rather than in ui.c for the same reason every other gesture
 * meaning does: ui.c reports presses and knows nothing about sessions.
 */
/* volatile, like every other cross-task deadline in this file: written on the
 * LVGL task, read and cleared on this one. It gates the only irreversible
 * action on the device, so it is the last one that should be cached in a
 * register across the telemetry loop. */
static volatile uint32_t s_forget_armed_ms;
#define FORGET_CONFIRM_MS 5000

/*
 * When a transient status word ("forgotten") should be taken off the panel.
 *
 * SEPARATE from the offer above, because the two have different lifetimes and
 * sharing one variable was a bug: any gesture clears the offer, so a third hold
 * after a confirm cleared the very deadline that was going to put the label
 * back, and "forgotten" stayed on a device that had moved on.
 */
static volatile uint32_t s_notice_until_ms;
#define STATUS_NOTICE_MS 3000

static bool in_forget_window(void)
{
    return s_forget_armed_ms != 0 &&
           (now_ms() - s_forget_armed_ms) < FORGET_CONFIRM_MS;
}

/* Runs on the LVGL task with the LVGL lock held: signal only, never block. */
static void on_gesture(ui_gesture_t gesture)
{
    /*
     * Any deliberate touch retires the bad-key warning. The user has seen it and
     * is acting, and leaving it armed would let the 1 Hz re-assert in the status
     * loop paint it back over session_ctl's "connecting" a beat after a tap
     * asked for a session.
     */
    s_bad_key_notice = false;

    /*
     * ANY gesture answers the offer, and only the hold that arrives while it
     * stands can answer it with "yes". Reading it and clearing it here is what
     * makes that true: without it the arm survives an intervening tap -- start
     * the session, have the start fail, hold again to retry -- and that retry
     * silently destroys the conversation with no offer anywhere on screen.
     * For the one action on this device that cannot be undone.
     */
    const bool forget_offered = in_forget_window();
    s_forget_armed_ms = 0;

    switch (gesture) {
    case UI_TAP:
        /*
         * ONE TARGET, TWO MEANINGS, AND EXACTLY ONE OF THEM PER TAP.
         *
         * Interrupting used to have a target of its own -- everything outside the
         * 70 px button, which is most of a 466 px panel -- and that is why it is
         * here instead: the gesture nobody aimed at collected every brush of the
         * bezel, and each one cost a sentence.
         *
         * The two meanings can share the button because they are never both
         * plausible. Agent speaking means "stop talking"; anything else means
         * "start or end the conversation". So the split is this if/else and
         * nothing more: an if/else cannot fall through into the toggle, which is
         * the property being bought -- an interrupt must never be able to also
         * hang up.
         *
         * audio_io_playback_active() rather than a state of our own. It is true
         * while the ring holds audio and for PLAYBACK_TAIL_MS past the last write,
         * which is the same thing as "you can still hear it" -- and being wrong in
         * the generous direction is the safe way to be wrong here, since the worst
         * case is a tap that interrupts nothing instead of a tap that ends the
         * conversation.
         */
        if (audio_io_playback_active()) {
            do_interrupt();
        } else if (in_interrupt_grace()) {
            /*
             * The second tap of an impatient double-tap, landing after the flush
             * has already silenced the speaker. Without this it reads as a toggle
             * and hangs up -- the exact failure the if/else above exists to
             * prevent, arriving a beat late instead of on the same press.
             */
            ESP_LOGI(TAG, "EVT tap ignored (interrupt grace)");
        } else {
            ESP_LOGI(TAG, "EVT tap");
            session_ctl_request_toggle();
        }
        break;
    case UI_HOLD:
        /*
         * ONE GESTURE, TWO MEANINGS, SPLIT ON WHETHER A SESSION IS UP -- the
         * same construction the tap above uses, and safe for the same reason:
         * the two are never both plausible.
         *
         * Running, a hold means what it always has: force a restart, for a
         * session that is up but wedged. Stopped, there is no session to
         * recover and a hold already did nothing a tap could not do -- so that
         * is where the destructive gesture can live without taking anything
         * away.
         *
         * WHY A SECOND HOLD CONFIRMS AND NOT A TAP. A tap on a stopped device
         * already means "start", so tap-to-confirm would put two live meanings
         * on one press with no way to tell them apart. Holding again is
         * unambiguous, and it is deliberate in a way a brush of the panel is
         * not. Forgetting a conversation is the one thing here that cannot be
         * undone.
         */
        if (session_ctl_is_running() || session_ctl_is_busy()) {
            /*
             * The busy test is not redundant with the running one, and leaving
             * it out was a live bug. do_stop() clears s_running and then keeps
             * working -- the stopped repaint and a blocking flash write still
             * follow it, and a restart of a wedged session goes straight on into
             * do_start() and a TLS handshake. All of that is a window that looks
             * "stopped" while the worker is mid-action, and it is exactly when
             * someone is holding the button impatiently. Two more holds in it
             * would arm and then confirm the one action here that cannot be
             * undone.
             *
             * The boolean, not busy_for_ms(): that one is a duration and reads
             * zero for the first millisecond of every action, which is a hole in
             * a gate guarding something irreversible.
             */
            ESP_LOGI(TAG, "EVT hold");
            session_ctl_request_restart();
        } else if (!dg_agent_has_history()) {
            /* Nothing to forget. Keep the old meaning rather than inventing a
             * gesture that silently does nothing. */
            ESP_LOGI(TAG, "EVT hold");
            session_ctl_request_restart();
        } else if (forget_offered) {
            ESP_LOGI(TAG, "EVT hold -> forget confirmed");
            dg_agent_clear_history();
            ui_set_status("forgotten", false);
            /* A notice, not an offer: nothing is being asked any more, and the
             * telemetry loop takes it off the panel on its own deadline. */
            s_notice_until_ms = now_ms() + STATUS_NOTICE_MS;
        } else {
            ESP_LOGI(TAG, "EVT hold -> offering to forget");
            s_forget_armed_ms = now_ms();
            /*
             * A repaint deadline as well as an offer window, because the two can
             * come apart: any gesture clears the arm, and a tap that session_ctl
             * then REFUSES (cooldown) repaints nothing -- stranding a prompt for
             * an irreversible action on the panel with nothing left to retire
             * it. This deadline does not care why the offer ended.
             */
            s_notice_until_ms = now_ms() + FORGET_CONFIRM_MS + STATUS_NOTICE_MS;
            ui_set_status("forget? hold again", false);
        }
        break;

    case UI_TEST_DONE:
        /*
         * Undo the display test's two side effects, in the reverse order they
         * were applied: the microphone stops feeding the display, and a fresh
         * session replaces the one the test closed.
         */
        ESP_LOGI(TAG, "EVT test-done, restarting session");
        audio_io_capture_set_monitor(false);
        /*
         * Before the start, not after: the idle clock has been running
         * untouched for the whole test -- nothing notes activity while the
         * session is down -- so without this the timeout below sees the test's
         * own duration as quiet time and stops the session it just started.
         * Measured: a 41 s test killed the new session 9 s after it went ready.
         */
        note_activity();
        session_ctl_request_start();
        break;
    }
}

/* File scope so session_ctl can reopen a session with the same callbacks. */
static const dg_agent_callbacks_t s_callbacks = {
    .on_state = on_state,
    .on_conversation_text = on_conversation_text,
    .on_audio = on_audio,
    .on_agent_audio_done = on_agent_audio_done,
    .on_user_started_speaking = on_user_started_speaking,
    .on_reload_required = on_reload_required,
    .on_display_test_required = on_display_test_required,
};

/*
 * Hands the device over to the setup portal. Never returns -- wifi_prov_run()
 * ends in a reboot either way, which is what keeps the AP-to-STA transition
 * from having to be unpicked on a live device.
 */
static void enter_provisioning(void) __attribute__((noreturn));
static void enter_provisioning(void)
{
    /*
     * The ring has nothing to visualise with no session, and a calm screen
     * showing the network name is the instruction.
     *
     * Two lines now: the AP is WPA2, so the passphrase has to be legible to
     * anyone whose camera will not act on the QR below. update_qr() moves this
     * label under the code for exactly that purpose.
     *
     * Static storage because ui keeps the pointer, not the bytes.
     */
    static char panel[48];
    snprintf(panel, sizeof(panel), "%s\n%s",
             wifi_prov_ap_name(), wifi_prov_ap_password());
    ui_set_stopped(true);
    ui_set_status(panel, false);

    if (wifi_prov_start() != ESP_OK) {
        ESP_LOGE(TAG, "could not start the setup portal");
        ui_set_status("setup failed", false);
        wifi_prov_run();
    }

    /*
     * The AP as something a camera can act on. A phone that scans this joins
     * the network directly -- passphrase included, so encrypting the AP costs
     * the user nothing -- which skips the one step of this whole flow that
     * involves reading characters off a 466 px round panel and retyping them.
     *
     * The payload needs no escaping because the passphrase alphabet excludes
     * every character WIFI: treats as reserved. See AP_PASS_ALPHABET.
     */
    static char qr[80];
    snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:%s;P:%s;;",
             wifi_prov_ap_name(), wifi_prov_ap_password());
    ui_show_qr(qr);

    wifi_prov_run();
}

void app_main(void)
{
    /*
     * FIRST, so the allocation-failure hook is armed before anything large is
     * allocated. Compiles to nothing unless CONFIG_HEAP_PROBE is on.
     */
    heap_probe_start();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Wi-Fi keeps calibration data in NVS; a stale partition must be wiped
         * or esp_wifi_init() fails on a board flashed with a different build. */
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /*
     * Diagnostic, and the reason conversation history is NOT kept here. The nvs
     * partition is 0x6000 -- six 4 kB pages, one always held back for
     * compaction, 126 entries each -- so `total` reads 630 and that is the whole
     * budget, shared with the Wi-Fi driver's calibration blob (nvs.net80211),
     * the credentials, the API key and the agent settings. A kilobyte-scale blob
     * rewritten per turn would force compaction, and compaction rewrites its
     * neighbours: the erase budget it spends is the one holding the credentials.
     * See main/history_store.c for where the history went instead.
     */
    nvs_stats_t nvs_stats;
    if (nvs_get_stats(NULL, &nvs_stats) == ESP_OK) {
        ESP_LOGI(TAG, "nvs: %zu/%zu entries used, %zu free, %zu namespaces",
                 nvs_stats.used_entries, nvs_stats.total_entries,
                 nvs_stats.free_entries, nvs_stats.namespace_count);
    }

    /*
     * First, and before anything that can block: the escape hatch out of a bad
     * network has to work on a device that is failing to finish booting.
     */
    ESP_ERROR_CHECK(boot_button_start());

    /* After nvs_flash_init(), before the first Settings message is built. */
    voices_init();
    agent_name_init();

    /* Before the session, so the greeting has somewhere to go the moment it
     * arrives. */
    ESP_ERROR_CHECK(audio_io_init(DG_AUDIO_SAMPLE_RATE));

    /*
     * After audio, because that is what brings the I2C bus up -- though
     * battery_start() calls bsp_i2c_init() itself rather than relying on the
     * order, the same way the BSP's own entry points do. Compiles to nothing
     * unless CONFIG_BATTERY is on, and a board whose PMU does not answer simply
     * logs and carries on.
     */
    battery_start();

    ESP_ERROR_CHECK(wifi_stack_init());

    /*
     * Non-blocking, and bringing the panel up costs ~1.2 s in the CO5300 reset
     * sequence -- so start associating first and overlap the two.
     */
    char ssid[WIFI_CREDS_SSID_LEN];
    char pass[WIFI_CREDS_PASS_LEN];
    const bool have_creds = wifi_creds_load(ssid, pass);
    if (have_creds) {
        ESP_ERROR_CHECK(wifi_sta_start(ssid, pass));
        ui_set_status("connecting", false);
    } else {
        ui_set_status("setup", false);
    }
    if (ui_start() == ESP_OK) {
        audio_io_set_playback_tap(ui_feed_agent);
        audio_io_set_capture_tap(ui_feed_mic);
        ui_set_gesture_handler(on_gesture);
    } else {
        /* A dark screen is not a reason to give up the voice loop. */
        ESP_LOGW(TAG, "spectrum display unavailable, continuing headless");
    }


    /* After the panel, so the portal's instructions are actually readable. */
    if (!have_creds) {
        ESP_LOGI(TAG, "no network configured, starting setup portal");
        enter_provisioning();
    }

    /*
     * Capture before the session, and gated until one opens. The task cannot be
     * created twice, so it has to outlive any individual session -- see
     * audio_io_capture_set_enabled().
     */
    audio_io_capture_set_enabled(false);
    ESP_ERROR_CHECK(audio_io_capture_start(mic_to_agent));

    err = wifi_sta_wait_connected(WIFI_CONNECT_TIMEOUT_MS);
    if (err != ESP_OK) {
        /*
         * The saved network is unreachable -- moved, renamed, or its password
         * changed. Offer the portal rather than sitting on a "no network"
         * screen the user cannot act on.
         *
         * The credentials are deliberately NOT erased here: a router that is
         * merely rebooting must not cost the user their password. The portal
         * overwrites them on save, and times out back into another attempt if
         * nobody arrives.
         */
        ESP_LOGE(TAG, "could not join \"%s\" (%s), starting setup portal",
                 ssid, esp_err_to_name(err));
        wifi_sta_stop();
        enter_provisioning();
    }

    /*
     * AFTER THE LINK IS UP, not before it.
     *
     * session_ctl_start() calls dg_agent_init(), which reads the API key out of
     * NVS -- a flash access, and on this config a flash access briefly disables
     * the cache on both cores (SPIRAM_FETCH_INSTRUCTIONS / SPIRAM_RODATA). Doing
     * that while the station is still trying to associate puts an avoidable
     * hiccup inside the one window where the driver is timing-sensitive. Nothing
     * needed it earlier: the session is not requested until the line below, and
     * every session_ctl_request_*() is a no-op while s_task is NULL, so a tap
     * during "connecting" is ignored rather than starting a doomed session.
     *
     * The unreachable-network path above never reaches this at all now, which is
     * the small bonus: a device on its way to the portal no longer reads the key
     * the portal is about to overwrite.
     */
    ESP_ERROR_CHECK(session_ctl_start(&s_callbacks));
    session_ctl_request_start();

    /*
     * ONE telemetry line, one cadence, machine-parseable.
     *
     * This used to be a 10 s prose status line, alongside four other logs on four
     * other cadences -- so no single line ever described the device at one
     * instant, and answering any question meant interleaving five drifting
     * streams by timestamp. Everything is here now, once a second, as key=value.
     *
     * It runs on THIS task rather than in the UI, deliberately: ESP-IDF console
     * writes block, and ~200 bytes at 115200 baud is most of a frame. Logging
     * this from the frame timer would corrupt the very timings it reports.
     *
     * Internal RAM stays in the line because it is the scarce resource once the
     * display is up, and the case that bites is a WebSocket reconnect wanting a
     * burst of it with the render buffer already allocated. `intmax` matters more
     * than the total: if it sags towards 40 kB, shrink DRAW_ROWS in ui.c before
     * tuning anything else.
     */
    /*
     * After a face switch, shorten the next wait instead of the usual interval.
     * The switch is reported in the window it happened, so the "before" sample is
     * the previous line -- but the "after" would otherwise be a whole interval
     * later, with a window's worth of averaging smeared across the transition.
     * A short follow-up gives a clean post-switch reading.
     */
    uint32_t wait_ms = CONFIG_UI_TELEMETRY_MS;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
        wait_ms = CONFIG_UI_TELEMETRY_MS;

        /*
         * Hand over once the speaker is actually done, so the announcement is
         * not cut off. Polled here rather than in a task of its own, at a
         * tighter cadence than the telemetry window so the wait is not audible
         * as a gap -- the same trick the post-face-switch reading uses.
         */
        if (s_test_entry_pending) {
            bool drained = !audio_io_playback_active();
            /* Wrap-safe, like the mute deadline below. */
            bool timed_out = (now_ms() - s_test_entry_deadline_ms) < (UINT32_MAX / 2);
            if (drained || timed_out) {
                s_test_entry_pending = false;
                ESP_LOGI(TAG, "EVT test entering (%s)",
                         drained ? "speaker drained" : "wait timed out");
                enter_display_test();
            } else {
                wait_ms = TEST_ENTRY_POLL_MS;
            }
        }

        /*
         * Backstop only. The real release is on_user_started_speaking(); this
         * catches a tap that is never followed by speech. Unsigned compare, so it
         * is correct across the 49-day wrap.
         */
        if (s_mute_deadline_ms != 0 &&
            (now_ms() - s_mute_deadline_ms) < (UINT32_MAX / 2)) {
            end_interrupt("deadline");
        }

        /*
         * BATTERY. Read here rather than anywhere else for the reason every other
         * subsystem is read here: this task is the one allowed to be slow, and
         * battery.c samples on its own schedule, so this is a copy of the last
         * value and not a bus transaction.
         *
         * The warning is the dots themselves -- battery.c's hysteresed `low` flag
         * keeps them on screen whatever else is happening. The status label is
         * deliberately NOT used: it reports the session, on_state_change() rewrites
         * it constantly, and a battery message parked there would either be
         * overwritten within the second or be a lie about the session.
         */
        battery_status_t bat;
        const bool bat_ok = battery_get(&bat);
        ui_set_battery(bat.percent, bat.charging, bat.low, bat_ok);

        /*
         * WI-FI SIGNAL, read here for the same reason and at the same cadence.
         *
         * Unlike the battery this is not a cached copy of a slow sample -- the
         * Wi-Fi driver holds the number and hands it over in microseconds, so
         * reading it on this pass is the whole mechanism. See wifi_sta.h for why
         * it needs no sampler of its own.
         */
        wifi_signal_t sig;
        const bool sig_ok = wifi_sta_get_signal(&sig);
        ui_set_signal(sig.bars, sig.weak, sig_ok);

        /*
         * CRITICAL: hold the panel asleep, session or no session. It is the
         * largest single draw on the board and dimming it is the only lever this
         * firmware has over how long the rest of the charge lasts. The session is
         * left alone on purpose -- a device that hangs up mid-sentence to save
         * power is worse than one that goes dark and keeps talking.
         */
#if CONFIG_BATTERY
        const bool bat_critical = (CONFIG_BATTERY_CRITICAL_PCT > 0) && bat_ok &&
                                  !bat.charging &&
                                  bat.percent <= CONFIG_BATTERY_CRITICAL_PCT;
#else
        const bool bat_critical = false;
#endif
        static bool prev_critical;
        if (bat_critical != prev_critical) {
            prev_critical = bat_critical;
            ESP_LOGW(TAG, "EVT battery critical=%d pct=%d", (int)bat_critical, bat.percent);
        }

        /*
         * SLEEP, and the radio follows the panel rather than the other way round.
         *
         * ui_set_sleep() is only a request: the frame timer applies it on the LVGL
         * task, and it refuses while the display test is running. A touch withdraws
         * it and wakes the panel there and then, without telling this loop. So the
         * panel is the source of truth and ui_is_asleep() is what the Wi-Fi radio
         * tracks -- which is why the wake edge below is detected rather than
         * assumed.
         *
         * The microphone is absent from all this deliberately. The capture task
         * parks and closes the ES7210 itself whenever nothing downstream wants
         * samples, which the stop already arranged 30 s earlier; see the park
         * block in audio_io.c.
         */
        static uint32_t stopped_since_ms;
        static bool prev_asleep;
        static bool radio_saving;
        const bool panel_asleep = ui_is_asleep();

        if (prev_asleep && !panel_asleep) {
            /* Something woke the panel -- a finger, almost always. Start counting
             * again from now rather than sleeping straight back down. */
            stopped_since_ms = now_ms();
        }
        prev_asleep = panel_asleep;

        if (bat_critical) {
            /* Ahead of the session check, which is the only branch that would
             * otherwise wake the panel back up on the next pass. */
            ui_set_sleep(true);
        } else if (session_ctl_is_running()) {
            stopped_since_ms = 0;
            ui_set_sleep(false);
        } else if (stopped_since_ms == 0) {
            stopped_since_ms = now_ms();
        } else if (!panel_asleep &&
                   (now_ms() - stopped_since_ms) > SLEEP_AFTER_STOPPED_MS) {
            ui_set_sleep(true);
        }

        if (panel_asleep != radio_saving) {
            wifi_sta_set_power_save(panel_asleep);
            radio_saving = panel_asleep;
        }

        uint32_t played, dropped, captured;
        audio_io_stats(&played, &dropped, &captured);

        ui_telemetry_t t;
        ui_get_telemetry(&t);

        /*
         * Idle check. Playback counts as activity in its own right: a long answer
         * can outlast the timeout with no message arriving mid-stream, and cutting
         * the session while the speaker is mid-sentence would be worse than any
         * saving. The session must be running for the clock to mean anything.
         */
        if (audio_io_playback_active()) {
            note_activity();
        }
        /*
         * AND IT MUST BE READY, not merely running.
         *
         * Nothing notes activity while the socket is down, so a reconnect spends
         * the whole window accumulating idle time and the timeout fires on a
         * session that was never given the chance to be busy. Measured: a
         * transport write error at 78.9 s, the client's own 5 s reconnect, the
         * timeout at 85.9 s, and the recovered session live at 86.4 s and stopped
         * 2 ms later by a request issued before it came back.
         *
         * Gating the check is only half of it. s_activity_ms keeps accumulating
         * while the socket is down, so a gate alone would not save the session --
         * it would defer the kill to the exact moment readiness returned, with the
         * clock already past the limit. Which is the symptom, not a fix.
         *
         * So the clock is also RESTARTED on the notready -> ready edge. Coming
         * back counts as activity, and the recovered session gets a full window to
         * prove itself idle rather than being judged on the outage.
         *
         * An edge rather than stamping while ready, so a session that sits ready
         * and silent still times out as it should.
         *
         * Same fault as the display test's, which I fixed as a symptom rather than
         * as a pattern -- a clock counting time in which activity was impossible.
         */
        bool ready_now = dg_agent_is_ready();
        static bool was_ready;
        if (ready_now && !was_ready) {
            note_activity();
        }
        was_ready = ready_now;

        /*
         * DEADLOCK BACKSTOP, and the last line of defence rather than a fix.
         *
         * session_ctl refuses every request while an action is busy, so an action
         * that never finishes is a device that never accepts another gesture: the
         * panel sits on "stopping" and only a physical reset recovers it. That
         * happened repeatedly on 2026-08-27, and the cause -- an upstream CLOSE
         * with an infinite deadline -- is fixed in dg_agent_stop(). This exists
         * because the NEXT such blocker should cost a reboot rather than a trip
         * to the bench.
         *
         * The threshold is deliberately far above any legitimate stop. A healthy
         * one is ~150 ms; the worst measured on a congested link was 5.4 s, and
         * the structural worst case is AUDIO_SEND_TIMEOUT plus the client's
         * network_timeout_ms, about 7 s. Thirty seconds cannot be anything but
         * stuck.
         *
         * A reboot is the right answer and not a cop-out: it is what this project
         * already does for the AP-to-STA transition, on the grounds that it costs
         * ~2 s and is impossible to get subtly wrong. NVS holds the network, the
         * key, the voice and the volume, so nothing the user set is lost.
         */
        const uint32_t busy_ms = session_ctl_busy_for_ms();
        if (busy_ms > 30000) {
            ESP_LOGE(TAG, "EVT sessionstuck busy=%" PRIu32 "ms -- rebooting; a "
                          "session action has not completed and no gesture can be "
                          "accepted until it does", busy_ms);
            /* Straight to the log, then out: nothing else can run on this task,
             * and anything that could has already had thirty seconds. */
            fflush(stdout);
            esp_restart();
        }

        /*
         * A rejected key is the one failure reconnecting cannot fix, so stop
         * rather than retry it every 5 s for as long as the board is powered.
         */
        if (s_bad_key && session_ctl_is_running()) {
            ESP_LOGE(TAG, "EVT badkey -- stopping; fix the key via the setup portal "
                          "(hold BOOT 3 s) or menuconfig");
            s_bad_key = false;   /* one stop per rejection, not one per second */
            s_bad_key_notice = true;
            session_ctl_request_stop();
        }

        /*
         * AND KEEP PUTTING THE REASON BACK. session_ctl narrates its own
         * teardown -- "stopping", then "stopped" -- which would otherwise bury
         * the only message that says what to fix under one that says nothing.
         *
         * Re-asserted every pass rather than once, because do_stop() clears
         * s_running one line BEFORE it paints "stopped": a single re-assert
         * would lose that race. Cheap to repeat -- update_status_label()
         * compares literals by pointer, so an unchanged status costs no render
         * pass.
         *
         * ONLY WHILE THE SESSION IS DOWN, and this is the part worth getting
         * right. An earlier version cleared the notice whenever a session was
         * running, which looked equivalent and was not: s_running stays TRUE for
         * the whole of do_stop(), so a stop that blocked -- and one can, see
         * stop_wait_task()'s portMAX_DELAY in esp_websocket_client -- threw the
         * reason away and left "stopping" on screen instead. Observed on
         * hardware. Holding the notice and asserting it only once the session is
         * actually down keeps the two cases apart: "stopping" while it is
         * stopping, the reason once it has stopped.
         */
        /*
         * Retire an unanswered "forget?" offer. Nothing else repaints a stopped
         * device, so without this the question sits on the panel until someone
         * touches it -- and a question that never expires reads as a state the
         * device is stuck in.
         *
         * Edge-triggered on the flag, not on the window, so this runs once and
         * does not fight the label for the rest of the session.
         */
        /*
         * Read once, then clear only if it has not moved. Testing
         * s_forget_armed_ms and then calling in_forget_window() read it twice,
         * and a gesture landing on the LVGL task in between made this branch
         * fire against an offer that had already ended -- disarming a hold that
         * had just armed, or overwriting the 3 s "forgotten" deadline so
         * "stopped" painted over the confirmation on the same pass.
         *
         * Compare-and-clear rather than a lock: the LVGL task must not block
         * here, this is a 1 Hz janitor rather than a correctness path, and a new
         * arm within the same millisecond as the one being retired is the only
         * remaining collision. Failing it just means the offer stands one more
         * second.
         */
        const uint32_t armed = s_forget_armed_ms;
        if (armed != 0 && (now_ms() - armed) >= FORGET_CONFIRM_MS) {
            if (s_forget_armed_ms == armed) {
                s_forget_armed_ms = 0;
                s_notice_until_ms = now_ms();   /* repaint on this pass */
                ESP_LOGI(TAG, "EVT forget offer expired");
            }
        }

        /* Wrap-safe, the same idiom as the deadlines above. */
        if (s_notice_until_ms != 0 &&
            (now_ms() - s_notice_until_ms) < (UINT32_MAX / 2)) {
            /*
             * session_ctl owns what a stopped device says, including the case
             * this used to get wrong: a start attempted during the window leaves
             * "error" on screen, and painting "stopped" over it hides the one
             * thing the user needs to see. It runs BEFORE the bad-key
             * re-assert just below rather than after, so that a pass which owes
             * both ends on "check api key" instead of painting "stopped" over
             * it for a second.
             *
             * The deadline is cleared only once the repaint has ACTUALLY
             * happened. Clearing it first drops the notice on the floor whenever
             * the worker is mid-action -- and it can be, on a stopped device:
             * enter_display_test() issues an unconditional REQ_STOP that raises
             * s_busy and paints nothing. "forgotten" would then be stranded on
             * the panel with nothing left to take it off.
             */
            if (!session_ctl_is_busy()) {
                s_notice_until_ms = 0;
                session_ctl_repaint_idle_status();
            }
        }


        if (s_bad_key_notice && !session_ctl_is_running()) {
            ui_set_status("check api key", false);
        }

        if (CONFIG_SESSION_IDLE_TIMEOUT_S > 0 && session_ctl_is_running() &&
            ready_now && s_activity_ms != 0) {
            /* Unsigned, so correct across the 49.7-day wrap. */
            uint32_t quiet_ms = now_ms() - s_activity_ms;
            if (quiet_ms > (uint32_t)CONFIG_SESSION_IDLE_TIMEOUT_S * 1000) {
                ESP_LOGI(TAG, "EVT idletimeout after=%.1fs",
                         (double)quiet_ms / 1000.0);
                session_ctl_request_stop();
                /* So the next window does not ask again while the stop is in
                 * flight -- session_ctl has its own cooldown to respect. */
                note_activity();
            }
        }

        /*
         * Block COUNTS, not just sizes. A largest-free-block that falls without
         * total-free falling means the arena is being carved up; a fall in both
         * with one more allocated block means something simply took 29 kB. The
         * two failure modes need opposite fixes, and only these numbers tell them
         * apart.
         */
        multi_heap_info_t ih;
        heap_caps_get_info(&ih, MALLOC_CAP_INTERNAL);

        if (t.face_changed) {
            wait_ms = TELEMETRY_SWITCH_MS;
        }

        ESP_LOGI(TAG,
                 "TLM up=%.1f face=%s%s beh=%s src=%s sess=%s "
                 "frames=%" PRIu32 " fps=%.1f draw=%.1f/%.1f "
                 "amp=%.3f/%.3f low=%.2f/%.2f mid=%.2f/%.2f high=%.2f/%.2f "
                 "pk=%.3f/%.3f turns=%" PRIu32 " mic=%" PRIu32 " rx=%" PRIu32
                 " played=%" PRIu32 " drop=%" PRIu32 " updrop=%" PRIu32
                 " txdrop=%" PRIu32 " vadsup=%" PRIu32
                 " heap=%" PRIu32 " int=%u intmax=%u ifree=%u iblocks=%u"
                 " ialloc=%u bat=%d mv=%d chg=%d chgst=%d"
                 " rssi=%d bars=%d ch=%d",
                 (double)esp_timer_get_time() / 1000000.0,
                 t.face, t.face_changed ? "*" : "", t.behaviour, t.source,
                 !session_ctl_is_running() ? "stopped"
                     : dg_agent_is_ready() ? "ready" : "notready",
                 t.frames, t.fps, t.draw_avg_ms, t.draw_max_ms,
                 t.amp_avg, t.amp_max, t.low_avg, t.low_max,
                 t.mid_avg, t.mid_max, t.high_avg, t.high_max,
                 t.peak_mic, t.peak_agent,
                 s_turns, captured, s_audio_bytes, played, dropped,
                 dg_agent_audio_dropped(), dg_agent_transport_dropped(),
                 audio_io_uplink_suppressed(),
                 esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)ih.total_free_bytes, (unsigned)ih.free_blocks,
                 (unsigned)ih.allocated_blocks,
                 /* -1, not 0: "no reading" and "flat" have to be told apart by
                  * anything parsing this line. */
                 bat_ok ? bat.percent : -1, bat_ok ? bat.millivolts : -1,
                 (int)bat.charging,
                 /* The raw AXP2101 charge state, 0-5, or -1 before the first
                  * read. Which state a charge stopped in is the only thing that
                  * distinguishes "hit the configured target voltage" from
                  * "something else stopped it". */
                 bat.chg_state,
                 /* -1 for all three, on the same convention as bat= above: no
                  * association and a genuinely awful link have to be
                  * distinguishable by anything parsing this line, and 0 dBm
                  * would read as the best signal ever measured. */
                 sig_ok ? sig.rssi : -1,
                 sig_ok ? (int)sig.bars : -1,
                 sig_ok ? (int)sig.channel : -1);
    }
}
