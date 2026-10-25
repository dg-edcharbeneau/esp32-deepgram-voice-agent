/*
 * The ring screen. See ui_ring.h for what it is.
 *
 * Painted into the face's canvas like the touch panel, whose kit it borrows:
 * the button shapes come from the panel's precomputed masks, the icons from its
 * stroked set, the colours from its palette in the orb's colour. The orb is the
 * orb clock's sphere, smaller.
 *
 * TWO REGIONS, PAINTED ON THEIR OWN CLOCKS. Above the buttons -- the orb, its
 * rings, the label, the overtime -- moves every frame and is cleared and
 * redrawn every frame. The buttons change only when touched, or while the
 * snooze fade runs, so they are painted then and otherwise left alone: at the
 * ring rate a full repaint would be most of the frame for pixels that did not
 * change.
 */
#include "ui_ring.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "alarm_sched.h"
#include "clock_face.h"
#include "controls.h"
#include "panel_geom.h"
#include "ui_paint.h"
#include "ui_panel.h"
#include "ui_tone.h"
#if CONFIG_UI_RING_SELFTEST
#include "screenshot.h"
#endif

static const char *TAG = "ring";

LV_FONT_DECLARE(font_panel_28);
LV_FONT_DECLARE(font_panel_40);

/* ---------------- the design, at 1:1 ---------------- */

#define COL_SPHERE      0xCFE3FF   /* the orb's dots, on a white orb */
#define COL_LINE        0xC9CED6   /* the overtime line and the rings */

#define ORB_DY          (-133)     /* the orb's centre, from the panel's */
#define ORB_R           45.0f
#define LABEL_DY        (-57)
#define LINE_DY         (-30)
#define LINE_SPACE      2          /* letter-spacing of the spaced caps */
#define LINE_DOT_GAP    18         /* the "·" between its two halves, drawn */

/* The rings pulsing out of the orb: three, staggered, each growing from just
 * outside it to past the rim and fading as it goes. */
#define RINGS           3
#define RING_PERIOD_S   2.4f
#define RING_FROM       (ORB_R + 8.0f)
#define RING_TO         260.0f
#define RING_W          1.5f
#define RING_ALPHA      0.45f

/* Where the buttons begin: the top edge of the bottom half, past the gap. */
#define TOP_END         ((int)(PANEL_C + PANEL_GAP))

/* Timer: one Stop, the whole bottom half. */
#define STOP_ICON_DY    81
#define STOP_TEXT_DY    130
/* Alarm: two quarters, the text over the icon in each. */
#define QUAD_DX         78
#define QUAD_TEXT_DY    58
#define QUAD_ICON_DY    95

#define SNOOZE_FADE_MS  1500

/* ---------------- state ---------------- */

static lv_obj_t *s_canvas;
static clock_dot_t *s_orb;      /* PSRAM: 3.6 kB has no business in internal RAM */
static uint32_t s_seq;          /* the ring on screen */
static uint32_t s_done_seq;     /* a ring ended from this screen */
static uint32_t s_t0_ms;        /* when it appeared, for the motion */
static int s_snooze_min;        /* minutes shown on the snooze button, 0 none yet */
static uint32_t s_snooze_ms;    /* the last snooze press; 0 no fade running */
static bool s_buttons_dirty;
static bool s_full;             /* clear everything, not just the top */
static uint32_t s_tint;

#if CONFIG_UI_RING_SELFTEST
static void selftest(const ui_ring_t *r, uint32_t now_ms);   /* bench only; below */
#endif

void ui_ring_init(lv_obj_t *canvas)
{
    s_canvas = canvas;
    s_orb = heap_caps_malloc(sizeof(clock_dot_t) * CLOCK_FACE_SPHERE, MALLOC_CAP_SPIRAM);
    if (s_orb == NULL) {
        /* The screen still works; it just has no orb. */
        ESP_LOGW(TAG, "no PSRAM for the ring screen's orb");
    }
}

bool ui_ring_wanted(const ui_ring_t *r)
{
    return r->ringing && r->seq != s_done_seq;
}

void ui_ring_leave(void)
{
    s_seq = 0;
    lv_canvas_fill_bg(s_canvas, lv_color_black(), LV_OPA_COVER);
    lv_obj_invalidate(s_canvas);
}

/* ---------------- painting ---------------- */

static uint32_t mix(uint32_t a, uint32_t b, float f)
{
    uint32_t out = 0;
    for (int s = 16; s >= 0; s -= 8) {
        const float ca = (float)((a >> s) & 0xFF), cb = (float)((b >> s) & 0xFF);
        out |= (uint32_t)(ca + (cb - ca) * f + 0.5f) << s;
    }
    return out;
}

/* "+5:11", or "+1:05:11" past the hour. */
static void overtime(uint32_t secs, char *out, size_t n)
{
    if (secs >= 3600) {
        snprintf(out, n, "+%u:%02u:%02u", (unsigned)(secs / 3600), (unsigned)((secs / 60) % 60),
                 (unsigned)(secs % 60));
    } else {
        snprintf(out, n, "+%u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
    }
}

static void paint_top(const ui_paint_target_t *t, lv_layer_t *L, const ui_ring_t *r,
                      uint32_t now_ms, uint32_t tint, bool h12, const ui_panel_palette_t *pal)
{
    const float c = PANEL_C;
    const float elapsed = (float)(now_ms - s_t0_ms) / 1000.0f;
    const uint32_t c_line = ui_tone_ink(ui_tone_mul(tint, COL_LINE));

    /* The rings only while it sounds: past the cap it is quiet, and so is this. */
    if (r->sounding) {
        for (int k = 0; k < RINGS; k++) {
            float f = elapsed / RING_PERIOD_S + (float)k / (float)RINGS;
            f -= floorf(f);
            ui_paint_circle(t, c, c + ORB_DY, RING_FROM + f * (RING_TO - RING_FROM), RING_W,
                            c_line, RING_ALPHA * (1.0f - f), TOP_END);
        }
    }

    if (s_orb != NULL) {
        const uint32_t c_sphere = ui_tone_mul(tint, COL_SPHERE);
        clock_face_sphere(s_orb, elapsed, c, c + ORB_DY, ORB_R);
        for (int i = 0; i < CLOCK_FACE_SPHERE; i++) {
            ui_paint_disc(t, s_orb[i].x, s_orb[i].y, 0.0f, s_orb[i].r, c_sphere, s_orb[i].a);
        }
    }

    const char *label = r->label[0] ? r->label : (r->timer ? "Timer" : "Alarm");
    ui_paint_text(L, &font_panel_28, pal->text, (int32_t)c, (int32_t)c + LABEL_DY, 0, true, label);

    /* "TIME'S UP · +5:11", or the alarm's time for the first half. The dot is
     * drawn rather than set: the 14 px face has no U+00B7. */
    char left[16], right[16];
    if (r->timer) {
        strlcpy(left, "TIME'S UP", sizeof(left));
    } else {
        alarm_fmt_time(r->hh, r->mm, h12, left, sizeof(left));
    }
    overtime((now_ms - r->since_ms) / 1000, right, sizeof(right));
    const lv_font_t *f = &lv_font_montserrat_14;
    const int32_t wl = ui_paint_text_width(f, LINE_SPACE, left);
    const int32_t wr = ui_paint_text_width(f, LINE_SPACE, right);
    const int32_t x0 = (int32_t)c - (wl + LINE_DOT_GAP + wr) / 2;
    const int32_t y = (int32_t)c + LINE_DY;
    ui_paint_text(L, f, c_line, x0 + wl / 2, y, LINE_SPACE, true, left);
    ui_paint_disc(t, (float)(x0 + wl + LINE_DOT_GAP / 2), (float)y + 0.5f, 0.0f, 1.5f, c_line, 1.0f);
    ui_paint_text(L, f, c_line, x0 + wl + LINE_DOT_GAP + wr / 2, y, LINE_SPACE, true, right);
}

static void paint_buttons(const ui_paint_target_t *t, lv_layer_t *L, const ui_ring_t *r,
                          uint32_t now_ms, const ui_panel_palette_t *pal)
{
    const int32_t c = (int32_t)PANEL_C;
    if (r->timer) {
        ui_panel_kit_fill(t->px, t->stride, PANEL_SEG_HALF_BOTTOM, pal->primary);
        ui_panel_kit_icon(L, UI_ICON_BELL_OFF, pal->primary_ink, c, c + STOP_ICON_DY);
        ui_paint_text(L, &font_panel_40, pal->primary_ink, c, c + STOP_TEXT_DY, 0, false, "Stop");
        return;
    }

    ui_panel_kit_fill(t->px, t->stride, PANEL_SEG_QUAD_BR, pal->primary);
    ui_paint_text(L, &font_panel_28, pal->primary_ink, c + QUAD_DX, c + QUAD_TEXT_DY, 0, false,
                  "Stop");
    ui_panel_kit_icon(L, UI_ICON_BELL_OFF, pal->primary_ink, c + QUAD_DX, c + QUAD_ICON_DY);

    /* Snooze: solid on a press, fading to its dark shade over SNOOZE_FADE_MS. */
    float fade = 1.0f;
    if (s_snooze_ms != 0) {
        fade = (float)(now_ms - s_snooze_ms) / (float)SNOOZE_FADE_MS;
        fade = fade < 0.0f ? 0.0f : (fade > 1.0f ? 1.0f : fade);
    }
    const uint32_t fill = mix(pal->primary, pal->secondary, fade);
    /* Whichever ink stands out more from the fill as it is now, rather than
     * fading the ink too -- half way that would be grey on grey -- or flipping
     * at a fixed point, which on the lighter orb colours flips too early. */
    const int32_t yf = (int32_t)ui_tone_luma(fill);
    const int32_t d_dark = yf - (int32_t)ui_tone_luma(pal->primary_ink);
    const int32_t d_light = (int32_t)ui_tone_luma(pal->text) - yf;
    const uint32_t ink = d_dark >= d_light ? pal->primary_ink : pal->text;
    ui_panel_kit_fill(t->px, t->stride, PANEL_SEG_QUAD_BL, fill);
    char lab[20];
    snprintf(lab, sizeof(lab), "+%d min", s_snooze_min > 0 ? s_snooze_min : 1);
    ui_paint_text(L, &font_panel_28, ink, c - QUAD_DX, c + QUAD_TEXT_DY, 0, false, lab);
    ui_panel_kit_icon(L, UI_ICON_ALARM, ink, c - QUAD_DX, c + QUAD_ICON_DY);
}

void ui_ring_frame(const ui_ring_t *r, uint32_t now_ms, uint32_t tint_rgb, bool h12)
{
    if (r->seq != s_seq) {
        s_seq = r->seq;
        s_t0_ms = now_ms;
        s_snooze_min = 0;
        s_snooze_ms = 0;
        s_full = true;
    }
    if (tint_rgb != s_tint) {
        s_tint = tint_rgb;
        s_full = true;
    }

    /* The snooze commits when its fade has run out. */
    bool fading = false;
    if (s_snooze_ms != 0) {
        if (now_ms - s_snooze_ms >= SNOOZE_FADE_MS) {
            ESP_LOGI(TAG, "EVT ring snooze commit %d min", s_snooze_min);
            controls_alarm_snooze(s_snooze_min);
            /* Stays up, dark, until alarms.c ends the ring -- or keeps it if
             * the snooze could not be kept, when a tap starts over. */
            s_snooze_ms = 0;
            s_buttons_dirty = true;
        } else {
            fading = true;
        }
    }

    lv_draw_buf_t *buf = lv_canvas_get_draw_buf(s_canvas);
    const ui_paint_target_t t = {
        .px = (uint16_t *)buf->data,
        .stride = buf->header.stride / sizeof(uint16_t),
        .w = (int)buf->header.w,
        .h = (int)buf->header.h,
    };
    const bool buttons = s_full || s_buttons_dirty || fading;
    if (s_full) {
        memset(t.px, 0, (size_t)buf->header.stride * buf->header.h);
    } else {
        memset(t.px, 0, (size_t)buf->header.stride * TOP_END);
    }

    ui_panel_palette_t pal;
    ui_panel_kit_palette(tint_rgb, &pal);

    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);
    paint_top(&t, &layer, r, now_ms, tint_rgb, h12, &pal);
    if (buttons) {
        paint_buttons(&t, &layer, r, now_ms, &pal);
    }
    lv_canvas_finish_layer(s_canvas, &layer);

    if (buttons) {
        lv_obj_invalidate(s_canvas);
    } else {
        const lv_area_t top = {0, 0, t.w - 1, TOP_END - 1};
        lv_obj_invalidate_area(s_canvas, &top);
    }
    s_full = false;
    s_buttons_dirty = false;
#if CONFIG_UI_RING_SELFTEST
    selftest(r, now_ms);
#endif
}

/* ---------------- self-test ---------------- */

#if CONFIG_UI_RING_SELFTEST
/*
 * BENCH AID, NOT A FEATURE: drives each ring through the same hit test a finger
 * uses, and captures what it shows. The timer cycles for 40 s, then Stop; the
 * alarm takes three snooze taps and commits; its snoozed return is stopped.
 */
static bool shot(uint32_t seq)
{
    static unsigned n;
    char tag[24];
    snprintf(tag, sizeof(tag), "ring-%u-%u", (unsigned)seq, n);
    lv_draw_buf_t *buf = lv_canvas_get_draw_buf(s_canvas);
    if (screenshot_capture((const uint16_t *)buf->data, buf->header.stride / sizeof(uint16_t),
                           (int)buf->header.w, (int)buf->header.h, tag)) {
        n++;
        return true;
    }
    return false;
}

static void selftest(const ui_ring_t *r, uint32_t now_ms)
{
    static uint32_t seq, t0;
    static int step, alarms_seen;
    if (r->seq != seq) {
        seq = r->seq;
        t0 = now_ms;
        step = 0;
        if (!r->timer) {
            alarms_seen++;
        }
    }
    const uint32_t in = now_ms - t0;
    const int32_t c = (int32_t)PANEL_C;

    if (step == 0) {
        if (in >= 1500 && shot(seq)) {
            step = 1;
        }
        return;
    }
    if (r->timer) {
        /* Long enough for chime, announcement, chime again. */
        if (step == 1 && in >= 40000) {
            ESP_LOGI(TAG, "EVT ring selftest stop timer");
            ui_ring_pressed(r, c, c + 120, now_ms);
            step = 2;
        }
    } else if (alarms_seen == 1) {
        static const uint32_t TAP_AT[3] = {3000, 3300, 3600};
        if (step <= 3) {
            if (in >= TAP_AT[step - 1]) {
                ESP_LOGI(TAG, "EVT ring selftest snooze tap %d", step);
                ui_ring_pressed(r, c - QUAD_DX, c + QUAD_TEXT_DY, now_ms);
                step++;
            }
        } else if ((step == 4 && in >= 3700) || (step == 5 && in >= 4400)) {
            /* Solid just after the third tap, then part way through the fade. */
            if (shot(seq)) {
                step++;
            }
        }
    } else if (step == 1 && in >= 5000) {
        ESP_LOGI(TAG, "EVT ring selftest stop alarm");
        ui_ring_pressed(r, c + QUAD_DX, c + QUAD_TEXT_DY, now_ms);
        step = 2;
    }
}
#endif

/* ---------------- touch ---------------- */

void ui_ring_pressed(const ui_ring_t *r, int32_t x, int32_t y, uint32_t now_ms)
{
    const float px = (float)x + 0.5f, py = (float)y + 0.5f;
    const bool stop = r->timer ? panel_seg_sdf(PANEL_SEG_HALF_BOTTOM, px, py) <= 0.0f
                               : panel_seg_sdf(PANEL_SEG_QUAD_BR, px, py) <= 0.0f;
    const bool snooze = r->snoozable && panel_seg_sdf(PANEL_SEG_QUAD_BL, px, py) <= 0.0f;

    if (stop) {
        ESP_LOGI(TAG, "EVT ring stop");
        controls_alarm_stop();
        s_done_seq = r->seq;   /* gone now, not on alarms.c's next tick */
    } else if (snooze) {
        if (s_snooze_min < ALARM_SNOOZE_MAX_MIN) {
            s_snooze_min++;
        }
        s_snooze_ms = now_ms ? now_ms : 1;
        s_buttons_dirty = true;
        ESP_LOGI(TAG, "EVT ring snooze +%d min", s_snooze_min);
    }
}
