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

int main(int argc, char **argv)
{
    float size = (argc > 1) ? strtof(argv[1], NULL) : 466.0f;
    orb_init(size);

    static orb_frame_t frame;
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
