#include <inttypes.h>
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

#include "audio_aec.h"
#include "audio_codecs.h"
#include "audio_io.h"

static const char *TAG = "audio_io";

/* The codecs are opened with this many channels; see the header. */
#define CODEC_CHANNELS 2
/*
 * Sample width on the wire to both codecs.
 *
 * 32 under CONFIG_AEC_REF_PROBE, and it is not about audio quality. The ES7210
 * emits a 4x16-bit TDM frame -- 64 bits -- and the S3's I2S RX cannot be put in
 * TDM mode independently, because RX and TX share BCLK/WS in full duplex and
 * must be configured identically. Reading the same 64 bits as 2 channels x
 * 32 bits gets the whole frame through standard I2S, with one 32-bit word
 * carrying two 16-bit channels.
 *
 * es7210_set_fs() is written for exactly this: in TDM mode with channel <= 2 and
 * channel_mask == 0 it halves the requested width, so asking for 32 programs
 * 16-bit slots.
 *
 * Do NOT ask for 16 expecting 8-bit slots. `bits >>= 1` gives 8 and
 * es7210_set_bits() has no case 8 -- it falls through to default and writes
 * 16-bit anyway, so the ADC would clock out 64 bits against 32 from the S3 and
 * MIC3/MIC4 would never be shifted out at all.
 */
#if CONFIG_AEC_REF_PROBE
#define CODEC_BITS     32
#else
#define CODEC_BITS     16
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
#define CAPTURE_FRAMES 1280

/*
 * Playback ring holds MONO bytes -- the stereo doubling happens in the drain
 * task, so the buffer covers twice the wall-clock it otherwise would. 192 kB of
 * mono at 16 kHz is ~6 s, which comfortably holds one agent turn. Deepgram
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
#define PLAYBACK_TAIL_US 300000

static esp_codec_dev_handle_t s_spk;
static esp_codec_dev_handle_t s_mic;
static StreamBufferHandle_t s_ring;
static audio_io_capture_sink_t s_sink;
static audio_io_tap_t s_play_tap;
static audio_io_tap_t s_cap_tap;
#if CONFIG_AEC_ENABLE
/* False if the AFE could not be created; the capture path then behaves exactly
 * as it does in a build without AEC. */
static bool s_aec_running;
#endif

static volatile uint32_t s_played;
static volatile uint32_t s_dropped;
static volatile uint32_t s_captured;
static volatile int64_t s_last_play_us;
static volatile bool s_flush_pending;
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
    /*
     * Mono in, stereo out: the codec is open with two channels, so every sample
     * has to be written twice. Under the probe the codec is also open at 32 bits,
     * so each written sample is four bytes and the output buffer doubles again.
     */
#if CONFIG_AEC_REF_PROBE
    const size_t out_bytes_per_sample = 2 * sizeof(int32_t);
#else
    const size_t out_bytes_per_sample = 2 * sizeof(int16_t);
#endif
    int16_t *mono = heap_caps_malloc(CHUNK_MONO, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    void *stereo = heap_caps_malloc((CHUNK_MONO / sizeof(int16_t)) * out_bytes_per_sample,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
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

#if CONFIG_AEC_REF_PROBE
        /*
         * Left-justified into the 32-bit slot: the ES8311 takes the top 16 bits,
         * so << 16 is the identity transform on the audio and only the container
         * changed. Shifting the other way would attenuate by 96 dB.
         */
        int32_t *out32 = (int32_t *)stereo;
        for (size_t i = 0; i < samples; i++) {
            int32_t v = (int32_t)mono[i] << 16;
            out32[2 * i] = v;
            out32[2 * i + 1] = v;
        }
#else
        int16_t *out16 = (int16_t *)stereo;
        for (size_t i = 0; i < samples; i++) {
            out16[2 * i] = mono[i];
            out16[2 * i + 1] = mono[i];
        }
#endif

        int err = esp_codec_dev_write(s_spk, stereo,
                                      (int)(samples * out_bytes_per_sample));
        if (err != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "esp_codec_dev_write failed: %d", err);
        }
        s_last_play_us = esp_timer_get_time();
    }
}

/* ---------------- capture ---------------- */

#if CONFIG_AEC_ENABLE
/*
 * Cancelled audio, on the AFE's fetch task. This is what goes upstream now: the
 * raw microphones still contain the agent's voice, and the whole point of the
 * canceller is that this does not.
 *
 * The two gates that used to live in capture_task move here, because they are
 * about what the SESSION should receive, not about how the audio is produced --
 * and the canceller must keep being fed regardless of either.
 */
static void aec_output(const int16_t *mono, size_t samples)
{
    /*
     * SUPPRESSION MEASUREMENT -- the pass/fail for whether cancellation works.
     *
     * Configuration being right is not the same as echo being cancelled. The
     * baseline from the reference probe, with no AEC at all:
     *
     *   mic lanes, agent speaking ... peaks 537-12353
     *   mic lanes, room quiet ...... peaks 30-334
     *
     * So with cancellation working and nobody talking, the peak HERE during
     * playback should fall toward the quiet-room floor instead of sitting at the
     * echo level. Roughly an order of magnitude of suppression is a pass. If it
     * does not, the filter is not converging: suspect the reference lane mapping
     * (AFE_INPUT_FORMAT) first and aec_filter_length second.
     *
     * Logged before the gates below, because it is a property of the canceller
     * rather than of what the session happens to be doing.
     */
#if CONFIG_MIC_LEVEL_LOG
    int16_t peak = 0;
    for (size_t i = 0; i < samples; i++) {
        int16_t a = (mono[i] < 0) ? (int16_t)-mono[i] : mono[i];
        if (a > peak) {
            peak = a;
        }
    }
    static int64_t next_log_us;
    static int16_t peak_play, peak_quiet;
    bool playing = audio_io_playback_active();
    if (playing) {
        if (peak > peak_play) peak_play = peak;
    } else if (peak > peak_quiet) {
        peak_quiet = peak;
    }
    int64_t now = esp_timer_get_time();
    if (now >= next_log_us) {
        next_log_us = now + 3000000;
        ESP_LOGI(TAG, "AECOUT peak play=%d quiet=%d (echo baseline was 537-12353,"
                      " quiet floor 30-334)", peak_play, peak_quiet);
        peak_play = peak_quiet = 0;
    }
#endif

    /* Session gate: a stopped device neither streams nor visualizes the room. */
    if (!s_capture_enabled) {
        return;
    }

    if (s_cap_tap != NULL) {
        s_cap_tap(mono, samples);
    }

#if CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS
    /*
     * Belt and braces, and deliberately kept available: with cancellation working
     * this gate is redundant, but leaving it switchable means AEC can be brought
     * up and observed without risking the agent answering itself. Turning this
     * gate off is the single change that enables barge-in.
     */
    if (audio_io_playback_active()) {
        return;
    }
#endif

    if (s_sink != NULL) {
        s_sink((const uint8_t *)mono, samples * sizeof(int16_t));
        s_captured += samples * sizeof(int16_t);
    }
}
#endif /* CONFIG_AEC_ENABLE */

static void capture_task(void *arg)
{
    /*
     * Under the probe a frame is four 16-bit TDM slots -- 8 bytes -- read as two
     * 32-bit I2S words. Without it, two 16-bit channels, 4 bytes.
     */
#if CONFIG_AEC_REF_PROBE
    const size_t stereo_bytes = CAPTURE_FRAMES * 4 * sizeof(int16_t);
#else
    const size_t stereo_bytes = CAPTURE_FRAMES * CODEC_CHANNELS * sizeof(int16_t);
#endif
    const size_t mono_bytes = CAPTURE_FRAMES * sizeof(int16_t);

    int16_t *stereo = heap_caps_malloc(stereo_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *mono = heap_caps_malloc(mono_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
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

#if CONFIG_AEC_REF_PROBE
        /*
         * Four lanes. Slot ORDER in TDM is not documented by the driver, so this
         * deliberately does not assume which lane is which: it measures all four
         * and lets the run identify them. Two independent signatures pin it down
         * -- exactly one lane should track playback amplitude (the reference,
         * expected at index 2), and exactly one should sit at the noise floor
         * under all conditions, because the netlist AC-couples MIC4 to AGND.
         * Finding the dead lane is what distinguishes "the reference works" from
         * "the slots are offset".
         */
        int16_t peak[4] = { 0, 0, 0, 0 };
        for (size_t i = 0; i < CAPTURE_FRAMES; i++) {
            const int16_t *f = &stereo[4 * i];
            for (int c = 0; c < 4; c++) {
                int16_t a = (f[c] < 0) ? (int16_t)-f[c] : f[c];
                if (a > peak[c]) {
                    peak[c] = a;
                }
            }
            /* Deepgram keeps being fed from the same two microphone lanes, so the
             * conversation is unaffected while probing. Lanes 1 and 3 are the
             * MEMS mics; lane 0 is the echo reference and lane 2 is grounded. */
            mono[i] = (int16_t)(((int32_t)f[1] + (int32_t)f[3]) / 2);
        }
#else
        int16_t peak_l = 0, peak_r = 0;
        for (size_t i = 0; i < CAPTURE_FRAMES; i++) {
            int16_t l = stereo[2 * i];
            int16_t r = stereo[2 * i + 1];
            /* Average, matching spec_analyzer_radial's downmix. */
            mono[i] = (int16_t)(((int32_t)l + (int32_t)r) / 2);

            int16_t al = (l < 0) ? -l : l;
            int16_t ar = (r < 0) ? -r : r;
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
        int64_t now = esp_timer_get_time();
        if (now >= next_level_log) {
            next_level_log = now + 3000000;
#if CONFIG_AEC_REF_PROBE
            /*
             * Reading this: with the agent speaking and nobody talking, exactly
             * one lane should rise -- that is the echo reference. Exactly one
             * should stay at the floor in every condition -- that is the
             * grounded MIC4, and it is what fixes the slot order. Speak with the
             * speaker idle and lanes 0/1 should rise while the reference stays
             * down, which rules out crosstalk being mistaken for a reference.
             */
            ESP_LOGI(TAG, "AECPROBE lanes=%d,%d,%d,%d play=%d",
                     peak[0], peak[1], peak[2], peak[3],
                     audio_io_playback_active() ? 1 : 0);
#else
            ESP_LOGI(TAG, "mic peak L=%d R=%d%s", peak_l, peak_r,
                     audio_io_playback_active() ? " (gated: agent speaking)" : "");
#endif
        }
#endif

#if CONFIG_AEC_ENABLE
        /*
         * Hand the whole four-channel frame to the canceller and stop here.
         *
         * ALWAYS, including while the agent is speaking -- that is precisely when
         * there is an echo to learn, so gating the feed would starve the adaptive
         * filter of the only signal it can converge on.
         *
         * Nothing else happens in this task now: the sink and the tap are driven
         * by aec_output() below, off the AFE's own fetch, because the cancelled
         * audio is what should go upstream rather than the raw microphones.
         */
        if (s_aec_running) {
            audio_aec_feed(stereo, CAPTURE_FRAMES);
            continue;
        }
        /* Fell back: drop through to the raw path below, gate included. */
#endif

#if CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS
        /*
         * Half duplex. The speaker and mic sit centimetres apart with no echo
         * cancellation in this project, so anything the agent says is captured
         * and sent straight back, and the agent starts answering itself.
         * Dropping capture while it speaks is the crude fix; it also disables
         * barge-in, which is the trade.
         *
         * The real answer is NOT server-side. Deepgram's "Audio Preprocessing &
         * Barge-In" guide has no AEC setting and explicitly pushes it to the
         * device. It has to run here, and the board does provide what is needed:
         * an echo reference wired from the ES8311's outputs into ES7210 MIC3,
         * sample-aligned because it is captured by the same ADC in the same
         * frame. Measured and confirmed -- see CONFIG_AEC_REF_PROBE and
         * audio_codecs.h. Cancellation itself would be esp-sr's AFE.
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
#if CONFIG_AEC_REF_PROBE
    /*
     * Build the codecs ourselves so the ES7210 gets all four inputs, which is
     * the only way MIC3 -- the echo reference -- is ever powered or clocked.
     * See audio_codecs.h. The BSP's own path hardcodes mic_selected to 0.
     */
    esp_err_t codec_err = audio_codecs_init_tdm(&s_spk, &s_mic);
    if (codec_err != ESP_OK) {
        ESP_LOGE(TAG, "TDM codec init failed: %s", esp_err_to_name(codec_err));
        return ESP_FAIL;
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

#if CONFIG_AEC_ENABLE
    /*
     * Before the capture task, so the AFE exists by the time the first frame is
     * fed to it. A failure here is fatal rather than degraded: with AEC compiled
     * in, capture_task feeds the AFE and nothing else, so a missing AFE means no
     * audio reaches the session at all.
     */
    esp_err_t aec_err = audio_aec_start(aec_output, CAPTURE_FRAMES);
    if (aec_err != ESP_OK) {
        /*
         * NOT fatal, and this matters: main.c wraps this call in
         * ESP_ERROR_CHECK, so returning an error here aborts the boot and the
         * device reboot-loops -- which is both useless and hard to recover from,
         * since esptool struggles to catch a chip that keeps resetting.
         *
         * Echo cancellation is an enhancement. Losing it costs barge-in, which
         * is exactly what the device did without it. So fall back to the raw
         * microphone path and say so loudly.
         */
        ESP_LOGE(TAG, "AEC unavailable (%s) -- falling back to the raw mic path; "
                      "keep CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS on",
                 esp_err_to_name(aec_err));
        s_aec_running = false;
    } else {
        s_aec_running = true;
    }
#endif

    /*
     * Priority above playback: a missed read is lost audio, a late write is only
     * a small gap the ring buffer absorbs.
     *
     * 4 kB is enough again. It was raised to 8 kB when this task also fed the
     * AFE -- which runs its filtering on the CALLER's stack and overflowed 4 kB
     * immediately -- but that work now lives on the AEC's own task on core 0. All
     * this task does is read the codec and hand the block over.
     */
    if (xTaskCreatePinnedToCore(capture_task, "audio_cap", 4096, NULL, 7,
                                NULL, 1) != pdPASS) {
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

    s_last_play_us = esp_timer_get_time();

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
    }
}

void audio_io_capture_set_enabled(bool enabled)
{
    s_capture_enabled = enabled;
}

void audio_io_reset(void)
{
    /* Safe only because the caller has already stopped the WebSocket task, which
     * is the sole writer of the carry. */
    s_in_carry_valid = false;
    s_in_carry = 0;
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
    return (esp_timer_get_time() - s_last_play_us) < PLAYBACK_TAIL_US;
}

void audio_io_stats(uint32_t *played, uint32_t *dropped, uint32_t *captured)
{
    if (played)   *played = s_played;
    if (dropped)  *dropped = s_dropped;
    if (captured) *captured = s_captured;
}
