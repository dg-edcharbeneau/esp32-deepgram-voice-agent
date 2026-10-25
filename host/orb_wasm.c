/*
 * The orb geometry as a WebAssembly module, for host/orb_web.html.
 *
 * Glue only: orb_geometry.c compiles unchanged, so the page animates the same
 * float maths the board runs rather than a transcription of it. What face_orb.c
 * does with the frames -- which mode a state uses, the 280 ms blends -- lives in
 * the page, and orb_raster.c's ink rules are redone there on a canvas.
 *
 * Two frames because a cross-mode fade draws two animations at once. Host-only;
 * not part of the firmware build. See orb_web.sh.
 */
#include <emscripten.h>

#include "orb_geometry.h"

static orb_frame_t s_frames[2];

EMSCRIPTEN_KEEPALIVE int orbw_init(float size)
{
    return orb_init(size) ? 1 : 0;
}

/* One shell frame: a behaviour blend, the level, the bands, and the turn the
 * listening fill takes. Same calls, same order, as face_orb.c's build_mode. */
EMSCRIPTEN_KEEPALIVE void orbw_shell(int i, float t, int from, int to, float mix,
                                     float amp, float low, float mid, float high,
                                     float rotate_deg)
{
    const orb_bands_t bands = { .low = low, .mid = mid, .high = high };
    orb_build(&s_frames[i], t, (orb_behaviour_t)from, (orb_behaviour_t)to, mix, amp,
              &bands);
    orb_rotate(&s_frames[i], rotate_deg);
}

/* 1 rubik, 2 braid, 3 web: face_orb.c's MODE_* numbering. */
EMSCRIPTEN_KEEPALIVE void orbw_mode(int i, int mode, float t)
{
    switch (mode) {
    case 1: orb_build_rubik(&s_frames[i], t); break;
    case 2: orb_build_braid(&s_frames[i], t); break;
    case 3: orb_build_web(&s_frames[i], t); break;
    default: s_frames[i].count = s_frames[i].line_count = 0; break;
    }
}

EMSCRIPTEN_KEEPALIVE const orb_dot_t *orbw_dots(int i) { return s_frames[i].dots; }
EMSCRIPTEN_KEEPALIVE int orbw_count(int i) { return (int)s_frames[i].count; }
EMSCRIPTEN_KEEPALIVE const orb_line_t *orbw_lines(int i) { return s_frames[i].lines; }
EMSCRIPTEN_KEEPALIVE int orbw_line_count(int i) { return (int)s_frames[i].line_count; }
