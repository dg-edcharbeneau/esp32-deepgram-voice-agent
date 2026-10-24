/* See ui_paint.h. */
#include "ui_paint.h"

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
