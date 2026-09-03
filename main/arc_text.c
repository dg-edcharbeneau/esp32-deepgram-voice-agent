#include "arc_text.h"

#include <math.h>
#include <string.h>

#include "bsp/display.h"

#define CENTER_X (BSP_LCD_H_RES / 2)
#define CENTER_Y (BSP_LCD_V_RES / 2)

/*
 * Slop around every box this blacks and invalidates, for the anti-aliased edge
 * of a rotated glyph. The same two pixels the other overlays in ui.c allow.
 */
#define ARC_SLOP 2

/*
 * ASCII only, which is not a shortcut. lv_font_montserrat_24 is built over
 * 0x20-0x7F in this project, so a multi-byte sequence would draw a placeholder
 * box whatever this did with it -- and every string that reaches here is a
 * literal from ui.c's own status table. Walking bytes keeps the layout pass and
 * the draw pass trivially in step, which matters because they have to agree
 * exactly or the wipe misses the glyph.
 */
static int32_t glyph_advance(const lv_font_t *font, char c)
{
    lv_font_glyph_dsc_t g;
    if (!lv_font_get_glyph_dsc(font, &g, (uint32_t)(unsigned char)c, 0)) {
        return 0;
    }
    return g.adv_w;
}

static int32_t total_advance(const char *text, const lv_font_t *font)
{
    int32_t w = 0;
    for (const char *p = text; *p != '\0'; p++) {
        w += glyph_advance(font, *p);
    }
    return w;
}

/*
 * Walk the string, handing each glyph's centre and angle to `visit`.
 *
 * ONE WALK, THREE CALLERS -- the box, the wipe and the draw all have to agree on
 * where every letter lands, and the cheapest way to guarantee that is for them
 * to share the arithmetic rather than to each repeat it. A wipe that disagreed
 * with the draw by a pixel would leave a crumb of the previous frame behind
 * every letter.
 *
 * Angles are measured clockwise from twelve o'clock, so a glyph's own "up"
 * points radially outward and the run reads left to right across the top.
 */
typedef void (*arc_visit_t)(void *user, uint32_t unicode, int32_t cx, int32_t cy,
                            int32_t rot_deg10, const lv_font_glyph_dsc_t *g);

static void arc_walk(const char *text, const lv_font_t *font, arc_visit_t visit, void *user)
{
    const int32_t width = total_advance(text, font);
    if (width <= 0) {
        return;
    }

    const float r = (float)ARC_TEXT_RADIUS;
    /* Arc length over radius is the angle it subtends. The run is centred on
     * twelve o'clock, so it starts half of that to the left. */
    const float span = (float)width / r;
    float theta = -span * 0.5f;

    for (const char *p = text; *p != '\0'; p++) {
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(font, &g, (uint32_t)(unsigned char)*p, 0)) {
            continue;
        }
        const float adv = (float)g.adv_w / r;
        /* The glyph sits at the middle of its own advance, not at the start of
         * it, or the run drifts right by half a letter. */
        const float mid = theta + adv * 0.5f;

        const int32_t cx = CENTER_X + (int32_t)lroundf(r * sinf(mid));
        const int32_t cy = CENTER_Y - (int32_t)lroundf(r * cosf(mid));
        const int32_t rot = (int32_t)lroundf(mid * (1800.0f / (float)M_PI));

        visit(user, (uint32_t)(unsigned char)*p, cx, cy, rot, &g);
        theta += adv;
    }
}

/*
 * The box a rotated glyph's ink actually lands in.
 *
 * THE INK, NOT THE LINE BOX, and the difference is what you see on the panel.
 * The first version circumscribed a square around the whole advance-by-line-box
 * rectangle: about 52 px on a side, which for eighteen overlapping letters is a
 * 52 px black band along the arc -- far taller than the text sitting in it, and
 * it reads as a slab painted over the face rather than as a caption. A glyph's
 * ink is much smaller than its line box (box_w x box_h, and for a lowercase
 * letter that is roughly 13 x 17 against 14 x 29), so rotating the four corners
 * of the ink rectangle and taking their extent gives a ribbon that follows the
 * text instead of swallowing it.
 *
 * Measured from the pivot, which lv_draw_letter() puts at (adv_w/2, baseline) --
 * not at the middle of the line box. The baseline is nowhere near the vertical
 * centre (montserrat_24 has 23 px above it and 6 below), and getting that wrong
 * undersizes the box for every ascender, which leaves glyph tops outside both
 * the wipe and the invalidated area, surviving the fade as litter nothing ever
 * takes back.
 *
 * LVGL's own y axis points down and the font's ofs_y points up from the
 * baseline, hence the sign flip on the vertical corners.
 */
static lv_area_t glyph_box(int32_t cx, int32_t cy, int32_t rot_deg10,
                           const lv_font_glyph_dsc_t *g, const lv_font_t *font)
{
    /* A space has no ink at all; give it something to keep the union honest. */
    const int32_t bw = g->box_w > 0 ? (int32_t)g->box_w : 1;
    const int32_t bh = g->box_h > 0 ? (int32_t)g->box_h : 1;

    /*
     * Left edge at minus half an advance, with no ofs_x term: LVGL puts the
     * pivot at adv_w/2 measured from the INK's left edge rather than from the
     * origin, so the ink starts exactly half an advance before the pivot however
     * far the glyph is inset. Adding ofs_x here would slide the wipe off the
     * letter by a pixel or two -- and a wipe that misses is litter that survives
     * the fade.
     */
    const int32_t half_adv = (g->adv_w > 0 ? (int32_t)g->adv_w : bw) / 2;
    const float left = (float)(-half_adv);
    const float right = left + (float)bw;
    const float bottom = (float)(-g->ofs_y);          /* screen-down */
    const float top = bottom - (float)bh;

    const float rad = (float)rot_deg10 * ((float)M_PI / 1800.0f);
    const float cs = cosf(rad);
    const float sn = sinf(rad);

    const float xs[4] = { left, right, right, left };
    const float ys[4] = { top, top, bottom, bottom };

    float min_x = 0.0f, max_x = 0.0f, min_y = 0.0f, max_y = 0.0f;
    for (int i = 0; i < 4; i++) {
        /* Clockwise, matching LVGL's positive rotation. */
        const float rx = xs[i] * cs - ys[i] * sn;
        const float ry = xs[i] * sn + ys[i] * cs;
        if (i == 0 || rx < min_x) min_x = rx;
        if (i == 0 || rx > max_x) max_x = rx;
        if (i == 0 || ry < min_y) min_y = ry;
        if (i == 0 || ry > max_y) max_y = ry;
    }

    lv_area_t a;
    a.x1 = cx + (int32_t)floorf(min_x) - ARC_SLOP;
    a.y1 = cy + (int32_t)floorf(min_y) - ARC_SLOP;
    a.x2 = cx + (int32_t)ceilf(max_x) + ARC_SLOP;
    a.y2 = cy + (int32_t)ceilf(max_y) + ARC_SLOP;
    return a;
}

/* ---------------- box ---------------- */

typedef struct {
    const lv_font_t *font;
    lv_area_t box;
    bool any;
} box_ctx_t;

static void box_visit(void *user, uint32_t unicode, int32_t cx, int32_t cy,
                      int32_t rot_deg10, const lv_font_glyph_dsc_t *g)
{
    (void)unicode;
    box_ctx_t *c = user;
    const lv_area_t a = glyph_box(cx, cy, rot_deg10, g, c->font);
    if (!c->any) {
        c->box = a;
        c->any = true;
        return;
    }
    if (a.x1 < c->box.x1) c->box.x1 = a.x1;
    if (a.y1 < c->box.y1) c->box.y1 = a.y1;
    if (a.x2 > c->box.x2) c->box.x2 = a.x2;
    if (a.y2 > c->box.y2) c->box.y2 = a.y2;
}

bool arc_text_box(const char *text, const lv_font_t *font, lv_area_t *out)
{
    if (text == NULL || *text == '\0' || font == NULL) {
        return false;
    }
    box_ctx_t c = { .font = font, .any = false };
    arc_walk(text, font, box_visit, &c);
    if (!c.any) {
        return false;
    }
    *out = c.box;
    return true;
}

/* ---------------- wipe ---------------- */

typedef struct {
    const lv_font_t *font;
    lv_layer_t *layer;
    lv_draw_rect_dsc_t fill;
} wipe_ctx_t;

static void wipe_visit(void *user, uint32_t unicode, int32_t cx, int32_t cy,
                       int32_t rot_deg10, const lv_font_glyph_dsc_t *g)
{
    (void)unicode;
    wipe_ctx_t *c = user;
    lv_area_t a = glyph_box(cx, cy, rot_deg10, g, c->font);
    lv_draw_rect(c->layer, &c->fill, &a);
}

void arc_text_wipe(lv_layer_t *layer, const char *text, const lv_font_t *font)
{
    if (text == NULL || *text == '\0' || font == NULL) {
        return;
    }
    wipe_ctx_t c = { .font = font, .layer = layer };
    lv_draw_rect_dsc_init(&c.fill);
    c.fill.bg_color = lv_color_black();
    c.fill.bg_opa = LV_OPA_COVER;
    c.fill.border_width = 0;
    arc_walk(text, font, wipe_visit, &c);
}

/* ---------------- draw ---------------- */

typedef struct {
    const lv_font_t *font;
    lv_layer_t *layer;
    lv_color_t color;
    lv_opa_t opa;
} draw_ctx_t;

static void draw_visit(void *user, uint32_t unicode, int32_t cx, int32_t cy,
                       int32_t rot_deg10, const lv_font_glyph_dsc_t *g)
{
    draw_ctx_t *c = user;

    lv_draw_letter_dsc_t dsc;
    lv_draw_letter_dsc_init(&dsc);
    dsc.font = c->font;
    dsc.color = c->color;
    dsc.opa = c->opa;
    dsc.unicode = unicode;
    dsc.rotation = rot_deg10;

    /*
     * WHERE `point` ACTUALLY PUTS THE GLYPH, which is not what the name suggests
     * and cost three rounds of chasing the wrong thing.
     *
     * lv_draw_letter() sets dsc->pivot to (adv_w/2, ascent) and hands the point
     * on. lv_draw_label.c then builds the ink box from it -- and moves that box
     * by MINUS the pivot (see the lv_area_move at the end of that block) before
     * the rotation transform adds the pivot back in its own frame. Work the two
     * shifts through and the pivot, the one point that stays still under
     * rotation, lands at exactly (point.x + ofs_x, point.y).
     *
     * So the back-off by half an advance and an ascent that looks obviously
     * right is a DOUBLE shift: it drew every glyph 23 px above and half an
     * advance left of the arc. Which read as text too high, clipped at the rim,
     * black wipe boxes sitting beside the letters instead of under them, and
     * litter left behind when the caption cleared -- all one bug.
     *
     * The pivot is the baseline centre and the baseline centre is what sits on
     * the arc, so the point is simply the target, less the ink's own x offset.
     */
    lv_point_t p = {
        .x = cx - g->ofs_x,
        .y = cy,
    };
    lv_draw_letter(c->layer, &dsc, &p);
}

void arc_text_draw(lv_layer_t *layer, const char *text, const lv_font_t *font,
                   lv_color_t color, lv_opa_t opa)
{
    if (text == NULL || *text == '\0' || font == NULL || opa == LV_OPA_TRANSP) {
        return;
    }
    draw_ctx_t c = { .font = font, .layer = layer, .color = color, .opa = opa };
    arc_walk(text, font, draw_visit, &c);
}
