/*
 * Host checks for main/clock_face.c; see host/clock.sh. The expected values are
 * the mockup's (OrbClock.dc.html), worked by hand.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clock_face.h"

static int failures;

static void check(const char *what, int ok)
{
    printf("%-66s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        failures++;
    }
}

static clock_frame_t f;

static int near(float a, float b, float tol)
{
    return fabsf(a - b) <= tol;
}

/* Minutes are dots [0, 60), hours [60, 72), the sphere the rest. */
static void rings(void)
{
    clock_face_build(&f, 1.0f, 14, 38, 0.0f, 0.0f);
    check("frame: 60 minutes + 12 hours + 180 sphere dots", f.n == CLOCK_FACE_DOTS);
    check("minute 0 is at twelve, r 186", near(f.dot[0].x, 233, 0.01f) && near(f.dot[0].y, 47, 0.01f));
    check("minute 15 is at three", near(f.dot[15].x, 419, 0.01f) && near(f.dot[15].y, 233, 0.01f));
    check("hour 3 is at three, r 146", near(f.dot[63].x, 379, 0.01f) && near(f.dot[63].y, 233, 0.01f));

    /* 2:38 -> minutes 0..37 lit, 38 current, 39..59 unlit; hours 0..1 lit, 2 current. */
    int lit = 0, unlit = 0, ok = 1;
    for (int i = 0; i < 60; i++) {
        const clock_dot_t *d = &f.dot[i];
        if (i < 38) {
            lit += d->r == 2.0f && d->a >= 0.32f - 1e-4f && d->a <= 0.92f + 1e-4f;
        } else if (i == 38) {
            ok &= d->r == 5.5f && d->a == 1.0f;
        } else {
            unlit += d->r == 1.5f && d->a == 0.18f;
        }
    }
    check("2:38: 38 minutes lit and twinkling, 21 unlit", lit == 38 && unlit == 21);
    check("2:38: minute 38 is the big one, 11 px", ok);
    lit = unlit = 0;
    ok = 1;
    for (int i = 0; i < 12; i++) {
        const clock_dot_t *d = &f.dot[60 + i];
        if (i < 2) {
            lit += d->r == 3.0f;
        } else if (i == 2) {
            ok &= d->r == 6.5f && d->a == 1.0f;
        } else {
            unlit += d->r == 2.5f && d->a == 0.18f;
        }
    }
    check("14:38: hours 12 and 1 lit, 2 the big one, 3..11 unlit", lit == 2 && ok && unlit == 9);
    check("halos ring the current minute and hour", near(f.halo[0].x, f.dot[38].x, 0.01f) &&
                                                       near(f.halo[1].y, f.dot[62].y, 0.01f) &&
                                                       f.halo[0].r_in == 5.5f &&
                                                       f.halo[0].r_out >= 8.5f && f.halo[0].r_out <= 14.5f);

    clock_face_build(&f, 1.0f, 0, 0, 0.0f, 0.0f);
    lit = 0;
    for (int i = 0; i < 72; i++) {
        lit += f.dot[i].a > 0.18f;
    }
    check("00:00: only twelve on each ring is lit", lit == 2 && f.dot[0].r == 5.5f && f.dot[60].r == 6.5f);

    clock_face_build(&f, 1.0f, 12, 59, 0.0f, 0.0f);
    lit = 0;
    for (int i = 0; i < 60; i++) {
        lit += f.dot[i].a > 0.18f;
    }
    check("12:59: the whole minute ring is lit, hour twelve current", lit == 60 && f.dot[60].r == 6.5f);
}

static void sphere(void)
{
    int inside = 1, meridian_frames = 0, meridian_ok = 1;
    for (int k = 0; k < 200; k++) {
        const float t = (float)k * 0.2f;
        clock_face_build(&f, t, 9, 5, 0.0f, 0.0f);
        int lit = 0;
        for (int i = 72; i < f.n; i++) {
            const clock_dot_t *d = &f.dot[i];
            const float r = hypotf(d->x - 233.0f, d->y - 233.0f);
            inside &= r <= 96.0f * 1.015f + 0.01f;
            /* Off the meridian a dot is at most 4 px across; on it, 5.4 and opaque. */
            if (d->r > 2.0f + 1e-4f) {
                lit++;
                meridian_ok &= d->a == 1.0f;
            }
        }
        meridian_frames += lit > 0;
    }
    check("sphere: every dot within the 96 px radius and its breath", inside);
    check("meridian: lit dots are opaque and grown", meridian_ok);
    check("meridian: shows on most frames as it sweeps", meridian_frames > 100);
}

static void offsets(void)
{
    static clock_frame_t g;
    clock_face_build(&f, 2.5f, 7, 20, 0.0f, 0.0f);
    clock_face_build(&g, 2.5f, 7, 20, -6.0f, 3.0f);
    int ok = 1;
    for (int i = 0; i < f.n; i++) {
        ok &= near(g.dot[i].x, f.dot[i].x - 6.0f, 1e-3f) && near(g.dot[i].y, f.dot[i].y + 3.0f, 1e-3f);
    }
    check("burn-in step moves every dot by the same amount", ok);
}

static void fade(void)
{
    check("text fade: 0 inside the box", clock_face_text_fade(233, 233, 180, 210, 290, 250, 14) == 0.0f);
    check("text fade: 1 at the soft edge and beyond",
          clock_face_text_fade(233, 264, 180, 210, 290, 250, 14) == 1.0f &&
              clock_face_text_fade(100, 100, 180, 210, 290, 250, 14) == 1.0f);
    const float mid = clock_face_text_fade(233, 257, 180, 210, 290, 250, 14);
    check("text fade: in between, part way", mid > 0.0f && mid < 1.0f);
}

/* One frame as greys: each dot's opacity as ink, area-weighted the cheap way. */
static void dump(const char *dir)
{
    static unsigned char px[466 * 466];
    memset(px, 0, sizeof(px));
    clock_face_build(&f, 3.0f, 14, 38, 0.0f, 0.0f);
    for (int i = 0; i < f.n; i++) {
        const clock_dot_t *d = &f.dot[i];
        for (int y = (int)(d->y - d->r - 2); y <= (int)(d->y + d->r + 2); y++) {
            for (int x = (int)(d->x - d->r - 2); x <= (int)(d->x + d->r + 2); x++) {
                if (x < 0 || y < 0 || x >= 466 || y >= 466) {
                    continue;
                }
                const float dist = hypotf(x + 0.5f - d->x, y + 0.5f - d->y) - d->r;
                const float cov = fminf(fmaxf(0.5f - dist, 0.0f), 1.0f) * d->a;
                const int v = px[y * 466 + x] + (int)(cov * 255.0f);
                px[y * 466 + x] = (unsigned char)(v > 255 ? 255 : v);
            }
        }
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/clock-1438.pgm", dir);
    FILE *out = fopen(path, "wb");
    if (out == NULL) {
        perror(path);
        failures++;
        return;
    }
    fprintf(out, "P5\n466 466\n255\n");
    fwrite(px, 1, sizeof(px), out);
    fclose(out);
    printf("wrote %s\n", path);
}

int main(int argc, char **argv)
{
    rings();
    sphere();
    offsets();
    fade();
    if (argc > 1) {
        dump(argv[1]);
    }
    printf("\n%s\n", failures ? "FAILURES" : "all clock checks pass");
    return failures ? 1 : 0;
}
