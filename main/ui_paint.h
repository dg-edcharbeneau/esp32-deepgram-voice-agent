#pragma once

/*
 * What the two canvas painters -- the touch panel (ui_panel.c) and the orb clock
 * (ui_clock.c) -- both need: RGB565 packing that rounds, a source-over blend,
 * and text placed by its centre. One copy, so a fix to either reaches both.
 * The coverage ramp is panel_coverage() in panel_geom.c, where the host tests
 * can reach it.
 *
 * LVGL task only, like both painters.
 */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/*
 * 8-bit channels to RGB565, ROUNDED to the nearest step rather than truncated.
 * Truncating takes 3 bits off red and blue but only 2 off green, which turned
 * the reference sheet's neutral dark greys (#16181D) visibly green on the panel.
 */
static inline uint16_t ui_paint_rgb565(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)((((r * 31 + 127) / 255) << 11) | (((g * 63 + 127) / 255) << 5) |
                      ((b * 31 + 127) / 255));
}

/* `rgb` scaled by cov/255 -- "this much of the colour, over black". */
static inline uint16_t ui_paint_rgb565_cov(uint32_t rgb, uint32_t cov)
{
    return ui_paint_rgb565((((rgb >> 16) & 0xFF) * cov + 127) / 255,
                           (((rgb >> 8) & 0xFF) * cov + 127) / 255,
                           ((rgb & 0xFF) * cov + 127) / 255);
}

/* `rgb` at a/255 over the RGB565 pixel `dst`. */
static inline uint16_t ui_paint_blend(uint16_t dst, uint32_t rgb, uint32_t a)
{
    const uint32_t dr = (((dst >> 11) & 31) * 255 + 15) / 31;
    const uint32_t dg = (((dst >> 5) & 63) * 255 + 31) / 63;
    const uint32_t db = ((dst & 31) * 255 + 15) / 31;
    const uint32_t na = 255 - a;
    return ui_paint_rgb565((((rgb >> 16) & 0xFF) * a + dr * na + 127) / 255,
                           (((rgb >> 8) & 0xFF) * a + dg * na + 127) / 255,
                           ((rgb & 0xFF) * a + db * na + 127) / 255);
}

/* Where character `ch`'s ink sits in `f`'s line box: its top and bottom rows
 * below the box's top. */
void ui_paint_ink_rows(const lv_font_t *f, uint32_t ch, int32_t *top, int32_t *bottom);

/* ---------------- anti-aliased shapes, blended ---------------- */

/* Where a painter draws: the canvas pixels and their bounds. */
typedef struct {
    uint16_t *px;
    uint32_t stride;   /* in pixels */
    int w, h;
} ui_paint_target_t;

/* Opacity 0..1 as the blend's 0..255. */
static inline uint32_t ui_paint_a8(float a)
{
    return a <= 0.0f ? 0 : (a >= 1.0f ? 255 : (uint32_t)(a * 255.0f + 0.5f));
}

/* One pixel, source-over at a/255, clipped to the target. */
static inline void ui_paint_plot(const ui_paint_target_t *t, int x, int y, uint32_t rgb, uint32_t a)
{
    if (a == 0 || x < 0 || y < 0 || x >= t->w || y >= t->h) {
        return;
    }
    uint16_t *p = t->px + (size_t)y * t->stride + x;
    *p = ui_paint_blend(*p, rgb, a);
}

/*
 * A filled disc, or with r_in > 0 an annulus, at `alpha`. Squared distance
 * first: inside the edge band a pixel is wholly covered, outside it wholly not,
 * and only the one-pixel band takes a sqrtf -- a library call on this FPU. The
 * bands are exactly where panel_coverage() returns 1 or 0.
 */
void ui_paint_disc(const ui_paint_target_t *t, float cx, float cy, float r_in, float r_out,
                   uint32_t rgb, float alpha);

/*
 * A thin circle of radius r and stroke w, drawn only on rows above `y_end`.
 * Walks the two short spans each row crosses rather than the circle's whole
 * box, so a circle the size of the panel costs a few thousand pixels.
 */
void ui_paint_circle(const ui_paint_target_t *t, float cx, float cy, float r, float w,
                     uint32_t rgb, float alpha, int y_end);

/* Width of `s` set in `f` with `space` px between letters. */
int32_t ui_paint_text_width(const lv_font_t *f, int32_t space, const char *s);

/*
 * Draw `s` centred on (cx, cy) into the layer; returns the box it occupies.
 *
 * `by_ink` false centres the line box, as the panel's labels always have. True
 * centres what is actually inked -- the first character's glyph, top to bottom
 * -- which is what the clock needs, where a 52 px line box carries ascender room
 * the digits never use. The returned box is then the ink's rows.
 */
lv_area_t ui_paint_text(lv_layer_t *L, const lv_font_t *f, uint32_t rgb, int32_t cx, int32_t cy,
                        int32_t space, bool by_ink, const char *s);
