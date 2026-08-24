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
    case UI_BEHAVIOUR_LISTENING:  return MODE_WAVE;   /* the user talking */
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

static orb_behaviour_t to_orb(ui_behaviour_t b)
{
    switch (b) {
    case UI_BEHAVIOUR_INITIALIZING: return ORB_INITIALIZING;
    case UI_BEHAVIOUR_LISTENING:    return ORB_LISTENING;
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
    int64_t t_geom = esp_timer_get_time();
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
    static orb_mode_t s_mode_shown = MODE_SHELL;
    if (mode != s_mode_shown) {
        ESP_LOGI("face_orb", "EVT mode %s->%s (cut)",
                 mode_name(s_mode_shown), mode_name(mode));
        s_mode_shown = mode;
    }

    switch (mode) {
    /* wave takes the microphone level: LISTENING is the user talking. */
    case MODE_WAVE:   orb_build_wave(s_frame, t, amp); break;
    case MODE_RUBIK:  orb_build_rubik(s_frame, t); break;
    /*
     * Kept, and currently unreachable: no state maps to MODE_RIBBON since
     * SPEAKING moved to the shell's pulse. The port stays because it is
     * parity-verified and cheap to point a state back at.
     */
    case MODE_RIBBON: orb_build_ribbon(s_frame, t, amp); break;
    case MODE_BRAID:  orb_build_braid(s_frame, t); break;
    case MODE_WEB:    orb_build_web(s_frame, t); break;
    default:
        orb_build(s_frame, t, s_from, s_to, mix, amp, &bands);
        break;
    }
    int64_t t_rast = esp_timer_get_time();
    /* Colour is the user's, resolved by ui.c. Nothing to latch or reset: it is a
     * pure draw parameter, so a change lands on the next frame by itself -- which
     * is also why activate() has nothing to say about it. */
    orb_raster_draw(s_frame, ctx->tint_rgb);
    int64_t t_end = esp_timer_get_time();

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
}

const ui_face_t ui_face_orb = {
    .name = "orb",
    .init = init,
    .activate = activate,
    .feed_pcm = NULL, /* levels are enough; no raw samples wanted */
    .render = render,
};
