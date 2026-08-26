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
#include "aec_bench.h"
#include "audio_codecs.h"
#include "audio_io.h"
#include "boot_button.h"
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
static volatile int64_t s_activity_us;

/* Follow-up interval after a face switch -- long enough for a frame or two to
 * land in the window, short enough not to smear the transition. */
#define TELEMETRY_SWITCH_MS 200

/* While waiting for the speaker to drain before the display test. */
#define TEST_ENTRY_POLL_MS 100

static void note_activity(void)
{
    s_activity_us = esp_timer_get_time();
}

static const char *state_name(dg_agent_state_t state)
{
    switch (state) {
    case DG_AGENT_DISCONNECTED: return "disconnected";
    case DG_AGENT_CONNECTED:    return "connected";
    case DG_AGENT_READY:        return "ready";
    case DG_AGENT_ERROR:        return "error";
    }
    return "?";
}

static void on_state(dg_agent_state_t state, void *ctx)
{
    ESP_LOGI(TAG, "agent session %s", state_name(state));
    if (state == DG_AGENT_CONNECTED) {
        /* A new socket is a new conversation, so the status line should describe
         * this one rather than every session since boot. */
        s_audio_bytes = 0;
        s_turns = 0;
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
        /* Frozen, not merely dim: this one has stopped trying. */
        ui_set_failed(true);
        ui_set_behaviour(UI_BEHAVIOUR_DISCONNECTED);
        break;
    case DG_AGENT_DISCONNECTED:
        /* Between attempts. session_ctl reports a deliberate stop separately, so
         * arriving here means the socket went away on its own. */
        ui_set_behaviour(UI_BEHAVIOUR_CONNECTING);
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
static bool s_test_entry_pending;
static int64_t s_test_entry_deadline_us;

/*
 * An interrupted turn's mute, and the deadline that guarantees it ends.
 *
 * 32-bit ms rather than the int64_t microseconds used elsewhere in this file,
 * because three tasks touch it -- the LVGL task sets it, the WebSocket task
 * clears it on AgentAudioDone, this loop clears it on the deadline -- and a
 * 32-bit store is indivisible where a 64-bit one is two halves. Same reasoning
 * as the split in audio_io.c. The clear/set race between them is benign: the
 * worst outcome is a mute that lasts until the deadline.
 *
 * The deadline exists because AgentAudioDone is not guaranteed. A turn that
 * never reports done would otherwise leave the device permanently silent with no
 * symptom other than a device that has stopped talking.
 */
#define MUTE_MAX_MS 30000
static volatile uint32_t s_mute_deadline_ms;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void end_interrupt(const char *why)
{
    if (s_mute_deadline_ms == 0) {
        return;
    }
    s_mute_deadline_ms = 0;
    audio_io_mute_playback(false);
    ESP_LOGI(TAG, "EVT interrupt-end (%s)", why);
}

/* How long to wait for the speaker to drain before starting anyway. Long enough
 * for a sentence the agent has already finished sending, short enough that a
 * playback path stuck busy cannot swallow the feature.
 *
 * NOTE the ring holds 12.3 s, not the ~6 s four places in this tree used to
 * claim, so this 8 s is NOT the "cannot possibly still be playing" bound it reads
 * as. A reply longer than 8 s of audio still gets cut off mid-word -- the exact
 * fault the comment above enter_display_test() says this deferral exists to
 * prevent. Unmeasured: no logged reply has been long enough to trip it. */
#define TEST_ENTRY_WAIT_US (8 * 1000000)

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
    s_test_entry_pending = true;
    s_test_entry_deadline_us = esp_timer_get_time() + TEST_ENTRY_WAIT_US;
}

static void on_agent_audio_done(void *ctx)
{
    note_activity();
    /* The turn is over, so whatever was being discarded has stopped arriving. */
    end_interrupt("turn done");
    ESP_LOGI(TAG, "turn complete, %" PRIu32 " audio bytes received", s_audio_bytes);
}

/* Runs on the LVGL task with the LVGL lock held: signal only, never block. */
static void on_gesture(ui_gesture_t gesture)
{
    switch (gesture) {
    case UI_TAP:
        ESP_LOGI(TAG, "EVT tap");
        session_ctl_request_toggle();
        break;
    case UI_HOLD:
        ESP_LOGI(TAG, "EVT hold");
        session_ctl_request_restart();
        break;

    case UI_INTERRUPT:
        /*
         * Both halves, or neither works: the flush silences the ring, the mute
         * stops Deepgram's remaining audio refilling it. Deliberately NOT routed
         * through session_ctl -- request() drops anything arriving while busy or
         * inside COOLDOWN_MS, which is exactly when someone interrupts, and both
         * calls here are a single flag store so they are safe on the LVGL task.
         *
         * With the mic gate on, the reward is immediate: playback goes inactive,
         * capture reopens within PLAYBACK_TAIL_MS, and the user can just talk.
         */
        if (audio_io_playback_active()) {
            ESP_LOGI(TAG, "EVT interrupt");
            audio_io_flush();
            audio_io_mute_playback(true);
            s_mute_deadline_ms = now_ms() + MUTE_MAX_MS;
            note_activity();
        } else {
            /* Nothing to stop. Logged because a ring tap that appears to do
             * nothing is otherwise indistinguishable from a dead touch panel. */
            ESP_LOGI(TAG, "EVT interrupt (nothing playing)");
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
    /* The ring has nothing to visualise with no session, and a calm screen
     * showing the network name is the instruction. */
    ui_set_stopped(true);
    ui_set_status(wifi_prov_ap_name(), false);

    if (wifi_prov_start() != ESP_OK) {
        ESP_LOGE(TAG, "could not start the setup portal");
        ui_set_status("setup failed", false);
        wifi_prov_run();
    }

    /*
     * The AP as something a camera can act on. A phone that scans this joins
     * the network directly, which skips the one step of this whole flow that
     * involves reading characters off a 466 px round panel and retyping them.
     *
     * Static storage because ui keeps the pointer, not the bytes.
     */
    static char qr[64];
    snprintf(qr, sizeof(qr), "WIFI:T:nopass;S:%s;;", wifi_prov_ap_name());
    ui_show_qr(qr);

    wifi_prov_run();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Wi-Fi keeps calibration data in NVS; a stale partition must be wiped
         * or esp_wifi_init() fails on a board flashed with a different build. */
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

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

#if CONFIG_AEC_BENCH
    /*
     * AFTER ui_start(), deliberately. The display's contiguous render buffer is
     * the largest single allocation this firmware makes, and a canceller priced
     * before it would be priced against a heap the device is never actually in.
     * a4fa137 failed on exactly that distinction.
     */
    aec_bench_run();
#endif

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

    ESP_ERROR_CHECK(session_ctl_start(&s_callbacks));

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
            bool timed_out = esp_timer_get_time() > s_test_entry_deadline_us;
            if (drained || timed_out) {
                s_test_entry_pending = false;
                ESP_LOGI(TAG, "EVT test entering (%s)",
                         drained ? "speaker drained" : "wait timed out");
                enter_display_test();
            } else {
                wait_ms = TEST_ENTRY_POLL_MS;
            }
        }

        /* Unsigned compare, so it is correct across the 49-day wrap. */
        if (s_mute_deadline_ms != 0 &&
            (now_ms() - s_mute_deadline_ms) < (UINT32_MAX / 2)) {
            end_interrupt("deadline");
        }

        uint32_t played, dropped, captured;
        audio_io_stats(&played, &dropped, &captured);
        uint32_t lane_mic, lane_ref, lane_dead;
        audio_io_lane_peaks(&lane_mic, &lane_ref, &lane_dead);

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
         * Gating the check is only half of it. s_activity_us keeps accumulating
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

        if (CONFIG_SESSION_IDLE_TIMEOUT_S > 0 && session_ctl_is_running() &&
            ready_now && s_activity_us != 0) {
            int64_t quiet_us = esp_timer_get_time() - s_activity_us;
            if (quiet_us > (int64_t)CONFIG_SESSION_IDLE_TIMEOUT_S * 1000000) {
                ESP_LOGI(TAG, "EVT idletimeout after=%.1fs",
                         (double)quiet_us / 1000000.0);
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
                 " played=%" PRIu32 " drop=%" PRIu32
                 " lane=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " ovf=%" PRIu32
                 " heap=%" PRIu32 " int=%u intmax=%u ifree=%u iblocks=%u"
                 " ialloc=%u",
                 (double)esp_timer_get_time() / 1000000.0,
                 t.face, t.face_changed ? "*" : "", t.behaviour, t.source,
                 !session_ctl_is_running() ? "stopped"
                     : dg_agent_is_ready() ? "ready" : "notready",
                 t.frames, t.fps, t.draw_avg_ms, t.draw_max_ms,
                 t.amp_avg, t.amp_max, t.low_avg, t.low_max,
                 t.mid_avg, t.mid_max, t.high_avg, t.high_max,
                 t.peak_mic, t.peak_agent,
                 s_turns, captured, s_audio_bytes, played, dropped,
                 lane_mic, lane_ref, lane_dead, audio_codecs_rx_overruns(),
                 esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)ih.total_free_bytes, (unsigned)ih.free_blocks,
                 (unsigned)ih.allocated_blocks);
    }
}
