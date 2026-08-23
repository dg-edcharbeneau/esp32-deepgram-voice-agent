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
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

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

static void on_agent_audio_done(void *ctx)
{
    note_activity();
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
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_UI_TELEMETRY_MS));

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
        if (CONFIG_SESSION_IDLE_TIMEOUT_S > 0 && session_ctl_is_running() &&
            s_activity_us != 0) {
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

        ESP_LOGI(TAG,
                 "TLM up=%.1f face=%s beh=%s src=%s sess=%s "
                 "frames=%" PRIu32 " fps=%.1f draw=%.1f/%.1f "
                 "amp=%.3f/%.3f low=%.2f/%.2f mid=%.2f/%.2f high=%.2f/%.2f "
                 "pk=%.3f/%.3f turns=%" PRIu32 " mic=%" PRIu32 " rx=%" PRIu32
                 " played=%" PRIu32 " drop=%" PRIu32
                 " heap=%" PRIu32 " int=%u intmax=%u",
                 (double)esp_timer_get_time() / 1000000.0,
                 t.face, t.behaviour, t.source,
                 !session_ctl_is_running() ? "stopped"
                     : dg_agent_is_ready() ? "ready" : "notready",
                 t.frames, t.fps, t.draw_avg_ms, t.draw_max_ms,
                 t.amp_avg, t.amp_max, t.low_avg, t.low_max,
                 t.mid_avg, t.mid_max, t.high_avg, t.high_max,
                 t.peak_mic, t.peak_agent,
                 s_turns, captured, s_audio_bytes, played, dropped,
                 esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
}
