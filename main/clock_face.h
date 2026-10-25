#pragma once

/*
 * The orb clock's moving parts, as numbers: a small dotted sphere with a
 * meridian sweeping it, and two rings of dots round it that light up to the
 * hour and the minute. What a frame shows, not how it is painted -- ui_clock.c
 * paints it. Plain C over libm, no LVGL, so host/clock.sh compiles it as is.
 *
 * Every constant is Ed's "Orb clock" mockup (OrbClock.dc.html in the design
 * export), at 1:1 device pixels. Sizes there are diameters; here they are
 * radii, halved once where they are named.
 */

#include <stdbool.h>
#include <stdint.h>

#define CLOCK_FACE_C        233.0f   /* centre, both axes */

#define CLOCK_FACE_SPHERE   180      /* Fibonacci points on the sphere */
#define CLOCK_FACE_SPHERE_R 96.0f
#define CLOCK_FACE_MINUTES  60
#define CLOCK_FACE_MIN_R    186.0f
#define CLOCK_FACE_HOURS    12
#define CLOCK_FACE_HOUR_R   146.0f
#define CLOCK_FACE_DOTS     (CLOCK_FACE_SPHERE + CLOCK_FACE_MINUTES + CLOCK_FACE_HOURS)

typedef enum {
    CLOCK_DOT_SPHERE = 0,   /* the mockup's #CFE3FF */
    CLOCK_DOT_RING,         /* the mockup's #F2F4F7 */
} clock_dot_kind_t;

typedef struct {
    float x, y;    /* centre, screen pixels */
    float r;       /* radius */
    float a;       /* opacity 0..1 */
    uint8_t kind;  /* clock_dot_kind_t */
} clock_dot_t;

/* The pulsing ring round the current hour and minute: an annulus outside the
 * dot (the mockup's box-shadow spread), drawn under it. */
typedef struct {
    float x, y;
    float r_in, r_out;
    float a;
} clock_halo_t;

typedef struct {
    /* In paint order, the mockup's: minutes, hours, then the sphere. */
    clock_dot_t dot[CLOCK_FACE_DOTS];
    int n;
    clock_halo_t halo[2];   /* [0] the minute, [1] the hour */
} clock_frame_t;

/*
 * One frame at `t` seconds since the clock appeared, for hh:mm (24-hour; the
 * hour ring wraps at 12), with everything moved by (ox, oy) -- the burn-in
 * step. The time drives only which dots are lit; `t` drives all the motion.
 */
void clock_face_build(clock_frame_t *f, float t, int hh, int mm, float ox, float oy);

/*
 * The sphere alone -- CLOCK_FACE_SPHERE dots into `out` -- centred at (cx, cy)
 * with `radius`: the clock's at CLOCK_FACE_SPHERE_R, the ring screen's smaller.
 * The dots keep their mockup sizes whatever the radius.
 */
void clock_face_sphere(clock_dot_t *out, float t, float cx, float cy, float radius);

/*
 * The mockup's black text-shadow, as a fade on the dots instead: 0 inside the
 * box (x0,y0)-(x1,y1), 1 from `soft` pixels outside it, smooth between. LVGL
 * text has no shadow, and the shadow's only job is to keep the sphere's dots
 * off the digits.
 */
float clock_face_text_fade(float x, float y, float x0, float y0, float x1, float y1, float soft);
