/*
 * The voice shell. See orb_geometry.h for the contract.
 *
 * PROVENANCE
 *
 * Transcribed from two MIT-licensed projects:
 *
 *   thinking-orbs (c) Jakub Antalik -- https://github.com/Jakubantalik/thinking-orbs
 *     src/engine/core.ts: makeProj, hashD, radiusScale, frac, finalizeFrame.
 *
 *   expo-thinking-orbs (c) Mahdi Davoodi -- https://github.com/mahdidavoodi7/expo-thinking-orbs
 *     src/engine/voice.ts: precomputeVoice, ringState, buildVoice, and every
 *     tuning constant below.
 *
 * The formulae are transcribed rather than reinterpreted, deliberately: the
 * reference is heavily commented about WHY each number is what it is, and the
 * design rules below were arrived at by measurement there. Breaking them
 * produces something that looks almost right and reads as a swarm.
 *
 *   1. A REGULAR lattice, not a random cloud. You must see a surface being
 *      disturbed, not particles moving.
 *   2. Motion phase comes from LATTICE POSITION -- ring index and longitude --
 *      never a per-dot hash. A hash gives every dot an unrelated phase, which
 *      is mathematically noise and looks like it.
 *   3. Every behaviour is a SMOOTH PERIODIC function of t. Never a sawtooth: a
 *      recycling frac() phase snaps when it wraps.
 *   4. Radius and ink move TOGETHER off one crest quantity -- bigger and darker
 *      at once. That coupling is the whole 3-D illusion.
 *   5. Tempo is CONSTANT. Amplitude scales gesture DEPTH. Driving rate from
 *      level is frequency modulation and reads as vibration, not as a voice.
 *   6. Radius factors never exceed RF_CEILING, so the shell cannot clip.
 *
 * NOT YET PORTED (all additive polish, all scaled by idle_w below, which is
 * pinned to 0 until they land): idle's gesture layer (hive ripple, twist, sigh,
 * hop) and its body motion (float, breath, squash-and-stretch). Idle keeps its
 * base pose -- the three-sine breath and the differential twist -- which is
 * what carries its character.
 */

#include "orb_geometry.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TAU 6.28318530717958647692f

/* ---------------- lattice ---------------- */

/*
 * The loop below runs ri = 0..ORB_RINGS INCLUSIVE, so there are 18 ring bands,
 * and each takes max(1, round(|cos lat| * 42)) longitudes. The counts taper by
 * cosine -- 1 at each pole, 42 at the two equatorial rings -- and sum to 456.
 *
 * A flat 17x42 grid reaches the same total by coincidence while over-packing the
 * poles and deforming the silhouette. This is the single easiest thing to get
 * wrong in the port.
 */
#define ORB_RINGS 17
#define ORB_RING_COUNT (ORB_RINGS + 1)
#define ORB_LON_DENSITY 42

typedef struct {
    float sin_lat, cos_lat;
    int lon_count;
    int base; /* first dot index, into the flat arrays */
} orb_ring_t;

static orb_ring_t s_rings[ORB_RING_COUNT];
static int s_dot_count;

/*
 * WAVE'S OWN LATTICE. Its profile is rings 15 / lonDensity 40 against the voice
 * shell's 17 / 42, so it cannot share the tables above -- 384 dots, not 456.
 *
 * Worth a second lattice rather than a parameterised one: rubik's profile is
 * latRings 15 / lonDensity 40 too, so this serves both, and leaving the voice
 * shell's tables untouched keeps 1,000 lines of parity-verified code out of the
 * blast radius.
 */
#define WAVE_RINGS 15
#define WAVE_RING_COUNT (WAVE_RINGS + 1)
#define WAVE_LON_DENSITY 40

static orb_ring_t s_wave_rings[WAVE_RING_COUNT];
static int s_wave_dot_count;
static float *s_wave_cos_lon;
static float *s_wave_sin_lon;
/*
 * The same lattice as unit cartesian coordinates, which is what rubik's slab
 * test needs -- and it must be computed in DOUBLE, as buildLattice does.
 *
 * Not a micro-optimisation avoided: rubik decides slab membership with
 * `coord < lo || coord >= hi` where the bounds are exact multiples of 0.5, so
 * the SIGN of a coordinate near zero changes which slab a dot turns with.
 * cos(pi/2) is +6.1e-17 in double and -4.4e-8 in float -- opposite sides of the
 * 0.0 boundary. Deriving these per frame in float moved 26 of 384 dots.
 */
static double *s_wave_unit; /* ux,uy,uz interleaved: 3 doubles a dot */

/*
 * RIBBON'S TABLES. Nothing lattice-shaped about this mode: a Fibonacci-sphere
 * ghost shell for depth, and a band of `lanes` parallel tracks each of `segs`
 * segments, precessing as a plane and undulating along its length.
 */
/*
 * Tuned DOWN from the reference's 150/88. At the defaults ribbon cost ~30 ms a
 * frame -- 12.9 of geometry, 17.7 of raster -- and dropped the panel from 26 fps
 * to 19, which is the wrong trade for the state on screen most of a turn. 90/56
 * is 370 dots against 590, and 280 band dots against 440. The band dots are the
 * expensive ones: two sinf and a sqrtf each.
 *
 * Parity is unaffected. The harness passes the same numbers -- it needs the two
 * sides to AGREE, not to use defaults.
 */
#define RIBBON_GHOSTS 90
#define RIBBON_SEGS 56
#define RIBBON_LANES 5

/*
 * Braid keeps the reference's 150, on its OWN table though it shared ribbon's
 * until now: fibDir(i, n) depends on n, so a prefix of a 150-ghost table is not a
 * 90-ghost table. Shrinking ribbon's would have quietly restyled braid.
 */
#define BRAID_GHOSTS 150

static float *s_ghost_dx, *s_ghost_dy, *s_ghost_dz;
static float *s_bghost_dx, *s_bghost_dy, *s_bghost_dz;
static float *s_seg_a, *s_seg_cos, *s_seg_sin;
static float *s_lane_off, *s_lane_edge;

/*
 * The per-dot lattice tables, heap allocated as ONE block.
 *
 * Deliberately not static arrays. On the device those land in internal .bss, and
 * internal RAM is the binding resource here -- it is shared with Wi-Fi, lwIP and
 * TLS, and the symptom of running short is not a failed boot but a WebSocket
 * write failing mid-session. Three separate arrays of 1,824 B would each sit
 * below CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL and be handed internal memory
 * anyway; one 5,472 B block clears that threshold and goes to PSRAM.
 *
 * Plain malloc, not heap_caps_malloc, so this file still compiles on the host
 * for the parity harness.
 */
static float *s_lattice;
static float *s_cos_lon;
static float *s_sin_lon;
static float *s_scatter;

static float s_cx, s_cy; /* frame centre */
static float s_shell_r;  /* shell radius in pixels */
static float s_rs;       /* radius scale for this frame size */

/* ---------------- shared primitives (core.ts) ---------------- */

/*
 * Deterministic hash in [0, 1).
 *
 * DOUBLE, deliberately, and this is not optional. The construction is chaotic by
 * design: the argument to sin() reaches ~6200 for the last dot, where float32
 * has about 4e-4 of absolute precision, and multiplying by 43758 before taking
 * the fractional part amplifies that into a completely different number. In
 * float32 this function returns noise unrelated to the reference's values, which
 * silently breaks golden-vector verification while still looking plausible on
 * screen.
 *
 * It is called 456 times at init and never again, so the cost is irrelevant.
 */
static float hash_d(double a, double b)
{
    double h = sin(a * 12.9898 + b * 78.233) * 43758.5453;
    return (float)(h - floor(h));
}

/* ---------------- shaping (voice.ts) ---------------- */

/* Wave's undulation verbatim: two incommensurate sines phased by RING INDEX,
 * which is what makes a roll visible as a wave rather than as noise. */
static float wave_w(float t, int ri)
{
    return 0.62f * sinf(t * 2.1f - ri * 0.52f) +
           0.38f * sinf(t * 1.27f + ri * 0.83f);
}

/* A sine's positive lobe to the fifth: the ring rests most of the cycle and
 * lunges briefly. A spike rather than a wobble, and still perfectly smooth --
 * unlike a sawtooth it eases out of rest and back into it. */
static float spike_pulse(float x)
{
    float sv = sinf(x);
    if (sv <= 0.0f) {
        return 0.0f;
    }
    float s2 = sv * sv;
    return s2 * s2 * sv;
}

/* A sine's positive lobe cubed: broader, so a travelling wavefront reads as a
 * swell passing through rather than a hairline ring, and still exactly zero
 * between crests so the shell rests. */
static float ripple_pulse(float x)
{
    float sv = sinf(x);
    if (sv <= 0.0f) {
        return 0.0f;
    }
    return sv * sv * sv;
}

/* Idle's own wandering clock: bounded offset, strictly increasing, so the whole
 * pose breathes and turns at a tempo that drifts. */
static float idle_time(float t)
{
    return t + 1.6f * sinf(t * 0.211f) + 0.7f * sinf(t * 0.0873f + 2.1f);
}

/* ---------------- idle's gestures and body ---------------- */

/*
 * `idle` is where a live session sits between turns, and where a home screen
 * sits indefinitely -- so it is the state most likely to be watched long enough
 * to be found out. A single looping pose reads as a mechanism after about ten
 * seconds. On top of its quasi-periodic base pose it therefore carries two more
 * layers: a BODY that floats, breathes and deforms, and one of four GESTURES
 * that plays every so often.
 *
 * Which gesture and when are pure functions of the clock. The epoch length is
 * FIXED and the start time inside it is hashed: a jittered epoch length would
 * make floor(t / epoch) disagree with itself, where a hashed start keeps the
 * index exact and still puts an uneven gap between one gesture and the next --
 * which is the part that reads as unpredictable.
 */
#define GESTURE_EPOCH 9.0f
#define GESTURE_SPAN 3.2f
#define GESTURE_COUNT 4
#define GESTURE_RIPPLE 0
#define GESTURE_TWIST 1
#define GESTURE_SIGH 2
#define GESTURE_HOP 3

/* The float: two sines, so the drift never quite repeats. */
#define BODY_BOB_A1 0.015f
#define BODY_BOB_W1 0.31f
#define BODY_BOB_A2 0.007f
#define BODY_BOB_W2 0.1971f

/* Squash and stretch, driven by the float's ANALYTIC velocity -- differencing
 * frames would make the deformation depend on frame rate. */
#define BODY_WOBBLE 0.55f
#define BODY_WOBBLE_MAX 0.038f

/* The hop: a real bounce, stretching as it leaves and squashing as it lands. */
#define HOP_HEIGHT 0.026f
#define HOP_BOUNCES 1.5f
#define HOP_DEFORM 0.055f

/*
 * The hive ripple: one dot twitches and the disturbance travels out across the
 * SURFACE, dying as it spreads -- a jostle passing through a dense colony rather
 * than a concentric pulse on the screen. That distinction is why it cannot live
 * in ring_state: a front spreading from a point crosses each latitude ring at a
 * different longitude, so it is per-dot by nature.
 */
#define HIVE_WIDTH 0.34f
#define HIVE_INV_WIDTH (1.0f / HIVE_WIDTH)
#define HIVE_LIFT 0.028f
#define HIVE_CREST 0.5f
#define HIVE_BEE_D 0.06f
#define HIVE_BEE_LIFT 0.017f
#define HIVE_SQUASH 0.05f

/*
 * Which gesture is playing and how far into it: `which`, `env`, `local`.
 *
 * `env` is a raised cosine -- zero VALUE and zero SLOPE at both ends -- and
 * zero for the quiet remainder of the epoch. Everything a gesture does is
 * scaled by it, which is what makes epoch boundaries silent: a gesture can never
 * be cut off mid-movement, because it has already returned to rest before its
 * slot ends.
 */
static void idle_gesture(float t, int *which, float *env, float *local)
{
    float k = floorf(t / GESTURE_EPOCH);
    float in_epoch = t - k * GESTURE_EPOCH;
    /* Somewhere in the epoch's slack, so consecutive gestures are 6-12 clock
     * units apart rather than exactly 9. */
    float start = hash_d((double)k, 7.31) * (GESTURE_EPOCH - GESTURE_SPAN);
    float u = (in_epoch - start) / GESTURE_SPAN;

    *which = (int)(hash_d((double)k, 3.17) * (float)GESTURE_COUNT);
    if (u <= 0.0f || u >= 1.0f) {
        *env = 0.0f;
        *local = 0.0f;
        return;
    }
    *env = 0.5f - 0.5f * cosf(TAU * u);
    *local = u;
}

/* Wave's own resting radius factor -- the family's scale. */
#define WAVE_BASE 0.88f
/* Nothing may exceed this: wave's own maximum radius factor. */
#define RF_CEILING 0.985f
/* Wavefronts visible across the radius of the disc at once. Two-and-a-bit reads
 * as travelling; more looks like corduroy, fewer like a single throb. */
#define RIPPLE_K 2.2f
/* How fast a wavefront crosses from centre to rim, cycles per clock unit. */
#define RIPPLE_RATE 0.5f

#define ASSEMBLE_PERIOD 4.0f
#define BUFFER_PERIOD 2.2f
#define SIGNAL_PERIOD 5.0f

/*
 * The speaking level meter.
 *
 * FILL_REACH maps the audio level to how far out the light reaches, 0 at the
 * equator and 1 at either pole. Measured agent playback runs a mean near 0.35 and
 * peaks around 0.65, so 1.5 puts ordinary speech at about half the shell and a
 * peak at nearly all of it -- a meter with headroom rather than one that pegs.
 *
 * FILL_FADE is how abruptly the fill ends, in the same units. About one and a
 * half rings, enough that the boundary is not a hard band.
 *
 * LEAD_WIDTH IS SET BY THE RING SPACING, not by taste. Rings sit 0.118 apart in these
 * units, and a cap narrower than that falls BETWEEN rings for most fill levels --
 * at 0.12 the bright edge simply vanished at all but one level. 0.20 always
 * catches a ring. It is the same aliasing that made the first attempt at this
 * behaviour, a travelling crest, read as static stripes.
 *
 * The gate exists so silence is the disconnected shell exactly, rather than the
 * shell plus a permanent glow at the equator where the cap sits at fill zero.
 */
/* [1.0..2.0]   how far the light reaches. 1.0 never fills, 2.0 pegs at half
 *              volume. */
#define FILL_REACH 1.2f
/* [0.12..0.35] how gradually the body FADES OUT behind the leading edge, in
 *              from_eq units where one ring is 0.118. Under ~0.12 it ends in a
 *              hard band. */
#define FILL_FADE 0.18f
/* [0.20..0.35] width of the leading edge. 0.20 IS A FLOOR, not a preference:
 *              narrower than the 0.118 ring spacing and the edge falls between
 *              rings and vanishes entirely. */
#define LEAD_WIDTH 0.20f
/* [0..0.86]    brightness of the lit BODY trailing the edge, above the 0.14
 *              floor. LOWER THIS to make the leading edge stand out: the two
 *              brightnesses share 0.86 of headroom, so raising LEAD_BRIGHT alone
 *              just flattens both against the clamp. 0.55/0.40 gives the edge a
 *              1.13x contrast over the body; 0.18/0.68 gives it 2.39x. */
#define FILL_BRIGHT 0.25f
/* [0..~0.7]    brightness of the LEADING EDGE itself, on top of the body. The
 *              ceiling is not fixed -- it is whatever FILL_BRIGHT has left of the
 *              0.86 headroom at the boundary, so dimming the body raises it. At
 *              FILL_BRIGHT 0.25 this can reach about 0.7 before clamping; at 0.55
 *              only about 0.55. Under ~0.22 it stops reading as an edge at all. */
#define LEAD_BRIGHT 0.58f
/* [0..0.6]     how much lit dots swell and darken, together. Past ~0.6
 *              the near side blows out to solid ink. */
#define FILL_SWELL 0.40f
/* [0.03..0.15] silence cutoff. Higher means more has to be playing
 *              before anything lights at all. */
#define FILL_GATE 0.08f
/* [0.2..1.0]   the LISTENING fill's response curve: fill = level^CURVE * REACH.
 *              1.0 is linear, 0.5 a square root; LOWER compresses harder, lifting
 *              quiet speech without pushing loud speech off the top.
 *
 *              MEASURED, not chosen by feel. On device across beh=LISTENING a
 *              normal speaking voice runs amp 0.032 average / 0.087 peak and a
 *              raised one 0.144 / 0.227 -- a spread of only 4.5x, sitting just
 *              above a live-mic room noise of 0.014. That range is too narrow for
 *              a square root: any reach that fills normal speech also pins BOTH
 *              loud levels at 1.0, so shouting and talking look identical. 0.35
 *              puts normal speech at 0.42-0.68 and a loud peak at 0.99 -- the
 *              equator, reached but not clipped, so the top still has somewhere
 *              to go. Re-measure before changing it; a different mic or gain
 *              moves every number here. */
#define LISTEN_CURVE 0.35f
/* [1.0..2.5]   how far the fill reaches inward from the poles at full level.
 *              The plain loudness knob -- raise for more fill everywhere. Paired
 *              with LISTEN_CURVE and LISTEN_FLOOR: 1.6 is what puts a loud peak at
 *              the equator given those, so changing one wants the others checked. */
#define LISTEN_REACH 1.6f
/* [0..0.01]    mic level treated as silence, subtracted BEFORE the curve.
 *              DELIBERATELY BELOW ROOM NOISE, which is the opposite of what it
 *              looks like it should be. Setting it AT measured room noise (0.014)
 *              was a real bug on the device: speech is not a plateau, and the
 *              valleys between words drop straight through a floor sized for a
 *              quiet room. Captured mid-sentence with beh=LISTENING, amp ran
 *              0.010, 0.011 and 0.016 -- at or under 0.014 -- so the fill
 *              collapsed to nothing for a full second at a time while the state
 *              was still LISTENING and the orb sat bare.
 *
 *              Room noise does not need a floor here, because this behaviour is
 *              VAD-GATED: it is drawn only while Deepgram reports the user
 *              speaking. At rest the orb is in IDLE and this code never runs, so
 *              a quiet room never reaches it. Inside LISTENING even a quiet
 *              measurement IS the user, and deserves some fill rather than none.
 *              0.004 keeps true silence at zero and nothing else.
 *
 *              RAISE ONLY if silence itself lights the poles. Raising it to
 *              suppress a noisy room breaks mid-sentence instead. */
#define LISTEN_FLOOR 0.004f

/* Depth mapping. Radius and ink are both derived from it, which is rule 4. */
#define R_BASE 0.6f
#define R_DEPTH 1.7f
#define INK_FAR 0.66f
#define INK_SPAN 0.56f
#define RS_POW 0.6f

/* Dot radii were tuned for a 300 pt frame; sub-linear scaling keeps a small
 * shell legible. */
#define R_MIN 0.3f
#define ALPHA_CULL ORB_ALPHA_CULL /* one source of truth; see orb_geometry.h */

/*
 * The body breath: a scale that only ever draws IN, applied to every behaviour.
 *
 * Deliberately NOT gated on idle, unlike the rest of the body layer -- the
 * reference computes it unconditionally, so even LISTENING and SPEAKING breathe
 * slightly and the shell is never quite static. Scaling UP instead would push
 * the shell toward the frame edge for no gain: the eye reads the rhythm, not the
 * absolute size.
 *
 * It rides idle's wandering clock even outside idle, which is what keeps the
 * tempo of the breath consistent across a state change.
 */
#define BODY_BREATH 0.03f
#define BODY_BREATH_W 0.24f

/* ---------------- ringState ---------------- */

/* Indices into the seven answers a behaviour gives about one ring. */
enum { RS_RF, RS_CREST, RS_SHEAR, RS_ALPHA, RS_FORM, RS_SPIKE, RS_CRESTGAIN, RS_N };

/*
 * Answer one behaviour's questions about one ring.
 *
 *   rf         resting radius factor, against the shell radius
 *   crest      event weight 0..1; drives radius UP and ink DARKER together
 *   shear      BOUNDED extra yaw for this ring, radians. Must not accumulate
 *              with t: blending interpolates it, so t*rate would differ by tens
 *              of radians late in a session and whip the shell round.
 *   alpha      ring opacity; 1 unless the behaviour is signalling an event
 *   form       how assembled the ring is; 1 for everything but INITIALIZING
 *   spike      how far a passing wavefront displaces a dot, and WHICH WAY.
 *              Positive carries dots outward (a voice leaving), negative draws
 *              them in (a voice arriving).
 *   crestGain  how much a passing wavefront emphasises the dots under it
 */
static void ring_state(orb_behaviour_t b, int ri, float ring_t, float sin_lat,
                       float t, float amp, int g_which, float g_env,
                       float g_local, float *out)
{
    out[RS_FORM] = 1.0f;
    out[RS_SPIKE] = 0.0f;
    out[RS_CRESTGAIN] = 0.0f;

    switch (b) {
    case ORB_LISTENING:
        /* Taking the voice IN. Fixed tempo -- the mic sets only how deep each
         * lunge goes, so a loud voice makes a bigger gesture, not a faster one.
         * At silence the shell is nearly still, waiting. */
        out[RS_RF] = 0.9f;
        out[RS_CREST] = 0.0f;
        out[RS_SHEAR] = 0.0f;
        out[RS_ALPHA] = 1.0f;
        /* NEGATIVE: the wavefronts converge and the dots they reach are drawn
         * toward the core. */
        out[RS_SPIKE] = -(0.05f + 0.17f * amp);
        out[RS_CRESTGAIN] = 0.3f + 0.7f * amp;
        return;

    case ORB_SPEAKING:
        /* Putting the voice OUT: the mirror gesture. The shell also swells with
         * the level, reaching wave's own maximum at full volume and never past. */
        out[RS_RF] = 0.78f + 0.02f * amp;
        out[RS_CREST] = 0.0f;
        out[RS_SHEAR] = 0.0f;
        out[RS_ALPHA] = 1.0f;
        out[RS_SPIKE] = 0.04f + 0.145f * amp;
        out[RS_CRESTGAIN] = 0.35f + 0.65f * amp;
        return;

    case ORB_THINKING: {
        /* Wave's undulation at a little under half its swing. The full-width
         * roll is too broad next to the spikes either side of it in a turn;
         * narrowing keeps the character and the tempo while letting it sit as
         * the calm middle. No amplitude term at all. */
        float w = wave_w(t, ri);
        out[RS_RF] = 0.9f + 0.045f * w;
        out[RS_CREST] = (w > 0.0f) ? w : 0.0f;
        out[RS_SHEAR] = 0.0f;
        out[RS_ALPHA] = 1.0f;
        return;
    }

    case ORB_CONNECTING: {
        /* Straining to reach: the most urgent thing here. */
        float sp = spike_pulse(t * 3.4f - ri * 0.9f);
        out[RS_RF] = 0.85f + 0.05f * sinf(t * 2.6f + ri * 0.5f) + 0.07f * sp;
        out[RS_CREST] = 0.25f + 0.75f * sp;
        out[RS_SHEAR] = 0.5f * sin_lat * sinf(t * 1.1f);
        /* Energetic but DIM. Motion says it is straining; faintness says it has
         * not got through. A bright agitated shell would read as connected. */
        out[RS_ALPHA] = 0.5f + 0.35f * sp;
        return;
    }

    case ORB_BUFFERING: {
        /* Through, and working: a bright band sweeps pole to pole and back.
         * Calmer than CONNECTING but fuller and brighter, so the session reads
         * as having made progress rather than still struggling. */
        float at = 0.5f - 0.5f * cosf(TAU * (t / BUFFER_PERIOD));
        float d = ring_t - at;
        float band = expf(-(d * d) / 0.012f);
        out[RS_RF] = WAVE_BASE + 0.06f * band;
        out[RS_CREST] = band;
        out[RS_SHEAR] = 0.0f;
        out[RS_ALPHA] = 0.7f + 0.3f * band;
        return;
    }

    case ORB_INITIALIZING: {
        /* A formation wave sweeping pole to pole. The ramp is a raised cosine,
         * smooth at BOTH ends, so nothing ever jumps back to scattered.
         * Offsetting phase by a full turn across the rings means part of the
         * shell is always assembled and part always arriving. */
        float form = 0.5f - 0.5f * cosf(TAU * (t / ASSEMBLE_PERIOD + ring_t));
        out[RS_RF] = WAVE_BASE + 0.02f;
        out[RS_CREST] = form * form;
        out[RS_SHEAR] = 0.0f;
        /* The one legitimate alpha event: dots still in flight are faint. */
        out[RS_ALPHA] = 0.6f + 0.4f * form;
        out[RS_FORM] = form;
        return;
    }

    case ORB_DISCONNECTED: {
        /* Straining for a signal that is not there. Deliberately the emptiest
         * state -- it is what everything else is brighter than. */
        float at = 0.5f - 0.5f * cosf(TAU * (t / SIGNAL_PERIOD));
        float d = ring_t - at;
        float ping = expf(-(d * d) / 0.02f);
        out[RS_RF] = 0.8f + 0.012f * sinf(t * 0.5f + ri * 0.3f);
        out[RS_CREST] = 0.3f * ping;
        out[RS_SHEAR] = 0.0f;
        out[RS_ALPHA] = 0.14f + 0.4f * ping;
        return;
    }

    case ORB_SPEAKING_FILL: {
        /*
         * The agent talking: the disconnected shell as a level meter.
         *
         * Rings ILLUMINATE OUTWARD from the equator rather than a crest
         * travelling through them. How far the light reaches is the audio level,
         * so it extends while the agent speaks and retracts as it falls -- a
         * spectrum analyser bar wrapped onto the sphere, with a brighter cap at
         * the leading edge the way an analyser marks its peak.
         *
         * Built ON disconnected -- same drawn-in radius, same 0.14 floor -- so a
         * turn ending is the light withdrawing from a shell that was already
         * there rather than one object replacing another.
         *
         * Symmetric about the equator on purpose: a fill with a direction would
         * imply the sound came from somewhere, and it comes from the whole object.
         *
         * A PURE FUNCTION, like every other behaviour here. An analyser's falling
         * peak marker would need memory across frames, and the shell's contract is
         * that a dropped frame costs nothing and there is no state to resync. The
         * retraction comes from ui.c's own fast-attack slow-release on `amp`,
         * which is where that shaping belongs.
         */
        float from_eq = fabsf(ring_t - 0.5f) * 2.0f;

        float fill = amp * FILL_REACH;
        if (fill > 1.0f) {
            fill = 1.0f;
        }
        /* Off entirely below a whisper, so silence is exactly the dim shell. */
        float gate = fill / FILL_GATE;
        if (gate > 1.0f) {
            gate = 1.0f;
        }

        float lit = (fill - from_eq) / FILL_FADE;
        if (lit < 0.0f) lit = 0.0f;
        else if (lit > 1.0f) lit = 1.0f;

        /* The cap: a soft peak sitting exactly at the fill boundary. */
        float td = (fill - from_eq) / LEAD_WIDTH;
        float tip = expf(-td * td);

        out[RS_RF] = 0.8f + 0.012f * sinf(t * 0.5f + ri * 0.3f);
        /* Rule 4: a crest makes a dot bigger AND darker at the same instant. */
        out[RS_CREST] = FILL_SWELL * lit * gate;
        out[RS_SHEAR] = 0.0f;
        float a = 0.14f + (FILL_BRIGHT * lit + LEAD_BRIGHT * tip) * gate;
        out[RS_ALPHA] = (a > 1.0f) ? 1.0f : a;
        return;
    }

    case ORB_LISTENING_FILL: {
        /*
         * The user talking: the speaking fill, inward.
         *
         * from_pole is 0 at either pole and 1 at the equator, so the lit band
         * grows from the ends towards the middle and the two fronts converge when
         * someone is loud -- the mirror of SPEAKING, which grows outward from the
         * equator to the ends.
         *
         * That mirroring is deliberate and is why this is a separate behaviour
         * rather than a sign flip on a shared one. The voice shell's vocabulary
         * had the user's speech travelling inward and the agent's outward; keeping
         * it means the same object reads as receiving or as producing, rather than
         * as two animations that happen to share a shell.
         *
         * Everything about the LOOK is shared with the speaking fill on purpose --
         * retuning the brightness retunes both, because they are one language.
         * Only the direction and the level mapping differ.
         *
         * A COMPRESSED CURVE, unlike SPEAKING's linear map. Measured across
         * LISTENING, the microphone spans a live-mic room noise of 0.014 to a loud
         * peak of 0.227 -- a 4.5x window sitting close to the noise, where playback
         * gets a wide and predictable one. See LISTEN_CURVE for why a square root
         * is not enough, and LISTEN_FLOOR for why compressing means a floor has to
         * come off first. The floor is load-bearing, not a preference.
         */
        float from_pole = 1.0f - fabsf(ring_t - 0.5f) * 2.0f;

        float lvl = (amp - LISTEN_FLOOR) / (1.0f - LISTEN_FLOOR);
        if (lvl < 0.0f) {
            lvl = 0.0f;
        }
        float fill = powf(lvl, LISTEN_CURVE) * LISTEN_REACH;
        if (fill > 1.0f) {
            fill = 1.0f;
        }
        float gate = fill / FILL_GATE;
        if (gate > 1.0f) {
            gate = 1.0f;
        }

        float lit = (fill - from_pole) / FILL_FADE;
        if (lit < 0.0f) lit = 0.0f;
        else if (lit > 1.0f) lit = 1.0f;

        float td = (fill - from_pole) / LEAD_WIDTH;
        float tip = expf(-td * td);

        out[RS_RF] = 0.8f + 0.012f * sinf(t * 0.5f + ri * 0.3f);
        out[RS_CREST] = FILL_SWELL * lit * gate;
        out[RS_SHEAR] = 0.0f;
        float a = 0.14f + (FILL_BRIGHT * lit + LEAD_BRIGHT * tip) * gate;
        out[RS_ALPHA] = (a > 1.0f) ? 1.0f : a;
        return;
    }

    case ORB_IDLE:
    default: {
        /*
         * Connected and at rest, and ALIVE rather than merely looping -- this is
         * where a session sits between turns, so it is the state most likely to
         * be watched long enough to be found out.
         *
         * Three sines, and the third is what stops it repeating: wave's pair are
         * a simple frequency ratio and return to the same pose on a fixed loop,
         * so a term at an irrational multiple makes the sum quasi-periodic.
         * Normalised by the sum of the weights so w stays inside +/-1 exactly as
         * the two-sine version did -- without that, every amplitude budgeted
         * against RF_CEILING below would be wrong.
         *
         * ti, not t: idle's own wandering clock.
         */
        float ti = idle_time(t);
        float w = (0.62f * sinf(ti * 1.05f - ri * 0.52f) +
                   0.38f * sinf(ti * 0.635f + ri * 0.83f) +
                   0.22f * sinf(ti * 0.4271f + ri * 0.31f)) / 1.22f;
        /* SKEWED breath -- squashed toward its own sign, so the shell fills
         * faster than it empties. A symmetric sine reads as a machine at rest;
         * an asymmetric one reads as breathing. */
        float breath = (w > 0.0f) ? w * (1.15f - 0.15f * w) : w * 0.8f;
        out[RS_RF] = 0.9f + 0.038f * breath;
        out[RS_CREST] = 0.25f * ((w > 0.0f) ? w : 0.0f);

        /*
         * Differential twist: the equator leads and the poles lag. cos^2(lat) is
         * maximum at the equator and zero at both poles, so nothing tears.
         *
         * THREE terms, and the point is that the rings do not agree. The two
         * main ones take phase from the ring index with opposite signs and
         * incommensurate rates, so neighbours are always somewhat out of step
         * and the pattern never realigns; the third alternates SIGN with ring
         * index, so bands creep against each other the way shear layers move in
         * a fluid. A single term twisted the shell as one piece and read as
         * rigid however much it moved.
         */
        float eq = 1.0f - sin_lat * sin_lat;
        out[RS_SHEAR] = eq * (0.085f * sinf(ti * 0.42f + ri * 0.77f) +
                              0.05f * sinf(ti * 0.2571f - ri * 1.31f) +
                              0.035f * sinf(ri * 0.9f) * sinf(ti * 0.1733f));
        out[RS_ALPHA] = 1.0f;

        /* ...and on top, one of idle's gestures. Only the two per-RING ones are
         * answerable here; the ripple is per-dot and lives in orb_build(). */
        if (g_env > 0.0f) {
            if (g_which == GESTURE_TWIST) {
                /* A gust through the twist: the differential shear briefly
                 * deepens and runs the other way, like a breeze crossing a
                 * field. Rides the same cos^2(lat) profile as the base twist so
                 * it cannot tear at the poles. */
                out[RS_SHEAR] -= g_env * 0.16f * eq * sinf(ti * 0.29f);
            } else if (g_which == GESTURE_SIGH) {
                /* One deep, slow breath. The swing roughly doubles and the crest
                 * lifts with it, so the shell fills visibly and settles -- the
                 * same gesture a body makes, and the reason it is separate from
                 * the base breath rather than a bigger version of it. */
                float slow = sinf(TAU * g_local * 0.5f);
                out[RS_RF] += g_env * 0.04f * slow;
                out[RS_CREST] += g_env * 0.18f * ((slow > 0.0f) ? slow : 0.0f);
            }
        }
        return;
    }
    }
}

/* Defined with the voice build below; every mode sorts into the same order. */
static int cmp_draw_order(const void *pa, const void *pb);
/* Built once by orb_init(), defined with the rubik mode below. */
static void rubik_make_moves(void);

/* ---------------- setup ---------------- */

bool orb_init(float size)
{
    if (s_lattice == NULL) {
        /*
         * ONE allocation for every table, wave's included. Not tidiness: a
         * separate 3 kB block for wave's longitudes would sit under
         * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL and be handed internal RAM, which
         * is the resource this file has always been careful with. Pooled, the
         * block clears the threshold and lands in PSRAM.
         */
        s_lattice = malloc((3 * ORB_VOICE_DOTS + 2 * ORB_WAVE_DOTS
                            + 3 * RIBBON_GHOSTS + 3 * RIBBON_SEGS
                            + 2 * RIBBON_LANES
                            + 3 * BRAID_GHOSTS) * sizeof(float));
        if (s_lattice == NULL) {
            return false;
        }
        /* Doubles, so a separate block -- 9.2 kB, which clears
         * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL on its own and lands in PSRAM. */
        s_wave_unit = malloc(3 * ORB_WAVE_DOTS * sizeof(double));
        if (s_wave_unit == NULL) {
            return false;
        }
        size_t off = 0;
        s_cos_lon = &s_lattice[off]; off += ORB_VOICE_DOTS;
        s_sin_lon = &s_lattice[off]; off += ORB_VOICE_DOTS;
        s_scatter = &s_lattice[off]; off += ORB_VOICE_DOTS;
        s_wave_cos_lon = &s_lattice[off]; off += ORB_WAVE_DOTS;
        s_wave_sin_lon = &s_lattice[off]; off += ORB_WAVE_DOTS;
        s_ghost_dx = &s_lattice[off]; off += RIBBON_GHOSTS;
        s_ghost_dy = &s_lattice[off]; off += RIBBON_GHOSTS;
        s_ghost_dz = &s_lattice[off]; off += RIBBON_GHOSTS;
        s_seg_a = &s_lattice[off]; off += RIBBON_SEGS;
        s_seg_cos = &s_lattice[off]; off += RIBBON_SEGS;
        s_seg_sin = &s_lattice[off]; off += RIBBON_SEGS;
        s_lane_off = &s_lattice[off]; off += RIBBON_LANES;
        s_lane_edge = &s_lattice[off]; off += RIBBON_LANES;
        s_bghost_dx = &s_lattice[off]; off += BRAID_GHOSTS;
        s_bghost_dy = &s_lattice[off]; off += BRAID_GHOSTS;
        s_bghost_dz = &s_lattice[off]; off += BRAID_GHOSTS;
    }

    s_cx = size / 2.0f;
    s_cy = size / 2.0f;
    s_shell_r = (size / 2.0f) * 0.874f;
    s_rs = powf(size / 300.0f, RS_POW);

    int n = 0;
    for (int ri = 0; ri <= ORB_RINGS; ri++) {
        float lat = -(float)M_PI / 2.0f + ((float)ri / (float)ORB_RINGS) * (float)M_PI;
        float cos_lat = cosf(lat);
        float sin_lat = sinf(lat);

        int lon_count = (int)lroundf(fabsf(cos_lat) * (float)ORB_LON_DENSITY);
        if (lon_count < 1) {
            lon_count = 1;
        }

        /*
         * THE CAP IS HAND-COMPUTED AND NOTHING CHECKED IT UNTIL NOW.
         *
         * ORB_VOICE_DOTS sizes the pooled lattice above, but the count comes
         * from this loop's arithmetic over ORB_RINGS and ORB_LON_DENSITY -- and
         * those are tuning knobs the header explicitly invites moving ("Reduce a
         * mode's tuning if a frame is too dear"). They agree today, at exactly
         * 456. Nudge either and the writes below would run off a PSRAM block
         * with nothing to stop them, which is a corruption bug somewhere else
         * rather than a short orb.
         *
         * Failing the boot is the right answer: face_orb's init() turns this
         * into ESP_ERR_NO_MEM, select_face() logs it by name, and the device
         * carries on headless -- loud, and cheaper than the alternative. There
         * is no ESP_LOGE here because this file stays free of ESP-IDF so
         * host/run.sh can compile it.
         */
        if (n + lon_count > ORB_VOICE_DOTS) {
            return false;
        }

        s_rings[ri].sin_lat = sin_lat;
        s_rings[ri].cos_lat = cos_lat;
        s_rings[ri].lon_count = lon_count;
        s_rings[ri].base = n;

        for (int lj = 0; lj < lon_count; lj++) {
            float lon = ((float)lj / (float)lon_count) * TAU;
            s_cos_lon[n + lj] = cosf(lon);
            s_sin_lon[n + lj] = sinf(lon);
        }
        n += lon_count;
    }
    s_dot_count = n;

    for (int i = 0; i < n; i++) {
        /* Inside the shell only -- a scattered dot must not poke past the rim. */
        s_scatter[i] = 0.16f + 0.8f * hash_d((double)i, 3.71);
    }

    /* Wave's lattice: same cosine taper, its own ring count and density. */
    int wn = 0;
    for (int ri = 0; ri <= WAVE_RINGS; ri++) {
        float lat = -(float)M_PI / 2.0f + ((float)ri / (float)WAVE_RINGS) * (float)M_PI;
        float cos_lat = cosf(lat);
        float sin_lat = sinf(lat);

        int lon_count = (int)lroundf(fabsf(cos_lat) * (float)WAVE_LON_DENSITY);
        if (lon_count < 1) {
            lon_count = 1;
        }

        /* Same guard as the shell's, against ORB_WAVE_DOTS. Currently 384, and
         * this lattice feeds s_wave_unit as three doubles per dot as well, so an
         * overrun here walks two allocations rather than one. */
        if (wn + lon_count > ORB_WAVE_DOTS) {
            return false;
        }

        s_wave_rings[ri].sin_lat = sin_lat;
        s_wave_rings[ri].cos_lat = cos_lat;
        s_wave_rings[ri].lon_count = lon_count;
        s_wave_rings[ri].base = wn;

        /* Double throughout, matching buildLattice, then narrowed. The cast
         * keeps the sign of a near-zero cosine, which is the whole point. */
        double lat_d = -M_PI / 2.0 + ((double)ri / (double)WAVE_RINGS) * M_PI;
        double cos_lat_d = cos(lat_d);
        double sin_lat_d = sin(lat_d);
        for (int lj = 0; lj < lon_count; lj++) {
            double lon_d = ((double)lj / (double)lon_count) * 2.0 * M_PI;
            s_wave_cos_lon[wn + lj] = (float)cos(lon_d);
            s_wave_sin_lon[wn + lj] = (float)sin(lon_d);
            s_wave_unit[3 * (wn + lj) + 0] = cos_lat_d * cos(lon_d);
            s_wave_unit[3 * (wn + lj) + 1] = sin_lat_d;
            s_wave_unit[3 * (wn + lj) + 2] = cos_lat_d * sin(lon_d);
        }
        wn += lon_count;
    }
    s_wave_dot_count = wn;

    rubik_make_moves();

    /* ribbon's precompute. fibDir's golden angle, verbatim. */
    const double golden = M_PI * (3.0 - sqrt(5.0));
    for (int i = 0; i < RIBBON_GHOSTS; i++) {
        double gy = 1.0 - (2.0 * ((double)i + 0.5)) / (double)RIBBON_GHOSTS;
        double rad = sqrt(1.0 - gy * gy);
        double ga = (double)i * golden;
        s_ghost_dx[i] = (float)(rad * cos(ga));
        s_ghost_dy[i] = (float)gy;
        s_ghost_dz[i] = (float)(rad * sin(ga));
    }
    for (int i = 0; i < BRAID_GHOSTS; i++) {
        double gy = 1.0 - (2.0 * ((double)i + 0.5)) / (double)BRAID_GHOSTS;
        double rad = sqrt(1.0 - gy * gy);
        double ga = (double)i * golden;
        s_bghost_dx[i] = (float)(rad * cos(ga));
        s_bghost_dy[i] = (float)gy;
        s_bghost_dz[i] = (float)(rad * sin(ga));
    }
    for (int k = 0; k < RIBBON_SEGS; k++) {
        double a = ((double)k / (double)RIBBON_SEGS) * 2.0 * M_PI;
        s_seg_a[k] = (float)a;
        s_seg_cos[k] = (float)cos(a);
        s_seg_sin[k] = (float)sin(a);
    }
    for (int w = 0; w < RIBBON_LANES; w++) {
        double mid = ((double)RIBBON_LANES - 1.0) / 2.0;
        s_lane_off[w] = (float)(((double)w - mid) * 0.075);
        double half = (mid > 1.0) ? mid : 1.0;
        s_lane_edge[w] = (float)(fabs((double)w - mid) / half);
    }
    return true;
}

/* ---------------- wave (lattice.ts buildWave) ---------------- */

/*
 * The playground's `listening` orb: one lat/long shell undulating under two
 * incommensurate sines. No behaviours, no events, no amplitude -- a wave does
 * the same thing forever, which is exactly why it reads as attentive.
 *
 * Reuses wave_w() above, which the voice shell already carried verbatim, and the
 * same radius/ink coupling: R_BASE, R_DEPTH, INK_FAR and INK_SPAN are wave's own
 * defaults, which is where the voice shell got them.
 */
/*
 * How the microphone level reaches wave: it scales every radius, through the
 * reference's own dyn.rMul.
 *
 * NOT AS EXPRESSIVE AS THE SHELL'S LISTENING, and that is inherent rather than a
 * shortcut. buildWave has no wobMul and no wavefront term -- it undulates the same
 * way whatever is happening -- so rMul is the only amplitude hook the reference
 * gives it. The shell's listening pose sent wavefronts travelling inward with
 * depth set by volume; wave can only swell. Anything more would mean inventing a
 * parameter upstream does not have, which is exactly what the parity harness
 * exists to stop.
 *
 * SQRT OF THE LEVEL, NOT THE LEVEL. Measured across LISTENING on real speech, amp
 * spans 0.003 to 0.534 -- a 180x range -- and no linear map serves both ends of
 * that. A gain set for the quiet end saturates at amp 0.14 and throws away every
 * dynamic above moderate speech; one set for the loud end barely moves at
 * conversational volume. The square root compresses the top and opens out the
 * bottom, so the whole range is usable:
 *
 *   amp 0.010  near silence    rMul 1.19
 *   amp 0.056  typical mean    rMul 1.45
 *   amp 0.096  typical peak    rMul 1.59
 *   amp 0.268  loud mean       rMul 1.98
 *   amp 0.534  loud peak       rMul 2.39, just short of the ceiling
 *
 * The earlier linear gain was calibrated against a mean of 0.067 and a peak of
 * 0.10, taken while LISTENING barely triggered -- so it was measured on the quiet
 * moments of speech rather than on speech.
 *
 * CAPPED AT 1.6 FOR THE RASTERISER, not for taste. blit_dot's footprint buffer is
 * SPRITE_MAX 14 px square and it CLIPS a disc that does not fit rather than
 * overrunning. A dot's worst case here is (0.6 + 1.7) * rs * rMul, which at
 * rs 1.302 reaches a 14 px sprite around rMul 1.85. 1.6 leaves margin.
 */
/* [0..~1.9]    sqrt(amp) -> swell. 1.9 puts a loud peak just under the ceiling;
 *              past that, loud speech saturates and stops registering. */
#define WAVE_RMUL_GAIN 1.9f
/* [1.0..2.6]   swell ceiling. A RASTERISER BOUND: blit_dot's footprint buffer is
 *              SPRITE_MAX px square and CLIPS a disc that does not fit. At
 *              SPRITE_MAX 20 a wave dot reaches the edge around rMul 2.6. */
#define WAVE_RMUL_MAX 2.4f
/* [0..1.0]     sqrt(amp) -> extra brightness, applied by orb_wave_ink() as a
 *              separate pass. LOCAL, not from the reference: buildWave has no ink
 *              hook, so this composes over the finished frame the way voice_pass
 *              does and the harness never sees it. 0.62 gives a 0.15 shift at a
 *              conversational level, where the old linear 1.2 gave 0.07. */
#define WAVE_INK_GAIN 0.62f
/* [0..0.28]    how much DARKER wave sits at rest, before any level brightens it.
 *              Ink is inverted on a dark ground, so this ADDS to white.
 *
 *              Contrast, not taste: the reference's near side rests at grey 230
 *              of 255, so brightening had 26 levels to work in and the response
 *              was invisible however hard it was driven. Resting darker gives it
 *              room -- 0.20 opens the near side to 76 levels and the far side to
 *              115. Past ~0.28 the far side rests near grey 15 and the shell
 *              stops reading as a surface at all. */
#define WAVE_INK_FLOOR 0.20f

void orb_build_wave(orb_frame_t *out, float t, float amp)
{
    const float R = s_shell_r;

    if (amp < 0.0f) amp = 0.0f;
    else if (amp > 1.0f) amp = 1.0f;
    /* rMul is 1 at rest, which is the reference's default -- so silence is the
     * unmodified mode rather than a special case. */
    float r_mul = 1.0f + WAVE_RMUL_GAIN * sqrtf(amp);
    if (r_mul > WAVE_RMUL_MAX) {
        r_mul = WAVE_RMUL_MAX;
    }
    const float rs = s_rs * r_mul;

    /* buildWave's projection: yaw = t*0.18, pitch = 0.38, no roll, no shear. */
    const float yaw = t * 0.18f;
    const float tilt = 0.38f;
    const float sy = sinf(yaw), cyw = cosf(yaw);
    const float st = sinf(tilt), ct = cosf(tilt);

    size_t count = 0;

    for (int ri = 0; ri < WAVE_RING_COUNT; ri++) {
        const orb_ring_t *ring = &s_wave_rings[ri];
        float sin_lat = ring->sin_lat;
        float cos_lat = ring->cos_lat;

        float w = wave_w(t, ri);
        /* The undulation pulls the shell in and out around 0.88 of the rim. */
        float dr = 0.88f + 0.105f * w;
        float rr = R * dr;
        float crest = (w > 0.0f) ? w : 0.0f;

        const float *cos_lon = &s_wave_cos_lon[ring->base];
        const float *sin_lon = &s_wave_sin_lon[ring->base];

        for (int lj = 0; lj < ring->lon_count; lj++) {
            /* Project the unit vector and scale after: the projection is linear,
             * so this is identical to projecting the scaled vector. */
            float ux = cos_lat * cos_lon[lj];
            float uz = cos_lat * sin_lon[lj];

            float x1 = ux * cyw + uz * sy;
            float z1 = -ux * sy + uz * cyw;
            float y1 = sin_lat * ct - z1 * st;
            float dz = sin_lat * st + z1 * ct;

            /*
             * DEPTH COMES FROM THE SCALED z, NOT THE UNIT ONE. The reference
             * reads (z / R + 1) / 2 off a vector of magnitude rr, and rr is not
             * R here -- it is R * dr. The voice build's (dz + 1) / 2 is only
             * equivalent because its radius is exactly R.
             */
            float depth = (dz * dr + 1.0f) / 2.0f;
            if (depth < 0.0f) {
                depth = 0.0f;
            } else if (depth > 1.0f) {
                depth = 1.0f;
            }

            orb_dot_t *d = &out->dots[count++];
            d->x = s_cx + x1 * rr;
            /* MINUS: makeProj's py is `cy - yr * scale`. Screen y grows
             * downward and the shell's y grows up, so the projection flips it.
             * Everything else here matched the reference to 0.0001 with this
             * wrong -- a mirrored orb looks perfectly plausible on a round
             * panel, which is why the harness catches it and an eye would not. */
            d->y = s_cy - y1 * rr;
            d->z = dz * rr;
            d->r = (R_BASE + R_DEPTH * depth) * (1.0f + 0.4f * crest) * rs;
            if (d->r < R_MIN) {
                d->r = R_MIN;
            }
            d->white = INK_FAR - INK_SPAN * depth - 0.1f * crest;
            d->a = 1.0f; /* wave never signals an event, so never fades */
        }
    }

    qsort(out->dots, count, sizeof(out->dots[0]), cmp_draw_order);
    out->count = count;
    out->line_count = 0; /* only web emits lines */
}

/* ---------------- rubik (lattice.ts buildRubik) ---------------- */

/*
 * The playground's `solving` orb: a dotted sphere whose bands scramble and
 * unscramble, one quarter-turn at a time, forever.
 *
 * Shares wave's lattice -- its profile is latRings 15 / lonDensity 40 too -- but
 * nothing else. Its own shell radius (0.82, not 0.874), its own ink constants,
 * and a projection that folds R into makeProj's `scale` rather than scaling
 * afterwards, which is why depth here comes from an UNSCALED z.
 */
#define RUBIK_MOVE_COUNT 14
#define RUBIK_SLOT_DUR 0.42f
#define RUBIK_REST 1.2f

typedef struct {
    int axis;     /* 0 = x, 1 = y, 2 = z */
    float lo, hi; /* the slab this move turns */
    double ang;   /* double: see rubik_apply_moves */
} rubik_move_t;

static rubik_move_t s_rubik_moves[RUBIK_MOVE_COUNT];

/* makeMoves: a fixed, hash-derived scramble. Same every boot on purpose -- the
 * reference's sequence is part of what the harness diffs. */
static void rubik_make_moves(void)
{
    for (int i = 0; i < RUBIK_MOVE_COUNT; i++) {
        int axis = (int)floorf(hash_d((double)i, 2.3) * 3.0f);
        if (axis > 2) {
            axis = 2;
        }
        int step = (int)floorf(hash_d((double)i, 5.9) * 4.0f);
        if (step > 3) {
            step = 3;
        }
        float dir = (hash_d((double)i, 7.7) < 0.5f) ? 1.0f : -1.0f;

        s_rubik_moves[i].axis = axis;
        s_rubik_moves[i].lo = -1.0f + 0.5f * (float)step;
        s_rubik_moves[i].hi = s_rubik_moves[i].lo + 0.5f;
        s_rubik_moves[i].ang = (double)dir * M_PI / 2.0;
    }
}

/*
 * How far through the scramble-and-solve cycle each move is.
 *
 * Moves land one at a time, then unwind in reverse -- so the orb is never
 * "wrong", it is always mid-procedure. The ease-out is cubic and deliberately
 * mechanical: it arrives like a machine placing a part, not like something alive.
 */
static int rubik_solve_cycle(float t, double amount[RUBIK_MOVE_COUNT])
{
    const int count = RUBIK_MOVE_COUNT;
    for (int i = 0; i < count; i++) {
        amount[i] = 0.0;
    }

    float cyc = 2.0f * (float)count * RUBIK_SLOT_DUR + RUBIK_REST;
    float tc = fmodf(t, cyc);
    if (tc < 0.0f) {
        tc += cyc;
    }
    if (tc >= 2.0f * (float)count * RUBIK_SLOT_DUR) {
        return -1; /* the rest between cycles: solved and still */
    }

    int slot = (int)floorf(tc / RUBIK_SLOT_DUR);
    /* Double from here down: `ep` becomes a rotation angle whose cosine decides
     * the next move's slab test, so its last bits are not cosmetic. */
    double pr = ((double)tc - (double)slot * (double)RUBIK_SLOT_DUR) / (double)RUBIK_SLOT_DUR;
    double cl = (pr / 0.7 > 1.0) ? 1.0 : (pr / 0.7);
    double inv = 1.0 - cl;
    double ep = 1.0 - inv * inv * inv;

    int active;
    if (slot < count) {
        for (int i = 0; i < slot; i++) {
            amount[i] = 1.0;
        }
        amount[slot] = ep;
        active = slot;
    } else {
        int u = 2 * count - 1 - slot;
        for (int i = 0; i < u; i++) {
            amount[i] = 1.0;
        }
        amount[u] = 1.0 - ep;
        active = u;
    }
    return active;
}

/*
 * Rotate a dot through every move whose slab contains it. Returns true if the
 * dot is inside the move currently turning -- the band that inks darker.
 *
 * DOUBLE, unlike every other hot path in this file, and not by preference.
 * Slab membership is tested against the coordinate AS ALREADY ROTATED by earlier
 * moves, so this is a chain of discontinuous decisions rather than a smooth
 * function -- and a fully applied move turns by exactly +/-pi/2, where cosf
 * gives -4.4e-8 and cos gives +6.1e-17. Opposite sides of the 0.0 slab
 * boundary. In float, 38 of 384 dots turned with the wrong band.
 *
 * The trig is per MOVE, not per dot -- hoisted by the caller -- so what this
 * actually costs is a few double multiplies for the ~25% of dots inside any
 * given slab.
 */
static bool rubik_apply_moves(const double *unit, const double *amount,
                              const double *ca_tab, const double *sa_tab,
                              int active, float out[3])
{
    double x = unit[0], y = unit[1], z = unit[2];
    bool in_active = false;

    for (int i = 0; i < RUBIK_MOVE_COUNT; i++) {
        if (amount[i] <= 0.0) {
            continue;
        }
        const rubik_move_t *mv = &s_rubik_moves[i];
        double coord = (mv->axis == 0) ? x : (mv->axis == 1) ? y : z;
        if (coord < (double)mv->lo || coord >= (double)mv->hi) {
            continue;
        }
        if (i == active) {
            in_active = true;
        }
        double ca = ca_tab[i], sa = sa_tab[i];
        if (mv->axis == 0) {
            double y2 = y * ca - z * sa;
            z = y * sa + z * ca;
            y = y2;
        } else if (mv->axis == 1) {
            double x2 = x * ca + z * sa;
            z = -x * sa + z * ca;
            x = x2;
        } else {
            double x2 = x * ca - y * sa;
            y = x * sa + y * ca;
            x = x2;
        }
    }
    out[0] = (float)x;
    out[1] = (float)y;
    out[2] = (float)z;
    return in_active;
}

#define RUBIK_R_ACTIVE 0.3f
#define RUBIK_INK_FAR 0.62f
#define RUBIK_INK_SPAN 0.54f

void orb_build_rubik(orb_frame_t *out, float t)
{
    const float R = s_cx * 0.82f; /* rubik's own shell, tighter than wave's */

    /* buildRubik folds R into makeProj's `scale`, so the projection returns
     * screen pixels directly and an UNSCALED z. */
    const float yaw = t * 0.55f;
    const float tilt = 0.35f + 0.1f * sinf(t * 0.9f);
    const float sy = sinf(yaw), cyw = cosf(yaw);
    const float st = sinf(tilt), ct = cosf(tilt);

    double amount[RUBIK_MOVE_COUNT];
    int active = rubik_solve_cycle(t, amount);

    /* Per move, not per dot: fourteen sin/cos a frame rather than thousands. */
    double ca_tab[RUBIK_MOVE_COUNT], sa_tab[RUBIK_MOVE_COUNT];
    for (int i = 0; i < RUBIK_MOVE_COUNT; i++) {
        double a = s_rubik_moves[i].ang * amount[i];
        ca_tab[i] = cos(a);
        sa_tab[i] = sin(a);
    }

    size_t count = 0;

    for (int ri = 0; ri < WAVE_RING_COUNT; ri++) {
        const orb_ring_t *ring = &s_wave_rings[ri];

        for (int lj = 0; lj < ring->lon_count; lj++) {
            size_t k = (size_t)ring->base + (size_t)lj;
            float m[3];
            bool in_active = rubik_apply_moves(&s_wave_unit[3 * k], amount,
                                               ca_tab, sa_tab, active, m);

            float x1 = m[0] * cyw + m[2] * sy;
            float z1 = -m[0] * sy + m[2] * cyw;
            float y1 = m[1] * ct - z1 * st;
            float zr = m[1] * st + z1 * ct;

            /* Unscaled, because R went into the projection's scale. */
            float depth = (zr + 1.0f) / 2.0f;
            if (depth < 0.0f) {
                depth = 0.0f;
            } else if (depth > 1.0f) {
                depth = 1.0f;
            }

            orb_dot_t *d = &out->dots[count++];
            d->x = s_cx + x1 * R;
            d->y = s_cy - y1 * R;
            d->z = zr;
            d->r = (R_BASE + R_DEPTH * depth + (in_active ? RUBIK_R_ACTIVE : 0.0f)) * s_rs;
            if (d->r < R_MIN) {
                d->r = R_MIN;
            }
            d->white = RUBIK_INK_FAR - RUBIK_INK_SPAN * depth - (in_active ? 0.14f : 0.0f);
            d->a = 1.0f;
        }
    }

    qsort(out->dots, count, sizeof(out->dots[0]), cmp_draw_order);
    out->count = count;
    out->line_count = 0; /* only web emits lines */
}

/* ---------------- ribbon (ribbon.ts buildRibbon) ---------------- */

/*
 * The playground's `composing` orb: a band that precesses through the sphere,
 * undulating along its length, over a haze of ghost dots.
 *
 * The largest mode at 590 dots, and the only one so far that varies alpha per
 * dot -- the ghosts sit at 0.1..0.32 so they read as depth rather than as marks.
 * Its rBase is 1.1, not the 0.6 wave and rubik share: the band is a line the eye
 * follows, so its dots are fatter.
 */
#define RIBBON_R_BASE 1.1f
#define RIBBON_GHOST_INK 0.78f

/*
 * How the voice level reaches the band: it scales the undulation's DEPTH.
 *
 * The reference exposes this as wobMul, and it is the right hook rather than a
 * convenient one -- rule 5 of the voice shell is that amplitude scales how deep a
 * gesture goes and never how fast, because driving rate from level is frequency
 * modulation and reads as vibration rather than as a voice. The band's tempo is
 * untouched; only how far it flexes moves.
 *
 * TUNED FROM MEASURED PLAYBACK, after a first attempt that "hardly moved".
 *
 * The fault was the BASE, not the gain. At 0.35 the band already rippled clearly
 * in silence, and since tempo is fixed the only thing speech could change was
 * depth -- so the eye had constant motion to compare against and the modulation
 * vanished into it. Dropping the base gives it a still reference to see against.
 *
 * Against real agent playback, amp runs a mean of 0.26..0.50 and peaks 0.39..0.65,
 * varying by better than 2x inside a single second. So:
 *
 *   silence      amp 0.00 -> 0.08   nearly a clean band
 *   speech mean  amp 0.35 -> 0.85   clearly flexing
 *   speech peak  amp 0.65 -> 1.51   deep, past the reference's own default
 *
 * A 19x range where the first attempt had 2.7x. Capped, because the offset is
 * applied before the band is renormalised onto the sphere and very large values
 * stop reading as a band at all.
 */
#define RIBBON_WOB_BASE 0.08f
#define RIBBON_WOB_GAIN 2.2f
#define RIBBON_WOB_MAX 1.6f

void orb_build_ribbon(orb_frame_t *out, float t, float amp)
{
    const float R = s_cx * 0.78f;

    /* scale = 1, so scaled vectors go in and z comes back scaled. */
    const float yaw = t * 0.1f;
    const float tilt = 0.3f;
    const float sy = sinf(yaw), cyw = cosf(yaw);
    const float st = sinf(tilt), ct = cosf(tilt);

    if (amp < 0.0f) amp = 0.0f;
    else if (amp > 1.0f) amp = 1.0f;
    float wob_mul = RIBBON_WOB_BASE + RIBBON_WOB_GAIN * amp;
    if (wob_mul > RIBBON_WOB_MAX) {
        wob_mul = RIBBON_WOB_MAX;
    }

    size_t count = 0;

    /* The ghost shell first, matching the reference's emission order. */
    const float ghost_r = 0.8f * s_rs;
    for (int i = 0; i < RIBBON_GHOSTS; i++) {
        float ux = s_ghost_dx[i], uy = s_ghost_dy[i], uz = s_ghost_dz[i];

        float x1 = ux * cyw + uz * sy;
        float z1 = -ux * sy + uz * cyw;
        float y1 = uy * ct - z1 * st;
        float dz = uy * st + z1 * ct;

        /* The unit vector is scaled by R before projection, so z/R is dz. */
        float depth = (dz + 1.0f) / 2.0f;
        if (depth < 0.0f) depth = 0.0f;
        else if (depth > 1.0f) depth = 1.0f;

        orb_dot_t *d = &out->dots[count++];
        d->x = s_cx + x1 * R;
        d->y = s_cy - y1 * R;
        d->z = dz * R;
        d->r = ghost_r;
        if (d->r < R_MIN) {
            d->r = R_MIN;
        }
        d->white = RIBBON_GHOST_INK;
        d->a = 0.1f + 0.22f * depth;
    }

    /*
     * The band's plane, as two orthogonal in-plane vectors and their normal.
     * Recomputed per frame because it precesses; the reference builds it the same
     * way, and the cross product is what the lane offset is applied along.
     */
    float ya = t * 0.24f;
    float ta = 0.55f + 0.3f * sinf(t * 0.18f);
    float bux = cosf(ya), buy = 0.0f, buz = sinf(ya);
    float bvx = -buz * sinf(ta), bvy = cosf(ta), bvz = bux * sinf(ta);
    float bnx = buy * bvz - buz * bvy;
    float bny = buz * bvx - bux * bvz;
    float bnz = bux * bvy - buy * bvx;

    for (int w = 0; w < RIBBON_LANES; w++) {
        float lane_off = s_lane_off[w];
        float edge = s_lane_edge[w];
        for (int k = 0; k < RIBBON_SEGS; k++) {
            float a = s_seg_a[k];
            float ca = s_seg_cos[k];
            float sa = s_seg_sin[k];

            /* Two travelling waves along the band, phased per lane so the whole
             * ribbon flexes rather than every lane moving as one. */
            float wob = (0.16f * sinf(a * 3.0f - t * 1.7f + (float)w * 0.22f) +
                         0.07f * sinf(a * 5.0f + t * 1.1f)) * wob_mul;
            float off = lane_off + wob;

            float x = bux * ca + bvx * sa + bnx * off;
            float y = buy * ca + bvy * sa + bny * off;
            float z = buz * ca + bvz * sa + bnz * off;
            float l = sqrtf(x * x + y * y + z * z);
            /* Back onto the unit sphere: the band rides the surface. */
            x /= l; y /= l; z /= l;

            float x1 = x * cyw + z * sy;
            float z1 = -x * sy + z * cyw;
            float y1 = y * ct - z1 * st;
            float dz = y * st + z1 * ct;

            float depth = (dz + 1.0f) / 2.0f;
            if (depth < 0.0f) depth = 0.0f;
            else if (depth > 1.0f) depth = 1.0f;

            orb_dot_t *d = &out->dots[count++];
            d->x = s_cx + x1 * R;
            d->y = s_cy - y1 * R;
            d->z = dz * R;
            d->r = (RIBBON_R_BASE + R_DEPTH * depth) * (1.0f - 0.25f * edge) * s_rs;
            if (d->r < R_MIN) {
                d->r = R_MIN;
            }
            d->white = 0.52f - 0.44f * depth + 0.18f * edge;
            d->a = 0.4f + 0.6f * depth;
        }
    }

    qsort(out->dots, count, sizeof(out->dots[0]), cmp_draw_order);
    out->count = count;
    out->line_count = 0; /* only web emits lines */
}

/* ---------------- braid (braid.ts frameBraid) ---------------- */

/*
 * frac, in DOUBLE, and that matters more than it looks.
 *
 * It is discontinuous at every integer, and it is what walks a strand pole to
 * pole -- so a float rounding that lands on the other side of an integer does not
 * shift a dot slightly, it teleports it to the opposite pole. Same class of
 * problem as rubik's slab test: a smooth-looking mode with one discrete decision
 * buried in it.
 */
static double orb_frac(double x)
{
    return x - floor(x);
}

/*
 * The playground's `weaving` orb: three strands plaiting pole to pole, over the
 * same Fibonacci ghost shell ribbon uses.
 *
 * THE ONLY MODE THAT CULLS. A strand fades to nothing at the poles, so dots fall
 * below the 0.02 alpha floor and the count varies frame to frame -- which is why
 * the harness compares counts rather than assuming them.
 */
#define BRAID_STRANDS 3
#define BRAID_STRAND_N 52
#define BRAID_TURNS 3.0f
#define BRAID_R_BASE 1.2f
#define BRAID_R_DEPTH 1.8f

void orb_build_braid(orb_frame_t *out, float t)
{
    const float R = s_cx * 0.76f;

    const float yaw = t * 0.4f;
    const float tilt = 0.3f;
    const float sy = sinf(yaw), cyw = cosf(yaw);
    const float st = sinf(tilt), ct = cosf(tilt);

    size_t count = 0;

    /* Ghosts: identical to ribbon's but on braid's tighter shell. */
    const float ghost_r = 0.8f * s_rs;
    for (int i = 0; i < BRAID_GHOSTS; i++) {
        float ux = s_bghost_dx[i] * R, uy = s_bghost_dy[i] * R, uz = s_bghost_dz[i] * R;

        float x1 = ux * cyw + uz * sy;
        float z1 = -ux * sy + uz * cyw;
        float y1 = uy * ct - z1 * st;
        float z = uy * st + z1 * ct;

        /* NOT CLAMPED, unlike every other mode here. braid's weave term pushes a
         * strand past R, so depth legitimately exceeds 1 and the reference lets
         * it -- radius and alpha run slightly over their nominal range on the
         * near side. Clamping pinned r to a constant and cost 15 dots. */
        float depth = (z / R + 1.0f) / 2.0f;

        float alpha = 0.1f + 0.22f * depth;
        if (alpha < ALPHA_CULL) {
            continue;
        }
        orb_dot_t *d = &out->dots[count++];
        d->x = s_cx + x1;
        d->y = s_cy - y1;
        d->z = z;
        d->r = (ghost_r < R_MIN) ? R_MIN : ghost_r;
        d->white = RIBBON_GHOST_INK;
        d->a = alpha;
    }

    for (int sn = 0; sn < BRAID_STRANDS; sn++) {
        float phase = ((float)sn / (float)BRAID_STRANDS) * TAU;
        for (int i = 0; i < BRAID_STRAND_N; i++) {
            /* u walks pole to pole; the frac drift slides the whole strand. */
            float u = (float)((orb_frac((double)i / (double)BRAID_STRAND_N
                                        + (double)t * 0.045) * 2.0 - 1.0) * 0.96);
            float s1 = 1.0f - u * u;
            float surf = sqrtf((s1 > 0.0f) ? s1 : 0.0f);
            float end_fade = (1.0f - fabsf(u)) / 0.1f;
            if (end_fade > 1.0f) {
                end_fade = 1.0f;
            }
            float a = u * (float)M_PI * BRAID_TURNS + phase;
            /* Radial breathing: the strands trade places, which is what reads as
             * the over-and-under of a plait rather than three separate helices. */
            float weave = 1.0f + 0.075f * sinf(u * (float)M_PI * BRAID_TURNS * 2.0f
                                               + phase * 2.0f + t * 0.8f);
            float rr = surf * R * weave;

            float vx = cosf(a) * rr;
            float vy = u * R * weave;
            float vz = sinf(a) * rr;

            float x1 = vx * cyw + vz * sy;
            float z1 = -vx * sy + vz * cyw;
            float y1 = vy * ct - z1 * st;
            float zr = vy * st + z1 * ct;

            float depth = (zr / R + 1.0f) / 2.0f; /* unclamped: see above */

            float alpha = end_fade * (0.45f + 0.55f * depth);
            if (alpha < ALPHA_CULL) {
                continue;
            }
            orb_dot_t *d = &out->dots[count++];
            d->x = s_cx + x1;
            d->y = s_cy - y1;
            d->z = zr;
            d->r = (BRAID_R_BASE + BRAID_R_DEPTH * depth) * s_rs;
            if (d->r < R_MIN) {
                d->r = R_MIN;
            }
            d->white = 0.55f - 0.45f * depth;
            d->a = alpha;
        }
    }

    qsort(out->dots, count, sizeof(out->dots[0]), cmp_draw_order);
    out->count = count;
    out->line_count = 0; /* only web emits lines */
}

/* ---------------- web (web.ts frameWeb) ---------------- */

/*
 * Value noise on an integer grid, smoothstepped. In DOUBLE: floor() is
 * discontinuous, and this is what makes the nodes wander, so a rounding that
 * crosses a grid line jumps a node rather than nudging it.
 */
static double orb_vnoise(double x, double y)
{
    double xi = floor(x), yi = floor(y);
    double fx = x - xi, fy = y - yi;
    fx = fx * fx * (3.0 - 2.0 * fx);
    fy = fy * fy * (3.0 - 2.0 * fy);
    double a = hash_d(xi, yi);
    double b = hash_d(xi + 1.0, yi);
    double c = hash_d(xi, yi + 1.0);
    double d = hash_d(xi + 1.0, yi + 1.0);
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

/*
 * The playground's `connecting` orb: a constellation wiring itself.
 *
 * Thirty nodes drift on the sphere, every pair closer than the threshold grows
 * an edge, and five packets run along pairs re-picked on a coarse clock. The only
 * mode with lines, and the only one whose node dots carry no alpha of their own --
 * the reference leaves Dot.a undefined there, which finalizeFrame reads as 1.
 */
#define WEB_NODES 30
#define WEB_SIGNALS 5
#define WEB_THR 0.72f
#define WEB_NODE_R 1.4f
#define WEB_NODE_R_DEPTH 1.8f

void orb_build_web(orb_frame_t *out, float t)
{
    const float R = s_cx * 0.8f;

    /* scale = R here, so unit vectors go in and the distances below stay in
     * unit-sphere space -- which is what the threshold is expressed in. */
    const float yaw = t * 0.12f;
    const float tilt = 0.32f;
    const float sy = sinf(yaw), cyw = cosf(yaw);
    const float st = sinf(tilt), ct = cosf(tilt);

    /* Nodes: a Fibonacci lattice pushed around by slow noise, back onto the
     * surface. Doubles, because orb_vnoise's grid is. */
    double nx[WEB_NODES], ny[WEB_NODES], nz[WEB_NODES];
    const double golden = M_PI * (3.0 - sqrt(5.0));
    for (int i = 0; i < WEB_NODES; i++) {
        double gy = 1.0 - (2.0 * ((double)i + 0.5)) / (double)WEB_NODES;
        double rad = sqrt(1.0 - gy * gy);
        double ga = (double)i * golden;
        double x = rad * cos(ga) + 0.3 * (orb_vnoise((double)i * 0.31 + 9.0, (double)t * 0.24) - 0.5) * 2.0;
        double y = gy + 0.3 * (orb_vnoise((double)i * 0.53 + 27.0, (double)t * 0.21) - 0.5) * 2.0;
        double z = rad * sin(ga) + 0.3 * (orb_vnoise((double)i * 0.77 + 55.0, (double)t * 0.27) - 0.5) * 2.0;
        double l = sqrt(x * x + y * y + z * z);
        nx[i] = x / l; ny[i] = y / l; nz[i] = z / l;
    }

    /* Project each node once: both the edges and the node dots need it. */
    float px[WEB_NODES], py[WEB_NODES], pz[WEB_NODES];
    for (int i = 0; i < WEB_NODES; i++) {
        float ux = (float)nx[i], uy = (float)ny[i], uz = (float)nz[i];
        float x1 = ux * cyw + uz * sy;
        float z1 = -ux * sy + uz * cyw;
        float y1 = uy * ct - z1 * st;
        pz[i] = uy * st + z1 * ct;
        px[i] = s_cx + x1 * R;
        py[i] = s_cy - y1 * R;
    }

    size_t lines = 0;
    for (int i = 0; i < WEB_NODES; i++) {
        for (int j = i + 1; j < WEB_NODES; j++) {
            double dx = nx[i] - nx[j], dy = ny[i] - ny[j], dz = nz[i] - nz[j];
            double dist = sqrt(dx * dx + dy * dy + dz * dz);
            if (dist >= (double)WEB_THR) {
                continue;
            }
            float depth = ((pz[i] + pz[j]) / 2.0f + 1.0f) / 2.0f;
            float alpha = (float)(1.0 - dist / (double)WEB_THR) * (0.3f + 0.55f * depth);
            if (alpha < ALPHA_CULL || lines >= ORB_MAX_LINES) {
                continue;
            }
            orb_line_t *ln = &out->lines[lines++];
            ln->x1 = px[i]; ln->y1 = py[i];
            ln->x2 = px[j]; ln->y2 = py[j];
            ln->white = 0.42f;
            ln->a = alpha;
            ln->w = (0.8f * s_rs < 0.6f) ? 0.6f : (0.8f * s_rs);
        }
    }

    size_t count = 0;
    for (int i = 0; i < WEB_NODES; i++) {
        float depth = (pz[i] + 1.0f) / 2.0f;
        float pulse = 1.0f + 0.25f * sinf(t * 1.4f + (float)i * 2.7f);
        orb_dot_t *d = &out->dots[count++];
        d->x = px[i];
        d->y = py[i];
        d->z = pz[i];
        d->r = (WEB_NODE_R + WEB_NODE_R_DEPTH * depth) * pulse * s_rs;
        if (d->r < R_MIN) {
            d->r = R_MIN;
        }
        d->white = 0.55f - 0.45f * depth;
        d->a = 1.0f; /* the reference leaves Dot.a undefined; finalizeFrame reads 1 */
    }

    /* Signals: the pair is re-picked on a coarse clock, so a packet finishes its
     * run and the next one starts somewhere else entirely. */
    for (int sg = 0; sg < WEB_SIGNALS; sg++) {
        double phase = (double)t * 0.55 + (double)sg * 7.31;
        double seg = floor(phase);
        int ia = (int)floor(hash_d(seg, (double)sg * 3.1 + 1.7) * (double)WEB_NODES);
        int ib = (int)floor(hash_d(seg, (double)sg * 5.7 + 4.2) * (double)WEB_NODES);
        if (ia == ib || ia < 0 || ib < 0 || ia >= WEB_NODES || ib >= WEB_NODES) {
            continue;
        }
        double f = orb_frac(phase);
        double x = nx[ia] + (nx[ib] - nx[ia]) * f;
        double y = ny[ia] + (ny[ib] - ny[ia]) * f;
        double z = nz[ia] + (nz[ib] - nz[ia]) * f;
        double l = sqrt(x * x + y * y + z * z);
        if (l < 1e-6) {
            l = 1e-6;
        }
        float ux = (float)(x / l), uy = (float)(y / l), uz = (float)(z / l);

        float x1 = ux * cyw + uz * sy;
        float z1 = -ux * sy + uz * cyw;
        float y1 = uy * ct - z1 * st;
        float zr = uy * st + z1 * ct;
        float depth = (zr + 1.0f) / 2.0f;

        float alpha = 0.5f + 0.5f * depth;
        if (alpha < ALPHA_CULL) {
            continue;
        }
        orb_dot_t *d = &out->dots[count++];
        d->x = s_cx + x1 * R;
        d->y = s_cy - y1 * R;
        d->z = zr;
        d->r = (WEB_NODE_R * 1.5f + WEB_NODE_R_DEPTH * depth) * s_rs;
        if (d->r < R_MIN) {
            d->r = R_MIN;
        }
        d->white = 0.05f;
        d->a = alpha;
    }

    qsort(out->dots, count, sizeof(out->dots[0]), cmp_draw_order);
    out->count = count;
    out->line_count = lines;
}

/* ---------------- wave's ink pass ---------------- */

/*
 * Brighten wave's dots with the microphone level.
 *
 * SEPARATE FROM orb_build_wave ON PURPOSE, and the separation is the point.
 * buildWave exposes exactly one dynamic input, dyn.rMul, so radius is the only
 * thing the reference lets volume touch -- and a 60% swell on its own does not
 * read as someone talking. Ink does, and ink is ours.
 *
 * So this composes over the finished dot list rather than going inside the build,
 * exactly as voice_pass does and for exactly the same reason: orb_build_wave stays
 * a faithful transcription the harness can diff, and the thing with no upstream
 * stays outside it. host/orb_dump.c simply never calls this.
 *
 * Ink is inverted on a dark ground -- grey is (1 - white) * 255 -- so brightening
 * means SUBTRACTING. Alpha is left alone: wave sets it to 1 throughout, so there
 * is no headroom there and ink is the whole of brightness on this panel.
 */
void orb_wave_ink(orb_frame_t *out, float amp)
{
    if (amp < 0.0f) {
        amp = 0.0f;
    } else if (amp > 1.0f) {
        amp = 1.0f;
    }
    /*
     * Runs even at silence, unlike before: the floor has to be applied whatever
     * the level, since resting darker is the whole reason the brightening reads.
     *
     * sqrt matches the swell, for the same reason -- a 180x amp range that no
     * linear map serves at both ends.
     */
    float d = WAVE_INK_FLOOR - WAVE_INK_GAIN * sqrtf(amp);
    for (size_t i = 0; i < out->count; i++) {
        float w = out->dots[i].white + d;
        if (w < 0.0f) w = 0.0f;
        else if (w > 1.0f) w = 1.0f; /* the floor must not push the far side past black */
        out->dots[i].white = w;
    }
}

/* ---------------- the voice pass ---------------- */

/*
 * Peak outward displacement from the low band, as a fraction of a dot's distance
 * from the centre. Small on purpose: the shell reads as breathing at 0.09 and as
 * a bouncing ball well before 0.2.
 */
#define VP_SWELL 0.09f
/* Peak displacement of the travelling mid-band wave, same units. */
#define VP_RIPPLE 0.055f
/* Radians of wave across the shell's radius. About two crests visible at once --
 * enough to read as travelling, few enough that neighbours stay correlated and
 * the shell does not turn to noise. */
#define VP_RIPPLE_K 7.5f
/* Wave travel, radians per second. Negative travels outward. */
#define VP_RIPPLE_SPEED (-4.2f)
/* Peak radius gain from the low band, and the extra riding the mid-band crest. */
#define VP_FATTEN 0.35f
#define VP_FATTEN_CREST 0.3f
/* Peak ink shift from the high band. Ink is 0 at darkest, so this SUBTRACTS.
 * Kept well under INK_SPAN so a loud sibilant never flattens the near/far read
 * that the depth ramp encodes. */
#define VP_INK 0.22f
/* Below this a band cannot displace a dot visibly, so the whole pass is skipped.
 * See the guard in voice_pass(). */
#define VP_SILENT (1.0f / 512.0f)

/*
 * A band-reactive pass over the FINISHED dot list.
 *
 * The behaviours decide where dots go; this only moves dots already there. That
 * separation is the point -- it composes with every behaviour instead of being
 * wired into one, and it is exactly a no-op when nothing is being said.
 *
 * Runs before the sort, unlike the reference, because this port's draw order is
 * bucketed by row for cache locality: displacing y afterwards would scatter the
 * very locality the bucketing exists to create.
 */
static void voice_pass(orb_frame_t *out, size_t count, float t,
                       const orb_bands_t *b)
{
    /*
     * Silence must cost nothing and change nothing.
     *
     * An epsilon, not `<= 0`. The caller's release filter approaches zero
     * asymptotically and never actually arrives, so testing against zero makes
     * this guard fire once before the first sound and never again -- which
     * measured as a permanent 4.6 ms per frame for a pass doing nothing visible.
     * A band below one part in 512 cannot move a dot by a visible amount.
     */
    if (b->low < VP_SILENT && b->mid < VP_SILENT && b->high < VP_SILENT) {
        return;
    }

    float swell = VP_SWELL * b->low;
    float ripple = VP_RIPPLE * b->mid;
    float fatten = VP_FATTEN * b->low;
    float crest = VP_FATTEN_CREST * b->mid;
    float ink = VP_INK * b->high;

    /* Normalise distance by the half-size, so the wavelength is a fraction of
     * the shell rather than a pixel count and the pass reads the same at any
     * size. */
    float inv = (s_cx > 0.0f) ? (1.0f / s_cx) : 0.0f;

    /*
     * WRAPPED, and this is not cosmetic. sinf is TAU-periodic so wrapping is
     * exact, but an unwrapped `t * RIPPLE_SPEED` reaches -3000 and beyond within
     * minutes of uptime, and newlib's argument reduction for a large operand is
     * far more expensive than the reduced case -- measured as most of a 4.6 ms
     * per-frame cost that had nothing to do with the work being done.
     *
     * The same rule the travelling wavefront in orb_build() already follows:
     * anything carrying t must stay bounded.
     */
    float phase = fmodf(t * VP_RIPPLE_SPEED, TAU);

    for (size_t i = 0; i < count; i++) {
        orb_dot_t *d = &out->dots[i];
        float dx = d->x - s_cx;
        float dy = d->y - s_cy;
        float dist = sqrtf(dx * dx + dy * dy);
        float u = dist * inv;

        /* The wave is a function of RADIUS, so a dot exactly at the centre has
         * no direction to be pushed along. Leave it, and give it the trough
         * value rather than dividing by zero. */
        float wave = (u > 0.0f) ? sinf(phase + u * VP_RIPPLE_K) : -1.0f;

        if (dist > 1e-6f) {
            float k = 1.0f + swell + ripple * wave;
            d->x = s_cx + dx * k;
            d->y = s_cy + dy * k;
        }

        d->r *= 1.0f + fatten + crest * (0.5f + 0.5f * wave);

        float w = d->white - ink * (0.5f + 0.5f * wave);
        d->white = (w < 0.0f) ? 0.0f : ((w > 1.0f) ? 1.0f : w);
    }
}

/* ---------------- build ---------------- */

/*
 * Draw order: horizontal band first, depth within the band.
 *
 * Depth alone is the obvious answer and it was measurably the wrong one. The
 * rasteriser writes into a 434 kB PSRAM canvas, and consecutive dots in pure
 * depth order land at unrelated screen positions, so essentially every dot pays
 * fresh cache-line fills -- 23 ms per frame of pure memory latency, against 2 ms
 * for all the geometry.
 *
 * Bucketing by a band of rows first means the dots that follow one another in
 * the list also share cache lines. Depth still orders everything inside a band,
 * so occlusion is preserved except between dots whose centres straddle a band
 * boundary and whose discs happen to overlap -- at ORB_Y_BAND rows against a
 * ~4 px dot, a rare case confined to a few pixels, and worth it.
 */
#define ORB_Y_BAND 16

static int cmp_draw_order(const void *a, const void *b)
{
    const orb_dot_t *da = (const orb_dot_t *)a;
    const orb_dot_t *db = (const orb_dot_t *)b;

    int ba = (int)da->y / ORB_Y_BAND;
    int bb = (int)db->y / ORB_Y_BAND;
    if (ba != bb) {
        return (ba < bb) ? -1 : 1;
    }
    return (da->z < db->z) ? -1 : ((da->z > db->z) ? 1 : 0);
}

void orb_rotate(orb_frame_t *f, float degrees)
{
    if (degrees == 0.0f) {
        return;
    }
    const float rad = degrees * (3.14159265358979f / 180.0f);
    const float c = cosf(rad), s = sinf(rad);
    /* Screen y grows downward, so this turns clockwise as seen. */
    for (size_t i = 0; i < f->count; i++) {
        const float dx = f->dots[i].x - s_cx, dy = f->dots[i].y - s_cy;
        f->dots[i].x = s_cx + dx * c - dy * s;
        f->dots[i].y = s_cy + dx * s + dy * c;
    }
    /* Both endpoints, or web's edges detach from the nodes they connect. */
    for (size_t i = 0; i < f->line_count; i++) {
        orb_line_t *l = &f->lines[i];
        const float ax = l->x1 - s_cx, ay = l->y1 - s_cy;
        const float bx = l->x2 - s_cx, by = l->y2 - s_cy;
        l->x1 = s_cx + ax * c - ay * s;
        l->y1 = s_cy + ax * s + ay * c;
        l->x2 = s_cx + bx * c - by * s;
        l->y2 = s_cy + bx * s + by * c;
    }
}

void orb_build(orb_frame_t *out, float t, orb_behaviour_t from,
               orb_behaviour_t to, float mix, float amp,
               const orb_bands_t *bands)
{
    if (!(amp > 0.0f)) {
        amp = 0.0f; /* !(x > 0) so a NaN lands on silence, not on every dot */
    } else if (amp > 1.0f) {
        amp = 1.0f;
    }
    if (!(mix > 0.0f)) {
        mix = 0.0f;
    } else if (mix > 1.0f) {
        mix = 1.0f;
    }

    /* Skip the second evaluation once a transition has landed -- the steady
     * state is the overwhelmingly common one. */
    bool blending = (mix < 1.0f) && (from != to);

    /* Idle's gesture, resolved ONCE for the whole frame: it is a pure function of
     * t, so one evaluation is the same answer any caller would compute. */
    int g_which;
    float g_env, g_local;
    idle_gesture(t, &g_which, &g_env, &g_local);

    /*
     * How much of this frame is IDLE, from the same blend the poses use. Every
     * idle-only flourish is scaled by it, so a call arriving mid-gesture fades
     * the gesture out over the blend instead of cutting it -- and non-idle frames
     * skip the work entirely rather than multiplying by zero per dot.
     */
    float idle_w = ((from == ORB_IDLE) ? (1.0f - mix) : 0.0f) +
                   ((to == ORB_IDLE) ? mix : 0.0f);

    /* --- idle's body: float, breath, wobble, hop --------------------------
     *
     * All of it on idle's own wandering clock, so the body drifts in tempo with
     * the surface rather than beating against it, and all of it scaled by idle_w
     * so a call arrives at a body that is exactly centred and round.
     */
    float tb = idle_time(t);
    /* Position AND analytic velocity: the derivative of the same two sines,
     * because differencing frames would make the wobble frame-rate dependent. */
    float bob = BODY_BOB_A1 * sinf(tb * BODY_BOB_W1) +
                BODY_BOB_A2 * sinf(tb * BODY_BOB_W2 + 1.7f);
    float bob_vel = BODY_BOB_A1 * BODY_BOB_W1 * cosf(tb * BODY_BOB_W1) +
                    BODY_BOB_A2 * BODY_BOB_W2 * cosf(tb * BODY_BOB_W2 + 1.7f);

    /* The breath draws IN only, and is NOT gated on idle -- see BODY_BREATH. */
    float body_scale =
        1.0f - BODY_BREATH * (0.5f + 0.5f * sinf(tb * BODY_BREATH_W));

    if (idle_w > 0.0f && g_env > 0.0f && g_which == GESTURE_HOP) {
        /*
         * NEGATIVE sine, and that sign is the whole gesture. The envelope peaks
         * mid-span, so the lobe landing there is what the eye reads as the
         * movement: with this sign that is the LEAP, and what falls either side
         * becomes an anticipating dip before it and a squashing landing after.
         * The obvious sign gives a body that sinks in the middle of its own hop.
         */
        float hop = -sinf(TAU * g_local * HOP_BOUNCES);
        float hop_vel = -TAU * HOP_BOUNCES * cosf(TAU * g_local * HOP_BOUNCES);
        bob += idle_w * g_env * HOP_HEIGHT * hop;
        /* Follows the hop's own VELOCITY, not the envelope shape, so it stretches
         * on the way up, rounds at the apex and squashes through the landing. */
        bob_vel += idle_w * g_env * HOP_HEIGHT * hop_vel * HOP_DEFORM * 12.0f;
        /* Heaviest at the bottom of the arc -- the body compressing under itself. */
        body_scale -= idle_w * g_env * HOP_DEFORM * 0.35f *
                      ((-hop > 0.0f) ? -hop : 0.0f);
    }

    /* Vertical travel stretches the body along its motion and narrows it across;
     * volume roughly preserved, which is what sells mass. */
    float stretch = idle_w * BODY_WOBBLE * bob_vel;
    if (stretch > BODY_WOBBLE_MAX) {
        stretch = BODY_WOBBLE_MAX;
    } else if (stretch < -BODY_WOBBLE_MAX) {
        stretch = -BODY_WOBBLE_MAX;
    }
    const float body_x = body_scale * (1.0f - stretch);
    const float body_y = body_scale * (1.0f + stretch);
    /* Screen y grows downward, so a POSITIVE bob subtracts to float up. */
    const float body_cy = s_cy - idle_w * bob * (s_cx * 2.0f);

    /* --- the hive ripple, set up once per frame ------------------------- */
    bool hiving = (idle_w > 0.0f) && (g_env > 0.0f) && (g_which == GESTURE_RIPPLE);
    float ox = 0.0f, oy = 1.0f, oz = 0.0f;
    float front = 0.0f, hive_decay = 0.0f, hive_squash = 1.0f;
    if (hiving) {
        /* Where the first dot moved. Uniform on the sphere (oy uniform in height
         * is what makes it uniform in AREA), hashed off the epoch -- so the
         * origin differs every time and is still a pure function of the clock. */
        float k = floorf(t / GESTURE_EPOCH);
        oy = 2.0f * hash_d((double)k, 11.13) - 1.0f;
        float ring_r = sqrtf((1.0f - oy * oy > 0.0f) ? (1.0f - oy * oy) : 0.0f);
        float az = TAU * hash_d((double)k, 19.07);
        ox = ring_r * cosf(az);
        oz = ring_r * sinf(az);
        /* Advanced through a cosine so the front travels at constant ANGULAR
         * speed, and reaches the far side just as the envelope closes -- so it
         * never has to be cut off, it runs out of surface. */
        front = 1.0f - cosf((float)M_PI * g_local);
        hive_squash = 1.0f - HIVE_SQUASH * idle_w * g_env;
        /* Fades as it goes, the way a disturbance loses energy to the dots it has
         * already moved. g_env alone would let the far side move as much as the
         * origin did, which reads as a pulse rather than a ripple. */
        hive_decay = idle_w * g_env * (1.0f - 0.75f * g_local);
    }

    /*
     * ONE shared base rotation for every behaviour. A state change must never
     * alter the accumulated yaw, or blending would whip the shell round;
     * per-ring shear rides on top and is bounded.
     *
     * The spin no longer runs at one speed: three BOUNDED terms ride the
     * accumulating base rate, so the turn visibly gathers pace and eases off.
     * Their summed derivative stays well under the 0.18 base, so the shell never
     * stalls or reverses -- a spin that stops reads as a dropped frame, not life.
     */
    float spin_drift = 0.45f * sinf(t * 0.081f) +
                       0.18f * sinf(t * 0.1913f + 0.7f) +
                       0.06f * sinf(t * 0.27f);
    const float yaw = t * 0.18f + idle_w * spin_drift;
    /* Two rates on the axis as well, so the pole traces a slow irregular path
     * instead of rocking between two positions. */
    const float tilt = 0.38f + idle_w * (0.05f * sinf(t * 0.19f + 1.1f) +
                                         0.025f * sinf(t * 0.0729f));
    const float roll = idle_w * 0.03f * sinf(t * 0.13f);

    const float st = sinf(tilt), ct = cosf(tilt);
    const float sy = sinf(yaw), cyw = cosf(yaw);
    const float sr = sinf(roll), cr = cosf(roll);

    float ra[RS_N], rb[RS_N];
    size_t count = 0;

    for (int ri = 0; ri < ORB_RING_COUNT; ri++) {
        const orb_ring_t *ring = &s_rings[ri];
        float ring_t = (float)ri / (float)(ORB_RING_COUNT - 1);
        float sin_lat = ring->sin_lat;
        float cos_lat = ring->cos_lat;

        ring_state(blending ? from : to, ri, ring_t, sin_lat, t, amp,
                   g_which, g_env, g_local, ra);
        float rf = ra[RS_RF];
        float crest = ra[RS_CREST];
        float shear = ra[RS_SHEAR];
        float alpha = ra[RS_ALPHA];
        float form = ra[RS_FORM];

        /*
         * The two wavefront terms are deliberately NOT blended into one value.
         * Everything above is a scalar pose that interpolates meaningfully, but
         * a pair of counter-travelling waves does not: their average is a wave
         * with a direction neither behaviour has. They are carried per behaviour
         * and their CONTRIBUTIONS blended, below.
         */
        float spike_a = ra[RS_SPIKE];
        float crest_gain_a = ra[RS_CRESTGAIN];
        float spike_b = spike_a;
        float crest_gain_b = crest_gain_a;

        if (blending) {
            ring_state(to, ri, ring_t, sin_lat, t, amp,
                       g_which, g_env, g_local, rb);
            rf += (rb[RS_RF] - rf) * mix;
            crest += (rb[RS_CREST] - crest) * mix;
            shear += (rb[RS_SHEAR] - shear) * mix;
            alpha += (rb[RS_ALPHA] - alpha) * mix;
            form += (rb[RS_FORM] - form) * mix;
            spike_b = rb[RS_SPIKE];
            crest_gain_b = rb[RS_CRESTGAIN];
        }

        float cs = cosf(shear), sn = sinf(shear);
        bool scattered = form < 0.999f;
        bool rippling = (spike_a != 0.0f) || (spike_b != 0.0f);

        /*
         * The travelling phase, wrapped into one period. ripple_pulse has period
         * TAU, so wrapping is identical to the unwrapped TAU*RIPPLE_RATE*t --
         * but the phase stays bounded however long a session runs.
         */
        float cycles = RIPPLE_RATE * t;
        float ph = TAU * (cycles - floorf(cycles));

        /*
         * Direction is read from each BEHAVIOUR's own spike, never a blended
         * one. A blended spike sweeps through zero on any direct
         * listening <-> speaking change -- a barge-in -- and flipping the sign
         * of an accumulated phase at that moment teleports the whole wavefront
         * pattern by an amount proportional to session uptime.
         */
        float flow_a = (spike_a >= 0.0f) ? -ph : ph;
        float flow_b = (spike_b >= 0.0f) ? -ph : ph;

        const float *cos_lon = &s_cos_lon[ring->base];
        const float *sin_lon = &s_sin_lon[ring->base];

        for (int lj = 0; lj < ring->lon_count; lj++) {
            /*
             * Project the UNIT vector. The projection is linear, so scaling
             * afterwards is identical to projecting the scaled vector -- and
             * this way the dot's screen-space radius is known BEFORE it is
             * displaced, which is what the wavefront travels through.
             */
            float ux = cos_lat * cos_lon[lj];
            float uz = cos_lat * sin_lon[lj];

            float vx = ux * cs + uz * sn;
            float vy = sin_lat;
            float vz = -ux * sn + uz * cs;

            float x1 = vx * cyw + vz * sy;
            float z1 = -vx * sy + vz * cyw;
            float y1 = vy * ct - z1 * st;
            float dz = vy * st + z1 * ct;

            /* Roll is a rotation in SCREEN space, applied after yaw and tilt and
             * before the scale. It leaves z alone, so it cannot reorder the
             * painter's sort. */
            float xr = (roll == 0.0f) ? x1 : (x1 * cr - y1 * sr);
            float yr = (roll == 0.0f) ? y1 : (x1 * sr + y1 * cr);

            /* Offsets of a UNIT vector, so dx/dy are in shell space. Measured
             * against the FLOATING centre, not the static one: fold the float in
             * here and the ripple's screen radius below -- which has to be read
             * in the shell's own frame -- would swell as the body drifted. */
            float dx = xr;
            float dy = -yr;

            float dr = rf;
            float dot_crest = crest;

            if (hiving) {
                /*
                 * Angular distance from the origin as 1 - cos: a dot product, no
                 * trig. Measured on the PRE-shear vector deliberately -- the
                 * shear twists the surface, and anchoring the origin after it
                 * would slide the ripple's source around the shell as the twist
                 * moved.
                 */
                float c = ux * ox + sin_lat * oy + uz * oz;
                float d = 1.0f - c;
                /*
                 * A quadratic bump rather than a gaussian: the same shape where
                 * it matters, no expf per dot, and exactly zero outside the
                 * front's width so most dots cost a compare. The width is
                 * constant in the 1 - cos measure, so the front reads a little
                 * broader across the far hemisphere -- which flatters a dying
                 * ripple rather than fighting it.
                 */
                float x = (d - front) * HIVE_INV_WIDTH;
                if (x > -1.0f && x < 1.0f) {
                    float bump = (1.0f - x * x);
                    bump *= bump;
                    /* Lift and darken together, the coupling every event here
                     * uses. */
                    dr += HIVE_LIFT * bump * hive_decay;
                    dot_crest += HIVE_CREST * bump * hive_decay;
                }
                /* The bee itself: a tighter, sharper move right at the origin
                 * over the first fifth of the gesture, before the ripple has
                 * gone anywhere. This is the part that makes it read as CAUSED --
                 * something moved, and then the surface answered. */
                if (g_local < 0.2f && d < HIVE_BEE_D) {
                    float near = 1.0f - d / HIVE_BEE_D;
                    float kick = sinf((float)M_PI * (g_local / 0.2f));
                    dr += HIVE_BEE_LIFT * near * near * kick * idle_w * g_env;
                }
                /* ...and the whole shell contracts under all of it. Applied last
                 * so it scales the front's lift too: the front stays
                 * proportionally as strong against a body drawn in around it. */
                dr *= hive_squash;
            }

            if (rippling) {
                /*
                 * Distance from the centre of the disc, 0 at the middle and 1 at
                 * the silhouette. Concentric wavefronts sweep across it, so
                 * every dot the same distance out moves together -- that
                 * coherence is what reads as a surface.
                 */
                float sr = sqrtf(dx * dx + dy * dy);
                float kr = TAU * RIPPLE_K * sr;
                float rip_a = ripple_pulse(kr + flow_a);
                float dr_add = spike_a * rip_a;
                float crest_add = crest_gain_a * rip_a;
                if (blending) {
                    /* Both waves, each in its own direction, crossfaded. Costs a
                     * sinf per dot, and only for the ~280 ms a transition is
                     * live. */
                    float rip_b = ripple_pulse(kr + flow_b);
                    dr_add += (spike_b * rip_b - dr_add) * mix;
                    crest_add += (crest_gain_b * rip_b - crest_add) * mix;
                }
                dr += dr_add;
                dot_crest += crest_add;
            }

            if (scattered) {
                float sc = s_scatter[ring->base + lj];
                dr = sc + (dr - sc) * form;
            }
            if (dr > RF_CEILING) {
                dr = RF_CEILING;
            }
            float rr = s_shell_r * dr;

            float depth = (dz + 1.0f) / 2.0f;
            if (depth < 0.0f) {
                depth = 0.0f;
            } else if (depth > 1.0f) {
                depth = 1.0f;
            }

            /* Cull before writing -- the reference drops anything this faint,
             * and a face should not pay to blend an invisible dot. */
            if (alpha < ALPHA_CULL) {
                continue;
            }

            orb_dot_t *d = &out->dots[count++];
            /* The body's squash goes on HERE rather than into dr, because it is a
             * screen-space deformation and dr is a radius factor the RF_CEILING
             * clamp is expressed in. */
            d->x = s_cx + dx * rr * body_x;
            d->y = body_cy + dy * rr * body_y;
            /* Depth stays unsquashed: it decides draw order, not appearance. */
            d->z = dz * rr;
            /* Rule 4: a crest makes a dot bigger AND darker at the same instant,
             * never one without the other. */
            d->r = (R_BASE + R_DEPTH * depth) * (1.0f + 0.4f * dot_crest) * s_rs;
            if (d->r < R_MIN) {
                d->r = R_MIN;
            }
            d->white = INK_FAR - INK_SPAN * depth - 0.1f * dot_crest;
            d->a = alpha;
        }
    }

    if (bands != NULL) {
        voice_pass(out, count, t, bands);
    }

    qsort(out->dots, count, sizeof(out->dots[0]), cmp_draw_order);
    out->count = count;
    out->line_count = 0; /* only web emits lines */
}
