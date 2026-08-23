/*
 * Dump frames from the C port in the same TSV as orb_ref.mjs, so the two can be
 * diffed numerically. Host-only; not part of the firmware build.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_geometry.h"

typedef struct {
    const char *label;
    orb_behaviour_t from, to;
    float mix, t, amp;
} orb_case_t;

/* Must match CASES in orb_ref.mjs exactly, in order. */
static const orb_case_t CASES[] = {
    { "idle_a",        ORB_IDLE,         ORB_IDLE,         1.0f, 1.7f,  0.0f },
    { "idle_b",        ORB_IDLE,         ORB_IDLE,         1.0f, 5.5f,  0.0f },
    { "idle_c",        ORB_IDLE,         ORB_IDLE,         1.0f, 13.2f, 0.0f },
    { "idle_d",        ORB_IDLE,         ORB_IDLE,         1.0f, 20.9f, 0.0f },
    { "initializing",  ORB_INITIALIZING, ORB_INITIALIZING, 1.0f, 1.7f, 0.0f },
    { "listening",     ORB_LISTENING,    ORB_LISTENING,    1.0f, 1.7f, 0.0f },
    { "listening_amp", ORB_LISTENING,    ORB_LISTENING,    1.0f, 3.3f, 0.8f },
    { "thinking",      ORB_THINKING,     ORB_THINKING,     1.0f, 1.7f, 0.0f },
    { "speaking",      ORB_SPEAKING,     ORB_SPEAKING,     1.0f, 1.7f, 0.0f },
    { "speaking_amp",  ORB_SPEAKING,     ORB_SPEAKING,     1.0f, 3.3f, 0.8f },
    { "connecting",    ORB_CONNECTING,   ORB_CONNECTING,   1.0f, 1.7f, 0.0f },
    { "buffering",     ORB_BUFFERING,    ORB_BUFFERING,    1.0f, 1.7f, 0.0f },
    { "disconnected",  ORB_DISCONNECTED, ORB_DISCONNECTED, 1.0f, 1.7f, 0.0f },
    { "blend_l2s",     ORB_LISTENING,    ORB_SPEAKING,     0.4f, 2.5f, 0.6f },
};

/*
 * Wave has no behaviour, mix or amplitude, so it gets its own table rather than
 * dead columns in the one above. Must match WAVE_CASES in orb_ref.mjs, in order.
 */
typedef struct {
    const char *label;
    float t;
} wave_case_t;

static const wave_case_t WAVE_CASES[] = {
    { "wave_a", 1.7f },
    { "wave_b", 3.3f },
    { "wave_c", 5.5f },
    { "wave_d", 13.2f },
};

/* Must match WEB_CASES in orb_ref.mjs, in order. */
static const wave_case_t WEB_CASES[] = {
    { "web_a", 1.7f },
    { "web_b", 5.4f },
    { "web_c", 13.1f },
};

/* Must match BRAID_CASES in orb_ref.mjs, in order. */
static const wave_case_t BRAID_CASES[] = {
    { "braid_a", 1.7f },
    { "braid_b", 6.2f },
    { "braid_c", 14.5f },
};

/* Must match RIBBON_CASES in orb_ref.mjs, in order. */
typedef struct {
    const char *label;
    float t, amp;
} ribbon_case_t;

static const ribbon_case_t RIBBON_CASES[] = {
    { "ribbon_a", 1.7f,  0.0f },
    { "ribbon_b", 4.6f,  0.0f },
    { "ribbon_c", 11.9f, 0.0f },
    { "ribbon_amp", 4.6f, 0.8f },
};

/* Must match RUBIK_CASES in orb_ref.mjs, in order. */
static const wave_case_t RUBIK_CASES[] = {
    { "rubik_a", 1.7f },
    { "rubik_b", 5.0f },
    { "rubik_c", 9.3f },
    { "rubik_d", 12.4f },
};

int main(int argc, char **argv)
{
    float size = (argc > 1) ? strtof(argv[1], NULL) : 466.0f;
    orb_init(size);

    static orb_frame_t frame;

    /* Wave first, matching orb_ref.mjs's emission order. */
    for (size_t c = 0; c < sizeof(WAVE_CASES) / sizeof(WAVE_CASES[0]); c++) {
        const wave_case_t *k = &WAVE_CASES[c];
        orb_build_wave(&frame, k->t);
        for (size_t i = 0; i < frame.count; i++) {
            const orb_dot_t *d = &frame.dots[i];
            printf("%s\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\n",
                   k->label, d->x, d->y, d->z, d->r, d->white, d->a);
        }
    }

    for (size_t c = 0; c < sizeof(RUBIK_CASES) / sizeof(RUBIK_CASES[0]); c++) {
        const wave_case_t *k = &RUBIK_CASES[c];
        orb_build_rubik(&frame, k->t);
        for (size_t i = 0; i < frame.count; i++) {
            const orb_dot_t *d = &frame.dots[i];
            printf("%s\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\n",
                   k->label, d->x, d->y, d->z, d->r, d->white, d->a);
        }
    }

    for (size_t c = 0; c < sizeof(RIBBON_CASES) / sizeof(RIBBON_CASES[0]); c++) {
        const ribbon_case_t *k = &RIBBON_CASES[c];
        orb_build_ribbon(&frame, k->t, k->amp);
        for (size_t i = 0; i < frame.count; i++) {
            const orb_dot_t *d = &frame.dots[i];
            printf("%s\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\n",
                   k->label, d->x, d->y, d->z, d->r, d->white, d->a);
        }
    }

    for (size_t c = 0; c < sizeof(WEB_CASES) / sizeof(WEB_CASES[0]); c++) {
        const wave_case_t *k = &WEB_CASES[c];
        orb_build_web(&frame, k->t);
        for (size_t i = 0; i < frame.count; i++) {
            const orb_dot_t *d = &frame.dots[i];
            printf("%s\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\n",
                   k->label, d->x, d->y, d->z, d->r, d->white, d->a);
        }
        for (size_t i = 0; i < frame.line_count; i++) {
            const orb_line_t *l = &frame.lines[i];
            printf("%s~L\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\n",
                   k->label, l->x1, l->y1, l->x2, l->y2, l->white, l->a, l->w);
        }
    }

    for (size_t c = 0; c < sizeof(BRAID_CASES) / sizeof(BRAID_CASES[0]); c++) {
        const wave_case_t *k = &BRAID_CASES[c];
        orb_build_braid(&frame, k->t);
        for (size_t i = 0; i < frame.count; i++) {
            const orb_dot_t *d = &frame.dots[i];
            printf("%s\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\n",
                   k->label, d->x, d->y, d->z, d->r, d->white, d->a);
        }
    }

    for (size_t c = 0; c < sizeof(CASES) / sizeof(CASES[0]); c++) {
        const orb_case_t *k = &CASES[c];
        /*
         * No bands, and nothing here covers voice_pass().
         *
         * This harness verifies a TRANSCRIPTION, and the band pass is not
         * transcribed from anything -- the reference's buildVoice takes no band
         * input at all, so there is no upstream frame to diff it against. It
         * composes over the finished dot list precisely so it can stay out of
         * the way of what is checked here.
         */
        orb_build(&frame, k->t, k->from, k->to, k->mix, k->amp, NULL);
        for (size_t i = 0; i < frame.count; i++) {
            const orb_dot_t *d = &frame.dots[i];
            printf("%s\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\n",
                   k->label, d->x, d->y, d->z, d->r, d->white, d->a);
        }
    }
    return 0;
}
