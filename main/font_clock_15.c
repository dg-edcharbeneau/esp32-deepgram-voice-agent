/*******************************************************************************
 * Size: 15 px
 * Bpp: 4
 * Opts: --no-compress --no-prefilter --bpp 4 --size 15 --font managed_components/lvgl__lvgl/scripts/built_in_font/Montserrat-Medium.ttf -r 0x41,0x4D,0x50 --format lvgl -o main/font_clock_15.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef FONT_CLOCK_15
#define FONT_CLOCK_15 1
#endif

#if FONT_CLOCK_15

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0041 "A" */
    0x0, 0x0, 0x8, 0xf7, 0x0, 0x0, 0x0, 0x0,
    0xe, 0xce, 0x0, 0x0, 0x0, 0x0, 0x7e, 0x1e,
    0x60, 0x0, 0x0, 0x0, 0xe6, 0x7, 0xd0, 0x0,
    0x0, 0x6, 0xe0, 0x0, 0xf5, 0x0, 0x0, 0xd,
    0x70, 0x0, 0x8d, 0x0, 0x0, 0x5f, 0xff, 0xff,
    0xff, 0x50, 0x0, 0xd9, 0x33, 0x33, 0x3a, 0xc0,
    0x4, 0xf2, 0x0, 0x0, 0x3, 0xf4, 0xc, 0xb0,
    0x0, 0x0, 0x0, 0xcc,

    /* U+004D "M" */
    0x6f, 0x20, 0x0, 0x0, 0x0, 0xcb, 0x6f, 0xb0,
    0x0, 0x0, 0x6, 0xfb, 0x6f, 0xf5, 0x0, 0x0,
    0x1f, 0xfb, 0x6f, 0x8e, 0x10, 0x0, 0xac, 0xbb,
    0x6f, 0xd, 0x90, 0x4, 0xf3, 0xbb, 0x6f, 0x4,
    0xf3, 0xd, 0x80, 0xbb, 0x6f, 0x0, 0xad, 0x8e,
    0x0, 0xbb, 0x6f, 0x0, 0x1e, 0xf4, 0x0, 0xbb,
    0x6f, 0x0, 0x5, 0x90, 0x0, 0xbb, 0x6f, 0x0,
    0x0, 0x0, 0x0, 0xbb,

    /* U+0050 "P" */
    0x6f, 0xff, 0xfe, 0xb3, 0x0, 0x6f, 0x54, 0x46,
    0xcf, 0x50, 0x6f, 0x10, 0x0, 0xc, 0xd0, 0x6f,
    0x10, 0x0, 0x8, 0xf0, 0x6f, 0x10, 0x0, 0xa,
    0xe0, 0x6f, 0x10, 0x2, 0x8f, 0x80, 0x6f, 0xff,
    0xff, 0xe7, 0x0, 0x6f, 0x43, 0x32, 0x0, 0x0,
    0x6f, 0x10, 0x0, 0x0, 0x0, 0x6f, 0x10, 0x0,
    0x0, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 176, .box_w = 12, .box_h = 10, .ofs_x = -1, .ofs_y = 0},
    {.bitmap_index = 60, .adv_w = 229, .box_w = 12, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 120, .adv_w = 173, .box_w = 10, .box_h = 10, .ofs_x = 1, .ofs_y = 0}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_0[] = {
    0x0, 0xc, 0xf
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 65, .range_length = 16, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 3, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};

/*-----------------
 *    KERNING
 *----------------*/


/*Map glyph_ids to kern left classes*/
static const uint8_t kern_left_class_mapping[] =
{
    0, 1, 0, 2
};

/*Map glyph_ids to kern right classes*/
static const uint8_t kern_right_class_mapping[] =
{
    0, 1, 2, 2
};

/*Kern values between classes*/
static const int8_t kern_class_values[] =
{
    3, 0, -11, -2
};


/*Collect the kern class' data in one place*/
static const lv_font_fmt_txt_kern_classes_t kern_classes =
{
    .class_pair_values   = kern_class_values,
    .left_class_mapping  = kern_left_class_mapping,
    .right_class_mapping = kern_right_class_mapping,
    .left_class_cnt      = 2,
    .right_class_cnt     = 2,
};

/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LV_VERSION_CHECK(8, 0, 0)
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = &kern_classes,
    .kern_scale = 16,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 1,
    .bitmap_format = 0,
#if LV_VERSION_CHECK(8, 0, 0)
    .cache = &cache
#endif
};


/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LV_VERSION_CHECK(8, 0, 0)
const lv_font_t font_clock_15 = {
#else
lv_font_t font_clock_15 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 10,          /*The maximum line height required by the font*/
    .base_line = 0,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc           /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
};



#endif /*#if FONT_CLOCK_15*/

