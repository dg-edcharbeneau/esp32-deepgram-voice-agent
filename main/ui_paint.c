/* See ui_paint.h. */
#include "ui_paint.h"

#include <math.h>

#include "panel_geom.h"

void ui_paint_disc(const ui_paint_target_t *t, float cx, float cy, float r_in, float r_out,
                   uint32_t rgb, float alpha)
{
    const float out_lo = r_out - 0.5f, out_hi = r_out + 0.5f;
    const float in_lo = r_in - 0.5f, in_hi = r_in + 0.5f;
    const float solid2 = out_lo > 0.0f ? out_lo * out_lo : -1.0f;   /* d2 <= this: covered */
    const float none2 = out_hi * out_hi;                            /* d2 >= this: not */
    const float hole2 = in_lo > 0.0f ? in_lo * in_lo : -1.0f;       /* d2 <= this: in the hole */
    const float rim2 = in_hi * in_hi;                               /* d2 >= this: clear of it */
    const uint32_t a_full = ui_paint_a8(alpha);

    const int x0 = (int)floorf(cx - r_out - 1.0f), x1 = (int)ceilf(cx + r_out + 1.0f);
    const int y0 = (int)floorf(cy - r_out - 1.0f), y1 = (int)ceilf(cy + r_out + 1.0f);
    for (int y = y0; y <= y1; y++) {
        const float dy = (float)y + 0.5f - cy;
        for (int x = x0; x <= x1; x++) {
            const float dx = (float)x + 0.5f - cx;
            const float d2 = dx * dx + dy * dy;
            if (d2 >= none2 || (r_in > 0.0f && d2 <= hole2)) {
                continue;
            }
            const bool solid_out = d2 <= solid2;
            const bool solid_in = r_in <= 0.0f || d2 >= rim2;
            if (solid_out && solid_in) {
                ui_paint_plot(t, x, y, rgb, a_full);
                continue;
            }
            const float dist = sqrtf(d2);
            float cov = solid_out ? 1.0f : panel_coverage(dist - r_out);
            if (!solid_in) {
                cov = fminf(cov, 1.0f - panel_coverage(dist - r_in));
            }
            ui_paint_plot(t, x, y, rgb, ui_paint_a8(cov * alpha));
        }
    }
}

void ui_paint_circle(const ui_paint_target_t *t, float cx, float cy, float r, float w,
                     uint32_t rgb, float alpha, int y_end)
{
    const float outer = r + w * 0.5f + 1.0f, inner = r - w * 0.5f - 1.0f;
    const int y0 = (int)floorf(cy - outer);
    int y1 = (int)ceilf(cy + outer);
    if (y1 >= y_end) {
        y1 = y_end - 1;
    }
    for (int y = y0; y <= y1; y++) {
        const float dy = (float)y + 0.5f - cy;
        const float o2 = outer * outer - dy * dy;
        if (o2 <= 0.0f) {
            continue;
        }
        const float xo = sqrtf(o2);
        const float i2 = inner > 0.0f ? inner * inner - dy * dy : 0.0f;
        const float xi = i2 > 0.0f ? sqrtf(i2) : 0.0f;
        /* Left span, then right: [cx - xo, cx - xi] and [cx + xi, cx + xo]. */
        for (int side = -1; side <= 1; side += 2) {
            const float a = cx + side * xi, b = cx + side * xo;
            const int xa = (int)floorf(fminf(a, b)), xb = (int)ceilf(fmaxf(a, b));
            for (int x = xa; x <= xb; x++) {
                const float dx = (float)x + 0.5f - cx;
                const float d = fabsf(sqrtf(dx * dx + dy * dy) - r) - w * 0.5f;
                ui_paint_plot(t, x, y, rgb, ui_paint_a8(panel_coverage(d) * alpha));
            }
            if (xi == 0.0f) {
                break;   /* one span covers the row: nothing inside the stroke */
            }
        }
    }
}

int32_t ui_paint_text_width(const lv_font_t *f, int32_t space, const char *s)
{
    lv_point_t sz;
    lv_text_get_size(&sz, s, f, space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

void ui_paint_ink_rows(const lv_font_t *f, uint32_t ch, int32_t *top, int32_t *bottom)
{
    lv_font_glyph_dsc_t g;
    const int32_t ascent = f->line_height - f->base_line;
    if (lv_font_get_glyph_dsc(f, &g, ch, 0)) {
        *top = ascent - (g.ofs_y + g.box_h);
        *bottom = ascent - g.ofs_y;
    } else {
        *top = 0;
        *bottom = f->line_height;
    }
}

lv_area_t ui_paint_text(lv_layer_t *L, const lv_font_t *f, uint32_t rgb, int32_t cx, int32_t cy,
                        int32_t space, bool by_ink, const char *s)
{
    lv_point_t sz;
    lv_text_get_size(&sz, s, f, space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t top = 0, bottom = sz.y;
    int32_t y1 = cy - sz.y / 2;
    if (by_ink) {
        ui_paint_ink_rows(f, (uint32_t)(unsigned char)s[0], &top, &bottom);
        y1 = cy - (top + bottom) / 2;
    }
    /* The two painters' boxes as they always were, so neither moves a pixel. */
    const lv_area_t a = {cx - sz.x / 2 - 1, y1, cx + sz.x / 2 + 1,
                         by_ink ? y1 + sz.y : cy + sz.y / 2 + 1};

    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = f;
    d.color = lv_color_hex(rgb);
    d.text = s;
    d.letter_space = space;
    d.text_local = 1;   /* rendered at finish_layer, after the caller's buffer goes */
    d.align = LV_TEXT_ALIGN_CENTER;
    lv_draw_label(L, &d, &a);
    return (lv_area_t){a.x1, y1 + top, a.x2, y1 + bottom};
}
