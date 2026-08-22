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
 * Configure Wi-Fi credentials and the Deepgram API key with `idf.py menuconfig`
 * under "Deepgram Agent Device". They land in sdkconfig, which is gitignored.
 */

#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "audio_io.h"
#include "dg_agent.h"
#include "session_ctl.h"
#include "spectrum_ui.h"
#include "voices.h"
#include "wifi_sta.h"

static const char *TAG = "main";

#define WIFI_CONNECT_TIMEOUT_MS 30000

/* Written by the WebSocket task's callback, read by app_main's status loop. A
 * torn read of a counter only misreports one line of logging. */
static volatile uint32_t s_audio_bytes;
static volatile uint32_t s_turns;

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
    spectrum_ui_set_status(state_name(state), state == DG_AGENT_READY);
}

static void on_conversation_text(const char *role, const char *content, void *ctx)
{
    s_turns++;
}

static void on_audio(const uint8_t *data, size_t len, void *ctx)
{
    s_audio_bytes += len;
    /* Non-blocking by contract -- this is the WebSocket task. */
    audio_io_play(data, len);
}

static void on_user_started_speaking(void *ctx)
{
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
    ESP_LOGI(TAG, "turn complete, %" PRIu32 " audio bytes received", s_audio_bytes);
}

/* Runs on the LVGL task with the LVGL lock held: signal only, never block. */
static void on_gesture(spectrum_ui_gesture_t gesture)
{
    switch (gesture) {
    case SPECTRUM_UI_TAP:
        session_ctl_request_toggle();
        break;
    case SPECTRUM_UI_HOLD:
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

    /* After nvs_flash_init(), before the first Settings message is built. */
    voices_init();

    /* Before the session, so the greeting has somewhere to go the moment it
     * arrives. */
    ESP_ERROR_CHECK(audio_io_init(DG_AUDIO_SAMPLE_RATE));

    /* Non-blocking, and bringing the panel up costs ~1.2 s in the CO5300 reset
     * sequence -- so start associating first and overlap the two. */
    ESP_ERROR_CHECK(wifi_sta_start());

    spectrum_ui_set_status("connecting", false);
    if (spectrum_ui_start() == ESP_OK) {
        audio_io_set_playback_tap(spectrum_ui_feed_agent);
        audio_io_set_capture_tap(spectrum_ui_feed_mic);
        spectrum_ui_set_gesture_handler(on_gesture);
    } else {
        /* A dark screen is not a reason to give up the voice loop. */
        ESP_LOGW(TAG, "spectrum display unavailable, continuing headless");
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
         * Not fatal any more. Returning here would leave a device with a live
         * screen and no way to retry; the status loop and the touch handler stay
         * up instead, so a long press can start a session once the network is
         * back.
         */
        ESP_LOGE(TAG, "no network (%s) -- check SSID/password in menuconfig",
                 esp_err_to_name(err));
        spectrum_ui_set_stopped(true);
        spectrum_ui_set_status("no network", false);
    } else {
        session_ctl_request_start();
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        uint32_t played, dropped, captured;
        audio_io_stats(&played, &dropped, &captured);
        /*
         * Internal RAM is the scarce resource once the display is up, and the
         * case that bites is a WebSocket reconnect: the TLS handshake wants a
         * burst of it with the render buffer already allocated. Largest-block
         * matters more than the total -- if it sags towards 40 kB, shrink
         * DRAW_ROWS in spectrum_ui.c before tuning anything else.
         */
        ESP_LOGI(TAG, "%s | turns=%" PRIu32 " mic=%" PRIu32 " B rx=%" PRIu32
                      " B played=%" PRIu32 " B dropped=%" PRIu32
                      " B | heap=%" PRIu32 " B internal=%u B (largest %u B)",
                 !session_ctl_is_running() ? "stopped"
                     : dg_agent_is_ready() ? "ready" : "not ready",
                 s_turns, captured, s_audio_bytes, played, dropped,
                 esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
}
