/*
 * Radial spectrum analyzer for the agent session. See spectrum_ui.h for the
 * threading contract.
 *
 * The rendering arrangement is inherited from spec_analyzer_radial, where it
 * was arrived at by measuring variants on this same panel:
 *
 *   direct-to-layer, BSP defaults ....  3.7 fps + task_wdt panic
 *   PSRAM canvas, BSP defaults ......  15 fps
 *   direct-to-layer, full-frame buf ..  does not run at all
 *   PSRAM canvas + internal render ...  this arrangement
 *
 * Two things drive that. First, draw ONCE per frame into a canvas: a custom
 * LV_EVENT_DRAW_MAIN handler is re-invoked once per render chunk and
 * re-rasterises every anti-aliased line each time, whereas a canvas makes the
 * per-chunk cost a memcpy. Second, keep LVGL's render buffer in INTERNAL RAM --
 * which is why the display is registered here instead of via
 * bsp_display_start(), whose buffer lives in PSRAM and needs a DMA bounce copy
 * on every flush. Do not raise DRAW_ROWS towards a full frame: a PSRAM-sourced
 * SPI transfer needs an internal bounce buffer the same size, so a full frame
 * asks for 434 kB of internal RAM and every flush fails with ESP_ERR_NO_MEM.
 *
 * WHAT IS DIFFERENT HERE
 *
 * That project had ~300 kB of internal RAM to itself. This one shares 288 kB
 * with Wi-Fi, lwIP and TLS, and the failure mode is not boot -- it is the first
 * WebSocket reconnect, when a handshake wants a burst of internal RAM with the
 * display already up. So DRAW_ROWS is halved to 32, and the FFT scratch buffers
 * live in PSRAM. That costs nothing measurable: esp-dsp's own twiddle and
 * bit-reversal tables are already in PSRAM even in the project above, since
 * both exceed CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL.
 */

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "esp_dsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_lv_adapter.h"
#include "esp_lv_adapter_input.h"

#include "audio_io.h"
#include "spectrum_ui.h"

static const char *TAG = "spectrum_ui";

/* ---------------- analysis ---------------- */

#define FFT_N   1024
#define FFT_HOP 512 /* 32 ms at 16 kHz -- one capture chunk, ~one frame */

#define STRIPE_COUNT 48 /* analysis bands; also one half of the ring */
#define DB_MIN (-90.0f)
#define DB_MAX (0.0f)

/* How long after the last sample we treat the ring as idle. Slightly longer
 * than one hop so a late chunk does not flicker the bars to flat. */
#define IDLE_US 250000

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
#define FRAME_MS 33

/* Frames per breath while idle -- 3 s in, 3 s out at FRAME_MS. */
#define BREATHE_FRAMES 180

/*
 * The inner circle is the button. Touching anywhere else does nothing.
 *
 * Matches the inner ring arc drawn at R_INNER - 10, so the affordance on screen
 * is exactly the hit area. The whole 466x466 screen used to be live, which made
 * brushing the bezel enough to end a conversation.
 */
#define BUTTON_RADIUS (R_INNER - 10)

/* Rows per LVGL render chunk, in internal RAM: 466 * rows * 2 bytes. Must stay
 * small enough that the SPI driver can allocate a DMA buffer of the same size,
 * and small enough that Wi-Fi can still find contiguous internal RAM later. */
#define DRAW_ROWS 32

/* ---------------- sample handoff: audio tasks -> LVGL task ---------------- */

typedef enum {
    SRC_NONE = 0,
    SRC_AGENT,
    SRC_MIC,
} viz_source_t;

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
 */
static int16_t s_window[FFT_N];  /* owned by the audio tasks */
static size_t s_hop_fill;        /* samples into the new half */
static int16_t s_bank[2][FFT_N];
static uint32_t s_publishes;     /* release-stored by writer, acquire-loaded by reader */

static int64_t s_last_feed_us;
static viz_source_t s_source;

/* Session state, for the middle of the ring. Set from the WebSocket task; the
 * pointer is stored, never the characters, so this is a single word write. */
static const char *s_status = "starting";
static bool s_session_live;

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

static lv_obj_t *canvas_obj;
static lv_obj_t *status_label;
static uint32_t s_frame;
static lv_indev_t *s_touch;
static void (*s_gesture_handler)(spectrum_ui_gesture_t gesture);
static volatile bool s_stopped;
/* Both LVGL-task only. Separate flags on purpose -- see gesture_event_cb. */
static bool s_press_in_button;  /* gate: did this press start on the button? */
static bool s_press_active;     /* visual: is a finger down on it right now? */

/* Map a ring position to an analysis band, mirroring the second half back over
 * the first so the left and right sides match. */
static inline int band_of_bar(int bar)
{
    int mirrored = BAR_COUNT - 1 - bar;
    return bar < mirrored ? bar : mirrored;
}

/* ---------------- feed side (audio tasks) ---------------- */

static void feed(const int16_t *mono, size_t samples, viz_source_t src)
{
    s_source = src;
    s_last_feed_us = esp_timer_get_time();

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
        memcpy(s_bank[n & 1], s_window, sizeof(s_window));
        __atomic_store_n(&s_publishes, n + 1, __ATOMIC_RELEASE);

        /* Slide: the half just published becomes the history half. */
        memmove(s_window, &s_window[FFT_HOP], FFT_HOP * sizeof(int16_t));
        s_hop_fill = 0;
    }
}

void spectrum_ui_feed_agent(const int16_t *mono, size_t samples)
{
    feed(mono, samples, SRC_AGENT);
}

void spectrum_ui_feed_mic(const int16_t *mono, size_t samples)
{
    /*
     * Agent audio wins, so only one task is ever inside feed(). With
     * CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS the capture task is already stopped
     * here; without it, this is what keeps the two writers apart. The residual
     * sliver -- playback ending between this check and the tap firing -- costs
     * at worst one garbled window, which is not worth a mutex on a priority-7
     * audio task.
     */
    if (audio_io_playback_active()) {
        return;
    }
    feed(mono, samples, SRC_MIC);
}

void spectrum_ui_set_gesture_handler(void (*handler)(spectrum_ui_gesture_t gesture))
{
    s_gesture_handler = handler;
}

void spectrum_ui_set_stopped(bool stopped)
{
    s_stopped = stopped;
}

void spectrum_ui_set_status(const char *text, bool session_live)
{
    s_status = (text != NULL) ? text : "";
    s_session_live = session_live;
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
static void render_ring(bool idle, viz_source_t src)
{
    lv_layer_t layer;
    lv_canvas_init_layer(canvas_obj, &layer);
    lv_canvas_fill_bg(canvas_obj, lv_color_black(), LV_OPA_COVER);

    /* The inner ring is a constant dim circle while audio is playing, and
     * breathes while nothing is. It is drawn every frame either way, so the
     * idle animation is free -- no lv_anim, no extra draw call. */
    lv_color_t ring_color = lv_color_hex(0x202020);
    if (s_press_active) {
        /* The button has no other affordance, and a press that lands during the
         * cooldown does nothing -- so show that the touch itself registered. */
        ring_color = lv_color_hex(0x8ad4e8);
    } else if (idle && !s_stopped) {
        float phase = (float)(s_frame % BREATHE_FRAMES) / BREATHE_FRAMES;
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

    const lv_color_t *palette = (src == SRC_MIC) ? band_color_mic : band_color_agent;

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

    lv_canvas_finish_layer(canvas_obj, &layer);
}

static void update_status_label(bool idle)
{
    /*
     * All candidates are string literals, so the "has it changed" test is a
     * pointer compare. That matters: every rewrite adds another invalid area
     * and therefore another render pass.
     */
    const char *want;
    if (!s_session_live) {
        want = s_status; /* connecting / error / disconnected */
    } else if (audio_io_playback_active()) {
        want = "speaking";
    } else if (!idle && s_source == SRC_MIC) {
        want = "listening";
    } else {
        want = s_status; /* "ready" */
    }

    static const char *shown;
    if (want != shown) {
        shown = want;
        lv_label_set_text(status_label, want);
    }
}

static void frame_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    s_frame++;

    /*
     * Idle has to be explicit. Between turns the playback task simply stops
     * calling the tap, so without decaying here the bars would hold their last
     * height forever -- only the peak markers would fall. This also covers
     * barge-in, where audio_io_flush() drops the queue mid-sentence.
     */
    bool idle = (esp_timer_get_time() - s_last_feed_us) > IDLE_US;

    if (idle) {
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

    update_status_label(idle);
    render_ring(idle, s_source);
}

/* ---------------- setup ---------------- */

/*
 * SHORT_CLICKED and LONG_PRESSED, never CLICKED: CLICKED is sent on release
 * regardless of long press, so pairing it with LONG_PRESSED would fire the tap
 * action on every hold too. SHORT_CLICKED is emitted only when LVGL's
 * long_pr_sent flag is clear -- the same flag LONG_PRESSED sets -- which makes
 * the two gestures mutually exclusive by construction.
 *
 * The hit area is decided once, on PRESSED, and both gestures are gated on that
 * decision. Testing the point at release instead would let a press that started
 * on the bezel drift into the circle and count -- and LONG_PRESSED has no
 * release point to test at all.
 *
 * Runs on the LVGL task holding the LVGL lock, so the handler only signals.
 */
static bool press_is_in_button(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev == NULL) {
        return false;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    int32_t dx = p.x - CENTER_X;
    int32_t dy = p.y - CENTER_Y;
    return (dx * dx + dy * dy) <= (BUTTON_RADIUS * BUTTON_RADIUS);
}

static void gesture_event_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s_press_in_button = press_is_in_button(e);
        s_press_active = s_press_in_button;
        break;

    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        /*
         * Only the highlight is cleared here. s_press_in_button must survive:
         * LVGL sends RELEASED *before* SHORT_CLICKED (lv_indev.c, RELEASED at
         * the top of indev_proc_release and the click events below it), so
         * clearing the gate here would make every tap a no-op. It is set fresh
         * on the next PRESSED, so leaving it set costs nothing.
         */
        s_press_active = false;
        break;

    case LV_EVENT_SHORT_CLICKED:
        if (s_press_in_button && s_gesture_handler) {
            s_gesture_handler(SPECTRUM_UI_TAP);
        }
        break;

    case LV_EVENT_LONG_PRESSED:
        if (s_press_in_button && s_gesture_handler) {
            s_gesture_handler(SPECTRUM_UI_HOLD);
        }
        break;

    default:
        break;
    }
}

/* The CO5300 only accepts even start / odd end coordinates. bsp_display_start()
 * installs this; since we register the display ourselves, we must too. */
static void rounder_event_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

static lv_display_t *display_start(void)
{
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = 1;
    /*
     * Below audio_play (6) and audio_cap (7), which share this core. The
     * adapter defaults to 6, which would round-robin against playback: a
     * starved LVGL task drops frames, a starved audio task drops audio.
     */
    adapter_cfg.task_priority = 4;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }

    static esp_lcd_panel_handle_t panel;
    static esp_lcd_panel_io_handle_t panel_io;
    const bsp_display_config_t panel_cfg = {
        .max_transfer_sz = BSP_LCD_H_RES * DRAW_ROWS * BSP_LCD_BITS_PER_PIXEL / 8,
    };
    /* Brings up QSPI, resets the panel and turns it on. Blocks ~1.2 s in the
     * CO5300 init sequence. Touches only SPI2 and the reset GPIO, so it does
     * not interact with the I2C/I2S bring-up audio_io_init() already did. */
    if (bsp_display_new(&panel_cfg, &panel, &panel_io) != ESP_OK) {
        return NULL;
    }

    esp_lv_adapter_display_config_t disp_cfg = {
        .panel = panel,
        .panel_io = panel_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = DRAW_ROWS,
            .use_psram = false, /* internal RAM: DMA-able without a bounce copy */
            .enable_ppa_accel = false,
            .require_double_buffer = false,
            .mono_layout = ESP_LV_ADAPTER_MONO_LAYOUT_NONE,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .te_sync = ESP_LV_ADAPTER_TE_SYNC_DISABLED(),
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (disp == NULL) {
        return NULL;
    }
    lv_display_add_event_cb(disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    /*
     * Touch, in the same slot bsp_display_start() uses. Only touch_flags is read
     * out of this struct, and these three values are the ones the BSP pairs with
     * a ROTATE_0 display -- they have to match or the axes come out wrong.
     *
     * bsp_touch_new() calls bsp_i2c_init() itself, which is already done by
     * audio_io_init(). That is safe here, unlike the bsp_audio_init() trap: this
     * guard tests the flag the same function sets, so the second call is a plain
     * no-op returning the existing bus.
     */
    static esp_lcd_touch_handle_t tp;
    bsp_display_cfg_t touch_cfg = {
        .touch_flags = { .swap_xy = 0, .mirror_x = 1, .mirror_y = 1 },
    };
    if (bsp_touch_new(&touch_cfg, &tp) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_touch_config_t tcfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
    s_touch = esp_lv_adapter_register_touch(&tcfg);
    if (s_touch == NULL) {
        return NULL;
    }
    /* LVGL defaults to 400 ms, which is well inside an ordinary tap. A restart
     * is destructive enough to want a deliberate hold. */
    lv_indev_set_long_press_time(s_touch, 1000);

    /* Must follow bsp_display_new() -- brightness is a panel command over the
     * same io -- and precede the first flush. It sets full brightness itself. */
    if (bsp_display_brightness_init() != ESP_OK || esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static esp_err_t dsp_start(void)
{
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
    if (s_audio == NULL || s_wind == NULL || s_fft == NULL || s_spectrum == NULL) {
        ESP_LOGE(TAG, "no PSRAM for FFT buffers");
        return ESP_ERR_NO_MEM;
    }

    dsps_wind_hann_f32(s_wind, FFT_N);
    return ESP_OK;
}

static esp_err_t build_ui(void)
{
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

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Canvas pixels must live in PSRAM; 466*466*2 is far past internal RAM. */
    uint32_t stride = lv_draw_buf_width_to_stride(BSP_LCD_H_RES, LV_COLOR_FORMAT_RGB565);
    uint32_t buf_size = stride * BSP_LCD_V_RES;
    void *pixels = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        ESP_LOGE(TAG, "no PSRAM for the %" PRIu32 " byte canvas", buf_size);
        return ESP_ERR_NO_MEM;
    }
    static lv_draw_buf_t canvas_buf;
    lv_draw_buf_init(&canvas_buf, BSP_LCD_H_RES, BSP_LCD_V_RES,
                     LV_COLOR_FORMAT_RGB565, stride, pixels, buf_size);
    ESP_LOGI(TAG, "canvas %" PRIu32 " B PSRAM, render buffer %d B internal",
             buf_size, BSP_LCD_H_RES * DRAW_ROWS * 2);

    canvas_obj = lv_canvas_create(scr);
    lv_obj_set_size(canvas_obj, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_pos(canvas_obj, 0, 0);
    lv_canvas_set_draw_buf(canvas_obj, &canvas_buf);

    status_label = lv_label_create(scr);
    lv_label_set_text(status_label, s_status);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(status_label);

    /*
     * The screen, not the canvas. lv_canvas derives from lv_image, whose
     * constructor removes LV_OBJ_FLAG_CLICKABLE, and a non-clickable object is
     * transparent to hit-testing rather than a blocker -- so a touch falls
     * through the full-screen canvas and the label to the screen, which is
     * clickable by default. The remove_flag(SCROLLABLE) above is load bearing
     * too: LVGL suppresses LONG_PRESSED while a scroll object is latched.
     */
    lv_obj_add_event_cb(scr, gesture_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, gesture_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(scr, gesture_event_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(scr, gesture_event_cb, LV_EVENT_LONG_PRESSED, NULL);

    lv_timer_create(frame_timer_cb, FRAME_MS, NULL);
    return ESP_OK;
}

esp_err_t spectrum_ui_start(void)
{
    esp_err_t err = dsp_start();
    if (err != ESP_OK) {
        return err;
    }

    if (display_start() == NULL) {
        ESP_LOGE(TAG, "display init failed");
        return ESP_FAIL;
    }

    /* LVGL is not thread safe and its task is already running. */
    if (bsp_display_lock(-1) != ESP_OK) {
        return ESP_FAIL;
    }
    err = build_ui();
    bsp_display_unlock();

    return err;
}
