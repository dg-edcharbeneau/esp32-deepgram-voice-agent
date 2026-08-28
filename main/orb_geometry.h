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
    ORB_THINKING = 3,
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

    /*
     * ALSO NOT FROM THE REFERENCE. The speaking fill run backwards: rings
     * illuminate INWARD from both poles to a distance set by the microphone
     * level, the two fronts meeting at the equator when someone is loud.
     *
     * The direction is the whole point of it existing separately. The voice
     * shell's own vocabulary had the user's speech travelling INWARD and the
     * agent's OUTWARD, and that grammar is worth keeping: the same object, filling
     * one way while listening and the other while speaking, rather than two
     * unrelated animations.
     *
     * Shares the speaking fill's look -- fade, cap width, brightnesses -- and
     * differs only in direction and in how the level is mapped, because the
     * microphone's range is nothing like playback's.
     */
    ORB_LISTENING_FILL = 9,

    ORB_BEHAVIOUR_COUNT = 8,
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
#define ORB_BRAID_DOTS 306  /* ghostN 150 + 3 strands * strandN 52 */
#define ORB_WEB_DOTS 35     /* nodeN 30 + signals 5 */

#define ORB_MAX2(a, b) ((a) > (b) ? (a) : (b))

/*
 * Capacity is sized for TWO modes at once, not one.
 *
 * A cross-mode transition draws the outgoing and incoming animations together and
 * fades between them on alpha -- two different lattices cannot be interpolated
 * ring by ring the way two shell behaviours can, so the frame carries both dot
 * lists concatenated. The largest pair is the shell against wave or rubik.
 *
 * The cost is 9 kB of PSRAM, which is not the resource under pressure here --
 * internal RAM is, and none of this touches it.
 */
#define ORB_ONE_MODE_DOTS                                   \
    ORB_MAX2(ORB_VOICE_DOTS,                                \
             ORB_MAX2(ORB_WAVE_DOTS, ORB_BRAID_DOTS))
#define ORB_SECOND_MODE_DOTS ORB_WAVE_DOTS /* the next largest after the shell */
#define ORB_MAX_DOTS (ORB_ONE_MODE_DOTS + ORB_SECOND_MODE_DOTS)

/*
 * Marks fainter than this are dropped rather than drawn.
 *
 * Part of the frame's contract rather than an implementation detail: it is the
 * threshold the reference's finalizeFrame uses, so it is what the parity harness
 * compares against -- and anything composing frames has to cull by the same rule
 * or it changes the dot count.
 */
#define ORB_ALPHA_CULL 0.02f

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
 * Build one frame of `rubik` -- the playground's `solving` orb, ported from
 * lattice.ts buildRubik. Shares wave's lattice and dot count; everything else,
 * including the shell radius and the ink constants, is its own.
 */
void orb_build_rubik(orb_frame_t *out, float t);


/*
 * Build one frame of `braid` -- the playground's `weaving` orb, ported from
 * braid.ts frameBraid.
 *
 * Three strands plaiting pole to pole over a Fibonacci ghost shell. The
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
 * Turn a finished frame about the canvas centre, degrees clockwise on screen.
 *
 * This is a true rotation of the object about the VIEW axis, not a cheat: z is
 * untouched, so depth order stays correct, and radii and the cull threshold are
 * unaffected. The dot lattice turns with the light, which is what rotating the
 * orb means -- a 90 puts the poles left and right and the rings read vertical.
 *
 * A LOCAL POST-PASS with no upstream, so host/run.sh cannot
 * check it. Keeping it out of the build calls is what leaves those transcriptions
 * diffable. Zero returns immediately, so an unrotated behaviour pays nothing.
 */
void orb_rotate(orb_frame_t *f, float degrees);

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
 * `amp` is 0..1 and scales how DEEP a gesture goes, never how fast. Four
 * behaviours read it and the rest ignore it: LISTENING and SPEAKING deepen a
 * gesture with it, while the two fills are driven ENTIRELY by it -- at zero they
 * are the dim disconnected shell and nothing else.
 *
 * WHICH LEVEL it is depends on the behaviour, and they are not interchangeable:
 * the speaking side wants playback, the listening side wants the microphone. See
 * LISTEN_FLOOR for what the microphone's range costs.
 */
void orb_build(orb_frame_t *out, float t, orb_behaviour_t from,
               orb_behaviour_t to, float mix, float amp,
               const orb_bands_t *bands);
