#include <inttypes.h>
#include <stdlib.h>
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

#include "audio_codecs.h"
#include "audio_io.h"

#if CONFIG_AEC_ENABLE
#include "esp_aec.h"
#endif

static const char *TAG = "audio_io";

/* The codecs are opened with this many channels; see the header. */
#define CODEC_CHANNELS 2

#if CONFIG_AEC_ENABLE
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
 *
 * The playback side pays for this too: the ES8311 is opened at the same width,
 * so the drain task left-justifies each mono sample into a 32-bit slot.
 */
#define CODEC_BITS     32
typedef int32_t codec_sample_t;
#else
#define CODEC_BITS     16
typedef int16_t codec_sample_t;
#endif

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
#define CAPTURE_FRAMES AUDIO_IO_CAPTURE_FRAMES

/*
 * Playback ring holds MONO bytes -- the stereo doubling happens in the drain
 * task, so the buffer covers twice the wall-clock it otherwise would. 192 kB of
 * mono at 16 kHz is ~6 s, which comfortably holds one agent turn. Deepgram
 * delivers a turn faster than it plays, so this buffer is what absorbs the
 * difference; too shallow and long replies drop mid-sentence.
 */
#define RING_BYTES  (384 * 1024)
#define CHUNK_MONO  1024

/* How often a parked capture task looks to see whether it is wanted again. The
 * whole of this is hidden behind the WebSocket handshake on the way back up. */
#define CAPTURE_PARK_POLL_MS 100

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

#if CONFIG_AEC_ENABLE
/*
 * The canceller, and the two buffers it needs beside the ones already here.
 *
 * FD_LOW_COST by measurement, not by the published table: it achieved 17.3 dB of
 * ERLE on Espressif's own vectors against their 18.3 -- BOTH AT nlp_level = AGGR,
 * which is what the bench hardcodes and NOT what this build necessarily runs; see
 * CONFIG_AEC_NLP_LEVEL. The SR modes have no non-linear stage at all
 * (aec_nlp_process() returns 0) and scored NEGATIVE.
 *
 * ITS INTERNAL-RAM COST IS ~14.7 kB, AND THE OLD "16 BYTES" WAS WRONG.
 *
 * Measured 2026-08-29 on this board, steady state past 20 s uptime, live session,
 * gate still closed, AEC the only variable -- 39 samples against 33:
 *
 *                     AEC off      AEC on       delta
 *   int (free)        70,260 B     55,564 B     -14,696
 *   intmax (largest)  34,816 B     22,528 B     -12,288
 *   fps                 26.2         19.7          -6.5
 *
 * Both intmax figures were pinned, not noisy. That corroborates the 13,348 B a
 * later test app measured for this mode on esp-sr 2.5.1 and refutes the 16 B
 * this comment used to assert, which came from an archived bench and had been
 * carried forward unchecked ever since.
 *
 * 22.5 kB of largest free block is still 13x the 1,630 B INTERNAL|DMA allocation
 * whose failure killed the previous full-duplex attempt, and far from the 3,584 B
 * the AFE attempt died at -- so this is a real price, not obviously a fatal one.
 * Note the earlier measurement of the SAME class made three times in this
 * project: read intmax, never total free.
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
 * user -- twice in three runs, never later in the session.
 *
 * So behave like the old half-duplex gate until the filter has actually seen
 * echo, then defer to CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS. Counted in blocks with
 * real reference energy rather than in wall-clock, because silence teaches an
 * adaptive filter nothing: 16 blocks at 64 ms is about a second of the agent
 * actually speaking.
 */
/* A Kconfig bool is UNDEFINED when off rather than 0, so it cannot be used in a
 * C expression -- only in #if, where undefined evaluates to 0. */
#if CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS
#define AEC_GATE_DESC "on (half duplex)"
#else
#define AEC_GATE_DESC "OFF (full duplex)"
#endif

#if CONFIG_AEC_UPLINK_VAD
/*
 * UPLINK VAD STATE.
 *
 * Hangover is counted in capture blocks rather than milliseconds so the
 * comparison is an integer decrement per block and nothing reads the clock in
 * the audio path. One block is CAPTURE_FRAMES at 16 kHz = 64 ms.
 */
#define VAD_BLOCK_MS (CAPTURE_FRAMES * 1000 / 16000)
#define VAD_HANGOVER_BLOCKS \
    ((CONFIG_AEC_UPLINK_VAD_HANGOVER_MS + VAD_BLOCK_MS - 1) / VAD_BLOCK_MS)
static uint32_t s_vad_hold;      /* blocks left before the uplink shuts */
static volatile uint32_t s_vad_suppressed; /* blocks dropped, for telemetry */
#endif

#define AEC_WARMUP_BLOCKS 16
#define AEC_WARMUP_REF_PEAK 500
static uint32_t s_aec_warm;

static aec_handle_t *s_aec;
static int16_t *s_aec_ref;   /* lane 0, the echo reference */
static int16_t *s_aec_out;   /* cancelled microphone */
static int s_aec_chunk;
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
/*
 * The capture task's handle, which exists to make starting twice impossible.
 *
 * Not a diagnostic: audio_io.h states that the task cannot be created twice as
 * though it were a property of this module, and until now nothing enforced it.
 * A second audio_cap task at priority 7 would invalidate every "one writer only,
 * so the read-modify-write needs no lock" argument in ui.c and face_spectrum.c
 * at once -- s_level_peak's peak-hold, the FFT window's hop fill, and the
 * seqlock's publish counter all assume a single producer.
 */
static TaskHandle_t s_capture_task;
static volatile bool s_capture_enabled = true;
/* Tap-only override, consulted only while capture is disabled. See
 * audio_io_capture_set_monitor() in the header for why it exists. */
static volatile bool s_monitor;
static int s_rate;
/* The format both codecs were opened with. File scope so capture_task can
 * reopen the microphone after parking it; see the park block in that task. */
static esp_codec_dev_sample_info_t s_fs;

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
    /* PSRAM: esp_codec_dev_write() copies out of here into the I2S DMA
     * descriptors, so it never needs to be DMA-capable itself, and internal RAM
     * is the scarce resource on this board. Measured over the AEC work: moving
     * this and its capture twin took largest free internal block at session start
     * from 32,768 to 59,392. */
    codec_sample_t *stereo = heap_caps_malloc(CHUNK_MONO * sizeof(codec_sample_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mono == NULL || stereo == NULL) {
        /* Name both pools, because only one of these is internal RAM and a
         * message that blames the wrong one sends the next person to the wrong
         * budget. Free the half that succeeded: this task is about to go away,
         * so nothing else will. */
        ESP_LOGE(TAG, "no memory for playback buffers (mono %d B internal, "
                      "stereo %u B PSRAM)", CHUNK_MONO,
                 (unsigned)(CHUNK_MONO * sizeof(codec_sample_t)));
        free(mono);
        free(stereo);
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

        for (size_t i = 0; i < samples; i++) {
#if CONFIG_AEC_ENABLE
            /*
             * Left-justified into the 32-bit slot: the ES8311 takes the top 16
             * bits, so << 16 is the identity transform on the audio and only the
             * container changed. Shifting the other way would attenuate by 96 dB.
             */
            const codec_sample_t v = (int32_t)mono[i] << 16;
#else
            const codec_sample_t v = mono[i];
#endif
            stereo[2 * i] = v;
            stereo[2 * i + 1] = v;
        }

        int err = esp_codec_dev_write(s_spk, stereo,
                                      (int)(samples * 2 * sizeof(codec_sample_t)));
        if (err != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "esp_codec_dev_write failed: %d", err);
        }
        s_play_write_ms = now_ms();
    }
}

/* ---------------- capture ---------------- */

static void capture_task(void *arg)
{
#if CONFIG_AEC_ENABLE
    /* Four 16-bit TDM slots per frame -- 8 bytes -- read as two 32-bit I2S words. */
    const size_t stereo_bytes = CAPTURE_FRAMES * AEC_LANES * sizeof(int16_t);
#else
    const size_t stereo_bytes = CAPTURE_FRAMES * CODEC_CHANNELS * sizeof(int16_t);
#endif
    const size_t mono_bytes = CAPTURE_FRAMES * sizeof(int16_t);

    /* PSRAM, same reasoning as the playback side: this is only where
     * esp_codec_dev_read() copies the DMA descriptors to. `mono` stays internal
     * because every consumer walks it per sample.
     *
     * `mono` is 16-byte aligned unconditionally rather than only under the AEC:
     * esp_aec.h warns about the requirement twice, and an alignment that depends
     * on a build flag is the kind that holds until the flag is flipped on the
     * bench and nowhere else. It costs nothing when the canceller is off. */
    int16_t *stereo = heap_caps_malloc(stereo_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *mono = heap_caps_aligned_alloc(16, mono_bytes,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (stereo == NULL || mono == NULL) {
        /* Same as the playback side: name the right pool for each, and release
         * whichever one succeeded. */
        ESP_LOGE(TAG, "no memory for capture buffers (stereo %u B PSRAM, "
                      "mono %u B internal)",
                 (unsigned)stereo_bytes, (unsigned)mono_bytes);
        free(stereo);
        free(mono);
        vTaskDelete(NULL);
    }

#if CONFIG_MIC_LEVEL_LOG
    int64_t next_level_log = 0;
    int64_t next_speak_log = 0;
#endif

    /* The microphone is open on entry -- audio_io_init() opened it. */
    bool mic_open = true;

    while (1) {
#if !CONFIG_AUDIO_CAPTURE_ALWAYS
        /*
         * PARK BEFORE THE READ, not after the downmix.
         *
         * The session gate further down has always stopped samples reaching the
         * network, which is what it was written for -- but it sits BELOW the
         * read, so a stopped device still pulled 5,120 B off the ES7210 and ran
         * this loop's 1,280-sample downmix 12.5 times a second before throwing
         * the result away. That is 88% of this device's life.
         *
         * The condition is "nobody downstream wants samples", which the existing
         * state already answers: the session gate is shut AND monitor mode is off.
         * Monitor mode is the display test feeding the orb with no session, so it
         * has to keep the microphone running.
         *
         * While parked the codec is CLOSED, which stops the ADC and the I2S clock
         * rather than merely ignoring them -- and closing it here, from the task
         * that reads it, is the only safe place to do so. Closing a codec another
         * task is blocked inside esp_codec_dev_read() on is a race with no upside.
         *
         * Waking costs one poll interval plus the open, against the 1.1-6.0 s the
         * WebSocket handshake takes; see the note on CONNECTING in main.c.
         */
        if (!s_capture_enabled && !s_monitor) {
            if (mic_open) {
                esp_codec_dev_close(s_mic);
                mic_open = false;
                ESP_LOGI(TAG, "microphone parked");
            }
            vTaskDelay(pdMS_TO_TICKS(CAPTURE_PARK_POLL_MS));
            continue;
        }
        if (!mic_open) {
            /* Gain before open, mirroring audio_io_init()'s order. */
            esp_codec_dev_set_in_gain(s_mic, (float)CONFIG_MIC_IN_GAIN);
            /*
             * ONE UPSTREAM LOG LINE, SILENCED FOR THE LENGTH OF ONE CALL.
             *
             * esp_codec_dev's _i2s_data_set_fmt() disables the I2S channel before
             * reconfiguring it, UNCONDITIONALLY -- it never asks whether the
             * channel was enabled, and it discards the return value. After a
             * close it was not, so the driver logs
             *
             *   E i2s_common: i2s_channel_disable(): the channel has not been
             *   enabled yet
             *
             * and carries on correctly. Nothing is wrong: the reopen that follows
             * works, which the session either side of it demonstrates. But it is
             * an ERROR-level line on every single wake, and a log where E means
             * "look at this" is worth more than one where it does not.
             *
             * Scoped to this call rather than silenced globally, and restored
             * immediately, so a genuine i2s fault anywhere else still shouts. The
             * only i2s activity inside the window is the open being performed
             * here, and its own result is still checked below.
             *
             * Delete this once esp_codec_dev checks the channel state before
             * disabling it.
             */
            esp_log_level_set("i2s_common", ESP_LOG_NONE);
            int oerr = esp_codec_dev_open(s_mic, &s_fs);
            esp_log_level_set("i2s_common", ESP_LOG_INFO);
            if (oerr != ESP_CODEC_DEV_OK) {
                ESP_LOGE(TAG, "microphone reopen failed: %d", oerr);
                vTaskDelay(pdMS_TO_TICKS(CAPTURE_PARK_POLL_MS));
                continue;
            }
#if CONFIG_AEC_ENABLE
            /*
             * AFTER the open, and it has to be redone on every wake -- not just
             * once in audio_io_init(). esp_codec_dev_open() ends in
             * _update_codec_setting(), which replays the device-wide mic_gain
             * over all four inputs, so each reopen silently puts
             * CONFIG_MIC_IN_GAIN back onto the reference lane and clips it.
             * Nothing reports that: es7210's vtable never assigns .is_open, so
             * every call here returns OK either way.
             */
            int rerr = esp_codec_dev_set_in_channel_gain(s_mic, AEC_REF_INPUT_MASK,
                                                         (float)CONFIG_AEC_REF_GAIN_DB);
            if (rerr != ESP_CODEC_DEV_OK) {
                ESP_LOGW(TAG, "reference lane gain not restored on wake: %d", rerr);
            }
#endif
            mic_open = true;
            ESP_LOGI(TAG, "microphone resumed");
        }
#endif
        int err = esp_codec_dev_read(s_mic, stereo, (int)stereo_bytes);
        if (err != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "esp_codec_dev_read failed: %d", err);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

#if CONFIG_MIC_LEVEL_LOG
        /*
         * DECIDED BEFORE THE WORK, NOT AFTER IT -- which is the whole reason this
         * sits up here rather than next to the ESP_LOGI that consumes it.
         *
         * The post-AEC peak needs its own pass over the block, and it used to run
         * on every block while being printed at most every 500 ms: seven passes
         * in eight computed and thrown away, on the capture task, inside the
         * budget the canceller had just made tight. Knowing here whether the line
         * will print is what lets that pass be skipped.
         *
         * TWO RATES, AND THE FAST ONE IS THE POINT. A flat 3 s timer samples
         * whatever it lands on, and what it lands on is mostly silence: across
         * four captures only 2 of 11 samples fell during playback, which is the
         * only moment an echo canceller can be judged at all. The first read of
         * those two suggested the reference lane was dead; it was not, it was
         * undersampled. So while the agent is speaking, log every 500 ms.
         */
        const int64_t now = esp_timer_get_time();
        bool want_level_log = false;
        if (audio_io_playback_active() && now >= next_speak_log) {
            next_speak_log = now + 500000;
            next_level_log = now + 3000000;
            want_level_log = true;
        } else if (now >= next_level_log) {
            next_level_log = now + 3000000;
            next_speak_log = now;
            want_level_log = true;
        }
#endif

#if CONFIG_AEC_ENABLE
        /*
         * ONE runtime test, used by every AEC site below.
         *
         * s_aec_ref was previously written inside the downmix loop under the
         * compile-time #if alone, while the null check sat further down. Both of
         * audio_io_init()'s graceful-degradation paths leave these pointers NULL
         * -- so a failed aec_create_from_config(), the one case the comment there
         * promises is survivable, stored to address 0 on the first capture block
         * and panicked. Deterministic, so it would have been a boot loop, on the
         * path meant to avoid one.
         */
        const bool aec_on = (s_aec != NULL && s_aec_ref != NULL && s_aec_out != NULL);

        int32_t peak_l = 0, peak_r = 0, peak_ref = 0;
#if CONFIG_MIC_LEVEL_LOG
        /* Instrumentation only -- peak_ref is not, the convergence gate reads it. */
        int32_t peak_dead = 0, peak_post = 0;
#endif
        for (size_t i = 0; i < CAPTURE_FRAMES; i++) {
            const int16_t *f = &stereo[AEC_LANES * i];
            /*
             * THE TWO MICROPHONE LANES, and only those. The probe in 9479446
             * averaged lanes 0 and 1 because it did not yet know the order; lane
             * 0 is the echo REFERENCE, so mixing it in here would send the
             * speaker's own signal to Deepgram -- guaranteeing the
             * self-conversation this whole exercise exists to stop.
             */
            const int16_t l = f[AEC_LANE_MIC_A];
            const int16_t r = f[AEC_LANE_MIC_B];
            mono[i] = (int16_t)(((int32_t)l + (int32_t)r) / 2);
            if (aec_on) {
                s_aec_ref[i] = f[AEC_LANE_REF];
            }

            /* int32_t, not int16_t: negating INT16_MIN in 16 bits gives
             * INT16_MIN back, so a full-scale negative sample used to read as
             * the quietest possible one. */
            int32_t al = (l < 0) ? -(int32_t)l : (int32_t)l;
            int32_t ar = (r < 0) ? -(int32_t)r : (int32_t)r;
            int32_t aref = (f[AEC_LANE_REF] < 0) ? -(int32_t)f[AEC_LANE_REF]
                                                 : (int32_t)f[AEC_LANE_REF];
            if (al > peak_l) peak_l = al;
            if (ar > peak_r) peak_r = ar;
            if (aref > peak_ref) peak_ref = aref;
#if CONFIG_MIC_LEVEL_LOG
            int32_t adead = (f[AEC_LANE_DEAD] < 0) ? -(int32_t)f[AEC_LANE_DEAD]
                                                   : (int32_t)f[AEC_LANE_DEAD];
            if (adead > peak_dead) peak_dead = adead;
#endif
        }

        /*
         * CANCEL BEFORE ANY GATE, TAP OR SINK. Everything downstream -- the level
         * the orb draws, what Deepgram hears -- should see the cleaned signal,
         * not the raw microphone.
         *
         * Two chunks of exactly s_aec_chunk; CAPTURE_FRAMES is sized so there is
         * no remainder, and audio_io_init() refuses to enable the canceller if
         * the library disagrees. The filter is adaptive and stateful, so every
         * block must go through in order even when the session is stopped --
         * skipping frames would make it diverge and it would then have to
         * reconverge mid-reply, which is the worst possible moment.
         */
        if (aec_on) {
            for (size_t off = 0; off + (size_t)s_aec_chunk <= CAPTURE_FRAMES;
                 off += (size_t)s_aec_chunk) {
                aec_process(s_aec, mono + off, s_aec_ref + off, s_aec_out + off);
            }
            memcpy(mono, s_aec_out, CAPTURE_FRAMES * sizeof(int16_t));

            /* POST-AEC, and that is the point of computing it here rather than in
             * the loop above. The previous attempt reported "mic peak 28-34
             * against 25,102 uncancelled" as evidence the canceller worked; that
             * line was computed in the downmix loop, upstream of aec_process(),
             * and had always been showing the uncancelled microphone. */
#if CONFIG_MIC_LEVEL_LOG
            if (want_level_log) {
                for (size_t i = 0; i < CAPTURE_FRAMES; i++) {
                    int32_t a = (mono[i] < 0) ? -(int32_t)mono[i] : (int32_t)mono[i];
                    if (a > peak_post) peak_post = a;
                }
            }
#endif

            if (s_aec_warm < AEC_WARMUP_BLOCKS && peak_ref > AEC_WARMUP_REF_PEAK) {
                s_aec_warm++;
                if (s_aec_warm == AEC_WARMUP_BLOCKS) {
                    ESP_LOGI(TAG, "AEC converged (%d blocks of reference audio)",
                             AEC_WARMUP_BLOCKS);
                }
            }
            /* Half-duplex until then regardless of the gate setting -- the filter
             * still gets every frame above, it is only the OUTPUT that is
             * withheld while it is still learning. */
            if (s_aec_warm < AEC_WARMUP_BLOCKS && audio_io_playback_active()) {
                continue;
            }
        }
#else
        int32_t peak_l = 0, peak_r = 0;
        for (size_t i = 0; i < CAPTURE_FRAMES; i++) {
            int16_t l = stereo[2 * i];
            int16_t r = stereo[2 * i + 1];
            /* Average, matching spec_analyzer_radial's downmix. */
            mono[i] = (int16_t)(((int32_t)l + (int32_t)r) / 2);

            /* int32_t, not int16_t: negating INT16_MIN in 16 bits gives
             * INT16_MIN back, so a full-scale negative sample used to read as
             * the quietest possible one. */
            int32_t al = (l < 0) ? -(int32_t)l : (int32_t)l;
            int32_t ar = (r < 0) ? -(int32_t)r : (int32_t)r;
            if (al > peak_l) peak_l = al;
            if (ar > peak_r) peak_r = ar;
        }
#endif

#if CONFIG_MIC_LEVEL_LOG
        /*
         * Per-channel peaks, not a combined level. If the board only wires one
         * of the ES7210's inputs, the downmix above halves every sample and a
         * combined meter would just look "quiet" -- this shows which channel is
         * actually live.
         */
        if (want_level_log) {
#if CONFIG_AEC_ENABLE
            /*
             * L and R are the RAW microphone lanes; `out` is what actually
             * leaves this task. Both are printed because the pair is the
             * measurement -- out against L/R while the speaker is live is the
             * ERLE, and a single number cannot show it. `dead` is MIC4,
             * AC-coupled to AGND: it reads 3-9 always, and is the control that
             * identifies the slot order.
             */
            ESP_LOGI(TAG, "mic peak L=%d R=%d out=%d ref=%d dead=%d%s",
                     (int)peak_l, (int)peak_r, (int)peak_post,
                     (int)peak_ref, (int)peak_dead,
                     audio_io_playback_active() ? " (agent speaking)" : "");
#else
            ESP_LOGI(TAG, "mic peak L=%d R=%d%s", (int)peak_l, (int)peak_r,
                     audio_io_playback_active() ? " (gated: agent speaking)" : "");
#endif
        }
#endif

        /*
         * Half duplex. The speaker and mic sit centimetres apart, so without a
         * canceller anything the agent says is captured and sent straight back,
         * and the agent starts answering itself. Dropping capture while it
         * speaks is the crude fix; it also disables barge-in, which is the trade.
         *
         * THE FLAG ONLY EXISTS WHEN THE CANCELLER DOES. With CONFIG_AEC_ENABLE
         * off this is unconditional, as it has been since the previous attempt
         * was abandoned, and CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS is not offered:
         * an uncancelled open microphone during playback is the
         * self-conversation, not barge-in. See the Kconfig help for the volume
         * the experiment has to be run at.
         *
         * The real answer is NOT server-side, whatever an earlier version of this
         * comment claimed. Deepgram's "Audio Preprocessing & Barge-In" guide has
         * no AEC setting and explicitly pushes cancellation to the device.
         *
         * A CANCELLER WAS BUILT AND IT WORKS. Not the ~70 kB AFE a4fa137 measured
         * -- that was the wrong object -- but esp-sr's standalone AEC in
         * FD_LOW_COST, which achieves 17.3 dB of ERLE against Espressif's own
         * output at 18.3 on their test vectors. With it running the device stops
         * answering itself: one turn in an empty room where before there were
         * sixteen in twenty-four seconds. (Its internal-RAM cost used to be
         * asserted here as 16 bytes. That figure is disputed and has been moved
         * to the s_aec declaration, where the dispute is written down.)
         *
         * IT DID NOT BUY BARGE-IN when it was tried, for two reasons, neither of
         * them the canceller's: streaming the microphone through the agent's
         * reply saturated the TCP send queue until a 1,630 B DMA allocation
         * failed and the session dropped, and even while that audio reached
         * Deepgram it never distinguished a person talking over the agent from
         * the residual echo. Both measurements were taken at
         * AUDIO_OUT_VOLUME=100, which is the one setting where the required ERLE
         * cannot be met -- that is what is being retested, and it is why the
         * gate is now a flag rather than a constant. Regardless of how it goes,
         * the interrupt on this device stays a tap on the centre button while
         * the agent is speaking -- see on_gesture() in main.c. It works in every
         * room and needs no canceller.
         *
         * The whole investigation, the numbers and where the removed code lives
         * are in docs/notes/echo-cancellation.md. Read it before removing this gate again.
         *
         * THE MUTE DELIBERATELY DOES NOT GATE THIS. It used to, to keep the
         * microphone out of the reply's inbound tail after an interruption. Two
         * things killed that: the release for the mute is the user SPEAKING AGAIN
         * (AgentAudioDone does not arrive reliably -- measured 0 times across a
         * 12-minute run), so a gated mic can never hear its own release; and the
         * session-death that motivated the gating is handled properly now, in
         * transport_ws.c LOCAL PATCH 2, which drops a congested audio frame
         * instead of tearing the socket down. Full duplex here costs dropped
         * frames, not the conversation.
         */
#if !CONFIG_AEC_ENABLE || CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS
        if (audio_io_playback_active()) {
            continue;
        }
#else
        /*
         * A VOLUME CEILING THAT IS OFF BY DEFAULT -- the condition is never true
         * at the default of 100, and this compiles to a compare the branch
         * predictor will never take.
         *
         * It exists because an archived table predicted this board's amplifier
         * going non-linear between 85 and 100 and ERLE collapsing with it.
         * Measured on hardware, it does not: 23.2 dB mean at volume 100 against
         * 22.4 at 70, and barge-in fires at both. The ceiling was set to 85 on
         * the strength of the table and moved to 100 on the strength of the
         * measurement. See the Kconfig help.
         *
         * Kept rather than deleted because the effect it guards against is real
         * amplifier physics that another unit may show. Lowering it costs
         * barge-in above that volume WITHOUT telling the user -- the prompt is
         * chosen at build time and will still say they can talk over the agent.
         *
         * Read live rather than latched at init, because audio_io_adjust_volume()
         * can move it mid-session and the ceiling has to move with it.
         */
        if (audio_io_playback_active() &&
            s_volume > CONFIG_AEC_FULL_DUPLEX_MAX_VOLUME) {
            continue;
        }
#endif

#if CONFIG_AEC_UPLINK_VAD
        /*
         * THE UPLINK VAD, AND IT ONLY EXISTS DURING PLAYBACK.
         *
         * The canceller solved the echo; it did not solve the bandwidth. With
         * the gate open the device streamed 32 kB/s straight through the reply
         * and starved the TLS path of INTERNAL|DMA memory -- the 1,630 B
         * allocation failed eight times and the session dropped, exactly as it
         * had before. See the Kconfig help for the measurement.
         *
         * So during playback, forward a block only if the POST-AEC signal has
         * real energy in it. `mono` is already the cancelled signal here, which
         * is the whole reason this can work: residual echo is a few tens of
         * counts while somebody talking over the agent is an order of magnitude
         * above that, so the test separates cleanly on a plain peak.
         *
         * MEASURED ON THE CANCELLED SIGNAL, NOT THE MICROPHONE. Reading the raw
         * lanes here would see the agent's own voice at full strength and hold
         * the uplink open for the entire reply, which is the behaviour being
         * removed. This is the same class of mistake as the level log that
         * reported uncancelled audio for the whole of the previous attempt.
         *
         * A DECISION, NOT A JUMP. It has to be computed here -- while `mono` is
         * the cancelled signal and before anything downstream -- but it must not
         * skip the session gate below or the display tap, so it is carried to
         * the sink as a flag. Outside playback the flag is always true and
         * ordinary listening is bit-for-bit unchanged.
         */
        bool uplink_ok = true;
        if (audio_io_playback_active()) {
            int32_t vad_peak = 0;
            for (size_t i = 0; i < CAPTURE_FRAMES; i++) {
                int32_t a = (mono[i] < 0) ? -(int32_t)mono[i] : (int32_t)mono[i];
                if (a > vad_peak) {
                    vad_peak = a;
                }
            }
            if (vad_peak >= CONFIG_AEC_UPLINK_VAD_PEAK) {
                s_vad_hold = VAD_HANGOVER_BLOCKS;
            } else if (s_vad_hold > 0) {
                /* Inside the hangover: still speech as far as the uplink is
                 * concerned, so the rest of the sentence survives the pauses
                 * between its words. */
                s_vad_hold--;
            } else {
                uplink_ok = false;
                s_vad_suppressed++;
            }
        } else {
            s_vad_hold = 0;
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

#if CONFIG_AEC_UPLINK_VAD
        /* The tap above already ran, so the orb keeps drawing the room whether
         * or not the network is being spared. Only the uplink is withheld. */
        if (s_sink != NULL && uplink_ok) {
#else
        if (s_sink != NULL) {
#endif
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
#if CONFIG_AEC_ENABLE
    /*
     * NOT THE BSP, and audio_codecs.h has the full reason. In short: the
     * canceller needs the hardware echo-reference lane, which is ES7210 MIC3;
     * bsp_audio_codec_microphone_init() leaves `mic_selected` at 0 so the driver
     * defaults to MIC1|MIC2 and MIC3 is never powered or clocked out. The BSP
     * also keeps its i2s_data_if file-static with no accessor, so there is no way
     * to reach MIC3 through it. audio_codecs_init_tdm() is the BSP's own bring-up
     * with that one field changed.
     *
     * It calls bsp_i2c_init() itself, first, for the same reason the BSP does.
     */
    esp_err_t codec_err = audio_codecs_init_tdm(&s_spk, &s_mic);
    if (codec_err != ESP_OK) {
        ESP_LOGE(TAG, "TDM codec init failed: %s", esp_err_to_name(codec_err));
        return codec_err;
    }
#else
    s_spk = bsp_audio_codec_speaker_init();
    if (s_spk == NULL) {
        ESP_LOGE(TAG, "speaker (ES8311) init failed");
        return ESP_FAIL;
    }

    s_mic = bsp_audio_codec_microphone_init();
    if (s_mic == NULL) {
        ESP_LOGE(TAG, "microphone (ES7210) init failed");
        return ESP_FAIL;
    }
#endif

    s_rate = sample_rate;

    /* One sample_info for both: they share the I2S clock. Kept at file scope so
     * the capture task can reopen the microphone with exactly this format. */
    s_fs = (esp_codec_dev_sample_info_t){
        .sample_rate     = sample_rate,
        .channel         = CODEC_CHANNELS,
        .bits_per_sample = CODEC_BITS,
    };
    esp_codec_dev_sample_info_t fs = s_fs;

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
#if CONFIG_AEC_ENABLE
    /*
     * AFTER THE OPEN, AND THAT IS THE WHOLE POINT.
     *
     * esp_codec_dev_set_in_gain() above is device-wide: es7210_set_gain() fans
     * out to _es7210_set_channel_gain(codec, 0xF, db), all four inputs, so the
     * echo reference gets CONFIG_MIC_IN_GAIN too. It is a line-level tap of the
     * ES8311's own output and wants far less -- at 24 dB it has under 8 dB of
     * headroom left, and a clipped reference is a nonlinearity in the one signal
     * a canceller needs clean. But 0 dB is the opposite mistake -- 181 peak,
     * -45 dBFS -- so the value is CONFIG_AEC_REF_GAIN_DB.
     *
     * Setting it BEFORE the open silently does nothing. esp_codec_dev_open()
     * ends in _update_codec_setting(), which replays the stored device-wide
     * mic_gain over every channel -- so the per-channel value is overwritten
     * before a single frame is read, and both calls still return OK because
     * es7210's vtable never assigns .is_open so nothing rejects them. Measured:
     * the reference lane read 13,541 peak when it should read about 850.
     *
     * The mask indexes the ES7210's INPUTS, where the reference is MIC3 (bit 2).
     * That is NOT its position in the captured frame, which is lane 0. Confusing
     * the two silently attenuates a microphone instead.
     *
     * The capture task's park/resume path reapplies MIC_IN_GAIN and reopens the
     * microphone, so it has to reapply this too -- see there.
     */
    int ref_gain_err = esp_codec_dev_set_in_channel_gain(s_mic, AEC_REF_INPUT_MASK,
                                                         (float)CONFIG_AEC_REF_GAIN_DB);
    if (ref_gain_err != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "could not set reference lane gain: %d", ref_gain_err);
    }

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
             * not boot. The log says the microphone is uncancelled, and
             * capture_task's aec_on test is what keeps it from dereferencing
             * this. */
            ESP_LOGE(TAG, "AEC create failed -- running WITHOUT cancellation");
        } else {
            s_aec_chunk = aec_get_chunksize(s_aec);
            s_aec_ref = heap_caps_aligned_alloc(16, CAPTURE_FRAMES * sizeof(int16_t),
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            s_aec_out = heap_caps_aligned_alloc(16, CAPTURE_FRAMES * sizeof(int16_t),
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (s_aec_ref == NULL || s_aec_out == NULL ||
                s_aec_chunk <= 0 || (CAPTURE_FRAMES % s_aec_chunk) != 0) {
                /* The divisor is checked against what the library REPORTS rather
                 * than against the 512 it is expected to report: a partial frame
                 * handed to aec_process() is a frame of silence to the adaptive
                 * filter, and that failure is silent at every other layer. */
                ESP_LOGE(TAG, "AEC unusable (chunk=%d, frames=%d) -- disabling",
                         s_aec_chunk, CAPTURE_FRAMES);
                aec_destroy(s_aec);
                s_aec = NULL;
                free(s_aec_ref); free(s_aec_out);
                s_aec_ref = NULL; s_aec_out = NULL;
            } else {
                ESP_LOGI(TAG, "AEC: FD_LOW_COST, chunk=%d, %d per capture block, "
                         "nlp=%d, gate=%s", s_aec_chunk, CAPTURE_FRAMES / s_aec_chunk,
                         (int)CONFIG_AEC_NLP_LEVEL, AEC_GATE_DESC);
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
    /* Refused rather than ignored, the same way dg_agent_init() and
     * session_ctl_start() refuse a second call: silently creating a second
     * producer is the kind of fault that shows up as corrupted audio levels
     * somewhere else entirely. */
    if (s_capture_task != NULL) {
        ESP_LOGE(TAG, "capture already started");
        return ESP_ERR_INVALID_STATE;
    }
    s_sink = sink;

    /* Priority above playback: a missed read is lost audio, a late write is
     * only a small gap the ring buffer absorbs. */
    if (xTaskCreatePinnedToCore(capture_task, "audio_cap", 4096, NULL, 7,
                                &s_capture_task, 1) != pdPASS) {
        s_capture_task = NULL;
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
         * queue clock, so audio_io_playback_active() is allowed to fall -- the
         * speaker IS silent, and the display must stop radiating at it.
         *
         * The microphone stays OPEN through all of this, deliberately. The mute
         * is released by the user speaking again -- see main.c -- so gating the
         * mic on it would make the device deaf to its own release condition.
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

void audio_io_note_stream_gap(void)
{
    /* Same flag the flush and the mute raise; audio_io_play() consumes it before
     * the stitch. Safe from any task -- one bool store, and the producer is the
     * only reader. */
    s_play_gap = true;
}

void audio_io_mute_playback(bool muted)
{
    /*
     * Scoped by the caller, never latched here. main.c releases it when the user
     * speaks again, with a long backstop for a tap that is never followed by
     * speech -- left set forever, the agent would simply stop being audible.
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

uint32_t audio_io_uplink_suppressed(void)
{
#if CONFIG_AEC_UPLINK_VAD
    return s_vad_suppressed;
#else
    return 0;
#endif
}

void audio_io_stats(uint32_t *played, uint32_t *dropped, uint32_t *captured)
{
    if (played)   *played = s_played;
    if (dropped)  *dropped = s_dropped;
    if (captured) *captured = s_captured;
}
