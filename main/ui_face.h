/*
 * The face interface: one drawable visual on the round panel.
 *
 * ui.c owns everything that was expensive to get right and is the same whatever
 * is on screen -- the hand-registered display and its internal-RAM render
 * buffer, the touch panel, the status label, the QR overlay, the frame timer,
 * and the PSRAM canvas. A face owns only the pixels inside that canvas.
 *
 * THREADING
 *
 * feed_pcm() runs on the audio tasks. init(), activate() and render() run on
 * the LVGL task inside the frame timer, with the LVGL lock held. A face that
 * wants raw audio therefore owns the handoff between the two -- which is why
 * feed_pcm hands over samples rather than ui.c publishing a window in some
 * size it would have to guess.
 *
 * Only ui.c and the face implementations include this header. The rest of the
 * firmware uses ui.h, which is deliberately free of LVGL.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#include "ui.h"

/* Which half of the conversation last produced audio. */
typedef enum {
    UI_SRC_NONE = 0,
    UI_SRC_AGENT,
    UI_SRC_MIC,
} ui_source_t;

/*
 * Everything a face is allowed to know about the frame it is drawing.
 *
 * Passed by value-ish (a const pointer to ui.c's own stack) so a face cannot
 * hold onto it, and so adding a field never breaks an existing face.
 */
typedef struct {
    lv_obj_t *canvas;   /* full-screen RGB565 canvas in PSRAM; draw here */
    uint32_t frame;     /* monotonic, one per frame timer tick */
    int64_t now_us;     /* esp_timer_get_time() sampled once for the frame */

    /* No audio has arrived for longer than the idle threshold. A face must
     * animate its own way down to rest rather than holding the last value:
     * between turns the taps simply stop being called. */
    bool idle;
    ui_source_t source;

    bool stopped;       /* deliberately stopped, not merely between turns */

    /*
     * The session failed rather than merely ended. A face should hold still.
     *
     * The distinction is worth drawing: a stopped session is still straining for
     * a signal and keeps its faint periodic ping, because it could come back. A
     * failed one has given up, and a motionless mark says so more plainly than
     * any label.
     */
    bool frozen;
    bool press_active;  /* a finger is on the button right now */

    /* What the session is doing, resolved by ui.c. A face may ignore it -- the
     * spectrum does -- but the orb's whole vocabulary is built on it. */
    ui_behaviour_t behaviour;

    /*
     * Smoothed level of whichever direction is live, 0..1. Fast attack, slow
     * release, and frame-rate independent -- see update_amp() in ui.c.
     *
     * This scales how DEEP a gesture goes, never how fast. Driving a rate from
     * amplitude is frequency modulation: it reads as vibration rather than as a
     * voice. Faces must keep their own tempo constant.
     */
    float amp;

    /*
     * Band-split energy of whatever is live, each 0..1 and smoothed like `amp`.
     * All three are zero when the noise gate is shut, so a face's audio response
     * switches itself off between turns rather than animating room noise.
     *
     * Three bands rather than one number because they do three different jobs:
     * low is bulk swell, mid is a travelling ripple, high is brightness with no
     * motion at all. That split is what makes a sharp consonant look different
     * from a loud vowel.
     */
    float band_low, band_mid, band_high;
} ui_render_ctx_t;

typedef struct {
    /* Also the enum value the agent's set_face function uses, so it has to be
     * something a person would say out loud. */
    const char *name;

    /* Allocate, and latch whatever the face needs from the canvas. Called once,
     * on first activation, so a face nobody selects costs nothing. */
    esp_err_t (*init)(lv_obj_t *canvas);

    /* Called on every switch to this face, after init(). Optional: use it to
     * clear state that would otherwise animate in from whatever was on screen
     * the last time this face was up. */
    void (*activate)(void);

    /* Raw mono 16 kHz PCM, on the audio task, block-at-a-time. NULL when the
     * face does not need samples -- which lets ui.c skip the whole handoff. */
    void (*feed_pcm)(const int16_t *mono, size_t samples);

    /* Draw one frame. Called exactly once per frame: a canvas is used here
     * precisely so this cost is paid once instead of once per render chunk. */
    void (*render)(const ui_render_ctx_t *ctx);

} ui_face_t;

/* The faces, defined by their own translation units. */
extern const ui_face_t ui_face_spectrum;
extern const ui_face_t ui_face_orb;
