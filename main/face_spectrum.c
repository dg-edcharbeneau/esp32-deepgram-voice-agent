/*
 * The radial spectrum analyzer, as a face.
 *
 * Lifted out of spectrum_ui.c unchanged in behaviour: 48 FFT bands mirrored
 * into 96 bars around the panel, bass at 12 o'clock, treble at 6. Ported
 * originally from the sibling spec_analyzer_radial project, which fans the same
 * bands around the same round panel but reads I2S itself -- impossible here,
 * because audio_io already owns the codec, so the bands are fed from its taps
 * instead and show whichever half of the conversation is live.
 *
 * WHAT THIS FILE OWNS
 *
 * The FFT and everything it needs, including the audio-task-to-LVGL-task
 * handoff. That handoff lives here rather than in ui.c because its window size
 * is a property of this consumer: 1024 samples is an FFT parameter, not a
 * display one, and ui.c would only have to guess at it.
 *
 * The esp-dsp scratch buffers are allocated on first activation, so a build
 * that boots to the orb and never switches never pays for them.
 */

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "esp_dsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "bsp/display.h"

#include "ui_face.h"

static const char *TAG = "face_spectrum";

/* ---------------- analysis ---------------- */

#define FFT_N   1024
#define FFT_HOP 512 /* 32 ms at 16 kHz -- one capture chunk, ~one frame */

/*
 * Analysis bands, and one half of the ring -- so BAR_COUNT below is 48 bars.
 *
 * WAS 48 (96 bars), AND THE COUNT IS A MEMORY DECISION, NOT A VISUAL ONE.
 *
 * Every lv_draw_line allocates a mask buffer sized to the bar's screen width at
 * its angle -- see lv_draw_sw_line.c -- so 96 bars is 96 differently-sized
 * transient internal allocations per frame. Varying sizes are what fragment:
 * free a 200 byte block, ask for 340, it does not fit, carve elsewhere, leave a
 * hole. Uniform churn would coalesce and cost nothing.
 *
 * Measured with a live session, largest free block of internal RAM:
 *
 *   96 bars ...  11,776 B floor   (below the 29,824 B render buffer itself)
 *   48 bars ...  32,768 B floor
 *   orb face ... 43,008 B
 *
 * That matters because a TLS handshake on a reconnect wants one large
 * contiguous block, and this is the failure ui.c's header warns about -- total
 * free was never the problem here, it stayed above 44 kB throughout.
 *
 * Halving also made the face FASTER, 16 ms of draw down to 13.6-15.7 ms, since
 * it halves the pixel work as well as the allocation count.
 *
 * Do not raise this back without re-measuring intmax under a live session.
 */
#define STRIPE_COUNT 24
#define DB_MIN (-90.0f)
#define DB_MAX (0.0f)

/* dB shed per frame once idle: ~18 frames from full scale to silence. */
#define DECAY_DB 5.0f

/* ---------------- ring geometry ---------------- */

/* BAR_COUNT is twice STRIPE_COUNT so the display is mirrored about the vertical
 * axis: bass at 12 o'clock, treble at 6 o'clock. */
#define BAR_COUNT (STRIPE_COUNT * 2)
#define CENTER_X (BSP_LCD_H_RES / 2)
#define CENTER_Y (BSP_LCD_V_RES / 2)
#define R_INNER 80   /* where the bars start */
#define R_LENGTH 138 /* longest a bar can reach beyond R_INNER */
#define BAR_WIDTH 9
#define PEAK_THICKNESS 3
#define PEAK_FALL_PX 2.0f /* peak marker decay per frame */

/* Frames per breath while idle -- 3 s in, 3 s out at the frame timer's period. */
#define BREATHE_FRAMES 180

/* ---------------- sample handoff: audio tasks -> LVGL task ---------------- */

/*
 * A rolling FFT_N window with an FFT_HOP hop, published into two banks under a
 * seqlock.
 *
 * The producer hop (32 ms) and the frame timer (33 ms) are within 3% of each
 * other, so the reader periodically lands one flip behind -- and it can be
 * preempted mid-copy, since it runs at priority 4 and the audio tasks at 6 and
 * 7 on the same core. A torn window is not subtle: splicing two unrelated
 * segments puts a step discontinuity through the FFT and splatters the whole
 * spectrum. Hence the retry rather than a bare volatile.
 *
 * IN PSRAM, ALLOCATED IN init(). As internal .bss these buffers cost 6 kB from
 * boot whether or not anyone ever selects this face -- both faces are linked in
 * unconditionally, so a face's statics are resident even when it is not, and
 * internal RAM is the resource that binds here. Safe because every toucher is a
 * task and never an ISR: the taps fire inside the playback and capture task
 * loops, and take_window() runs in the LVGL frame timer.
 */
static int16_t *s_window;        /* owned by the audio tasks */
static size_t s_hop_fill;        /* samples into the new half */
static int16_t s_bank[2][FFT_N];
static uint32_t s_publishes;     /* release-stored by writer, acquire-loaded by reader */

/* ---------------- LVGL-owned state ---------------- */

/* FFT scratch, in PSRAM. 16-byte aligned: the S3 SIMD path requires it. */
static float *s_audio;    /* FFT_N     */
static float *s_wind;     /* FFT_N     */
static float *s_fft;      /* FFT_N * 2 */
static float *s_spectrum; /* FFT_N / 2 */

static float display_spectrum[STRIPE_COUNT];
static int band_len[STRIPE_COUNT];
static float band_peak[STRIPE_COUNT];

/* Precomputed, so no trig or colour conversion runs inside the draw path. */
static float bar_sin[BAR_COUNT];
static float bar_cos[BAR_COUNT];
static lv_color_t band_color_agent[STRIPE_COUNT];
static lv_color_t band_color_mic[STRIPE_COUNT];

/* Map a ring position to an analysis band, mirroring the second half back over
 * the first so the left and right sides match. */
static inline int band_of_bar(int bar)
{
    int mirrored = BAR_COUNT - 1 - bar;
    return bar < mirrored ? bar : mirrored;
}

/* ---------------- feed side (audio tasks) ---------------- */

static void feed_pcm(const int16_t *mono, size_t samples)
{
    while (samples > 0) {
        size_t room = FFT_HOP - s_hop_fill;
        size_t take = (samples < room) ? samples : room;

        /* New audio always lands in the second half of the window. */
        memcpy(&s_window[FFT_N - FFT_HOP + s_hop_fill], mono, take * sizeof(int16_t));
        s_hop_fill += take;
        mono += take;
        samples -= take;

        if (s_hop_fill < FFT_HOP) {
            break;
        }

        /* Publish into the bank the reader is not looking at, then make the
         * counter visible -- release ordering pairs with the reader's acquire. */
        uint32_t n = s_publishes;
        memcpy(s_bank[n & 1], s_window, FFT_N * sizeof(int16_t));
        __atomic_store_n(&s_publishes, n + 1, __ATOMIC_RELEASE);

        /* Slide: the half just published becomes the history half. */
        memmove(s_window, &s_window[FFT_HOP], FFT_HOP * sizeof(int16_t));
        s_hop_fill = 0;
    }
}

/* ---------------- analysis (LVGL task) ---------------- */

/* Copies the newest published window into s_audio as normalised floats.
 * Returns false if nothing has been published, or if the writer kept lapping
 * us -- in which case the caller holds the previous frame's spectrum. */
static bool take_window(void)
{
    for (int try = 0; try < 3; try++) {
        uint32_t n = __atomic_load_n(&s_publishes, __ATOMIC_ACQUIRE);
        if (n == 0) {
            return false;
        }

        const int16_t *src = s_bank[(n - 1) & 1];
        for (int i = 0; i < FFT_N; i++) {
            s_audio[i] = src[i] / 32768.0f;
        }

        if (__atomic_load_n(&s_publishes, __ATOMIC_ACQUIRE) == n) {
            return true;
        }
    }
    ESP_LOGD(TAG, "window torn three times, holding previous frame");
    return false;
}

static void analyse(void)
{
    dsps_mul_f32(s_audio, s_wind, s_audio, FFT_N, 1, 1, 1);

    for (int i = 0; i < FFT_N; i++) {
        s_fft[2 * i] = s_audio[i];
        s_fft[2 * i + 1] = 0;
    }

    dsps_fft2r_fc32(s_fft, FFT_N);
    dsps_bit_rev_fc32(s_fft, FFT_N);

    for (int i = 0; i < FFT_N / 2; i++) {
        float real = s_fft[2 * i];
        float imag = s_fft[2 * i + 1];
        float magnitude = sqrtf(real * real + imag * imag);
        s_spectrum[i] = 20 * log10f(magnitude / (FFT_N / 2) + 1e-9);
    }

    for (int i = 0; i < STRIPE_COUNT; i++) {
        int fft_idx = i * (FFT_N / 2) / STRIPE_COUNT;
        display_spectrum[i] = fmaxf(DB_MIN, fminf(DB_MAX, s_spectrum[fft_idx]));
    }
}

static void decay(void)
{
    for (int i = 0; i < STRIPE_COUNT; i++) {
        display_spectrum[i] = fmaxf(DB_MIN, display_spectrum[i] - DECAY_DB);
    }
}

/* ---------------- render ---------------- */

/* Renders the whole ring into the canvas. Called exactly once per frame, which
 * is the entire point of using a canvas here. */
static void render(const ui_render_ctx_t *ctx)
{
    /*
     * Idle has to be explicit. Between turns the playback task simply stops
     * calling the tap, so without decaying here the bars would hold their last
     * height forever -- only the peak markers would fall. This also covers
     * barge-in, where audio_io_flush() drops the queue mid-sentence.
     */
    if (ctx->idle) {
        decay();
    } else if (take_window()) {
        analyse();
    }

    for (int i = 0; i < STRIPE_COUNT; i++) {
        float norm = (display_spectrum[i] - DB_MIN) / (DB_MAX - DB_MIN);
        norm = fmaxf(0.0f, fminf(1.0f, norm));
        norm = sqrtf(norm); /* same perceptual curve the original used */

        band_len[i] = (int)(norm * R_LENGTH);

        if (band_peak[i] < band_len[i]) {
            band_peak[i] = band_len[i];
        } else {
            band_peak[i] = fmaxf(0.0f, band_peak[i] - PEAK_FALL_PX);
        }
    }

    lv_layer_t layer;
    lv_canvas_init_layer(ctx->canvas, &layer);
    lv_canvas_fill_bg(ctx->canvas, lv_color_black(), LV_OPA_COVER);

    /* The inner ring is a constant dim circle while audio is playing, and
     * breathes while nothing is. It is drawn every frame either way, so the
     * idle animation is free -- no lv_anim, no extra draw call. */
    lv_color_t ring_color = lv_color_hex(0x202020);
    if (ctx->press_active) {
        /* The button has no other affordance, and a press that lands during the
         * cooldown does nothing -- so show that the touch itself registered. */
        ring_color = lv_color_hex(0x8ad4e8);
    } else if (ctx->idle && !ctx->stopped) {
        float phase = (float)(ctx->frame % BREATHE_FRAMES) / BREATHE_FRAMES;
        float amp = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * phase);
        ring_color = lv_color_mix(lv_color_hex(0x1e5a6e), ring_color,
                                  (uint8_t)(amp * 255.0f));
    }

    lv_draw_arc_dsc_t ring_dsc;
    lv_draw_arc_dsc_init(&ring_dsc);
    ring_dsc.color = ring_color;
    ring_dsc.width = 2;
    ring_dsc.center.x = CENTER_X;
    ring_dsc.center.y = CENTER_Y;
    ring_dsc.radius = R_INNER - 10;
    ring_dsc.start_angle = 0;
    ring_dsc.end_angle = 360;
    ring_dsc.opa = LV_OPA_COVER;
    lv_draw_arc(&layer, &ring_dsc);

    const lv_color_t *palette =
        (ctx->source == UI_SRC_MIC) ? band_color_mic : band_color_agent;

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.width = BAR_WIDTH;
    line_dsc.round_start = 0; /* rounded ends cost a lot of AA work */
    line_dsc.round_end = 0;
    line_dsc.opa = LV_OPA_COVER;

    for (int bar = 0; bar < BAR_COUNT; bar++) {
        int band = band_of_bar(bar);
        float s = bar_sin[bar];
        float c = bar_cos[bar];

        line_dsc.color = palette[band];

        int outer = R_INNER + band_len[band];
        line_dsc.p1.x = (lv_value_precise_t)(CENTER_X + R_INNER * s);
        line_dsc.p1.y = (lv_value_precise_t)(CENTER_Y - R_INNER * c);
        line_dsc.p2.x = (lv_value_precise_t)(CENTER_X + outer * s);
        line_dsc.p2.y = (lv_value_precise_t)(CENTER_Y - outer * c);
        lv_draw_line(&layer, &line_dsc);

        /* Peak marker floats just past the bar's high-water mark. Skipping the
         * decayed ones roughly halves the draw count when the room is quiet. */
        if (band_peak[band] < 3.0f) {
            continue;
        }
        int peak_r = R_INNER + (int)band_peak[band] + 4;
        line_dsc.width = PEAK_THICKNESS;
        line_dsc.p1.x = (lv_value_precise_t)(CENTER_X + peak_r * s);
        line_dsc.p1.y = (lv_value_precise_t)(CENTER_Y - peak_r * c);
        line_dsc.p2.x = (lv_value_precise_t)(CENTER_X + (peak_r + PEAK_THICKNESS) * s);
        line_dsc.p2.y = (lv_value_precise_t)(CENTER_Y - (peak_r + PEAK_THICKNESS) * c);
        lv_draw_line(&layer, &line_dsc);
        line_dsc.width = BAR_WIDTH;
    }

    lv_canvas_finish_layer(ctx->canvas, &layer);
}

/* ---------------- setup ---------------- */

static esp_err_t init(lv_obj_t *canvas)
{
    LV_UNUSED(canvas);

    /*
     * Keep CONFIG_DSP_MAX_FFT_SIZE as the argument. Passing FFT_N instead takes
     * a different branch on the S3 that reuses -- and writes into -- a twiddle
     * table in ROM. The tables allocated here both exceed
     * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL, so they land in PSRAM by themselves.
     */
    esp_err_t err = dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FFT init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_audio = heap_caps_aligned_alloc(16, FFT_N * sizeof(float), MALLOC_CAP_SPIRAM);
    s_wind = heap_caps_aligned_alloc(16, FFT_N * sizeof(float), MALLOC_CAP_SPIRAM);
    s_fft = heap_caps_aligned_alloc(16, FFT_N * 2 * sizeof(float), MALLOC_CAP_SPIRAM);
    s_spectrum = heap_caps_aligned_alloc(16, (FFT_N / 2) * sizeof(float), MALLOC_CAP_SPIRAM);

    /*
     * The rolling PCM window. MALLOC_CAP_SPIRAM explicitly, not plain malloc: at
     * 2 kB this sits under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (4096) and would
     * be handed internal memory anyway -- the same trap orb_geometry.c pools
     * three small tables to dodge.
     *
     * calloc, not alloc, unlike the scratch buffers above. Those are written in
     * full before they are read; this one is not. New audio only ever lands in
     * the window's second half, so the first hop publishes a history half that
     * nothing has written yet -- as .bss that was silently zero, and from the
     * heap it would be whatever PSRAM held. That is exactly the step
     * discontinuity the seqlock comment above is about, and at 5 dB of decay per
     * frame one full-scale splatter takes ~600 ms to fall off the display.
     */
    s_window = heap_caps_aligned_calloc(16, FFT_N, sizeof(int16_t), MALLOC_CAP_SPIRAM);

    if (s_audio == NULL || s_wind == NULL || s_fft == NULL || s_spectrum == NULL ||
        s_window == NULL) {
        ESP_LOGE(TAG, "no PSRAM for FFT buffers");
        return ESP_ERR_NO_MEM;
    }

    dsps_wind_hann_f32(s_wind, FFT_N);

    for (int bar = 0; bar < BAR_COUNT; bar++) {
        /* 0 rad points at 12 o'clock, advancing clockwise. */
        float theta = (float)bar * 2.0f * (float)M_PI / BAR_COUNT;
        bar_sin[bar] = sinf(theta);
        bar_cos[bar] = cosf(theta);
    }
    for (int band = 0; band < STRIPE_COUNT; band++) {
        /* Agent: the full warm-to-violet sweep. Mic: a narrow cyan-to-blue
         * band, so which half of the conversation is live reads at a glance. */
        band_color_agent[band] = lv_color_hsv_to_rgb((uint16_t)(band * 270 / STRIPE_COUNT), 100, 100);
        band_color_mic[band] = lv_color_hsv_to_rgb((uint16_t)(170 + band * 70 / STRIPE_COUNT), 90, 100);
    }

    /* Silence, not full scale: display_spectrum is in dB, so zero-initialised
     * .bss would mean every bar starts pinned at DB_MAX. */
    for (int i = 0; i < STRIPE_COUNT; i++) {
        display_spectrum[i] = DB_MIN;
    }

    return ESP_OK;
}

const ui_face_t ui_face_spectrum = {
    .name = "spectrum",
    .init = init,
    .activate = NULL,
    .feed_pcm = feed_pcm,
    .render = render,
};
