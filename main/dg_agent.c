#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
/* The local patches' one accessor; see components/tcp_transport. */
#include "transport_ws_local.h"

#include "agent_name.h"
#include "agent_prompt.h"
#include "api_key.h"
#include "audio_io.h"
#include "battery.h"
#include "wifi_sta.h"
#include "dg_agent.h"
#include "faces.h"
#include "history_store.h"
#include "orb_colors.h"
#include "session_ctl.h"
#include "ui.h"
#include "voices.h"

static const char *TAG = "dg_agent";

#define DG_AGENT_URI "wss://agent.deepgram.com/v1/agent/converse"

/* Frames bigger than this are delivered to WEBSOCKET_EVENT_DATA in slices. */
#define WS_RX_BUFFER      4096
/* Ceiling on a reassembled JSON message. AgentThinking with a long chain of
 * thought is the biggest thing the server sends; 8 kB has plenty of room. */
#define JSON_REASSEMBLY_MAX 8192

/* Deepgram closes an idle Agent socket after ~10 s of no audio. */
#define KEEPALIVE_PERIOD_MS 5000

/*
 * Long-quiet fallback for the keepalive. NOT the primary rule -- see the task.
 *
 * This was 2000 and that was actively harmful: a congested socket blocks the
 * capture task, which stops the audio, which ages this clock past the threshold,
 * which fires the keepalive INTO the congestion that caused it. The stall
 * manufactured its own trigger, and killed a live session doing it.
 *
 * 6 s leaves margin under Deepgram's ~10 s while being longer than any stall
 * actually observed, so an ordinary congestion episode no longer trips it.
 */
#define KEEPALIVE_QUIET_MS 6000

#define WS_OPCODE_CONT   0x00
#define WS_OPCODE_TEXT   0x01
#define WS_OPCODE_BINARY 0x02

#define SEND_TIMEOUT pdMS_TO_TICKS(2000)

/*
 * Mic audio keeps the generous deadline, and the impatience lives in the
 * transport instead. This is worth understanding before shortening it again.
 *
 * The comment here used to explain why it had to be generous: a short deadline
 * made esp_transport_poll_write() time out, transport_ws returned 0, and the
 * WebSocket client treated a zero-length write as a broken socket and tore the
 * session down. 200 ms dropped the session every few seconds.
 *
 * LOCAL PATCH 2 in components/tcp_transport/transport_ws.c removes that trap --
 * a congested BINARY frame is now dropped and reported sent. But shortening THIS
 * was still wrong, twice over, and both were measured on the device:
 *
 *   1. This value is also the lock deadline. esp_websocket_client takes its
 *      client lock with it, so 200 ms produced a flood of "Could not lock
 *      ws-client within 200 timeout" and the client dropped mic frames before
 *      they ever reached the socket. CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK looks
 *      like the cure and is not -- it deadlocks the client outright; see the
 *      comment on it in sdkconfig.defaults. The deadline has to cover the lock.
 *
 *   2. It is also the deadline for the header and payload writes themselves.
 *      Those happen AFTER the frame is committed, and a timeout there tears a
 *      frame in half -- "esp_transport_write() returned 0" with no poll line
 *      above it, which is fatal and correctly so.
 *
 * The capture-task stall this was meant to fix is handled where it can be handled
 * safely: WS_AUDIO_POLL_MS caps the WRITABILITY POLL at 150 ms for audio frames,
 * so a congested socket costs one frame and 150 ms, while a frame that has
 * started going out still gets the full deadline to finish.
 *
 * The better structure is still to move the send off the capture task entirely.
 */
#define AUDIO_SEND_TIMEOUT SEND_TIMEOUT

static esp_websocket_client_handle_t s_client;
/* Written by the capture task, read by the keepalive task. One 32-bit store. */
static volatile uint32_t s_last_audio_ms;
static dg_agent_callbacks_t s_cb;
static volatile bool s_ready;
static TaskHandle_t s_keepalive_task;
static volatile bool s_suppress_state;

/*
 * THE UPLINK QUEUE, AND WHY THE SEND IS NOT ON THE CAPTURE TASK ANY MORE.
 *
 * esp_websocket_client takes its client lock for the duration of a send, with
 * AUDIO_SEND_TIMEOUT as the deadline (see the note on it above). While the
 * capture task owned that send, a congested socket meant the capture task was
 * the thing holding the lock -- and dg_agent_stop() had no way to know it, or to
 * wait for it. esp_websocket_client_stop() waits on STOPPED_BIT with
 * portMAX_DELAY (stop_wait_task() in the component), so a stop issued while a
 * send was wedged never returned: session_ctl's do_stop() stalled after painting
 * "stopping", s_busy latched, and every gesture was refused from then on. The
 * device kept rendering at 22 fps with the panel stuck on "stopping". Measured on
 * hardware, 2026-08-27.
 *
 * With the send on its own task, "no send is in flight" becomes something
 * dg_agent_stop() can assert before it touches the client -- which is the
 * property that was missing, and the reason this is worth the extra task.
 *
 * Four frames of slack. Each is 80 ms, so this is 320 ms -- enough to ride out a
 * retransmission burst, short enough that what finally goes out is still worth
 * hearing. Overflowing drops the NEWEST frame rather than growing a backlog of
 * stale speech, which is the same trade transport_ws.c's LOCAL PATCH 2 already
 * makes one layer down: drop a frame, never a session.
 */
#define AUDIO_QUEUE_FRAMES 4
/* From audio_io.h, not a literal repeated here: the queue is sized in whole
 * capture chunks, so the two must agree by construction. */
#define AUDIO_FRAME_BYTES  AUDIO_IO_CAPTURE_BYTES
#define AUDIO_QUEUE_BYTES  (AUDIO_QUEUE_FRAMES * (AUDIO_FRAME_BYTES + 16))

/* PSRAM: it is 10 kB that nothing touches from an ISR, and internal RAM is what
 * runs out first on this board once the display is up. */
static RingbufHandle_t s_audio_rb;
static TaskHandle_t s_send_task;
/*
 * Raised around a send that holds the client lock; what dg_agent_stop() waits on.
 *
 * One flag per sending task, because a shared counter would need atomics that
 * a volatile ++ does not give on this target. Covers the two sends that can be
 * in flight while a stop arrives: the audio frames, and the keepalive -- which
 * is TEXT, so transport_ws.c's LOCAL PATCH 2 cannot drop it and it blocks in
 * poll_write holding the lock (see the note in keepalive_task).
 *
 * BOTH ARE RAISED BEFORE THEIR TASK TESTS s_ready, NOT AFTER, and that ordering
 * is the property that makes them work at all. dg_agent_stop() clears s_ready
 * and then waits here; raising a flag after the test leaves a window in which
 * the sender has committed to a send and the stop cannot see it. Claim first,
 * test second, and every interleaving ends with either the stop waiting or the
 * send skipping. The full argument is at the top of audio_send_task().
 *
 * send_json() is deliberately NOT covered. It runs at session setup and on a
 * function-call response, not on the cadence a stop has to race, and adding it
 * would mean a flag written by two different tasks.
 */
static volatile bool s_sending;
static volatile bool s_sending_ka;

/*
 * No congestion heuristic lives here any more, and that is deliberate.
 *
 * Two were tried. The first read the send's return value, which is wrong for a
 * reason transport_ws.c states outright: LOCAL PATCH 2 drops a congested audio
 * frame and reports it as sent -- "strictly bounded, because the drop lies to
 * the caller" -- so it read healthy while fifty frames a minute went in the bin.
 * The second measured send duration, which does not lie, but was still a guess
 * guarding a call that must not be made at all. See dg_agent_stop().
 */
static volatile uint32_t s_audio_dropped;

/* Reassembly buffer for JSON messages split across several DATA events. */
static char *s_json;
static int s_json_len;
/*
 * Set when the message being reassembled has already overrun the buffer, so its
 * remaining slices are discarded instead of being taken for a new message.
 *
 * Without it, resetting s_json_len on overflow made the NEXT slices of the SAME
 * message accumulate from offset 0, and the fin slice then parsed a fragment
 * tail -- so an oversized message was reported as "unparseable message" against
 * a message that was perfectly well formed. The drop has to last until the end
 * of the message, not until the end of the slice.
 */
static bool s_json_dropping;

/*
 * When new_conversation was last called without clearing. 0 when nothing is
 * armed. See the handler for why forgetting takes two calls.
 */
/* volatile, like s_reload_pending: written on the WebSocket event task, cleared
 * on session_ctl's worker in dg_agent_stop(), read on both. */
static volatile uint32_t s_clear_armed_ms;
#define CLEAR_CONFIRM_WINDOW_MS 60000
/*
 * User turns seen when it was armed. The window says the confirmation is recent;
 * this says it is the USER'S.
 *
 * A time window alone is not a confirmation. Nothing stops the model calling the
 * function twice in one breath, and two calls inside sixty seconds would then
 * wipe the conversation without anyone having been asked -- for the one
 * operation here that cannot be undone. Requiring a user turn in between makes
 * the check the thing it claims to be: somebody spoke after the question.
 */
static uint32_t s_user_turns;
static volatile uint32_t s_clear_armed_turns;

/* Set when a setting change needs a new session; acted on once the agent has
 * finished saying so, then cleared. */
/* volatile: written on the WebSocket event task, the esp_timer task and
 * session_ctl's worker, and read on all three. */
static volatile bool s_reload_pending;
/* Same deferral as s_reload_pending, for the same reason: let the agent finish
 * the sentence before the socket it is speaking over goes away. */
static volatile bool s_test_pending;

/*
 * The conversation so far, replayed into the next session's Settings so that
 * reopening the socket resumes it rather than restarting it -- and persisted by
 * history_store.c so that a reboot does the same.
 *
 * A PACKED ARENA, NOT AN ARRAY OF TURNS. The old shape was six slots of 160
 * bytes, and it was wrong in both directions at once: it truncated exactly the
 * long assistant turns that carry the context, while six twelve-byte "yes
 * please" turns spent 960 bytes saying nothing. Sizing the store in BYTES and
 * evicting whole oldest turns until the new one fits gets 25-40 real turns out
 * of 3 kB instead of 6, in less memory than a 40 x 512 array would need.
 *
 * Each turn is stored NUL-terminated, one byte per turn that buys the whole of
 * history_to_json(): the content can be handed to cJSON in place, with no copy
 * onto a stack this file works hard to keep small.
 *
 * WHAT IS STORED IS NOT WHAT IS SENT, and the gap between the two is wide on
 * purpose. The arena is the device's memory; HISTORY_REPLAY_BYTES below is all
 * that goes into a Settings message, and it is a fraction of this. Keeping them
 * separate is what lets the device remember deeply without asking the internal
 * heap for something it cannot spare at the one moment it is most stretched --
 * see the note on that constant for the capture that settled the number.
 */
#define HISTORY_BYTES     3072  /* arena bytes, terminators included */
/*
 * How much of the arena goes on the wire. A HARD LIMIT, not a backstop -- an
 * earlier version of this comment called it a backstop and set it high enough
 * not to bind, and that was the bug.
 *
 * MEASURED ON THE DEVICE, 2026-09-01. At 16 turns / 6144 bytes the Settings
 * message came out at 20,265 bytes and the session began flapping: two of five
 * connects died with
 *
 *     E esp-aes: Failed to allocate memory
 *     E esp-tls-mbedtls: write error :-0x0001
 *     E dg_agent: failed to send Settings
 *
 * followed by a five-second reconnect, a fresh 20 kB attempt, and the same coin
 * toss again. The TLM line across that capture has `intmax` -- the largest free
 * INTERNAL block -- swinging between 27,648 and 6,144 bytes, and the writes fail
 * on the dips. Sending Settings is the worst moment to ask for internal memory:
 * the TLS handshake has just finished, and building the message churns ten small
 * cJSON allocations per replayed turn through the same heap.
 *
 * So the limit is not about the lwIP send buffer (23040, which 20 kB technically
 * fits) and not about WebSocket framing. It is about how much internal heap this
 * device can be asked for at the one moment it has least to give.
 *
 * The numbers below restore the envelope the firmware ran in for months before
 * the arena existed: six turns of at most 160 characters, about 1.2 kB of
 * history on a ~14 kB base. Depth on FLASH is deliberately not limited to this
 * -- the device remembers 25-40 turns and replays the most recent handful.
 *
 * Raising either of these means re-running that capture. `send_json` logs the
 * byte count and `main.c`'s TLM line logs `intmax`; the failure is not
 * reproducible on demand, so a short run that happens to work proves nothing.
 */
#define HISTORY_REPLAY_BYTES 1280
#define HISTORY_REPLAY_MAX_TURNS 6
/*
 * Per-turn JSON overhead: the braces, three keys and their quoting come to 48 B
 * before escaping. Rounded up, because cJSON escapes quotes and newlines in the
 * content and this estimate is only worth making if it is the pessimistic one.
 */
#define HISTORY_TURN_JSON_OVERHEAD 64
#define HISTORY_MAX_TURNS 40    /* index slots; a cap on count, not on content */
#define HISTORY_TURN_MAX  512   /* per-turn truncation, generous by design */

enum {
    HISTORY_ROLE_USER = 0,
    HISTORY_ROLE_ASSISTANT = 1,
};

typedef struct {
    uint16_t off;               /* into the arena, at the first content byte */
    uint16_t len;               /* content bytes, excluding the terminator */
    uint8_t role;               /* HISTORY_ROLE_* */
} history_idx_t;

/*
 * PSRAM, allocated once in dg_agent_init(). history_to_json() runs on the
 * WebSocket task, where internal RAM is scarcest and where the largest free
 * block is the number this project already watches on every toggle. Nothing
 * here is touched from an ISR.
 */
static char *s_arena;
static history_idx_t s_idx[HISTORY_MAX_TURNS];
static int s_history_count;         /* turns held, oldest at index 0 */
static size_t s_history_bytes;      /* arena bytes in use, terminators included */

/*
 * TWO TASKS TOUCH THE ARENA. history_add() runs on the WebSocket event task as
 * turns arrive; dg_agent_flush_history() reads the whole thing on session_ctl's
 * worker. Without this, a turn landing mid-copy writes a record whose index and
 * arena disagree -- and because the record is validated on load, the cost is not
 * one garbled turn but the entire history being discarded at the next boot. The
 * one thing this feature exists to prevent.
 *
 * SAFE TO HOLD, unlike almost any other lock in this file. It is ordered inside
 * nothing: neither holder calls into esp_websocket_client while holding it, so
 * it cannot participate in the transmit-mutex inversion docs/session-control.md
 * describes. And the flush copies under the lock but writes to flash outside it,
 * so the WebSocket task waits on a memcpy, never on a sector erase.
 */
static SemaphoreHandle_t s_history_lock;

/*
 * Set by a clear, cleared when the next session goes live.
 *
 * WITHOUT IT, FORGETTING DOES NOT TAKE. The clear happens inside the function
 * handler, and the agent then SPEAKS its confirmation -- which comes back as a
 * ConversationText and goes straight into the history that was just emptied.
 * The reload a moment later replays that turn, suppresses the greeting, tells
 * the model it is resuming, and writes the whole thing to flash. The user asked
 * to be forgotten and got a device that remembers being asked.
 *
 * Released at SettingsApplied rather than at the reload, because that is the
 * point at which the new session -- the one with no context in it -- is actually
 * the session whose turns are being recorded.
 */
/* volatile: set from whichever task asked to forget, read on the WebSocket
 * task. A bool store is indivisible here; what this rules out is the compiler
 * caching the read across the message loop. */
static volatile bool s_history_frozen;

/*
 * The flush's staging buffer, sized for the largest record the constants above
 * can produce. Held for the life of the device; see dg_agent_flush_history().
 */
#define HISTORY_RECORD_MAX (sizeof(uint16_t) + sizeof(s_idx) + HISTORY_BYTES)
static uint8_t *s_record;

/*
 * At build time, because the runtime failure is a bad one to debug: a record
 * larger than a slot passes this file's own size check, comes back from the
 * store as ESP_ERR_INVALID_SIZE, and after three attempts latches persistence
 * off. The device then simply stops remembering, with one warning nobody was
 * watching for. Raising HISTORY_BYTES or HISTORY_MAX_TURNS should fail here
 * instead, loudly, with the number to change in the message.
 */
_Static_assert(HISTORY_RECORD_MAX <= HISTORY_STORE_MAX_PAYLOAD,
               "history record outgrew a flash slot -- lower HISTORY_BYTES or "
               "HISTORY_MAX_TURNS, or widen the store");

/*
 * Consecutive failed saves, and the latch that stops trying. A missing partition
 * or a dead sector is not a transient, and retrying it forever costs a worker
 * wakeup every debounce for nothing.
 */
#define HISTORY_SAVE_ATTEMPTS 3
static int s_save_failures;
static bool s_save_disabled;

static const char *history_role_name(uint8_t role)
{
    return (role == HISTORY_ROLE_ASSISTANT) ? "assistant" : "user";
}

/* Anything that is not "assistant" is the user; the server sends only the two. */
static uint8_t history_role_id(const char *role)
{
    return (strcmp(role, "assistant") == 0) ? HISTORY_ROLE_ASSISTANT
                                            : HISTORY_ROLE_USER;
}

bool dg_agent_has_history(void)
{
    return s_history_count > 0;
}

/*
 * PERSISTENCE IS DEFERRED, AND DEFERRED TWICE.
 *
 * history_add() runs on the WebSocket client's own event task, inside the
 * ConversationText branch of handle_json(). A sector erase is tens of
 * milliseconds with the flash cache disabled on both cores; spending that here
 * stalls audio delivery and the KeepAlive at the exact moment the agent is
 * about to speak. So the turn only sets a flag and arms a timer.
 *
 * The timer callback does not write either -- esp_timer's task runs at priority
 * 22, above lwIP, and blocking flash there is no better than blocking here. It
 * asks session_ctl's worker, which exists for slow blocking work off the audio
 * core, and that is where the write actually happens.
 *
 * The debounce is restarted by every turn, so a user turn and the reply it
 * provokes coalesce into ONE write rather than two. The cost is the exposure
 * window: up to HISTORY_FLUSH_DEBOUNCE_MS of conversation lost to a brownout
 * nobody announced. Against the alternative -- writing only at a clean stop,
 * which survives none of the crashes this feature exists for -- that is the
 * right end of the trade.
 */
#define HISTORY_FLUSH_DEBOUNCE_MS 1500
/* How far the debounce may push the deadline out in total. See the cap in
 * history_mark_dirty() for why a restarting debounce needs one. */
#define HISTORY_FLUSH_MAX_DEFER_MS 5000

static esp_timer_handle_t s_flush_timer;
/* When the current unwritten stretch began, for the deferral cap. */
static uint32_t s_dirty_since_ms;
/* volatile, like every other flag here that crosses a task boundary: set on the
 * WebSocket event task, cleared and read on session_ctl's worker. */
static volatile bool s_history_dirty;

/*
 * Backstop for the reload a confirmed forget schedules.
 *
 * The deferral to AgentAudioDone is right when it works -- it lets the agent
 * finish saying "it is forgotten" before the socket goes -- but main.c's own
 * notes record that Deepgram sent AgentAudioDone ZERO times across a 12-minute
 * run. Every other user of that deferral degrades to "the setting applies a bit
 * later". This one does not: without the reload the model still holds every turn
 * server-side after saying it forgot them, and s_history_frozen -- released only
 * at SettingsApplied -- stays set for the rest of the session, so nothing said
 * afterwards is recorded or persisted. Silently.
 *
 * So the forget path arms this as well, and whichever fires first wins.
 */
static esp_timer_handle_t s_reload_backstop;
/*
 * ONE TIMER EACH, not one shared between them. Sharing looked economical and was
 * wrong twice over: arming for a voice change reset the deadline of a display
 * test already waiting, pushing it out by another eight seconds for every
 * setting the user touched -- and since AgentAudioDone is the path that mostly
 * does not happen, the backstop IS the schedule, not a safety net. The other
 * half was one firing consuming both flags, which reloaded the session and then
 * tore that fresh session down for the test a moment later.
 */
static esp_timer_handle_t s_test_backstop;
#define RELOAD_BACKSTOP_MS 8000

/*
 * Ask for a session reload once the agent has finished the sentence it is
 * speaking, and guarantee that it happens.
 *
 * EVERY deferred reload goes through here, which is the point of the function
 * existing. The flag alone is only half of it: AgentAudioDone is what consumes
 * it, and main.c records Deepgram sending that ZERO times across a 12-minute
 * run. A voice change that relied on the flag alone was persisted to NVS and
 * then never applied for the rest of the session -- the same silent stall the
 * forget path could not afford, arriving at a different function.
 */
static void schedule_reload(void)
{
    s_reload_pending = true;
    if (s_reload_backstop != NULL) {
        esp_timer_stop(s_reload_backstop);
        esp_timer_start_once(s_reload_backstop, RELOAD_BACKSTOP_MS * 1000);
    }
}

/*
 * Same deferral, same hazard, same answer, and its OWN timer -- see the note on
 * s_test_backstop for what sharing one cost. The display test waits on
 * AgentAudioDone exactly as a reload does, so it inherits the same problem: the
 * message that consumes it mostly does not arrive, leaving the test either never
 * running or running unprompted much later.
 */
static void schedule_test(void)
{
    s_test_pending = true;
    if (s_test_backstop != NULL) {
        esp_timer_stop(s_test_backstop);
        esp_timer_start_once(s_test_backstop, RELOAD_BACKSTOP_MS * 1000);
    }
}

/*
 * Test-and-clear a deferred flag, indivisibly.
 *
 * Two tasks race for each of these: the WebSocket event task when
 * AgentAudioDone arrives, and the esp_timer task when the backstop expires. Both
 * read the flag before either clears it, so a plain test-then-clear lets both
 * win -- and two winners is two reloads, a teardown and a TLS handshake done
 * twice back to back. The window is microseconds and neither path is hot, which
 * is exactly the kind of race that survives a decade and then reproduces once.
 */
static bool take_flag(volatile bool *flag)
{
    static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    bool had;
    portENTER_CRITICAL(&lock);
    had = *flag;
    *flag = false;
    portEXIT_CRITICAL(&lock);
    return had;
}

/*
 * NO s_ready TEST HERE, unlike the reload's, and the asymmetry is the point. A
 * display test is a display test: it needs the panel, not a socket, and the user
 * asked for it out loud. Gating it on a live session would strand it exactly
 * when the session is the thing misbehaving -- and s_ready goes false on a
 * socket drop as well as on a stop, so that is not a rare corner.
 */
static void test_backstop_cb(void *arg)
{
    (void)arg;
    if (take_flag(&s_test_pending)) {
        ESP_LOGW(TAG, "AgentAudioDone never came -- starting the display test anyway");
        if (s_cb.on_display_test_required) {
            s_cb.on_display_test_required(s_cb.ctx);
        }
    }
}

static void reload_backstop_cb(void *arg)
{
    (void)arg;

    /*
     * s_ready as well as the flag here, because this fires on the esp_timer task
     * and can land on a session that has just been torn down. dg_agent_stop()
     * clears s_ready first and stops this timer second, but esp_timer_stop()
     * does not wait for a callback already running -- and a reload arriving
     * after a deliberate stop would open a fresh session seconds after the user
     * ended theirs.
     */
    if (s_ready && take_flag(&s_reload_pending)) {
        ESP_LOGW(TAG, "AgentAudioDone never came -- reloading anyway");
        /*
         * The _soon() variant, not the immediate one. This fires on the
         * esp_timer task against an 8 s deadline, so it can land in the window
         * between a user's tap being accepted and the worker starting the stop
         * -- and session_ctl_request_reload() would overwrite that pending
         * toggle, finishing the stop and then reopening the session the user
         * just ended. s_ready alone does not close that window; a flag that
         * cannot displace a request does.
         */
        session_ctl_request_reload_soon();
    }
}

static void history_flush_timer_cb(void *arg)
{
    (void)arg;
    session_ctl_request_history_flush();
}

static void history_mark_dirty(void);

/*
 * A save did not happen. Retry a few times, then stop trying and say so once.
 *
 * The retry has to re-arm the timer, not just re-raise the flag: the debounce
 * has already fired, DISCONNECTED is suppressed on a deliberate stop, and if the
 * failure was the stop's own flush there is no later turn and no later stop
 * coming. A bare flag would leave the conversation unwritten behind a warning
 * nobody reads as fatal.
 *
 * And it has to be able to give up. history_store.h documents a missing
 * partition as something to run without rather than refuse over, and an
 * unbounded re-arm turns exactly that case into a timer waking the worker every
 * debounce for the life of the device, for a write that cannot ever succeed.
 */
static void history_note_save_failure(esp_err_t err)
{
    s_save_failures++;
    if (s_save_failures < HISTORY_SAVE_ATTEMPTS) {
        history_mark_dirty();
        ESP_LOGW(TAG, "history not saved (%s), retrying (%d of %d)",
                 esp_err_to_name(err), s_save_failures, HISTORY_SAVE_ATTEMPTS);
        return;
    }
    s_save_disabled = true;
    ESP_LOGE(TAG, "history not saved (%s) after %d attempts -- conversations "
                  "will no longer persist",
             esp_err_to_name(err), s_save_failures);
}

static void history_mark_dirty(void)
{
    /*
     * The latch means what it says. Without this, giving up only stopped the
     * RETRY re-arm: ordinary turns kept arming the timer and waking
     * session_ctl's worker every debounce for the life of a device whose
     * `storage` partition is missing -- exactly the behaviour the latch exists
     * to end.
     */
    if (s_save_disabled) {
        return;
    }

    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!s_history_dirty) {
        s_history_dirty = true;
        s_dirty_since_ms = now;
    }
    if (s_flush_timer == NULL) {
        return;
    }

    /*
     * A CAP ON THE DEFERRAL, because the debounce restarts on every turn. A
     * brisk back-and-forth of turns closer together than the debounce would
     * push the deadline out ahead of itself indefinitely and keep the whole
     * conversation out of flash -- the "~1.5 s of exposure" this design
     * advertises would quietly become the length of the conversation.
     *
     * The cap is on the DEADLINE, not merely on whether the debounce restarts.
     * Simply declining to restart still leaves the existing timer to run out,
     * which puts the true bound a whole debounce past the cap; arming the
     * remainder instead makes the number here the number that happens.
     */
    const uint32_t elapsed = now - s_dirty_since_ms;
    uint32_t delay = HISTORY_FLUSH_DEBOUNCE_MS;
    if (elapsed + delay > HISTORY_FLUSH_MAX_DEFER_MS) {
        if (elapsed >= HISTORY_FLUSH_MAX_DEFER_MS) {
            return;     /* already past it; whatever is armed is soon enough */
        }
        delay = HISTORY_FLUSH_MAX_DEFER_MS - elapsed;
    }

    /* Restart, not start: esp_timer_start_once() fails outright on an already
     * armed timer, and the restart is what buys the coalescing above. */
    esp_timer_stop(s_flush_timer);
    esp_timer_start_once(s_flush_timer, (uint64_t)delay * 1000);
}

/*
 * Drop the oldest turn and slide the rest down. O(n) over at most 3 kB, a few
 * times per turn at worst, on a task that is about to do TLS anyway -- the
 * alternative is a wrap-around cursor that makes every other function here
 * reason about two ranges instead of one.
 */
static void history_evict_oldest(void)
{
    if (s_history_count == 0) {
        return;
    }
    const size_t drop = (size_t)s_idx[0].len + 1;
    memmove(s_arena, s_arena + drop, s_history_bytes - drop);
    s_history_bytes -= drop;
    s_history_count--;
    for (int i = 0; i < s_history_count; i++) {
        s_idx[i] = s_idx[i + 1];
        s_idx[i].off = (uint16_t)(s_idx[i].off - drop);
    }
}

static void history_add(const char *role, const char *content)
{
    if (s_arena == NULL) {
        return;
    }

    size_t len = strlen(content);
    if (len > HISTORY_TURN_MAX) {
        len = HISTORY_TURN_MAX;
    }
    /*
     * A turn that cannot fit even an empty arena would evict everything and
     * still not fit, so the loop below would spin on an empty ring. Clamp first;
     * losing the tail of one enormous turn beats not returning.
     */
    if (len + 1 > HISTORY_BYTES) {
        len = HISTORY_BYTES - 1;
    }

    /*
     * Back off to a character boundary. A WebSocket TEXT frame must be valid
     * UTF-8, so half a multi-byte sequence in the replayed context is a 1007
     * close -- and now that these bytes are on flash it would be one on every
     * reconnect and every reboot, not a transient. A continuation byte is
     * 10xxxxxx; walking back to the first byte that is not one lands on the lead
     * byte of the sequence being cut, which is exactly where to stop.
     */
    while (len > 0 && ((unsigned char)content[len] & 0xC0) == 0x80) {
        len--;
    }

    xSemaphoreTake(s_history_lock, portMAX_DELAY);

    /*
     * Inside the lock, because dg_agent_clear_history() sets it inside the lock.
     * Tested outside, a clear landing in between would let this turn -- the
     * agent's own "it is forgotten" -- into the arena that was just emptied, and
     * on to flash, which is the whole failure the freeze exists to stop.
     * Unreachable at today's call sites; that is not a reason to write it the
     * fragile way.
     */
    if (s_history_frozen) {
        xSemaphoreGive(s_history_lock);
        return;
    }

    while (s_history_count >= HISTORY_MAX_TURNS ||
           s_history_bytes + len + 1 > HISTORY_BYTES) {
        history_evict_oldest();
    }

    memcpy(s_arena + s_history_bytes, content, len);
    s_arena[s_history_bytes + len] = '\0';
    s_idx[s_history_count] = (history_idx_t){
        .off = (uint16_t)s_history_bytes,
        .len = (uint16_t)len,
        .role = history_role_id(role),
    };
    s_history_bytes += len + 1;
    s_history_count++;

    xSemaphoreGive(s_history_lock);

    history_mark_dirty();
}

/*
 * Oldest first, which is the order the server expects -- but chosen newest
 * first, because when the budget runs out it is the OLDEST turns that should go.
 * So the start index is found by walking back from the end, and the emit runs
 * forward from there.
 */
static void history_to_json(cJSON *agent)
{
    if (s_history_lock == NULL) {
        return;
    }
    /*
     * Under the lock, all of it. This runs on the WebSocket task and
     * dg_agent_clear_history() can empty the arena from the LVGL task; a clear
     * landing between the count read and the emit takes `first` negative and
     * hands cJSON an arbitrary pointer. The gesture that does that is gated on
     * the session being down, so this cannot happen today -- but "cannot happen
     * because of a check in another file" is a poor guard for a wild pointer,
     * and the other holder only ever does a memcpy or a zeroing.
     */
    xSemaphoreTake(s_history_lock, portMAX_DELAY);
    if (s_history_count == 0) {
        xSemaphoreGive(s_history_lock);
        return;
    }

    int first = s_history_count;
    size_t budget = HISTORY_REPLAY_BYTES;
    while (first > 0 && (s_history_count - first) < HISTORY_REPLAY_MAX_TURNS) {
        const size_t cost = (size_t)s_idx[first - 1].len + HISTORY_TURN_JSON_OVERHEAD;
        if (cost > budget) {
            break;
        }
        budget -= cost;
        first--;
    }
    /* A single turn longer than the whole budget would otherwise replay nothing
     * at all, which reads to the model as a new conversation. One is better. */
    if (first == s_history_count) {
        first = s_history_count - 1;
    }

    if (first > 0) {
        ESP_LOGI(TAG, "replaying %d of %d turns (send budget)",
                 s_history_count - first, s_history_count);
    }

    cJSON *messages = cJSON_AddArrayToObject(
        cJSON_AddObjectToObject(agent, "context"), "messages");

    for (int i = first; i < s_history_count; i++) {
        const history_idx_t *t = &s_idx[i];
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "type", "History");
        cJSON_AddStringToObject(m, "role", history_role_name(t->role));
        /* In place: cJSON copies, and the arena keeps each turn terminated
         * exactly so this needs no buffer of its own. */
        cJSON_AddStringToObject(m, "content", s_arena + t->off);
        cJSON_AddItemToArray(messages, m);
    }

    xSemaphoreGive(s_history_lock);
}

/*
 * Read back what dg_agent_flush_history() wrote. The mirror of it, and the two
 * must be changed together -- which is why the layout version lives in
 * history_store.c's magic, so a mismatch throws the record away rather than
 * misreading it.
 *
 * Every field is validated against this build's limits before it is trusted.
 * The record comes off flash, and flash outlives the firmware that wrote it.
 */
static void history_load(void)
{
    uint8_t *rec = heap_caps_malloc(HISTORY_STORE_MAX_PAYLOAD, MALLOC_CAP_SPIRAM);
    if (rec == NULL) {
        return;
    }

    size_t len = 0;
    if (history_store_load(rec, HISTORY_STORE_MAX_PAYLOAD, &len) != ESP_OK ||
        len < sizeof(uint16_t)) {
        free(rec);
        return;
    }

    uint16_t count;
    memcpy(&count, rec, sizeof(count));
    if (count > HISTORY_MAX_TURNS) {
        ESP_LOGW(TAG, "saved history holds %u turns, this build caps at %d -- discarding",
                 count, HISTORY_MAX_TURNS);
        free(rec);
        return;
    }

    const size_t idx_bytes = (size_t)count * sizeof(history_idx_t);
    if (len < sizeof(count) + idx_bytes) {
        free(rec);
        return;
    }
    memcpy(s_idx, rec + sizeof(count), idx_bytes);

    const size_t arena_bytes = len - sizeof(count) - idx_bytes;
    if (arena_bytes > HISTORY_BYTES) {
        ESP_LOGW(TAG, "saved history is %zu B, arena is %d -- discarding",
                 arena_bytes, HISTORY_BYTES);
        free(rec);
        return;
    }
    memcpy(s_arena, rec + sizeof(count) + idx_bytes, arena_bytes);
    free(rec);

    /*
     * The index must TILE the arena exactly -- turns butted end to end from
     * offset 0, each one terminated, together accounting for every byte. Merely
     * checking that each turn lies inside the arena is not enough: a record
     * claiming zero turns over a non-empty arena would pass that and install
     * count == 0 with bytes > 0, and history_add()'s eviction loop would then
     * spin forever with the mutex held, having nothing left to evict and still
     * no room. The exact failure its own clamp exists to prevent.
     *
     * Worth being strict about. This comes off flash, and flash outlives the
     * firmware that wrote it.
     */
    size_t walk = 0;
    for (int i = 0; i < count; i++) {
        if (s_idx[i].off != walk || walk + s_idx[i].len >= arena_bytes ||
            s_arena[walk + s_idx[i].len] != '\0') {
            ESP_LOGW(TAG, "saved history failed its bounds check -- discarding");
            return;
        }
        walk += (size_t)s_idx[i].len + 1;
    }
    if (walk != arena_bytes) {
        ESP_LOGW(TAG, "saved history does not tile its arena -- discarding");
        return;
    }

    s_history_count = count;
    s_history_bytes = arena_bytes;
    ESP_LOGI(TAG, "resumed %d turns, %zu bytes", s_history_count, s_history_bytes);
}

/*
 * Forget the conversation, on flash as well as in RAM.
 *
 * Nothing calls this implicitly any more. Stopping a session -- by tap, by BOOT
 * or by the idle timeout -- keeps the conversation, because the device being
 * quiet is not the same as the conversation being over. Clearing is a thing the
 * user asks for and confirms; see the new_conversation function and the
 * hold-again gesture in main.c.
 *
 * The store ends up recording an empty history rather than erasing its slots,
 * so "this was deliberately forgotten" and "this device has never talked to
 * anyone" stay distinguishable in a serial capture.
 *
 * DOES NOT TOUCH FLASH, and that is not an oversight. Its two callers are the
 * new_conversation handler, on the WebSocket event task, and the hold-again
 * gesture, on the LVGL task with the LVGL lock held -- and a sector erase is
 * tens of milliseconds neither of them can spend. So this clears RAM and marks
 * the result dirty; session_ctl's worker writes the empty record through the
 * same path every ordinary turn uses. The exposure is the same 1.5 s debounce,
 * and a power loss inside it costs a clear that has to be asked for again --
 * the harmless direction to fail in.
 */
void dg_agent_clear_history(void)
{
    /* Whatever armed the voice path, this answered it. Leaving it armed would
     * make a later, unrelated new_conversation call skip its confirmation. */
    s_clear_armed_ms = 0;

    /* Same guard as history_add() and the flush: init can fail, and the device
     * runs without continuity rather than panicking on a null handle. */
    if (s_history_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_history_lock, portMAX_DELAY);
    s_history_count = 0;
    s_history_bytes = 0;
    s_history_frozen = true;
    xSemaphoreGive(s_history_lock);

    /*
     * Lift the give-up latch for this write. Persistence latching off is a
     * reasonable answer to "the device cannot save any more"; it is the wrong
     * answer to "the user asked to be forgotten", because the previous record is
     * still on flash and history_load() would resurrect the whole conversation
     * at the next boot -- silently undoing the one operation here that is meant
     * to be irreversible. A forget gets a fresh set of attempts.
     */
    s_save_disabled = false;
    s_save_failures = 0;

    history_mark_dirty();
    /*
     * And ask for the write NOW rather than waiting out the debounce. The
     * gesture path reaches this on a STOPPED device, so there is no do_stop()
     * flush coming along behind it -- the debounce is the only thing that would
     * ever write the empty record. Power off in that second and a half, having
     * just read "forgotten", and the whole conversation comes back at the next
     * boot. Non-blocking and safe from any task, which is the point of it.
     */
    session_ctl_request_history_flush();
}

/*
 * Write the conversation out if anything has changed since the last write.
 *
 * BLOCKS on the flash erase, and is called only from session_ctl's worker --
 * see the note on history_mark_dirty() for why that matters and why no other
 * task may call it.
 */
void dg_agent_flush_history(void)
{
    if (!s_history_dirty || s_arena == NULL || s_save_disabled) {
        return;
    }
    s_history_dirty = false;

    xSemaphoreTake(s_history_lock, portMAX_DELAY);

    /*
     * The index goes ahead of the arena so a reader knows how to cut the bytes
     * up: count first, then the used index entries, then exactly the bytes in
     * use -- an empty tail is not worth the erase time.
     */
    const uint16_t count = (uint16_t)s_history_count;
    const size_t idx_bytes = (size_t)s_history_count * sizeof(history_idx_t);
    const size_t total = sizeof(count) + idx_bytes + s_history_bytes;

    /*
     * Against HISTORY_RECORD_MAX, the size of the DESTINATION, not against the
     * store's slot limit. The two differ by 762 bytes today and the memcpys
     * below are bounded by the smaller one, so checking the larger leaves room
     * to overflow an internal-RAM heap block the day someone raises
     * HISTORY_BYTES or HISTORY_MAX_TURNS -- silently, and with the store's own
     * limit still satisfied. A buffer check belongs to the buffer.
     */
    if (s_record == NULL || total > HISTORY_RECORD_MAX) {
        /* The size case cannot happen with the constants above -- 2 + 240 + 3072
         * is exactly HISTORY_RECORD_MAX -- but it would otherwise be a silent
         * corruption rather than a conversation that stops persisting. */
        ESP_LOGW(TAG, "history not saved: %s",
                 (s_record == NULL) ? "no record buffer"
                                    : "record larger than the buffer");
        xSemaphoreGive(s_history_lock);
        /* Through the same counted retry as a failed write, rather than just
         * re-flagging: a bare flag re-arms nothing, and this branch would
         * otherwise both never retry and never give up. Unreachable at today's
         * constants, and it should still fail the way its sibling does. */
        history_note_save_failure(ESP_ERR_INVALID_SIZE);
        return;
    }

    /*
     * Assembled straight into s_record, which is allocated once at init. Three
     * reasons it is not a malloc here, and each one on its own is sufficient:
     * a local array would be ~3 kB of frame on session_ctl's worker; an
     * allocation would happen while holding s_history_lock, so the WebSocket
     * task would block on the heap rather than on the memcpy this design
     * promises; and a flush that can fail to allocate is a flush that can stop
     * persisting under memory pressure, which is when it matters most.
     *
     * INTERNAL RAM, not PSRAM, and that is the point of it. esp_flash_write()
     * takes its direct_write path only for a source it can DMA from; handed a
     * PSRAM buffer it copies in 32-byte pieces, disabling and re-enabling the
     * cache on BOTH CORES for each one -- about a hundred stalls where the
     * deferral bought one.
     */
    memcpy(s_record, &count, sizeof(count));
    memcpy(s_record + sizeof(count), s_idx, idx_bytes);
    memcpy(s_record + sizeof(count) + idx_bytes, s_arena, s_history_bytes);
    xSemaphoreGive(s_history_lock);

    /* Outside the lock: the erase blocks for tens of milliseconds and the
     * WebSocket task must never wait that long to record a turn. */
    const esp_err_t err = history_store_save(s_record, total);

    if (err != ESP_OK) {
        /*
         * Re-arm, do not merely re-flag. Setting the flag alone leaves nothing
         * to act on it: the debounce has already fired, DISCONNECTED is
         * suppressed on a deliberate stop, and if this WAS the stop's flush
         * there is no later turn and no later stop coming. The conversation
         * would go unwritten with a warning nobody reads as fatal.
         *
         * BUT A RETRY MUST BE ABLE TO GIVE UP. history_store.h documents a
         * missing partition as something to run without rather than refuse over,
         * and an unbounded re-arm turns exactly that case into a timer waking
         * the worker every 1.5 s for the life of the device -- burning the flush
         * path on a write that cannot ever succeed. So a few attempts, then
         * persistence latches off and says so once.
         */
        history_note_save_failure(err);
        return;
    }
    s_save_failures = 0;
}

static void set_state(dg_agent_state_t state)
{
    s_ready = (state == DG_AGENT_READY);
    /* s_ready still tracks reality while suppressed -- only the notification is
     * withheld, so nothing starts sending audio into a closing socket. */
    if (s_cb.on_state && !s_suppress_state) {
        s_cb.on_state(state, s_cb.ctx);
    }
}

void dg_agent_suppress_state_events(bool suppress)
{
    s_suppress_state = suppress;
}

static esp_err_t send_json(cJSON *root, const char *what)
{
    char *text = cJSON_PrintUnformatted(root);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_DEEPGRAM_LOG_WIRE_JSON
    ESP_LOGI(TAG, "-> %s", text);
#endif

    int sent = esp_websocket_client_send_text(s_client, text, strlen(text), SEND_TIMEOUT);
    esp_err_t err = (sent < 0) ? ESP_FAIL : ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to send %s", what);
    } else {
        ESP_LOGI(TAG, "sent %s (%d bytes)", what, sent);
    }

    cJSON_free(text);
    return err;
}

/*
 * Applies a voice and tells the agent it worked.
 *
 * ORDERING MATTERS. UpdateSpeak goes first, because the FunctionCallResponse is
 * what prompts the agent's next turn and the next turn is exactly when Flux
 * starts using the new voice. Send them the other way round and the
 * confirmation is spoken in the old voice, one turn late.
 *
 * Both are plain text frames, so sending them from here -- the WebSocket task,
 * inside event dispatch -- is safe: client->lock is recursive and this task
 * already owns it. send_settings() has always done the same thing from
 * WEBSOCKET_EVENT_CONNECTED. What must never happen here is a stop/close, which
 * the client refuses by comparing task handles.
 */
static void send_function_response(const char *id, const char *name, const char *content)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON_AddStringToObject(root, "type", "FunctionCallResponse");
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "content", content);
    send_json(root, "FunctionCallResponse");
    cJSON_Delete(root);
}


/*
 * WHY THIS REOPENS THE SESSION
 *
 * UpdateSpeak is the documented way to change the voice in place. On this
 * account it answers SpeakUpdated and then changes nothing -- reproduced with a
 * bare UpdateSpeak sent nowhere near a function call, with both a Flux v2 and
 * an Aura v1 provider, against JSON matching the documented example exactly. A
 * fresh Settings message does work, so that is what this does.
 *
 * The cost is a new session, which is why dg_agent keeps a short history and
 * replays it -- see history_to_json().
 */
/*
 * `arguments` arrives as a JSON-encoded *string*, not a nested object, so it
 * needs a second parse. Caller owns the result and must cJSON_Delete it.
 */
static cJSON *function_args(const cJSON *fn)
{
    const cJSON *args_str = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
    if (!cJSON_IsString(args_str)) {
        return NULL;
    }
    return cJSON_Parse(args_str->valuestring);
}

static void handle_function_call(const cJSON *root)
{
    const cJSON *functions = cJSON_GetObjectItemCaseSensitive(root, "functions");
    if (!cJSON_IsArray(functions)) {
        return;
    }

    const cJSON *fn;
    cJSON_ArrayForEach(fn, functions) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(fn, "id");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(fn, "name");
        const cJSON *client_side = cJSON_GetObjectItemCaseSensitive(fn, "client_side");
        if (!cJSON_IsString(id) || !cJSON_IsString(name)) {
            continue;
        }
        /* false means Deepgram already ran it against an endpoint of ours. */
        if (cJSON_IsBool(client_side) && !cJSON_IsTrue(client_side)) {
            continue;
        }

        char content[128];

        if (strcmp(name->valuestring, "reset_voice") == 0) {
            const voice_t *v = voices_default();
            voices_reset();
            schedule_reload();
            snprintf(content, sizeof(content),
                     "Switching back to %s. Tell the user you are changing voice now.",
                     v->name);
            send_function_response(id->valuestring, name->valuestring, content);
            continue;
        }

        if (strcmp(name->valuestring, "set_name") == 0) {
            const char *wanted = NULL;
            cJSON *nargs = function_args(fn);
            if (nargs != NULL) {
                const cJSON *n = cJSON_GetObjectItemCaseSensitive(nargs, "name");
                if (cJSON_IsString(n)) {
                    wanted = n->valuestring;
                }
            }

            /*
             * NO RELOAD, unlike set_voice. The voice is a Settings field and
             * this account ignores UpdateSpeak, so changing it costs a new
             * session; the name is only ever text inside the prompt, and telling
             * the model here is enough for this session. {{name}} carries it
             * from the next Settings onwards, so the two agree again as soon as
             * anything reconnects.
             */
            esp_err_t nerr = agent_name_set(wanted);
            if (nerr != ESP_OK) {
                /* Say so rather than staying silent, exactly as an unknown voice
                 * does: nothing is applied and nothing is saved. */
                send_function_response(id->valuestring, name->valuestring,
                                       "That name will not work on this device. Ask them "
                                       "for a shorter one, just the name on its own.");
            } else {
                ESP_LOGI(TAG, "EVT setname -> \"%s\"", agent_name_get());
                snprintf(content, sizeof(content),
                         "Your name is now %s. Answer to it from here on, and say "
                         "it back once.", agent_name_get());
                send_function_response(id->valuestring, name->valuestring, content);
            }
            cJSON_Delete(nargs);
            continue;
        }

        if (strcmp(name->valuestring, "reset_name") == 0) {
            agent_name_reset();
            ESP_LOGI(TAG, "EVT setname -> \"%s\" (default)", agent_name_get());
            snprintf(content, sizeof(content),
                     "You are called %s again. Answer to it from here on.",
                     agent_name_get());
            send_function_response(id->valuestring, name->valuestring, content);
            continue;
        }

        if (strcmp(name->valuestring, "new_conversation") == 0) {
            /*
             * TWO CALLS, AND THE SECOND ONE IS THE CONFIRMATION.
             *
             * The description tells the model to ask first, but a description is
             * a request and this is the one function here that destroys
             * something. So the confirmation is enforced in code: the first call
             * arms and does nothing, and only a second call within the window
             * actually forgets. A model that skips straight to calling it still
             * cannot lose the conversation, because the first call is not the
             * one that clears.
             *
             * The window matters in the other direction too. Without it, an arm
             * from ten minutes ago would turn an unrelated later request into an
             * immediate wipe.
             */
            const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            const bool armed = s_clear_armed_ms != 0 &&
                               (now - s_clear_armed_ms) < CLEAR_CONFIRM_WINDOW_MS &&
                               /*
                                * EXACTLY ONE user turn since arming.
                                *
                                * BE CLEAR ABOUT WHAT THIS DOES AND DOES NOT BUY,
                                * because an earlier version of this comment
                                * claimed more than the code delivers. What is
                                * guaranteed: the device asked before it wiped
                                * (the first call only ever returns "ask them"),
                                * somebody spoke after being asked, and it was
                                * recent. What is NOT guaranteed is that the
                                * answer was yes -- "no" is a user turn like any
                                * other, and nothing here can read it. A model
                                * that calls this again after being told no
                                * wipes, and no client-side counter can prevent
                                * that; it is the same class as a model calling
                                * any other function wrongly.
                                *
                                * The "exactly one" rather than "at least one" is
                                * worth what it costs anyway: it retires the arm
                                * once the conversation has moved past the answer,
                                * so an unanswered question from earlier in the
                                * minute cannot be cashed in later. It also means
                                * a confirmation split over two utterances
                                * ("Yes." "Go ahead.") does not count and the
                                * device asks again -- the direction to be wrong
                                * in, since asking twice is recoverable and
                                * wiping is not.
                                *
                                * Counting the USER's turns rather than the
                                * agent's is deliberate: how many
                                * ConversationText events the agent emits per
                                * reply is Deepgram's business and could change,
                                * and a rule built on it would fail closed --
                                * forget would become unreachable and the device
                                * would ask forever.
                                *
                                * There is no no-arena special case here because
                                * none is needed: the counter is maintained in
                                * the ConversationText handler rather than in
                                * history_add(), so it keeps advancing when there
                                * is no arena and while the history is frozen.
                                * See the note there.
                                */
                               s_user_turns == s_clear_armed_turns + 1;

            if (!armed) {
                s_clear_armed_ms = now;
                s_clear_armed_turns = s_user_turns;
                ESP_LOGI(TAG, "EVT newconv armed");
                send_function_response(id->valuestring, name->valuestring,
                    "Not done yet. Ask them to confirm they want you to forget the "
                    "whole conversation, wait for them to answer, and call "
                    "new_conversation again only if that one answer was yes. If "
                    "they say anything else, do not call it again.");
                continue;
            }

            s_clear_armed_ms = 0;
            dg_agent_clear_history();
            /*
             * The turns are gone from here, but this session still holds them
             * server-side -- so the fresh start has to be a fresh SESSION. The
             * reload is deferred to AgentAudioDone like every other one, so the
             * socket does not vanish mid-sentence; the next Settings then
             * carries no context and the greeting branch fires.
             */
            schedule_reload();
            ESP_LOGI(TAG, "EVT newconv -> cleared");
            send_function_response(id->valuestring, name->valuestring,
                "Done -- it is forgotten. Say so briefly, in a few words, and stop "
                "there; you are about to start over.");
            continue;
        }

        if (strcmp(name->valuestring, "adjust_volume") == 0) {
            int delta = 0;
            cJSON *vargs = function_args(fn);
            if (vargs != NULL) {
                const cJSON *d = cJSON_GetObjectItemCaseSensitive(vargs, "delta");
                if (cJSON_IsNumber(d)) {
                    delta = d->valueint;
                }
                cJSON_Delete(vargs);
            }

            int before = audio_io_get_volume();
            int now = audio_io_adjust_volume(delta);
            /*
             * No reload: volume is a codec register, not a Settings field, so
             * it is already in effect -- the agent's confirmation is itself
             * spoken at the new level. Phrased as done, not as about to happen.
             */
            if (now == before && delta > 0) {
                snprintf(content, sizeof(content),
                         "Already at maximum volume, %d. Say so and do not try again.", now);
            } else if (now == before && delta < 0) {
                snprintf(content, sizeof(content),
                         "Already at minimum volume, %d. Say so and do not try again.", now);
            } else {
                snprintf(content, sizeof(content), "Volume is now %d out of 100.", now);
            }
            send_function_response(id->valuestring, name->valuestring, content);
            continue;
        }

        if (strcmp(name->valuestring, "set_volume") == 0) {
            int level = -1;
            cJSON *sargs = function_args(fn);
            if (sargs != NULL) {
                const cJSON *l = cJSON_GetObjectItemCaseSensitive(sargs, "level");
                if (cJSON_IsNumber(l)) {
                    level = l->valueint;
                }
                cJSON_Delete(sargs);
            }

            if (level < 0) {
                /* No usable level: ask rather than pick one on their behalf. */
                ESP_LOGW(TAG, "EVT setvolume -> no level");
                snprintf(content, sizeof(content),
                         "No level was given. Ask how loud they want you, 20 to 100.");
                send_function_response(id->valuestring, name->valuestring, content);
                continue;
            }

            /*
             * audio_io clamps to 20..100, so `now` can differ from what was
             * asked for. Say which stop it landed on rather than reporting the
             * number as if it were the request -- see adjust_volume above for
             * why no reload is needed.
             */
            int now = audio_io_set_volume(level);
            ESP_LOGI(TAG, "EVT setvolume req=%d -> %d", level, now);
            if (now < level) {
                snprintf(content, sizeof(content),
                         "The loudest I go is %d, and I am there now. Say so.", now);
            } else if (now > level) {
                snprintf(content, sizeof(content),
                         "The quietest I go is %d, and I am there now. Say so.", now);
            } else {
                snprintf(content, sizeof(content), "Volume is now %d out of 100.", now);
            }
            send_function_response(id->valuestring, name->valuestring, content);
            continue;
        }

#if CONFIG_BATTERY
        if (strcmp(name->valuestring, "get_battery") == 0) {
            battery_status_t bat;
            bool ok = battery_get(&bat);
            ESP_LOGI(TAG, "EVT battery -> ok=%d pct=%d mv=%d chg=%d full=%d chgst=%d",
                     (int)ok, bat.percent, bat.millivolts, (int)bat.charging,
                     (int)bat.full, bat.chg_state);

            /*
             * Four answers, and the distinction between them is the point.
             * "Charging" is current going in, which is not the same as plugged
             * in -- a full battery on USB is not charging, and saying it is
             * would be wrong every time someone left it on the charger
             * overnight. The low case adds the suggestion because that is the
             * one reading the user is expected to act on.
             */
            if (!ok) {
                snprintf(content, sizeof(content),
                         "I cannot read my battery. Say so plainly and do not guess a "
                         "number.");
            } else if (bat.charging) {
                snprintf(content, sizeof(content),
                         "Battery is at %d percent and charging. Say so briefly.",
                         bat.percent);
            } else if (bat.full) {
                /* Plugged in and done. Reported as its own case because
                 * "not charging" would be true here and would read as a fault,
                 * and because the charger stops at a configured voltage that
                 * need not be 100% on the gauge -- so the number is worth
                 * saying alongside it rather than being smoothed to "full". */
                snprintf(content, sizeof(content),
                         "Battery is at %d percent and done charging, still plugged "
                         "in. Say so briefly.", bat.percent);
            } else if (bat.low) {
                snprintf(content, sizeof(content),
                         "Battery is at %d percent and not charging. Say so and suggest "
                         "plugging me in.", bat.percent);
            } else {
                snprintf(content, sizeof(content),
                         "Battery is at %d percent, not plugged in. Say so briefly.",
                         bat.percent);
            }
            send_function_response(id->valuestring, name->valuestring, content);
            continue;
        }
#endif

#if CONFIG_WIFI_SIGNAL
        if (strcmp(name->valuestring, "get_signal_strength") == 0) {
            wifi_signal_t sig;
            bool ok = wifi_sta_get_signal(&sig);
            ESP_LOGI(TAG, "EVT signal -> ok=%d rssi=%d bars=%d ch=%d weak=%d",
                     (int)ok, (int)sig.rssi, (int)sig.bars, (int)sig.channel,
                     (int)sig.weak);

            /*
             * THE dBm NEVER LEAVES THIS FUNCTION. "Minus sixty-two dBm" spoken
             * aloud tells the listener nothing, and handing the model a number
             * with no scale invites it to invent one -- so the bucket is turned
             * into the sentence here, the same way the battery cases above hand
             * over a phrasing rather than a policy.
             *
             * A standing irony worth knowing about: a link bad enough to matter
             * may not deliver the tool call at all. What this tool describes is
             * always a link that was good enough to carry the question.
             */
            if (!ok) {
                snprintf(content, sizeof(content),
                         "You cannot read your Wi-Fi signal right now. Say so plainly "
                         "and do not guess.");
            } else {
                static const char *const PHRASE[5] = {
                    "very weak. Say so and suggest moving closer to the router",
                    "weak. Say so and mention you may cut out",
                    "fair. Say so briefly",
                    "good. Say so briefly",
                    "excellent. Say so briefly",
                };
                const int b = (sig.bars > 4) ? 4 : sig.bars;
                snprintf(content, sizeof(content),
                         "Your Wi-Fi signal is %s.", PHRASE[b]);
            }
            send_function_response(id->valuestring, name->valuestring, content);
            continue;
        }
#endif

        if (strcmp(name->valuestring, "set_face") == 0) {
            int index = -1;
            const char *wanted = NULL;
            cJSON *fargs = function_args(fn);
            if (fargs != NULL) {
                const cJSON *f = cJSON_GetObjectItemCaseSensitive(fargs, "face");
                if (cJSON_IsString(f)) {
                    wanted = f->valuestring;
                    index = faces_find(wanted);
                }
            }

            if (index < 0) {
                ESP_LOGW(TAG, "EVT setface req=\"%s\" -> unknown",
                         (wanted != NULL) ? wanted : "");
                snprintf(content, sizeof(content),
                         "There is no '%s' face. Ask which one they want.",
                         (wanted != NULL) ? wanted : "");
            } else {
                /*
                 * No reload: the face is a local display setting, already in
                 * effect by the time this is spoken. Phrased as done, not as
                 * about to happen -- the opposite of set_voice below.
                 */
                ui_set_face(index);
                /* The requested string, not just the resolved face: whether the
                 * model picks this function from an indirect phrasing is the
                 * thing being tested, and that is only visible in what it sent. */
                ESP_LOGI(TAG, "EVT setface req=\"%s\" -> %s", wanted,
                         faces_name((size_t)index));
                snprintf(content, sizeof(content),
                         "The screen is now showing the %s. Say so briefly.",
                         faces_name((size_t)index));
            }
            cJSON_Delete(fargs);
            send_function_response(id->valuestring, name->valuestring, content);
            continue;
        }

        if (strcmp(name->valuestring, "set_color") == 0) {
            int index = -1;
            const char *wanted = NULL;
            cJSON *cargs = function_args(fn);
            if (cargs != NULL) {
                const cJSON *c = cJSON_GetObjectItemCaseSensitive(cargs, "color");
                if (cJSON_IsString(c)) {
                    wanted = c->valuestring;
                    index = orb_colors_find(wanted);
                }
            }

            if (index < 0) {
                ESP_LOGW(TAG, "EVT setcolor req=\"%s\" -> unknown",
                         (wanted != NULL) ? wanted : "");
                snprintf(content, sizeof(content),
                         "The orb has no '%s' colour. Ask which one they want.",
                         (wanted != NULL) ? wanted : "");
            } else {
                /* No reload, like set_face and unlike set_voice: a local display
                 * setting, in effect by the time this is spoken. */
                ui_set_orb_color(index);
                /* The requested string as well as the resolved colour: whether
                 * the model routes an indirect phrasing here is the thing worth
                 * testing, and that is only visible in what it actually sent. */
                ESP_LOGI(TAG, "EVT setcolor req=\"%s\" -> %s", wanted,
                         orb_colors_name((size_t)index));
                /* "The orb is", not "the screen is": the colour applies whichever
                 * face is up, so this stays true while the spectrum is showing. */
                snprintf(content, sizeof(content),
                         "The orb is now %s. Say so briefly.",
                         orb_colors_name((size_t)index));
            }
            cJSON_Delete(cargs);
            send_function_response(id->valuestring, name->valuestring, content);
            continue;
        }

        if (strcmp(name->valuestring, "start_display_test") == 0) {
            /*
             * No arguments, but parse-and-free anyway: the model sometimes sends
             * "{}" and function_args() allocates regardless of content.
             */
            cJSON *targs = function_args(fn);
            cJSON_Delete(targs);

            /*
             * Logged at INFO with no detail to add, because the interesting
             * question is only ever whether this fired at all -- the trigger is a
             * spoken phrase that has to survive speech-to-text, and a miss looks
             * identical to a user who never said it. The transcript lines
             * either side of this in the log are what tell them apart.
             */
            ESP_LOGI(TAG, "EVT displaytest requested");
            schedule_test();
            snprintf(content, sizeof(content),
                     "Starting the display test. Tell the user to tap the screen "
                     "to step through each state, briefly.");
            send_function_response(id->valuestring, name->valuestring, content);
            continue;
        }

        if (strcmp(name->valuestring, "set_voice") != 0) {
            send_function_response(id->valuestring, name->valuestring, "Unknown function.");
            continue;
        }

        const voice_t *v = NULL;
        cJSON *args = function_args(fn);
        if (args != NULL) {
            const cJSON *want = cJSON_GetObjectItemCaseSensitive(args, "voice");
            if (cJSON_IsString(want)) {
                v = voices_find(want->valuestring);
            }
            cJSON_Delete(args);
        }

        if (v == NULL) {
            /* Say so rather than staying silent: the agent turns this into an
             * explanation, and nothing is applied or saved. */
            send_function_response(id->valuestring, name->valuestring,
                                   "That voice is not available on this device.");
            continue;
        }

        ESP_LOGI(TAG, "voice change requested: %s", v->model);
        /* Persisted now rather than on an acknowledgement, because the new
         * session reads it while building its Settings. */
        voices_set(v);
        schedule_reload();
        snprintf(content, sizeof(content),
                 "Switching to %s. Tell the user you are changing voice now.", v->name);
        send_function_response(id->valuestring, name->valuestring, content);
    }
}

/*
 * Builds the one message the session cannot start without.
 *
 * The shape below is the whole contract: `audio.input` describes what we will
 * send, `audio.output` what we want back, and `agent.{listen,think,speak}`
 * picks the three models Deepgram wires together internally. Omitting
 * audio.output entirely is legal but leaves the format to the server's default,
 * which is not what a fixed-rate codec on the other end wants.
 */
static esp_err_t send_settings(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "Settings");

    cJSON *audio = cJSON_AddObjectToObject(root, "audio");
    cJSON *input = cJSON_AddObjectToObject(audio, "input");
    cJSON_AddStringToObject(input, "encoding", DG_AUDIO_ENCODING);
    cJSON_AddNumberToObject(input, "sample_rate", DG_AUDIO_SAMPLE_RATE);
    cJSON *output = cJSON_AddObjectToObject(audio, "output");
    cJSON_AddStringToObject(output, "encoding", DG_AUDIO_ENCODING);
    cJSON_AddNumberToObject(output, "sample_rate", DG_AUDIO_SAMPLE_RATE);
    /* Already the default, but stated because Flux TTS *rejects* containers and
     * compressed encodings rather than ignoring them -- better a loud failure
     * than a silent format mismatch feeding the codec. */
    cJSON_AddStringToObject(output, "container", "none");

    cJSON *agent = cJSON_AddObjectToObject(root, "agent");

    cJSON *listen_provider = cJSON_AddObjectToObject(
        cJSON_AddObjectToObject(agent, "listen"), "provider");
    cJSON_AddStringToObject(listen_provider, "type", "deepgram");

    /*
     * Flux. `version` is what selects it -- the model name alone is not enough,
     * and v1 is assumed when the field is absent.
     *
     * No `language` here: that is a v1 listen-provider option. Flux takes
     * `language_hints` instead, and flux-general-en already implies English.
     * Thresholds are omitted unless configured, because the server defaults are
     * the right starting point.
     */
    cJSON_AddStringToObject(listen_provider, "version", "v2");
    cJSON_AddStringToObject(listen_provider, "model", "flux-general-en");
    if (strlen(CONFIG_DEEPGRAM_FLUX_EOT_THRESHOLD) > 0) {
        cJSON_AddStringToObject(listen_provider, "eot_threshold",
                                CONFIG_DEEPGRAM_FLUX_EOT_THRESHOLD);
    }
#if CONFIG_DEEPGRAM_FLUX_EOT_TIMEOUT_MS > 0
    cJSON_AddNumberToObject(listen_provider, "eot_timeout_ms",
                            CONFIG_DEEPGRAM_FLUX_EOT_TIMEOUT_MS);
#endif

    cJSON *think = cJSON_AddObjectToObject(agent, "think");
    cJSON *think_provider = cJSON_AddObjectToObject(think, "provider");
    cJSON_AddStringToObject(think_provider, "type", "open_ai");
    cJSON_AddStringToObject(think_provider, "model", "gpt-4o-mini");

    /*
     * The persona, assembled from the blocks in main/prompt. Built in PSRAM and freed
     * immediately -- cJSON copies the string, and this function's frame must not
     * grow with the prompt (see the stack note further down and agent_prompt.h).
     *
     * A resumed session gets told so: history_to_json() below replays the last
     * few turns, and without this the model reads them as a conversation it is
     * joining rather than one it has been having.
     */
    agent_prompt_ctx_t pctx = {
        .notes = (s_history_count > 0)
                     ? "You have already been talking with this person for a "
                       "few turns. What follows is that same conversation, not "
                       "a new one, so pick it up where it left off and do not "
                       "start over or greet them again."
                     : NULL,
    };
    char *prompt = agent_prompt_build(&pctx);
    if (prompt != NULL) {
        cJSON_AddStringToObject(think, "prompt", prompt);
        free(prompt);
    } else {
        /* A session with a default persona is worth far more than no session. */
        ESP_LOGE(TAG, "no system prompt; continuing without one");
    }

    /*
     * Client-side functions, which is signalled by the *absence* of an
     * "endpoint" -- with one, Deepgram would call a web service instead of
     * asking us.
     *
     * The array itself is not Flux-gated: volume is a local codec setting and
     * works on either speech stack. Only the voice functions are, because that
     * catalog is entirely flux-* models.
     */
    cJSON *functions = cJSON_AddArrayToObject(think, "functions");

    cJSON *adjust_volume = cJSON_CreateObject();
    cJSON_AddStringToObject(adjust_volume, "name", "adjust_volume");
    cJSON_AddStringToObject(adjust_volume, "description",
                            "Make yourself louder or quieter BY A RELATIVE AMOUNT, for "
                            "'a bit louder' or 'turn it down'. Negative is quieter, "
                            "positive is louder. A small change is about 10, a big one "
                            "about 30. The scale runs 20 to 100, so once you are told you "
                            "are at a limit, stop trying. If they name a number to land on, "
                            "use set_volume instead.");
    cJSON *vparams = cJSON_AddObjectToObject(adjust_volume, "parameters");
    cJSON_AddStringToObject(vparams, "type", "object");
    cJSON *vprops = cJSON_AddObjectToObject(vparams, "properties");
    cJSON *delta_prop = cJSON_AddObjectToObject(vprops, "delta");
    cJSON_AddStringToObject(delta_prop, "type", "integer");
    cJSON_AddNumberToObject(delta_prop, "minimum", -100);
    cJSON_AddNumberToObject(delta_prop, "maximum", 100);
    cJSON *vrequired = cJSON_AddArrayToObject(vparams, "required");
    cJSON_AddItemToArray(vrequired, cJSON_CreateString("delta"));
    cJSON_AddItemToArray(functions, adjust_volume);

    cJSON *set_volume = cJSON_CreateObject();
    cJSON_AddStringToObject(set_volume, "name", "set_volume");
    cJSON_AddStringToObject(set_volume, "description",
                            "Set how loud you are TO A SPECIFIC LEVEL, for 'set your "
                            "volume to 50'. The scale runs 20 to 100: 20 is barely "
                            "audible, 100 is maximum. 20 is as quiet as you go and there "
                            "is no mute, so a request below it lands on 20. For 'louder' "
                            "or 'quieter' with no number, use adjust_volume instead.");
    cJSON *sparams = cJSON_AddObjectToObject(set_volume, "parameters");
    cJSON_AddStringToObject(sparams, "type", "object");
    cJSON *sprops = cJSON_AddObjectToObject(sparams, "properties");
    cJSON *level_prop = cJSON_AddObjectToObject(sprops, "level");
    cJSON_AddStringToObject(level_prop, "type", "integer");
    cJSON_AddNumberToObject(level_prop, "minimum", 20);
    cJSON_AddNumberToObject(level_prop, "maximum", 100);
    cJSON *srequired = cJSON_AddArrayToObject(sparams, "required");
    cJSON_AddItemToArray(srequired, cJSON_CreateString("level"));
    cJSON_AddItemToArray(functions, set_volume);

#if CONFIG_BATTERY
    /*
     * Reading, not doing -- the only tool here that changes nothing. Gated on
     * CONFIG_BATTERY because a build with no battery module would have nothing
     * to answer with, and a tool that always fails is worse than an absent one:
     * the model would keep offering it.
     *
     * "how much charge is left" is the phrasing this exists for, and the
     * description says so, because the model has no other way to know that this
     * device has a battery at all.
     */
    cJSON *get_battery = cJSON_CreateObject();
    cJSON_AddStringToObject(get_battery, "name", "get_battery");
    cJSON_AddStringToObject(get_battery, "description",
                            "Read your OWN remaining battery charge. Use it whenever "
                            "they ask how much charge or battery is left, how long you "
                            "will last, or whether you are plugged in or charging. You "
                            "run on a rechargeable battery and this is the only way you "
                            "can know its level -- never guess or estimate one.");
    cJSON *bparams = cJSON_AddObjectToObject(get_battery, "parameters");
    cJSON_AddStringToObject(bparams, "type", "object");
    cJSON_AddObjectToObject(bparams, "properties");
    cJSON_AddItemToArray(functions, get_battery);
#endif

#if CONFIG_WIFI_SIGNAL
    /*
     * The other reading tool, and gated for the same reason.
     *
     * "why do you keep cutting out" is the phrasing this exists for as much as
     * "how is your wifi" -- the link is the most likely cause of a broken
     * conversation and the model has no other way to see it.
     */
    cJSON *get_signal = cJSON_CreateObject();
    cJSON_AddStringToObject(get_signal, "name", "get_signal_strength");
    cJSON_AddStringToObject(get_signal, "description",
                            "Read your OWN Wi-Fi signal strength. Use it whenever they "
                            "ask about your wifi, your signal, your connection, or why "
                            "you are cutting out or sounding choppy. You reach the "
                            "network over Wi-Fi and this is the only way you can know "
                            "how good that link is -- never guess or estimate it.");
    cJSON *wparams = cJSON_AddObjectToObject(get_signal, "parameters");
    cJSON_AddStringToObject(wparams, "type", "object");
    cJSON_AddObjectToObject(wparams, "properties");
    cJSON_AddItemToArray(functions, get_signal);
#endif

    /*
     * Not Flux-gated either: the display is local and works on either speech
     * stack. The catalog goes in the description for the same reason set_voice
     * does it -- JSON Schema has nowhere to hang a per-enum-value note, and
     * without one the model is choosing between two bare nouns.
     */
    /*
     * PSRAM and ONE buffer, the pattern set_color introduced below and the
     * reason this function's frame no longer grows with the catalog. This was
     * the stack pair the canary note further down describes -- 512 + 700 = 1,212
     * B of the frame for two faces. See .claude/skills/esp-stack-budget/.
     *
     * 377 B in use at two faces. Rounded up because it is PSRAM and free, so a
     * few more faces cannot quietly reach faces_describe()'s truncation path.
     */
    enum { FACE_DESC_LEN = 1024 };
    char *face_desc = heap_caps_malloc(FACE_DESC_LEN, MALLOC_CAP_SPIRAM);
    /* Losing the catalog is survivable -- the enum still constrains the model to
     * valid names. Losing the function is not. */
    const char *face_desc_str = "Change what the device's screen shows.";
    if (face_desc != NULL) {
        int n = snprintf(face_desc, FACE_DESC_LEN,
                         "Change what the device's screen shows. Use when the user asks for a "
                         "different look, mentions the display, or names one of these. "
                         "Faces: ");
        if (n > 0 && (size_t)n < FACE_DESC_LEN) {
            faces_describe(face_desc + n, FACE_DESC_LEN - (size_t)n);
            /* The trailing stop the one-shot snprintf used to supply, kept so
             * this is a move off the stack and not a change to what the model
             * reads. */
            size_t used = strlen(face_desc);
            if (used + 2 <= FACE_DESC_LEN) {
                face_desc[used] = '.';
                face_desc[used + 1] = '\0';
            }
            face_desc_str = face_desc;
        }
    }

    cJSON *set_face = cJSON_CreateObject();
    cJSON_AddStringToObject(set_face, "name", "set_face");
    cJSON_AddStringToObject(set_face, "description", face_desc_str);
    free(face_desc); /* cJSON copied it; free(NULL) is fine */
    cJSON *fparams = cJSON_AddObjectToObject(set_face, "parameters");
    cJSON_AddStringToObject(fparams, "type", "object");
    cJSON *fprops = cJSON_AddObjectToObject(fparams, "properties");
    cJSON *face_prop = cJSON_AddObjectToObject(fprops, "face");
    cJSON_AddStringToObject(face_prop, "type", "string");
    faces_add_enum(face_prop, "enum");
    cJSON *frequired = cJSON_AddArrayToObject(fparams, "required");
    cJSON_AddItemToArray(frequired, cJSON_CreateString("face"));
    cJSON_AddItemToArray(functions, set_face);

    /*
     * Colour, same shape and the same reasoning as the face above: local to the
     * device, so not Flux-gated, and the catalog goes in the description because
     * JSON Schema has nowhere to hang a per-enum-value note.
     *
     * The description names the orb explicitly. The spectrum draws its own
     * palette and ignores the setting, and saying so is what stops the model
     * offering a colour change as the answer to "the bars look wrong".
     */
    /*
     * PSRAM, and ONE buffer rather than a catalog-plus-description pair.
     *
     * NOT A STYLE CHOICE -- MEASURED. This task has task_stack = 6144. This
     * function once gave every described catalog a stack PAIR, a buffer for the
     * catalog and another for the description around it: 1,212 B for faces,
     * 1,668 for voices. Adding a third pair of 1,280 for these colours tripped
     * the stack canary on the first session and put the device in a boot loop,
     * before cJSON's own recursion is even counted. Recovering it needed BOOT
     * held while RESET was tapped -- the board rebooted faster than esptool
     * could sync.
     *
     * So the prefix is written first and the catalog appended into the tail of
     * the same allocation, which costs no stack at all. cJSON copies the string,
     * so it is freed immediately.
     *
     * ALL THREE USE THIS NOW, which is the part worth keeping. The pair pattern
     * was the trap rather than the colour function that sprang it: its cost was
     * O(n) in declared functions with no budget written down anywhere, so it was
     * going to fail for whoever added the third one, whatever it happened to be.
     * Measured either side of moving faces and voices across: this function had
     * a 2,944 B frame and was called from on_ws_event's 192 B one, so the path
     * cost 3,136 B of the 6,144 available before cJSON recursed at all. With the
     * buffers gone it is small enough that the compiler inlines it into
     * on_ws_event outright -- there is no send_settings frame in the image any
     * more, and that caller is still 192 B. 3,136 -> 192.
     *
     * A new described catalog should cost nothing here; if one ever appears to,
     * measure with .claude/skills/esp-stack-budget/ before enlarging anything.
     */
    /* 543 B in use at thirteen colours (201 prefix + 341 catalog + NUL). Rounded
     * up because it is PSRAM and free, so a couple more colours cannot quietly
     * run into orb_colors_describe()'s truncation path. */
    enum { COLOR_DESC_LEN = 1024 };
    char *color_desc = heap_caps_malloc(COLOR_DESC_LEN, MALLOC_CAP_SPIRAM);
    /* Losing the catalog is survivable -- the enum still constrains the model to
     * valid names, it just has less to reason about. Losing the function is not. */
    const char *color_desc_str = "Change the colour of the orb on the device's screen.";
    if (color_desc != NULL) {
        int n = snprintf(color_desc, COLOR_DESC_LEN,
                         "Change the colour of the orb on the device's screen. Use "
                         "when the user asks for a different colour, or to go back "
                         "to normal. Only the orb is affected; the spectrum face "
                         "keeps its own colours. Colours: ");
        if (n > 0 && (size_t)n < COLOR_DESC_LEN) {
            orb_colors_describe(color_desc + n, COLOR_DESC_LEN - (size_t)n);
            color_desc_str = color_desc;
        }
    }

    cJSON *set_color = cJSON_CreateObject();
    cJSON_AddStringToObject(set_color, "name", "set_color");
    cJSON_AddStringToObject(set_color, "description", color_desc_str);
    free(color_desc); /* cJSON copied it; free(NULL) is fine */
    cJSON *cparams = cJSON_AddObjectToObject(set_color, "parameters");
    cJSON_AddStringToObject(cparams, "type", "object");
    cJSON *cprops = cJSON_AddObjectToObject(cparams, "properties");
    cJSON *color_prop = cJSON_AddObjectToObject(cprops, "color");
    cJSON_AddStringToObject(color_prop, "type", "string");
    orb_colors_add_enum(color_prop, "enum");
    cJSON *crequired = cJSON_AddArrayToObject(cparams, "required");
    cJSON_AddItemToArray(crequired, cJSON_CreateString("color"));
    cJSON_AddItemToArray(functions, set_color);

    /*
     * The display test. No parameters, so no enum and no catalog -- which means
     * a plain literal description and none of the buffer juggling above.
     *
     * THE PHRASE HAS TO SURVIVE SPEECH-TO-TEXT, and that is the whole risk in
     * this function. "up up down down left right left right" can arrive merged,
     * hyphenated, as "up-up-down-down", or with a stray "B A" on the end from a
     * user who knows the rest of the code. So this describes the SHAPE of the
     * utterance -- a run of repeated directions -- rather than one exact string,
     * and says to ignore the surrounding wording.
     */
    /*
     * The name. No catalog and no enum -- any name is valid, which is exactly
     * why the DESCRIPTION carries the rules instead: this arrives from
     * speech-to-text, so the model is passing on something it HEARD, and left to
     * itself it will hand over the whole sentence it heard it in.
     */
    cJSON *set_name = cJSON_CreateObject();
    cJSON_AddStringToObject(set_name, "name", "set_name");
    cJSON_AddStringToObject(set_name, "description",
        "Change what you are called, and remember it. Use when the user asks you "
        "to go by something else, gives you a new name, or asks what they should "
        "call you and then names one. Pass ONLY the name itself, spelled as a "
        "name and capitalised, with nothing else around it -- not the sentence "
        "you heard it in, and not a title. If you did not catch it clearly, ask "
        "them to say it again instead of guessing. Do not call this when the "
        "user is telling you THEIR name.");
    cJSON *nparams = cJSON_AddObjectToObject(set_name, "parameters");
    cJSON_AddStringToObject(nparams, "type", "object");
    cJSON *nprops = cJSON_AddObjectToObject(nparams, "properties");
    cJSON *name_prop = cJSON_AddObjectToObject(nprops, "name");
    cJSON_AddStringToObject(name_prop, "type", "string");
    cJSON *nrequired = cJSON_AddArrayToObject(nparams, "required");
    cJSON_AddItemToArray(nrequired, cJSON_CreateString("name"));
    cJSON_AddItemToArray(functions, set_name);

    cJSON *reset_name = cJSON_CreateObject();
    cJSON_AddStringToObject(reset_name, "name", "reset_name");
    cJSON_AddStringToObject(reset_name, "description",
        "Go back to the name this device came with. Use when the user asks you to "
        "reset your name or go back to what you were called before.");
    cJSON *reset_nparams = cJSON_AddObjectToObject(reset_name, "parameters");
    cJSON_AddStringToObject(reset_nparams, "type", "object");
    cJSON_AddObjectToObject(reset_nparams, "properties");
    cJSON_AddItemToArray(functions, reset_name);

    cJSON *new_conv = cJSON_CreateObject();
    cJSON_AddStringToObject(new_conv, "name", "new_conversation");
    cJSON_AddStringToObject(new_conv, "description",
        "Forget everything you and the user have talked about and start over from "
        "nothing. This is destructive and cannot be undone. Only use it when the "
        "user has asked to start fresh, wipe the conversation, or have you forget "
        "what was said -- never on your own initiative, and never just because a "
        "topic changed. Call it once to ask them to confirm, and only call it a "
        "second time after they have actually said yes.");
    cJSON *new_cparams = cJSON_AddObjectToObject(new_conv, "parameters");
    cJSON_AddStringToObject(new_cparams, "type", "object");
    cJSON_AddObjectToObject(new_cparams, "properties");
    cJSON_AddItemToArray(functions, new_conv);

    cJSON *set_test = cJSON_CreateObject();
    cJSON_AddStringToObject(set_test, "name", "start_display_test");
    cJSON_AddStringToObject(set_test, "description",
        "Put the device into its display test, which steps through every visual "
        "state of the orb one tap at a time. Call this when the user says a run "
        "of directions in sequence -- the Konami code, \"up up down down left "
        "right left right\" -- however it comes through: merged, hyphenated, "
        "repeated, or with A and B on the end. Match on the sequence of "
        "directions itself and ignore any wording around it. Do not call it for "
        "a single direction, or for a question about the display.");
    cJSON *tparams = cJSON_AddObjectToObject(set_test, "parameters");
    cJSON_AddStringToObject(tparams, "type", "object");
    cJSON_AddObjectToObject(tparams, "properties");
    cJSON_AddItemToArray(functions, set_test);

    /* The catalog goes in the description because JSON Schema has nowhere to
     * hang a per-enum-value note, and without it the model is choosing from
     * bare first names. */
    /*
     * The last of the three stack pairs, and the largest at 768 + 900 = 1,668 B.
     * Same one-buffer PSRAM pattern as the two above.
     *
     * 2048 rather than the 1024 the others use, and sized for growth rather than
     * for today: 705 B is in use at thirteen featured voices, and voices.c keeps
     * the other twenty-three precisely so widening the offer is "a one-flag
     * change". All thirty-six would be 1,743 B, so that flag can be flipped
     * without landing on voices_describe()'s truncation path -- which is the
     * whole point of the catalog not living on the stack any more.
     */
    enum { VOICE_DESC_LEN = 2048 };
    char *description = heap_caps_malloc(VOICE_DESC_LEN, MALLOC_CAP_SPIRAM);
    const char *description_str = "Change the voice you speak in.";
    if (description != NULL) {
        int n = snprintf(description, VOICE_DESC_LEN,
                         "Change the voice you speak in. Use when the user asks you to sound "
                         "different, or asks for a particular accent or gender. Voices: ");
        if (n > 0 && (size_t)n < VOICE_DESC_LEN) {
            voices_describe(description + n, VOICE_DESC_LEN - (size_t)n);
            size_t used = strlen(description);
            if (used + 2 <= VOICE_DESC_LEN) {
                description[used] = '.';
                description[used + 1] = '\0';
            }
            description_str = description;
        }
    }

    cJSON *set_voice = cJSON_CreateObject();
    cJSON_AddStringToObject(set_voice, "name", "set_voice");
    cJSON_AddStringToObject(set_voice, "description", description_str);
    free(description); /* cJSON copied it; free(NULL) is fine */
    cJSON *params = cJSON_AddObjectToObject(set_voice, "parameters");
    cJSON_AddStringToObject(params, "type", "object");
    cJSON *props = cJSON_AddObjectToObject(params, "properties");
    cJSON *voice_prop = cJSON_AddObjectToObject(props, "voice");
    cJSON_AddStringToObject(voice_prop, "type", "string");
    voices_add_enum(voice_prop, "enum");
    cJSON *required = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString("voice"));
    cJSON_AddItemToArray(functions, set_voice);

    cJSON *reset_voice = cJSON_CreateObject();
    cJSON_AddStringToObject(reset_voice, "name", "reset_voice");
    cJSON_AddStringToObject(reset_voice, "description",
                            "Go back to the device's default voice. Use when the user asks "
                            "you to reset your voice or return to how you normally sound.");
    cJSON *reset_params = cJSON_AddObjectToObject(reset_voice, "parameters");
    cJSON_AddStringToObject(reset_params, "type", "object");
    cJSON_AddObjectToObject(reset_params, "properties");
    cJSON_AddItemToArray(functions, reset_voice);

    cJSON *speak_provider = cJSON_AddObjectToObject(
        cJSON_AddObjectToObject(agent, "speak"), "provider");
    cJSON_AddStringToObject(speak_provider, "type", "deepgram");
    /* Same story as listen: "v2" is what picks Flux TTS. Omitting agent.speak
     * entirely would also get Flux with flux-kit-en, but being explicit keeps
     * the voice configurable. */
    cJSON_AddStringToObject(speak_provider, "version", "v2");
    cJSON_AddStringToObject(speak_provider, "model", voices_current_model());
    /*
     * Replayed context, and the greeting only when there is none. Resuming a
     * conversation should not open with "Hi! I am running on an ESP32" -- and
     * this is also what stops an auto-reconnect after a network blip from
     * re-greeting, which used to be the most visible symptom of a dropped
     * socket.
     */
    history_to_json(agent);
    if (s_history_count == 0 && strlen(CONFIG_DEEPGRAM_AGENT_GREETING) > 0) {
        cJSON_AddStringToObject(agent, "greeting", CONFIG_DEEPGRAM_AGENT_GREETING);
    }

    esp_err_t err = send_json(root, "Settings");
    cJSON_Delete(root);
    return err;
}

/* ---------------- server messages ---------------- */

static void handle_json(const char *json, int len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "unparseable message: %.*s", len, json);
        return;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }
    const char *t = type->valuestring;

    if (strcmp(t, "Welcome") == 0) {
        const cJSON *rid = cJSON_GetObjectItemCaseSensitive(root, "request_id");
        /* Worth logging: this id is what Deepgram support asks for. */
        ESP_LOGI(TAG, "Welcome, request_id=%s",
                 cJSON_IsString(rid) ? rid->valuestring : "?");

    } else if (strcmp(t, "SettingsApplied") == 0) {
        static uint32_t sessions;
        /* Numbered: the greeting is spoken once per session, so a greeting you
         * did not ask for always has a new number in front of it. */
        ESP_LOGI(TAG, "SettingsApplied -- session #%" PRIu32 " is live", ++sessions);
        /* The session this records turns for is now the one that was told
         * nothing. Anything said before here belonged to the conversation the
         * user asked to forget. */
        s_history_frozen = false;
        set_state(DG_AGENT_READY);

    } else if (strcmp(t, "ConversationText") == 0) {
        const cJSON *role = cJSON_GetObjectItemCaseSensitive(root, "role");
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
        if (cJSON_IsString(role) && cJSON_IsString(content)) {
            ESP_LOGI(TAG, "%s: %s", role->valuestring, content->valuestring);
            /*
             * Counted HERE and not inside history_add(), which returns early
             * when there is no arena and while the history is frozen. This
             * counter is what tells a new_conversation confirmation apart from
             * the model calling the function twice by itself, and a counter that
             * stops advancing in either of those states would make the
             * confirmation unanswerable -- the device would ask forever. It is a
             * fact about the conversation, not about the arena.
             */
            if (strcmp(role->valuestring, "user") == 0) {
                s_user_turns++;
            }
            history_add(role->valuestring, content->valuestring);
            if (s_cb.on_conversation_text) {
                s_cb.on_conversation_text(role->valuestring, content->valuestring, s_cb.ctx);
            }
        }

    } else if (strcmp(t, "UserStartedSpeaking") == 0) {
        ESP_LOGI(TAG, "user started speaking");
        if (s_cb.on_user_started_speaking) {
            s_cb.on_user_started_speaking(s_cb.ctx);
        }

    } else if (strcmp(t, "AgentThinking") == 0) {
        /*
         * Never observed in practice. This was wired to a display state for a
         * while; across every session logged it did not arrive once, so the state
         * was unreachable and has been removed. Still handled here rather than
         * left to the catch-all, so if this stack ever does start sending it the
         * log says so instead of reading as an unknown type.
         */
        ESP_LOGD(TAG, "agent thinking");

    } else if (strcmp(t, "AgentAudioDone") == 0) {
        ESP_LOGI(TAG, "agent finished speaking");
        if (s_cb.on_agent_audio_done) {
            s_cb.on_agent_audio_done(s_cb.ctx);
        }
        /*
         * Deferred to here so the agent gets to say what it is doing before the
         * socket goes away -- that sentence is spoken in the old voice, and
         * everything after the reconnect is in the new one.
         *
         * take_flag() rather than a test and a clear: esp_timer runs at priority
         * 22, above this task, so the backstop can read the flag between the two
         * and reload as well. Stopping the timer first is not enough on its own
         * -- esp_timer_stop() does not wait for a callback already running.
         */
        if (take_flag(&s_reload_pending)) {
            if (s_reload_backstop != NULL) {
                esp_timer_stop(s_reload_backstop);
            }
            ESP_LOGI(TAG, "reopening session to apply new settings");
            if (s_cb.on_reload_required) {
                s_cb.on_reload_required(s_cb.ctx);
            }
        }
        /* Same deferral, and the same test-and-clear, for the reason the reload
         * branch above gives: the confirmation is spoken over this session, and
         * the display test closes it. */
        if (take_flag(&s_test_pending)) {
            if (s_test_backstop != NULL) {
                esp_timer_stop(s_test_backstop);
            }
            ESP_LOGI(TAG, "handing the screen to the display test");
            if (s_cb.on_display_test_required) {
                s_cb.on_display_test_required(s_cb.ctx);
            }
        }

    } else if (strcmp(t, "FunctionCallRequest") == 0) {
        handle_function_call(root);

    } else if (strcmp(t, "Error") == 0) {
        const cJSON *desc = cJSON_GetObjectItemCaseSensitive(root, "description");
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        ESP_LOGE(TAG, "server error [%s]: %s",
                 cJSON_IsString(code) ? code->valuestring : "?",
                 cJSON_IsString(desc) ? desc->valuestring : "?");
        set_state(DG_AGENT_ERROR);

    } else if (strcmp(t, "LatencyReport") == 0 || strcmp(t, "History") == 0 ||
               strcmp(t, "AgentStartedSpeaking") == 0) {
        /* Known and deliberately ignored. Named explicitly so the catch-all
         * below stays a signal rather than a flood -- LatencyReport alone
         * arrives several times a second while the agent speaks. */

    } else if (strcmp(t, "Warning") == 0) {
        /* The server's soft-failure channel -- e.g. a model it could not honour
         * and silently substituted. Previously this fell into the LOGD branch
         * below, which CONFIG_LOG_MAXIMUM_LEVEL_INFO compiles out entirely, so
         * it was invisible. */
        const cJSON *desc = cJSON_GetObjectItemCaseSensitive(root, "description");
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        ESP_LOGW(TAG, "server warning [%s]: %s",
                 cJSON_IsString(code) ? code->valuestring : "?",
                 cJSON_IsString(desc) ? desc->valuestring : "?");

    } else {
        /* INFO, not DEBUG: anything the server says that we do not model is
         * exactly what we want to see when behaviour does not match the docs. */
        ESP_LOGI(TAG, "unhandled message type %s", t);
    }

    cJSON_Delete(root);
}

/*
 * Rebuilds a JSON message that the client split across events.
 *
 * esp_websocket_client hands over at most buffer_size bytes at a time and
 * reports where the slice sits inside the frame, so a 3 kB ConversationText
 * arrives as two events with the same op_code. Parsing each slice on its own
 * would throw away every message longer than the buffer.
 */
static void accumulate_json(const esp_websocket_event_data_t *ev)
{
    /* A TEXT opcode at offset 0 is the first slice of a new message; a CONT
     * opcode continues the previous one and must not reset the buffer even
     * though its own payload_offset starts back at 0. */
    if (ev->op_code == WS_OPCODE_TEXT && ev->payload_offset == 0) {
        s_json_len = 0;
        s_json_dropping = false;
    }

    if (!s_json_dropping) {
        if (s_json_len + ev->data_len > JSON_REASSEMBLY_MAX) {
            ESP_LOGW(TAG, "message exceeds %d byte reassembly buffer, dropping",
                     JSON_REASSEMBLY_MAX);
            s_json_dropping = true;
            s_json_len = 0;
        } else {
            memcpy(s_json + s_json_len, ev->data_ptr, ev->data_len);
            s_json_len += ev->data_len;
        }
    }

    /*
     * Complete only when this is the last slice of the last frame: payload_len
     * covers one frame, fin covers the fragment chain.
     *
     * Reached whether or not the message was dropped, and that is the point: the
     * drop is cleared HERE rather than on the next message's first slice, so a
     * following message that never presents an offset-0 TEXT slice still starts
     * clean.
     */
    if (ev->fin && ev->payload_offset + ev->data_len >= ev->payload_len) {
        if (!s_json_dropping) {
            handle_json(s_json, s_json_len);
        }
        s_json_len = 0;
        s_json_dropping = false;
    }
}

static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const esp_websocket_event_data_t *ev = data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "socket open");
        set_state(DG_AGENT_CONNECTED);
        /* Settings first, before anything else, or the server ignores us. */
        send_settings();
        break;

    case WEBSOCKET_EVENT_DATA:
        if (ev->op_code == WS_OPCODE_BINARY) {
            if (s_cb.on_audio && ev->data_len > 0) {
                s_cb.on_audio((const uint8_t *)ev->data_ptr, ev->data_len, s_cb.ctx);
            }
        } else if (ev->op_code == WS_OPCODE_TEXT || ev->op_code == WS_OPCODE_CONT) {
            if (ev->data_len > 0) {
                accumulate_json(ev);
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        /*
         * The client formats a human-readable reason into the event *data* and
         * only fills error_handle for handshake failures -- reading the handle
         * here printed uninitialised numbers and hid the actual cause.
         */
        ESP_LOGE(TAG, "transport error: %.*s",
                 ev->data_len, ev->data_ptr ? ev->data_ptr : "(no detail)");
        /*
         * ONE FIELD OF error_handle IS WORTH READING, and it is the one that
         * tells a rejected API key apart from a bad network. That distinction
         * did not matter while the key came from menuconfig; it matters now that
         * someone types it into the setup portal and can mistype it.
         *
         * GATE ON THE STATUS CODE, NOT error_type. This path sets
         * error_type = WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT after filling the
         * status code in just above it (esp_websocket_client.c), so a test for
         * ..._HANDSHAKE would never fire despite this being a handshake failure.
         *
         * AND 401 IS A POSITIVE SIGNAL ONLY. transport_ws.c assigns
         * http_status_code only once a response header has actually been read,
         * and never clears it between attempts, so a later non-HTTP failure can
         * still be carrying an old one. Seeing 401 means a 401 was really read.
         * NOT seeing it proves nothing about the key, so nothing here may treat
         * its absence as the key being good.
         */
        if (ev->error_handle.esp_ws_handshake_status_code == 401) {
            ESP_LOGE(TAG, "the Deepgram API key was rejected (HTTP 401)");
            set_state(DG_AGENT_BAD_KEY);
        } else {
            set_state(DG_AGENT_ERROR);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "socket closed (status %d)", ev->close_status_code);
        s_json_len = 0;
        s_json_dropping = false;
        /*
         * The deferrals die with the socket, exactly as they do in
         * dg_agent_stop() -- and this is the path that does NOT go through it.
         * An auto-reconnect rebuilds the session from scratch, which is what any
         * pending reload wanted; leaving one armed means the backstop fires
         * against a session that already fixed itself, and request_now() skips
         * both the busy flag and the cooldown on its way to tearing it down.
         *
         * take_flag(), not a plain store: s_ready is still true at this point --
         * the set_state() at the end of this block is what clears it -- so the
         * backstop may be reading the flag on the esp_timer task right now, and
         * a plain clear lets it win and drive a teardown on top of the client's
         * own auto-reconnect.
         */
        (void)take_flag(&s_reload_pending);
        /* The reload's timer only. The display test has its own, and it
         * deliberately survives a socket drop -- see the note below. */
        if (s_reload_backstop != NULL) {
            esp_timer_stop(s_reload_backstop);
        }
        /*
         * The clear-arm too, for the reason dg_agent_stop() gives: an unanswered
         * "are you sure?" that outlives its session lets a single
         * new_conversation call in the NEXT one wipe with no question asked in
         * it. This path does not go through dg_agent_stop(), so it has to say so
         * itself.
         */
        s_clear_armed_ms = 0;
        s_clear_armed_turns = 0;
        /*
         * s_test_pending deliberately SURVIVES. The argument above -- that a
         * reconnect rebuilds the session, which is all a pending reload wanted
         * -- does not extend to the display test: a reconnect never runs one, so
         * clearing it here would silently drop something the user asked for.
         */
        set_state(DG_AGENT_DISCONNECTED);
        break;

    default:
        break;
    }
}

/*
 * Deepgram drops an Agent socket that has been quiet for ~10 s. Once a
 * microphone is streaming, the audio itself keeps it open and this becomes
 * redundant; until then it is the only thing holding the session up.
 *
 * This lives in its own task rather than an esp_timer callback because sending
 * a frame takes the client's transmit lock and can block.
 */
/*
 * Drains the queue into the socket, and is the ONLY caller of send_bin.
 *
 * Persistent, like keepalive_task: it outlives any one session, so nothing has
 * to join or delete it on a teardown path. While stopped, s_ready is false and
 * it simply discards what it dequeues -- which also drains anything the capture
 * task queued in the moments before the stop.
 */
static void audio_send_task(void *arg)
{
    while (1) {
        size_t len = 0;
        /* The timeout is a liveness tick, not a deadline: it is what lets this
         * task notice a queue that has been deleted or a session that ended. */
        void *frame = xRingbufferReceive(s_audio_rb, &len, pdMS_TO_TICKS(100));
        if (frame == NULL) {
            continue;
        }

        /*
         * CLAIMED BEFORE THE READINESS TEST, NEVER AFTER IT.
         *
         * dg_agent_stop() clears s_ready and then waits for this flag to fall.
         * Raising it after the test left a window between them: the sender had
         * already decided to send, the stop saw an idle sender, and
         * esp_websocket_client_stop() ran underneath a send that was about to
         * take the client lock -- the exact wedge this task exists to prevent.
         *
         * Flag first, test second, and the two orders cannot both miss. Either
         * the stop observes s_sending and waits, or this observes !s_ready and
         * skips. There is no third outcome, and both tasks are pinned to core 0
         * so preemption is the only interleaving there is to cover.
         *
         * The cost is that the flag is briefly raised for a frame that is then
         * discarded, so a stop can spin one extra 10 ms tick in its quiesce
         * loop. That is the whole price.
         */
        s_sending = true;
        if (s_ready && s_client != NULL) {
            /*
             * Ahead of the send, not after it: a send that blocks is exactly when
             * the keepalive must stay out of the way, and stamping afterwards
             * would leave the clock stale for the whole time it was blocked.
             */
            s_last_audio_ms = (uint32_t)(esp_timer_get_time() / 1000);
            (void)esp_websocket_client_send_bin(s_client, (const char *)frame,
                                                (int)len, AUDIO_SEND_TIMEOUT);

            /*
             * Stack headroom, reported once with real history behind it.
             *
             * Two things this deliberately is NOT. Not on entry -- this task's
             * stack has to cover send_bin down through mbedtls, and a mark taken
             * before the first send measures none of it. And not on the FIRST
             * send either: that lands ~4 s after boot, before a serial capture
             * reliably has the port back (the board re-enumerates after a reset),
             * so the one number worth having was the one that always got lost.
             *
             * 200 frames is ~16 s of session, which is late enough to be
             * capturable and long enough to have met a congested send or two.
             * Not more: CONFIG_SESSION_IDLE_TIMEOUT_S ends a session after 15 s
             * of quiet, so a threshold set for a minute of continuous talking is
             * a threshold that never fires.
             * 4 kB here is INTERNAL RAM, the scarce resource on this board, so
             * this is what says whether 4 kB was the right guess.
             */
            static uint32_t sends;
            if (++sends == 200) {
                ESP_LOGI(TAG, "uplink task stack high water mark: %u B free of 4096"
                              " after %" PRIu32 " frames",
                         (unsigned)uxTaskGetStackHighWaterMark(NULL), sends);
            }
        }
        s_sending = false;
        vRingbufferReturnItem(s_audio_rb, frame);
    }
}

static void keepalive_task(void *arg)
{
    /* A constant string, so there is nothing to build and nothing to free. The
     * task outlives any one session: while stopped the guard below simply fails
     * and it goes back to sleep, which is why nothing has to join or delete it
     * on the teardown path. */
    static const char KEEPALIVE[] = "{\"type\":\"KeepAlive\"}";

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(KEEPALIVE_PERIOD_MS));
        /*
         * Claimed before the readiness test, for the reason audio_send_task()
         * spells out at length: dg_agent_stop() clears s_ready and then waits on
         * this flag, so a claim that comes after the test leaves a window where
         * the stop tears the client down under a send that has already been
         * decided on. This one matters even more than the audio flag -- a
         * keepalive is TEXT, so transport_ws.c's LOCAL PATCH 2 cannot drop it and
         * it blocks in poll_write holding the client lock.
         *
         * Which is why the guard below became a positive condition rather than a
         * pair of `continue`s: the flag has to wrap the whole test, and a
         * `continue` would skip past the lowering.
         */
        s_sending_ka = true;
        if (s_ready && esp_websocket_client_is_connected(s_client)) {
            /*
             * WHEN this frame goes out matters more than whether it does, because it
             * is TEXT and transport_ws cannot drop it when the send queue is full --
             * LOCAL PATCH 2 covers binary only, since Settings must never vanish
             * silently. A congested TEXT send blocks in poll_write holding the client
             * lock, stalls the capture task behind it, and finally times out and takes
             * the session down. Observed twice, as a live session dying with mic=
             * frozen for the full SEND_TIMEOUT and rx=0.
             *
             * So send it only when the uplink is genuinely quiet AND uncongested, and
             * there is one condition that means exactly that: the mic gate is shut
             * because the agent is speaking. Nothing is being pushed upstream then, so
             * the send queue is draining rather than filling, and this is also the one
             * case the keepalive exists for -- a long reply during which the device
             * sends no audio at all and Deepgram's ~10 s idle timer is running.
             *
             * Any other time, either audio is flowing and doing the job already, or
             * the uplink is stalled -- and a stall is precisely when adding a
             * blocking TEXT write is worst. An earlier version keyed on "no audio for
             * 2 s" and did exactly that to itself: the stall aged the clock, the clock
             * fired the keepalive, the keepalive killed the session.
             */
            uint32_t quiet_ms = (uint32_t)(esp_timer_get_time() / 1000) -
                                s_last_audio_ms;
            /* The same rule as the `continue` this replaced, stated the other way
             * round: send iff the mic gate is shut, or the uplink has been quiet
             * long enough that nothing else is holding the session open. */
            if (audio_io_playback_active() || quiet_ms >= KEEPALIVE_QUIET_MS) {
                esp_websocket_client_send_text(s_client, KEEPALIVE,
                                               sizeof(KEEPALIVE) - 1, SEND_TIMEOUT);
            }
        }
        s_sending_ka = false;
    }
}

/* ---------------- public API ---------------- */

esp_err_t dg_agent_init(const dg_agent_callbacks_t *callbacks)
{
    if (s_client != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (callbacks != NULL) {
        s_cb = *callbacks;
    }

    s_json = malloc(JSON_REASSEMBLY_MAX);
    if (s_json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * The conversation arena and its debounce timer, then whatever the last run
     * left on flash. Here rather than at the first session, because this is the
     * one place that runs before anything can open a socket -- and a resumed
     * conversation has to be in memory before the first Settings is built, or
     * the device greets someone it was mid-sentence with.
     *
     * A failure to allocate is not fatal: history_add() checks s_arena and the
     * device runs without continuity, which is exactly how it behaved before
     * any of this existed.
     */
    /*
     * Outside the history block below, because the forget path arms it whether
     * or not the arena exists -- and a memory-starved boot is exactly when the
     * missing backstop would leave the model holding a conversation it said it
     * had forgotten, with recording silently off for the rest of the session.
     */
    const esp_timer_create_args_t rargs = {
        .callback = reload_backstop_cb,
        .name = "reload_bkstp",
    };
    esp_timer_create(&rargs, &s_reload_backstop);
    /* Its own timer, for the reasons on s_test_backstop. */
    const esp_timer_create_args_t tbargs = {
        .callback = test_backstop_cb,
        .name = "test_bkstp",
    };
    esp_timer_create(&tbargs, &s_test_backstop);

    s_history_lock = xSemaphoreCreateMutex();
    s_arena = (s_history_lock != NULL)
                  ? heap_caps_malloc(HISTORY_BYTES, MALLOC_CAP_SPIRAM)
                  : NULL;
    /* Internal and DMA-capable, so esp_flash_write() can take its direct path
     * rather than copying the record 32 bytes at a time with the cache off. */
    s_record = heap_caps_malloc(HISTORY_RECORD_MAX,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_arena == NULL || s_record == NULL) {
        /* All or nothing: every guard in this file tests s_arena, so leaving one
         * of the two allocated would make that guard mean the wrong thing. */
        free(s_arena);
        free(s_record);
        s_arena = NULL;
        s_record = NULL;
        ESP_LOGW(TAG, "no memory for history -- conversations will not persist");
    } else {
        const esp_timer_create_args_t targs = {
            .callback = history_flush_timer_cb,
            .name = "hist_flush",
        };
        esp_timer_create(&targs, &s_flush_timer);
        history_load();
    }

    /*
     * THE AUTH HEADER IS BUILT AT RUNTIME, because the key is no longer a
     * compile-time constant -- it comes from NVS, written by the setup portal,
     * with CONFIG_DEEPGRAM_API_KEY as a first-boot seed. See api_key.h.
     *
     * Both buffers are heap, not stack, deliberately. This function is 640 B of
     * frame before them and the two together are ~290 B more; the pattern this
     * project settled on for transient buffers of that size is the heap, in
     * PSRAM where it can be -- see .claude/skills/esp-stack-budget/.
     *
     * The client STRDUPS the header (esp_websocket_client.c, cfg->headers), so
     * it does not have to outlive esp_websocket_client_init(). That is the only
     * reason this can be freed a few lines down instead of living forever.
     */
    char *key = heap_caps_malloc(DG_API_KEY_LEN, MALLOC_CAP_SPIRAM);
    char *auth = heap_caps_malloc(sizeof("Authorization: Token \r\n") + DG_API_KEY_LEN,
                                  MALLOC_CAP_SPIRAM);
    if (key == NULL || auth == NULL) {
        free(key);
        free(auth);
        free(s_json);
        s_json = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (!api_key_load(key)) {
        ESP_LOGE(TAG, "no Deepgram API key: set one through the setup portal, "
                      "or seed CONFIG_DEEPGRAM_API_KEY with `idf.py menuconfig`");
        free(key);
        free(auth);
        free(s_json);
        s_json = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(auth, sizeof("Authorization: Token \r\n") + DG_API_KEY_LEN,
             "Authorization: Token %s\r\n", key);
    /* Done with the plaintext copy; the header keeps the only one this function
     * needs, and that is freed below too. */
    memset(key, 0, DG_API_KEY_LEN);
    free(key);

    const esp_websocket_client_config_t cfg = {
        .uri = DG_AGENT_URI,
        /* The Agent endpoint authenticates on the upgrade request only. */
        .headers = auth,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = WS_RX_BUFFER,
        /* JSON handling plus a user callback per frame needs more than the
         * 4 kB default. */
        .task_stack = 6144,
        .reconnect_timeout_ms = 5000,
        /*
         * esp_transport_connect() runs holding the client's own mutex, so this
         * is also the worst-case wait for a stop that lands mid-handshake. Ten
         * seconds of an unresponsive UI is too long now that stopping is a
         * gesture rather than a reboot.
         */
        .network_timeout_ms = 5000,
        .disable_auto_reconnect = false,
    };

    s_client = esp_websocket_client_init(&cfg);
    /* Copied by now, whether init succeeded or not. */
    memset(auth, 0, strlen(auth));
    free(auth);
    if (s_client == NULL) {
        free(s_json);
        s_json = NULL;
        return ESP_FAIL;
    }

    /*
     * Exactly once, for the life of the process. Registering per session would
     * stack duplicate handlers and fire every callback N times.
     */
    esp_err_t err = esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                                  on_ws_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event registration failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        free(s_json);
        s_json = NULL;
        return err;
    }

    if (xTaskCreate(keepalive_task, "dg_keepalive", 3072, NULL, 4, &s_keepalive_task) != pdPASS) {
        ESP_LOGW(TAG, "keepalive task not created; idle sessions will drop");
    }

    /*
     * The uplink queue and its sender. Created here, once, for the life of the
     * process -- see the note on AUDIO_QUEUE_FRAMES.
     *
     * Priority 5, between the capture task (7, must never wait on this) and the
     * keepalive (4). Core 0 with the rest of the network work, deliberately NOT
     * core 1 where the audio tasks live: this one blocks on the socket by design
     * and must not share a core with the task feeding it.
     *
     * 4096 rather than the keepalive's 3072 because this runs send_bin down
     * through mbedtls 12.5 times a second rather than once every five seconds --
     * the same depth, far more often, so the margin is worth 1 kB.
     */
    s_audio_rb = xRingbufferCreateWithCaps(AUDIO_QUEUE_BYTES, RINGBUF_TYPE_NOSPLIT,
                                           MALLOC_CAP_SPIRAM);
    if (s_audio_rb == NULL) {
        ESP_LOGE(TAG, "no PSRAM for the uplink queue");
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        free(s_json);
        s_json = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(audio_send_task, "dg_uplink", 4096, NULL, 5,
                                &s_send_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "uplink task not created");
        vRingbufferDeleteWithCaps(s_audio_rb);
        s_audio_rb = NULL;
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        free(s_json);
        s_json = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t dg_agent_start(void)
{
    if (s_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Stale bytes from a message that was cut off by the previous teardown, and
     * a drop that teardown may have left mid-message. */
    s_json_len = 0;
    s_json_dropping = false;

    ESP_LOGI(TAG, "connecting to %s", DG_AGENT_URI);
    esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "client start failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t dg_agent_stop(void)
{
    if (s_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_ready = false;
    /*
     * NOTHING ABOUT THE OLD SESSION MAY OUTLIVE IT. Every one of these is state
     * that only means anything inside the conversation it was created in, and
     * each one carried into the next session is its own bug:
     *
     *   - a deferred reload fires one turn into the NEXT session and closes a
     *     socket nobody asked to close. The stop already achieves whatever the
     *     reload was for, since the next Settings is built fresh either way.
     *   - the reload's backstop does the same thing, later and with less
     *     warning, and can consume a reload that belongs to something else --
     *     a voice change, say, cut off mid-sentence.
     *   - an unanswered "are you sure?" would let a single new_conversation call
     *     in a FRESH session wipe the history with no question asked in it. The
     *     whole point of two calls is that somebody was asked.
     *
     * The display test is the exception, and the note below says why.
     *
     * CLEARED AFTER THE CLIENT STOPS, not here. Everything between this point
     * and esp_websocket_client_stop() is time in which the event task is still
     * dispatching, so a FunctionCallRequest arriving in that window -- up to
     * AUDIO_SEND_TIMEOUT plus a margin -- would re-arm what had just been
     * cleared, and a surviving "are you sure?" is the one that matters: a single
     * new_conversation call in the next session would then wipe with no question
     * asked in it.
     */
    /*
     * s_test_pending is NOT cleared, here or on the socket-drop path. The
     * argument for dropping a reload -- that the next session is built fresh, so
     * the reload has already happened -- says nothing about a display test,
     * which no reconnect performs. Discarding it would silently throw away
     * something the user asked for out loud.
     */

    /*
     * QUIESCE THE UPLINK BEFORE TOUCHING THE CLIENT. This is the whole reason
     * the audio send lives on its own task.
     *
     * s_ready is already false, so audio_send_task() will not START another
     * send; what is left is a send that is already in flight, holding the client
     * lock. esp_websocket_client_stop() cannot survive that -- it waits on
     * STOPPED_BIT with portMAX_DELAY -- so waiting here, where the wait is
     * BOUNDED, is what turns a permanent hang into a delay.
     *
     * The bound is AUDIO_SEND_TIMEOUT plus a margin, because that is the deadline
     * the in-flight send is itself running against: it has to return by then,
     * successfully or not. Proceeding anyway on expiry is deliberate -- it leaves
     * us exactly where the old code always was, rather than adding a second way
     * to hang.
     */
    const int64_t quiesce_deadline = esp_timer_get_time() +
                                     (int64_t)pdTICKS_TO_MS(AUDIO_SEND_TIMEOUT) * 1000 + 500000;
    while ((s_sending || s_sending_ka) && esp_timer_get_time() < quiesce_deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_sending || s_sending_ka) {
        ESP_LOGW(TAG, "still sending at the stop deadline (audio=%d keepalive=%d) "
                      "-- stopping anyway", (int)s_sending, (int)s_sending_ka);
    }

    /* Nothing queued is worth sending now, and leaving it would have the sender
     * push stale audio into the next session's socket. */
    if (s_audio_rb != NULL) {
        size_t len = 0;
        void *frame;
        while ((frame = xRingbufferReceive(s_audio_rb, &len, 0)) != NULL) {
            vRingbufferReturnItem(s_audio_rb, frame);
        }
    }

    /*
     * The CLOSE frame is worth sending: Deepgram bills on session duration, and
     * a half-open socket only finalises at the server's idle timeout. But only
     * when connected -- otherwise close() is a guaranteed no-op that just logs
     * an error. Neither call is checked: close() returns ESP_FAIL if the client
     * is already down, and stop() returns ESP_FAIL when a successful close has
     * already stopped the task. Both are expected, not failures.
     *
     * EXCEPT THAT THE CLOSE FRAME IS NOT SENT AT ALL ANY MORE, and the reason is
     * an upstream bug rather than a decision about billing.
     *
     * esp_websocket_client_close() TAKES A TIMEOUT AND IGNORES IT. It forwards
     * portMAX_DELAY to esp_websocket_client_send_close() -- see
     * esp_websocket_client_close_with_optional_body() in the managed component --
     * so the 1000 ms this used to pass never meant anything. On a socket that is
     * not draining, that write never returns; and because it runs BEFORE
     * esp_websocket_client_stop(), it hangs the caller outright. session_ctl's
     * do_stop() stalls with "stopping" on the panel, s_busy latches, and every
     * gesture is refused until the board is physically reset.
     *
     * Measured on hardware 2026-08-27, repeatedly, on an access point that could
     * not drain the uplink.
     *
     * TWO ATTEMPTS AT SENDING IT ONLY WHEN SAFE BOTH FAILED, and the second is
     * why this is now unconditional: the first read the send's return value,
     * which lies by design (transport_ws.c LOCAL PATCH 2), and the second timed
     * the send, which does not lie but is still a guess -- and a guess whose
     * failure mode is a device that needs a reset is not a guess worth making.
     *
     * Skipping it costs a few seconds of billing per session: without a CLOSE the
     * socket is half-open and Deepgram finalises it at its own idle timer instead
     * of immediately. That is the whole price, and it buys a stop bounded by
     * esp_websocket_client_stop() alone -- ~4 s on a congested link, measured,
     * and it completes.
     *
     * The way back, if that billing ever matters more than this did: vendor
     * esp_websocket_client the way components/tcp_transport already vendors its
     * upstream, and make close_with_optional_body() honour its own timeout. Worth
     * knowing before anyone tries -- on a congested socket the frame will not go
     * out even with a correct deadline, so the payoff is failing in 1 s rather
     * than hanging, which is what this line already achieves for free.
     */
    (void)esp_websocket_client_stop(s_client);

    /*
     * Only now. See the note at the top of this function: until the client is
     * stopped its event task is still dispatching, and a message landing in that
     * window can re-arm any of these.
     */
    s_reload_pending = false;
    /* The reload's timer only. The display test has its own and deliberately
     * outlives a stop -- see the note below. */
    if (s_reload_backstop != NULL) {
        esp_timer_stop(s_reload_backstop);
    }
    s_clear_armed_ms = 0;
    s_clear_armed_turns = 0;

    ESP_LOGI(TAG, "session stopped");
    return ESP_OK;
}

bool dg_agent_is_ready(void)
{
    return s_ready;
}

esp_err_t dg_agent_send_audio(const void *pcm, size_t len)
{
    if (!s_ready || len == 0 || s_audio_rb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * NON-BLOCKING, AND THAT IS THE ENTIRE POINT. This runs on the capture task,
     * which also drives the display tap and must come back for the next 80 ms
     * chunk whatever the network is doing. A zero wait means a congested uplink
     * costs one frame here instead of stalling the task that owns the microphone
     * -- and, more importantly, means this task never holds the client lock that
     * dg_agent_stop() needs.
     */
    if (xRingbufferSend(s_audio_rb, pcm, len, 0) != pdTRUE) {
        /* Counted rather than logged: at 12.5 frames/s a log line per drop is
         * its own denial of service, and the count rides the TLM line. */
        s_audio_dropped++;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

uint32_t dg_agent_audio_dropped(void)
{
    return s_audio_dropped;
}

uint32_t dg_agent_transport_dropped(void)
{
    return transport_ws_local_dropped_frames();
}

