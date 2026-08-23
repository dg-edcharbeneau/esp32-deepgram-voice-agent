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

/* Depth mapping. Radius and ink are both derived from it, which is rule 4. */
#define R_BASE 0.6f
#define R_DEPTH 1.7f
#define INK_FAR 0.66f
#define INK_SPAN 0.56f
#define RS_POW 0.6f

/* Dot radii were tuned for a 300 pt frame; sub-linear scaling keeps a small
 * shell legible. */
#define R_MIN 0.3f
#define ALPHA_CULL 0.02f

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
                       float t, float amp, float *out)
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
        return;
    }
    }
}

/* ---------------- setup ---------------- */

bool orb_init(float size)
{
    if (s_lattice == NULL) {
        s_lattice = malloc(3 * ORB_MAX_DOTS * sizeof(float));
        if (s_lattice == NULL) {
            return false;
        }
        s_cos_lon = &s_lattice[0];
        s_sin_lon = &s_lattice[ORB_MAX_DOTS];
        s_scatter = &s_lattice[2 * ORB_MAX_DOTS];
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
    return true;
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

    /*
     * ONE shared base rotation for every behaviour. A state change must never
     * alter the accumulated yaw, or blending would whip the shell round;
     * per-ring shear rides on top and is bounded.
     *
     * The idle-only spin drift and tilt wobble are the not-yet-ported body
     * layer, so their weight is pinned at zero here.
     */
    const float yaw = t * 0.18f;
    const float tilt = 0.38f;

    const float st = sinf(tilt), ct = cosf(tilt);
    const float sy = sinf(yaw), cyw = cosf(yaw);

    /*
     * Screen-space squash, so it goes on the x/y radius and NOT on z: the squash
     * is what the viewer sees, not a change to the shell's geometry, and scaling
     * z would reorder the painter's sort.
     *
     * The idle-only companion terms -- the float that offsets the centre and the
     * velocity-driven squash-and-stretch that makes x and y differ -- are part of
     * the unported body layer, so x and y share one scale here.
     */
    const float body_scale =
        1.0f - BODY_BREATH * (0.5f + 0.5f * sinf(idle_time(t) * BODY_BREATH_W));

    float ra[RS_N], rb[RS_N];
    size_t count = 0;

    for (int ri = 0; ri < ORB_RING_COUNT; ri++) {
        const orb_ring_t *ring = &s_rings[ri];
        float ring_t = (float)ri / (float)(ORB_RING_COUNT - 1);
        float sin_lat = ring->sin_lat;
        float cos_lat = ring->cos_lat;

        ring_state(blending ? from : to, ri, ring_t, sin_lat, t, amp, ra);
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
            ring_state(to, ri, ring_t, sin_lat, t, amp, rb);
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

            /* Offsets of a UNIT vector, so dx/dy are in shell space. */
            float dx = x1;
            float dy = -y1;

            float dr = rf;
            float dot_crest = crest;

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
            d->x = s_cx + dx * rr * body_scale;
            d->y = s_cy + dy * rr * body_scale;
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
}
