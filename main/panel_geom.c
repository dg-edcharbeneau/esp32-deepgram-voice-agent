/*
 * Shapes as signed distance functions, which buys three things at once: an
 * anti-aliased edge from one subtraction, rounded corners from one formula, and
 * a hit test that is the same function as the paint. See panel_geom.h.
 */
#include "panel_geom.h"

#include <math.h>

/*
 * Intersection of two shapes with the corner where their edges meet rounded to
 * r -- Inigo Quilez's "round intersection". Inside both and away from the
 * corner it is plain max(a, b), so the straight edges stay exact; only within r
 * of the corner does the arc take over.
 */
static float round_isect(float a, float b, float r)
{
    const float ua = fmaxf(a + r, 0.0f);
    const float ub = fmaxf(b + r, 0.0f);
    return fminf(-r, fmaxf(a, b)) + sqrtf(ua * ua + ub * ub);
}

float panel_seg_sdf(panel_seg_t seg, float x, float y)
{
    const float dx = x - PANEL_C;
    const float dy = y - PANEL_C;
    const float disc = sqrtf(dx * dx + dy * dy) - PANEL_R;

    /* Each half-plane is "this side of the gap": negative inside. */
    const float above = dy + PANEL_GAP;
    const float below = -dy + PANEL_GAP;
    const float left = dx + PANEL_GAP;
    const float right = -dx + PANEL_GAP;

    switch (seg) {
    case PANEL_SEG_HALF_TOP:
        return round_isect(disc, above, PANEL_CORNER);
    case PANEL_SEG_HALF_BOTTOM:
        return round_isect(disc, below, PANEL_CORNER);
    case PANEL_SEG_QUAD_TL:
        return round_isect(round_isect(disc, left, PANEL_CORNER), above, PANEL_CORNER);
    case PANEL_SEG_QUAD_TR:
        return round_isect(round_isect(disc, right, PANEL_CORNER), above, PANEL_CORNER);
    case PANEL_SEG_QUAD_BL:
        return round_isect(round_isect(disc, left, PANEL_CORNER), below, PANEL_CORNER);
    case PANEL_SEG_QUAD_BR:
        return round_isect(round_isect(disc, right, PANEL_CORNER), below, PANEL_CORNER);
    case PANEL_SEG_HALF_LEFT:
        return round_isect(disc, left, PANEL_CORNER);
    case PANEL_SEG_HALF_RIGHT:
        return round_isect(disc, right, PANEL_CORNER);
    }
    return 1.0f;
}

float panel_circle_sdf(float cx, float cy, float r, float x, float y)
{
    const float dx = x - cx;
    const float dy = y - cy;
    return sqrtf(dx * dx + dy * dy) - r;
}

float panel_coverage(float d)
{
    const float c = 0.5f - d;
    return c <= 0.0f ? 0.0f : (c >= 1.0f ? 1.0f : c);
}

void panel_seg_bbox(panel_seg_t seg, int *x0, int *y0, int *x1, int *y1)
{
    /* One pixel of margin for the anti-aliased edge. */
    const int lo = (int)(PANEL_C - PANEL_R) - 1;
    const int hi = (int)(PANEL_C + PANEL_R) + 1;
    const int gap_lo = (int)(PANEL_C - PANEL_GAP) + 1;
    const int gap_hi = (int)(PANEL_C + PANEL_GAP) - 1;

    *x0 = lo;
    *x1 = hi;
    *y0 = lo;
    *y1 = hi;
    switch (seg) {
    case PANEL_SEG_HALF_TOP:
        *y1 = gap_lo;
        break;
    case PANEL_SEG_HALF_BOTTOM:
        *y0 = gap_hi;
        break;
    case PANEL_SEG_QUAD_TL:
        *x1 = gap_lo;
        *y1 = gap_lo;
        break;
    case PANEL_SEG_QUAD_TR:
        *x0 = gap_hi;
        *y1 = gap_lo;
        break;
    case PANEL_SEG_QUAD_BL:
        *x1 = gap_lo;
        *y0 = gap_hi;
        break;
    case PANEL_SEG_QUAD_BR:
        *x0 = gap_hi;
        *y0 = gap_hi;
        break;
    case PANEL_SEG_HALF_LEFT:
        *x1 = gap_lo;
        break;
    case PANEL_SEG_HALF_RIGHT:
        *x0 = gap_hi;
        break;
    }
}

void panel_slot_center(int slot, float *x, float *y)
{
    const float a = (float)slot * (2.0f * (float)M_PI / (float)PANEL_SLOTS);
    *x = PANEL_C + PANEL_RING_R * sinf(a);
    *y = PANEL_C - PANEL_RING_R * cosf(a);
}

panel_hit_t panel_hit(panel_page_t page, float x, float y)
{
    switch (page) {
    case PANEL_PAGE_SOUND: {
        /* Above the bridge is +, below it -, and the bridge itself is only a
         * readout: a tap on the number changes nothing. */
        const float dy = y - PANEL_C;
        if (dy >= -PANEL_BRIDGE && dy <= PANEL_BRIDGE) {
            return PANEL_HIT_NONE;
        }
        const bool up = dy < 0.0f;
        if (panel_seg_sdf(PANEL_SEG_HALF_LEFT, x, y) <= 0.0f) {
            return up ? PANEL_HIT_MIC_UP : PANEL_HIT_MIC_DOWN;
        }
        if (panel_seg_sdf(PANEL_SEG_HALF_RIGHT, x, y) <= 0.0f) {
            return up ? PANEL_HIT_VOL_UP : PANEL_HIT_VOL_DOWN;
        }
        return PANEL_HIT_NONE;
    }

    case PANEL_PAGE_COLOR:
        /* Adjacent swatches are 85 px apart, so a slop of 8 cannot overlap two. */
        for (int s = 0; s < PANEL_SLOTS; s++) {
            float cx, cy;
            panel_slot_center(s, &cx, &cy);
            if (panel_circle_sdf(cx, cy, PANEL_SWATCH_R + PANEL_HIT_SLOP, x, y) <= 0.0f) {
                return (panel_hit_t)(PANEL_HIT_SLOT + s);
            }
        }
        return PANEL_HIT_NONE;

    case PANEL_PAGE_TOGGLES:
        if (panel_seg_sdf(PANEL_SEG_HALF_TOP, x, y) <= 0.0f) return PANEL_HIT_NS;
        if (panel_seg_sdf(PANEL_SEG_HALF_BOTTOM, x, y) <= 0.0f) return PANEL_HIT_CLOCK_FMT;
        return PANEL_HIT_NONE;

    case PANEL_PAGE_ALARMS:
        if (panel_circle_sdf(PANEL_C - PANEL_NAV_X, PANEL_NAV_Y,
                             PANEL_NAV_R + PANEL_HIT_SLOP, x, y) <= 0.0f) {
            return PANEL_HIT_ALARM_PREV;
        }
        if (panel_circle_sdf(PANEL_C + PANEL_NAV_X, PANEL_NAV_Y,
                             PANEL_NAV_R + PANEL_HIT_SLOP, x, y) <= 0.0f) {
            return PANEL_HIT_ALARM_NEXT;
        }
        if (panel_seg_sdf(PANEL_SEG_HALF_BOTTOM, x, y) <= 0.0f) return PANEL_HIT_ALARM_CANCEL;
        return PANEL_HIT_NONE;

    default:
        return PANEL_HIT_NONE;
    }
}

int panel_color_for_slot(int subpage, int slot, int color_count)
{
    if (slot < 0 || slot >= PANEL_SLOTS) {
        return -1;
    }
    if (slot == PANEL_SLOT_MORE) {
        return PANEL_SLOT_IS_MORE;
    }
    /* Eleven positions on page A: every slot but six. */
    const int per_page = PANEL_SLOTS - 1;
    const int k = (slot < PANEL_SLOT_MORE) ? slot : slot - 1;
    const int index = subpage * per_page + k;
    return (index < color_count) ? index : -1;
}

bool panel_slot_for_color(int index, int color_count, int *subpage, int *slot)
{
    if (index < 0 || index >= color_count) {
        return false;
    }
    const int per_page = PANEL_SLOTS - 1;
    const int k = index % per_page;
    *subpage = index / per_page;
    *slot = (k < PANEL_SLOT_MORE) ? k : k + 1;
    return true;
}
