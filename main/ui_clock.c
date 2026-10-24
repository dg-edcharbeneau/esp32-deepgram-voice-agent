/*
 * The orb clock's painter. See ui_clock.h for what it is and clock_face.h for
 * the geometry.
 *
 * PAINTED STRAIGHT INTO THE CANVAS, like the touch panel and for the same
 * reasons: 250-odd anti-aliased dots and a few hairline icons are cheap as
 * per-pixel arithmetic and expensive as LVGL draw calls, each of which takes
 * mask buffers from internal RAM. Only the text goes through an LVGL layer.
 *
 * NOT orb_raster.c, though it blits dots: it takes one tint per frame, where the
 * clock has two dot colours, and its sprites stop at a 9 px radius, which the
 * pulsing halo passes. Its parity-checked code stays the orb face's alone.
 *
 * EVERY FRAME FROM BLACK. The canvas is cleared whole and everything redrawn:
 * at the sleep rate of 5 fps that is cheaper to get right than tracking what
 * moved, and the twinkle means nearly every dot changes every frame anyway.
 */
#include "ui_clock.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "clock_face.h"
#include "ui_tone.h"

static const char *TAG = "clock";

LV_FONT_DECLARE(font_clock_52);
LV_FONT_DECLARE(font_clock_15);

/* ---------------- the mockup's palette, on a white orb ---------------- */

#define COL_SPHERE  0xCFE3FF
#define COL_RING    0xF2F4F7
#define COL_DIGITS  0xF2F4F7
#define COL_STATUS  0xC9CED6   /* AM/PM, the Wi-Fi and battery row */
#define COL_LOW     0xF85044   /* battery fill at or under BATTERY_LOW_PCT; stays red */

/* ---------------- the mockup's layout ---------------- */

#define STATUS_TOP     104     /* the row's top edge */
#define STATUS_H       20      /* the Wi-Fi glyph is the tallest thing in it */
#define STATUS_GAP     10
#define WIFI_W         20
#define BAT_W          24      /* body, outline included */
#define BAT_H          12
#define BAT_BORDER     2
#define BAT_RADIUS     3
#define BAT_FILL_MAX   18      /* fill width at 100 % */
#define BAT_FILL_H     6
#define BAT_NUB_GAP    2
#define BAT_NUB_W      2
#define BAT_NUB_H      5
#define BOLT_GAP       4       /* not in the mockup: today's charging bolt, kept */
#define BOLT_HW        4
#define BOLT_HH        7

#define TIME_H         52      /* the digits' line box: line-height 1 */
#define TIME_GAP       2
#define AMPM_H         18      /* 15 px at the normal line height */
#define AMPM_SPACE     2       /* 0.14 em of 15 px */
#define NEXT_BELOW     22      /* the next-alarm line's centre under AM/PM's */
#define TEXT_SOFT      14.0f   /* the mockup's 14 px text-shadow, as a fade */

#define DRIFT_PX       3.0f    /* burn-in drift: how far from centre, at most */
#define DRIFT_X_MS     (17u * 60u * 1000u)   /* and how slowly, per axis: two */
#define DRIFT_Y_MS     (23u * 60u * 1000u)   /* periods that never line up */

/* ---------------- state ---------------- */

static lv_obj_t *s_canvas;
static uint16_t *s_px;
static uint32_t s_stride;
static int s_w, s_h;
static clock_frame_t *s_frame;   /* PSRAM: 5 kB is too much for the LVGL stack */
static bool s_running;
static uint32_t s_t0_ms;

/* ---------------- pixels ---------------- */

static inline uint16_t pack565(uint32_t r, uint32_t g, uint32_t b)
{
    /* Rounded, not truncated: truncating tints neutral greys green. */
    return (uint16_t)((((r * 31 + 127) / 255) << 11) | (((g * 63 + 127) / 255) << 5) |
                      ((b * 31 + 127) / 255));
}

/* Source-over, `a` of 0..255. */
static inline void blend(int x, int y, uint32_t rgb, uint32_t a)
{
    if (a == 0 || x < 0 || y < 0 || x >= s_w || y >= s_h) {
        return;
    }
    uint16_t *p = s_px + (size_t)y * s_stride + x;
    const uint32_t d = *p;
    const uint32_t dr = (((d >> 11) & 31) * 255 + 15) / 31;
    const uint32_t dg = (((d >> 5) & 63) * 255 + 31) / 63;
    const uint32_t db = ((d & 31) * 255 + 15) / 31;
    const uint32_t na = 255 - a;
    *p = pack565((((rgb >> 16) & 0xFF) * a + dr * na + 127) / 255,
                 (((rgb >> 8) & 0xFF) * a + dg * na + 127) / 255,
                 ((rgb & 0xFF) * a + db * na + 127) / 255);
}

/* How much of a pixel a shape covers at signed distance d: a one-pixel ramp,
 * as panel_coverage(). */
static inline float coverage(float d)
{
    const float c = 0.5f - d;
    return c <= 0.0f ? 0.0f : (c >= 1.0f ? 1.0f : c);
}

static inline uint32_t a8(float a)
{
    return a <= 0.0f ? 0 : (a >= 1.0f ? 255 : (uint32_t)(a * 255.0f + 0.5f));
}

/* A filled disc, or with r_in > 0 an annulus. */
static void disc(float cx, float cy, float r_in, float r_out, uint32_t rgb, float alpha)
{
    const int x0 = (int)floorf(cx - r_out - 1.0f), x1 = (int)ceilf(cx + r_out + 1.0f);
    const int y0 = (int)floorf(cy - r_out - 1.0f), y1 = (int)ceilf(cy + r_out + 1.0f);
    for (int y = y0; y <= y1; y++) {
        const float dy = (float)y + 0.5f - cy;
        for (int x = x0; x <= x1; x++) {
            const float dx = (float)x + 0.5f - cx;
            const float dist = sqrtf(dx * dx + dy * dy);
            float cov = coverage(dist - r_out);
            if (r_in > 0.0f) {
                cov = fminf(cov, 1.0f - coverage(dist - r_in));
            }
            blend(x, y, rgb, a8(cov * alpha));
        }
    }
}

/* A stroked arc about straight up, +-half radians, round caps. */
static void arc_up(float cx, float cy, float r, float half, float w, uint32_t rgb)
{
    const float ex = r * sinf(half), ey = r * cosf(half);   /* the right-hand end */
    const int x0 = (int)floorf(cx - ex - w), x1 = (int)ceilf(cx + ex + w);
    const int y0 = (int)floorf(cy - r - w), y1 = (int)ceilf(cy - ey + w);
    for (int y = y0; y <= y1; y++) {
        const float py = (float)y + 0.5f - cy;
        for (int x = x0; x <= x1; x++) {
            const float px = (float)x + 0.5f - cx;
            float d;
            if (py < 0.0f && fabsf(atan2f(px, -py)) <= half) {
                d = fabsf(sqrtf(px * px + py * py) - r);
            } else {
                const float ax = fabsf(px) - ex, ay = py + ey;   /* nearer end */
                d = sqrtf(ax * ax + ay * ay);
            }
            blend(x, y, rgb, a8(coverage(d - w * 0.5f)));
        }
    }
}

/* Signed distance to a rounded box, negative inside. */
static float rbox(float px, float py, float x0, float y0, float x1, float y1, float rad)
{
    const float hx = (x1 - x0) * 0.5f - rad, hy = (y1 - y0) * 0.5f - rad;
    const float qx = fabsf(px - (x0 + x1) * 0.5f) - hx, qy = fabsf(py - (y0 + y1) * 0.5f) - hy;
    const float ox = fmaxf(qx, 0.0f), oy = fmaxf(qy, 0.0f);
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.0f) - rad;
}

/* A rounded box, filled, or with border > 0 only its border, drawn inside the
 * edge the way CSS's border-box draws it. */
static void box(float x0, float y0, float x1, float y1, float rad, float border, uint32_t rgb)
{
    for (int y = (int)floorf(y0) - 1; y <= (int)ceilf(y1); y++) {
        for (int x = (int)floorf(x0) - 1; x <= (int)ceilf(x1); x++) {
            float d = rbox((float)x + 0.5f, (float)y + 0.5f, x0, y0, x1, y1, rad);
            if (border > 0.0f) {
                d = fabsf(d + border * 0.5f) - border * 0.5f;
            }
            blend(x, y, rgb, a8(coverage(d)));
        }
    }
}

/* A filled triangle, anti-aliased on its edges: the nearest edge's signed
 * distance, through the same ramp as everything else. */
static void tri(float ax, float ay, float bx, float by, float cx, float cy, uint32_t rgb)
{
    const float px[3] = {ax, bx, cx}, py[3] = {ay, by, cy};
    const float wind = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax) > 0.0f ? 1.0f : -1.0f;
    const int x0 = (int)floorf(fminf(ax, fminf(bx, cx))) - 1;
    const int x1 = (int)ceilf(fmaxf(ax, fmaxf(bx, cx))) + 1;
    const int y0 = (int)floorf(fminf(ay, fminf(by, cy))) - 1;
    const int y1 = (int)ceilf(fmaxf(ay, fmaxf(by, cy))) + 1;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float inside = 1e9f;   /* distance in from the nearest edge */
            for (int e = 0; e < 3; e++) {
                const int n = (e + 1) % 3;
                const float ex = px[n] - px[e], ey = py[n] - py[e];
                const float len = sqrtf(ex * ex + ey * ey);
                const float d = wind * (ex * ((float)y + 0.5f - py[e]) - ey * ((float)x + 0.5f - px[e])) / len;
                inside = fminf(inside, d);
            }
            blend(x, y, rgb, a8(coverage(-inside)));
        }
    }
}

/* ---------------- text ---------------- */

/*
 * Where the digits' ink sits in a line box, so text is placed by what is seen
 * rather than by the box: the top and bottom of '0' below the box's top.
 */
static void ink_rows(const lv_font_t *f, uint32_t ch, int32_t *top, int32_t *bottom)
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

/* Draw `s` with its ink centred on (cx, cy); returns its inked box. */
static lv_area_t text_at(lv_layer_t *L, const lv_font_t *f, uint32_t rgb, int32_t cx, int32_t cy,
                         int32_t space, const char *s)
{
    lv_point_t sz;
    lv_text_get_size(&sz, s, f, space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t top, bottom;
    ink_rows(f, (uint32_t)(unsigned char)s[0], &top, &bottom);
    const int32_t y1 = cy - (top + bottom) / 2;
    const lv_area_t a = {cx - sz.x / 2 - 1, y1, cx + sz.x / 2 + 1, y1 + sz.y};

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

static int32_t text_width(const lv_font_t *f, const char *s)
{
    lv_point_t sz;
    lv_text_get_size(&sz, s, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

/* ---------------- the status row ---------------- */

/* The mockup's Wi-Fi glyph is a 24-unit icon drawn at 20 px: three arcs and a
 * dot, each arc's centre and reach as its SVG path puts them. Lit from the dot
 * outwards, one element per bar; the rest at the indicators' quarter brightness. */
static void wifi_glyph(float x, float y, int bars, uint32_t on, uint32_t off)
{
    const float k = (float)WIFI_W / 24.0f, w = 2.2f * k;
    static const struct { float cy, r, half; } ARCS[3] = {
        {19.07f, 5.0f, 0.775f},    /* M8.5 15.5 a5 5   */
        {19.14f, 10.0f, 0.775f},   /* M5 12 a10 10     */
        {19.68f, 15.0f, 0.730f},   /* M2 8.5 a15 15    */
    };
    disc(x + 12.0f * k, y + 19.0f * k, 0.0f, 1.2f * k + w * 0.5f, bars >= 1 ? on : off, 1.0f);
    for (int i = 0; i < 3; i++) {
        arc_up(x + 12.0f * k, y + ARCS[i].cy * k, ARCS[i].r * k, ARCS[i].half, w,
               bars >= i + 2 ? on : off);
    }
}

static void status_row(lv_layer_t *L, const ui_clock_state_t *st, float ox, float oy,
                       uint32_t ink, uint32_t ink_off, uint32_t fill)
{
    char pct[8] = "";
    int32_t pct_w = 0;
    if (st->bat_valid) {
        snprintf(pct, sizeof(pct), "%d%%", st->bat_percent);
        pct_w = text_width(&lv_font_montserrat_14, pct);
    }
    const int32_t bat_w = BAT_W + BAT_NUB_GAP + BAT_NUB_W;
    const int32_t bolt_w = (st->bat_valid && st->bat_charging) ? BOLT_GAP + 2 * BOLT_HW : 0;
    int32_t w = 0;
    if (st->sig_valid) {
        w += WIFI_W;
    }
    if (st->bat_valid) {
        w += (w ? STATUS_GAP : 0) + bat_w + STATUS_GAP + pct_w + bolt_w;
    }
    if (w == 0) {
        return;
    }

    float x = CLOCK_FACE_C + ox - (float)w * 0.5f;
    const float top = (float)STATUS_TOP + oy, mid = top + STATUS_H * 0.5f;
    if (st->sig_valid) {
        wifi_glyph(x, top, st->sig_bars, ink, ink_off);
        x += WIFI_W + STATUS_GAP;
    }
    if (!st->bat_valid) {
        return;
    }
    const float by = mid - BAT_H * 0.5f;
    box(x, by, x + BAT_W, by + BAT_H, BAT_RADIUS, BAT_BORDER, ink);
    const int pc = st->bat_percent < 0 ? 0 : (st->bat_percent > 100 ? 100 : st->bat_percent);
    const float fw = (float)BAT_FILL_MAX * (float)pc / 100.0f;
    if (fw > 0.0f) {
        const float fx = x + BAT_BORDER + 1, fy = by + BAT_BORDER + 1;
        box(fx, fy, fx + fw, fy + BAT_FILL_H, 1.0f, 0.0f, fill);
    }
    const float nx = x + BAT_W + BAT_NUB_GAP;
    box(nx, mid - BAT_NUB_H * 0.5f, nx + BAT_NUB_W, mid + BAT_NUB_H * 0.5f, 1.0f, 0.0f, ink);
    x += bat_w + STATUS_GAP;

    text_at(L, &lv_font_montserrat_14, ink, (int32_t)(x + pct_w * 0.5f), (int32_t)mid, 0, pct);
    x += pct_w;

    if (bolt_w) {
        /* A bolt: two wedges whose inner ends stop short of the far side, so
         * they join in a zig-zag. (Full-width wedges, as the 3 o'clock
         * indicator draws them, make a parallelogram at this size.) */
        const float bx = x + BOLT_GAP + BOLT_HW;
        tri(bx + BOLT_HW, mid - BOLT_HH, bx - BOLT_HW, mid + 1, bx + 1, mid + 1, ink);
        tri(bx - BOLT_HW, mid + BOLT_HH, bx + BOLT_HW, mid - 1, bx - 1, mid - 1, ink);
    }
}

/* ---------------- the frame ---------------- */

bool ui_clock_init(lv_obj_t *canvas)
{
    s_canvas = canvas;
    s_frame = heap_caps_malloc(sizeof(*s_frame), MALLOC_CAP_SPIRAM);
    if (s_frame == NULL) {
        ESP_LOGE(TAG, "no PSRAM for the orb clock");
        return false;
    }
    return true;
}

void ui_clock_leave(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;
    lv_canvas_fill_bg(s_canvas, lv_color_black(), LV_OPA_COVER);
    lv_obj_invalidate(s_canvas);
}

void ui_clock_frame(const ui_clock_state_t *st)
{
    if (s_frame == NULL) {
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    if (!s_running) {
        s_running = true;
        s_t0_ms = st->now_ms;
    }
    const float t = (float)(st->now_ms - s_t0_ms) / 1000.0f;

    lv_draw_buf_t *buf = lv_canvas_get_draw_buf(s_canvas);
    s_px = (uint16_t *)buf->data;
    s_stride = buf->header.stride / sizeof(uint16_t);
    s_w = (int)buf->header.w;
    s_h = (int)buf->header.h;
    memset(s_px, 0, (size_t)buf->header.stride * buf->header.h);

    /*
     * An AMOLED marks under a picture held for hours, so the whole clock never
     * holds still: it drifts round a 3 px Lissajous, 17 minutes across and 23
     * down. Continuous, not stepped -- the old clock jumped 3 px a minute (12
     * back on every fifth), which on a screen-wide ring of dots read as the
     * whole face jumping off centre. At most 0.02 px a second, nobody sees it
     * move; the dots and icons take the fraction, the text whole pixels.
     */
    const float two_pi = 2.0f * (float)M_PI;
    const float ox = DRIFT_PX * sinf(two_pi * (float)(st->now_ms % DRIFT_X_MS) / (float)DRIFT_X_MS);
    const float oy = DRIFT_PX * sinf(two_pi * (float)(st->now_ms % DRIFT_Y_MS) / (float)DRIFT_Y_MS);

    const uint32_t tint = st->tint_rgb;
    const uint32_t c_sphere = ui_tone_mul(tint, COL_SPHERE);
    const uint32_t c_ring = ui_tone_mul(tint, COL_RING);
    const uint32_t c_digits = ui_tone_ink(ui_tone_mul(tint, COL_DIGITS));
    const uint32_t c_status = ui_tone_ink(ui_tone_mul(tint, COL_STATUS));
    /* Unlit Wi-Fi bars: the same quarter brightness the edge indicators use. */
    const uint32_t c_status_off = ui_tone_mul(c_status, 0x404040);
    const uint32_t c_fill = st->bat_low ? COL_LOW : c_digits;
    const uint32_t c_next = ui_tone_grey(tint, UI_HINT_RGB);

    /* The text's place first: the dots under it fade, as the mockup's shadow
     * darkens them. */
    char hm[8];
    const int h = st->h12 ? ((st->hh % 12) ? (st->hh % 12) : 12) : st->hh;
    snprintf(hm, sizeof(hm), st->h12 ? "%d:%02d" : "%02d:%02d", h, st->mm);
    const int32_t col_h = st->h12 ? TIME_H + TIME_GAP + AMPM_H : TIME_H;
    const int32_t col_top = (int32_t)lroundf(CLOCK_FACE_C + oy) - col_h / 2;
    const int32_t cx = (int32_t)lroundf(CLOCK_FACE_C + ox);
    const int32_t time_cy = col_top + TIME_H / 2;
    const int32_t ampm_cy = col_top + TIME_H + TIME_GAP + AMPM_H / 2;
    const bool has_next = st->next != NULL && st->next[0] != '\0';

    /* The box the dots stay out of: the digits' ink, down to whatever line is
     * last under them. */
    const int32_t tw = text_width(&font_clock_52, hm);
    int32_t dt, db;
    ink_rows(&font_clock_52, '0', &dt, &db);
    const int32_t time_bottom = time_cy + (db - dt) / 2;
    /* Under AM/PM, or under the digits in 24-hour, by the same step. */
    const int32_t next_cy = st->h12 ? ampm_cy + NEXT_BELOW : time_bottom + NEXT_BELOW - 6;
    int32_t half_w = tw / 2;
    float fy1 = (float)(st->h12 ? ampm_cy + 6 : time_bottom);
    if (has_next) {
        const int32_t nw = text_width(&lv_font_montserrat_14, st->next);
        half_w = nw / 2 > half_w ? nw / 2 : half_w;
        fy1 = (float)(next_cy + 6);
    }
    const float fx0 = (float)(cx - half_w), fx1 = (float)(cx + half_w);
    const float fy0 = (float)(time_cy - (db - dt) / 2);

    clock_face_build(s_frame, t, st->hh, st->mm, ox, oy);
    for (int i = 0; i < 2; i++) {
        const clock_halo_t *hl = &s_frame->halo[i];
        disc(hl->x, hl->y, hl->r_in, hl->r_out, c_ring, hl->a);
    }
    for (int i = 0; i < s_frame->n; i++) {
        const clock_dot_t *d = &s_frame->dot[i];
        const float fade = clock_face_text_fade(d->x, d->y, fx0, fy0, fx1, fy1, TEXT_SOFT);
        if (fade > 0.0f) {
            disc(d->x, d->y, 0.0f, d->r, d->kind == CLOCK_DOT_SPHERE ? c_sphere : c_ring,
                 d->a * fade);
        }
    }

    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);
    status_row(&layer, st, ox, oy, c_status, c_status_off, c_fill);
    text_at(&layer, &font_clock_52, c_digits, cx, time_cy, 0, hm);
    if (st->h12) {
        text_at(&layer, &font_clock_15, c_status, cx, ampm_cy, AMPM_SPACE, st->hh < 12 ? "AM" : "PM");
    }
    if (has_next) {
        text_at(&layer, &lv_font_montserrat_14, c_next, cx, next_cy, 0, st->next);
    }
    lv_canvas_finish_layer(s_canvas, &layer);
    lv_obj_invalidate(s_canvas);

    /* Once a minute is plenty to see what a frame costs while it sleeps. */
    static int logged_mm = -1;
    if (st->mm != logged_mm) {
        logged_mm = st->mm;
        ESP_LOGI(TAG, "EVT clock draw %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
    }
}
