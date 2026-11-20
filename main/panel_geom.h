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
 * (x + 0.5, y + 0.5) when painting. The numbers below are the sheet's, on the
 * 466 px panel; PXF() scales them to this board's (see ui_scale.h).
 */

#include <stdbool.h>

#include "ui_scale.h"

#define PANEL_C         (PANEL_SIZE * 0.5f)   /* centre, both axes */
#define PANEL_R         PXF(199.0f)   /* 233 - the 34 px edge inset */
#define PANEL_GAP       PXF(18.0f)    /* half of the 36 px gap */
#define PANEL_CORNER    PXF(8.0f)

/*
 * The sound page's "linked" buttons: each side is one half of the disc, + above
 * and - below, joined by a band across the middle (the bridge) that shows the
 * value they share. The bridge is a readout, not a button.
 */
#define PANEL_BRIDGE    PXF(38.0f)    /* half its height: 76 px */

/*
 * The colour picker: one dial, a 60 px swatch per colour set evenly round it,
 * clockwise from twelve o'clock. Slot n is orb_colors.c's index n, so this is
 * that catalog's size -- host/panel.sh checks the two agree.
 */
#define PANEL_RING_R    PXF(165.0f)
#define PANEL_SWATCH_R  PXF(30.0f)
#define PANEL_SLOTS     7

/* The alarm page's previous / next circles, either side of the time. */
#define PANEL_NAV_X     PXF(150.0f)   /* offset from centre */
#define PANEL_NAV_Y     PXF(165.0f)   /* absolute */
#define PANEL_NAV_R     PXF(26.0f)

/* A finger is wider than a swatch's edge is precise. Circles hit this far out.
 * A finger's width, so not scaled: see ui_scale.h. */
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

/* Centre of swatch `slot` (0 at twelve, then clockwise) on the dial. */
void panel_slot_center(int slot, float *x, float *y);

/* What a touch at (x, y) lands on, on `page`. */
panel_hit_t panel_hit(panel_page_t page, float x, float y);

