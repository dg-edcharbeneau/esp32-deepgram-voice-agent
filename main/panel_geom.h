#pragma once

/*
 * The touch panel's geometry: what each control's shape is, and what a touch at
 * a point lands on. Plain C over libm, no LVGL, so host/panel.sh compiles it as
 * is -- and the same functions both paint a control and hit-test it, so what is
 * drawn is exactly what can be touched.
 *
 * The visual language comes from Ed's reference sheet ("ESP32 Round Display
 * UI.pdf", 1:1 device pixels): buttons fill a disc inset 34 px from the panel
 * edge, split by 36 px gaps, every corner rounded to 8 px.
 *
 * Coordinates are screen pixels, (0,0) top left. Pass a pixel's CENTRE
 * (x + 0.5, y + 0.5) when painting.
 */

#include <stdbool.h>

#define PANEL_SIZE      466
#define PANEL_C         233.0f   /* centre, both axes */
#define PANEL_R         199.0f   /* 233 - the 34 px edge inset */
#define PANEL_GAP       18.0f    /* half of the 36 px gap */
#define PANEL_CORNER    8.0f

/*
 * The sound page's "linked" buttons: each side is one half of the disc, + above
 * and - below, joined by a band across the middle (the bridge) that shows the
 * value they share. The bridge is a readout, not a button.
 */
#define PANEL_BRIDGE    38.0f    /* half its height: 76 px */

/* The colour picker: twelve clock positions, 60 px swatches. */
#define PANEL_RING_R    165.0f
#define PANEL_SWATCH_R  30.0f
#define PANEL_SLOTS     12
#define PANEL_SLOT_MORE 6        /* 6 o'clock: "+2" on page A, back on page B */

/* The alarm page's previous / next circles, either side of the time. */
#define PANEL_NAV_X     150.0f   /* offset from centre */
#define PANEL_NAV_Y     165.0f   /* absolute */
#define PANEL_NAV_R     26.0f

/* A finger is wider than a swatch's edge is precise. Circles hit this far out. */
#define PANEL_HIT_SLOP  8.0f

typedef enum {
    PANEL_PAGE_SOUND = 0,
    PANEL_PAGE_COLOR,
    PANEL_PAGE_TOGGLES,
    PANEL_PAGE_ALARMS,
    PANEL_PAGE_COUNT,
} panel_page_t;

typedef enum {
    PANEL_SEG_HALF_TOP = 0,
    PANEL_SEG_HALF_BOTTOM,
    PANEL_SEG_QUAD_TL,
    PANEL_SEG_QUAD_TR,
    PANEL_SEG_QUAD_BL,
    PANEL_SEG_QUAD_BR,
    /* Side by side, split by the vertical gap; the HALF_TOP shape transposed. */
    PANEL_SEG_HALF_LEFT,
    PANEL_SEG_HALF_RIGHT,
} panel_seg_t;

typedef enum {
    PANEL_HIT_NONE = 0,
    /* Sound: mic gain on the left, volume on the right, + above the bridge. */
    PANEL_HIT_MIC_UP,
    PANEL_HIT_MIC_DOWN,
    PANEL_HIT_VOL_UP,
    PANEL_HIT_VOL_DOWN,
    /* Colour: PANEL_HIT_SLOT + clock position 0..11 (0 is twelve o'clock). */
    PANEL_HIT_SLOT,
    PANEL_HIT_SLOT_LAST = PANEL_HIT_SLOT + PANEL_SLOTS - 1,
    /* Toggles. */
    PANEL_HIT_NS,
    PANEL_HIT_CLOCK_FMT,
    /* Timers & alarms. */
    PANEL_HIT_ALARM_PREV,
    PANEL_HIT_ALARM_NEXT,
    PANEL_HIT_ALARM_CANCEL,
} panel_hit_t;

/* Signed distance to a segment's edge: negative inside, in pixels. */
float panel_seg_sdf(panel_seg_t seg, float x, float y);

/* Signed distance to a circle's edge. */
float panel_circle_sdf(float cx, float cy, float r, float x, float y);

/* How much of a pixel at signed distance d is inside: 0..1, a one-pixel ramp. */
float panel_coverage(float d);

/* The pixel box a segment can paint into, inclusive. */
void panel_seg_bbox(panel_seg_t seg, int *x0, int *y0, int *x1, int *y1);

/* Centre of clock position `slot` (0 = twelve, clockwise) on the swatch ring. */
void panel_slot_center(int slot, float *x, float *y);

/* What a touch at (x, y) lands on, on `page`. */
panel_hit_t panel_hit(panel_page_t page, float x, float y);

/*
 * The colour picker's two pages map clock positions onto orb_colors.c indices.
 * Page A holds indices 0..10 clockwise from twelve, skipping six; page B holds
 * the rest from twelve. Returns the colour index, PANEL_SLOT_IS_MORE for the
 * six o'clock button, or -1 for an empty position.
 */
#define PANEL_SLOT_IS_MORE (-2)
int panel_color_for_slot(int subpage, int slot, int color_count);

/* Inverse: where colour `index` sits. Returns false for an index out of range. */
bool panel_slot_for_color(int index, int color_count, int *subpage, int *slot);
