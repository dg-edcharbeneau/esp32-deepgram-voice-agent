#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "bsp/esp-bsp.h"

#include "audio_io.h"

static const char *TAG = "audio_io";

/* The codecs are opened with this many channels; see the header. */
#define CODEC_CHANNELS 2
#define CODEC_BITS     16

/*
 * Mono frames per capture read. 512 at 16 kHz is 32 ms, which is a comfortable
 * chunk for Deepgram and keeps the round trip snappy.
 */
#define CAPTURE_FRAMES 512

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

static volatile uint32_t s_played;
static volatile uint32_t s_dropped;
static volatile uint32_t s_captured;
static volatile int64_t s_last_play_us;
static volatile bool s_flush_pending;
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
    /* Mono in, stereo out: the codec is open with two channels, so every
     * sample has to be written twice. */
    int16_t *mono = heap_caps_malloc(CHUNK_MONO, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *stereo = heap_caps_malloc(CHUNK_MONO * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
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

        for (size_t i = 0; i < samples; i++) {
            stereo[2 * i] = mono[i];
            stereo[2 * i + 1] = mono[i];
        }

        int err = esp_codec_dev_write(s_spk, stereo, (int)(samples * 2 * sizeof(int16_t)));
        if (err != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "esp_codec_dev_write failed: %d", err);
        }
        s_last_play_us = esp_timer_get_time();
    }
}

/* ---------------- capture ---------------- */

static void capture_task(void *arg)
{
    const size_t stereo_bytes = CAPTURE_FRAMES * CODEC_CHANNELS * sizeof(int16_t);
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
            ESP_LOGI(TAG, "mic peak L=%d R=%d%s", peak_l, peak_r,
                     audio_io_playback_active() ? " (gated: agent speaking)" : "");
        }
#endif

#if CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS
        /*
         * Half duplex. The speaker and mic sit centimetres apart with no echo
         * cancellation in this project, so anything the agent says is captured
         * and sent straight back, and the agent starts answering itself.
         * Dropping capture while it speaks is the crude fix; it also disables
         * barge-in, which is the trade. Deepgram's echo-cancellation support is
         * the real answer -- see the Voice Agent "Audio Preprocessing &
         * Barge-In" docs.
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
        if (!s_capture_enabled) {
            continue;
        }

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

    s_rate = sample_rate;

    /* One sample_info for both: they share the I2S clock. */
    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = sample_rate,
        .channel         = CODEC_CHANNELS,
        .bits_per_sample = CODEC_BITS,
    };

#if CONFIG_AUDIO_OUT_EXTRA_GAIN_DB > 0
    /*
     * The stock curve tops out at unity, leaving most of the ES8311's +32 dB
     * DAC range unused. Re-map the top of the scale before setting the volume.
     * Only the top point moves: the bottom stays at -50 dB so the taper below
     * full volume is unchanged.
     */
    esp_codec_dev_vol_map_t vol_map[2] = {
        { .vol = 0,   .db_value = -50.0f },
        { .vol = 100, .db_value = (float)CONFIG_AUDIO_OUT_EXTRA_GAIN_DB },
    };
    esp_codec_dev_vol_curve_t curve = { .vol_map = vol_map, .count = 2 };
    int vol_err = esp_codec_dev_set_vol_curve(s_spk, &curve);
    if (vol_err != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "volume curve rejected: %d", vol_err);
    }
#endif

    /*
     * Both of these must follow the codec _init() calls above, not precede
     * them: esp_codec_dev_set_out_vol() checks that the codec is open and
     * returns without storing anything if it is not. es8311_codec_new() opens
     * it, so by here it is.
     */
    esp_codec_dev_set_out_vol(s_spk, CONFIG_AUDIO_OUT_VOLUME);
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

    ESP_LOGI(TAG, "codecs open: %d Hz, %d-bit, %d ch | volume %d (+%d dB), mic gain %d dB",
             sample_rate, CODEC_BITS, CODEC_CHANNELS,
             CONFIG_AUDIO_OUT_VOLUME, CONFIG_AUDIO_OUT_EXTRA_GAIN_DB, CONFIG_MIC_IN_GAIN);
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
