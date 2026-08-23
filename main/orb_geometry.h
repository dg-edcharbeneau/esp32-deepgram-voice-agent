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
    ORB_BEHAVIOUR_COUNT = 8,
} orb_behaviour_t;

/* 18 rings of cosine-tapered longitude counts. Not a 17x42 grid -- see
 * orb_init(). */
#define ORB_MAX_DOTS 456

typedef struct {
    float x, y;  /* screen pixels */
    float z;     /* depth; the list is sorted ascending on this */
    float r;     /* radius in pixels, already clamped to the floor */
    float white; /* ink, 0..1. On a dark ground the grey is (1 - white) * 255 */
    float a;     /* alpha, 0..1 */
} orb_dot_t;

typedef struct {
    orb_dot_t dots[ORB_MAX_DOTS];
    size_t count;
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
