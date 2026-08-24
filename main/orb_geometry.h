/*
 * Geometry for the dotted voice orb: one lat/long shell, eight behaviours.
 *
 * A C port of the voice shell from expo-thinking-orbs, which is itself built on
 * thinking-orbs' shared primitives. Both MIT -- see the notice in
 * orb_geometry.c.
 *
 * Pure math. No LVGL, no ESP-IDF, no allocation: this file and its .c compile
 * unchanged on the host, which is how the port is verified against the
 * reference's own golden vectors instead of by looking at the screen.
 *
 * The output is a finished draw list -- every value final, already sorted into
 * draw order. A renderer draws it and derives nothing.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Behaviour indices. Numeric and contiguous because two of them are BLENDED
 * arithmetically during a transition, so they have to be interpolable.
 *
 * The order is the reference's, which is also a deliberate ladder: every
 * connected state is visibly fuller and brighter than the one before it, so a
 * session's progress reads without a label.
 */
typedef enum {
    ORB_IDLE = 0,
    ORB_INITIALIZING = 1,
    ORB_LISTENING = 2,
    ORB_THINKING = 3,
    ORB_SPEAKING = 4,
    ORB_CONNECTING = 5,
    ORB_BUFFERING = 6,
    ORB_DISCONNECTED = 7,

    /*
     * NOT FROM THE REFERENCE -- the only behaviour here that is not.
     *
     * The dim disconnected shell used as a level meter: rings illuminate outward
     * from the equator to a distance set by the level of the audio playing, with a
     * brighter cap at the leading edge, and retract as it falls. A spectrum
     * analyser bar wrapped onto a sphere.
     *
     * It has no upstream, so host/run.sh CANNOT check it -- there is nothing to
     * diff against. It is verified by dumping alpha per ring across level, which
     * is what caught the two aliasing faults its tuning has already had.
     */
    ORB_SPEAKING_FILL = 8,

    ORB_BEHAVIOUR_COUNT = 9,
} orb_behaviour_t;

/*
 * Per-mode dot counts, and the capacity that has to cover all of them.
 *
 * A MAX RATHER THAN A LITERAL on purpose. This sizes orb_frame_t, the
 * rasteriser's dirty list and the lattice tables, and orb_raster_draw() silently
 * TRUNCATES anything past it -- so a cap that drifts below a mode's real count
 * drops dots and reads as missing rows in the harness rather than as an error.
 * Reduce a mode's tuning if a frame is too dear; never reduce the cap.
 */
#define ORB_VOICE_DOTS 456  /* 18 rings of cosine-tapered longitude counts */
#define ORB_WAVE_DOTS 384   /* rings 15 / lonDensity 40; rubik shares it */
#define ORB_RIBBON_DOTS 370 /* ghostN 90 + lanes 5 * segs 56, tuned down */
#define ORB_BRAID_DOTS 306  /* ghostN 150 + 3 strands * strandN 52 */
#define ORB_WEB_DOTS 35     /* nodeN 30 + signals 5 */

#define ORB_MAX2(a, b) ((a) > (b) ? (a) : (b))
#define ORB_MAX_DOTS                                        \
    ORB_MAX2(ORB_VOICE_DOTS,                                \
             ORB_MAX2(ORB_WAVE_DOTS,                        \
                      ORB_MAX2(ORB_RIBBON_DOTS, ORB_BRAID_DOTS)))

typedef struct {
    float x, y;  /* screen pixels */
    float z;     /* depth; the list is sorted ascending on this */
    float r;     /* radius in pixels, already clamped to the floor */
    float white; /* ink, 0..1. On a dark ground the grey is (1 - white) * 255 */
    float a;     /* alpha, 0..1 */
} orb_dot_t;

/*
 * A stroked edge. Only `web` emits these; every other mode leaves `line_count`
 * at zero, and a renderer that ignores lines entirely still draws those modes
 * correctly.
 */
typedef struct {
    float x1, y1, x2, y2; /* screen pixels */
    float white;          /* ink, same convention as orb_dot_t */
    float a;              /* alpha, 0..1 */
    float w;              /* stroke width in pixels */
} orb_line_t;

/*
 * web pairs 30 nodes and keeps those closer than the threshold, so the bound is
 * every pair -- 30*29/2. Typically far fewer survive; the cap is for the frame
 * buffer, not a prediction.
 */
#define ORB_MAX_LINES 435

typedef struct {
    orb_dot_t dots[ORB_MAX_DOTS];
    size_t count;
    orb_line_t lines[ORB_MAX_LINES];
    size_t line_count;
} orb_frame_t;

/*
 * Build the lattice and the radius scale for a square frame of `size` pixels.
 * Call once before orb_build(). Returns false only if the per-dot tables could
 * not be allocated.
 */
bool orb_init(float size);

/*
 * Band-split energy driving the voice pass, each 0..1. All zero is a no-op, so a
 * caller that has no audio gets exactly the pose the behaviour built.
 */
typedef struct {
    float low;  /* bulk radial swell   -- "someone is talking"     */
    float mid;  /* travelling ripple   -- "following the words"    */
    float high; /* ink only, no motion -- sibilance as brightness  */
} orb_bands_t;

/*
 * Build one frame of `wave` -- the playground's `listening` orb, ported from
 * lattice.ts buildWave.
 *
 * No behaviour, no blend, no amplitude: a wave is a single animation that does
 * the same thing forever. 384 dots on its own lattice, not the shell's 456.
 *
 * `amp` is the MICROPHONE level -- LISTENING is the user talking -- and scales
 * every radius through the reference's dyn.rMul. See WAVE_RMUL_GAIN for why that
 * is the only hook available and what it costs in expressiveness.
 */
void orb_build_wave(orb_frame_t *out, float t, float amp);

/*
 * Build one frame of `rubik` -- the playground's `solving` orb, ported from
 * lattice.ts buildRubik. Shares wave's lattice and dot count; everything else,
 * including the shell radius and the ink constants, is its own.
 */
void orb_build_rubik(orb_frame_t *out, float t);

/*
 * Build one frame of `ribbon` -- the playground's `composing` orb, ported from
 * ribbon.ts buildRibbon.
 *
 * The largest mode at 590 dots: a 150-dot Fibonacci ghost shell plus a five-lane
 * band of 88 segments that precesses and undulates. Unlike wave and rubik it
 * varies alpha per dot, so its ghosts read as a haze behind the band.
 *
 * `amp` is 0..1 and scales how DEEP the undulation goes, never how fast -- the
 * band's tempo is fixed. Tuned below the reference's dot counts for frame budget;
 * see RIBBON_GHOSTS in orb_geometry.c.
 */
void orb_build_ribbon(orb_frame_t *out, float t, float amp);

/*
 * Build one frame of `braid` -- the playground's `weaving` orb, ported from
 * braid.ts frameBraid.
 *
 * Three strands plaiting pole to pole over the same ghost shell ribbon uses. The
 * only mode so far that CULLS: a strand fades out at the poles, so the dot count
 * varies from frame to frame.
 */
void orb_build_braid(orb_frame_t *out, float t);

/*
 * Build one frame of `web` -- the playground's `connecting` orb, ported from
 * web.ts frameWeb.
 *
 * THE ONLY MODE THAT EMITS LINES. Thirty nodes drift on the sphere under slow
 * value noise, every pair closer than the threshold grows an edge, and bright
 * packets run along re-picked pairs. Sets both `count` and `line_count`; a
 * renderer with no line support draws the nodes and none of the wiring, which is
 * most of the point of it missing.
 */
void orb_build_web(orb_frame_t *out, float t);

/*
 * Evaluate one frame.
 *
 * `t` is seconds; everything here is a pure function of it, so a dropped frame
 * costs nothing and there is no state to resynchronise.
 *
 * `from`/`to`/`mix` are a transition: mix 0 is entirely `from`, 1 entirely
 * `to`. Pass from == to (any mix) for a steady state, which skips the second
 * evaluation.
 *
 * `amp` is 0..1 and scales how DEEP a gesture goes, never how fast. It is read
 * only by LISTENING and SPEAKING; every other behaviour ignores it.
 */
void orb_build(orb_frame_t *out, float t, orb_behaviour_t from,
               orb_behaviour_t to, float mix, float amp,
               const orb_bands_t *bands);
