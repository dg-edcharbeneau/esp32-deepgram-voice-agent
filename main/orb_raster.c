/*
 * Dots to pixels. See orb_raster.h.
 *
 * WHY A HAND-ROLLED BLITTER
 *
 * A frame is ~456 anti-aliased alpha-blended discs of radius 0.3 to 4.2 px. Put
 * through LVGL's draw pipeline that is 456 draw descriptors, area clips and
 * draw-unit dispatches per frame; written directly into the canvas pixels -- which
 * this module owns outright -- it is about 11,000 pixel blends, roughly 1.5 ms.
 *
 * The measured budget is what justifies keeping it this simple. On this panel the
 * spectrum face spends ~16 ms drawing inside a ~55 ms frame, and ~40 ms of that
 * frame is LVGL's PSRAM-to-internal copy plus the QSPI flush, which no amount of
 * rasteriser cleverness touches. So this file optimises the two things that do
 * matter -- how many pixels are touched, and how many are flushed -- and leaves
 * the per-pixel path obvious.
 *
 * COVERAGE, AND WHY NOT AN ATLAS
 *
 * The plan called for a precomputed A8 sprite atlas, on the grounds that the
 * naive coverage `clamp01(r + 0.5 - d)` over-covers badly below half a pixel: at
 * r = 0.3 it paints 0.8 where the true area is 0.283. Those faint far-hemisphere
 * dots are exactly what makes the shell read as a surface rather than a swarm, so
 * getting them wrong is not cosmetic.
 *
 * Normalising total coverage to the disc's true area fixes it outright, and makes
 * the atlas pointless: accumulate the raw edge function over the bounding box,
 * then scale so the coverage sums to pi*r^2. The edge shape is preserved where it
 * matters and the ink is exactly right at every radius, with no table, no 80 kB
 * of PSRAM, no boot-time precompute, and exact sub-pixel positioning for free.
 */

#include "orb_raster.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "orb_raster";

/* Widest a dot's footprint can be: radius 4.2 plus a pixel of AA either side. */
#define SPRITE_MAX 14

static lv_obj_t *s_canvas;
static uint16_t *s_pixels;
static int32_t s_stride_px;
static int32_t s_w, s_h;

/*
 * Last frame's footprints, so a frame clears only what it drew.
 *
 * The alternative is lv_canvas_fill_bg(), which is 434 kB of PSRAM writes -- about
 * 6 ms, and measurably more than everything else here put together. The union of
 * the dot boxes is ~11,000 pixels, or 23 kB. Overlapping boxes get cleared twice;
 * that is cheaper than tracking the overlap.
 */
static int16_t (*s_prev)[4];
static size_t s_prev_count;

/* Coverage below this contributes less than one 5-bit level, so skip the blend. */
#define COV_EPS (1.0f / 512.0f)

esp_err_t orb_raster_init(lv_obj_t *canvas)
{
    lv_draw_buf_t *buf = lv_canvas_get_draw_buf(canvas);
    if (buf == NULL || buf->data == NULL) {
        ESP_LOGE(TAG, "canvas has no draw buffer");
        return ESP_ERR_INVALID_STATE;
    }
    if (buf->header.cf != LV_COLOR_FORMAT_RGB565) {
        ESP_LOGE(TAG, "canvas is not RGB565 (cf=%d)", (int)buf->header.cf);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_prev == NULL) {
        /* PSRAM: only the LVGL task touches it, once per frame, sequentially --
         * and internal RAM is needed by the network stack. */
        s_prev = heap_caps_malloc(ORB_MAX_DOTS * sizeof(*s_prev), MALLOC_CAP_SPIRAM);
        if (s_prev == NULL) {
            ESP_LOGE(TAG, "no PSRAM for the dirty list");
            return ESP_ERR_NO_MEM;
        }
    }

    s_canvas = canvas;
    s_pixels = (uint16_t *)buf->data;
    /* stride is in BYTES; this blitter indexes in pixels. */
    s_stride_px = (int32_t)(buf->header.stride / sizeof(uint16_t));
    s_w = (int32_t)buf->header.w;
    s_h = (int32_t)buf->header.h;
    s_prev_count = 0;

    memset(buf->data, 0, buf->data_size);
    lv_obj_invalidate(canvas);

    ESP_LOGI(TAG, "raster %" PRId32 "x%" PRId32 ", stride %" PRId32 " px",
             s_w, s_h, s_stride_px);
    return ESP_OK;
}

void orb_raster_clear(void)
{
    if (s_pixels == NULL) {
        return;
    }
    lv_draw_buf_t *buf = lv_canvas_get_draw_buf(s_canvas);
    if (buf != NULL) {
        memset(buf->data, 0, buf->data_size);
    }
    s_prev_count = 0;
    lv_obj_invalidate(s_canvas);
}

/* Black out one box. */
static void clear_box(const int16_t *b)
{
    for (int32_t y = b[1]; y <= b[3]; y++) {
        uint16_t *row = &s_pixels[y * s_stride_px + b[0]];
        memset(row, 0, (size_t)(b[2] - b[0] + 1) * sizeof(uint16_t));
    }
}

/*
 * One dot, src-over, into RGB565.
 *
 * `g` is the 8-bit grey the ink resolves to and `alpha` the ring's opacity;
 * per-pixel coverage multiplies the latter. Blending is done with a 0..256
 * inverse so the inner loop shifts instead of dividing -- the resulting
 * half-level bias is well under one 5-bit RGB565 step.
 */
static void blit_dot(const orb_dot_t *d, int16_t *box)
{
    float r = d->r;
    int32_t x0 = (int32_t)floorf(d->x - r - 1.0f);
    int32_t y0 = (int32_t)floorf(d->y - r - 1.0f);
    int32_t x1 = (int32_t)ceilf(d->x + r + 1.0f);
    int32_t y1 = (int32_t)ceilf(d->y + r + 1.0f);

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s_w - 1) x1 = s_w - 1;
    if (y1 > s_h - 1) y1 = s_h - 1;
    if (x0 > x1 || y0 > y1) {
        box[0] = box[1] = 0;
        box[2] = box[3] = -1; /* empty */
        return;
    }

    int32_t bw = x1 - x0 + 1;
    int32_t bh = y1 - y0 + 1;
    if (bw > SPRITE_MAX || bh > SPRITE_MAX) {
        /* Geometry promises radii under ~4.2 px; refuse rather than overrun. */
        bw = bw > SPRITE_MAX ? SPRITE_MAX : bw;
        bh = bh > SPRITE_MAX ? SPRITE_MAX : bh;
        x1 = x0 + bw - 1;
        y1 = y0 + bh - 1;
    }

    /* Pass one: the raw edge function, and its total. */
    float cov[SPRITE_MAX * SPRITE_MAX];
    float sum = 0.0f;
    float ro = r + 0.5f, ri = r - 0.5f;
    float outer2 = ro * ro;
    float inner2 = (ri > 0.0f) ? ri * ri : -1.0f;
    for (int32_t y = 0; y < bh; y++) {
        float dy = ((float)(y0 + y) + 0.5f) - d->y;
        for (int32_t x = 0; x < bw; x++) {
            float dx = ((float)(x0 + x) + 0.5f) - d->x;
            float d2 = dx * dx + dy * dy;
            /* Squared-distance bounds first: only the ~1 px transition annulus
             * needs the root, so most pixels of a large dot skip it. */
            float c;
            if (d2 >= outer2) {
                c = 0.0f;
            } else if (d2 <= inner2) {
                c = 1.0f;
            } else {
                c = r + 0.5f - sqrtf(d2);
                if (c < 0.0f) c = 0.0f;
                else if (c > 1.0f) c = 1.0f;
            }
            cov[y * SPRITE_MAX + x] = c;
            sum += c;
        }
    }
    if (sum <= 0.0f) {
        box[0] = box[1] = 0;
        box[2] = box[3] = -1;
        return;
    }

    /*
     * Pass two: scale so the ink equals the disc's true area. This is what makes
     * sub-pixel dots correct -- at r = 0.3 the raw sum is ~0.8 against a true
     * area of 0.283, and painting the raw value makes the far hemisphere far too
     * heavy, which is what turns a surface back into a swarm.
     */
    float area = (float)M_PI * r * r;
    float k = area / sum;
    if (k > 1.0f) {
        k = 1.0f; /* never amplify: the edge function is already right for big r */
    }

    float w = d->white;
    if (w < 0.0f) w = 0.0f;
    else if (w > 1.0f) w = 1.0f;
    /* Dark ground, so ink is mirrored: near dots read bright. */
    uint32_t g = (uint32_t)((1.0f - w) * 255.0f + 0.5f);
    uint32_t sr = g >> 3, sg = g >> 2, sb = g >> 3;

    for (int32_t y = 0; y < bh; y++) {
        uint16_t *row = &s_pixels[(y0 + y) * s_stride_px + x0];
        for (int32_t x = 0; x < bw; x++) {
            float c = cov[y * SPRITE_MAX + x] * k * d->a;
            if (c < COV_EPS) {
                continue;
            }
            uint32_t a = (uint32_t)(c * 255.0f + 0.5f);
            if (a > 255) a = 255;
            uint32_t ia = 256 - a;

            uint16_t dst = row[x];
            uint32_t dr = (dst >> 11) & 0x1F;
            uint32_t dg = (dst >> 5) & 0x3F;
            uint32_t db = dst & 0x1F;

            dr = (sr * a + dr * ia) >> 8;
            dg = (sg * a + dg * ia) >> 8;
            db = (sb * a + db * ia) >> 8;
            if (dr > 0x1F) dr = 0x1F;
            if (dg > 0x3F) dg = 0x3F;
            if (db > 0x1F) db = 0x1F;

            row[x] = (uint16_t)((dr << 11) | (dg << 5) | db);
        }
    }

    box[0] = (int16_t)x0;
    box[1] = (int16_t)y0;
    box[2] = (int16_t)x1;
    box[3] = (int16_t)y1;
}

void orb_raster_draw(const orb_frame_t *frame)
{
    if (s_pixels == NULL) {
        return;
    }

    /* Union of what changes, so LVGL flushes a box rather than the whole panel.
     * Seeded from last frame's marks because those have to be cleared. */
    int32_t ux0 = s_w, uy0 = s_h, ux1 = -1, uy1 = -1;

    for (size_t i = 0; i < s_prev_count; i++) {
        const int16_t *b = s_prev[i];
        if (b[2] < b[0]) {
            continue;
        }
        clear_box(b);
        if (b[0] < ux0) ux0 = b[0];
        if (b[1] < uy0) uy0 = b[1];
        if (b[2] > ux1) ux1 = b[2];
        if (b[3] > uy1) uy1 = b[3];
    }

    size_t n = frame->count;
    if (n > ORB_MAX_DOTS) {
        n = ORB_MAX_DOTS;
    }
    for (size_t i = 0; i < n; i++) {
        blit_dot(&frame->dots[i], s_prev[i]);
        const int16_t *b = s_prev[i];
        if (b[2] < b[0]) {
            continue;
        }
        if (b[0] < ux0) ux0 = b[0];
        if (b[1] < uy0) uy0 = b[1];
        if (b[2] > ux1) ux1 = b[2];
        if (b[3] > uy1) uy1 = b[3];
    }
    s_prev_count = n;

    if (ux1 < ux0 || uy1 < uy0) {
        return; /* nothing drawn and nothing to clean up */
    }

    /* The canvas is an image widget, so invalidating a sub-area of it means
     * offsetting into the widget's own coordinates. It is placed at 0,0 and
     * covers the panel, so the two frames coincide. */
    lv_area_t area = {
        .x1 = ux0,
        .y1 = uy0,
        .x2 = ux1,
        .y2 = uy1,
    };
    lv_obj_invalidate_area(s_canvas, &area);
}
