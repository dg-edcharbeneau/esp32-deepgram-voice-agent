/*
 * The touch panel. See ui_panel.h for what it is, controls.h for what it calls,
 * and panel_geom.h for the shapes.
 *
 * PAINTED INTO THE FACE'S CANVAS, NOT BUILT FROM LVGL WIDGETS. LVGL has no shape
 * for a segment of a circle, and every widget would allocate mask buffers from
 * internal RAM while it draws -- the pool docs/display.md calls the binding
 * constraint. So the buttons are filled pixel by pixel straight into the PSRAM
 * canvas from panel_geom.c's distance functions, and only the text and the
 * small stroked icons go through an LVGL layer. Nothing is drawn unless
 * something changed: a page, a press, a value, or the once-a-second countdown.
 *
 * LVGL task only, like everything in ui.c.
 */
#include "ui_panel.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "audio_io.h"
#include "controls.h"
#include "orb_colors.h"
#include "panel_geom.h"
#include "ui.h"
#include "ui_paint.h"
#include "ui_tone.h"
#if CONFIG_UI_PANEL_SELFTEST
#include "screenshot.h"
#endif

static const char *TAG = "panel";

LV_FONT_DECLARE(font_panel_18);
LV_FONT_DECLARE(font_panel_28);
LV_FONT_DECLARE(font_panel_40);

/* ---------------- the reference sheet's palette ---------------- */

/* As the sheet draws it, in white and greys: what the panel shows on a white
 * orb. Any other orb colour is applied by ui_tone.h's rules in set_palette(). */
#define COL_PRIMARY        0xFFFFFF
#define COL_PRIMARY_INK    0x0B0C0F
#define COL_PRIMARY_LABEL  0x4A4F59   /* the small grey word on a white button */
#define COL_SECONDARY      0x1F2229
#define COL_PRESSED        0xC8CCD4
#define COL_DISABLED       0x16181D
#define COL_DISABLED_INK   0x5B606B
#define COL_DOT_OFF        0x3A3E48
#define COL_HINT           UI_HINT_RGB   /* the grey ui.c's hint and clock use */
#define COL_BRIDGE_FILL    0x4E5462   /* the sound page's level meter: the level */
#define COL_BRIDGE_EMPTY   0x2B2E37   /* ... and the rest of the band */

/* The palette in force: the sheet's, in the orb's colour. */
static struct {
    uint32_t primary, primary_ink, primary_label, secondary, text, pressed, disabled,
        disabled_ink, dot_off, hint, bridge_fill, bridge_empty;
} s_col;

static void set_palette(uint32_t tint)
{
    s_col.primary = ui_tone_mul(tint, COL_PRIMARY);
    s_col.primary_ink = ui_tone_mul(tint, COL_PRIMARY_INK);
    s_col.primary_label = ui_tone_mul(tint, COL_PRIMARY_LABEL);
    s_col.pressed = ui_tone_mul(tint, COL_PRESSED);
    s_col.text = ui_tone_ink(tint);
    s_col.secondary = ui_tone_grey(tint, COL_SECONDARY);
    s_col.disabled = ui_tone_grey(tint, COL_DISABLED);
    s_col.disabled_ink = ui_tone_grey(tint, COL_DISABLED_INK);
    s_col.dot_off = ui_tone_grey(tint, COL_DOT_OFF);
    s_col.hint = ui_tone_grey(tint, COL_HINT);
    s_col.bridge_fill = ui_tone_grey(tint, COL_BRIDGE_FILL);
    s_col.bridge_empty = ui_tone_grey(tint, COL_BRIDGE_EMPTY);
}

/* ---------------- gestures ---------------- */

#define OPEN_BAND_PX    40     /* a press must start this close to the top edge */
#define SWIPE_PX        80     /* how far a drag must travel to count */
#define TAP_SLOP_PX     20     /* further than this and it is not a tap */
#define IDLE_CLOSE_MS   8000
#define SAVE_QUIET_MS   2000   /* save volume / mic gain this long after the last press */
#define CANCEL_ARM_MS   3000

#define PAGE_DOTS_Y     446    /* in the 34 px rim outside the disc, on every page */

#define ALARMS_MAX      ALARM_MAX

/* ---------------- state ---------------- */

static lv_obj_t *s_canvas;
static bool s_open;
static panel_page_t s_page;
static int s_color_sub;                 /* the picker's own page: 0 or 1 */
static int s_alarm_idx;
static uint8_t s_cancel_armed_id;       /* 0: not armed */
static uint32_t s_cancel_armed_ms;

static bool s_opening;                  /* a press began in the top band, panel closed */
static bool s_open_gesture;             /* this press opened the panel */
static bool s_owned;                    /* this press began on the open panel */
static int32_t s_x0, s_y0, s_x, s_y;
static panel_hit_t s_pressed;
static uint32_t s_touch_ms;

static bool s_dirty;
static uint32_t s_sig;                  /* what the last paint showed; see signature() */

static alarm_view_t s_alarms[ALARMS_MAX];
static int s_alarm_n;
static uint32_t s_alarms_ms;            /* when s_alarms was taken */

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---------------- pixels ---------------- */

static uint16_t *s_px;
static uint32_t s_stride;   /* in pixels */

static void clear(void)
{
    for (int y = 0; y < PANEL_SIZE; y++) {
        memset(s_px + (size_t)y * s_stride, 0, PANEL_SIZE * sizeof(uint16_t));
    }
}

/*
 * COVERAGE MASKS, MADE ONCE. Evaluating the distance functions per pixel at
 * paint time cost 812 ms for the sound page on the board, and 160 ms even with
 * only the edge tiles done that way: the LVGL task shares core 1 with the echo
 * canceller and gets little of it, and sqrtf is a library call on this FPU.
 * But the shapes never change -- only their colours do -- and the four quarters
 * are one shape mirrored, the two halves another. So each is rasterised once
 * into an 8-bit coverage mask in PSRAM (~110 kB together), and painting is a
 * lookup: solid where the mask is 255, skipped where 0, scaled in between.
 */
typedef struct {
    uint8_t *cov;
    int x0, y0, w, h;   /* where the mask sits on screen, unmirrored */
} mask_t;

static mask_t s_half;   /* PANEL_SEG_HALF_TOP */
static mask_t s_quad;   /* PANEL_SEG_QUAD_TR */
/* Set once both masks are whole; the panel will not open before. Written by
 * mask_task, read by the LVGL task. */
static volatile bool s_masks_ready;

/* In 8 x 8 tiles: the distance at a tile's centre settles a whole tile more than
 * 7 px from any edge (host/panel.sh checks that against every pixel), so only the
 * tiles an edge crosses are evaluated per pixel. */
static bool build_mask(panel_seg_t seg, mask_t *m)
{
    int x0, y0, x1, y1;
    panel_seg_bbox(seg, &x0, &y0, &x1, &y1);
    m->x0 = x0;
    m->y0 = y0;
    m->w = x1 - x0 + 1;
    m->h = y1 - y0 + 1;
    m->cov = heap_caps_calloc((size_t)m->w * m->h, 1, MALLOC_CAP_SPIRAM);
    if (m->cov == NULL) {
        return false;
    }
    for (int ty = y0; ty <= y1; ty += 8) {
        const int ty1 = (ty + 7 < y1) ? ty + 7 : y1;
        for (int tx = x0; tx <= x1; tx += 8) {
            const int tx1 = (tx + 7 < x1) ? tx + 7 : x1;
            const float d = panel_seg_sdf(seg, (float)(tx + tx1 + 1) * 0.5f,
                                          (float)(ty + ty1 + 1) * 0.5f);
            if (d > 7.0f) {
                continue;
            }
            for (int y = ty; y <= ty1; y++) {
                uint8_t *row = m->cov + (size_t)(y - y0) * m->w - x0;
                for (int x = tx; x <= tx1; x++) {
                    row[x] = (d < -7.0f) ? 255
                             : (uint8_t)(panel_coverage(panel_seg_sdf(seg, x + 0.5f, y + 0.5f)) * 255.0f + 0.5f);
                }
            }
        }
    }
    return true;
}

/* Everything sits on black and nothing overlaps, so "over black" is the blend. */
static void fill_seg(panel_seg_t seg, uint32_t rgb)
{
    const bool half = seg == PANEL_SEG_HALF_TOP || seg == PANEL_SEG_HALF_BOTTOM;
    const mask_t *m = half ? &s_half : &s_quad;
    if (!s_masks_ready) {
        return;
    }
    /* Mirror about the centre: pixel p maps to PANEL_SIZE - 1 - p. */
    const bool flip_x = seg == PANEL_SEG_QUAD_TL || seg == PANEL_SEG_QUAD_BL;
    const bool flip_y = seg == PANEL_SEG_HALF_BOTTOM || seg == PANEL_SEG_QUAD_BL ||
                        seg == PANEL_SEG_QUAD_BR;
    const uint16_t solid = ui_paint_rgb565_cov(rgb, 255);
    for (int my = 0; my < m->h; my++) {
        const uint8_t *src = m->cov + (size_t)my * m->w;
        const int y = flip_y ? PANEL_SIZE - 1 - (m->y0 + my) : m->y0 + my;
        uint16_t *row = s_px + (size_t)y * s_stride;
        for (int mx = 0; mx < m->w; mx++) {
            const uint8_t c = src[mx];
            if (c == 0) {
                continue;
            }
            const int x = flip_x ? PANEL_SIZE - 1 - (m->x0 + mx) : m->x0 + mx;
            row[x] = (c == 255) ? solid : ui_paint_rgb565_cov(rgb, c);
        }
    }
}

/*
 * One side of the sound page: a side-by-side half of the disc in three bands --
 * the + button above the bridge, the - button below it, and the bridge between,
 * filled left to right to `level` (0..1) as a meter of the value the two share.
 *
 * The half's shape is the top half's mask transposed (and mirrored for the
 * right), which host/panel.sh checks is exact: pixel (x, y) of the left half
 * reads the top half's mask at (y, x). Read in mask order, written down columns.
 */
static void fill_side(bool right, uint32_t up_rgb, uint32_t down_rgb, float level)
{
    if (!s_masks_ready) {
        return;
    }
    const mask_t *m = &s_half;
    const uint32_t band_rgb[3] = {up_rgb, 0, down_rgb};
    /* The meter runs across the half's full width, outer rim to gap, left to
     * right on both sides, as the design has it. */
    const float start = right ? PANEL_C + PANEL_GAP : PANEL_C - PANEL_R;
    const float edge = start + level * (PANEL_R - PANEL_GAP);
    for (int r = 0; r < m->h; r++) {
        const int xl = m->y0 + r;                         /* x on the left half */
        const int x = right ? PANEL_SIZE - 1 - xl : xl;
        const uint8_t *src = m->cov + (size_t)r * m->w;
        const uint32_t meter = ((float)x + 0.5f < edge) ? s_col.bridge_fill : s_col.bridge_empty;
        for (int c = 0; c < m->w; c++) {
            const uint8_t cov = src[c];
            if (cov == 0) {
                continue;
            }
            const int y = m->x0 + c;
            const float dy = (float)y + 0.5f - PANEL_C;
            const uint32_t rgb = (dy < -PANEL_BRIDGE) ? band_rgb[0]
                                 : (dy > PANEL_BRIDGE) ? band_rgb[2] : meter;
            s_px[(size_t)y * s_stride + x] = ui_paint_rgb565_cov(rgb, cov);
        }
    }
}

/*
 * A disc, or with ring_w > 0 a ring of that width centred on radius r. Squared
 * distance decides everything clear of the edge, so only the one-pixel band of
 * the edge itself pays for a square root.
 */
static void fill_circle(float cx, float cy, float r, float ring_w, uint32_t rgb)
{
    const float r_in = (ring_w > 0.0f) ? r - ring_w * 0.5f : 0.0f;
    const float r_out = (ring_w > 0.0f) ? r + ring_w * 0.5f : r;
    const float solid_lo = (r_in > 1.0f) ? (r_in + 1.0f) * (r_in + 1.0f) : 0.0f;
    const float solid_hi = (r_out > 1.0f) ? (r_out - 1.0f) * (r_out - 1.0f) : -1.0f;
    const float none_lo = (r_in > 1.0f) ? (r_in - 1.0f) * (r_in - 1.0f) : -1.0f;
    const float none_hi = (r_out + 1.0f) * (r_out + 1.0f);
    const uint16_t solid = ui_paint_rgb565_cov(rgb, 255);

    const int x0 = (int)(cx - r_out - 2.0f), x1 = (int)(cx + r_out + 2.0f);
    const int y0 = (int)(cy - r_out - 2.0f), y1 = (int)(cy + r_out + 2.0f);
    for (int y = y0; y <= y1; y++) {
        if (y < 0 || y >= PANEL_SIZE) continue;
        uint16_t *row = s_px + (size_t)y * s_stride;
        const float dy = (float)y + 0.5f - cy;
        for (int x = x0; x <= x1; x++) {
            if (x < 0 || x >= PANEL_SIZE) continue;
            const float dx = (float)x + 0.5f - cx;
            const float q = dx * dx + dy * dy;
            if (q >= none_hi || q <= none_lo) {
                continue;
            }
            if (q >= solid_lo && q <= solid_hi) {
                row[x] = solid;
                continue;
            }
            float d = sqrtf(q) - r;
            if (ring_w > 0.0f) {
                d = fabsf(d) - ring_w * 0.5f;
            }
            const float cov = panel_coverage(d);
            if (cov > 0.0f) {
                row[x] = ui_paint_rgb565_cov(rgb, (uint32_t)(cov * 255.0f + 0.5f));
            }
        }
    }
}

/* ---------------- text and icons, through an LVGL layer ---------------- */

/* Centred on (cx, cy), with `space` px between letters. */
/* Centred on (cx, cy) by the line box, as the panel's labels always have been. */
static void text_spaced(lv_layer_t *L, const lv_font_t *f, uint32_t rgb, int32_t cx, int32_t cy,
                        int32_t space, const char *s)
{
    ui_paint_text(L, f, rgb, cx, cy, space, false, s);
}

static void text(lv_layer_t *L, const lv_font_t *f, uint32_t rgb, int32_t cx, int32_t cy,
                 const char *s)
{
    ui_paint_text(L, f, rgb, cx, cy, 0, false, s);
}

static int32_t text_width(const lv_font_t *f, const char *s)
{
    return ui_paint_text_width(f, 0, s);
}

static void line_w(lv_layer_t *L, uint32_t rgb, int32_t width, int32_t x1, int32_t y1,
                   int32_t x2, int32_t y2)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(rgb);
    d.width = width;
    d.round_start = 1;
    d.round_end = 1;
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    lv_draw_line(L, &d);
}

static void line(lv_layer_t *L, uint32_t rgb, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(rgb);
    d.width = 3;
    d.round_start = 1;
    d.round_end = 1;
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    lv_draw_line(L, &d);
}

static void arc(lv_layer_t *L, uint32_t rgb, int32_t cx, int32_t cy, uint16_t r,
                int32_t start, int32_t end)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = lv_color_hex(rgb);
    d.width = 3;
    d.rounded = 1;
    d.center.x = cx;
    d.center.y = cy;
    d.radius = r;
    d.start_angle = start;   /* LVGL: 0 at three o'clock, clockwise */
    d.end_angle = end;
    lv_draw_arc(L, &d);
}

/* Outline icons about 28 px across, in the reference sheet's stroked style. */
static void icon(lv_layer_t *L, ui_icon_t which, uint32_t rgb, int32_t x, int32_t y)
{
    switch (which) {
    case UI_ICON_MIC: {
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.bg_opa = LV_OPA_TRANSP;
        r.border_color = lv_color_hex(rgb);
        r.border_width = 3;
        r.border_opa = LV_OPA_COVER;
        r.radius = LV_RADIUS_CIRCLE;
        const lv_area_t a = {x - 6, y - 14, x + 6, y + 4};
        lv_draw_rect(L, &r, &a);
        arc(L, rgb, x, y - 2, 11, 0, 180);
        line(L, rgb, x, y + 9, x, y + 14);
        break;
    }
    case UI_ICON_SPEAKER_HI:
    case UI_ICON_SPEAKER_LO:
        line(L, rgb, x - 12, y - 5, x - 6, y - 5);
        line(L, rgb, x - 6, y - 5, x + 1, y - 11);
        line(L, rgb, x + 1, y - 11, x + 1, y + 11);
        line(L, rgb, x + 1, y + 11, x - 6, y + 5);
        line(L, rgb, x - 6, y + 5, x - 12, y + 5);
        line(L, rgb, x - 12, y + 5, x - 12, y - 5);
        arc(L, rgb, x + 2, y, 7, 315, 45);
        if (which == UI_ICON_SPEAKER_HI) {
            arc(L, rgb, x + 2, y, 13, 315, 45);
        }
        break;
    case UI_ICON_PLUS:
        line(L, rgb, x - 8, y, x + 8, y);
        line(L, rgb, x, y - 8, x, y + 8);
        break;
    case UI_ICON_MINUS:
        line(L, rgb, x - 8, y, x + 8, y);
        break;
    case UI_ICON_X:
        line(L, rgb, x - 8, y - 8, x + 8, y + 8);
        line(L, rgb, x - 8, y + 8, x + 8, y - 8);
        break;
    case UI_ICON_LEFT:
        line(L, rgb, x + 4, y - 8, x - 4, y);
        line(L, rgb, x - 4, y, x + 4, y + 8);
        break;
    case UI_ICON_RIGHT:
        line(L, rgb, x - 4, y - 8, x + 4, y);
        line(L, rgb, x + 4, y, x - 4, y + 8);
        break;
    case UI_ICON_CHECK:
        line(L, rgb, x - 8, y, x - 2, y + 6);
        line(L, rgb, x - 2, y + 6, x + 9, y - 6);
        break;
    case UI_ICON_WAVE: {
        static const int8_t h[5] = {4, 9, 13, 7, 3};
        for (int i = 0; i < 5; i++) {
            const int32_t lx = x - 12 + i * 6;
            line(L, rgb, lx, y - h[i], lx, y + h[i]);
        }
        break;
    }
    case UI_ICON_CLOCK:
        arc(L, rgb, x, y, 12, 0, 360);
        line(L, rgb, x, y, x, y - 7);
        line(L, rgb, x, y, x + 5, y + 3);
        break;
    case UI_ICON_BELL_OFF:
        /* A bell -- dome, flared sides, rim, clapper -- struck through. */
        arc(L, rgb, x, y - 3, 8, 180, 360);
        line(L, rgb, x - 8, y - 3, x - 10, y + 6);
        line(L, rgb, x + 8, y - 3, x + 10, y + 6);
        line(L, rgb, x - 12, y + 6, x + 12, y + 6);
        arc(L, rgb, x, y + 9, 3, 0, 180);
        line(L, rgb, x - 12, y - 12, x + 12, y + 12);
        break;
    case UI_ICON_ALARM:
        /* An alarm clock: a face with hands, two bells, two feet. */
        arc(L, rgb, x, y + 2, 10, 0, 360);
        line(L, rgb, x, y + 2, x, y - 4);
        line(L, rgb, x, y + 2, x + 4, y + 5);
        line(L, rgb, x - 13, y - 8, x - 8, y - 13);
        line(L, rgb, x + 13, y - 8, x + 8, y - 13);
        line(L, rgb, x - 7, y + 11, x - 9, y + 14);
        line(L, rgb, x + 7, y + 11, x + 9, y + 14);
        break;
    case UI_ICON_RESET:
        /* An arrow coming round anticlockwise to its start: most of a circle,
         * open at the top left, the head at its end. */
        arc(L, rgb, x, y, 8, 270, 180);
        line(L, rgb, x - 8, y, x - 11, y - 4);
        line(L, rgb, x - 8, y, x - 4, y - 3);
        break;
    case UI_ICON_FLAG:
        line(L, rgb, x - 6, y - 9, x - 6, y + 9);
        line(L, rgb, x - 6, y - 8, x + 7, y - 4);
        line(L, rgb, x + 7, y - 4, x - 6, y);
        break;
    case UI_ICON_PLAY: {
        /* Filled, as the design draws it. */
        lv_draw_triangle_dsc_t t;
        lv_draw_triangle_dsc_init(&t);
        t.color = lv_color_hex(rgb);
        t.opa = LV_OPA_COVER;
        t.p[0].x = x - 5;
        t.p[0].y = y - 8;
        t.p[1].x = x - 5;
        t.p[1].y = y + 8;
        t.p[2].x = x + 8;
        t.p[2].y = y;
        lv_draw_triangle(L, &t);
        break;
    }
    case UI_ICON_PAUSE:
        line(L, rgb, x - 4, y - 7, x - 4, y + 7);
        line(L, rgb, x + 4, y - 7, x + 4, y + 7);
        break;
    }
}

/* ---------------- the kit, for the ring screen ---------------- */

void ui_panel_kit_palette(uint32_t tint, ui_panel_palette_t *out)
{
    set_palette(tint);
    out->primary = s_col.primary;
    out->primary_ink = s_col.primary_ink;
    out->secondary = s_col.secondary;
    out->text = s_col.text;
    out->hint = s_col.hint;
}

bool ui_panel_kit_ready(void)
{
    return s_masks_ready;
}

void ui_panel_kit_fill(uint16_t *px, uint32_t stride_px, panel_seg_t seg, uint32_t rgb)
{
    s_px = px;
    s_stride = stride_px;
    fill_seg(seg, rgb);
}

void ui_panel_kit_icon(lv_layer_t *L, ui_icon_t which, uint32_t rgb, int32_t x, int32_t y)
{
    icon(L, which, rgb, x, y);
}

/* ---------------- what each control shows ---------------- */

typedef enum { ST_PRIMARY, ST_SECONDARY, ST_PRESSED, ST_DISABLED } style_t;

static uint32_t fill_of(style_t s)
{
    switch (s) {
    case ST_PRIMARY: return s_col.primary;
    case ST_SECONDARY: return s_col.secondary;
    case ST_PRESSED: return s_col.pressed;
    default: return s_col.disabled;
    }
}

static uint32_t ink_of(style_t s, bool was_secondary)
{
    if (s == ST_DISABLED) return s_col.disabled_ink;
    if (s == ST_SECONDARY || (s == ST_PRESSED && was_secondary)) return s_col.text;
    return s_col.primary_ink;
}

static style_t style(panel_hit_t id, bool enabled, bool secondary)
{
    if (!enabled) return ST_DISABLED;
    if (s_pressed == id) return ST_PRESSED;
    return secondary ? ST_SECONDARY : ST_PRIMARY;
}

/* Is this control live right now? A press on a disabled one does nothing. */
static bool enabled(panel_hit_t id)
{
    switch (id) {
    case PANEL_HIT_MIC_UP:
        return controls_mic_gain_available() && controls_mic_gain() < AUDIO_MIC_GAIN_MAX_DB;
    case PANEL_HIT_MIC_DOWN:
        return controls_mic_gain_available() && controls_mic_gain() > AUDIO_MIC_GAIN_MIN_DB;
    case PANEL_HIT_VOL_UP:
        return controls_volume() < AUDIO_VOLUME_MAX;
    case PANEL_HIT_VOL_DOWN:
        return controls_volume() > AUDIO_VOLUME_MIN;
    case PANEL_HIT_NS:
        return controls_ns_available();
    case PANEL_HIT_CLOCK_FMT:
        return controls_clock_available();
    case PANEL_HIT_ALARM_PREV:
        return s_alarm_n > 0 && s_alarm_idx > 0;
    case PANEL_HIT_ALARM_NEXT:
        return s_alarm_n > 0 && s_alarm_idx < s_alarm_n - 1;
    case PANEL_HIT_ALARM_CANCEL:
        return s_alarm_n > 0;
    default:
        if (id >= PANEL_HIT_SLOT && id <= PANEL_HIT_SLOT_LAST) {
            return panel_color_for_slot(s_color_sub, id - PANEL_HIT_SLOT,
                                        (int)orb_colors_count()) != -1;
        }
        return false;
    }
}

/* ---------------- pages ---------------- */

/*
 * One linked pair: + above the bridge, - below, and the value they share on the
 * bridge as a level meter, its icon, its number and a small spaced label. At a
 * limit the button that cannot go further turns dark, as on the reference sheet.
 */
static void paint_pair(lv_layer_t *L, bool right, panel_hit_t up_id, panel_hit_t down_id,
                       float level, ui_icon_t ico, const char *value, const char *label)
{
    const style_t su = style(up_id, enabled(up_id), false);
    const style_t sd = style(down_id, enabled(down_id), false);
    fill_side(right, fill_of(su), fill_of(sd), level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level));

    const int32_t cx = (int32_t)(PANEL_C + (right ? 1.0f : -1.0f) * (PANEL_R + PANEL_GAP) * 0.5f);
    const int32_t cy = (int32_t)PANEL_C;
    /* The signs: bold strokes, as large as the reference draws them, hugging
     * the bridge and set in towards the centre gap rather than under the value. */
    const int32_t sx = (int32_t)(PANEL_C + (right ? 76.0f : -76.0f));
    const uint32_t ink_u = ink_of(su, false), ink_d = ink_of(sd, false);
    line_w(L, ink_u, 5, sx - 12, cy - 72, sx + 12, cy - 72);
    line_w(L, ink_u, 5, sx, cy - 84, sx, cy - 60);
    line_w(L, ink_d, 5, sx - 12, cy + 79, sx + 12, cy + 79);

    /* The bridge: icon and number as one centred pair, the label under them. */
    const int32_t icon_w = 26, gap = 8;
    const int32_t num_w = text_width(&font_panel_28, value);
    const int32_t left = cx - (icon_w + gap + num_w) / 2;
    icon(L, ico, s_col.text, left + icon_w / 2, cy - 9);
    text(L, &font_panel_28, s_col.text, left + icon_w + gap + num_w / 2, cy - 9, value);
    text_spaced(L, &lv_font_montserrat_14, s_col.text, cx, cy + 20, 2, label);
}

static void paint_sound(lv_layer_t *L)
{
    char mic[16], vol[16];
    const int g = controls_mic_gain();
    const int v = controls_volume();
    snprintf(mic, sizeof(mic), "%d dB", g);
    snprintf(vol, sizeof(vol), "%d%%", v);
    /* Volume's meter reads as its number does, out of 100. Mic gain's spans the
     * range the codec allows, so empty is the lowest gain and full the highest. */
    paint_pair(L, false, PANEL_HIT_MIC_UP, PANEL_HIT_MIC_DOWN,
               (float)(g - AUDIO_MIC_GAIN_MIN_DB) / (float)(AUDIO_MIC_GAIN_MAX_DB - AUDIO_MIC_GAIN_MIN_DB),
               UI_ICON_MIC, mic, "MIC GAIN");
    paint_pair(L, true, PANEL_HIT_VOL_UP, PANEL_HIT_VOL_DOWN, (float)v / (float)AUDIO_VOLUME_MAX,
               UI_ICON_SPEAKER_HI, vol, "VOLUME");
}

static int color_index_of(uint32_t rgb)
{
    for (size_t i = 0; i < orb_colors_count(); i++) {
        if (orb_colors_rgb(i) == rgb) {
            return (int)i;
        }
    }
    return -1;
}

static void paint_color(lv_layer_t *L, uint32_t tint_rgb)
{
    const int count = (int)orb_colors_count();
    const int current = color_index_of(tint_rgb);

    for (int slot = 0; slot < PANEL_SLOTS; slot++) {
        float x, y;
        panel_slot_center(slot, &x, &y);
        const int idx = panel_color_for_slot(s_color_sub, slot, count);
        const bool pressed = s_pressed == (panel_hit_t)(PANEL_HIT_SLOT + slot);
        if (idx == PANEL_SLOT_IS_MORE) {
            fill_circle(x, y, PANEL_SWATCH_R, 0.0f, pressed ? s_col.pressed : s_col.secondary);
            const uint32_t ink = pressed ? s_col.primary_ink : s_col.text;
            if (s_color_sub == 0) {
                char more[16];
                snprintf(more, sizeof(more), "+%d", count - (PANEL_SLOTS - 1));
                text(L, &font_panel_18, ink, (int32_t)x, (int32_t)y, more);
            } else {
                icon(L, UI_ICON_LEFT, ink, (int32_t)x, (int32_t)y);
            }
        } else if (idx < 0) {
            fill_circle(x, y, 3.0f, 0.0f, s_col.dot_off);
        } else {
            const uint32_t rgb = orb_colors_rgb((size_t)idx);
            /* Pressed shrinks it a touch: the only feedback a colour can take. */
            fill_circle(x, y, PANEL_SWATCH_R - (pressed ? 3.0f : 0.0f), 0.0f, rgb);
            if (idx == current) {
                fill_circle(x, y, PANEL_SWATCH_R + 6.0f, 3.0f, rgb);
                icon(L, UI_ICON_CHECK, s_col.primary_ink, (int32_t)x, (int32_t)y);
            }
        }
    }

    /* The centre: the colour in force, by name, in itself. */
    if (current >= 0) {
        char name[16];
        snprintf(name, sizeof(name), "%s", orb_colors_name((size_t)current));
        if (name[0] >= 'a' && name[0] <= 'z') {
            name[0] = (char)(name[0] - 'a' + 'A');
        }
        text(L, &font_panel_40, tint_rgb, (int32_t)PANEL_C, (int32_t)PANEL_C, name);
    }
    /* The picker's own two pages. */
    for (int i = 0; i < 2; i++) {
        fill_circle(PANEL_C - 8.0f + 16.0f * (float)i, PANEL_C + 52.0f, 3.0f, 0.0f,
                    i == s_color_sub ? s_col.primary : s_col.dot_off);
    }
}

static void paint_toggles(lv_layer_t *L)
{
    /* Top: noise reduction, white while it is on. */
    const bool ns_ok = controls_ns_available();
    const bool ns_on = controls_ns_on();
    const style_t st = style(PANEL_HIT_NS, ns_ok, !ns_on);
    fill_seg(PANEL_SEG_HALF_TOP, fill_of(st));
    const uint32_t ink = ink_of(st, !ns_on);
    icon(L, UI_ICON_WAVE, ink, (int32_t)PANEL_C, (int32_t)PANEL_C - 128);
    text(L, &font_panel_28, ink, (int32_t)PANEL_C, (int32_t)PANEL_C - 84, "Noise reduction");
    text(L, &font_panel_18, st == ST_PRIMARY ? s_col.primary_label : (ns_ok ? s_col.hint : s_col.disabled_ink),
         (int32_t)PANEL_C, (int32_t)PANEL_C - 50, ns_ok ? (ns_on ? "On" : "Off") : "Unavailable");

    /* Bottom: the clock format in force; a tap switches to the other. A switch
     * like the one above it, so it reads the same way: solid for 12-hour. */
    const bool clk_ok = controls_clock_available();
    const bool h12 = controls_clock_12h();
    const style_t sb = style(PANEL_HIT_CLOCK_FMT, clk_ok, !h12);
    fill_seg(PANEL_SEG_HALF_BOTTOM, fill_of(sb));
    const uint32_t inkb = ink_of(sb, !h12);
    text(L, &font_panel_28, inkb, (int32_t)PANEL_C, (int32_t)PANEL_C + 62, h12 ? "12-hour" : "24-hour");
    text(L, &font_panel_18, sb == ST_PRIMARY ? s_col.primary_label : (clk_ok ? s_col.hint : s_col.disabled_ink),
         (int32_t)PANEL_C, (int32_t)PANEL_C + 96, h12 ? "tap for 24-hour" : "tap for 12-hour");
    icon(L, UI_ICON_CLOCK, inkb, (int32_t)PANEL_C, (int32_t)PANEL_C + 136);
}

static void fmt_countdown(int64_t s, char *out, size_t n)
{
    if (s >= 3600) {
        snprintf(out, n, "%d:%02d:%02d", (int)(s / 3600), (int)((s / 60) % 60), (int)(s % 60));
    } else {
        snprintf(out, n, "%d:%02d", (int)(s / 60), (int)(s % 60));
    }
}

/* Seconds left on a timer now, counted on from its snapshot. */
static int64_t timer_left(const alarm_view_t *v, uint32_t now)
{
    const int32_t since = (int32_t)(now - s_alarms_ms);
    const int64_t left = v->secs_left - (since > 0 ? since / 1000 : 0);
    return left > 0 ? left : 0;
}

static void paint_alarms(lv_layer_t *L, uint32_t now)
{
    if (s_alarm_n == 0) {
        text(L, &font_panel_28, s_col.hint, (int32_t)PANEL_C, (int32_t)PANEL_C - 16, "No timers");
        text(L, &font_panel_28, s_col.hint, (int32_t)PANEL_C, (int32_t)PANEL_C + 20, "or alarms");
        return;
    }
    if (s_alarm_idx >= s_alarm_n) {
        s_alarm_idx = s_alarm_n - 1;
    }
    const alarm_view_t *v = &s_alarms[s_alarm_idx];

    char count[32], big[24], when[32];
    snprintf(count, sizeof(count), "%d of %d", s_alarm_idx + 1, s_alarm_n);
    if (v->timer) {
        fmt_countdown(timer_left(v, now), big, sizeof(big));
        snprintf(when, sizeof(when), "timer");
    } else {
        snprintf(big, sizeof(big), "%s", v->time);
        snprintf(when, sizeof(when), "%s", v->when);
    }
    text(L, &font_panel_18, s_col.hint, (int32_t)PANEL_C, 80, count);
    text(L, &font_panel_40, s_col.text, (int32_t)PANEL_C, 128, big);
    text(L, &font_panel_18, s_col.hint, (int32_t)PANEL_C, 170, when);
    if (v->label[0]) {
        text(L, &font_panel_18, s_col.text, (int32_t)PANEL_C, 196, v->label);
    }

    for (int side = 0; side < 2; side++) {
        const panel_hit_t id = side ? PANEL_HIT_ALARM_NEXT : PANEL_HIT_ALARM_PREV;
        const style_t st = style(id, enabled(id), true);
        const float x = PANEL_C + (side ? PANEL_NAV_X : -PANEL_NAV_X);
        fill_circle(x, PANEL_NAV_Y, PANEL_NAV_R, 0.0f, fill_of(st));
        icon(L, side ? UI_ICON_RIGHT : UI_ICON_LEFT, ink_of(st, true), (int32_t)x, (int32_t)PANEL_NAV_Y);
    }

    /* Cancel, in two taps: the first arms it and says so, white. */
    const bool armed = s_cancel_armed_id == v->id;
    const style_t sc = (s_pressed == PANEL_HIT_ALARM_CANCEL) ? ST_PRESSED
                       : (armed ? ST_PRIMARY : ST_SECONDARY);
    fill_seg(PANEL_SEG_HALF_BOTTOM, fill_of(sc));
    const uint32_t ink = ink_of(sc, !armed);
    text(L, &font_panel_28, ink, (int32_t)PANEL_C, (int32_t)PANEL_C + 62, armed ? "Cancel?" : "Cancel");
    icon(L, UI_ICON_X, ink, (int32_t)PANEL_C, (int32_t)PANEL_C + 104);
    if (armed) {
        text(L, &font_panel_18, s_col.primary_label, (int32_t)PANEL_C, (int32_t)PANEL_C + 140, "tap again");
    }
}

static void paint(uint32_t now, uint32_t tint_rgb)
{
    set_palette(tint_rgb);
    const int64_t t0 = esp_timer_get_time();
    lv_draw_buf_t *buf = lv_canvas_get_draw_buf(s_canvas);
    s_px = (uint16_t *)buf->data;
    s_stride = buf->header.stride / sizeof(uint16_t);

    clear();
    const int64_t t1 = esp_timer_get_time();
    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);
    switch (s_page) {
    case PANEL_PAGE_SOUND: paint_sound(&layer); break;
    case PANEL_PAGE_COLOR: paint_color(&layer, tint_rgb); break;
    case PANEL_PAGE_TOGGLES: paint_toggles(&layer); break;
    case PANEL_PAGE_ALARMS: paint_alarms(&layer, now); break;
    default: break;
    }
    const int64_t t2 = esp_timer_get_time();
    lv_canvas_finish_layer(s_canvas, &layer);
    const int64_t t3 = esp_timer_get_time();

    /* Which page of the panel, in the rim below the disc on every page. */
    for (int i = 0; i < PANEL_PAGE_COUNT; i++) {
        fill_circle(PANEL_C + 14.0f * ((float)i - (PANEL_PAGE_COUNT - 1) / 2.0f), PAGE_DOTS_Y,
                    3.0f, 0.0f, i == (int)s_page ? s_col.primary : s_col.dot_off);
    }

    lv_obj_invalidate(s_canvas);
#if CONFIG_UI_PANEL_SELFTEST
    /* Every repaint the self-test causes, as an image: host/shot2png.py. A paint
     * that lands while the last capture is still printing is not captured. */
    static unsigned shot;
    char tag[24];
    snprintf(tag, sizeof(tag), "panel-p%d-%02u", (int)s_page, shot);
    if (screenshot_capture(s_px, s_stride, PANEL_SIZE, PANEL_SIZE, tag)) {
        shot++;
    }
#endif
    /* clear / shapes (and queuing the text) / text and icons, in ms. */
    ESP_LOGI(TAG, "EVT panel draw page=%d %" PRId64 " ms (clear %" PRId64 " shapes %" PRId64
                  " text %" PRId64 ")",
             (int)s_page, (esp_timer_get_time() - t0) / 1000, (t1 - t0) / 1000,
             (t2 - t1) / 1000, (t3 - t2) / 1000);
}

/*
 * A cheap fingerprint of everything a page shows that can change under it --
 * by voice, by BOOT, or with time. Different from the last paint means repaint.
 */
static uint32_t signature(uint32_t now, uint32_t tint_rgb)
{
    uint32_t h = (uint32_t)s_page * 2654435761u ^ tint_rgb;   /* every page wears the colour */
    switch (s_page) {
    case PANEL_PAGE_SOUND:
        h ^= (uint32_t)controls_volume() << 8 ^ (uint32_t)controls_mic_gain() << 16;
        break;
    case PANEL_PAGE_COLOR:
        h ^= (uint32_t)s_color_sub << 24;
        break;
    case PANEL_PAGE_TOGGLES:
        h ^= (uint32_t)controls_ns_on() << 3 ^ (uint32_t)controls_clock_12h() << 5;
        break;
    case PANEL_PAGE_ALARMS: {
        h ^= (uint32_t)s_alarm_n << 4 ^ (uint32_t)s_alarm_idx << 12 ^ (uint32_t)s_cancel_armed_id << 20;
        if (s_alarm_n > 0 && s_alarm_idx < s_alarm_n && s_alarms[s_alarm_idx].timer) {
            h ^= (uint32_t)timer_left(&s_alarms[s_alarm_idx], now) * 40503u;
        }
        for (int i = 0; i < s_alarm_n; i++) {
            h = h * 31u + s_alarms[i].id;
        }
        break;
    }
    default:
        break;
    }
    return h;
}

static void refresh_alarms(uint32_t now)
{
    s_alarm_n = controls_alarms(s_alarms, ALARMS_MAX);
    s_alarms_ms = now;
    if (s_alarm_idx >= s_alarm_n) {
        s_alarm_idx = s_alarm_n > 0 ? s_alarm_n - 1 : 0;
    }
}

/* ---------------- actions ---------------- */

static void act(panel_hit_t id)
{
    if (!enabled(id)) {
        return;
    }
    switch (id) {
    case PANEL_HIT_MIC_UP: controls_mic_gain_step(+1); break;
    case PANEL_HIT_MIC_DOWN: controls_mic_gain_step(-1); break;
    case PANEL_HIT_VOL_UP: controls_volume_step(+1); break;
    case PANEL_HIT_VOL_DOWN: controls_volume_step(-1); break;
    case PANEL_HIT_NS: controls_ns_toggle(); break;
    case PANEL_HIT_CLOCK_FMT: controls_clock_toggle_12h(); break;
    case PANEL_HIT_ALARM_PREV: s_alarm_idx--; s_cancel_armed_id = 0; break;
    case PANEL_HIT_ALARM_NEXT: s_alarm_idx++; s_cancel_armed_id = 0; break;
    case PANEL_HIT_ALARM_CANCEL: {
        const uint8_t id_now = s_alarms[s_alarm_idx].id;
        if (s_cancel_armed_id == id_now) {
            s_cancel_armed_id = 0;
            controls_alarm_cancel(id_now);
            refresh_alarms(now_ms());
        } else {
            s_cancel_armed_id = id_now;
            s_cancel_armed_ms = now_ms();
        }
        break;
    }
    default: {
        const int idx = panel_color_for_slot(s_color_sub, id - PANEL_HIT_SLOT,
                                             (int)orb_colors_count());
        if (idx == PANEL_SLOT_IS_MORE) {
            s_color_sub ^= 1;
        } else if (idx >= 0) {
            ESP_LOGI(TAG, "EVT ctl color %s", orb_colors_name((size_t)idx));
            ui_set_orb_color(idx);   /* applied by the frame timer; main.c saves it */
        }
        break;
    }
    }
    s_dirty = true;
}

static void set_page(int page)
{
    s_page = (panel_page_t)((page + PANEL_PAGE_COUNT) % PANEL_PAGE_COUNT);
    s_cancel_armed_id = 0;
    if (s_page == PANEL_PAGE_ALARMS) {
        s_alarm_idx = 0;
        refresh_alarms(now_ms());
    }
    ESP_LOGI(TAG, "EVT panel page %d", (int)s_page);
    s_dirty = true;
}

static void open_panel(void)
{
    s_open = true;
    s_pressed = PANEL_HIT_NONE;
    s_color_sub = 0;
    s_touch_ms = now_ms();
    ESP_LOGI(TAG, "EVT panel open");
    set_page(PANEL_PAGE_SOUND);
}

void ui_panel_close(const char *why)
{
    if (!s_open) {
        return;
    }
    s_open = false;
    s_owned = false;
    s_pressed = PANEL_HIT_NONE;
    s_cancel_armed_id = 0;
    controls_commit(now_ms(), 0);
    /* Hand the face a black canvas: the orb repaints every dot each frame and the
     * spectrum clears its own, so this is all either needs to come back whole. */
    lv_canvas_fill_bg(s_canvas, lv_color_black(), LV_OPA_COVER);
    lv_obj_invalidate(s_canvas);
    ESP_LOGI(TAG, "EVT panel close (%s)", why);
}

/* ---------------- touch ---------------- */

bool ui_panel_pressed(int32_t x, int32_t y, bool can_open)
{
    s_x0 = s_x = x;
    s_y0 = s_y = y;
    s_open_gesture = false;
    if (!s_open) {
        s_opening = can_open && s_masks_ready && y < OPEN_BAND_PX;
        s_owned = false;
        return false;
    }
    s_opening = false;
    s_owned = true;
    s_touch_ms = now_ms();
    const panel_hit_t hit = panel_hit(s_page, (float)x, (float)y);
    s_pressed = enabled(hit) ? hit : PANEL_HIT_NONE;
    if (s_pressed != PANEL_HIT_NONE) {
        s_dirty = true;
    }
    return true;
}

void ui_panel_pressing(int32_t x, int32_t y)
{
    s_x = x;
    s_y = y;
    if (s_opening) {
        const int32_t dy = y - s_y0;
        const int32_t dx = x - s_x0;
        if (dy >= SWIPE_PX && (dx < 0 ? -dx : dx) < dy) {
            s_opening = false;
            s_open_gesture = true;
            open_panel();
        }
        return;
    }
    if (s_open && s_owned) {
        s_touch_ms = now_ms();
        const int32_t dx = x - s_x0, dy = y - s_y0;
        if (s_pressed != PANEL_HIT_NONE && dx * dx + dy * dy > TAP_SLOP_PX * TAP_SLOP_PX) {
            s_pressed = PANEL_HIT_NONE;   /* it is becoming a swipe */
            s_dirty = true;
        }
    }
}

bool ui_panel_released(bool lost)
{
    s_opening = false;
    if (s_open_gesture) {
        s_open_gesture = false;
        return true;
    }
    if (!s_open || !s_owned) {
        return false;
    }
    s_owned = false;
    const panel_hit_t pressed = s_pressed;
    if (pressed != PANEL_HIT_NONE) {
        s_pressed = PANEL_HIT_NONE;
        s_dirty = true;
    }
    if (lost) {
        return true;
    }
    const int32_t dx = s_x - s_x0, dy = s_y - s_y0;
    const int32_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx >= SWIPE_PX && adx > 2 * ady) {
        /* Finger moving left brings the next page in from the right. */
        set_page((int)s_page + (dx < 0 ? 1 : -1));
    } else if (dy <= -SWIPE_PX && ady > adx) {
        ui_panel_close("swipe");
    } else if (adx * adx + ady * ady <= TAP_SLOP_PX * TAP_SLOP_PX && pressed != PANEL_HIT_NONE) {
        act(pressed);
    }
    return true;
}

/* ---------------- self-test ---------------- */

#if CONFIG_UI_PANEL_SELFTEST
/*
 * Drives the panel through every page and control during a live session, through
 * the same hit tests a finger uses, and puts back whatever it changed. Exists so
 * the panel's timing, RAM and actions can be checked from the serial log with
 * nobody touching the board. Cancel is only ever armed, never confirmed.
 */
static void tap(float x, float y)
{
    const panel_hit_t hit = panel_hit(s_page, x, y);
    ESP_LOGI(TAG, "EVT panel selftest tap (%d,%d) -> hit %d", (int)x, (int)y, (int)hit);
    s_touch_ms = now_ms();
    act(hit);
}

static void tap_slot(int slot)
{
    float x, y;
    panel_slot_center(slot, &x, &y);
    tap(x, y);
}

static void selftest(uint32_t now, bool session_live, uint32_t tint_rgb)
{
    static uint32_t live_since, next_ms;
    static int step = -1;
    static int color_back;   /* the slot that holds the colour in force at the start */
    static int color_back_sub;
    if (step == -2) {
        return;   /* done */
    }
    if (!session_live) {
        live_since = 0;
        return;
    }
    if (live_since == 0) {
        live_since = now;
    }
    if (step == -1) {
        if (now - live_since < 10000) {
            return;
        }
        step = 0;
        next_ms = now;
    }
    if ((int32_t)(now - next_ms) < 0) {
        return;
    }
    next_ms = now + 1500;
    const float c = PANEL_C;
    ESP_LOGI(TAG, "EVT panel selftest step %d", step);
    /*
     * Steps 1 and 2 show the sound page at its limits -- volume at the top, mic
     * gain at the bottom, so one + and one - are disabled -- and put both back.
     * First, before any other change, so the deferred save can only ever write
     * the restored values. Nothing is playing yet, so the 100 is silent.
     */
    static int vol_back, mic_back;
    const int k = step++;
    if (k == 1) {
        vol_back = controls_volume();
        mic_back = controls_mic_gain();
        audio_io_set_volume_live(AUDIO_VOLUME_MAX);
        audio_io_set_mic_gain_live(AUDIO_MIC_GAIN_MIN_DB);
        s_dirty = true;
        return;
    }
    if (k == 2) {
        audio_io_set_volume_live(vol_back);
        audio_io_set_mic_gain_live(mic_back);
        s_dirty = true;
        return;
    }
    switch (k > 2 ? k - 2 : k) {
    case 0: open_panel(); break;
    case 1: tap(c + 90, c - 90); break;      /* volume + */
    case 2: tap(c + 90, c + 90); break;      /* volume - */
    case 3: tap(c - 90, c - 90); break;      /* mic + */
    case 4: tap(c - 90, c + 90); break;      /* mic - */
    case 5: {
        set_page(PANEL_PAGE_COLOR);
        const int cur = color_index_of(tint_rgb);
        if (!panel_slot_for_color(cur, (int)orb_colors_count(), &color_back_sub, &color_back)) {
            color_back_sub = 0;
            color_back = 0;
        }
        break;
    }
    case 6: tap_slot(3); break;              /* orange */
    case 7: tap_slot(PANEL_SLOT_MORE); break;
    case 8: tap_slot(1); break;              /* pink */
    case 9:
        if (s_color_sub != color_back_sub) {
            tap_slot(PANEL_SLOT_MORE);
        }
        break;
    case 10: tap_slot(color_back); break;    /* put it back */
    case 11: set_page(PANEL_PAGE_TOGGLES); break;
    case 12: tap(c, c - 100); break;         /* noise reduction */
    case 13: tap(c, c - 100); break;         /* ... and back */
    case 14: tap(c, c + 100); break;         /* clock format */
    case 15: tap(c, c + 100); break;         /* ... and back */
    case 16: set_page(PANEL_PAGE_ALARMS); break;
    case 17: tap(c + PANEL_NAV_X, PANEL_NAV_Y); break;
    case 18: tap(c - PANEL_NAV_X, PANEL_NAV_Y); break;
    case 19: tap(c, c + 100); break;         /* arm cancel; it lapses unconfirmed */
    case 20: ui_panel_close("selftest"); break;
    default:
        ESP_LOGI(TAG, "EVT panel selftest done");
        step = -2;
        break;
    }
}
#endif

/* ---------------- frame ---------------- */

/*
 * The masks take ~0.5 s to make, which on the LVGL task -- where init runs, with
 * the lock held -- would freeze the first frames of boot. So a one-shot task on
 * the other core makes them at the lowest priority and goes away; the panel
 * simply cannot open until they exist, which nobody can try for seconds anyway.
 */
static void mask_task(void *arg)
{
    (void)arg;
    const int64_t t0 = esp_timer_get_time();
    const bool ok = build_mask(PANEL_SEG_HALF_TOP, &s_half) && build_mask(PANEL_SEG_QUAD_TR, &s_quad);
    if (ok) {
        ESP_LOGI(TAG, "shape masks %d + %d B PSRAM in %" PRId64 " ms",
                 s_half.w * s_half.h, s_quad.w * s_quad.h, (esp_timer_get_time() - t0) / 1000);
        s_masks_ready = true;
    } else {
        ESP_LOGE(TAG, "no PSRAM for the panel's shape masks; the panel stays shut");
    }
    vTaskDelete(NULL);
}

void ui_panel_init(lv_obj_t *canvas)
{
    s_canvas = canvas;
    if (xTaskCreatePinnedToCore(mask_task, "panel_mask", 3072, NULL, 1, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "no task for the panel's shape masks; the panel stays shut");
    }
}

bool ui_panel_is_open(void)
{
    return s_open;
}

int ui_panel_page(void)
{
    return s_open ? (int)s_page : -1;
}

bool ui_panel_frame(uint32_t now, uint32_t tint_rgb, bool session_live)
{
    controls_commit(now, SAVE_QUIET_MS);
#if CONFIG_UI_PANEL_SELFTEST
    selftest(now, session_live, tint_rgb);
#else
    (void)session_live;
#endif
    if (!s_open) {
        return false;
    }
    /* Signed: a touch handled after this frame began stamps a time later than
     * `now`, and unsigned that difference reads as fifty days of idling. */
    if (!s_owned && (int32_t)(now - s_touch_ms) > IDLE_CLOSE_MS) {
        ui_panel_close("idle");
        return false;
    }
    if (s_page == PANEL_PAGE_ALARMS && (int32_t)(now - s_alarms_ms) >= 1000) {
        refresh_alarms(now);   /* a timer may have fired, or one been set by voice */
    }
    if (s_cancel_armed_id != 0 && (int32_t)(now - s_cancel_armed_ms) > CANCEL_ARM_MS) {
        s_cancel_armed_id = 0;
    }
    const uint32_t sig = signature(now, tint_rgb);
    if (s_dirty || sig != s_sig) {
        s_dirty = false;
        s_sig = sig;
        paint(now, tint_rgb);
    }
    return true;
}
