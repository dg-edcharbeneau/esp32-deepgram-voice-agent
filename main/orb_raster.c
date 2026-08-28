/*
 * Dots to pixels. See orb_raster.h.
 *
 * WHY A HAND-ROLLED BLITTER
 *
 * A frame is ~456 anti-aliased alpha-blended discs. Put through LVGL's draw
 * pipeline that is 456 draw descriptors, area clips and draw-unit dispatches per
 * frame; written directly into the canvas pixels -- which this module owns
 * outright -- it is a bounded number of pixel blends and one invalidated box.
 *
 * WHAT IT ACTUALLY COSTS, because the estimate that used to sit here was wrong
 * by an order of magnitude and was the stated reason not to do better.
 *
 * It claimed ~11,000 blends and "roughly 1.5 ms". Measured over 5,000 face_orb
 * log lines from real sessions: median 16.2 ms, minimum 9.8 ms, maximum 22.9 ms.
 * It never once came in under 9.8. Alongside that, from 12,339 telemetry lines on
 * this face: the frame period is a median 40 ms (25.0 fps) and the whole draw
 * callback a median 18.5 ms. So the raster is about 40% of the frame and ~88% of
 * the draw -- geometry is the other 2.2 ms.
 *
 * The old paragraph then argued from the SPECTRUM face's budget -- ~16 ms of
 * drawing inside a ~55 ms frame, ~40 ms of it LVGL's PSRAM-to-internal copy plus
 * the QSPI flush -- that no amount of rasteriser cleverness could matter. Two
 * things are wrong with borrowing it. This face's frame is 40 ms, not 55, so
 * there is ~21 ms behind the draw rather than ~40. And 16 ms of raster is not a
 * small draw next to it.
 *
 * The 11,000 figure is consistent with a mean footprint of about 5x5 px and does
 * not appear to account for blit_dot making TWO passes over each bounding box, or
 * for the sqrtf per pixel in the transition annulus -- and it predates SPRITE_MAX
 * going 14 -> 20, which roughly doubles a dot's footprint area at amplitude.
 *
 * NONE OF WHICH SAYS THE DESIGN IS WRONG. Direct-to-canvas still beats the draw
 * pipeline it replaced, and the two things this file optimises -- how many pixels
 * are touched and how many are flushed -- are still the two that matter. What is
 * no longer supported is the conclusion that the per-pixel path is too cheap to
 * be worth improving. If someone wants that back, measure it; the numbers above
 * are what to beat, and the atlas the plan originally called for was rejected on
 * the strength of the figure that turned out to be wrong.
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
 * the atlas pointless FOR CORRECTNESS: accumulate the raw edge function over the
 * bounding box, then scale so the coverage sums to pi*r^2. The edge shape is
 * preserved where it matters and the ink is exactly right at every radius, with
 * no table, no 80 kB of PSRAM, no boot-time precompute, and exact sub-pixel
 * positioning for free.
 *
 * AND THE ATLAS WAS RE-EXAMINED FOR SPEED, once the 1.5 ms above turned out to be
 * 16.2 ms. That is the number that had made the question moot, so it deserved
 * asking again rather than inheriting the old answer.
 *
 * The answer is still no, for a reason the first pass never reached: AN ATLAS
 * CANNOT TOUCH WHAT THIS COSTS. Counted over the real dot lists in
 * host/port.tsv, a shell frame is ~30k box pixels, ~5.7k of them needing the
 * sqrtf, and ~10k that actually blend -- so the old "about 11,000 pixel blends"
 * was roughly RIGHT. What was wrong by an order of magnitude is the cost of each
 * one, and the canvas is 434 kB in PSRAM. The blends are scattered
 * read-modify-writes into it, and clear_box() moves the boxes again before them;
 * at ~5k distinct cache lines per pass that is the shape of the bill. An atlas
 * removes coverage ARITHMETIC and adds atlas READS. It cannot remove a single
 * canvas write.
 *
 * What did help was noticing the boxes were 58% larger than they had to be; see
 * blit_dot(). That one is lossless, costs nothing, and cuts the clear as well.
 *
 * MEASURED ON THE DEVICE, and the estimate held. ORB_RASTER_PHASE_TIMING below
 * splits the frame; 407 samples across three behaviours say:
 *
 *              boxpx   clear    blit   raster    was    saved
 *   idle       11,575   1,973   9,503   11,476  16,165    29%
 *   listening  12,076   2,053   9,956   12,010  17,071    30%
 *   speaking   16,757   2,511  12,726   15,237  19,341    21%
 *
 * The box tightening cut box pixels 60% and the frame only 29%, and that gap is
 * what separates the two costs -- the tightening provably left the BLENDED pixel
 * count alone, so before-and-after solves for both. It gives ~270 ns per box
 * pixel and ~939 ns per blended pixel, the latter being 225 cycles at 240 MHz for
 * a two-byte read-modify-write. About 73% of the frame is that one term.
 *
 * TWO INDEPENDENT ROUTES TO THE SAME ANSWER, and the second was an accident. The
 * clear is pure memset with no arithmetic in it at all, and its cost per pixel
 * FELL from 170 ns to 150 ns as speech grew the boxes. Arithmetic per pixel
 * cannot get cheaper when there is more of it; only locality can, because a
 * bigger box fetches more useful pixels per cache line. A pure-memory signature.
 *
 * Which is also why the linear model above over-predicts a loud frame by ~11%:
 * the per-pixel costs are not constants, they improve with size. Being wrong in
 * that particular direction is more evidence for the conclusion, not less.
 */

#include "orb_raster.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

/*
 * BENCH ONLY: split the frame into its clear and blit phases and log both.
 *
 * This is the measurement the header's cost note asks for and does not have.
 * The clear is pure PSRAM writes with no arithmetic in it at all, so its cost
 * per box pixel prices every other pixel in the frame -- which is what decides
 * whether this rasteriser is memory-bound or compute-bound, and therefore
 * whether optimising the per-pixel path could ever pay.
 *
 * Off by default for the same reason face_orb.c's ORB_LOG_TIMINGS is: ESP_LOGI
 * on the LVGL task blocks on the UART for most of a frame, so the log spoils one
 * frame in every 60 and corrupts ui.c's telemetry alongside it. Turn both on
 * together, read the numbers knowing the frames they landed in are not
 * representative, and turn them off again.
 *
 * The only cost inside the measured path is one integer add per DOT for the box
 * accounting -- deliberately not per pixel, so the inner loops are untouched.
 */
#define ORB_RASTER_PHASE_TIMING 0

#if ORB_RASTER_PHASE_TIMING
#include "esp_timer.h"
static uint32_t s_dbg_boxpx;   /* box pixels this frame, summed over dots */
#endif

static const char *TAG = "orb_raster";

/*
 * Widest a dot's footprint can be.
 *
 * RAISED FROM 14 for wave's swell. The reference's rMul is the only amplitude hook
 * buildWave has, so listening can only express volume as dot size -- and 14 px
 * capped that at rMul 1.85, a 60% swell, which was not enough to read. 20 px
 * carries rMul to about 2.6.
 *
 * The cost is stack, not heap: this sizes a float cov[SPRITE_MAX * SPRITE_MAX]
 * inside blit_dot, so 14 was 784 B and 20 is 1,600 B on the LVGL task -- which has
 * 8 kB and was using 912 B for this function. Note blit_dot CLIPS a disc that does
 * not fit rather than overrunning, so exceeding this is a visual fault and never a
 * memory one.
 */
#define SPRITE_MAX 20

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

/*
 * Last frame's strokes, kept whole rather than as boxes.
 *
 * A dot is cleared by blacking its bounding box, which is a handful of pixels. A
 * line's box is not: an edge across the panel spans 200x200, so box-clearing 38
 * of them would be 1.5M pixel writes a frame against the ~11k the dots cost. So
 * a stroke is cleared by WALKING IT AGAIN in black -- the same few hundred pixels
 * it painted, and exactly symmetric with how it was drawn.
 */
static orb_line_t *s_prev_lines;
static size_t s_prev_line_count;

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
    if (s_prev_lines == NULL) {
        s_prev_lines = heap_caps_malloc(ORB_MAX_LINES * sizeof(*s_prev_lines),
                                       MALLOC_CAP_SPIRAM);
        if (s_prev_lines == NULL) {
            ESP_LOGE(TAG, "no PSRAM for the stroke list");
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
    s_prev_line_count = 0;

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
    s_prev_line_count = 0;
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
 * The ink resolves to an 8-bit luminance and `alpha` is the ring's opacity;
 * per-pixel coverage multiplies the latter. Blending is done with a 0..256
 * inverse so the inner loop shifts instead of dividing -- the resulting
 * half-level bias is well under one 5-bit RGB565 step.
 *
 * `tr`/`tg`/`tb` are the colour's channels as 0..1 scales, hoisted out of the
 * frame by the caller because they are constant across every dot in it.
 */
static void blit_dot(const orb_dot_t *d, int16_t *box, float tr, float tg, float tb)
{
    float r = d->r;
    /*
     * THE EXACT PIXEL-CENTRE BOUND, not a margin around the radius.
     *
     * Coverage is zero at d >= r + 0.5 -- the outer2 test below returns 0 there
     * -- and a pixel is sampled at its CENTRE, ix + 0.5. So pixel ix can only
     * carry ink when |ix + 0.5 - x| <= r + 0.5, which is x - r - 1 <= ix <= x + r.
     * That is this box, and nothing outside it can ever be non-zero.
     *
     * It replaces floor(x-r-1)..ceil(x+r+1), which was up to two whole columns
     * and rows of guaranteed-zero pixels on every dot. Measured over the 12,343
     * dots in host/port.tsv: 58% fewer box pixels, and the rendered canvas is
     * BYTE-IDENTICAL, because every pixel dropped had coverage exactly zero and
     * so contributed nothing to `sum` either -- the area normalisation below is
     * unchanged along with the ink.
     *
     * It pays three times over: pass one does 58% less arithmetic, pass two
     * iterates 58% fewer pixels for the same number of blends, and the box is
     * what clear_box() erases next frame, so the clear moves 58% less PSRAM.
     * See the frame-cost note at the top of this file for why that last one is
     * the part that matters.
     */
    int32_t x0 = (int32_t)ceilf(d->x - r - 1.0f);
    int32_t y0 = (int32_t)ceilf(d->y - r - 1.0f);
    int32_t x1 = (int32_t)floorf(d->x + r);
    int32_t y1 = (int32_t)floorf(d->y + r);

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

#if ORB_RASTER_PHASE_TIMING
    s_dbg_boxpx += (uint32_t)(bw * bh);
#endif

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
    float lum = (1.0f - w) * 255.0f;

    /*
     * Colour is a per-channel scale on that luminance, which is what keeps the
     * shell readable: the geometry's ink spans a real brightness ramp (measured
     * 87..249 over the parity dump's 6,384 dots), and scaling preserves it as
     * luminance instead of flattening the shell into one flat colour.
     *
     * FLOAT ON PURPOSE. The integer form (lum * ch) >> 8 is a level low at small
     * lum -- lum 8 with a full channel gives 7, which floors to a different
     * RGB565 step -- so white would stop being an exact identity and the default
     * appearance would shift. At tr = 1.0 this is bit-for-bit the expression it
     * replaced. Three multiplies per dot, in a file that calls sqrtf per pixel.
     */
    uint32_t sr = (uint32_t)(lum * tr + 0.5f) >> 3;
    uint32_t sg = (uint32_t)(lum * tg + 0.5f) >> 2;
    uint32_t sb = (uint32_t)(lum * tb + 0.5f) >> 3;

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

/* One pixel, src-over, or forced to black when clearing. Same 0..256 inverse the
 * disc blitter uses, so a stroke and a dot lay down identical ink. */
static inline void blend_px(int32_t x, int32_t y, float cov,
                            uint32_t sr, uint32_t sg, uint32_t sb, bool clear)
{
    uint16_t *px = &s_pixels[y * s_stride_px + x];
    if (clear) {
        *px = 0;
        return;
    }
    if (cov < COV_EPS) {
        return;
    }
    uint32_t a = (uint32_t)(cov * 255.0f + 0.5f);
    if (a > 255) a = 255;
    uint32_t ia = 256 - a;

    uint16_t dst = *px;
    uint32_t dr = (dst >> 11) & 0x1F;
    uint32_t dg = (dst >> 5) & 0x3F;
    uint32_t db = dst & 0x1F;

    dr = (sr * a + dr * ia) >> 8;
    dg = (sg * a + dg * ia) >> 8;
    db = (sb * a + db * ia) >> 8;
    if (dr > 0x1F) dr = 0x1F;
    if (dg > 0x3F) dg = 0x3F;
    if (db > 0x1F) db = 0x1F;

    *px = (uint16_t)((dr << 11) | (dg << 5) | db);
}

/*
 * One stroke, anti-aliased, src-over -- or blacked out when `clear` is set.
 *
 * Walked along its MAJOR AXIS rather than rasterised by bounding box: `web`'s
 * edges span most of the panel, so a box would be tens of thousands of pixels
 * for a stroke about one pixel wide. Stepping the long axis and covering only the
 * two or three pixels either side of the line keeps a stroke at a few hundred
 * pixels, the same order as a dot's footprint.
 *
 * Coverage is the perpendicular distance, not the axis distance -- the axis
 * offset is scaled by major/length. Without that a diagonal stroke inks visibly
 * heavier than an axis-aligned one of the same nominal width, because its pixels
 * are further from the centreline than their vertical offset suggests.
 */
static void stroke_line(const orb_line_t *l, uint32_t sr, uint32_t sg, uint32_t sb,
                        bool clear)
{
    float dx = l->x2 - l->x1;
    float dy = l->y2 - l->y1;
    float adx = fabsf(dx), ady = fabsf(dy);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) {
        return; /* degenerate: two nodes on top of each other */
    }

    float half = l->w * 0.5f;
    if (half < 0.35f) {
        half = 0.35f; /* a sub-pixel stroke still has to be visible */
    }

    bool x_major = adx >= ady;
    int steps = (int)ceilf(x_major ? adx : ady);
    if (steps < 1) {
        steps = 1;
    }
    /* Axis offset -> perpendicular distance. For an x-major line a pure-y
     * offset of e sits e*|dx|/len from the centreline. */
    float perp = (x_major ? adx : ady) / len;

    for (int i = 0; i <= steps; i++) {
        float f = (float)i / (float)steps;
        float px = l->x1 + dx * f;
        float py = l->y1 + dy * f;

        if (x_major) {
            int32_t x = (int32_t)lroundf(px);
            if (x < 0 || x >= s_w) {
                continue;
            }
            int32_t y0 = (int32_t)floorf(py - half - 1.0f);
            int32_t y1 = (int32_t)ceilf(py + half + 1.0f);
            if (y0 < 0) y0 = 0;
            if (y1 > s_h - 1) y1 = s_h - 1;
            for (int32_t y = y0; y <= y1; y++) {
                float e = fabsf((float)y + 0.5f - py) * perp;
                float c = half + 0.5f - e;
                if (c <= 0.0f) continue;
                if (c > 1.0f) c = 1.0f;
                blend_px(x, y, c * l->a, sr, sg, sb, clear);
            }
        } else {
            int32_t y = (int32_t)lroundf(py);
            if (y < 0 || y >= s_h) {
                continue;
            }
            int32_t x0 = (int32_t)floorf(px - half - 1.0f);
            int32_t x1 = (int32_t)ceilf(px + half + 1.0f);
            if (x0 < 0) x0 = 0;
            if (x1 > s_w - 1) x1 = s_w - 1;
            for (int32_t x = x0; x <= x1; x++) {
                float e = fabsf((float)x + 0.5f - px) * perp;
                float c = half + 0.5f - e;
                if (c <= 0.0f) continue;
                if (c > 1.0f) c = 1.0f;
                blend_px(x, y, c * l->a, sr, sg, sb, clear);
            }
        }
    }
}

void orb_raster_draw(const orb_frame_t *frame, uint32_t rgb)
{
    if (s_pixels == NULL) {
        return;
    }

#if ORB_RASTER_PHASE_TIMING
    const int64_t t_start = esp_timer_get_time();
    s_dbg_boxpx = 0;
#endif

    /* Once per frame, not once per dot: the colour cannot change mid-frame. */
    float tr = (float)((rgb >> 16) & 0xFF) / 255.0f;
    float tg = (float)((rgb >> 8) & 0xFF) / 255.0f;
    float tb = (float)(rgb & 0xFF) / 255.0f;

    /* Union of what changes, so LVGL flushes a box rather than the whole panel.
     * Seeded from last frame's marks because those have to be cleared. */
    int32_t ux0 = s_w, uy0 = s_h, ux1 = -1, uy1 = -1;

    /*
     * Last frame's strokes go first, and BEFORE this frame's dots are laid down.
     * Clearing is a black walk over the same path, so doing it after the dots
     * were drawn would punch holes through them.
     */
    for (size_t i = 0; i < s_prev_line_count; i++) {
        const orb_line_t *l = &s_prev_lines[i];
        stroke_line(l, 0, 0, 0, true);
        int32_t lx0 = (int32_t)floorf(fminf(l->x1, l->x2) - l->w - 2.0f);
        int32_t ly0 = (int32_t)floorf(fminf(l->y1, l->y2) - l->w - 2.0f);
        int32_t lx1 = (int32_t)ceilf(fmaxf(l->x1, l->x2) + l->w + 2.0f);
        int32_t ly1 = (int32_t)ceilf(fmaxf(l->y1, l->y2) + l->w + 2.0f);
        if (lx0 < 0) lx0 = 0;
        if (ly0 < 0) ly0 = 0;
        if (lx1 > s_w - 1) lx1 = s_w - 1;
        if (ly1 > s_h - 1) ly1 = s_h - 1;
        if (lx0 < ux0) ux0 = lx0;
        if (ly0 < uy0) uy0 = ly0;
        if (lx1 > ux1) ux1 = lx1;
        if (ly1 > uy1) uy1 = ly1;
    }

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

#if ORB_RASTER_PHASE_TIMING
    /* Everything above erased last frame; everything below draws this one. */
    const int64_t t_cleared = esp_timer_get_time();
#endif

    /*
     * This frame's strokes, UNDER the dots -- paintFrame's order, and the reason
     * a node reads as a node rather than as a thickening of the wire crossing it.
     */
    size_t ln = frame->line_count;
    if (ln > ORB_MAX_LINES) {
        ln = ORB_MAX_LINES;
    }
    for (size_t i = 0; i < ln; i++) {
        const orb_line_t *l = &frame->lines[i];
        /* Same ink convention as a dot, so a stroke takes the orb's colour too. */
        float lw = l->white;
        if (lw < 0.0f) lw = 0.0f;
        else if (lw > 1.0f) lw = 1.0f;
        float lum = (1.0f - lw) * 255.0f;
        uint32_t lsr = (uint32_t)(lum * tr + 0.5f) >> 3;
        uint32_t lsg = (uint32_t)(lum * tg + 0.5f) >> 2;
        uint32_t lsb = (uint32_t)(lum * tb + 0.5f) >> 3;
        stroke_line(l, lsr, lsg, lsb, false);

        s_prev_lines[i] = *l;
        int32_t lx0 = (int32_t)floorf(fminf(l->x1, l->x2) - l->w - 2.0f);
        int32_t ly0 = (int32_t)floorf(fminf(l->y1, l->y2) - l->w - 2.0f);
        int32_t lx1 = (int32_t)ceilf(fmaxf(l->x1, l->x2) + l->w + 2.0f);
        int32_t ly1 = (int32_t)ceilf(fmaxf(l->y1, l->y2) + l->w + 2.0f);
        if (lx0 < 0) lx0 = 0;
        if (ly0 < 0) ly0 = 0;
        if (lx1 > s_w - 1) lx1 = s_w - 1;
        if (ly1 > s_h - 1) ly1 = s_h - 1;
        if (lx0 < ux0) ux0 = lx0;
        if (ly0 < uy0) uy0 = ly0;
        if (lx1 > ux1) ux1 = lx1;
        if (ly1 > uy1) uy1 = ly1;
    }
    s_prev_line_count = ln;

    size_t n = frame->count;
    if (n > ORB_MAX_DOTS) {
        n = ORB_MAX_DOTS;
    }
    for (size_t i = 0; i < n; i++) {
        blit_dot(&frame->dots[i], s_prev[i], tr, tg, tb);
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

#if ORB_RASTER_PHASE_TIMING
    {
        const int64_t t_drawn = esp_timer_get_time();
        static int64_t clear_sum, draw_sum;
        static uint32_t boxpx_sum, frames;
        clear_sum += t_cleared - t_start;
        draw_sum  += t_drawn - t_cleared;
        boxpx_sum += s_dbg_boxpx;
        if (++frames >= 60) {
            /*
             * clear/ is the same box pixels as blit/ walks, written with memset
             * and no arithmetic. If the two are close, this rasteriser is bound
             * by the PSRAM canvas and the per-pixel path is not worth touching;
             * if clear is a small fraction, the arithmetic is worth attacking.
             */
            ESP_LOGI(TAG, "phase clear=%lld us blit=%lld us boxpx=%u dots=%u "
                          "-> clear %.1f ns/px, blit %.1f ns/px",
                     (long long)(clear_sum / frames), (long long)(draw_sum / frames),
                     (unsigned)(boxpx_sum / frames), (unsigned)n,
                     1000.0 * (double)clear_sum / (double)boxpx_sum,
                     1000.0 * (double)draw_sum / (double)boxpx_sum);
            clear_sum = draw_sum = 0;
            boxpx_sum = 0;
            frames = 0;
        }
    }
#endif

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
