#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "audio_io.h"
#include "dg_agent.h"
#include "session_ctl.h"
#include "spectrum_ui.h"

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
} request_t;

static TaskHandle_t s_task;
static dg_agent_callbacks_t s_callbacks;
static volatile bool s_running; /* written by the worker, read by the status loop */
static volatile bool s_busy;    /* an action is in progress */
static volatile int64_t s_ready_at_us; /* earliest time the next request is taken */
static bool s_logged_hwm;

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
    spectrum_ui_set_status("stopping", false);

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
    spectrum_ui_set_stopped(true);
    spectrum_ui_set_status("stopped", false);
}

static void do_start(void)
{
    if (s_running) {
        return;
    }
    ESP_LOGI(TAG, "starting session");

    /*
     * Safe to release here: dg_agent_stop() only returns once the client task
     * has dispatched its final event, so nothing is queued behind us. Releasing
     * before the start rather than after avoids a window where the new session's
     * first event is swallowed and the label sticks on "connecting".
     */
    dg_agent_suppress_state_events(false);

    spectrum_ui_set_stopped(false);
    spectrum_ui_set_status("connecting", false);

    esp_err_t err = dg_agent_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        spectrum_ui_set_stopped(true);
        spectrum_ui_set_status("error", false);
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

        s_busy = true;

        switch ((request_t)req) {
        case REQ_START:
            do_start();
            break;
        case REQ_TOGGLE:
            if (s_running) {
                do_stop();
            } else {
                do_start();
            }
            break;
        case REQ_RESTART:
            /* Unconditional: the point of the gesture is to recover a session
             * that is up but wedged, so it must work from either state. */
            do_stop();
            do_start();
            break;
        default:
            s_busy = false;
            continue;
        }

        /* Stamped after the work, so the quiet period is 1.5 s of settled
         * device rather than 1.5 s that a slow stop already spent. */
        s_ready_at_us = esp_timer_get_time() + (COOLDOWN_MS * 1000);
        s_busy = false;

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
        return ESP_FAIL;
    }
    return ESP_OK;
}

/*
 * eSetValueWithOverwrite, so presses during a transition collapse into one
 * pending request rather than queueing a runaway sequence of toggles.
 */
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
    if (s_busy || esp_timer_get_time() < s_ready_at_us) {
        ESP_LOGI(TAG, "request ignored (%s)", s_busy ? "busy" : "cooldown");
        return;
    }

    xTaskNotify(s_task, (uint32_t)req, eSetValueWithOverwrite);
}

void session_ctl_request_start(void)   { request(REQ_START); }
void session_ctl_request_toggle(void)  { request(REQ_TOGGLE); }
void session_ctl_request_restart(void) { request(REQ_RESTART); }
