/*
 * Exercises main/panel_geom.c: that every control is hit where it is drawn, that
 * the 36 px gaps and the rounded corners hit nothing, and that the colour
 * picker's dial holds exactly orb_colors.c's catalog, which is linked in.
 *
 * Built once per panel size (host/panel.sh): probe points that are the design's
 * own offsets go through PXF(); the one- and two-pixel edge probes do not.
 *
 * With an argument it also writes the four page shapes as PGM images into that
 * directory -- white where a control paints, grey on its anti-aliased edge --
 * for looking at the geometry without a board.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_colors.h"
#include "panel_geom.h"
#include "ui_tone.h"

/* The catalog's one cJSON use, orb_colors_add_enum(): nothing here calls it. */
cJSON *cJSON_AddArrayToObject(cJSON *parent, const char *key) { (void)parent; (void)key; return NULL; }
cJSON *cJSON_CreateString(const char *s) { (void)s; return NULL; }
int cJSON_AddItemToArray(cJSON *array, cJSON *item) { (void)array; (void)item; return 0; }

/* ui_panel.c's: a check on a swatch at least this bright is dark, else white. */
#define CHECK_DARK_LUMA 120

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
    check("sound: upper-left quarter is mic +", hit(PANEL_PAGE_SOUND, c - PXF(90), c - PXF(90)) == PANEL_HIT_MIC_UP);
    check("sound: lower-left quarter is mic -", hit(PANEL_PAGE_SOUND, c - PXF(90), c + PXF(90)) == PANEL_HIT_MIC_DOWN);
    check("sound: upper-right quarter is volume +", hit(PANEL_PAGE_SOUND, c + PXF(90), c - PXF(90)) == PANEL_HIT_VOL_UP);
    check("sound: lower-right quarter is volume -", hit(PANEL_PAGE_SOUND, c + PXF(90), c + PXF(90)) == PANEL_HIT_VOL_DOWN);
    check("sound: the centre of the cross hits nothing", hit(PANEL_PAGE_SOUND, c, c) == PANEL_HIT_NONE);
    check("sound: inside the vertical gap hits nothing", hit(PANEL_PAGE_SOUND, c + PXF(10), c - PXF(120)) == PANEL_HIT_NONE);
    check("sound: the bridge (the value) is not a button", hit(PANEL_PAGE_SOUND, c + PXF(120), c - PXF(10)) == PANEL_HIT_NONE);
    check("sound: the bridge's top edge is not a button", hit(PANEL_PAGE_SOUND, c - PXF(100), c - PANEL_BRIDGE + 1) == PANEL_HIT_NONE);
    check("sound: just above the bridge is +", hit(PANEL_PAGE_SOUND, c - PXF(100), c - PANEL_BRIDGE - 1) == PANEL_HIT_MIC_UP);
    check("sound: just below the bridge is -", hit(PANEL_PAGE_SOUND, c + PXF(100), c + PANEL_BRIDGE + 1) == PANEL_HIT_VOL_DOWN);
    check("sound: just past the gap is inside", hit(PANEL_PAGE_SOUND, c + PXF(19), c - PXF(120)) == PANEL_HIT_VOL_UP);
    check("sound: outside the disc hits nothing", hit(PANEL_PAGE_SOUND, c + PXF(150), c - PXF(150)) == PANEL_HIT_NONE);
    check("sound: the panel corner hits nothing", hit(PANEL_PAGE_SOUND, 5, 5) == PANEL_HIT_NONE);
    /* Where the vertical chord meets the arc, the 8 px rounding takes the tip. */
    const float chord_y = c - sqrtf(PANEL_R * PANEL_R - PANEL_GAP * PANEL_GAP);
    check("sound: the chord/arc corner tip is rounded off",
          hit(PANEL_PAGE_SOUND, c + PANEL_GAP + 1, chord_y + 1) == PANEL_HIT_NONE);
    check("sound: a few px in from that corner is inside",
          hit(PANEL_PAGE_SOUND, c + PANEL_GAP + PXF(6), chord_y + PXF(10)) == PANEL_HIT_VOL_UP);
}

static void halves(void)
{
    const float c = PANEL_C;
    check("toggles: top half is noise reduction", hit(PANEL_PAGE_TOGGLES, c, c - PXF(100)) == PANEL_HIT_NS);
    check("toggles: bottom half is clock format", hit(PANEL_PAGE_TOGGLES, c, c + PXF(100)) == PANEL_HIT_CLOCK_FMT);
    check("toggles: the gap hits nothing", hit(PANEL_PAGE_TOGGLES, c, c) == PANEL_HIT_NONE);
    check("toggles: the far left of the gap hits nothing", hit(PANEL_PAGE_TOGGLES, c - PXF(190), c + PXF(5)) == PANEL_HIT_NONE);
    /* Where the chord meets the arc, the 8 px rounding takes the sharp tip. */
    const float edge_x = c - sqrtf(PANEL_R * PANEL_R - PANEL_GAP * PANEL_GAP);
    check("toggles: the chord/arc corner tip is rounded off",
          hit(PANEL_PAGE_TOGGLES, edge_x + 1, c - PANEL_GAP - 1) == PANEL_HIT_NONE);
    check("toggles: the top of the disc is inside", hit(PANEL_PAGE_TOGGLES, c, c - PANEL_R + 2) == PANEL_HIT_NS);
    check("toggles: past the disc rim is outside", hit(PANEL_PAGE_TOGGLES, c, c - PANEL_R - 2) == PANEL_HIT_NONE);

    check("alarms: bottom half is cancel", hit(PANEL_PAGE_ALARMS, c, c + PXF(100)) == PANEL_HIT_ALARM_CANCEL);
    check("alarms: the left circle is previous",
          hit(PANEL_PAGE_ALARMS, c - PANEL_NAV_X, PANEL_NAV_Y) == PANEL_HIT_ALARM_PREV);
    check("alarms: the right circle is next",
          hit(PANEL_PAGE_ALARMS, c + PANEL_NAV_X, PANEL_NAV_Y) == PANEL_HIT_ALARM_NEXT);
    check("alarms: the text in the top half hits nothing", hit(PANEL_PAGE_ALARMS, c, c - PXF(60)) == PANEL_HIT_NONE);
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

    /* One dial: a slot per colour, evenly spaced, Fuchsia at twelve. */
    check("colour: the dial has a slot for every colour, and no more",
          (int)orb_colors_count() == PANEL_SLOTS);
    check("colour: twelve o'clock is fuchsia, the default",
          strcmp(orb_colors_name(0), "fuchsia") == 0);
    int even = 1;
    for (int s = 0; s < PANEL_SLOTS; s++) {
        float xa, ya, xb, yb;
        panel_slot_center(s, &xa, &ya);
        panel_slot_center((s + 1) % PANEL_SLOTS, &xb, &yb);
        const float d = sqrtf((xb - xa) * (xb - xa) + (yb - ya) * (yb - ya));
        if (fabsf(d - apart) > 0.01f) even = 0;
    }
    check("colour: the swatches are evenly spaced round the dial", even);
    check("colour: the swatch at twelve is slot 0",
          hit(PANEL_PAGE_COLOR, PANEL_C, PANEL_C - PANEL_RING_R + 1) == PANEL_HIT_SLOT);

    /* The design's checks: dark on Fuchsia, white on Led. */
    const int fuchsia = orb_colors_find("fuchsia"), led = orb_colors_find("led");
    check("colour: fuchsia gets a dark check",
          fuchsia >= 0 && ui_tone_luma(orb_colors_rgb((size_t)fuchsia)) >= CHECK_DARK_LUMA);
    check("colour: led gets a white check",
          led >= 0 && ui_tone_luma(orb_colors_rgb((size_t)led)) < CHECK_DARK_LUMA);
    check("colour: the plain word finds it too", orb_colors_find("purple") == led);
    snprintf(what, sizeof(what), "colour: no colour is near the check threshold (+-8 luma)");
    int clear = 1;
    for (size_t i = 0; i < orb_colors_count(); i++) {
        const int y = (int)ui_tone_luma(orb_colors_rgb(i));
        if (abs(y - CHECK_DARK_LUMA) < 8) clear = 0;
    }
    check(what, clear);
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

/*
 * ui_tone.h: white changes nothing, text keeps its floor, and a grey keeps its
 * brightness in any hue. The colours are white and orb_colors.c's own, read
 * from the linked catalog, so a change to the table is checked as it stands.
 */
static void tone(void)
{
    static const uint32_t greys[] = {0xFFFFFF, 0x0B0C0F, 0x4A4F59, 0x1F2229, 0xC8CCD4,
                                     0x16181D, 0x5B606B, 0x3A3E48, UI_HINT_RGB,
                                     0x4E5462, 0x2B2E37, 0x4A5560};
    uint32_t tints[1 + PANEL_SLOTS] = {0xFFFFFF};
    const size_t n_tints = 1 + orb_colors_count();
    for (size_t i = 0; i + 1 < n_tints && i < PANEL_SLOTS; i++) {
        tints[i + 1] = orb_colors_rgb(i);
    }
    int same = 1, floor_ok = 1, bright = 1;
    for (size_t i = 0; i < sizeof(greys) / sizeof(greys[0]); i++) {
        same &= ui_tone_mul(0xFFFFFF, greys[i]) == greys[i];
        same &= ui_tone_grey(0xFFFFFF, greys[i]) == greys[i];
    }
    same &= ui_tone_ink(0xFFFFFF) == 0xFFFFFF;
    for (size_t t = 0; t < n_tints && t < sizeof(tints) / sizeof(tints[0]); t++) {
        floor_ok &= ui_tone_luma(ui_tone_ink(tints[t])) >= UI_TONE_INK_LUMA - 1;
        /* Dark greys only: the light ones clip at 255 by design. */
        for (size_t i = 0; i < sizeof(greys) / sizeof(greys[0]); i++) {
            const int want = (int)ui_tone_luma(greys[i]);
            if (want <= 0x60) {
                /* Within 5: the palette's Glass has no red and its Clay almost
                 * no green, and the idle indicator's 0x4A5560 is a blue-grey
                 * rather than a neutral one, so it lands 4-5 luma off in those
                 * two. Every other pairing is within 3. */
                const int got = (int)ui_tone_luma(ui_tone_grey(tints[t], greys[i]));
                bright &= abs(got - want) <= 5;
            }
        }
    }
    check("tone: on white every colour is exactly the reference sheet's", same);
    check("tone: text on every orb colour is at least UI_TONE_INK_LUMA bright", floor_ok);
    check("tone: dark greys keep their brightness in every hue", bright);
}

int main(int argc, char **argv)
{
    sound();
    halves();
    colours();
    coverage();
    tone();

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
