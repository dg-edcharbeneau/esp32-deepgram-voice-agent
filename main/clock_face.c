/*
 * See clock_face.h. Each formula below is the mockup's, in the mockup's order,
 * so the two can be read side by side.
 */
#include "clock_face.h"

#include <math.h>

/* The sphere: steady yaw, fixed tilt, a faint breath. */
#define YAW_RATE     0.45f   /* rad/s */
#define TILT         0.38f   /* rad */
#define BREATH       0.015f
#define BREATH_RATE  1.2f

/* The meridian: a band of the near side lit as sin(0.9 t) sweeps across. */
#define SWEEP_RATE   0.9f
#define SWEEP_HALF   0.07f   /* half its width, in sphere radii */
#define SWEEP_GROW   1.4f    /* diameter added to a lit dot */

/* Rings: base diameters, the current dot's growth, the twinkle and the pulse. */
#define MIN_BASE     4.0f
#define HOUR_BASE    6.0f
#define CURRENT_GROW 7.0f
#define UNLIT_SHRINK 1.0f
#define UNLIT_A      0.18f
#define TWINKLE_A    0.62f
#define TWINKLE_AMP  0.3f
#define TWINKLE_RATE 2.2f
#define TWINKLE_STEP 1.7f    /* phase between neighbouring dots */
#define PULSE_RATE   3.0f
#define HALO_MIN     3.0f    /* spread, px */
#define HALO_GROW    6.0f
#define HALO_A_MIN   0.06f
#define HALO_A_GROW  0.28f

static void ring(clock_frame_t *f, int count, float radius, float base, int current,
                 float t, float ox, float oy, clock_halo_t *halo)
{
    for (int i = 0; i < count; i++) {
        const float ang = (float)i * 2.0f * (float)M_PI / (float)count;
        clock_dot_t *d = &f->dot[f->n++];
        d->x = CLOCK_FACE_C + ox + radius * sinf(ang);
        d->y = CLOCK_FACE_C + oy - radius * cosf(ang);
        d->kind = CLOCK_DOT_RING;
        float size;
        if (i == current) {
            const float pulse = (sinf(t * PULSE_RATE) + 1.0f) * 0.5f;
            size = base + CURRENT_GROW;
            d->a = 1.0f;
            halo->x = d->x;
            halo->y = d->y;
            halo->r_in = size * 0.5f;
            halo->r_out = size * 0.5f + HALO_MIN + pulse * HALO_GROW;
            halo->a = HALO_A_GROW * (1.0f - pulse) + HALO_A_MIN;
        } else if (i < current) {
            size = base;
            d->a = TWINKLE_A + TWINKLE_AMP * sinf(t * TWINKLE_RATE + (float)i * TWINKLE_STEP);
        } else {
            size = base - UNLIT_SHRINK;
            d->a = UNLIT_A;
        }
        d->r = size * 0.5f;
    }
}

/* In the mockup's order, which is the points' own: top pole to bottom. Its dots
 * are small and a little transparent, so where two overlap the order barely
 * shows, and copying it is the way to match the reference rather than argue
 * with it. */
void clock_face_sphere(clock_dot_t *out, float t, float cx, float cy, float radius)
{
    const float a = t * YAW_RATE;
    const float ca = cosf(a), sa = sinf(a), cb = cosf(TILT), sb = sinf(TILT);
    const float rr = radius * (1.0f + BREATH * sinf(t * BREATH_RATE));
    const float sweep = sinf(t * SWEEP_RATE);
    const float golden = (float)M_PI * (3.0f - sqrtf(5.0f));

    for (int i = 0; i < CLOCK_FACE_SPHERE; i++) {
        const float y = 1.0f - ((float)i / (float)(CLOCK_FACE_SPHERE - 1)) * 2.0f;
        const float r = sqrtf(fmaxf(0.0f, 1.0f - y * y));
        const float th = golden * (float)i;
        const float x = cosf(th) * r, z = sinf(th) * r;

        const float x1 = x * ca + z * sa, z1 = -x * sa + z * ca;
        const float y2 = y * cb - z1 * sb, z2 = y * sb + z1 * cb;
        const float depth = (z2 + 1.0f) * 0.5f;
        float op = 0.1f + 0.85f * powf(depth, 1.6f);
        float size = 1.6f + 2.4f * depth;
        if (z2 > 0.0f && fabsf(x1 - sweep) < SWEEP_HALF) {
            op = 1.0f;
            size += SWEEP_GROW;
        }
        out[i] = (clock_dot_t){
            .x = cx + x1 * rr,
            .y = cy + y2 * rr,
            .r = size * 0.5f,
            .a = op,
            .kind = CLOCK_DOT_SPHERE,
        };
    }
}

void clock_face_build(clock_frame_t *f, float t, int hh, int mm, float ox, float oy)
{
    f->n = 0;
    ring(f, CLOCK_FACE_MINUTES, CLOCK_FACE_MIN_R, MIN_BASE, mm, t, ox, oy, &f->halo[0]);
    ring(f, CLOCK_FACE_HOURS, CLOCK_FACE_HOUR_R, HOUR_BASE, hh % 12, t, ox, oy, &f->halo[1]);
    clock_face_sphere(&f->dot[f->n], t, CLOCK_FACE_C + ox, CLOCK_FACE_C + oy, CLOCK_FACE_SPHERE_R);
    f->n += CLOCK_FACE_SPHERE;
}

float clock_face_text_fade(float x, float y, float x0, float y0, float x1, float y1, float soft)
{
    const float dx = fmaxf(fmaxf(x0 - x, x - x1), 0.0f);
    const float dy = fmaxf(fmaxf(y0 - y, y - y1), 0.0f);
    const float d = sqrtf(dx * dx + dy * dy);
    if (d >= soft) {
        return 1.0f;
    }
    const float u = d / soft;
    return u * u * (3.0f - 2.0f * u);
}
