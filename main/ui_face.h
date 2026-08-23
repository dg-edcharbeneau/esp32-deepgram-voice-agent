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
 * MEMORY -- READ THIS BEFORE ADDING A FACE
 *
 * Internal RAM is the resource that binds, not PSRAM: 288 kB shared with Wi-Fi,
 * lwIP and TLS, and running short does not fail the boot, it fails a WebSocket
 * write or a reconnect's TLS handshake mid-session.
 *
 * The catch is that a face's two kinds of memory scale differently. Heap is
 * lazy -- init() runs on first activation, so a face nobody selects costs no
 * heap. Statics are not: every face is linked in unconditionally, so its .bss is
 * resident from boot whether or not it is ever shown. N faces means N faces'
 * worth of statics on a device displaying one. So:
 *
 *   - Keep static data to a few hundred bytes. Tables, buffers and scratch go on
 *     the heap in init(), not in .bss. The spectrum face was 6,984 B of internal
 *     .bss on a device that boots to the orb before its buffers were moved out;
 *     it is 848 B now.
 *
 *   - Ask for PSRAM explicitly: heap_caps_malloc(..., MALLOC_CAP_SPIRAM). A
 *     plain malloc under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (4096 B) is handed
 *     internal memory silently, which is the whole trap. When a face needs
 *     several small tables, pool them into one allocation that clears the
 *     threshold -- orb_geometry.c does exactly this and says why.
 *
 *   - Watch contiguity, not just totals. Per-frame LVGL draw allocations of
 *     varying sizes fragment the internal arena, and a TLS handshake wants one
 *     large contiguous block. STRIPE_COUNT in face_spectrum.c is halved for this
 *     reason alone, with the measured largest-free-block numbers recorded there.
 *
 *   - There is no deinit. A face selected once holds its heap for the rest of
 *     the boot, so budget PSRAM as the sum over every face a session might
 *     visit, not as the peak of one.
 *
 * main.c's TLM line reports int/intmax/iblocks/ialloc every window and is
 * stamped on a face change, which is how all of the above gets checked rather
 * than estimated.
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

    /*
     * The ink colour the user asked for, 0xRRGGBB. 0xFFFFFF is the default and
     * the orb rasteriser's exact identity.
     *
     * A face may ignore it, and the spectrum does: it colours its bands by
     * frequency and by which half of the conversation is live, so one tint would
     * destroy information rather than restyle it.
     */
    uint32_t tint_rgb;

    /*
     * Which ported reference mode to draw instead of the voice shell, or
     * UI_ORB_MODE_NONE for the shell.
     *
     * Only the display test sets this. The modes are dormant in normal operation
     * because a shell behaviour and a foreign mode share no lattice to blend
     * across -- but the test cuts between steps rather than crossfading, so it can
     * show them without that question being settled.
     */
    int orb_mode;
} ui_render_ctx_t;

/*
 * Which ported reference mode the display test is showing.
 *
 * Deliberately NOT orb_behaviour_t values and not in orb_geometry.h: these name
 * whole animations from thinking-orbs' registry, siblings of the voice shell
 * rather than states within it. ui.c must be able to script them without
 * including an orb header, which is why they live here with the face vtable.
 */
#define UI_ORB_MODE_NONE   (-1) /* the voice shell */
#define UI_ORB_MODE_WAVE   0
#define UI_ORB_MODE_RUBIK  1
#define UI_ORB_MODE_RIBBON 2
#define UI_ORB_MODE_BRAID  3
#define UI_ORB_MODE_WEB    4

typedef struct {
    /* Also the enum value the agent's set_face function uses, so it has to be
     * something a person would say out loud. */
    const char *name;

    /* Allocate, and latch whatever the face needs from the canvas. Called once,
     * on first activation, so a face nobody selects pays no HEAP -- but its
     * statics are resident from boot regardless. See the MEMORY note above:
     * this is the hook that makes a face free when it is not showing, and only
     * what is allocated here rather than declared static gets that benefit. */
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
