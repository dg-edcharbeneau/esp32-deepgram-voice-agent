#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "bsp/esp-bsp.h"

#if CONFIG_AEC_ENABLE
#include "esp_aec.h"
#endif

#include "audio_codecs.h"
#include "audio_io.h"

static const char *TAG = "audio_io";

/* The codecs are opened with this many channels; see the header. */
#define CODEC_CHANNELS 2
/*
 * 32, and it is not about audio quality.
 *
 * The ES7210 emits a 4x16-bit TDM frame -- 64 bits -- and the S3's I2S RX cannot
 * be put in TDM mode independently, because RX and TX share BCLK/WS in full
 * duplex and must be configured identically. Reading the same 64 bits as
 * 2 channels x 32 bits gets the whole frame through standard I2S, one 32-bit
 * word carrying two 16-bit lanes.
 *
 * es7210_set_fs() is written for exactly this: in TDM mode with channel <= 2 and
 * channel_mask == 0 it halves the requested width, so asking for 32 programs
 * 16-bit slots. Do NOT ask for 16 expecting 8-bit slots -- `bits >>= 1` gives 8,
 * es7210_set_bits() has no case 8 and writes 16-bit anyway, so the ADC would
 * clock out 64 bits against 32 from the S3 and the reference would never be
 * shifted out at all.
 */
#define CODEC_BITS     32

/*
 * Mono frames per capture read. 1280 at 16 kHz is 80 ms, the chunk size Flux
 * recommends for best model performance and latency.
 *
 * It also cuts the send rate from ~31/s to ~12.5/s. That matters more than it
 * looks: a WebSocket write that cannot complete inside its timeout returns 0,
 * which the client treats as a dead connection and reconnects -- so fewer,
 * larger writes directly reduce the pressure behind that failure. See the
 * "short send timeout" section of the README.
 */
/*
 * 1024 with the canceller, 1280 without.
 *
 * aec_get_chunksize() returns 512 for every mode on this chip -- measured, and
 * matching handle.frame_size -- so 1024 is exactly two AEC frames with no
 * remainder. 1280 would leave an alternating 256/0 residue, and a partial frame
 * fed to aec_process() is a frame of silence to the adaptive filter.
 *
 * The non-AEC path keeps 1280 so enabling the canceller is the only thing that
 * changes the send cadence to Deepgram.
 */
#if CONFIG_AEC_ENABLE
#define CAPTURE_FRAMES 1024
#else
#define CAPTURE_FRAMES 1280
#endif

/*
 * Playback ring holds MONO bytes -- the stereo doubling happens in the drain
 * task, so the buffer covers twice the wall-clock it otherwise would. 384 kB of
 * mono at 16 kHz is 12.3 s, which comfortably holds one agent turn. Deepgram
 * delivers a turn faster than it plays, so this buffer is what absorbs the
 * difference; too shallow and long replies drop mid-sentence.
 */
#define RING_BYTES  (384 * 1024)
#define CHUNK_MONO  1024

/* The drain task waits in bounded slices rather than forever so it can notice a
 * flush request itself. A stream buffer cannot be reset while a reader is
 * blocked on it, so the reader has to be the one to do the draining. */
#define DRAIN_WAIT_MS 50

/* How long after the last playback write we still consider the agent to be
 * speaking, covering audio already handed to the I2S DMA. */
#define PLAYBACK_TAIL_MS 300

static esp_codec_dev_handle_t s_spk;
static esp_codec_dev_handle_t s_mic;
static StreamBufferHandle_t s_ring;
static audio_io_capture_sink_t s_sink;
static audio_io_tap_t s_play_tap;
static audio_io_tap_t s_cap_tap;

static volatile uint32_t s_played;
static volatile uint32_t s_dropped;
static volatile uint32_t s_captured;
/*
 * Playback recency, SPLIT BY OWNER. Two tasks stamp this -- the drain task after
 * a codec write, and the WebSocket task as it queues -- and a 32-bit CPU stores a
 * 64-bit value in two halves, so a reader could land mid-write and see a time
 * that never existed. Exactly the fault 25e48d4 fixed for s_speech_us, and the
 * same remedy: one writer each, narrowed to 32-bit ms where a store is
 * indivisible. The reader takes the smaller elapsed, which is correct across the
 * 49-day wrap in a way that comparing the stamps themselves is not.
 */
static volatile uint32_t s_play_write_ms;  /* drain task only */
static volatile uint32_t s_play_queue_ms;  /* WebSocket task only */
static volatile bool s_flush_pending;

/*
 * Set when a turn has been interrupted -- see audio_io_mute_playback(). The
 * producer drops everything until the turn ends, because flushing the ring alone
 * only silences what has already arrived: Deepgram keeps sending the rest of the
 * reply, the ring refills, and the agent resumes mid-word a moment later.
 */
static volatile bool s_play_muted;

/*
 * Set by anything that BREAKS THE BYTE STREAM -- a flush, an interrupt. Consumed
 * by audio_io_play(), which owns the carry and is the only task allowed to touch
 * it: clearing the carry from the interrupting task instead would race a stitch
 * already in progress, and dropping a byte misaligns the stream just as surely as
 * keeping a stale one.
 *
 * Without this, a half sample left over from a cut-off turn is stitched onto the
 * FIRST chunk of the next one, and every sample after it is offset by a byte for
 * the rest of the session. It is not a click; it is white noise that never
 * recovers.
 */
static volatile bool s_play_gap;

/* Last block's per-lane peaks, for the telemetry line. Written by the capture
 * task, read by the main loop; 32-bit stores, so no tearing. */
static volatile uint32_t s_mic_peak;
static volatile uint32_t s_ref_peak;
static volatile uint32_t s_dead_peak;

#if CONFIG_AEC_ENABLE
/*
 * The canceller, and the two buffers it needs beside the ones already here.
 *
 * FD_LOW_COST by measurement, not by the published table: it achieved 17.3 dB of
 * ERLE on Espressif's own vectors against their 18.3 -- BOTH AT nlp_level = AGGR,
 * which is what the bench hardcodes and NOT what this build necessarily runs; see
 * CONFIG_AEC_NLP_LEVEL. It costs 16 bytes of internal
 * RAM, and leaves largest-contiguous-block untouched -- which is the number
 * a4fa137 died on. The SR modes have no non-linear stage at all
 * (aec_nlp_process() returns 0) and scored NEGATIVE. See AEC-FINDINGS.md.
 *
 * Buffers come from heap_caps_aligned_alloc(16, ...) because esp_aec.h warns
 * about it twice; plain heap_caps_malloc is what the rest of this file uses and
 * is not good enough here.
 */
/*
 * CONVERGENCE GATE.
 *
 * An adaptive filter knows nothing at boot. The greeting is the first audio it
 * ever sees, and measured over three empty-room runs it leaked enough of that
 * first burst for Deepgram to transcribe the agent's own greeting back as the
 * user -- twice in three runs, never later in the session. One turn of
 * self-conversation per boot, where before the canceller it was sixteen turns in
 * twenty-four seconds and did not stop.
 *
 * So behave like the old half-duplex gate until the filter has actually seen
 * echo, then get out of the way. Counted in blocks with real reference energy
 * rather than in wall-clock, because silence teaches an adaptive filter nothing:
 * 16 blocks at 64 ms is about a second of the agent actually speaking.
 */
#define AEC_WARMUP_BLOCKS 16
#define AEC_WARMUP_REF_PEAK 500
static uint32_t s_aec_warm;

static aec_handle_t *s_aec;
static int16_t *s_aec_ref;   /* lane 0, the echo reference */
static int16_t *s_aec_out;   /* cancelled microphone */
static int s_aec_chunk;
#endif

/*
 * LINEARITY ACCUMULATORS -- the Stage 3 measurement, and the go/no-go for
 * cancellation on this board. See AEC-FINDINGS.md.
 *
 * Both available references (this lane, and the software playback tap) sit
 * UPSTREAM of the NS4150B amplifier, so any distortion the amplifier or the
 * speaker adds is in the microphone and absent from the reference. No linear
 * filter removes that, and it sets a hard ceiling on achievable ERLE. Required
 * is 17-30 dB. Nobody has ever measured what this hardware allows.
 *
 * Accumulated only while the speaker is live, so with nobody talking the
 * microphone contains echo and little else:
 *   sum_mm  = sum(mic^2)     over the two microphone lanes, averaged
 *   sum_rr  = sum(ref^2)     over the reference lane
 *   sum_mr  = sum(mic*ref)   for the memoryless least-squares fit
 * int64 because 1280 frames x 32767^2 overflows 32 bits in a single block.
 *
 * Owned by the capture task; read and reset by it too, at the end of a turn.
 */
static int64_t s_lin_mm, s_lin_rr;
static uint32_t s_lin_blocks, s_lin_clip_mic, s_lin_clip_ref;
static bool s_lin_armed;

/*
 * THE LAG SEARCH, and the reason the first version of this measurement returned
 * nothing.
 *
 * Correlating mic against ref at zero lag measures noise: the reference is tapped
 * at the ES8311's output, and the microphone hears it only after the DAC's
 * interpolation filter, the amplifier, the air, and the ADC's decimation filter.
 * That is tens of samples. A memoryless fit at the wrong alignment reports
 * ERLE ~ 0 dB however linear the path actually is -- which is exactly what it did
 * (k swinging -0.12 to -1.04 across four runs).
 *
 * So correlate across a span of lags and take the best. 128 lags is 8 ms at
 * 16 kHz, comfortably past any codec group delay on this board, and costs
 * 1280 x 128 MACs per 80 ms block -- about 2 M MAC/s on a 240 MHz core.
 *
 * The result is still a MEMORYLESS bound: one scalar at one delay, no room
 * impulse response. A real adaptive filter does better. That is the point -- a
 * pessimistic floor that still rises as volume drops is evidence the ceiling is
 * nonlinearity rather than acoustics.
 */
#define LIN_MAX_LAG 128
/*
 * A FIXED WINDOW, because otherwise the runs are not comparable. The first sweep
 * accumulated 95, 36, 70 and 41 blocks at the four volumes -- different amounts
 * of different speech -- and any ratio computed across those is measuring the
 * content as much as the hardware. 30 blocks is 2.4 s, comfortably inside the
 * greeting at every volume.
 *
 * NOTE this whole measurement is not free: the correlation is ~2 M MAC/s on the
 * capture task, and it raised RX overruns from 4 to over 100 per run. Fine for a
 * measurement build, NOT fine to ship. It is gated on CONFIG_AEC_SWEEP_VOLUME
 * for that reason.
 */
#define LIN_MAX_BLOCKS 30

/*
 * PART C -- DOUBLE-TALK, the measurement that needs a person.
 *
 * A and B characterise the echo path with nobody talking. This one answers the
 * question that actually predicts whether voice barge-in will feel usable: when
 * someone speaks over the agent at normal distance and normal volume, how far
 * above the echo does their voice land at the microphone?
 *
 * It cannot use a detector, because a working detector is the thing that does
 * not exist. So it calibrates instead: the first DT_CAL_BLOCKS of a turn are
 * assumed to be agent-only, which fixes the amplitude ratio k = mic_echo / ref.
 * Every block after that is compared against k * ref, and the excess is the
 * person.
 *
 *   pred   = k * ref_rms           what the echo alone should measure
 *   excess = mic_rms / pred        how much the person added, in dB
 *   speech = sqrt(mic^2 - pred^2)  their voice, separated by power subtraction
 *   ser    = speech / pred         THE NUMBER -- signal-to-echo at the mic
 *
 * SAFE TO RUN WITH THE MIC GATE ON. This sits above the gate's continue, so the
 * microphone is measured but never sent to Deepgram -- you can talk over the
 * device all you like without starting the self-conversation.
 *
 * Cheap on purpose: one pass of squares per block, no correlation, so unlike the
 * Stage 3 lag search it does not steal enough CPU to cause RX overruns.
 */
#define DT_CAL_BLOCKS 8
static int64_t s_lin_corr[LIN_MAX_LAG];
static int16_t s_lin_hist[LIN_MAX_LAG];  /* past reference samples, ring */
static size_t s_lin_hist_pos;

#if CONFIG_AEC_DOUBLETALK_LOG
static int64_t s_dt_cal_mm, s_dt_cal_rr;
static uint32_t s_dt_blocks;
static double s_dt_k;
static double s_dt_best_ser;
static bool s_dt_have_ser;
static bool s_dt_armed;
#endif

/* Milliseconds since boot, truncated. One 32-bit store, so no writer can tear it
 * and no reader can catch it half-updated. */
static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}
static int s_volume;
static bool s_volume_from_nvs;

/*
 * Volume limits.
 *
 * The floor is 20, not 0, and that is deliberate: esp_codec_dev special-cases
 * volume 0 to -96 dB, which is silence rather than quiet -- and an agent that
 * has muted itself cannot be asked to unmute. 20 is about -40 dB, already
 * barely audible. Muting, if it is ever wanted, needs a way back that is not
 * the voice.
 *
 * The ceiling is the top of the volume curve. Going above it means
 * re-installing the curve, which reallocates inside esp_codec_dev and has no
 * business happening at runtime -- CONFIG_AUDIO_OUT_EXTRA_GAIN_DB is the
 * build-time knob for that, and it lifts the whole scale so this range
 * inherits it.
 */
#define VOLUME_MIN 20
#define VOLUME_MAX 100

/* Shared with the saved voice; see voices.c. */
#define NVS_NAMESPACE  "dgagent"
#define NVS_KEY_VOLUME "out_volume"
static volatile bool s_capture_enabled = true;
/* Tap-only override, consulted only while capture is disabled. See
 * audio_io_capture_set_monitor() in the header for why it exists. */
static volatile bool s_monitor;
static int s_rate;

/*
 * Sample-alignment carries -- the fix for intermittent loud static.
 *
 * PCM here is 16-bit, but nothing upstream respects sample boundaries: the
 * WebSocket transport hands us whatever a TLS record happened to contain, and a
 * stream buffer splits at any byte. Truncating an odd byte count to whole
 * samples throws one byte away, which shifts every following sample by 8 bits.
 * That is not a click -- it is permanent, full-scale noise that lasts until the
 * stream happens to realign.
 *
 * So both ends keep the orphaned byte and stitch it onto the next chunk, and
 * drops are always whole samples.
 */
static uint8_t s_in_carry;
static bool s_in_carry_valid;

/* ---------------- playback ---------------- */

static void playback_task(void *arg)
{
    /* Mono in, stereo out: the codec is open with two channels, so every
     * sample has to be written twice. */
    int16_t *mono = heap_caps_malloc(CHUNK_MONO, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    /*
     * Mono in, stereo out, and the codec is open at 32 bits, so each sample
     * leaves as two 32-bit words.
     *
     * PSRAM, not internal. esp_codec_dev_write() copies out of here into the I2S
     * DMA descriptors, so this buffer never needs to be DMA-capable itself -- and
     * internal RAM is the scarce resource. Measured: with the canceller running
     * and the mic gate off, free internal fell to 4,071 B with a largest block of
     * 1,536 and a 1,630 B AES DMA request then failed, taking the TLS session
     * with it. This is 4 kB of that back.
     */
    int32_t *stereo = heap_caps_aligned_alloc(16,
                                              (CHUNK_MONO / sizeof(int16_t)) * 2 * sizeof(int32_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mono == NULL || stereo == NULL) {
        ESP_LOGE(TAG, "no internal RAM for playback buffers");
        vTaskDelete(NULL);
    }

    uint8_t *mono_bytes = (uint8_t *)mono;
    uint8_t carry = 0;
    bool carry_valid = false;

    while (1) {
        /* Lead with the half sample left over from last time, if any. */
        size_t off = 0;
        if (carry_valid) {
            mono_bytes[0] = carry;
            off = 1;
        }

        size_t got = xStreamBufferReceive(s_ring, mono_bytes + off, CHUNK_MONO - off,
                                         pdMS_TO_TICKS(DRAIN_WAIT_MS));
        size_t total = off + got;

        if (s_flush_pending) {
            /* Barge-in. Throw away what we just read plus anything still
             * queued, and drop the carry -- keeping it would misalign the
             * first sample of the next reply. */
            uint8_t scratch[128];
            while (xStreamBufferReceive(s_ring, scratch, sizeof(scratch), 0) > 0) {
            }
            carry_valid = false;
            s_flush_pending = false;
            continue;
        }

        if (total < sizeof(int16_t)) {
            carry_valid = (total == 1);
            if (carry_valid) {
                carry = mono_bytes[0];
            }
            continue;
        }

        size_t samples = total / sizeof(int16_t);
        /* Stash the orphaned byte before it gets overwritten. */
        if (total & 1) {
            carry = mono_bytes[total - 1];
            carry_valid = true;
        } else {
            carry_valid = false;
        }

        /*
         * Before the write, because the write is what blocks. Note this still
         * runs ahead of what you hear: the I2S DMA holds 6 x 240 frames, ~90 ms
         * at 16 kHz, so a visualizer fed from here leads the speaker by about
         * three frames at steady state.
         */
        if (s_play_tap != NULL) {
            s_play_tap(mono, samples);
        }

        /*
         * Left-justified into the 32-bit slot: the ES8311 takes the top 16 bits,
         * so << 16 is the identity transform on the audio and only the container
         * changed. Shifting the other way would attenuate by 96 dB.
         */
        for (size_t i = 0; i < samples; i++) {
            int32_t v = (int32_t)mono[i] << 16;
            stereo[2 * i] = v;
            stereo[2 * i + 1] = v;
        }

        int err = esp_codec_dev_write(s_spk, stereo, (int)(samples * 2 * sizeof(int32_t)));
        if (err != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "esp_codec_dev_write failed: %d", err);
        }
        s_play_write_ms = now_ms();
    }
}

/* ---------------- capture ---------------- */

/*
 * One line per agent turn, unattended. Reports what a MEMORYLESS fit can cancel,
 * which is a pessimistic bound on ERLE -- it has no notion of the room's impulse
 * response, so real echo delay inflates the residual. That is deliberate: a
 * RISING trend as volume drops is evidence the ceiling is nonlinearity rather
 * than acoustics, which is exactly the question.
 *
 *   erl   = 10*log10(sum_mm / sum_rr)      how much louder the mic is than the ref
 *   erle  = 10*log10(sum_mm / residual)    what one scalar can remove
 *   k                                       the best scalar itself
 */
#if CONFIG_AEC_DOUBLETALK_LOG
static void dt_end(void)
{
    if (s_dt_armed && s_dt_blocks > DT_CAL_BLOCKS) {
        if (s_dt_have_ser) {
            ESP_LOGI(TAG, "DT turn done: blocks=%" PRIu32 " k=%.4f BEST ser=%+.1f dB",
                     s_dt_blocks, s_dt_k, s_dt_best_ser);
        } else {
            /* Nothing rose above the echo. Either nobody spoke, or they did and
             * the echo buried them -- which is itself the answer. */
            ESP_LOGI(TAG, "DT turn done: blocks=%" PRIu32 " k=%.4f no speech above echo",
                     s_dt_blocks, s_dt_k);
        }
    }
    s_dt_cal_mm = s_dt_cal_rr = 0;
    s_dt_blocks = 0;
    s_dt_k = 0.0;
    s_dt_best_ser = -999.0;
    s_dt_have_ser = false;
    s_dt_armed = false;
}
#endif

static void lin_reset(void)
{
    s_lin_mm = s_lin_rr = 0;
    s_lin_blocks = s_lin_clip_mic = s_lin_clip_ref = 0;
    s_lin_armed = false;
    for (size_t i = 0; i < LIN_MAX_LAG; i++) {
        s_lin_corr[i] = 0;
        s_lin_hist[i] = 0;
    }
    s_lin_hist_pos = 0;
}

static void lin_report(void)
{
    if (s_lin_blocks == 0 || s_lin_rr == 0 || s_lin_mm == 0) {
        lin_reset();
        return;
    }
    double mm = (double)s_lin_mm, rr = (double)s_lin_rr;

    /* Best single delay, by correlation magnitude. */
    size_t best = 0;
    double best_c = 0.0;
    for (size_t L = 0; L < LIN_MAX_LAG; L++) {
        double c = (double)s_lin_corr[L];
        if (c < 0) c = -c;
        if (c > best_c) { best_c = c; best = L; }
    }

    double k = (double)s_lin_corr[best] / rr;
    double resid = mm - k * k * rr;   /* == sum((mic - k*ref_delayed)^2) at the optimum */
    if (resid < 1.0) {
        resid = 1.0;
    }
    ESP_LOGI(TAG, "LIN vol=%d refgain=%d blocks=%" PRIu32
             " erl=%.1f erle=%.1f lag=%u k=%.4f clip_mic=%" PRIu32 " clip_ref=%" PRIu32,
             s_volume, CONFIG_AEC_REF_GAIN_DB, s_lin_blocks,
             10.0 * log10(mm / rr), 10.0 * log10(mm / resid),
             (unsigned)best, k, s_lin_clip_mic, s_lin_clip_ref);
    lin_reset();
}

static void capture_task(void *arg)
{
    /* Four 16-bit TDM slots per frame -- 8 bytes -- read as two 32-bit I2S words. */
    const size_t stereo_bytes = CAPTURE_FRAMES * AEC_LANES * sizeof(int16_t);
    const size_t mono_bytes = CAPTURE_FRAMES * sizeof(int16_t);

    /*
     * `stereo` is PSRAM and `mono` is internal, and the split is deliberate.
     *
     * stereo is only the destination esp_codec_dev_read() copies the DMA
     * descriptors into; nothing hands it to the canceller, so it need not be
     * internal and at 8 kB it is the largest single block this file was holding.
     * mono IS handed to aec_process(), which esp_aec.h says must be 16-byte
     * aligned, and it stays internal because the filter touches it twice a block.
     *
     * The 12 kB the two stereo buffers return is aimed at a measured failure: free
     * internal reaching 4,071 B with a largest block of 1,536, at which point a
     * 1,630 B AES DMA allocation failed and the TLS session dropped.
     */
    int16_t *stereo = heap_caps_aligned_alloc(16, stereo_bytes,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *mono = heap_caps_aligned_alloc(16, mono_bytes,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (stereo == NULL || mono == NULL) {
        ESP_LOGE(TAG, "no internal RAM for capture buffers");
        vTaskDelete(NULL);
    }

    int64_t next_level_log = 0;

    while (1) {
        int err = esp_codec_dev_read(s_mic, stereo, (int)stereo_bytes);
        if (err != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "esp_codec_dev_read failed: %d", err);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

#if CONFIG_AEC_ENABLE
        /*
         * ONE runtime test, used by every AEC site below.
         *
         * s_aec_ref was previously written inside the downmix loop under the
         * compile-time #if alone, while the null check sat sixteen lines further
         * down. Both of audio_io_init()'s graceful-degradation paths leave these
         * pointers NULL -- so a failed aec_create_from_config(), the one case the
         * comment there promises is survivable, stored to address 0 on the first
         * capture block and panicked. Deterministic, so it would have been a boot
         * loop, on the path meant to avoid one.
         */
        const bool aec_on = (s_aec != NULL && s_aec_ref != NULL && s_aec_out != NULL);
#endif

        int16_t peak[AEC_LANES] = { 0 };
        for (size_t i = 0; i < CAPTURE_FRAMES; i++) {
            const int16_t *f = &stereo[AEC_LANES * i];
            for (int c = 0; c < AEC_LANES; c++) {
                int16_t a = (f[c] < 0) ? (int16_t)-f[c] : f[c];
                if (a > peak[c]) {
                    peak[c] = a;
                }
            }
            /*
             * THE TWO MICROPHONE LANES, and only those. The probe in 9479446
             * averaged lanes 0 and 1 because it did not yet know the order; lane
             * 0 is the echo REFERENCE, so shipping that line would mix the
             * speaker's own signal into what goes to Deepgram -- guaranteeing the
             * self-conversation this whole exercise exists to stop.
             */
            mono[i] = (int16_t)(((int32_t)f[AEC_LANE_MIC_A] +
                                 (int32_t)f[AEC_LANE_MIC_B]) / 2);
#if CONFIG_AEC_ENABLE
            if (aec_on) {
                s_aec_ref[i] = f[AEC_LANE_REF];
            }
#endif
        }

#if CONFIG_AEC_ENABLE
        /*
         * CANCEL BEFORE ANY GATE, TAP OR SINK. Everything downstream -- the level
         * the orb draws, what Deepgram hears, the double-talk instrumentation --
         * should see the cleaned signal, not the raw microphone.
         *
         * Two chunks of exactly s_aec_chunk; CAPTURE_FRAMES is sized so there is
         * no remainder. The filter is adaptive and stateful, so every block must
         * go through in order even when the session is stopped -- skipping frames
         * would make it diverge and it would then have to reconverge mid-reply,
         * which is the worst possible moment.
         */
        if (aec_on) {
            for (size_t off = 0; off + (size_t)s_aec_chunk <= CAPTURE_FRAMES;
                 off += (size_t)s_aec_chunk) {
                aec_process(s_aec, mono + off, s_aec_ref + off, s_aec_out + off);
            }
            memcpy(mono, s_aec_out, CAPTURE_FRAMES * sizeof(int16_t));

            if (s_aec_warm < AEC_WARMUP_BLOCKS &&
                peak[AEC_LANE_REF] > AEC_WARMUP_REF_PEAK) {
                s_aec_warm++;
                if (s_aec_warm == AEC_WARMUP_BLOCKS) {
                    ESP_LOGI(TAG, "AEC converged (%d blocks of reference audio); "
                             "microphone open during playback from here",
                             AEC_WARMUP_BLOCKS);
                }
            }
            /* Half-duplex until then -- the filter still gets every frame above,
             * it is only the OUTPUT that is withheld while it is still learning. */
            if (s_aec_warm < AEC_WARMUP_BLOCKS && audio_io_playback_active()) {
                continue;
            }
        }
#endif
#if CONFIG_AEC_SWEEP_VOLUME > 0
        if (audio_io_playback_active() && s_lin_blocks < LIN_MAX_BLOCKS) {
            for (size_t i = 0; i < CAPTURE_FRAMES; i++) {
                const int16_t *f = &stereo[AEC_LANES * i];
                int32_t m = ((int32_t)f[AEC_LANE_MIC_A] + (int32_t)f[AEC_LANE_MIC_B]) / 2;
                int16_t r = f[AEC_LANE_REF];

                /* Newest reference sample into the ring first, so lag 0 is the
                 * sample captured in this very frame. */
                s_lin_hist[s_lin_hist_pos] = r;

                s_lin_mm += (int64_t)m * m;
                s_lin_rr += (int64_t)r * r;
                for (size_t L = 0; L < LIN_MAX_LAG; L++) {
                    size_t idx = (s_lin_hist_pos + LIN_MAX_LAG - L) % LIN_MAX_LAG;
                    s_lin_corr[L] += (int64_t)m * s_lin_hist[idx];
                }
                s_lin_hist_pos = (s_lin_hist_pos + 1) % LIN_MAX_LAG;

                if (m >= 32000 || m <= -32000) s_lin_clip_mic++;
                if (r >= 32000 || r <= -32000) s_lin_clip_ref++;
            }
            s_lin_blocks++;
            s_lin_armed = true;
            if (s_lin_blocks >= LIN_MAX_BLOCKS) {
                lin_report();   /* fixed window reached; report and stand down */
            }
        } else if (s_lin_armed && !audio_io_playback_active()) {
            /* Turn ended before the window filled -- report what there is, and
             * the blocks= field says it is short. */
            lin_report();
        }
#endif

#if CONFIG_AEC_DOUBLETALK_LOG
        if (audio_io_playback_active()) {
            int64_t mm = 0, rr = 0;
            for (size_t i = 0; i < CAPTURE_FRAMES; i++) {
                const int16_t *f = &stereo[AEC_LANES * i];
                int32_t m = ((int32_t)f[AEC_LANE_MIC_A] + (int32_t)f[AEC_LANE_MIC_B]) / 2;
                int32_t r = f[AEC_LANE_REF];
                mm += (int64_t)m * m;
                rr += (int64_t)r * r;
            }
            double mic_rms = sqrt((double)mm / CAPTURE_FRAMES);
            double ref_rms = sqrt((double)rr / CAPTURE_FRAMES);
            s_dt_armed = true;
            s_dt_blocks++;

            if (s_dt_blocks <= DT_CAL_BLOCKS) {
                /* Assumed agent-only -- wait a beat before speaking. */
                s_dt_cal_mm += mm;
                s_dt_cal_rr += rr;
                if (s_dt_blocks == DT_CAL_BLOCKS && s_dt_cal_rr > 0) {
                    s_dt_k = sqrt((double)s_dt_cal_mm / (double)s_dt_cal_rr);
                    ESP_LOGI(TAG, "DT calibrated: k=%.4f (mic_echo/ref) -- SPEAK NOW",
                             s_dt_k);
                }
            } else if (s_dt_k > 0.0 && ref_rms > 1.0) {
                double pred = s_dt_k * ref_rms;
                double excess_db = 20.0 * log10((mic_rms > 1.0 ? mic_rms : 1.0) / pred);
                double sp2 = mic_rms * mic_rms - pred * pred;
                if (sp2 > 0.0) {
                    double ser = 10.0 * log10(sp2 / (pred * pred));
                    if (!s_dt_have_ser || ser > s_dt_best_ser) {
                        s_dt_best_ser = ser;
                        s_dt_have_ser = true;
                    }
                    ESP_LOGI(TAG, "DT blk=%" PRIu32 " mic=%.0f ref=%.0f pred=%.0f "
                             "excess=%+.1f ser=%+.1f dB",
                             s_dt_blocks, mic_rms, ref_rms, pred, excess_db, ser);
                } else {
                    ESP_LOGI(TAG, "DT blk=%" PRIu32 " mic=%.0f ref=%.0f pred=%.0f "
                             "excess=%+.1f ser=-- (echo only)",
                             s_dt_blocks, mic_rms, ref_rms, pred, excess_db);
                }
            }
        } else if (s_dt_armed) {
            dt_end();
        }
#endif

        const int16_t peak_l = peak[AEC_LANE_MIC_A];
        const int16_t peak_r = peak[AEC_LANE_MIC_B];
        s_ref_peak = (uint32_t)peak[AEC_LANE_REF];
        s_dead_peak = (uint32_t)peak[AEC_LANE_DEAD];
        if (peak_l > peak_r) {
            s_mic_peak = (uint32_t)peak_l;
        } else {
            s_mic_peak = (uint32_t)peak_r;
        }

#if CONFIG_MIC_LEVEL_LOG
        /*
         * Per-channel peaks, not a combined level. If the board only wires one
         * of the ES7210's inputs, the downmix above halves every sample and a
         * combined meter would just look "quiet" -- this shows which channel is
         * actually live.
         */
        int64_t now = esp_timer_get_time();
        if (now >= next_level_log) {
            next_level_log = now + 3000000;
            /* All four lanes. The dead one is the control: if it ever moves,
             * the slot order has shifted and every other reading here is
             * meaningless. */
            ESP_LOGI(TAG, "mic peak L=%d R=%d ref=%d dead=%d%s",
                     peak_l, peak_r, peak[AEC_LANE_REF], peak[AEC_LANE_DEAD],
                     audio_io_playback_active() ? " (agent speaking)" : "");
        }
#endif

#if CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS
        /*
         * Half duplex. The speaker and mic sit centimetres apart with no echo
         * cancellation in this project, so anything the agent says is captured
         * and sent straight back, and the agent starts answering itself.
         * Dropping capture while it speaks is the crude fix; it also disables
         * barge-in, which is the trade.
         *
         * The real answer is NOT server-side, whatever an earlier version of this
         * comment claimed. Deepgram's "Audio Preprocessing & Barge-In" guide has
         * no AEC setting and explicitly pushes cancellation to the device.
         *
         * And the device could, in principle: this board wires an echo reference
         * from the ES8311's output into ES7210 MIC3, sample-aligned because the
         * same ADC captures it in the same frame. That was proven in commit
         * 9479446 and measured -- see the echo section of the README. What does
         * not fit is the canceller: esp-sr's AFE wants ~70 kB of internal RAM
         * against the ~78 kB free once the display is up, so enabling it stopped
         * the session completing a TLS handshake at all (a4fa137). Reaching
         * barge-in needs a much smaller algorithm, not this gate removed.
         *
         * That algorithm now has a name and a price. The 70 kB was the AFE, not
         * the canceller; esp-sr's standalone AEC is published at 8.2-26.9 kB of
         * internal RAM. Unmeasured here -- see AEC-FINDINGS.md before trying it.
         */
        if (audio_io_playback_active()) {
            continue;
        }
#endif

        /*
         * Session gate. Ahead of the tap as well as the sink, so a stopped
         * device neither streams nor visualizes the room -- a ring still
         * dancing to background noise reads as broken, not as stopped.
         */
#if !CONFIG_AUDIO_CAPTURE_ALWAYS
        if (!s_capture_enabled) {
            if (!s_monitor) {
                continue;
            }
            /*
             * Monitor mode: the display gets levels, the network gets nothing.
             * The continue is the safety property rather than a shortcut -- it is
             * what makes writing to a socket that is not there impossible.
             */
            if (s_cap_tap != NULL) {
                s_cap_tap(mono, CAPTURE_FRAMES);
            }
            continue;
        }
#else
        /* Bench build: the gate is removed so the microphone can be calibrated
         * with the session stopped and nothing able to talk back. The sink is
         * still gated below, so nothing is streamed upstream. */
        if (!s_capture_enabled && s_sink != NULL) {
            /* Feed the tap only; fall through with the sink suppressed. */
            if (s_cap_tap != NULL) {
                s_cap_tap(mono, CAPTURE_FRAMES);
            }
            continue;
        }
#endif

        /* After the gates above, so the tap sees what actually goes upstream. */
        if (s_cap_tap != NULL) {
            s_cap_tap(mono, CAPTURE_FRAMES);
        }

        if (s_sink != NULL) {
            s_sink((const uint8_t *)mono, mono_bytes);
            s_captured += mono_bytes;
        }
    }
}

/* ---------------- setup ---------------- */

/* ---------------- volume ---------------- */

static int volume_clamp(int level)
{
    if (level < VOLUME_MIN) return VOLUME_MIN;
    if (level > VOLUME_MAX) return VOLUME_MAX;
    return level;
}

/*
 * The saved level, or the Kconfig factory default when there is none.
 *
 * Two distinct misses to tolerate, because the namespace is shared with the
 * saved voice: nvs_open fails while nothing at all has been written, and
 * nvs_get_u8 returns NOT_FOUND once the voice exists but the volume does not.
 * Both are ordinary first-run states, not errors.
 */
static int volume_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return volume_clamp(CONFIG_AUDIO_OUT_VOLUME);
    }

    uint8_t saved = 0;
    esp_err_t err = nvs_get_u8(h, NVS_KEY_VOLUME, &saved);
    nvs_close(h);

    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "reading saved volume failed: %s", esp_err_to_name(err));
        }
        return volume_clamp(CONFIG_AUDIO_OUT_VOLUME);
    }
    s_volume_from_nvs = true;
    return volume_clamp((int)saved);
}

static void volume_save(int level)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_VOLUME, (uint8_t)level);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        /* Same policy as the saved voice: the session keeps the new level
         * either way, only the next boot loses it. */
        ESP_LOGW(TAG, "could not persist volume: %s", esp_err_to_name(err));
    }
}

int audio_io_get_volume(void)
{
    return s_volume;
}

int audio_io_adjust_volume(int delta)
{
    int level = volume_clamp(s_volume + delta);
    if (level == s_volume) {
        return s_volume;   /* already at the stop; no register write, no flash */
    }

    int err = esp_codec_dev_set_out_vol(s_spk, level);
    if (err != ESP_CODEC_DEV_OK) {
        /* Nothing was stored on the codec side either, so do not persist. */
        ESP_LOGW(TAG, "volume change rejected: %d", err);
        return s_volume;
    }

    s_volume = level;
    volume_save(level);
    ESP_LOGI(TAG, "volume %d", level);
    return level;
}

esp_err_t audio_io_init(int sample_rate)
{
    /*
     * Let the BSP do the I2S bring-up. Its speaker/microphone init calls
     * bsp_i2c_init() + bsp_audio_init() itself, but only behind an
     * `if (i2s_data_if == NULL)` guard -- so calling bsp_audio_init() ourselves
     * first makes that guard false and I2C never comes up, leaving the codec
     * control interface built on a NULL bus handle:
     *
     *   assert failed: bsp_audio_codec_speaker_init ...:225 (i2c_ctrl_if)
     *
     * The sample rate does not need a custom i2s_std_config anyway --
     * esp_codec_dev_open() below sets the clock, overriding the BSP's 22050 Hz
     * default. This is the order spec_analyzer_radial's bsp_extra uses.
     */
    /*
     * Built here rather than by the BSP, because the BSP hardcodes
     * mic_selected to 0 and the driver then falls back to MIC1|MIC2 -- so MIC3,
     * the echo reference, is never powered and never clocked out. See
     * audio_codecs.h. This path does the same bsp_i2c_init() first, so the
     * init-order trap the comment above describes is still avoided.
     */
    esp_err_t codec_err = audio_codecs_init_tdm(&s_spk, &s_mic);
    if (codec_err != ESP_OK) {
        ESP_LOGE(TAG, "TDM codec init failed: %s", esp_err_to_name(codec_err));
        return ESP_FAIL;
    }

    s_rate = sample_rate;

    /* One sample_info for both: they share the I2S clock. */
    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = sample_rate,
        .channel         = CODEC_CHANNELS,
        .bits_per_sample = CODEC_BITS,
    };

    /*
     * Replace the stock volume curve, always -- not just when adding gain.
     *
     * esp_codec_dev maps 0-100 onto -50..0 dB, and on this speaker everything
     * below roughly -15 dB is inaudible, so sixty of the hundred steps did
     * nothing at all: 20-60 were silent, and the whole usable range was
     * squeezed into 70-100. Measured, not assumed.
     *
     * The replacement spans AUDIO_OUT_RANGE_DB below the top instead, so the
     * full travel lands inside the audible part. The range is expressed
     * relative to the top rather than as an absolute floor, so raising
     * AUDIO_OUT_EXTRA_GAIN_DB shifts the whole scale up and keeps the same
     * amount of travel.
     *
     * Note volume 0 is special-cased to -96 dB inside esp_codec_dev regardless
     * of this curve, which is one reason the runtime floor is VOLUME_MIN.
     */
    esp_codec_dev_vol_map_t vol_map[2] = {
        { .vol = 0,   .db_value = (float)(CONFIG_AUDIO_OUT_EXTRA_GAIN_DB -
                                          CONFIG_AUDIO_OUT_RANGE_DB) },
        { .vol = 100, .db_value = (float)CONFIG_AUDIO_OUT_EXTRA_GAIN_DB },
    };
    esp_codec_dev_vol_curve_t curve = { .vol_map = vol_map, .count = 2 };
    int vol_err = esp_codec_dev_set_vol_curve(s_spk, &curve);
    if (vol_err != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "volume curve rejected: %d", vol_err);
    }

    /*
     * Both of these must follow the codec _init() calls above, not precede
     * them: esp_codec_dev_set_out_vol() checks that the codec is open and
     * returns without storing anything if it is not. es8311_codec_new() opens
     * it, so by here it is.
     */
    /* Resolved here rather than in a separate init step, because the codec has
     * to exist before a level can be applied at all. */
    s_volume = volume_load();
#if CONFIG_AEC_SWEEP_VOLUME > 0
    /* The saved volume normally wins over Kconfig, which makes a volume sweep
     * impossible without erasing NVS. This forces it for the run and does NOT
     * persist -- the saved value is untouched. */
    s_volume = CONFIG_AEC_SWEEP_VOLUME;
    ESP_LOGW(TAG, "AEC sweep: volume forced to %d (saved value not changed)", s_volume);
#endif
    esp_codec_dev_set_out_vol(s_spk, s_volume);
    esp_codec_dev_set_in_gain(s_mic, (float)CONFIG_MIC_IN_GAIN);

    int err = esp_codec_dev_open(s_spk, &fs);
    if (err != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "speaker open failed: %d", err);
        return ESP_FAIL;
    }
    err = esp_codec_dev_open(s_mic, &fs);
    if (err != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "microphone open failed: %d", err);
        return ESP_FAIL;
    }

    /*
     * AFTER THE OPEN, AND THAT IS THE WHOLE POINT.
     *
     * esp_codec_dev_set_in_gain() above is device-wide: es7210_set_gain() fans
     * out to _es7210_set_channel_gain(codec, 0xF, db), all four inputs, so the
     * echo reference gets CONFIG_MIC_IN_GAIN too. It is a line-level tap of the
     * ES8311's own output and wants no gain at all -- at 24 dB it has under 8 dB
     * of headroom left, and a clipped reference is a nonlinearity in the one
     * signal a canceller needs clean. But 0 dB is the opposite mistake -- 181
     * peak, -45 dBFS -- so the value is CONFIG_AEC_REF_GAIN_DB and the linearity
     * sweep is what should set it.
     *
     * Setting it BEFORE the open silently does nothing. esp_codec_dev_open()
     * ends in _update_codec_setting(), which replays the stored device-wide
     * mic_gain over every channel -- so the per-channel value is overwritten
     * before a single frame is read, and both calls still return OK because
     * es7210's vtable never assigns .is_open so nothing rejects them. Measured:
     * the reference lane read 13,541 peak when it should read about 850.
     *
     * The mask indexes the ES7210's INPUTS, where the reference is MIC3 (bit 2).
     * That is NOT its position in the captured frame, which is lane 0.
     */
    int ref_gain_err = esp_codec_dev_set_in_channel_gain(s_mic, AEC_REF_INPUT_MASK,
                                                        (float)CONFIG_AEC_REF_GAIN_DB);
    if (ref_gain_err != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "could not set reference lane gain: %d", ref_gain_err);
    }
#if CONFIG_AEC_ENABLE
    {
        aec_config_t acfg = {
            .mic_num       = 1,
            .ref_num       = 1,
            .out_num       = 1,
            .filter_length = 4,
            .sample_rate   = 16000,
            .caps          = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
            .mode          = AEC_MODE_FD_LOW_COST,
            .nlp_level     = CONFIG_AEC_NLP_LEVEL,
        };
        s_aec = aec_create_from_config(&acfg);
        if (s_aec == NULL) {
            /* Not fatal: a device that still talks is better than one that does
             * not boot. The log says the microphone is uncancelled. */
            ESP_LOGE(TAG, "AEC create failed -- running WITHOUT cancellation");
        } else {
            s_aec_chunk = aec_get_chunksize(s_aec);
            s_aec_ref = heap_caps_aligned_alloc(16, CAPTURE_FRAMES * sizeof(int16_t),
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            s_aec_out = heap_caps_aligned_alloc(16, CAPTURE_FRAMES * sizeof(int16_t),
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (s_aec_ref == NULL || s_aec_out == NULL ||
                s_aec_chunk <= 0 || (CAPTURE_FRAMES % s_aec_chunk) != 0) {
                ESP_LOGE(TAG, "AEC unusable (chunk=%d, frames=%d) -- disabling",
                         s_aec_chunk, CAPTURE_FRAMES);
                aec_destroy(s_aec);
                s_aec = NULL;
                free(s_aec_ref); free(s_aec_out);
                s_aec_ref = NULL; s_aec_out = NULL;
            } else {
                ESP_LOGI(TAG, "AEC: FD_LOW_COST, chunk=%d, %d per capture block, "
                         "nlp=%d", s_aec_chunk, CAPTURE_FRAMES / s_aec_chunk,
                         (int)CONFIG_AEC_NLP_LEVEL);
            }
        }
    }
#endif

    /* Both stay open for the session; reopening per turn clicks. */

    s_ring = xStreamBufferCreateWithCaps(RING_BYTES, 1, MALLOC_CAP_SPIRAM);
    if (s_ring == NULL) {
        ESP_LOGE(TAG, "no PSRAM for the %d byte playback buffer", RING_BYTES);
        return ESP_ERR_NO_MEM;
    }

    /* Core 1, to keep codec work off the core running the Wi-Fi stack. */
    if (xTaskCreatePinnedToCore(playback_task, "audio_play", 4096, NULL, 6, NULL, 1) != pdPASS) {
        return ESP_FAIL;
    }

    /* The dB figure is what we asked the curve for; the ES8311 then adds ~3.6 dB
     * of hw_gain compensation on top, so what you hear is a little above it. */
    int floor_db = CONFIG_AUDIO_OUT_EXTRA_GAIN_DB - CONFIG_AUDIO_OUT_RANGE_DB;
    int asked_db = floor_db + s_volume * CONFIG_AUDIO_OUT_RANGE_DB / 100;
    ESP_LOGI(TAG, "codecs open: %d Hz, %d-bit, %d ch | volume %d%s = %d dB "
                  "(range %d dB, gain +%d dB), mic gain %d dB",
             sample_rate, CODEC_BITS, CODEC_CHANNELS, s_volume,
             s_volume_from_nvs ? " (saved)" : " (default)", asked_db,
             CONFIG_AUDIO_OUT_RANGE_DB, CONFIG_AUDIO_OUT_EXTRA_GAIN_DB, CONFIG_MIC_IN_GAIN);
    return ESP_OK;
}

esp_err_t audio_io_capture_start(audio_io_capture_sink_t sink)
{
    if (s_mic == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_sink = sink;

    /* Priority above playback: a missed read is lost audio, a late write is
     * only a small gap the ring buffer absorbs. */
    /*
     * STAYS ON CORE 1, INCLUDING WITH THE CANCELLER -- and that is a measurement,
     * not an oversight.
     *
     * Enabling the AEC costs the display 25.0 -> 20.4 fps, draw 18 -> 23 ms.
     * a4fa137's lesson says to move DSP off the display's core, so core 0 was
     * tried: 20.0-22.6 fps, i.e. no recovery. Core placement is not the cause.
     *
     * What is left is the PSRAM bus. The canceller's ~123 kB working set is read
     * and written every frame, and the render buffer and orb geometry live in
     * PSRAM too; that contention is shared no matter which core runs which task.
     * UNVERIFIED as a mechanism -- what is measured is that moving cores does not
     * help, so moving cores was not kept. Core 0 also carries Wi-Fi and TLS,
     * which is a real risk for the handshake, and there is no reason to take it.
     */
    if (xTaskCreatePinnedToCore(capture_task, "audio_cap", 4096, NULL, 7, NULL, 1) != pdPASS) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "capture started: %d frame chunks (%d ms)",
             CAPTURE_FRAMES, CAPTURE_FRAMES * 1000 / s_rate);
    return ESP_OK;
}

esp_err_t audio_io_play(const uint8_t *pcm, size_t len)
{
    if (s_ring == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Ahead of the mute check, not inside it: the gap has to be honoured even
     * when NOTHING arrives while muted, which is precisely the case that bites.
     * Tap late in a turn, get no further chunks to clear the carry on, and the
     * stale byte lands on the next turn instead.
     */
    if (s_play_gap) {
        s_play_gap = false;
        s_in_carry_valid = false;
    }

    if (s_play_muted) {
        /*
         * Interrupted turn: drop the rest of it. Deliberately does NOT stamp the
         * queue clock -- the whole point is that playback goes quiet, so
         * audio_io_playback_active() must be allowed to fall and reopen the mic.
         */
        return ESP_OK;
    }

    s_play_queue_ms = now_ms();

    /* Stitch the previous frame's orphaned byte onto this one. */
    uint8_t head[2];
    size_t head_len = 0;
    if (s_in_carry_valid && len > 0) {
        head[0] = s_in_carry;
        head[1] = pcm[0];
        head_len = sizeof(head);
        pcm++;
        len--;
        s_in_carry_valid = false;
    }

    size_t body = len & ~(size_t)1;
    if (len & 1) {
        s_in_carry = pcm[body];
        s_in_carry_valid = true;
    }
    if (head_len == 0 && body == 0) {
        return ESP_OK;
    }

    /*
     * Zero timeout: this runs on the WebSocket task, which must keep draining
     * the socket -- blocking here would stall the session. Space is measured
     * first and rounded down to a whole sample so that a full buffer costs a
     * click, not a permanently misaligned stream.
     */
    size_t space = xStreamBufferSpacesAvailable(s_ring) & ~(size_t)1;
    size_t sent = 0;
    size_t dropped = 0;

    if (head_len > 0) {
        if (space >= head_len) {
            sent += xStreamBufferSend(s_ring, head, head_len, 0);
            space -= head_len;
        } else {
            dropped += head_len;
        }
    }
    if (body > 0) {
        size_t to_send = (body < space) ? body : space;
        if (to_send > 0) {
            sent += xStreamBufferSend(s_ring, pcm, to_send, 0);
        }
        dropped += body - to_send;
    }

    s_played += sent;
    if (dropped > 0) {
        s_dropped += dropped;
        /* Rate-limited: a full buffer usually means many frames in a row. */
        static int64_t next_warn_us;
        int64_t now = esp_timer_get_time();
        if (now >= next_warn_us) {
            next_warn_us = now + 1000000;
            ESP_LOGW(TAG, "playback buffer full, dropped %u B (total %" PRIu32 ")",
                     (unsigned)dropped, s_dropped);
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void audio_io_set_playback_tap(audio_io_tap_t tap)
{
    s_play_tap = tap;
}

void audio_io_set_capture_tap(audio_io_tap_t tap)
{
    s_cap_tap = tap;
}

void audio_io_flush(void)
{
    /*
     * Not xStreamBufferReset(): that fails outright while a task is blocked
     * reading, which the drain task normally is -- so the old code silently did
     * nothing. Ask the reader to drain instead; it checks every DRAIN_WAIT_MS.
     */
    if (s_ring != NULL) {
        s_flush_pending = true;
        /* The drain task drops its own carry; this is the producer's half, which
         * audio_io_reset() used to be the only thing that cleared -- and its sole
         * caller is a deliberate session stop, so an auto-reconnect never reached
         * it. */
        s_play_gap = true;
    }
}

void audio_io_mute_playback(bool muted)
{
    /*
     * Scoped to a turn by the caller, never latched here. main.c clears it on
     * AgentAudioDone and on a hard deadline, because a turn that never reports
     * done would otherwise leave the device permanently and silently deaf.
     */
    s_play_muted = muted;
    if (muted) {
        s_play_gap = true;
    }
}

void audio_io_capture_set_enabled(bool enabled)
{
    s_capture_enabled = enabled;
}

void audio_io_capture_set_monitor(bool monitor)
{
    s_monitor = monitor;
}

void audio_io_reset(void)
{
    /* Safe only because the caller has already stopped the WebSocket task, which
     * is the sole writer of the carry. */
    s_in_carry_valid = false;
    s_in_carry = 0;
    s_play_gap = false;
    s_play_muted = false;
    s_played = 0;
    s_dropped = 0;
    s_captured = 0;
}

bool audio_io_playback_active(void)
{
    if (s_ring == NULL) {
        return false;
    }
    if (xStreamBufferBytesAvailable(s_ring) > 0) {
        return true;
    }
    /* Nothing has ever played: both stamps are still 0, and `now - 0` is small
     * for the first PLAYBACK_TAIL_MS after boot -- which would claim playback is
     * active before a single sample has been queued. */
    if (s_play_write_ms == 0 && s_play_queue_ms == 0) {
        return false;
    }
    /* Elapsed per stamp, smaller wins -- see the declarations. */
    uint32_t now = now_ms();
    uint32_t since_write = now - s_play_write_ms;
    uint32_t since_queue = now - s_play_queue_ms;
    uint32_t since = (since_write < since_queue) ? since_write : since_queue;
    return since < PLAYBACK_TAIL_MS;
}

void audio_io_lane_peaks(uint32_t *mic, uint32_t *ref, uint32_t *dead)
{
    if (mic)  *mic = s_mic_peak;
    if (ref)  *ref = s_ref_peak;
    if (dead) *dead = s_dead_peak;
}

void audio_io_stats(uint32_t *played, uint32_t *dropped, uint32_t *captured)
{
    if (played)   *played = s_played;
    if (dropped)  *dropped = s_dropped;
    if (captured) *captured = s_captured;
}
