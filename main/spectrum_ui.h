/*
 * Radial spectrum analyzer on the 466x466 AMOLED, driven by the agent session.
 *
 * Ported from the sibling spec_analyzer_radial project, which fans FFT bands
 * around the round panel. The difference here is where the audio comes from:
 * that project reads the microphone over I2S itself, which is impossible in
 * this one because the codec is already owned by audio_io. Instead the ring is
 * fed by audio_io's taps -- agent audio as it reaches the speaker, mic audio as
 * it goes upstream -- so it shows whichever half of the conversation is live.
 *
 * THREADING
 *
 * The feed functions run on the audio tasks. Everything else -- FFT, layout,
 * drawing -- happens inside an LVGL timer on the LVGL task, which is pinned to
 * core 1 below the audio tasks so a slow frame costs frames, never audio.
 * Nothing outside this module may call lv_*: spectrum_ui_set_status() stores a
 * string and the timer applies it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Brings up the panel and the touch panel, registers both with LVGL, and starts
 * the frame timer. Blocks ~1.2 s in the CO5300 reset sequence. */
esp_err_t spectrum_ui_start(void);

typedef enum {
    SPECTRUM_UI_TAP,  /* short press: toggle */
    SPECTRUM_UI_HOLD, /* long press: force restart */
} spectrum_ui_gesture_t;

/*
 * Screen gestures. The handler runs on the LVGL task with the LVGL lock held,
 * so it must not block -- signal another task and return.
 */
void spectrum_ui_set_gesture_handler(void (*handler)(spectrum_ui_gesture_t gesture));

/*
 * Freeze the idle animation.
 *
 * Distinct from spectrum_ui_set_status(..., false): the session is also "not
 * live" while connecting, where the ring should keep breathing. This is only for
 * a device that has been deliberately stopped, and dims the ring to a constant.
 */
void spectrum_ui_set_stopped(bool stopped);

/* audio_io_tap_t-compatible. Agent audio wins when both are active. */
void spectrum_ui_feed_agent(const int16_t *mono, size_t samples);
void spectrum_ui_feed_mic(const int16_t *mono, size_t samples);

/*
 * Session state for the middle of the ring. Safe from any task: only the
 * pointer is stored, so `text` must have static lifetime -- pass a literal.
 *
 * `session_live` says whether the session is up. When it is, the ring shows
 * what the audio is doing ("speaking" / "listening") and falls back to `text`
 * only when both directions are quiet; when it is not, `text` is shown as-is.
 */
void spectrum_ui_set_status(const char *text, bool session_live);
