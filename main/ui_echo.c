/*
 * The Echo screen. See ui_echo.h for what it is.
 *
 * Painted into the face's canvas like the ring screen, with the touch panel's
 * kit: the quarters and the half-circle are the panel's precomputed shapes, so
 * what is lit is exactly what can be pressed.
 *
 * REPAINTED ONLY ON A CHANGE. Nothing here moves between changes -- a quarter
 * lights or goes dark, the level steps, the turn passes -- so a frame that
 * changes none of those paints nothing and invalidates nothing. Four quarter
 * fills are most of a frame's budget; at 30 frames a second for pixels that
 * did not change they would be the whole of it. And a change of the quarter
 * lit, two to every step played or pressed, repaints only the quarters that
 * went on or off (paint_quads()).
 */
#include "ui_echo.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

#include "audio_io.h"
#include "controls.h"
#include "orb_colors.h"
#include "panel_geom.h"
#include "echo.h"
#include "ui_paint.h"
#include "ui_panel.h"
#include "ui_fonts.h"
#include "ui_scale.h"
#include "ui_tone.h"
#if CONFIG_UI_ECHO_SELFTEST
#include "screenshot.h"
#endif

static const char *TAG = "echo";

#define NVS_NAMESPACE   "dgagent"
#define NVS_KEY_BEST    "echo_best"

/* ---------------- the design, at 1:1 on the 466 px panel; PX() scales it ---------------- */

/* The quarters, in echo.h's order, as orb_colors.c indices: Phosphor, Clay,
 * Brass and Glass, as the Echo design names them. Indices rather than RGB so
 * they stay the picker's own. */
static const uint8_t QUAD_COLOR[4] = {4, 1, 3, 5};
static const panel_seg_t QUAD_SEG[4] = {
    PANEL_SEG_QUAD_TL, PANEL_SEG_QUAD_TR, PANEL_SEG_QUAD_BL, PANEL_SEG_QUAD_BR,
};
#define QUAD_DIM        0x525252   /* about a third: the design's dark shades */

/* Each quarter's note: a G major arpeggio, low to high round the screen. */
static const uint16_t QUAD_HZ[4] = {392, 523, 659, 784};
#define WRONG_HZ        150
#define WRONG_MS        600
#define WON_HZ          784        /* the top quarter's note, held as long */

#define HUB_R           PXF(70.0f)
#define HUB_RING_W      PXF(3.0f)
#define COL_HUB_RING    0x2A2D33   /* the design's, on a white orb */
/* Both lines sit high enough that TAP TO START, 108 px wide, fits the chord
 * of the hub's inside where it is drawn. */
#define HUB_BIG_DY      PX(-10)      /* GO, or the level */
#define HUB_SMALL_DY    PX(22)         /* TAP TO START, or LEVEL */
#define SMALL_SPACE     PX(2)          /* letter-spacing of the spaced caps */

/* Game over, from the centre. */
#define DOTS_DY         PX(-148)
#define DOT_DX          PXF(23.0f)
#define DOT_R           PXF(6.5f)
#define OVER_TITLE_DY   PX(-102)
#define OVER_LINE_DY    PX(-65)
#define OVER_LINE_GAP   PX(20)         /* between LEVEL n and BEST n */
#define AGAIN_ICON_DY   PX(75)
#define AGAIN_TEXT_DY   PX(120)

/* Awake, and exempt from the session's idle timeout, this long after the last
 * press or round. */
#define RECENT_MS       (2 * 60 * 1000)

/* ---------------- state ---------------- */

static lv_obj_t *s_canvas;
static echo_t s_game;
static volatile uint16_t s_best;      /* s_game.best, readable from main.c */
static volatile uint32_t s_active_ms; /* the last press or round; 0 never */
static bool s_full;                   /* repaint whatever the key says */
static bool s_left;                   /* not ticked since ui_echo_leave() */

/* What is on the canvas now; a frame repaints only when this changes. */
typedef struct {
    echo_state_t state;
    int lit;
    int level;
    uint32_t tint;
} painted_t;
static painted_t s_painted;

#if CONFIG_UI_ECHO_SELFTEST
static void selftest(uint32_t now_ms, int lit);   /* bench only; below */
#endif

static void touch(uint32_t now_ms)
{
    s_active_ms = now_ms ? now_ms : 1;
}

void ui_echo_init(lv_obj_t *canvas)
{
    s_canvas = canvas;
    nvs_handle_t h;
    uint16_t best = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, NVS_KEY_BEST, &best);   /* NOT_FOUND leaves it 0 */
        nvs_close(h);
    }
    s_game.best = best;
    s_best = best;
}

void ui_echo_enter(uint32_t now_ms)
{
    const uint16_t best = s_game.best;
    s_game = (echo_t){.state = ECHO_READY, .best = best, .press_k = -1};
    s_full = true;
    s_left = false;
    touch(now_ms);
    controls_echo_open(true);
}

void ui_echo_close(void)
{
    controls_echo_open(false);
    s_active_ms = 0;
}

void ui_echo_leave(bool blank)
{
    s_full = true;
    s_left = true;
    if (blank) {
        lv_canvas_fill_bg(s_canvas, lv_color_black(), LV_OPA_COVER);
        lv_obj_invalidate(s_canvas);
    }
}

bool ui_echo_recent(uint32_t now_ms)
{
    const uint32_t at = s_active_ms;
    return at != 0 && now_ms - at < RECENT_MS;
}

uint16_t ui_echo_best(void)
{
    return s_best;
}

void ui_echo_best_persist(uint16_t best)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u16(h, NVS_KEY_BEST, best);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        /* On screen until a reboot either way; only the next boot loses it. */
        ESP_LOGW(TAG, "could not persist best: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "EVT echo best saved %u", (unsigned)best);
    }
}

/* ---------------- painting ---------------- */

static void paint_quad(const ui_paint_target_t *t, int k, int lit)
{
    const uint32_t rgb = orb_colors_rgb(QUAD_COLOR[k]);
    ui_panel_kit_fill(t->px, t->stride, QUAD_SEG[k], k == lit ? rgb : ui_tone_mul(QUAD_DIM, rgb));
}

/* The hub: black over the quarters' inner corners, ringed, and what it reads.
 * `t` may be a window on the canvas whose top left is (ox, oy); the text is
 * clipped by the layer's own clip area. */
static void paint_hub(const ui_paint_target_t *t, int ox, int oy, lv_layer_t *L, uint32_t tint,
                      const ui_panel_palette_t *pal)
{
    const float c = PANEL_C;
    const uint32_t ring = s_game.state == ECHO_INPUT ? pal->primary
                                                      : ui_tone_grey(tint, COL_HUB_RING);
    ui_paint_disc(t, c - (float)ox, c - (float)oy, 0.0f, HUB_R, 0x000000, 1.0f);
    ui_paint_disc(t, c - (float)ox, c - (float)oy, HUB_R - HUB_RING_W, HUB_R, ring, 1.0f);

    const int32_t ci = (int32_t)c;
    if (s_game.state == ECHO_READY) {
        ui_paint_text(L, FONT_PANEL_L, pal->text, ci, ci + HUB_BIG_DY, 0, true, "GO");
        ui_paint_text(L, FONT_SMALL, pal->text, ci, ci + HUB_SMALL_DY, 0, true,
                      "TAP TO START");
        return;
    }
    char n[8];
    snprintf(n, sizeof(n), "%d", echo_level(&s_game));
    ui_paint_text(L, FONT_PANEL_L, pal->text, ci, ci + HUB_BIG_DY, 0, true, n);
    ui_paint_text(L, FONT_SMALL, pal->hint, ci, ci + HUB_SMALL_DY, SMALL_SPACE, true,
                  "LEVEL");
}

static void paint_play(const ui_paint_target_t *t, lv_layer_t *L, int lit, uint32_t tint,
                       const ui_panel_palette_t *pal)
{
    for (int k = 0; k < 4; k++) {
        paint_quad(t, k, lit);
    }
    paint_hub(t, 0, 0, L, tint, pal);
}

/*
 * Only the quarter lit changed: repaint the one that went off and the one that
 * came on, each exactly as paint_play() would -- black, the quarter, the hub
 * over its inner corner -- but clipped to the quarter's box. The same order
 * gives the same pixels; and the hub's anti-aliased rim, which blends with
 * what is under it, is only ever painted over a quarter just refilled, never
 * blended twice. The quarters' boxes do not overlap.
 */
static void paint_quads(const ui_paint_target_t *t, lv_layer_t *L, int was, int lit,
                        uint32_t tint, const ui_panel_palette_t *pal)
{
    const int ks[2] = {was, lit};
    for (int i = 0; i < 2; i++) {
        const int k = ks[i];
        if (k < 0) {
            continue;
        }
        int x0, y0, x1, y1;
        panel_seg_bbox(QUAD_SEG[k], &x0, &y0, &x1, &y1);
        x0 = x0 < 0 ? 0 : x0;
        y0 = y0 < 0 ? 0 : y0;
        x1 = x1 >= t->w ? t->w - 1 : x1;
        y1 = y1 >= t->h ? t->h - 1 : y1;
        for (int y = y0; y <= y1; y++) {
            memset(t->px + (size_t)y * t->stride + x0, 0, (size_t)(x1 - x0 + 1) * sizeof(uint16_t));
        }
        paint_quad(t, k, lit);

        const ui_paint_target_t box = {
            .px = t->px + (size_t)y0 * t->stride + x0,
            .stride = t->stride,
            .w = x1 - x0 + 1,
            .h = y1 - y0 + 1,
        };
        const lv_area_t a = {x0, y0, x1, y1};
        L->_clip_area = a;   /* for the draw tasks added next: the hub's text */
        paint_hub(&box, x0, y0, L, tint, pal);
        lv_obj_invalidate_area(s_canvas, &a);
    }
}

static void paint_over(const ui_paint_target_t *t, lv_layer_t *L, const ui_panel_palette_t *pal)
{
    const float c = PANEL_C;
    const int32_t ci = (int32_t)c;
    for (int k = 0; k < 4; k++) {
        ui_paint_disc(t, c + ((float)k - 1.5f) * DOT_DX, c + DOTS_DY, 0.0f, DOT_R,
                      orb_colors_rgb(QUAD_COLOR[k]), 1.0f);
    }
    ui_paint_text(L, FONT_PANEL_L, pal->text, ci, ci + OVER_TITLE_DY, 0, false,
                  s_game.won ? "You win" : "Game over");

    char left[16], right[16];
    snprintf(left, sizeof(left), "LEVEL %d", echo_level(&s_game));
    snprintf(right, sizeof(right), "BEST %u", (unsigned)s_game.best);
    const lv_font_t *f = FONT_SMALL;
    const int32_t wl = ui_paint_text_width(f, SMALL_SPACE, left);
    const int32_t wr = ui_paint_text_width(f, SMALL_SPACE, right);
    const int32_t x0 = ci - (wl + OVER_LINE_GAP + wr) / 2;
    ui_paint_text(L, f, pal->hint, x0 + wl / 2, ci + OVER_LINE_DY, SMALL_SPACE, true, left);
    ui_paint_text(L, f, pal->hint, x0 + wl + OVER_LINE_GAP + wr / 2, ci + OVER_LINE_DY,
                  SMALL_SPACE, true, right);

    ui_panel_kit_fill(t->px, t->stride, PANEL_SEG_HALF_BOTTOM, pal->primary);
    ui_panel_kit_icon(L, UI_ICON_REPLAY, pal->primary_ink, ci, ci + AGAIN_ICON_DY);
    ui_paint_text(L, FONT_PANEL_L, pal->primary_ink, ci, ci + AGAIN_TEXT_DY, 0, false,
                  "Play again");
}

void ui_echo_frame(uint32_t now_ms, uint32_t tint_rgb)
{
    if (s_left) {
        /* Back from under the panel: the game's clock ran on while it was not
         * ticked, and a sequence half shown would jump to the player's turn. */
        s_left = false;
        echo_resume(&s_game, now_ms);
        ESP_LOGI(TAG, "EVT echo resume level %d", echo_level(&s_game));
    }
    int tone = -1;
    const echo_state_t was = s_game.state;
    const uint8_t was_pos = s_game.pos;
    const int lit = s_game.state == ECHO_READY || s_game.state == ECHO_OVER
                        ? -1
                        : echo_tick(&s_game, now_ms, audio_io_playback_active(), &tone);
    if (tone >= 0) {
        audio_io_tone(QUAD_HZ[tone], ECHO_STEP_MS);
    }
    if (s_game.state == ECHO_SHOW && was == ECHO_SHOW && s_game.pos < was_pos) {
        ESP_LOGI(TAG, "EVT echo replay level %d (agent speaking)", echo_level(&s_game));
    }
    if (s_game.state != was) {
        touch(now_ms);
        ESP_LOGI(TAG, "EVT echo %s level %d",
                 s_game.state == ECHO_SHOW ? "show" : s_game.state == ECHO_INPUT ? "input" : "?",
                 echo_level(&s_game));
    }

    const painted_t want = {
        .state = s_game.state,
        .lit = lit,
        .level = echo_level(&s_game),
        .tint = tint_rgb,
    };
    if (s_full || memcmp(&want, &s_painted, sizeof(want)) != 0) {
        const bool only_lit = !s_full && want.state == s_painted.state &&
                              want.level == s_painted.level && want.tint == s_painted.tint;
        const int was_lit = s_painted.lit;
        s_full = false;
        s_painted = want;

        lv_draw_buf_t *buf = lv_canvas_get_draw_buf(s_canvas);
        const ui_paint_target_t t = {
            .px = (uint16_t *)buf->data,
            .stride = buf->header.stride / sizeof(uint16_t),
            .w = (int)buf->header.w,
            .h = (int)buf->header.h,
        };
        ui_panel_palette_t pal;
        ui_panel_kit_palette(tint_rgb, &pal);
        lv_layer_t layer;
        lv_canvas_init_layer(s_canvas, &layer);
        if (only_lit) {
            paint_quads(&t, &layer, was_lit, lit, tint_rgb, &pal);
            lv_canvas_finish_layer(s_canvas, &layer);
        } else {
            memset(t.px, 0, (size_t)buf->header.stride * buf->header.h);
            if (s_game.state == ECHO_OVER) {
                paint_over(&t, &layer, &pal);
            } else {
                paint_play(&t, &layer, lit, tint_rgb, &pal);
            }
            lv_canvas_finish_layer(s_canvas, &layer);
            lv_obj_invalidate(s_canvas);
        }
    }
#if CONFIG_UI_ECHO_SELFTEST
    selftest(now_ms, lit);
#endif
}

/* ---------------- touch ---------------- */

static void start(uint32_t now_ms)
{
    echo_start(&s_game, esp_random(), now_ms);
    touch(now_ms);
    ESP_LOGI(TAG, "EVT echo start best %u", (unsigned)s_game.best);
}

void ui_echo_pressed(int32_t x, int32_t y, uint32_t now_ms)
{
    const float px = (float)x + 0.5f, py = (float)y + 0.5f;

    switch (s_game.state) {
    case ECHO_READY:
        if (panel_circle_sdf(PANEL_C, PANEL_C, HUB_R, px, py) <= PANEL_HIT_SLOP) {
            start(now_ms);
        }
        return;

    case ECHO_OVER:
        if (panel_seg_sdf(PANEL_SEG_HALF_BOTTOM, px, py) <= 0.0f) {
            start(now_ms);
        }
        return;

    case ECHO_INPUT:
        break;

    default:
        return;   /* the sequence is playing, or the agent is talking */
    }

    int k = -1;
    for (int i = 0; i < 4 && k < 0; i++) {
        if (panel_seg_sdf(QUAD_SEG[i], px, py) <= 0.0f) {
            k = i;
        }
    }
    if (k < 0) {
        return;
    }
    touch(now_ms);
    switch (echo_press(&s_game, k, now_ms)) {
    case ECHO_PRESS_RIGHT:
        audio_io_tone(QUAD_HZ[k], ECHO_PRESS_MS);
        break;
    case ECHO_PRESS_ROUND:
        audio_io_tone(QUAD_HZ[k], ECHO_PRESS_MS);
        ESP_LOGI(TAG, "EVT echo level %d", echo_level(&s_game));
        controls_echo_callout(echo_level(&s_game));
        break;
    case ECHO_PRESS_WRONG:
        audio_io_tone(WRONG_HZ, WRONG_MS);
        s_best = s_game.best;
        ESP_LOGI(TAG, "EVT echo over level %d best %u", echo_level(&s_game),
                 (unsigned)s_game.best);
        break;
    case ECHO_PRESS_WON:
        audio_io_tone(WON_HZ, WRONG_MS);
        s_best = s_game.best;
        ESP_LOGI(TAG, "EVT echo won level %d", echo_level(&s_game));
        controls_echo_won();
        break;
    default:
        break;
    }
}

/* ---------------- self-test ---------------- */

#if CONFIG_UI_ECHO_SELFTEST
/*
 * BENCH AID, NOT A FEATURE: plays through the same hit test a finger uses, and
 * captures what it shows -- GO, a step lit, the wait for the callout, the
 * player's turn, and game over after a wrong press at level 3. Then asks for
 * the two later tiers' callouts outright, since nobody reaches level 15 on the
 * bench, so their replies are in the log.
 */
static bool shot(const char *what)
{
    char tag[24];
    snprintf(tag, sizeof(tag), "echo-%s", what);
    lv_draw_buf_t *buf = lv_canvas_get_draw_buf(s_canvas);
    return screenshot_capture((const uint16_t *)buf->data, buf->header.stride / sizeof(uint16_t),
                              (int)buf->header.w, (int)buf->header.h, tag);
}

/* The middle of quarter k, for a press. */
static void quad_point(int k, int32_t *x, int32_t *y)
{
    const int32_t c = (int32_t)PANEL_C;
    *x = c + ((k & 1) ? 110 : -110);
    *y = c + ((k & 2) ? 110 : -110);
}

static void selftest(uint32_t now_ms, int lit)
{
    static uint32_t t0, next_ms;
    static bool shot_ready, shot_show, shot_speak, shot_input, shot_over, tiers_done;
    static int tier;
    const int32_t c = (int32_t)PANEL_C;
    if (t0 == 0) {
        t0 = now_ms | 1;
    }
    if ((int32_t)(now_ms - next_ms) < 0) {
        return;
    }

    switch (s_game.state) {
    case ECHO_READY:
        if (now_ms - t0 < 1500) {
            return;
        }
        if (!shot_ready) {
            shot_ready = shot("ready");
            return;
        }
        ESP_LOGI(TAG, "EVT echo selftest go");
        ui_echo_pressed(c, c, now_ms);
        return;

    case ECHO_SHOW:
        if (!shot_show && lit >= 0) {
            shot_show = shot("show");
        }
        return;

    case ECHO_SPEAK:
        if (!shot_speak) {
            shot_speak = shot("speak");
        }
        return;

    case ECHO_INPUT: {
        if (echo_level(&s_game) == 2 && !shot_input) {
            shot_input = shot("input");
            return;
        }
        int32_t x, y;
        int k = s_game.seq[s_game.pos];
        if (echo_level(&s_game) >= 3) {
            k = (k + 1) % 4;   /* wrong, on purpose */
        }
        quad_point(k, &x, &y);
        ui_echo_pressed(x, y, now_ms);
        next_ms = now_ms + 300;
        return;
    }

    case ECHO_OVER:
        if (!shot_over) {
            if (now_ms - s_active_ms >= 800) {
                shot_over = shot("over");
            }
            return;
        }
        if (!tiers_done) {
            static const int LEVELS[] = {12, 17};
            if (tier < 2) {
                ESP_LOGI(TAG, "EVT echo selftest callout %d", LEVELS[tier]);
                controls_echo_callout(LEVELS[tier]);
                tier++;
                next_ms = now_ms + 10000;
            } else {
                tiers_done = true;
                ESP_LOGI(TAG, "EVT echo selftest done");
            }
        }
        return;
    }
}
#endif
