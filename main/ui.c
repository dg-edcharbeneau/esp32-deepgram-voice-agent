/*
 * Display, touch, and face dispatch. See ui.h for the threading contract and
 * ui_face.h for what a face is.
 *
 * THE RENDERING ARRANGEMENT
 *
 * Inherited from spec_analyzer_radial, where it was arrived at by measuring
 * variants on this same panel:
 *
 *   direct-to-layer, BSP defaults ....  3.7 fps + task_wdt panic
 *   PSRAM canvas, BSP defaults ......  15 fps
 *   direct-to-layer, full-frame buf ..  does not run at all
 *   PSRAM canvas + internal render ...  this arrangement
 *
 * Two things drive that. First, draw ONCE per frame into a canvas: a custom
 * LV_EVENT_DRAW_MAIN handler is re-invoked once per render chunk and
 * re-rasterises everything each time, whereas a canvas makes the per-chunk cost
 * a memcpy. This is why ui_face_t::render is called exactly once per frame.
 * Second, keep LVGL's render buffer in INTERNAL RAM -- which is why the display
 * is registered here instead of via bsp_display_start(), whose buffer lives in
 * PSRAM and needs a DMA bounce copy on every flush. Do not raise DRAW_ROWS
 * towards a full frame: a PSRAM-sourced SPI transfer needs an internal bounce
 * buffer the same size, so a full frame asks for 434 kB of internal RAM and
 * every flush fails with ESP_ERR_NO_MEM.
 *
 * WHAT IS DIFFERENT FROM THAT PROJECT
 *
 * It had ~300 kB of internal RAM to itself. This shares 288 kB with Wi-Fi, lwIP
 * and TLS, and the failure mode is not boot -- it is the first WebSocket
 * reconnect, when a handshake wants a burst of internal RAM with the display
 * already up. So DRAW_ROWS is halved to 32. Measured while running, the largest
 * free internal block sits around 28-36 kB, which is also why a second render
 * buffer is not an option: it would need another contiguous 29,824 B.
 */

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_lv_adapter.h"
#include "esp_lv_adapter_input.h"

#include "audio_io.h"
#include "orb_colors.h"
#include "ui.h"
#include "ui_face.h"

static const char *TAG = "ui";

#define CENTER_X (BSP_LCD_H_RES / 2)
#define CENTER_Y (BSP_LCD_V_RES / 2)

/*
 * The inner circle is the button. Touching anywhere else does nothing.
 *
 * A literal rather than a radius borrowed from whatever the spectrum face draws:
 * the hit area must not move when the face changes. 70 px is where it has always
 * been. The whole 466x466 screen used to be live, which made brushing the bezel
 * enough to end a conversation.
 */
#define BUTTON_RADIUS 70

#if CONFIG_UI_SHOW_INDICATORS
/*
 * The button, made visible. See CONFIG_UI_SHOW_INDICATORS for what this is for;
 * the constraint worth repeating here is that it must only ever DRAW. Every
 * gesture behaves identically with the flag off, so nothing in this file may
 * branch on it outside a draw path.
 *
 * The ring's radius is BUTTON_RADIUS itself, not a hand-picked "looks about
 * right" value, and lv_draw_arc puts the OUTER edge there and grows inward -- so
 * the ink lies inside the live area and its outer edge is the boundary the hit
 * test actually uses. If the two ever disagree the drawing is wrong, which is
 * the only way an affordance like this stays worth trusting.
 */
#define INDICATOR_WIDTH 2

/* Dim, because the resting state is "here is the button" and it sits under a
 * face that is already using the screen. */
#define INDICATOR_RGB_IDLE 0x4a5560

/* Warm, and deliberately the loudest thing the overlay does: it marks the one
 * moment when a tap means something other than start/stop. */
#define INDICATOR_RGB_INTERRUPT 0xe8a54a
#endif

/* Rows per LVGL render chunk, in internal RAM: 466 * rows * 2 bytes. Must stay
 * small enough that the SPI driver can allocate a DMA buffer of the same size,
 * and small enough that Wi-Fi can still find contiguous internal RAM later. */
#define DRAW_ROWS 32

#define FRAME_MS 33

/* How long after the last sample we treat the display as idle. Slightly longer
 * than one producer hop so a late chunk does not flicker everything to flat. */
#define IDLE_MS 250

/* ---------------- audio level ---------------- */

/*
 * Speech RMS sits far below its peak, so a raw RMS barely moves anything. The
 * curve is below 1 so quiet speech lifts -- the display reads as full at
 * conversational volume rather than only when shouted at.
 *
 * The gains are PER SOURCE, and that is not tidiness. Measured on this board:
 * conversational speech peaks near 0.095 RMS at the microphone, agent playback
 * at full volume reaches 0.31. The reference's single provisional 3.2x put the
 * agent path at amp 0.994 -- pinned at the ceiling for whole sentences, with
 * every bit of expression in the speaking gesture thrown away -- while leaving
 * the microphone peaking at 0.42.
 *
 * This mirrors the reference's own split of inputAmplitude from
 * outputAmplitude; collapsing them into one level was the mistake.
 */
#define AMP_GAIN_MIC (CONFIG_ORB_AMP_GAIN_MIC / 100.0f)
#define AMP_GAIN_AGENT (CONFIG_ORB_AMP_GAIN_AGENT / 100.0f)
#define AMP_CURVE 0.7f

/* Fast attack, slow release: a voice should arrive at once and let go slowly. */
#define AMP_ATTACK_MS 45.0f
#define AMP_RELEASE_MS 240.0f


/*
 * BAND SPLIT
 *
 * Three bands from two cascaded one-pole lowpasses, complementary so they sum
 * back to the input exactly. Each drives a visually distinct job, and that
 * separation is the entire point: low is bulk swell (vowels and the fundamental,
 * so "someone is talking"), mid is a travelling ripple (articulation, so
 * "following the words"), high is ink only and never motion (sibilance, which
 * reads as brightness).
 *
 * As the reference puts it: splitting the response this way is what stops a loud
 * vowel and a sharp consonant from looking identical, which is all a single RMS
 * scalar can ever say.
 *
 * The gains differ wildly because speech does -- roughly 10:3:1 low to mid to
 * high -- so the high band needs about seven times the low band's gain to reach
 * a comparable number.
 */
#define BAND_LOW_HZ 250.0f
#define BAND_MID_HZ 2000.0f
#define BAND_SAMPLE_RATE 16000.0f
/*
 * MEASURED on this board, not the reference's provisional figures.
 *
 * The reference assumes speech is roughly 10:3:1 low:mid:high and picks
 * 3.2/9/22 to equalise that. On this hardware it does not hold: everything here
 * is band-limited to 8 kHz, and a small speaker and mic put proportionally more
 * energy above 250 Hz than full-band speech does. With the original gains the
 * observed peaks were low 0.55, mid 1.00, high 0.96 -- mid pinned at its ceiling,
 * which is precisely where a travelling ripple stops carrying any information
 * about articulation and becomes a constant.
 *
 * Retuned so each band spans its own range across quiet-to-loud speech, which is
 * what the split is for.
 *
 * Set from the acoustic session, WITH the source normalisation above in place.
 * Agent-path peaks landed at low 0.55 / mid 0.60 / high 0.57 against a target of
 * 0.75-0.85, so all three were raised about 1.4x from 4.5/6.0/14.0.
 *
 * The microphone path reads lower than the agent for the same nominal gain, and
 * that is arguably right rather than a miscalibration: a voice across a room IS
 * quieter than a speaker at full volume, `amp` carries gesture depth and is
 * separately calibrated, and the bands only drive the secondary pass. Note the
 * microphone data here is thin -- see the acoustic-protocol flaw recorded in the
 * plan -- so treat the mic side as the weaker of the two numbers.
 */
#define BAND_GAIN_LOW 6.5f
#define BAND_GAIN_MID 8.5f
#define BAND_GAIN_HIGH 20.0f

/*
 * Noise gate with hysteresis, tested on the PRE-curve mean so the threshold
 * means the same thing however the curve is tuned. Opening high and closing low
 * stops a marginal room from flickering the response on and off.
 *
 * Shutting publishes zero as a TARGET, never a freeze: the release filter
 * carries the shell down instead of dropping it.
 */
#define VAD_OPEN 0.06f
#define VAD_CLOSE 0.025f

/*
 * Setup QR geometry. 200 px leaves the code comfortably inside the 466 px
 * circular mask with its quiet zone intact, and lifting it 30 px above centre
 * makes room for the network name underneath.
 */
#define QR_SIZE   200
#define QR_Y_LIFT 30

/* ---------------- faces ---------------- */

static const ui_face_t *const s_faces[] = {
    &ui_face_orb,
    &ui_face_spectrum,
};
#define FACE_COUNT (sizeof(s_faces) / sizeof(s_faces[0]))

static const ui_face_t *s_face;          /* active; LVGL task only */
static size_t s_face_index;              /* which slot s_face is */
static bool s_face_ready[FACE_COUNT];    /* has init() succeeded */

/* Requested from any task, applied by the frame timer -- the same deferred
 * pattern as s_qr_payload, and for the same reason: bringing a face up touches
 * LVGL. -1 means nothing pending. */
static volatile int s_face_want = -1;

/*
 * Requested colour index, -1 for nothing pending. Same handoff as s_face_want:
 * written by whichever task takes the agent's function call, applied on the LVGL
 * task before anything draws.
 */
static volatile int s_color_want = -1;

/*
 * The live ink colour, replaced with CONFIG_UI_DEFAULT_ORB_COLOR's during
 * build_ui(). Initialised to white here rather than left in .bss on purpose:
 * zero is 0xRRGGBB black, and black ink on the black ground is an invisible orb
 * -- which reads as a dead panel, not as a bug worth reporting. So the static
 * initialiser is the safe value even if the config lookup ever moves or fails.
 */
static uint32_t s_tint_rgb = 0xFFFFFFu;

/* ---------------- display test ---------------- */

/*
 * A scripted walk through everything the orb can draw, one step per tap.
 *
 * The point is that most of these are otherwise almost impossible to look at:
 * several last a second or two mid-turn, the connection rungs need a network
 * failure to provoke, and THINKING cannot occur in a real session at all.
 *
 * ONLY ONE MODIFIER GETS A STEP, because only one of the four changes what the
 * ORB draws. face_orb.c reads frozen and press_active; idle and stopped are the
 * spectrum's, and on the orb they are already expressed as states -- idle is
 * step 0 and stopped is what resolve_behaviour() turns into DISCONNECTED at
 * step 7. Giving them steps of their own would add two poses indistinguishable
 * from plain listening, which is worse than not covering them.
 *
 * press_active has no step either: every tap demonstrates it on the way past.
 *
 * So: the eight states, then frozen against LISTENING -- the state whose gesture
 * responds most visibly to the microphone, which stays live throughout. See
 * audio_io_capture_set_monitor().
 */
typedef struct {
    ui_behaviour_t behaviour;
    bool frozen;
    const char *label; /* literal: update_status_label() compares by pointer */
} ui_test_step_t;

/*
 * ONE STEP PER STATE, and the label names the animation it now draws.
 *
 * The five modes had their own steps while they were dormant. They do not need
 * them any more: a state selects its mode in face_orb.c, so scripting the STATE
 * exercises exactly the path a real session takes -- and separate mode steps
 * became exact duplicates of the state steps once the wiring landed.
 *
 * Only three states still draw the shell. Its other five behaviours are now
 * unreachable in service, though the harness still diffs all eight.
 */
static const ui_test_step_t s_test_steps[] = {
    { UI_BEHAVIOUR_IDLE, false, "idle (shell)" },
    { UI_BEHAVIOUR_INITIALIZING, false, "initializing (shell)" },
    { UI_BEHAVIOUR_LISTENING, false, "listening (fill, in)" },
    { UI_BEHAVIOUR_THINKING, false, "thinking (rubik)" },
    { UI_BEHAVIOUR_SPEAKING, false, "speaking (fill, out)" },
    { UI_BEHAVIOUR_CONNECTING, false, "connecting (web)" },
    { UI_BEHAVIOUR_BUFFERING, false, "buffering (braid)" },
    { UI_BEHAVIOUR_DISCONNECTED, false, "disconnected (shell)" },
    /* frozen holds the clock, which is orb-wide, so the shell shows it clearest. */
    { UI_BEHAVIOUR_IDLE, true, "mod: frozen" },
};
#define TEST_STEP_COUNT (sizeof(s_test_steps) / sizeof(s_test_steps[0]))

static volatile bool s_test_want;   /* set from the agent task */
/* Both defined below select_face(), which they use. */
static void test_enter(void);
static void test_advance(void);
static bool s_test_active;          /* LVGL task only, below here */
static size_t s_test_step;
static size_t s_test_face_restore;

/* Latched on a switch, cleared when the telemetry is read. */
static volatile bool s_face_changed;

/* Defined with the rest of setup; the frame timer needs it to apply a switch. */
static esp_err_t select_face(size_t idx);

/* ---------------- cross-task state ---------------- */

/* 32-bit ms: written by the audio tasks, read by the LVGL task. Same narrowing
 * as the stamps in audio_io.c, and for the same reason -- a 64-bit store is two
 * halves here, so a reader could catch one mid-write. */
static volatile uint32_t s_last_feed_ms;
static ui_source_t s_source;

/*
 * Level of the newest block, as amp * 255, PEAK-HELD until the frame timer
 * reads it.
 *
 * Peak-held rather than last-written because the two rates do not divide: agent
 * blocks land every 32 ms, mic blocks every 80 ms, and a frame is ~58 ms. A
 * plain "newest value" would drop transients that fall between frames, and a
 * take-and-clear would read zero on the frames where no mic block happened to
 * arrive -- fluttering at the beat frequency between 80 ms and the frame
 * period. Holding the peak and letting the reader distinguish "nothing new"
 * from "genuinely silent" (via idle) gets both cases right.
 *
 * One writer only -- ui_feed_mic() defers to the agent -- so the read-modify
 * -write below needs no lock. The reader's exchange can at worst let one stale
 * block's peak survive an extra frame, which is invisible in a level meter.
 *
 * ONE SLOT PER SOURCE, which is what keeps the level describing the behaviour on
 * screen. A single shared slot made the level whichever source spoke last, so the
 * opening buffer of a reply retargeted it while the state was still LISTENING and
 * the fill collapsed to the agent's near-silence mid-sentence. Separate slots also
 * mean the two writers no longer share a word at all.
 */
static volatile uint32_t s_level_peak[2]; /* [0] agent, [1] mic */

/*
 * Channel indices for every per-source array here. Same order as s_band, so one
 * index reads across all of them.
 */
#define LVL_AGENT 0
#define LVL_MIC 1

/*
 * When each source last delivered a block, as opposed to when ANY source did.
 *
 * Per-source because the channels have to release independently: the microphone
 * stops feeding entirely while the agent speaks (ui_feed_mic defers, and the
 * capture task is gated outright), so a shared stamp would hold the mic channel
 * frozen at its last speech level for the whole reply instead of letting it fall.
 */
static volatile uint32_t s_feed_ms[2];

/*
 * The smoothed levels a face reads, one set per source.
 *
 * Up here with the other level state rather than next to the accumulator that
 * samples it, because resolve_behaviour()'s local speech gate reads the
 * microphone channel and sits well above that.
 */
static float s_amp[2], s_low[2], s_mid[2], s_high[2];

/*
 * Crossover state, one set per source.
 *
 * Separate because the mic and the agent are unrelated signals: carrying the
 * 2 kHz pole's state across a switch injects a step the size of the difference
 * between them, which the split then reports as a transient that never happened.
 */
typedef struct {
    float y1, y2;
} band_state_t;
static band_state_t s_band[2]; /* [0] agent, [1] mic */
static bool s_vad_open;

/*
 * The two crossover poles. Exact, not the x/(1+x) approximation: 2 kHz at 16 kHz
 * is nowhere near low enough relative to the sample rate for that to hold.
 *
 * Resolved once in build_ui() rather than lazily on first use. publish_level()
 * runs on BOTH audio tasks, and a lazy `if (a1 == 0.0f)` there could be entered
 * by one task while the other had set a1 and not yet a2 -- leaving a2 at zero for
 * a block, which collapses the mid and high bands into the input. Neither tap is
 * attached until ui_start() has returned, so by the time either can be called
 * these are already written.
 */
static float s_pole_low, s_pole_mid;

/*
 * When the microphone last carried actual SPEECH, as opposed to merely carrying
 * data.
 *
 * The distinction is the whole point. The capture tap fires every 80 ms for as
 * long as a session is open, so "a block arrived recently" is true essentially
 * always and cannot mean listening -- driving the behaviour off it pinned the
 * display in LISTENING permanently and IDLE was never shown at all.
 *
 * The noise gate is the right signal, and the telemetry shows it discriminating
 * cleanly: a quiet room reads amp 0.11 through the full-band path while all three
 * bands sit at 0.00.
 */
/*
 * MILLISECONDS IN 32 BITS, not microseconds in 64, and two variables rather than
 * one. Both for the same reason: this is now written from two tasks.
 *
 * s_speech_ms is Deepgram's, stamped by ui_note_user_speech() on the WebSocket
 * task. s_local_ms is the display's own -- the noise gate and the handoff below
 * -- and is touched only by the LVGL task. When they shared one variable there
 * were two writers on a 64-bit value that a 32-bit CPU stores in two halves, so a
 * read could land mid-write and see a timestamp that never existed. A 32-bit
 * store is indivisible here, and with one writer each there is nothing to tear.
 *
 * Elapsed time is computed per variable and the SMALLER taken, rather than taking
 * the later timestamp: unsigned subtraction is correct across the 49-day wrap,
 * comparing the stamps themselves is not.
 */
static volatile uint32_t s_speech_ms;
static uint32_t s_local_ms;

/*
 * How long LISTENING outlives the gate closing.
 *
 * Speech is full of gaps -- stops, breaths, the pause before a word -- and the
 * gate closes in every one of them. Without a hold the display would flicker
 * between listening and idle several times per sentence. Comfortably longer than
 * a within-sentence pause, comfortably shorter than a turn.
 */
/*
 * How long LISTENING outlives the last speech event.
 *
 * WAS 700 ms, for a local gate that fired on every block it opened on. Deepgram's
 * detector marks the START of an utterance and nothing else, and measured on real
 * speech its events came 0.2 s, 3.8 s and 2.0 s apart -- so a 700 ms hold dropped
 * out of LISTENING mid-sentence, repeatedly.
 *
 * 5 s covers the observed gaps with margin, and lingering is not the fault it
 * would be elsewhere: after the user stops, the device genuinely is still
 * listening. Nor does it delay the reply, because resolve_behaviour() tests
 * playback BEFORE this -- the moment the agent speaks, SPEAKING wins outright.
 */
#define LISTEN_HOLD_MS 5000u

/*
 * Local speech gate: the microphone's own evidence that someone is talking,
 * opening at MIC_SPEECH_OPEN and closing at MIC_SPEECH_CLOSE.
 *
 * WHY THIS EXISTS. LISTENING used to be reachable only through Deepgram's
 * UserStartedSpeaking, which is better evidence but arrives late -- measured at
 * 1.77 s after the agent stopped and the user had already begun. For that whole
 * second and a half the resolver fell through to IDLE while the level sat at
 * 0.09, so the orb went blank in the middle of someone speaking. The device
 * already knows; this lets it say so and lets the VAD confirm behind it.
 *
 * HYSTERESIS, and thresholds from measurement rather than feel. A plain "a block
 * arrived" test was tried before and pinned this permanently in LISTENING -- IDLE
 * was never drawn at all. Measured on device: a live-mic quiet room smooths to
 * 0.009-0.014, a normal voice to 0.031-0.093, peaks well past 0.3. Opening at
 * 0.030 clears the room with margin; closing at 0.020 stays above it so a pause
 * between words does not chatter the gate shut.
 *
 * Deliberately conservative: a false NEGATIVE just falls back to waiting for the
 * VAD, which is what used to happen anyway. A false positive brings back the
 * never-idle bug, which is worse. Very quiet speech is left to the VAD.
 *
 * Self-triggering is not a risk. The microphone delivers nothing at all while the
 * agent speaks -- the capture task is gated, and ui_feed_mic() defers regardless
 * -- so the agent's own voice cannot open this.
 */
#define MIC_SPEECH_OPEN 0.030f
#define MIC_SPEECH_CLOSE 0.020f

/*
 * Minimum time on screen for the connection rungs.
 *
 * Measured: BUFFERING lasted 130-220 ms and INITIALIZING 230-270 ms, both SHORTER
 * THAN THE 280 ms CROSSFADE -- so neither ever fully arrived, and the ladder built
 * to make progress legible had two rungs nobody could see. Holding them past the
 * blend costs a few hundred milliseconds of latency on a visual, which is the
 * whole point of showing them at all.
 */
#define DWELL_BUFFERING_US 500000
#define DWELL_INITIALIZING_US 700000

/* Calibration high-water marks. LVGL task only; see update_amp(). */
static float s_amp_peak_mic;
static float s_amp_peak_agent;

/* Session state, for the middle of the screen. Set from the WebSocket task; the
 * pointer is stored, never the characters, so this is a single word write. */
static const char *s_status = "starting";

/* Set from any task, applied by the frame timer -- see update_qr(). */
static const char *volatile s_qr_payload;
static lv_obj_t *qr_obj;
static bool s_session_live;

static volatile bool s_stopped;
static volatile bool s_failed;

/*
 * The connection phase the session layer last reported, or NONE.
 *
 * Only the phases need this: every conversational state is visible in the audio
 * path, and anything the audio path can see wins over this -- see
 * resolve_behaviour().
 */
#define UI_BEHAVIOUR_NONE ((ui_behaviour_t)0xFF)
static volatile ui_behaviour_t s_reported = UI_BEHAVIOUR_NONE;
/* 32-bit ms: set on the WebSocket task, read on the LVGL task. */
static volatile uint32_t s_reported_ms;

/*
 * INITIALIZING is an assembly animation, not a resting state, so it expires.
 *
 * Normally the greeting starts moments after the session goes ready and SPEAKING
 * takes over long before this matters. But a resumed session sends no greeting
 * -- see history_to_json() in dg_agent.c -- and without a deadline the shell
 * would sit there assembling forever. One assemble period is exactly long enough
 * to read as having completed.
 */
#define INITIALIZING_MAX_MS 4000

/* ---------------- LVGL-owned state ---------------- */

static lv_obj_t *canvas_obj;
static lv_obj_t *status_label;
#if CONFIG_UI_SHOW_INDICATORS
static lv_obj_t *hint_label;
#endif
static uint32_t s_frame;
static lv_indev_t *s_touch;
static void (*s_gesture_handler)(ui_gesture_t gesture);
/* Both LVGL-task only. Separate flags on purpose -- see gesture_event_cb. */
static bool s_press_in_button;  /* gate: did this press start on the button? */
static bool s_press_active;     /* visual: is a finger down on it right now? */

/* ---------------- feed side (audio tasks) ---------------- */

/*
 * Block RMS, shaped, on the audio task. A few flops per sample -- there is no
 * FFT in this path at all, which is what lets it run at priority 7 without
 * being noticed.
 *
 * int64 accumulation is not paranoia: 1280 samples of full-scale square is
 * 2^40, well past a 32-bit sum.
 */
/* Clamp, apply the perceptual curve, and quantise to a byte. */
static uint32_t shape_to_byte(float v)
{
    /* !(x > 0) rather than x < 0, so a NaN lands on silence instead of poisoning
     * every coordinate downstream of it. */
    if (!(v > 0.0f)) {
        return 0;
    }
    if (v > 1.0f) {
        v = 1.0f;
    }
    return (uint32_t)(powf(v, AMP_CURVE) * 255.0f);
}

static void publish_level(const int16_t *mono, size_t samples, ui_source_t src)
{
    if (samples == 0) {
        return;
    }

    const float a1 = s_pole_low, a2 = s_pole_mid;

    band_state_t *st = &s_band[(src == UI_SRC_MIC) ? 1 : 0];
    float y1 = st->y1, y2 = st->y2;
    float s_full = 0.0f, s_low = 0.0f, s_mid = 0.0f, s_high = 0.0f;

    for (size_t i = 0; i < samples; i++) {
        float x = (float)mono[i] / 32768.0f;
        y1 += a1 * (x - y1);
        y2 += a2 * (x - y2);
        /* Complementary, so the three sum back to x exactly. */
        float b_low = y1;
        float b_mid = y2 - y1;
        float b_high = x - y2;

        s_full += x * x;
        s_low += b_low * b_low;
        s_mid += b_mid * b_mid;
        s_high += b_high * b_high;
    }

    /* A NaN would otherwise persist in the filter state forever. */
    if (!isfinite(y1) || !isfinite(y2)) {
        y1 = y2 = 0.0f;
    }
    st->y1 = y1;
    st->y2 = y2;

    float inv_n = 1.0f / (float)samples;
    float gain = (src == UI_SRC_MIC) ? AMP_GAIN_MIC : AMP_GAIN_AGENT;

    /*
     * Normalise for the source BEFORE the band gains, so the band gains describe
     * speech's spectral tilt and nothing else.
     *
     * Without this the bands carry the same fault the amplitude used to: agent
     * playback runs about 3x hotter than the microphone (RMS 0.31 against 0.095),
     * so a shared gain pins the agent's mid band at 1.00 -- where a travelling
     * ripple stops describing articulation and becomes a constant -- while the
     * microphone barely moves them. The ratio here is that measured 3x, expressed
     * as the two calibrated amplitude gains so there is one place to retune.
     *
     * Filtering is linear, so scaling the band RMS afterwards is identical to
     * scaling the samples going in, and costs nothing per sample.
     */
    float src_norm = (src == UI_SRC_MIC) ? 1.0f : (AMP_GAIN_AGENT / AMP_GAIN_MIC);

    float r_low = sqrtf(s_low * inv_n) * BAND_GAIN_LOW * src_norm;
    float r_mid = sqrtf(s_mid * inv_n) * BAND_GAIN_MID * src_norm;
    float r_high = sqrtf(s_high * inv_n) * BAND_GAIN_HIGH * src_norm;

    float raw = (r_low + r_mid + r_high) / 3.0f;

    s_vad_open = s_vad_open ? (raw > VAD_CLOSE) : (raw > VAD_OPEN);

    /*
     * The full-band amplitude keeps its own calibrated gain and is deliberately
     * NOT gated: it sets how deep a gesture goes, and should follow a quiet voice
     * smoothly down rather than being cut off at a threshold. The bands are
     * gated, because a travelling ripple driven by room noise is just noise.
     */
    uint32_t q_amp = shape_to_byte(sqrtf(s_full * inv_n) * gain);

    uint32_t q_low = 0, q_mid = 0, q_high = 0;
    if (s_vad_open) {
        q_low = shape_to_byte(r_low);
        q_mid = shape_to_byte(r_mid);
        q_high = shape_to_byte(r_high);
    }

    /*
     * Peak-hold all four in one word, then a single store. One writer only, so
     * the read-modify-write needs no lock; the reader clears with an exchange.
     */
    volatile uint32_t *slot = &s_level_peak[(src == UI_SRC_MIC) ? LVL_MIC : LVL_AGENT];
    uint32_t prev = *slot;
    uint32_t b0 = prev & 0xFF, b1 = (prev >> 8) & 0xFF;
    uint32_t b2 = (prev >> 16) & 0xFF, b3 = (prev >> 24) & 0xFF;
    *slot = ((q_amp > b0 ? q_amp : b0)) |
            ((q_low > b1 ? q_low : b1) << 8) |
            ((q_mid > b2 ? q_mid : b2) << 16) |
            ((q_high > b3 ? q_high : b3) << 24);
}

static void feed(const int16_t *mono, size_t samples, ui_source_t src)
{
    s_source = src;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    s_last_feed_ms = now;
    s_feed_ms[(src == UI_SRC_MIC) ? LVL_MIC : LVL_AGENT] = now;

    publish_level(mono, samples, src);

    /*
     * Read once. The frame timer can swap s_face between this test and the
     * call, and a face that is being switched away from would rather receive
     * one stale block than have its feed_pcm pointer read as NULL mid-call.
     */
    const ui_face_t *face = s_face;
    if (face != NULL && face->feed_pcm != NULL) {
        face->feed_pcm(mono, samples);
    }
}

void ui_feed_agent(const int16_t *mono, size_t samples)
{
    feed(mono, samples, UI_SRC_AGENT);
}

void ui_feed_mic(const int16_t *mono, size_t samples)
{
    /*
     * Agent audio wins, so only one task is ever inside feed(). The half-duplex
     * gate in audio_io.c already stops the capture task before it gets here, so
     * this is belt and braces -- kept because it is what would keep the two
     * writers apart if that gate ever moved. The residual
     * sliver -- playback ending between this check and the tap firing -- costs
     * at worst one garbled window, which is not worth a mutex on a priority-7
     * audio task.
     */
    if (audio_io_playback_active()) {
        return;
    }
    feed(mono, samples, UI_SRC_MIC);
}

void ui_show_qr(const char *payload)
{
    s_qr_payload = payload;
}


void ui_set_gesture_handler(void (*handler)(ui_gesture_t gesture))
{
    s_gesture_handler = handler;
}

void ui_set_stopped(bool stopped)
{
    s_stopped = stopped;
}

void ui_set_behaviour(ui_behaviour_t behaviour)
{
    /* Stamp before the behaviour, so the frame timer cannot read a new phase
     * against the previous one's clock. */
    s_reported_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_reported = behaviour;
}

void ui_set_face(int index)
{
    if (index < 0 || (size_t)index >= FACE_COUNT) {
        ESP_LOGW(TAG, "EVT setface bad-index=%d", index);
        return;
    }
    s_face_want = index;
}

void ui_set_orb_color(int index)
{
    if (index < 0 || (size_t)index >= orb_colors_count()) {
        ESP_LOGW(TAG, "EVT setcolor bad-index=%d", index);
        return;
    }
    s_color_want = index;
}

void ui_note_user_speech(void)
{
    s_speech_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

void ui_start_display_test(void)
{
    s_test_want = true;
}

/*
 * Decide what the display should be showing.
 *
 * THE AUDIO PATH WINS. audio_io_playback_active() is tied to what the speaker is
 * actually doing, including audio_io_flush() on a barge-in; a live microphone
 * likewise. The agent's own messages are only consulted for states no audio
 * accompanies. Trusting a message over the hardware would leave the display
 * radiating at a silent speaker for the rest of a turn after a barge-in.
 */
static ui_behaviour_t resolve_behaviour(bool idle, int64_t now_us)
{
    /*
     * BEFORE EVERY EARLY RETURN below, because this is an edge and an edge cannot
     * be sampled only on the frames we happen to reach. Left below the guards, a
     * session that stopped mid-reply kept was_speaking true and the next session's
     * first silent frame invented a handoff that never happened.
     */
    const bool speaking_now = audio_io_playback_active();
    static bool was_speaking;
    const bool handoff = (was_speaking && !speaking_now);
    was_speaking = speaking_now;

    /* Every clock this function compares against is 32-bit ms now, so convert
     * once here rather than at each use. */
    const uint32_t now_ms = (uint32_t)(now_us / 1000);

    /*
     * FIRST, and it has to be. The session is stopped for the whole test, so the
     * s_stopped check below would swallow every step into DISCONNECTED.
     */
    if (s_test_active) {
        return s_test_steps[s_test_step].behaviour;
    }

    if (s_stopped) {
        return UI_BEHAVIOUR_DISCONNECTED;
    }

    ui_behaviour_t reported = s_reported;

    /*
     * The connection ladder outranks the audio path. Before a session is up,
     * audio says nothing worth showing -- and these three rungs are the whole
     * point of the ladder: each is visibly fuller and brighter than the last, so
     * progress reads without anyone having to read the label.
     */
    if (reported == UI_BEHAVIOUR_CONNECTING || reported == UI_BEHAVIOUR_BUFFERING) {
        return reported;
    }
    if (!s_session_live && reported != UI_BEHAVIOUR_INITIALIZING) {
        return UI_BEHAVIOUR_CONNECTING;
    }

    /*
     * Inside a live session the AUDIO PATH WINS. audio_io_playback_active() and a
     * live mic describe what the hardware is actually doing, including
     * audio_io_flush() on a barge-in; a reported state describes only what the
     * agent last claimed. Trusting the claim would leave the shell radiating at a
     * silent speaker for the rest of a turn after an interruption.
     */
    if (speaking_now) {
        return UI_BEHAVIOUR_SPEAKING;
    }

    /* Assembly gets to finish, unless real audio arrives to supersede it. */
    if (reported == UI_BEHAVIOUR_INITIALIZING &&
        (now_ms - s_reported_ms) < INITIALIZING_MAX_MS) {
        return UI_BEHAVIOUR_INITIALIZING;
    }

    /*
     * Speech, not merely data -- testing "a block arrived" instead kept this
     * permanently in LISTENING and IDLE was never once drawn.
     *
     * Two ways in. Deepgram's UserStartedSpeaking stamps s_speech_ms and is the
     * better evidence; the local gate below is the faster one, and covers the
     * second-and-a-half before the message arrives. Either opens the same 5 s
     * hold, so a pause mid-sentence is bridged the same way whichever noticed.
     *
     * Reading last frame's smoothed level: resolve_behaviour runs before
     * update_amp for this frame. One frame of lag on a 5 s hold is nothing.
     */
    /*
     * THE HANDOFF. The moment the agent stops talking, the device is listening --
     * the microphone is open and streaming to Deepgram, and a reply is what it is
     * waiting for. IDLE claims nothing is happening, which is the wrong answer and
     * a jarring one: measured at 1.78 s of dead orb between the agent finishing
     * and the user starting, which reads as a fault rather than as a pause.
     *
     * Stamping the same clock the gate and the VAD use, so the turn opens with a
     * full LISTENING hold that real speech then extends. A long silence still
     * falls through to IDLE once the hold runs out, which is what IDLE is for.
     */
    if (handoff) {
        s_local_ms = now_ms;
    }

    static bool gate_open;
    float mic = s_amp[LVL_MIC];
    gate_open = gate_open ? (mic > MIC_SPEECH_CLOSE) : (mic > MIC_SPEECH_OPEN);
    if (gate_open) {
        s_local_ms = now_ms;
    }

    /* Per-variable elapsed, smaller wins. Wrap-safe; comparing stamps is not. */
    const uint32_t since_vad = now_ms - s_speech_ms;
    const uint32_t since_local = now_ms - s_local_ms;
    const uint32_t since = (since_vad < since_local) ? since_vad : since_local;
    if (since < LISTEN_HOLD_MS) {
        return UI_BEHAVIOUR_LISTENING;
    }

    return UI_BEHAVIOUR_IDLE;
}

void ui_set_status(const char *text, bool session_live)
{
    s_status = (text != NULL) ? text : "";
    s_session_live = session_live;
}

void ui_set_failed(bool failed)
{
    s_failed = failed;
}

/* ---------------- frame ---------------- */

static void update_status_label(ui_behaviour_t beh)
{
    /*
     * All candidates are string literals, so the "has it changed" test is a
     * pointer compare. That matters: every rewrite adds another invalid area
     * and therefore another render pass.
     */
    const char *want;
    if (s_test_active) {
        /* Which state is on screen -- without it the test is a slideshow of
         * unlabelled poses and you cannot tell a miss from a subtle one. */
        want = s_test_steps[s_test_step].label;
    } else if (!s_session_live) {
        want = s_status; /* connecting / error / disconnected */
    } else if (beh == UI_BEHAVIOUR_SPEAKING) {
        want = "speaking";
    } else if (beh == UI_BEHAVIOUR_LISTENING) {
        want = "listening";
    } else {
        want = s_status; /* "ready" */
    }
    /*
     * DERIVED FROM THE RESOLVED BEHAVIOUR, not from a rule of its own.
     *
     * This used to test playback and `!idle && s_source == UI_SRC_MIC`
     * independently of resolve_behaviour, so the label and the orb could disagree
     * by construction -- and did: the screen read "ready" while the orb drew IDLE
     * mid-conversation, because s_source is only ever "whichever tap fired last".
     * One state, one answer, and the label cannot drift from the picture again.
     */

    static const char *shown;
    if (want != shown) {
        shown = want;
        lv_label_set_text(status_label, want);
    }
}

#if CONFIG_UI_SHOW_INDICATORS
/*
 * Draw the touch target and name the action it currently carries.
 *
 * CALLED AFTER THE FACE, always. The orb clears only the boxes its own dots
 * occupied last frame, so anything drawn before it is partly erased and the
 * overlay would come out moth-eaten; drawing last means the ring is simply on
 * top. It also means the ring has to invalidate its own bounding box -- the orb
 * invalidates dots, not this -- which is the frame cost the Kconfig help warns
 * about.
 *
 * WHERE THE ANSWER COMES FROM
 *
 * audio_io_playback_active(), because that is the exact predicate main.c's
 * on_gesture() branches on. Not a state of its own, and not the resolved
 * behaviour: SPEAKING is a presentation state with dwell times and crossfades
 * layered over it, so a hint keyed to it would go on promising "interrupt" after
 * the tap had stopped doing that. Two readings of one predicate can disagree by
 * at most a frame; two different predicates disagree by design.
 *
 * The one thing it does not model is main.c's post-interrupt grace: for
 * INTERRUPT_GRACE_MS after an interrupt a tap does nothing at all, and this will
 * already be reading "stop". Left alone deliberately -- a bench overlay that
 * reaches into another file's timers to be right for another second and a half
 * is a worse trade than a window this short being optimistic.
 */
static void draw_indicators(void)
{
    const char *hint;
    bool interrupt = false;

    if (s_qr_payload != NULL) {
        /* Provisioning owns the screen, the code is what to look at, and none of
         * the gestures below apply. Nothing drawn, nothing said. */
        hint = "";
    } else if (s_test_active) {
        hint = "tap: advance";
    } else if (audio_io_playback_active()) {
        interrupt = true;
        hint = "tap: interrupt";
    } else if (s_stopped) {
        hint = "tap: start";
    } else {
        hint = "tap: stop";
    }

    /* Pointer compare, like update_status_label(), and for the same reason: a
     * rewrite is another invalid area and another render pass. Every branch above
     * yields a literal, so this is exact. */
    static const char *shown;
    if (hint != shown) {
        shown = hint;
        lv_label_set_text(hint_label, hint);
    }

    if (s_qr_payload != NULL) {
        return;
    }

    lv_layer_t layer;
    lv_canvas_init_layer(canvas_obj, &layer);

    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = lv_color_hex(interrupt ? INDICATOR_RGB_INTERRUPT : INDICATOR_RGB_IDLE);
    dsc.width = INDICATOR_WIDTH;
    dsc.center.x = CENTER_X;
    dsc.center.y = CENTER_Y;
    dsc.radius = BUTTON_RADIUS;
    dsc.start_angle = 0;
    dsc.end_angle = 360;
    dsc.opa = LV_OPA_COVER;
    lv_draw_arc(&layer, &dsc);

    lv_canvas_finish_layer(canvas_obj, &layer);

    /* One pixel of slop for the anti-aliased outer edge. The canvas sits at 0,0
     * and covers the panel, so widget and screen coordinates coincide -- the same
     * assumption orb_raster.c documents where it invalidates. */
    const int32_t r = BUTTON_RADIUS + 1;
    lv_area_t area = {
        .x1 = CENTER_X - r,
        .y1 = CENTER_Y - r,
        .x2 = CENTER_X + r,
        .y2 = CENTER_Y + r,
    };
    lv_obj_invalidate_area(canvas_obj, &area);
}
#endif /* CONFIG_UI_SHOW_INDICATORS */

/*
 * Creates or tears down the QR overlay to match what was last requested.
 *
 * Pointer compare, like update_status_label(): rebuilding the code every frame
 * would re-encode it and re-invalidate a 200 px square sixty times a second.
 *
 * The status label moves down while the code is up rather than being hidden --
 * the AP name underneath is the fallback for a camera that will not act on a
 * WIFI: URI, and it is also what someone reads when picking the network by hand.
 */
static void update_qr(void)
{
    static const char *shown;
    const char *want = s_qr_payload;
    if (want == shown) {
        return;
    }
    shown = want;

    if (want == NULL) {
        if (qr_obj != NULL) {
            lv_obj_delete(qr_obj);
            qr_obj = NULL;
        }
        lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    if (qr_obj == NULL) {
        qr_obj = lv_qrcode_create(lv_screen_active());
        if (qr_obj == NULL) {
            return;
        }
        lv_qrcode_set_size(qr_obj, QR_SIZE);
        /* Dark-on-light regardless of what is behind it: a scanner needs the
         * quiet zone and the contrast, not the house style. */
        lv_qrcode_set_dark_color(qr_obj, lv_color_black());
        lv_qrcode_set_light_color(qr_obj, lv_color_white());
        lv_obj_align(qr_obj, LV_ALIGN_CENTER, 0, -QR_Y_LIFT);
    }

    if (lv_qrcode_update(qr_obj, want, strlen(want)) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "could not encode the setup QR");
        lv_obj_delete(qr_obj);
        qr_obj = NULL;
        lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    /* Below the code, still inside the circular mask. */
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, QR_SIZE / 2 - QR_Y_LIFT + 30);
}

/*
 * TELEMETRY ACCUMULATOR
 *
 * One window's worth of measurement, read and reset by ui_get_telemetry(). The
 * frame timer only ever adds to these; nothing here formats or logs, because
 * doing that on the LVGL task blocks on the UART for most of a frame and
 * corrupts the numbers being collected. See ui_telemetry_t in ui.h.
 *
 * Two timings, and the gap between them is the point. "draw" is what the frame
 * callback spends filling the canvas. The frame PERIOD is what the eye actually
 * sees, and it is not FRAME_MS just because the lv_timer asked for it: LVGL runs
 * the timer, then copies every invalidated chunk out of PSRAM and pushes it over
 * a 40 MHz QSPI bus before coming back. A full 466x466 RGB565 frame is 434 kB,
 * about 21.7 ms of bus time the callback never sees. A small draw next to a large
 * period means the panel is the limit and optimising the drawing buys nothing.
 */

/* Which channel the behaviour on screen selected this frame; for telemetry, so
 * the reported src and amp always describe the same signal. */
static ui_source_t s_level_sel = UI_SRC_MIC;

/*
 * Which audio the behaviour on screen is ABOUT.
 *
 * LISTENING is the user talking, so it reads the microphone; SPEAKING is the agent
 * talking, so it reads playback. Neither follows s_source, which is only ever
 * "whichever tap fired last" -- and that was the bug: the first playback buffer of
 * a reply retargeted the level while the state was still LISTENING, so the fill
 * dropped to the agent's opening near-silence in the middle of a sentence.
 *
 * The rest have no opinion, and keep the old behaviour of following the live
 * source. They either ignore amp or are not about either party's voice.
 */
static ui_source_t source_for(ui_behaviour_t b)
{
    switch (b) {
    case UI_BEHAVIOUR_LISTENING: return UI_SRC_MIC;
    case UI_BEHAVIOUR_SPEAKING:  return UI_SRC_AGENT;
    /*
     * Passing s_source straight through, UI_SRC_NONE included: before any audio
     * has ever arrived there is genuinely no source, and reporting one would be a
     * worse answer than admitting it. The channel below resolves NONE to the
     * playback side, which is silent then anyway.
     */
    default:                     return s_source;
    }
}

/* The array index for a source. NONE has no channel of its own; it resolves to
 * the playback side, which is silent whenever NONE is true. */
static int level_channel(ui_source_t src)
{
    return (src == UI_SRC_MIC) ? LVL_MIC : LVL_AGENT;
}


static struct {
    int64_t last_us;      /* previous frame's start, for the period */
    int64_t draw_sum, draw_max;
    int64_t period_sum;
    uint32_t frames;      /* draws accumulated */
    uint32_t periods;     /* draws that had a predecessor */

    float amp_sum, amp_max;
    float low_sum, low_max;
    float mid_sum, mid_max;
    float high_sum, high_max;
} s_tlm;

static void tlm_accumulate_frame(int64_t draw_start_us)
{
    int64_t draw = esp_timer_get_time() - draw_start_us;

    s_tlm.draw_sum += draw;
    s_tlm.frames++;
    if (draw > s_tlm.draw_max) {
        s_tlm.draw_max = draw;
    }

    /* Skip the first interval: last_us is zero, so the "period" would be the
     * whole uptime since boot. */
    if (s_tlm.last_us != 0) {
        s_tlm.period_sum += draw_start_us - s_tlm.last_us;
        s_tlm.periods++;
    }
    s_tlm.last_us = draw_start_us;
}

static void tlm_accumulate_audio(void)
{
    /* The SELECTED channel, so amp= in the telemetry line is the number the orb
     * actually reacted to rather than a mix of two signals. */
    const int c = level_channel(s_level_sel);
    s_tlm.amp_sum += s_amp[c];
    s_tlm.low_sum += s_low[c];
    s_tlm.mid_sum += s_mid[c];
    s_tlm.high_sum += s_high[c];
    if (s_amp[c] > s_tlm.amp_max) s_tlm.amp_max = s_amp[c];
    if (s_low[c] > s_tlm.low_max) s_tlm.low_max = s_low[c];
    if (s_mid[c] > s_tlm.mid_max) s_tlm.mid_max = s_mid[c];
    if (s_high[c] > s_tlm.high_max) s_tlm.high_max = s_high[c];
}

/*
 * Smooth the published peak into the level faces actually read.
 *
 * The time constants are in milliseconds and the step is computed from the real
 * elapsed time, so they hold whatever the frame rate turns out to be -- which
 * matters here, where it is 17 fps rather than the 30 the timer asks for.
 *
 * Losing the source is a TARGET of zero, not an assignment of zero: when the
 * taps stop being called the level releases over AMP_RELEASE_MS instead of
 * snapping, so the end of a sentence lets go rather than cutting out.
 */
/* One channel of the attack/release filter. Shared, so the bands and the
 * amplitude cannot drift apart in their timing. */
static void slew(float *cur, float target, float dt_ms)
{
    float tau = (target > *cur) ? AMP_ATTACK_MS : AMP_RELEASE_MS;
    *cur += (target - *cur) * (1.0f - expf(-dt_ms / tau));
}

/*
 * `sel` is the channel the caller wants back. BOTH are slewed regardless, so the
 * one not on screen keeps releasing towards zero and is already correct whenever
 * the behaviour switches to it -- rather than resuming from a value frozen when it
 * was last looked at.
 */
static float update_amp(int64_t now_us, bool idle, int sel)
{
    static int64_t last_us;

    float dt_ms = (last_us != 0) ? (float)(now_us - last_us) / 1000.0f : 0.0f;
    last_us = now_us;

    for (int ch = 0; ch < 2; ch++) {
        uint32_t q = __atomic_exchange_n(&s_level_peak[ch], 0, __ATOMIC_ACQ_REL);

        /* Per channel: the mic delivers nothing at all while the agent speaks, so
         * a shared idle would read as "still flowing" and hold it up there. */
        bool ch_idle = idle ||
                       ((uint32_t)(now_us / 1000) - s_feed_ms[ch]) > IDLE_MS;

        float t_amp, t_low, t_mid, t_high;
        if (q > 0) {
            t_amp = (float)(q & 0xFF) / 255.0f;
            t_low = (float)((q >> 8) & 0xFF) / 255.0f;
            t_mid = (float)((q >> 16) & 0xFF) / 255.0f;
            t_high = (float)((q >> 24) & 0xFF) / 255.0f;
        } else if (ch_idle) {
            t_amp = t_low = t_mid = t_high = 0.0f;
        } else {
            /* Nothing new, but audio is still flowing -- the source is simply
             * slower than the frame timer. Hold, do not dip. */
            t_amp = s_amp[ch];
            t_low = s_low[ch];
            t_mid = s_mid[ch];
            t_high = s_high[ch];
        }

        slew(&s_amp[ch], t_amp, dt_ms);
        slew(&s_low[ch], t_low, dt_ms);
        slew(&s_mid[ch], t_mid, dt_ms);
        slew(&s_high[ch], t_high, dt_ms);
    }

    float amp = s_amp[sel];

    /*
     * High-water marks, per source, never reset.
     *
     * Calibrating AMP_RMS_GAIN needs to know what a real voice peaks at, and a
     * spot reading every few seconds will miss it -- speech is mostly gaps. A
     * persistent peak means whoever is calibrating can just talk to the device
     * whenever and read the number afterwards, with nothing to synchronise.
     *
     * Recorded from the smoothed value rather than the raw block, because the
     * smoothed value is what a face actually reacts to.
     */
    if (!idle) {
        /* Keyed on the channel returned, not on the last tap, so a calibration
         * peak cannot be filed against the source that did not produce it. */
        if (sel == LVL_MIC) {
            if (amp > s_amp_peak_mic) {
                s_amp_peak_mic = amp;
            }
        } else if (amp > s_amp_peak_agent) {
            s_amp_peak_agent = amp;
        }
    }

    /* No logging here: the consolidated telemetry line carries all of it, from a
     * task that can afford to block on the UART. */
    tlm_accumulate_audio();
    return amp;
}

/* Static strings, so the telemetry struct can carry pointers rather than copies
 * and a caller on another task can format them safely. */
static const char *behaviour_name(ui_behaviour_t b)
{
    switch (b) {
    case UI_BEHAVIOUR_IDLE:         return "IDLE";
    case UI_BEHAVIOUR_INITIALIZING: return "INITIALIZING";
    case UI_BEHAVIOUR_LISTENING:    return "LISTENING";
    case UI_BEHAVIOUR_THINKING:     return "THINKING";
    case UI_BEHAVIOUR_SPEAKING:     return "SPEAKING";
    case UI_BEHAVIOUR_CONNECTING:   return "CONNECTING";
    case UI_BEHAVIOUR_BUFFERING:    return "BUFFERING";
    case UI_BEHAVIOUR_DISCONNECTED: return "DISCONNECTED";
    }
    return "?";
}

static const char *source_name(ui_source_t src)
{
    switch (src) {
    case UI_SRC_AGENT: return "agent";
    case UI_SRC_MIC:   return "mic";
    case UI_SRC_NONE:
    default:           return "none";
    }
}

/* Last resolved behaviour and when it was entered -- shared by the telemetry
 * line, the EVT transitions, and the minimum-dwell hold. */
static ui_behaviour_t s_behaviour_now;
static int64_t s_behaviour_since_us;

void ui_get_telemetry(ui_telemetry_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    /*
     * Read and reset. Not locked: the writer is the LVGL task and the reader is
     * main's loop, so a torn window costs at worst one frame landing in the wrong
     * bucket -- which is invisible in a one-second average and not worth taking
     * the LVGL lock on a diagnostic path.
     */
    uint32_t frames = s_tlm.frames;
    out->frames = frames;
    out->peak_mic = s_amp_peak_mic;
    out->peak_agent = s_amp_peak_agent;
    out->face = (s_face != NULL) ? s_face->name : "none";
    out->face_changed = s_face_changed;
    s_face_changed = false;
    out->behaviour = behaviour_name(s_behaviour_now);
    /* What was selected, matching amp= on the same line. */
    out->source = source_name(s_level_sel);

    if (frames > 0) {
        out->draw_avg_ms = (float)s_tlm.draw_sum / (float)frames / 1000.0f;
        out->draw_max_ms = (float)s_tlm.draw_max / 1000.0f;

        out->amp_avg = s_tlm.amp_sum / (float)frames;
        out->low_avg = s_tlm.low_sum / (float)frames;
        out->mid_avg = s_tlm.mid_sum / (float)frames;
        out->high_avg = s_tlm.high_sum / (float)frames;

        out->amp_max = s_tlm.amp_max;
        out->low_max = s_tlm.low_max;
        out->mid_max = s_tlm.mid_max;
        out->high_max = s_tlm.high_max;
    }
    if (s_tlm.periods > 0) {
        float period_us = (float)s_tlm.period_sum / (float)s_tlm.periods;
        out->fps = (period_us > 0.0f) ? (1000000.0f / period_us) : 0.0f;
    }

    /* Reset everything except last_us -- discarding that would make the next
     * window's first period read as the whole gap since boot. */
    int64_t keep = s_tlm.last_us;
    memset(&s_tlm, 0, sizeof(s_tlm));
    s_tlm.last_us = keep;
}

static void frame_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    s_frame++;

    int64_t draw_start_us = esp_timer_get_time();
    bool idle = ((uint32_t)(draw_start_us / 1000) - s_last_feed_ms) > IDLE_MS;

    /*
     * Apply a pending face switch before anything draws, so the new face owns the
     * whole frame rather than painting over half of the old one's output.
     */
    int want = s_face_want;
    if (want >= 0) {
        s_face_want = -1;
        if ((size_t)want != s_face_index) {
            select_face((size_t)want);
        }
    }

    /*
     * And the colour, for the same reason and in the same place: resolved before
     * anything draws, so a frame is never half one colour and half another.
     *
     * No clear needed, unlike a face switch -- every dot is repainted every frame,
     * so the new colour simply lands on the next one.
     */
    /*
     * The test claims the screen before any of the above matters, and before
     * anything draws -- same reason the face switch is resolved here.
     */
    if (s_test_want) {
        s_test_want = false;
        if (!s_test_active) {
            test_enter();
        }
    }

    int cwant = s_color_want;
    if (cwant >= 0) {
        s_color_want = -1;
        uint32_t rgb = orb_colors_rgb((size_t)cwant);
        if (rgb != s_tint_rgb) {
            ESP_LOGI(TAG, "EVT color %s (0x%06" PRIX32 ")",
                     orb_colors_name((size_t)cwant), rgb);
            s_tint_rgb = rgb;
        }
    }

    update_qr();

    ui_behaviour_t beh = resolve_behaviour(idle, draw_start_us);

    /*
     * Hold the connection rungs past the crossfade. Applied here rather than
     * inside resolve_behaviour so the precedence rules there stay a pure function
     * of the current state, and only the presentation is time-dependent.
     */
    int64_t min_us = 0;
    if (s_behaviour_now == UI_BEHAVIOUR_BUFFERING) {
        min_us = DWELL_BUFFERING_US;
    } else if (s_behaviour_now == UI_BEHAVIOUR_INITIALIZING) {
        min_us = DWELL_INITIALIZING_US;
    }
    if (min_us > 0 && beh != s_behaviour_now &&
        (draw_start_us - s_behaviour_since_us) < min_us) {
        beh = s_behaviour_now;
    }

    if (beh != s_behaviour_now) {
        /*
         * The dwell time of the state being LEFT is the number that matters: it
         * is what says whether THINKING and BUFFERING are ever actually visible
         * or merely momentary. Without it we have only ever inferred what was on
         * screen.
         */
        float held = (s_behaviour_since_us != 0)
                         ? (float)(draw_start_us - s_behaviour_since_us) / 1000000.0f
                         : 0.0f;
        ESP_LOGI(TAG, "EVT beh %s->%s after=%.2fs",
                 behaviour_name(s_behaviour_now), behaviour_name(beh), held);
        s_behaviour_since_us = draw_start_us;
        s_behaviour_now = beh;
    }

    /*
     * Pick the channel, then slew, THEN build the context.
     *
     * Hoisted out of the initializer on purpose: the bands used to be read in the
     * same initializer that called update_amp(), which relies on an evaluation
     * order C does not specify. It happened to work; it was not guaranteed to.
     *
     * The display test forces the MICROPHONE for every step, whatever the step
     * depicts. That is the whole point of monitor mode -- the agent is
     * disconnected there, so the playback channel is silent and a SPEAKING step
     * keyed to it would sit dead while you talk at it.
     */
    const ui_source_t src_now = s_test_active ? UI_SRC_MIC : source_for(beh);
    const int lvl = level_channel(src_now);
    s_level_sel = src_now; /* before update_amp: tlm_accumulate_audio reads it */
    const float amp_now = update_amp(draw_start_us, idle, lvl);

    const ui_render_ctx_t ctx = {
        .canvas = canvas_obj,
        .frame = s_frame,
        .now_us = draw_start_us,
        /*
         * The modifiers come from the step during the test, not from the session.
         * amp and the bands deliberately do NOT -- they arrive from the live
         * microphone, which is the whole reason monitor mode exists.
         */
        .idle = idle,
        /* What is actually driving the frame, not the last tap to fire, so this
         * and .amp can never describe different signals. */
        .source = src_now,
        .stopped = s_stopped,
        .frozen = s_test_active ? s_test_steps[s_test_step].frozen : s_failed,
        .press_active = s_press_active,
        .amp = amp_now,
        .band_low = s_low[lvl],
        .band_mid = s_mid[lvl],
        .band_high = s_high[lvl],
        .behaviour = beh,
        /* Load-bearing: this initializer is designated, so omitting the field
         * would silently pass 0 -- black ink, an invisible orb. */
        .tint_rgb = s_tint_rgb,
    };

    update_status_label(beh);

    if (s_face != NULL) {
        s_face->render(&ctx);
    }

#if CONFIG_UI_SHOW_INDICATORS
    /* Last, and inside the timing window on purpose: the ring is not free, and a
     * cost excluded from the measurement is a cost nobody finds. */
    draw_indicators();
#endif

    tlm_accumulate_frame(draw_start_us);
}

/* ---------------- touch ---------------- */

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
        /*
         * ONE target, and it is the centre. A click that started outside the
         * button is dropped here and reaches nothing.
         *
         * The ring briefly meant "interrupt" and that is what this reverts. It
         * was the whole 466 px panel minus a 70 px circle, so the gesture with
         * the largest target was the one nobody aimed at -- every bezel brush
         * cost a sentence. The interrupt did not go away with it; it moved onto
         * the button, where main.c chooses between interrupting and toggling.
         *
         * Nothing here asks whether the agent is actually speaking. That stays
         * main.c's call, which is what keeps this file free of any notion of a
         * turn -- the same reason the split lived here and the meaning did not.
         */
        if (!s_press_in_button) {
            break;
        }
        /* The one mode where a tap does not reach the session at all. */
        if (s_test_active) {
            test_advance();
            break;
        }
        if (s_gesture_handler) {
            s_gesture_handler(UI_TAP);
        }
        break;

    case LV_EVENT_LONG_PRESSED:
        if (s_press_in_button && s_gesture_handler) {
            s_gesture_handler(UI_HOLD);
        }
        break;

    default:
        break;
    }
}

/* ---------------- setup ---------------- */

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

/* Brings a face up if it is not already, then makes it current. LVGL task. */
static esp_err_t select_face(size_t idx)
{
    if (idx >= FACE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    const ui_face_t *face = s_faces[idx];

    if (!s_face_ready[idx]) {
        if (face->init != NULL) {
            esp_err_t err = face->init(canvas_obj);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "face '%s' init failed: %s", face->name,
                         esp_err_to_name(err));
                return err;
            }
        }
        s_face_ready[idx] = true;
    }

    if (face->activate != NULL) {
        face->activate();
    }
    ESP_LOGI(TAG, "EVT face %s->%s",
             (s_face != NULL) ? s_face->name : "none", face->name);
    /* So the telemetry pair either side of a switch lands adjacently in the log
     * rather than up to a window apart -- which is the whole point when the
     * question is what a face costs. */
    s_face_changed = true;
    s_face = face;
    s_face_index = idx;
    return ESP_OK;
}

/* ---------------- display test: enter, advance, leave ---------------- */

/* Find the orb's slot rather than assuming it. faces.c calls the order a
 * contract, but that contract is between faces.c and the schema; nothing says
 * this file gets to hardcode which slot the orb is in. */
static size_t orb_slot(void)
{
    for (size_t i = 0; i < FACE_COUNT; i++) {
        if (s_faces[i] == &ui_face_orb) {
            return i;
        }
    }
    return s_face_index; /* no orb built in: stay put rather than mis-index */
}

static void test_enter(void)
{
    s_test_face_restore = s_face_index;
    s_test_step = 0;
    s_test_active = true;

    size_t orb = orb_slot();
    if (orb != s_face_index) {
        select_face(orb);
    }
    ESP_LOGI(TAG, "EVT test enter steps=%u face-restore=%s",
             (unsigned)TEST_STEP_COUNT, s_faces[s_test_face_restore]->name);
    ESP_LOGI(TAG, "EVT test step=0/%u state=%s", (unsigned)TEST_STEP_COUNT,
             s_test_steps[0].label);
}

/* LVGL task, from the touch callback. Advances, or ends the test and hands the
 * session back to whoever owns it. */
static void test_advance(void)
{
    s_test_step++;
    if (s_test_step < TEST_STEP_COUNT) {
        ESP_LOGI(TAG, "EVT test step=%u/%u state=%s", (unsigned)s_test_step,
                 (unsigned)TEST_STEP_COUNT, s_test_steps[s_test_step].label);
        return;
    }

    s_test_active = false;
    if (s_test_face_restore != s_face_index) {
        select_face(s_test_face_restore);
    }
    ESP_LOGI(TAG, "EVT test done");
    if (s_gesture_handler) {
        s_gesture_handler(UI_TEST_DONE);
    }
}

static esp_err_t build_ui(void)
{
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

#if CONFIG_UI_SHOW_INDICATORS
    /*
     * Under the status word, and fixed there rather than aligned to it: the
     * status label slides down when the QR code is up, and this one is blanked in
     * that case anyway -- see draw_indicators(). Small and grey, because it
     * annotates the picture and must not become the picture.
     */
    hint_label = lv_label_create(scr);
    lv_label_set_text(hint_label, "");
    lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint_label, lv_color_hex(0x8b96a0), LV_PART_MAIN);
    lv_obj_align(hint_label, LV_ALIGN_CENTER, 0, 34);
#endif

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

    /* Not a static initialiser because it is a table lookup; orb_colors_rgb()
     * returns white for an out-of-range index, so a Kconfig list that has drifted
     * out of step with the table degrades to the default look. */
    s_tint_rgb = orb_colors_rgb(CONFIG_UI_DEFAULT_ORB_COLOR);

    /* Before the frame timer and before any tap is attached, which is what makes
     * publish_level() free of a first-use check. See s_pole_low. */
    s_pole_low = 1.0f - expf(-2.0f * (float)M_PI * BAND_LOW_HZ / BAND_SAMPLE_RATE);
    s_pole_mid = 1.0f - expf(-2.0f * (float)M_PI * BAND_MID_HZ / BAND_SAMPLE_RATE);

    esp_err_t err = select_face(CONFIG_UI_DEFAULT_FACE);
    if (err != ESP_OK) {
        return err;
    }

    lv_timer_create(frame_timer_cb, FRAME_MS, NULL);
    return ESP_OK;
}

esp_err_t ui_start(void)
{
    if (display_start() == NULL) {
        ESP_LOGE(TAG, "display init failed");
        return ESP_FAIL;
    }

    /* LVGL is not thread safe and its task is already running. */
    if (bsp_display_lock(-1) != ESP_OK) {
        return ESP_FAIL;
    }
    esp_err_t err = build_ui();
    bsp_display_unlock();

    return err;
}
