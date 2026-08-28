/*
 * The 466x466 AMOLED display, driven by the agent session.
 *
 * This module owns the panel, the touch panel, the status label, the QR
 * overlay, the frame timer and the PSRAM canvas everything is drawn into. What
 * appears inside that canvas is a *face* -- see ui_face.h. Faces are
 * interchangeable at runtime; the default is chosen at build time.
 *
 * Audio comes from audio_io's taps rather than from I2S directly, because
 * audio_io already owns the codec: agent audio as it reaches the speaker, mic
 * audio as it goes upstream, so the display shows whichever half of the
 * conversation is live.
 *
 * THREADING
 *
 * The feed functions run on the audio tasks. Everything else -- analysis,
 * layout, drawing -- happens inside an LVGL timer on the LVGL task, which is
 * pinned to core 1 below the audio tasks so a slow frame costs frames, never
 * audio. Nothing outside this module may call lv_*: the setters here store a
 * value and the frame timer applies it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Brings up the panel and the touch panel, registers both with LVGL, and starts
 * the frame timer. Blocks ~1.2 s in the CO5300 reset sequence. */
esp_err_t ui_start(void);

typedef enum {
    /*
     * A short press on the centre button. ONE gesture with two meanings, and the
     * handler picks between them: it interrupts if the agent is speaking, and
     * otherwise toggles the session.
     *
     * The two used to be separate targets -- centre to toggle, ring to interrupt
     * -- and the ring was the whole screen minus a 70 px circle, so it collected
     * every brush of the bezel. docs/notes/echo-cancellation.md records the same complaint from
     * the other side: "UI_INTERRUPT fired on any short click outside the centre
     * button, so stray touches triggered it."
     *
     * Collapsing them onto the button is what makes the overlap safe. There is no
     * moment where a tap could plausibly do both, because there is only one
     * branch, in main.c, and it is an if/else -- see on_gesture() there for which
     * way it goes and why the interrupt arm is not routed through session_ctl.
     */
    UI_TAP,
    UI_HOLD, /* long press on the centre button: force restart */

    /*
     * The display test reached its last step and has already put the previous
     * face back. Emitted rather than acted on here so ui.c stays free of any
     * session header, exactly as tap and hold do.
     */
    UI_TEST_DONE,
} ui_gesture_t;

/*
 * What the session is doing, as the display understands it.
 *
 * The order is a deliberate ladder -- every connected state is visibly fuller and
 * brighter than the one before -- so a session's progress reads without anyone
 * having to read the label.
 *
 * ui.c infers the conversational states for itself from the audio path, which is
 * tied to what the speaker and microphone are actually doing. The connection
 * phases are the ones it cannot see and has to be told.
 *
 * Nothing indexes an array by these or puts them on the wire, so the order is
 * free to change; it mirrors orb_behaviour_t's so the two read the same way.
 */
typedef enum {
    UI_BEHAVIOUR_IDLE = 0,
    UI_BEHAVIOUR_INITIALIZING,
    UI_BEHAVIOUR_LISTENING,

    /*
     * resolve_behaviour() NEVER RETURNS THIS, and that is not an oversight.
     * AgentThinking has not arrived in any logged session, so there is nothing to
     * infer it from. It exists so the display test can render the shell's
     * thinking pose -- a faithful, parity-verified port that would otherwise be
     * code nobody has ever seen run -- and so that the day the message does show
     * up, wiring it is one line in resolve_behaviour() and nothing else.
     */
    UI_BEHAVIOUR_THINKING,

    UI_BEHAVIOUR_SPEAKING,
    UI_BEHAVIOUR_CONNECTING,
    UI_BEHAVIOUR_BUFFERING,
    UI_BEHAVIOUR_DISCONNECTED,
} ui_behaviour_t;

/*
 * Report a connection phase, which the display cannot work out for itself. Safe
 * from any task.
 *
 * The audio path outranks anything reported here once a session is live: playback
 * and a live mic describe what the hardware is doing, while this only describes
 * what the session layer last said.
 */
void ui_set_behaviour(ui_behaviour_t behaviour);

/*
 * Screen gestures. The handler runs on the LVGL task with the LVGL lock held,
 * so it must not block -- signal another task and return.
 */
void ui_set_gesture_handler(void (*handler)(ui_gesture_t gesture));

/*
 * Freeze the idle animation.
 *
 * Distinct from ui_set_status(..., false): the session is also "not live" while
 * connecting, where the display should keep breathing. This is only for a
 * device that has been deliberately stopped.
 */
/*
 * Ask the display to sleep or wake. Safe from any task -- the request is applied
 * by the frame timer on the LVGL task, because changing the brightness is a panel
 * command over the same bus as the flush.
 *
 * Asleep the panel dims and the frame rate drops, but LVGL keeps running, so a
 * touch is answered as quickly as it is when awake -- and a touch wakes it on the
 * press, without the caller doing anything. Refused while the display test is
 * running, which can outlast the sleep timer.
 */
void ui_set_sleep(bool sleep);

/* Whether the display is currently in that state. */
bool ui_is_asleep(void);

void ui_set_stopped(bool stopped);

/* audio_io_tap_t-compatible. Agent audio wins when both are active. */
void ui_feed_agent(const int16_t *mono, size_t samples);
void ui_feed_mic(const int16_t *mono, size_t samples);

/*
 * Session state for the middle of the screen. Safe from any task: only the
 * pointer is stored, so `text` must have static lifetime -- pass a literal.
 *
 * `session_live` says whether the session is up. When it is, the label shows
 * what the audio is doing ("speaking" / "listening") and falls back to `text`
 * only when both directions are quiet; when it is not, `text` is shown as-is.
 */
void ui_set_status(const char *text, bool session_live);

/*
 * Overlay a QR code in the middle of the screen, or clear it.
 *
 * Used by provisioning to put the setup network on screen as something a phone
 * camera can act on, instead of a name to be retyped.
 *
 * Same contract as ui_set_status(): safe from any task, only the pointer is
 * kept, so `payload` must have static lifetime. The widget itself is created
 * and destroyed by the frame timer on the LVGL task -- callers must never touch
 * lv_* themselves.
 */
void ui_show_qr(const char *payload);

/*
 * Switch the visual. `index` is a faces.h catalog index; out-of-range is ignored.
 *
 * Same contract as the setters above: safe from any task, because it only stores
 * the index. The frame timer brings the face up on the LVGL task, which is the
 * only place a face's init() and activate() may run.
 */
void ui_set_face(int index);

/*
 * Set the orb's ink colour by index into the orb_colors.c catalog.
 *
 * Same contract as ui_set_face: an index, not a colour, so this header stays
 * free of both LVGL and the catalog. Out-of-range is logged and ignored. Applied
 * on the next frame, not persisted across a reboot -- a local display setting,
 * like the face and unlike the voice.
 */
void ui_set_orb_color(int index);

/*
 * Deepgram detected the user starting to speak.
 *
 * This is what drives LISTENING. Measured against a local band-energy gate on the
 * same speech, this separates cleanly -- six events across two speech phases, none
 * across seventeen seconds of quiet -- where band energy managed at best a 5x
 * ratio and could not be thresholded without either missing speech or flickering
 * on room noise.
 *
 * Costs a dependency on the session and a network round trip, and it marks only
 * the START of an utterance, so the display holds LISTENING for a while after.
 */
void ui_note_user_speech(void);

/*
 * Hand the screen over to the display test: every orb state and modifier, one per
 * tap, and nothing else on screen.
 *
 * Taps stop reaching the gesture handler for the duration -- they advance the
 * sequence instead -- so this is the only mode in which a tap does not toggle the
 * session. Forces the orb, since the sequence means nothing on the spectrum, and
 * puts the previous face back at the end.
 *
 * The CALLER owns the session and the microphone. This function draws; it does
 * not stop the session and does not start one at the end. It emits UI_TEST_DONE
 * and leaves that to whoever owns it.
 */
void ui_start_display_test(void);

/*
 * A window of accumulated measurements, for one telemetry line.
 *
 * WHY THE UI DOES NOT LOG THIS ITSELF
 *
 * ESP-IDF console writes block, and ~200 bytes at 115200 baud is about 17 ms --
 * most of a frame. Logging from the frame timer therefore distorts the very
 * timings it reports, which is the most likely explanation for the unexplained
 * 100 ms draw maxima seen early in this project. So the UI accumulates and the
 * caller (main's loop, on its own task) formats and logs.
 *
 * Every field that can spike carries an avg AND a max, because speech and frame
 * timing are both mostly-quiet with occasional peaks, and a periodic spot sample
 * misses exactly the peaks that matter.
 */
typedef struct {
    uint32_t frames;      /* samples in this window; 0 means nothing to report */

    float fps;            /* derived from the mean frame period */
    float draw_avg_ms, draw_max_ms;

    float amp_avg, amp_max;
    float low_avg, low_max;
    float mid_avg, mid_max;
    float high_avg, high_max;

    /* Lifetime high-water marks, per source. Never reset -- these are what let
     * someone walk up, talk, and read the peak off afterwards. */
    float peak_mic, peak_agent;

    const char *face;     /* static string */
    const char *behaviour;/* static string */
    const char *source;   /* static string */

    /*
     * Set when a face changed during this window, so the caller can log the
     * before/after pair adjacently instead of up to a whole window apart. The UI
     * cannot log it itself -- that blocks the LVGL task on the console.
     */
    bool face_changed;
} ui_telemetry_t;

/*
 * Snapshot the accumulated window and reset it. Safe from any task.
 *
 * `out->frames == 0` means no frame ran in the window, which is itself worth
 * reporting rather than papering over.
 */
void ui_get_telemetry(ui_telemetry_t *out);

/*
 * The session failed, as opposed to being stopped deliberately.
 *
 * Freezes the animation. A stopped device is still waiting and keeps moving
 * faintly; a failed one has stopped trying, and holding still says that without
 * needing to be read.
 */
void ui_set_failed(bool failed);
