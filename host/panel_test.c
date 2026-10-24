/*
 * Exercises main/panel_geom.c: that every control is hit where it is drawn, that
 * the 36 px gaps and the rounded corners hit nothing, and that the colour
 * picker's clock positions map onto orb_colors.c's thirteen indices and back.
 *
 * With an argument it also writes the four page shapes as PGM images into that
 * directory -- white where a control paints, grey on its anti-aliased edge --
 * for looking at the geometry without a board.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "panel_geom.h"

#define COLORS 13   /* orb_colors.c */

static int failures;

static void check(const char *what, int ok)
{
    printf("%-66s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        failures++;
    }
}

static int hit(panel_page_t page, float x, float y)
{
    return panel_hit(page, x, y);
}

static void sound(void)
{
    const float c = PANEL_C;
    check("sound: upper-left quarter is mic +", hit(PANEL_PAGE_SOUND, c - 90, c - 90) == PANEL_HIT_MIC_UP);
    check("sound: lower-left quarter is mic -", hit(PANEL_PAGE_SOUND, c - 90, c + 90) == PANEL_HIT_MIC_DOWN);
    check("sound: upper-right quarter is volume +", hit(PANEL_PAGE_SOUND, c + 90, c - 90) == PANEL_HIT_VOL_UP);
    check("sound: lower-right quarter is volume -", hit(PANEL_PAGE_SOUND, c + 90, c + 90) == PANEL_HIT_VOL_DOWN);
    check("sound: the centre of the cross hits nothing", hit(PANEL_PAGE_SOUND, c, c) == PANEL_HIT_NONE);
    check("sound: inside the vertical gap hits nothing", hit(PANEL_PAGE_SOUND, c + 10, c - 120) == PANEL_HIT_NONE);
    check("sound: the bridge (the value) is not a button", hit(PANEL_PAGE_SOUND, c + 120, c - 10) == PANEL_HIT_NONE);
    check("sound: the bridge's top edge is not a button", hit(PANEL_PAGE_SOUND, c - 100, c - PANEL_BRIDGE + 1) == PANEL_HIT_NONE);
    check("sound: just above the bridge is +", hit(PANEL_PAGE_SOUND, c - 100, c - PANEL_BRIDGE - 1) == PANEL_HIT_MIC_UP);
    check("sound: just below the bridge is -", hit(PANEL_PAGE_SOUND, c + 100, c + PANEL_BRIDGE + 1) == PANEL_HIT_VOL_DOWN);
    check("sound: just past the gap is inside", hit(PANEL_PAGE_SOUND, c + 19, c - 120) == PANEL_HIT_VOL_UP);
    check("sound: outside the disc hits nothing", hit(PANEL_PAGE_SOUND, c + 150, c - 150) == PANEL_HIT_NONE);
    check("sound: the panel corner hits nothing", hit(PANEL_PAGE_SOUND, 5, 5) == PANEL_HIT_NONE);
    /* Where the vertical chord meets the arc, the 8 px rounding takes the tip. */
    const float chord_y = c - sqrtf(PANEL_R * PANEL_R - PANEL_GAP * PANEL_GAP);
    check("sound: the chord/arc corner tip is rounded off",
          hit(PANEL_PAGE_SOUND, c + PANEL_GAP + 1, chord_y + 1) == PANEL_HIT_NONE);
    check("sound: a few px in from that corner is inside",
          hit(PANEL_PAGE_SOUND, c + PANEL_GAP + 6, chord_y + 10) == PANEL_HIT_VOL_UP);
}

static void halves(void)
{
    const float c = PANEL_C;
    check("toggles: top half is noise reduction", hit(PANEL_PAGE_TOGGLES, c, c - 100) == PANEL_HIT_NS);
    check("toggles: bottom half is clock format", hit(PANEL_PAGE_TOGGLES, c, c + 100) == PANEL_HIT_CLOCK_FMT);
    check("toggles: the gap hits nothing", hit(PANEL_PAGE_TOGGLES, c, c) == PANEL_HIT_NONE);
    check("toggles: the far left of the gap hits nothing", hit(PANEL_PAGE_TOGGLES, c - 190, c + 5) == PANEL_HIT_NONE);
    /* Where the chord meets the arc, the 8 px rounding takes the sharp tip. */
    const float edge_x = c - sqrtf(PANEL_R * PANEL_R - PANEL_GAP * PANEL_GAP);
    check("toggles: the chord/arc corner tip is rounded off",
          hit(PANEL_PAGE_TOGGLES, edge_x + 1, c - PANEL_GAP - 1) == PANEL_HIT_NONE);
    check("toggles: the top of the disc is inside", hit(PANEL_PAGE_TOGGLES, c, c - PANEL_R + 2) == PANEL_HIT_NS);
    check("toggles: past the disc rim is outside", hit(PANEL_PAGE_TOGGLES, c, c - PANEL_R - 2) == PANEL_HIT_NONE);

    check("alarms: bottom half is cancel", hit(PANEL_PAGE_ALARMS, c, c + 100) == PANEL_HIT_ALARM_CANCEL);
    check("alarms: the left circle is previous",
          hit(PANEL_PAGE_ALARMS, c - PANEL_NAV_X, PANEL_NAV_Y) == PANEL_HIT_ALARM_PREV);
    check("alarms: the right circle is next",
          hit(PANEL_PAGE_ALARMS, c + PANEL_NAV_X, PANEL_NAV_Y) == PANEL_HIT_ALARM_NEXT);
    check("alarms: the text in the top half hits nothing", hit(PANEL_PAGE_ALARMS, c, c - 60) == PANEL_HIT_NONE);
    /* The circles must not reach into the cancel button or the disc's rim. */
    check("alarms: nav circles clear the cancel button",
          PANEL_NAV_Y + PANEL_NAV_R + PANEL_HIT_SLOP < PANEL_C + PANEL_GAP);
    check("alarms: nav circles sit inside the panel",
          sqrtf(PANEL_NAV_X * PANEL_NAV_X + (PANEL_C - PANEL_NAV_Y) * (PANEL_C - PANEL_NAV_Y)) +
                  PANEL_NAV_R < PANEL_C);
}

static void colours(void)
{
    char what[96];
    /* Every clock position hits itself at its centre and at the slop edge. */
    int all = 1;
    for (int s = 0; s < PANEL_SLOTS; s++) {
        float x, y;
        panel_slot_center(s, &x, &y);
        if (hit(PANEL_PAGE_COLOR, x, y) != PANEL_HIT_SLOT + s) all = 0;
        if (hit(PANEL_PAGE_COLOR, x + PANEL_SWATCH_R + PANEL_HIT_SLOP - 1, y) != PANEL_HIT_SLOT + s) all = 0;
    }
    check("colour: each clock position hits itself, slop included", all);
    check("colour: the centre (name and hex) hits nothing", hit(PANEL_PAGE_COLOR, PANEL_C, PANEL_C) == PANEL_HIT_NONE);

    float x0, y0, x1, y1;
    panel_slot_center(0, &x0, &y0);
    panel_slot_center(1, &x1, &y1);
    const float apart = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    check("colour: neighbours' hit circles do not overlap", apart > 2 * (PANEL_SWATCH_R + PANEL_HIT_SLOP));
    check("colour: twelve o'clock is straight up", fabsf(x0 - PANEL_C) < 0.01f && y0 < PANEL_C);
    check("colour: swatches sit inside the panel", PANEL_RING_R + PANEL_SWATCH_R + 3 < PANEL_C);

    /* Page A: 0..5 at slots 0..5, "+2" at six, 6..10 at slots 7..11. */
    check("colour: A twelve o'clock is white (0)", panel_color_for_slot(0, 0, COLORS) == 0);
    check("colour: A five o'clock is lime (5)", panel_color_for_slot(0, 5, COLORS) == 5);
    check("colour: A six o'clock is the more button", panel_color_for_slot(0, 6, COLORS) == PANEL_SLOT_IS_MORE);
    check("colour: A seven o'clock is green (6)", panel_color_for_slot(0, 7, COLORS) == 6);
    check("colour: A eleven o'clock is indigo (10)", panel_color_for_slot(0, 11, COLORS) == 10);
    check("colour: B twelve o'clock is purple (11)", panel_color_for_slot(1, 0, COLORS) == 11);
    check("colour: B one o'clock is pink (12)", panel_color_for_slot(1, 1, COLORS) == 12);
    check("colour: B two o'clock is empty", panel_color_for_slot(1, 2, COLORS) == -1);
    check("colour: B six o'clock is the back button", panel_color_for_slot(1, 6, COLORS) == PANEL_SLOT_IS_MORE);

    int round_trip = 1;
    for (int i = 0; i < COLORS; i++) {
        int page, slot;
        if (!panel_slot_for_color(i, COLORS, &page, &slot) ||
            panel_color_for_slot(page, slot, COLORS) != i) {
            round_trip = 0;
            snprintf(what, sizeof(what), "colour: index %d does not round-trip", i);
            check(what, 0);
        }
    }
    check("colour: all 13 indices round-trip through their slot", round_trip);
    int page, slot;
    check("colour: an index past the table has no slot", !panel_slot_for_color(COLORS, COLORS, &page, &slot));
}

static void coverage(void)
{
    check("coverage: deep inside is fully covered", panel_coverage(-3.0f) == 1.0f);
    check("coverage: well outside is empty", panel_coverage(3.0f) == 0.0f);
    check("coverage: on the edge is half", fabsf(panel_coverage(0.0f) - 0.5f) < 1e-6f);

    /* The bounding box must hold every pixel the shape touches. */
    static const panel_seg_t segs[] = {
        PANEL_SEG_HALF_TOP, PANEL_SEG_HALF_BOTTOM, PANEL_SEG_QUAD_TL,
        PANEL_SEG_QUAD_TR, PANEL_SEG_QUAD_BL, PANEL_SEG_QUAD_BR,
        PANEL_SEG_HALF_LEFT, PANEL_SEG_HALF_RIGHT,
    };
    int contained = 1;
    for (size_t k = 0; k < sizeof(segs) / sizeof(segs[0]); k++) {
        int bx0, by0, bx1, by1;
        panel_seg_bbox(segs[k], &bx0, &by0, &bx1, &by1);
        for (int y = 0; y < PANEL_SIZE; y++) {
            for (int x = 0; x < PANEL_SIZE; x++) {
                const float cov = panel_coverage(panel_seg_sdf(segs[k], x + 0.5f, y + 0.5f));
                if (cov > 0.0f && (x < bx0 || x > bx1 || y < by0 || y > by1)) {
                    contained = 0;
                }
            }
        }
    }
    check("coverage: every segment stays inside its bounding box", contained);

    /* Segments never overlap, so painting them in any order is the same. */
    int disjoint = 1;
    for (int y = 0; y < PANEL_SIZE; y += 2) {
        for (int x = 0; x < PANEL_SIZE; x += 2) {
            int n = 0;
            for (size_t k = 2; k < 6; k++) {
                n += panel_seg_sdf(segs[k], x + 0.5f, y + 0.5f) < 0.5f;
            }
            if (n > 1) disjoint = 0;
        }
    }
    check("coverage: quarter segments never overlap", disjoint);

    /*
     * ui_panel.c paints in 8 x 8 tiles and trusts one sample at a tile's centre:
     * further than 7 px inside means every pixel is solid, further than 7 px
     * outside means none is touched. That holds only if the distance functions
     * never change faster than distance -- check it pixel by pixel, every tile.
     */
    int tiles_ok = 1, solid = 0, empty = 0, edge = 0;
    for (size_t k = 0; k < sizeof(segs) / sizeof(segs[0]); k++) {
        int bx0, by0, bx1, by1;
        panel_seg_bbox(segs[k], &bx0, &by0, &bx1, &by1);
        for (int ty = by0; ty <= by1; ty += 8) {
            const int ty1 = (ty + 7 < by1) ? ty + 7 : by1;
            for (int tx = bx0; tx <= bx1; tx += 8) {
                const int tx1 = (tx + 7 < bx1) ? tx + 7 : bx1;
                const float d = panel_seg_sdf(segs[k], (float)(tx + tx1 + 1) * 0.5f,
                                              (float)(ty + ty1 + 1) * 0.5f);
                if (d >= -7.0f && d <= 7.0f) {
                    edge++;
                    continue;
                }
                d < 0 ? solid++ : empty++;
                for (int y = ty; y <= ty1; y++) {
                    for (int x = tx; x <= tx1; x++) {
                        const float c = panel_coverage(panel_seg_sdf(segs[k], x + 0.5f, y + 0.5f));
                        if ((d < 0 && c < 1.0f) || (d > 0 && c > 0.0f)) {
                            tiles_ok = 0;
                        }
                    }
                }
            }
        }
    }
    check("coverage: the tile shortcut agrees with every pixel", tiles_ok);

    /* ui_panel.c keeps one mask per shape and mirrors it: pixel p maps to
     * PANEL_SIZE - 1 - p. That is only right if the shapes really are mirrors. */
    int mirrored = 1;
    for (int y = 0; y < PANEL_SIZE; y += 3) {
        for (int x = 0; x < PANEL_SIZE; x += 3) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float mx = (PANEL_SIZE - 1 - x) + 0.5f, my = (PANEL_SIZE - 1 - y) + 0.5f;
            const float tr = panel_seg_sdf(PANEL_SEG_QUAD_TR, px, py);
            if (fabsf(tr - panel_seg_sdf(PANEL_SEG_QUAD_TL, mx, py)) > 1e-3f ||
                fabsf(tr - panel_seg_sdf(PANEL_SEG_QUAD_BR, px, my)) > 1e-3f ||
                fabsf(tr - panel_seg_sdf(PANEL_SEG_QUAD_BL, mx, my)) > 1e-3f ||
                fabsf(panel_seg_sdf(PANEL_SEG_HALF_TOP, px, py) -
                      panel_seg_sdf(PANEL_SEG_HALF_BOTTOM, px, my)) > 1e-3f) {
                mirrored = 0;
            }
        }
    }
    check("coverage: quarters and halves are exact mirrors of one shape each", mirrored);

    /* The side-by-side halves are painted from the top half's mask, transposed
     * (and mirrored for the right): pixel (x, y) reads the mask at (y, x). */
    int transposed = 1;
    for (int y = 0; y < PANEL_SIZE; y += 3) {
        for (int x = 0; x < PANEL_SIZE; x += 3) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float mx = (PANEL_SIZE - 1 - x) + 0.5f;
            const float top = panel_seg_sdf(PANEL_SEG_HALF_TOP, py, px);
            if (fabsf(panel_seg_sdf(PANEL_SEG_HALF_LEFT, px, py) - top) > 1e-3f ||
                fabsf(panel_seg_sdf(PANEL_SEG_HALF_RIGHT, mx, py) - top) > 1e-3f) {
                transposed = 0;
            }
        }
    }
    check("coverage: the side-by-side halves are the top half transposed", transposed);
    printf("  (tiles: %d solid, %d empty, %d done per pixel)\n", solid, empty, edge);
}

/* Write a page's shapes as an 8-bit PGM, for eyeballing the geometry. */
static void dump(const char *dir, const char *name, const panel_seg_t *segs, int nsegs,
                 int swatches, int nav)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/panel_%s.pgm", dir, name);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        perror(path);
        return;
    }
    fprintf(f, "P5\n%d %d\n255\n", PANEL_SIZE, PANEL_SIZE);
    for (int y = 0; y < PANEL_SIZE; y++) {
        for (int x = 0; x < PANEL_SIZE; x++) {
            const float px = x + 0.5f, py = y + 0.5f;
            float cov = 0.0f;
            for (int k = 0; k < nsegs; k++) {
                cov = fmaxf(cov, panel_coverage(panel_seg_sdf(segs[k], px, py)));
            }
            for (int s = 0; s < (swatches ? PANEL_SLOTS : 0); s++) {
                float cx, cy;
                panel_slot_center(s, &cx, &cy);
                cov = fmaxf(cov, panel_coverage(panel_circle_sdf(cx, cy, PANEL_SWATCH_R, px, py)));
            }
            if (nav) {
                cov = fmaxf(cov, panel_coverage(panel_circle_sdf(PANEL_C - PANEL_NAV_X, PANEL_NAV_Y,
                                                                  PANEL_NAV_R, px, py)));
                cov = fmaxf(cov, panel_coverage(panel_circle_sdf(PANEL_C + PANEL_NAV_X, PANEL_NAV_Y,
                                                                  PANEL_NAV_R, px, py)));
            }
            /* The physical panel's own edge, faintly, for orientation. */
            const float rim = panel_coverage(fabsf(panel_circle_sdf(PANEL_C, PANEL_C, PANEL_C - 1, px, py)) - 0.5f);
            fputc((int)(fmaxf(cov * 255.0f, rim * 60.0f)), f);
        }
    }
    fclose(f);
    printf("wrote %s\n", path);
}

int main(int argc, char **argv)
{
    sound();
    halves();
    colours();
    coverage();

    if (argc > 1) {
        const panel_seg_t quads[] = {PANEL_SEG_HALF_LEFT, PANEL_SEG_HALF_RIGHT};
        const panel_seg_t halves_[] = {PANEL_SEG_HALF_TOP, PANEL_SEG_HALF_BOTTOM};
        const panel_seg_t bottom[] = {PANEL_SEG_HALF_BOTTOM};
        dump(argv[1], "sound", quads, 2, 0, 0);
        dump(argv[1], "colour", NULL, 0, 1, 0);
        dump(argv[1], "toggles", halves_, 2, 0, 0);
        dump(argv[1], "alarms", bottom, 1, 0, 1);
    }

    printf("\n%s\n", failures ? "FAILURES" : "all panel checks pass");
    return failures ? 1 : 0;
}
