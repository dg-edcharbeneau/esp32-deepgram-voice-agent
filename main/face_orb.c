/*
 * The voice orb as a face: geometry from orb_geometry.c, pixels via orb_raster.c.
 *
 * This file is only the glue -- which behaviour is showing, and the crossfade
 * between two of them. It deliberately holds no shell maths of its own.
 */

#include <math.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "bsp/display.h"

#include "orb_geometry.h"
#include "orb_raster.h"
#include "ui_face.h"

/*
 * A state change is a BLEND, not a cut. Both behaviours are evaluated per ring
 * and crossfaded, which is what makes listening -> thinking -> speaking read as
 * one object changing its mind several times a turn rather than three animations
 * being swapped.
 */
#define BLEND_MS 280.0f

/*
 * PSRAM: 10.9 kB, and the single largest thing the orb needs. As internal .bss it
 * took the largest free internal block from ~30 kB to 13 kB and the WebSocket
 * layer started failing its writes mid-session.
 *
 * Written sequentially by the geometry pass and read sequentially by the
 * rasteriser, so the cache handles it; the frame timing did not move measurably.
 */
static orb_frame_t *s_frame;

static orb_behaviour_t s_from = ORB_DISCONNECTED;
static orb_behaviour_t s_to = ORB_DISCONNECTED;
static int64_t s_blend_start_us;

/*
 * Clock origin, reset on activation.
 *
 * Every value the shell produces is a pure function of t, so the origin is free
 * to move -- and keeping t small matters: it is a float, and the fastest term
 * multiplies it by 3.4, so at a t of days the phase quantisation grows enough to
 * read as a stutter. Anchoring to activation keeps t in the range a session
 * actually spans, and the only cost is a discontinuity at the moment of a
 * deliberate face switch, where nobody is looking for continuity anyway.
 */
static int64_t s_t0_us;

/*
 * ui_behaviour_t and orb_behaviour_t are separate on purpose: one is the public
 * vocabulary, the other is the shell's. This is the only place they meet.
 *
 * ORB_THINKING is mapped but unreachable in a real session: resolve_behaviour()
 * never produces UI_BEHAVIOUR_THINKING, because AgentThinking has not arrived in
 * any logged session. The mapping exists so the display test can show the pose,
 * which until now was parity-verified geometry nobody had seen render.
 */
/*
 * Which ported mode a state uses, or the voice shell.
 *
 * The five that map are the states with a distinct thing to say. IDLE,
 * INITIALIZING and DISCONNECTED stay on the shell: they are the resting and
 * connecting poses the shell was written for, and its ladder is what makes
 * session progress legible without a label.
 *
 * SPEAKING and LISTENING are not symmetric and must not be confused. SPEAKING is
 * the AGENT talking -- Deepgram's synthesis coming out of the speaker -- and
 * LISTENING is the USER talking, with the microphone open for transcription. So
 * ribbon is driven by playback level and wave by microphone level, which ui.c
 * already resolves into ctx->amp as whichever direction is live, at its own gain.
 */
typedef enum {
    MODE_SHELL = 0, /* the voice shell, with its own behaviour blending */
    MODE_WAVE,
    MODE_RUBIK,
    MODE_RIBBON,
    MODE_BRAID,
    MODE_WEB,
} orb_mode_t;

static orb_mode_t mode_for(ui_behaviour_t b)
{
    switch (b) {
    /*
     * LISTENING is on the shell too, and wave is detached -- the same move
     * SPEAKING made below, for the same reason. See ORB_LISTENING_FILL.
     *
     * Worth noting what falls out of both of them being shell behaviours: the
     * conversational core -- listening, thinking, speaking -- now shares one
     * lattice, so the transitions a caller sees most often all crossfade. Only
     * the four foreign modes still cut.
     */
    case UI_BEHAVIOUR_THINKING:   return MODE_RUBIK;
    /*
     * SPEAKING IS BACK ON THE SHELL, and ribbon is detached.
     *
     * It gets ORB_SPEAKING_FILL, which is the disconnected shell with light
     * pulses running from the equator out, driven by the playback level. Ribbon
     * did not sit with the rest of the family -- a flat band among dotted shells.
     *
     * A side benefit of it being a shell behaviour rather than a mode: SPEAKING
     * regains the 280 ms crossfade, because it now shares a lattice with the
     * states either side of it. Only the four remaining modes cut.
     */
    case UI_BEHAVIOUR_CONNECTING: return MODE_WEB;
    case UI_BEHAVIOUR_BUFFERING:  return MODE_BRAID;
    default:                      return MODE_SHELL;
    }
}

/* How long a cross-mode fade lasts. The shell's own BLEND_MS for consistency:
 * a viewer should not be able to tell which kind of transition they are watching. */
#define MODE_BLEND_MS BLEND_MS

/*
 * The geometry/raster split, on the bench only.
 *
 * OFF BY DEFAULT because ESP_LOGI on the LVGL task blocks on the UART for most
 * of a frame, which corrupts this measurement and ui.c's telemetry alongside it
 * -- see the note above the accumulator in ui.c. Every 60 frames is one spoiled
 * frame every 2.4 s, inside the window tlm_accumulate_frame() is averaging.
 *
 * TLM's draw= carries the total cost in normal operation. Flip this when the
 * split between geometry and raster is specifically what you are after, and read
 * the numbers knowing the frame each one landed in is not representative. The
 * figures in orb_raster.c's header came from exactly this, which is the argument
 * for keeping it rather than deleting it.
 */
#define ORB_LOG_TIMINGS 0

/*
 * The outgoing animation during a cross-mode fade, and the second frame it needs.
 *
 * PSRAM, like the primary frame and for the same reason -- 32 kB of dot list has
 * no business in internal RAM. Allocated on first use rather than at init, so a
 * build with the crossfade disabled never pays for it.
 */
static orb_frame_t *s_frame_b;

static const char *mode_name(orb_mode_t m)
{
    switch (m) {
    case MODE_WAVE:   return "wave";
    case MODE_RUBIK:  return "rubik";
    case MODE_RIBBON: return "ribbon";
    case MODE_BRAID:  return "braid";
    case MODE_WEB:    return "web";
    case MODE_SHELL:
    default:          return "shell";
    }
}

/*
 * [0..360] How far the LISTENING fill turns the orb, degrees clockwise.
 *
 * The fill grows out of the poles, and the poles sit top and bottom, so 90 moves
 * them to the sides and the light closes in horizontally instead of vertically.
 * It turns the dot lattice with it, so the rings read vertical while listening --
 * that is what rotating the object means, as opposed to just redirecting the
 * light. Set 0 to stand it back up.
 */
#define LISTEN_ROTATE_DEG 90.0f

/* Every behaviour is upright but the listening fill. Kept as a function rather
 * than folded into build_mode because the two ends of a blend need it separately. */
static float rotate_for(orb_behaviour_t b)
{
    return (b == ORB_LISTENING_FILL) ? LISTEN_ROTATE_DEG : 0.0f;
}

/* Build one animation into `out`. The shell arm takes the behaviour blend; every
 * mode ignores from/to/mix, having no behaviours to blend. */
static void build_mode(orb_mode_t mode, orb_frame_t *out, float t, float amp,
                       const orb_bands_t *bands,
                       orb_behaviour_t from, orb_behaviour_t to, float mix)
{
    switch (mode) {
    /*
     * Kept, and currently unreachable: no state maps to MODE_WAVE since LISTENING
     * moved to the shell's inward fill. Parity-verified, and one line from being
     * pointed at a state again -- as ribbon already is.
     */
    case MODE_WAVE:
        orb_build_wave(out, t, amp);
        orb_wave_ink(out, amp); /* local pass; see orb_geometry.h */
        break;
    case MODE_RUBIK:  orb_build_rubik(out, t); break;
    /*
     * Kept, and currently unreachable: no state maps to MODE_RIBBON since
     * SPEAKING moved to the shell's fill. The port stays because it is
     * parity-verified and cheap to point a state back at.
     */
    case MODE_RIBBON: orb_build_ribbon(out, t, amp); break;
    case MODE_BRAID:  orb_build_braid(out, t); break;
    case MODE_WEB:    orb_build_web(out, t); break;
    case MODE_SHELL:
    default:
        orb_build(out, t, from, to, mix, amp, bands);
        /*
         * INTERPOLATED across the blend, not switched at the end of it, so
         * entering and leaving LISTENING turns the orb through the 280 ms rather
         * than snapping it upright on the last frame. Both ends are 0 for every
         * other pair, where orb_rotate returns immediately and this costs nothing.
         */
        {
            const float a0 = rotate_for(from), a1 = rotate_for(to);
            orb_rotate(out, a0 + (a1 - a0) * mix);
        }
        break;
    }
}

/* Fade a whole animation by scaling every mark's alpha. Lines included: web's
 * edges have to leave with its nodes, not after them. */
static void scale_alpha(orb_frame_t *f, float k)
{
    for (size_t i = 0; i < f->count; i++) {
        f->dots[i].a *= k;
    }
    for (size_t i = 0; i < f->line_count; i++) {
        f->lines[i].a *= k;
    }
}

/*
 * Concatenate `b` into `a`, dropping marks the fade has taken below the cull
 * threshold -- most of a frame, late in a transition, so this is what keeps the
 * doubled cost from being a doubled cost for the whole 280 ms.
 *
 * No re-sort, and the reason is narrower than this comment used to claim. The
 * row-band bucketing is real but it belongs to the GEOMETRY, not the rasteriser:
 * cmp_draw_order() in orb_geometry.c sorts by y band and then by depth, and
 * orb_raster_draw() simply blits the array in order. So both halves arrive sorted
 * the same way, which is not the same thing as their concatenation being sorted.
 *
 * Left unsorted deliberately: a second qsort over 840 dots every frame, to fix
 * the depth order of two overlapping animations that are both fading, during the
 * ~7 frames a transition lasts. If a cross-mode fade ever looks wrong, this is
 * the place to look -- but measure the qsort before adding it, because the raster
 * beneath it is already the most expensive thing in the frame (see orb_raster.c).
 */
static void append_frame(orb_frame_t *a, const orb_frame_t *b)
{
    for (size_t i = 0; i < b->count && a->count < ORB_MAX_DOTS; i++) {
        if (b->dots[i].a < ORB_ALPHA_CULL) {
            continue;
        }
        a->dots[a->count++] = b->dots[i];
    }
    for (size_t i = 0; i < b->line_count && a->line_count < ORB_MAX_LINES; i++) {
        if (b->lines[i].a < ORB_ALPHA_CULL) {
            continue;
        }
        a->lines[a->line_count++] = b->lines[i];
    }
}

static orb_behaviour_t to_orb(ui_behaviour_t b)
{
    switch (b) {
    case UI_BEHAVIOUR_INITIALIZING: return ORB_INITIALIZING;
    /* Not ORB_LISTENING: that is the reference's inward-wavefront pose, which the
     * shell keeps and nothing now selects. See ORB_LISTENING_FILL. */
    case UI_BEHAVIOUR_LISTENING:    return ORB_LISTENING_FILL;
    case UI_BEHAVIOUR_THINKING:     return ORB_THINKING;
    /* Not ORB_SPEAKING: that is the reference's outward-wavefront pose, which the
     * shell keeps and nothing now selects. See ORB_SPEAKING_FILL. */
    case UI_BEHAVIOUR_SPEAKING:     return ORB_SPEAKING_FILL;
    case UI_BEHAVIOUR_CONNECTING:   return ORB_CONNECTING;
    case UI_BEHAVIOUR_BUFFERING:    return ORB_BUFFERING;
    case UI_BEHAVIOUR_DISCONNECTED: return ORB_DISCONNECTED;
    case UI_BEHAVIOUR_IDLE:
    default:                        return ORB_IDLE;
    }
}

static esp_err_t init(lv_obj_t *canvas)
{
    if (s_frame == NULL) {
        s_frame = heap_caps_malloc(sizeof(*s_frame), MALLOC_CAP_SPIRAM);
        if (s_frame == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!orb_init((float)BSP_LCD_H_RES)) {
        return ESP_ERR_NO_MEM;
    }
    return orb_raster_init(canvas);
}

static void activate(void)
{
    orb_raster_clear();
    s_t0_us = 0; /* stamped on the next frame, which is the first one drawn */
    s_from = s_to = ORB_DISCONNECTED;
    s_blend_start_us = 0;
}

static void render(const ui_render_ctx_t *ctx)
{
    if (s_t0_us == 0) {
        s_t0_us = ctx->now_us;
    }

    /*
     * A frozen session stops the clock rather than the drawing. Every value the
     * shell produces is a pure function of t, so holding t still holds the whole
     * pose -- no separate "paused" path, and resuming picks up mid-gesture
     * instead of snapping back to some rest position.
     */
    static int64_t s_hold_us;
    if (ctx->frozen) {
        if (s_hold_us == 0) {
            s_hold_us = ctx->now_us;
        }
    } else if (s_hold_us != 0) {
        /* Shift the origin by the paused interval so t resumes where it stopped. */
        s_t0_us += ctx->now_us - s_hold_us;
        s_hold_us = 0;
    }
    int64_t at_us = (s_hold_us != 0) ? s_hold_us : ctx->now_us;
    float t = (float)((double)(at_us - s_t0_us) / 1000000.0);

    orb_behaviour_t want = to_orb(ctx->behaviour);
    if (want != s_to) {
        /*
         * Start the crossfade from whatever we were heading towards, not from the
         * blended pose showing right now. Mid-blend changes therefore step
         * slightly -- the alternative is carrying a third behaviour, and a turn
         * flips state faster than 280 ms only on a barge-in.
         */
        s_from = s_to;
        s_to = want;
        s_blend_start_us = ctx->now_us;
    }

    float mix = 1.0f;
    if (s_from != s_to) {
        float elapsed_ms = (float)(ctx->now_us - s_blend_start_us) / 1000.0f;
        mix = elapsed_ms / BLEND_MS;
        if (mix >= 1.0f) {
            mix = 1.0f;
            s_from = s_to; /* landed; stop paying for the second evaluation */
        }
    }

    /*
     * A press has to show, because the button has no other affordance and a tap
     * during the session cooldown does nothing else visible. Lifting the whole
     * shell's amplitude is the same "event" grammar every behaviour already uses,
     * so it costs no new geometry -- and unlike the spectrum's cyan ring it does
     * not need a ring to tint.
     */
    float amp = ctx->amp;
    if (ctx->press_active) {
        amp = 1.0f;
    }

    /*
     * Split timing, because the first measurement of the whole was 25 ms against
     * a 2 ms estimate and the estimate assumed the cost was arithmetic. Geometry
     * is ~456 dots of scalar float; rasterising is ~20k read-modify-write pixel
     * blends scattered across a 434 kB PSRAM buffer. Those fail very differently,
     * so measure them apart before optimising either.
     */
#if ORB_LOG_TIMINGS
    const int64_t t_geom = esp_timer_get_time();
#endif
    const orb_bands_t bands = {
        .low = ctx->band_low,
        .mid = ctx->band_mid,
        .high = ctx->band_high,
    };
    /* The state picks the mode. Nothing overrides it: the display test scripts
     * the STATE, which is the same path a session takes. */
    orb_mode_t mode = mode_for(ctx->behaviour);

    /*
     * A CHANGE INVOLVING A MODE IS A CUT, not a crossfade.
     *
     * orb_build blends two shell behaviours per RING, which works because they
     * share a lattice and every value they produce is a pose on it. Two different
     * modes share nothing -- different lattices, different dot counts, and in
     * web's case lines as well -- so there is no pose between them to interpolate
     * towards. Blending them would mean building both frames and fading on alpha,
     * which doubles the geometry for the length of a transition; ribbon is ~15 ms
     * and web ~25 ms, so that is not obviously affordable.
     *
     * Cuts for now, logged once each so what a turn actually looks like is on the
     * record rather than a matter of opinion. The shell keeps its 280 ms blend
     * between its own behaviours.
     */
    static orb_mode_t s_mode_from = MODE_SHELL, s_mode_to = MODE_SHELL;
    static int64_t s_mode_blend_us;
    /* What the outgoing shell was showing, so it can be held while it fades. */
    static orb_behaviour_t s_mode_from_beh = ORB_DISCONNECTED;

    if (mode != s_mode_to) {
        /* From where we were HEADING, not from the blend showing right now --
         * the same choice the shell's own behaviour blend makes above. */
        s_mode_from = s_mode_to;
        s_mode_from_beh = s_from;
        s_mode_to = mode;
        s_mode_blend_us = ctx->now_us;
        ESP_LOGI("face_orb", "EVT mode %s->%s", mode_name(s_mode_from),
                 mode_name(mode));
    }

    float mode_mix = 1.0f;
    if (s_mode_from != s_mode_to) {
        float el_ms = (float)(ctx->now_us - s_mode_blend_us) / 1000.0f;
        mode_mix = el_ms / MODE_BLEND_MS;
        if (mode_mix >= 1.0f) {
            mode_mix = 1.0f;
            s_mode_from = s_mode_to; /* landed; stop paying for the second build */
        }
    }

    if (s_mode_from != s_mode_to && s_frame_b == NULL) {
        /* First transition of the boot pays for the second frame. */
        s_frame_b = heap_caps_malloc(sizeof(*s_frame_b), MALLOC_CAP_SPIRAM);
    }

    build_mode(mode, s_frame, t, amp, &bands, s_from, s_to, mix);

    /*
     * A cross-mode fade: draw BOTH animations and fade on alpha.
     *
     * Two modes cannot be interpolated the way two shell behaviours can -- there
     * is no pose between a lattice and a braid -- so the frame carries both dot
     * lists at complementary alpha and the eye does the blending. Costs a second
     * build for the seven or so frames a transition lasts.
     *
     * The outgoing side is built at a FIXED behaviour, captured when the mode
     * changed. Letting it follow ctx->behaviour would have it animating towards
     * the state it is being replaced by, which is the one thing it must not do.
     */
    if (s_mode_from != s_mode_to && s_frame_b != NULL) {
        build_mode(s_mode_from, s_frame_b, t, amp, &bands,
                   s_mode_from_beh, s_mode_from_beh, 1.0f);
        scale_alpha(s_frame, mode_mix);
        scale_alpha(s_frame_b, 1.0f - mode_mix);
        append_frame(s_frame, s_frame_b);
    }
#if ORB_LOG_TIMINGS
    const int64_t t_rast = esp_timer_get_time();
#endif
    /* Colour is the user's, resolved by ui.c. Nothing to latch or reset: it is a
     * pure draw parameter, so a change lands on the next frame by itself -- which
     * is also why activate() has nothing to say about it. */
    orb_raster_draw(s_frame, ctx->tint_rgb);

#if ORB_LOG_TIMINGS
    const int64_t t_end = esp_timer_get_time();
    static int64_t geom_sum, rast_sum;
    static uint32_t n;
    geom_sum += t_rast - t_geom;
    rast_sum += t_end - t_rast;
    if (++n >= 60) {
        ESP_LOGI("face_orb", "geometry %lld us, raster %lld us, %u dots, %u lines",
                 (long long)(geom_sum / n), (long long)(rast_sum / n),
                 (unsigned)s_frame->count, (unsigned)s_frame->line_count);
        geom_sum = rast_sum = 0;
        n = 0;
    }
#endif
}

const ui_face_t ui_face_orb = {
    .name = "orb",
    .init = init,
    .activate = activate,
    .feed_pcm = NULL, /* levels are enough; no raw samples wanted */
    .render = render,
};
