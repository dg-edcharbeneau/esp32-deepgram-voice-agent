#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "audio_io.h"
#include "dg_agent.h"
#include "session_ctl.h"
#include "ui.h"

static const char *TAG = "session_ctl";

/*
 * Priority 4 on core 0. Core 0 keeps the teardown -- a TLS write plus whatever
 * the client does to unwind -- off core 1, where the audio tasks live. Wi-Fi
 * (23), lwIP (18) and esp_timer (22) all sit far above, so a priority-4 task
 * cannot starve the stack it is talking to.
 *
 * The stack has to cover esp_websocket_client_close() down through
 * mbedtls_ssl_write(); the client's own task is given 6144 for the same work.
 */
#define CTL_TASK_PRIO  4
#define CTL_TASK_CORE  0
#define CTL_TASK_STACK 5120

/*
 * Quiet period after an action completes, before another is accepted.
 *
 * Ending a conversation is disruptive and the only control is a touch panel, so
 * the cost of ignoring a real press is far lower than the cost of acting on an
 * accidental one. Measured from completion rather than from the request, so a
 * slow teardown does not eat into it.
 */
#define COOLDOWN_MS 1500

typedef enum {
    REQ_START = 1,
    REQ_TOGGLE,
    REQ_RESTART,
    REQ_RELOAD,   /* same work as RESTART, but keeps the conversation */
    REQ_STOP,     /* stop if running; never starts */
    REQ_RELOAD_SOON, /* like RELOAD, but a no-op when nothing is running */
} request_t;

static TaskHandle_t s_task;
/*
 * "Write the conversation out when you next get a moment."
 *
 * A FLAG RATHER THAN A REQUEST, and that is not a style choice. Every request
 * below travels as the notification VALUE, sent with eSetValueWithOverwrite --
 * so a flush arriving while a toggle was pending would REPLACE the toggle, and
 * a tap would silently do nothing. The flag rides alongside instead: the worker
 * checks it on every wakeup and after every action, so a flush never displaces
 * a gesture and never gets displaced by one.
 */
static volatile bool s_flush_wanted;
/*
 * "Reopen the session when you get a moment, and only if there is one."
 *
 * A flag for the same reason s_flush_wanted is one, and then a second reason on
 * top. session_ctl_request_reload() posts a VALUE with eSetValueWithOverwrite
 * and skips the cooldown, which is right for a reload the agent asked for --
 * but the forget backstop fires on a timer, and a timer that lands on the tap
 * the user just made would replace their toggle with a reload and reopen the
 * session they were ending. Riding alongside the value instead makes that
 * impossible, and the s_running test below makes a reload of a stopped session
 * the no-op it should always have been.
 */
static volatile bool s_reload_wanted;
/* Whether the last start attempt failed, so the idle label can say so. */
static volatile bool s_start_failed;
static dg_agent_callbacks_t s_callbacks;
static volatile bool s_running; /* written by the worker, read by the status loop */
static volatile bool s_busy;    /* an action is in progress */
/*
 * When the current action started, for session_ctl_busy_for_ms(). 0 while idle.
 *
 * MILLISECONDS IN 32 BITS, and that is the whole point rather than a unit
 * preference. This is written by the worker below and read by app_main, and a
 * 32-bit CPU stores a 64-bit value in TWO HALVES -- so a reader that landed
 * between them saw a timestamp that never existed. Same fault as the one
 * audio_io.c fixed for s_speech_us, and the same remedy: narrow to 32-bit ms,
 * where a store is indivisible and there is nothing left to tear.
 *
 * It mattered here more than most. main.c reboots the device when this reports
 * more than 30 s, so a torn read did not degrade a measurement -- it restarted
 * a healthy board mid-toggle and logged EVT sessionstuck about a session that
 * was never stuck. Past 2^32 us of uptime (~71 min) the high word is nonzero,
 * which is all a tear needs to invent minutes of busy time out of a stamp that
 * had just been cleared.
 */
static volatile uint32_t s_busy_since_ms;
/*
 * Earliest time the next request is taken. 32-bit ms for the reason the busy
 * stamp above gives: written by the worker, read by the LVGL and button tasks,
 * and a 64-bit value is stored in two halves. A tear here is milder than the
 * backstop's -- a gesture wrongly refused, or wrongly accepted inside the
 * cooldown -- but it is the same defect and the same one-line remedy.
 */
static volatile uint32_t s_ready_at_ms;
static bool s_logged_hwm;

/* Milliseconds since boot, truncated. One 32-bit store, so no writer can tear
 * it and no reader can catch it half-updated. */
static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

uint32_t session_ctl_busy_for_ms(void)
{
    /*
     * THE FLAG FIRST, THE STAMP SECOND, and the ordering is load bearing: the
     * worker writes the stamp before raising the flag and lowers the flag before
     * clearing the stamp, so a reader that sees s_busy is guaranteed a stamp
     * that belongs to the action it is asking about.
     */
    if (!s_busy) {
        return 0;
    }
    const uint32_t since = s_busy_since_ms;
    if (since == 0) {
        /* The action completed between the two reads. Not busy, and reporting
         * "since boot" here is exactly the false positive above. */
        return 0;
    }
    /* Unsigned subtraction, so this stays correct across the 49.7-day wrap in a
     * way that comparing the stamps themselves would not. */
    return now_ms() - since;
}

bool session_ctl_is_running(void)
{
    return s_running;
}

static void do_stop(void)
{
    if (!s_running) {
        return;
    }
    ESP_LOGI(TAG, "stopping session");

    /* Closing the socket raises DISCONNECTED, which would repaint the label
     * over "stopping". Suppressed until the next start is under way. */
    dg_agent_suppress_state_events(true);
    ui_set_status("stopping", false);

    /* Before the teardown: stop feeding the socket, and stop the ring reacting
     * to the room while we are on our way to "stopped". */
    audio_io_capture_set_enabled(false);

    /* Deepgram sends a turn faster than it plays, so without this the device
     * keeps talking for seconds after the press. First flush is for latency --
     * it silences the speaker now rather than after the socket finishes
     * closing, which can take a few hundred ms. */
    audio_io_flush();

    dg_agent_stop();

    /* Again, because the socket was still open across the first one: anything
     * on_audio queued in between would otherwise play on after the stop. */
    audio_io_flush();

    /* Only now -- audio_io_reset() clears state owned by the WebSocket task,
     * which dg_agent_stop() has just brought to a halt. */
    audio_io_reset();

    s_running = false;
    ui_set_stopped(true);
    /*
     * "saved" is the only sign, while the device is quiet, that there is still
     * a conversation here -- and it is also the cue that a hold will offer to
     * forget it. Without it the stopped screen looks identical whether the next
     * tap resumes or starts over.
     */
    session_ctl_repaint_idle_status();

    /*
     * Unconditional, and after the teardown rather than before it: a deliberate
     * or idle stop should not sit on an unwritten turn for the debounce window,
     * and this task is the only one allowed to block on flash.
     */
    dg_agent_flush_history();
}

static void do_start(void)
{
    if (s_running) {
        return;
    }
    ESP_LOGI(TAG, "starting session");
    s_start_failed = false;

    /*
     * Safe to release here: dg_agent_stop() only returns once the client task
     * has dispatched its final event, so nothing is queued behind us. Releasing
     * before the start rather than after avoids a window where the new session's
     * first event is swallowed and the label sticks on "connecting".
     */
    dg_agent_suppress_state_events(false);

    ui_set_stopped(false);
    /* "resuming" is the whole user-visible payoff of persistence: it says the
     * conversation outlived whatever just happened, before a word is spoken. */
    ui_set_status(dg_agent_has_history() ? "resuming" : "connecting", false);

    esp_err_t err = dg_agent_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        s_start_failed = true;
        ui_set_stopped(true);
        ui_set_status("error", false);
        return;
    }

    s_running = true;
    audio_io_capture_set_enabled(true);
}

static void session_ctl_task(void *arg)
{
    while (1) {
        uint32_t req = 0;
        xTaskNotifyWait(0, UINT32_MAX, &req, portMAX_DELAY);

        /*
         * Promote a deferred reload to a REAL request before anything else
         * looks at req. Doing that work inline was a hole: a deferred wakeup
         * arrives with req == 0, which is the case that deliberately does not
         * raise s_busy, so a multi-second teardown and TLS reconnect would run
         * with both is_running() and is_busy() false -- gestures accepted with
         * no cooldown, main.c's hold gate falling through to the forget path,
         * and the 30 s stuck-session watchdog blind to a wedge.
         *
         * When a real request is already in hand, leave the flag set and poke
         * the task again: a reload is never urgent, and a gesture must never
         * wait behind one.
         */
        if (s_reload_wanted) {
            if (req == 0) {
                s_reload_wanted = false;
                req = REQ_RELOAD_SOON;
            } else {
                xTaskNotify(s_task, 0, eNoAction);
            }
        }

        /*
         * Raise the flag BEFORE the flush, not after. The flush blocks for a
         * sector erase, and this task holding a request it has not started while
         * looking idle is a window in which request() accepts and latches
         * another gesture -- which then runs with the cooldown never re-checked.
         * A few instructions of that window is unavoidable; a full erase of it
         * is not.
         *
         * Only for a real request, though. A flush-only wakeup carries req == 0
         * and does no start or stop, so raising s_busy for it would make the
         * device refuse gestures ("request ignored (busy)") for the length of a
         * flash write it is not doing -- and would briefly send a hold to
         * restart instead of offering to forget, since main.c's gate reads this
         * too.
         *
         * Stamp before flag, so a reader that sees s_busy cannot see a stale or
         * zeroed stamp behind it. The clear paths below do the reverse.
         */
        if (req != 0) {
            s_busy_since_ms = now_ms();
            s_busy = true;
        }

        /*
         * BEFORE the switch, because the `default` arm below continues straight
         * back to the wait, so a check at the bottom would miss exactly the
         * flush-only wakeups this exists for. Doing it first also means the
         * conversation reaches flash before a stop tears the session down on top
         * of it.
         */
        if (s_flush_wanted) {
            s_flush_wanted = false;
            dg_agent_flush_history();
        }

        if (req == 0) {
            continue;
        }

        switch ((request_t)req) {
        case REQ_START:
            do_start();
            break;
        case REQ_TOGGLE:
            if (s_running) {
                /*
                 * STOPS ONLY. This used to clear the history too, on the reading
                 * that ending a conversation on purpose means ending it -- but
                 * a tap is how you stop the device streaming, and people tap it
                 * for reasons that have nothing to do with being finished. Now
                 * every stop behaves like the idle timeout always did, and
                 * forgetting is something you ask for and confirm: the
                 * new_conversation function, or hold-again on a stopped device.
                 */
                do_stop();
            } else {
                do_start();
            }
            break;
        case REQ_STOP:
            /*
             * Idempotent, unlike REQ_TOGGLE. The idle timeout must never be able
             * to START a session -- a toggle that raced the running flag would
             * turn a cost-saving measure into an unattended session that bills
             * until someone notices.
             *
             * History is kept: the device stopped because nobody was talking, not
             * because anyone ended the conversation, so picking it up again should
             * resume rather than re-greet.
             */
            if (s_running) {
                do_stop();
            }
            break;
        case REQ_RESTART:
            /* Unconditional: the point of the gesture is to recover a session
             * that is up but wedged, so it must work from either state. */
            do_stop();
            do_start();
            break;
        case REQ_RELOAD_SOON:
            /*
             * A reload of a stopped device should leave it stopped -- the whole
             * point of the deferred form.
             *
             * `continue`, not `break`, and for the reason the `default` arm
             * below uses it: break falls through to the unconditional cooldown
             * stamp, so a backstop firing at a stopped device would refuse the
             * user's next tap for 1.5 s. Nothing happened; nothing should be
             * charged for it.
             */
            if (!s_running) {
                s_busy = false;
                s_busy_since_ms = 0;
                continue;
            }
            ESP_LOGI(TAG, "reloading session (deferred)");
            do_stop();
            do_start();
            break;
        case REQ_RELOAD:
            /* Identical mechanically. Distinct so the intent is legible in the
             * log, and so it can skip the debounce on the way in. */
            ESP_LOGI(TAG, "reloading session to apply a setting change");
            do_stop();
            do_start();
            break;
        default:
            s_busy = false;
            s_busy_since_ms = 0;
            continue;
        }

        /* Stamped after the work, so the quiet period is 1.5 s of settled
         * device rather than 1.5 s that a slow stop already spent. */
        s_ready_at_ms = now_ms() + COOLDOWN_MS;
        /* Flag down before the stamp is cleared -- the mirror of the order on
         * the way in, and what makes both orders safe for the reader. */
        s_busy = false;
        s_busy_since_ms = 0;

        /* Repeated toggling is the thing most likely to leak, and internal RAM
         * is what runs out first. Largest-block matters more than the total. */
        ESP_LOGI(TAG, "%s | internal=%u B (largest %u B)",
                 s_running ? "running" : "stopped",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

        if (!s_logged_hwm) {
            s_logged_hwm = true;
            ESP_LOGI(TAG, "control task stack high water mark: %u B",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
    }
}

esp_err_t session_ctl_start(const dg_agent_callbacks_t *callbacks)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (callbacks != NULL) {
        s_callbacks = *callbacks;
    }

    esp_err_t err = dg_agent_init(&s_callbacks);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "agent init failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreatePinnedToCore(session_ctl_task, "session_ctl", CTL_TASK_STACK, NULL,
                                CTL_TASK_PRIO, &s_task, CTL_TASK_CORE) != pdPASS) {
        /* Report the heap, because this call being wrapped in ESP_ERROR_CHECK by
         * the caller means a bare ESP_FAIL aborts the boot with a backtrace that
         * points here and says nothing about why. */
        ESP_LOGE(TAG, "could not create the control task (%d B stack): "
                      "internal free %u B, largest block %u B",
                 CTL_TASK_STACK,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return ESP_FAIL;
    }
    return ESP_OK;
}

/*
 * eSetValueWithOverwrite, so presses during a transition collapse into one
 * pending request rather than queueing a runaway sequence of toggles.
 */
static void request_now(request_t req)
{
    if (s_task != NULL) {
        xTaskNotify(s_task, (uint32_t)req, eSetValueWithOverwrite);
    }
}

static void request(request_t req)
{
    if (s_task == NULL) {
        return;
    }

    /*
     * Debounce. Rejecting outright rather than queueing: eSetValueWithOverwrite
     * would collapse a flurry into one *extra* toggle that lands after the
     * current one finishes, which is exactly the accidental double-trigger this
     * is meant to stop.
     */
    /* Wrap-safe: the difference is compared against half the range rather than
     * the deadline against now, the same idiom main.c's deadlines use. At boot
     * s_ready_at_ms is 0, which correctly reads as "no cooldown". */
    const bool cooling = (now_ms() - s_ready_at_ms) >= (UINT32_MAX / 2);
    if (s_busy || cooling) {
        ESP_LOGI(TAG, "request ignored (%s)", s_busy ? "busy" : "cooldown");
        return;
    }

    xTaskNotify(s_task, (uint32_t)req, eSetValueWithOverwrite);
}

/* The boolean the gates want; session_ctl_busy_for_ms() is a duration and reads
 * zero for the first millisecond of an action. See the header. */
bool session_ctl_is_busy(void)
{
    return s_busy;
}

/*
 * ONE OWNER FOR THE STOPPED LABEL. main.c needs to restore it after a transient
 * overlay (the forget offer) lapses, and a copy of the rule there went stale the
 * moment this one grew a case: it repainted "stopped" over a failed start's
 * "error", hiding the only thing the user needed to see.
 */
void session_ctl_repaint_idle_status(void)
{
    /*
     * s_running only. NOT s_busy -- do_stop() calls this while it is still the
     * action in progress, so a busy guard here would make the one caller that
     * must paint do nothing, and leave "stopping" on the panel forever. That is
     * also the exact signature main.c watches for as a wedged device.
     *
     * "Do not repaint mid-transition" is a caller's concern and stays with the
     * caller; what lives here is only the rule for WHAT a stopped device says.
     */
    if (s_running) {
        return;
    }
    if (s_start_failed) {
        ui_set_status("error", false);
        return;
    }
    /* ASCII only: lv_font_montserrat_24 is built over 0x20-0x7F, so anything
     * prettier than a comma renders as a placeholder box. */
    ui_set_status(dg_agent_has_history() ? "stopped, saved" : "stopped", false);
}

/*
 * Deliberately not a request_t. See s_flush_wanted above: the notification value
 * is a single slot, and persistence must never be able to eat a user's press.
 *
 * eNoAction is what makes that true. It wakes the worker WITHOUT writing the
 * value, so a request already pending in that slot survives the wakeup intact;
 * eSetValueWithOverwrite here would have destroyed it, which is the whole
 * failure the flag exists to avoid. When nothing is pending the worker wakes on
 * a value of 0, falls through its switch, and finds the flag.
 */
void session_ctl_request_history_flush(void)
{
    s_flush_wanted = true;
    if (s_task != NULL) {
        xTaskNotify(s_task, 0, eNoAction);
    }
}

/* Same mechanism, same reason: see s_reload_wanted. */
void session_ctl_request_reload_soon(void)
{
    s_reload_wanted = true;
    if (s_task != NULL) {
        xTaskNotify(s_task, 0, eNoAction);
    }
}

void session_ctl_request_reload(void)  { request_now(REQ_RELOAD); }
void session_ctl_request_start(void)   { request(REQ_START); }
void session_ctl_request_toggle(void)  { request(REQ_TOGGLE); }
void session_ctl_request_stop(void)    { request(REQ_STOP); }
void session_ctl_request_restart(void) { request(REQ_RESTART); }
