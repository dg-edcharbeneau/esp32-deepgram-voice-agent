/*
 * esp-sr AFE wiring. See audio_aec.h for why this exists.
 */

#include "audio_aec.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_afe_config.h"
#include "esp_aec.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "audio_aec";

/*
 * Channel layout of the ES7210's TDM frame AS IT LANDS IN MEMORY.
 *
 * M = microphone, R = playback reference, N = unused. Position in this string is
 * position in the interleaved frame.
 *
 * "RMNM", not the "MMRN" one might expect from the datasheet's MIC1/2/3/4
 * ordering, and this is measured rather than assumed. Two facts fix it: a 32-bit
 * I2S word arrives MSB-first and stores little-endian, so each pair of 16-bit
 * slots swaps in memory; and the netlist AC-couples MIC4 to AGND, giving a
 * permanently dead lane that identifies the ordering rather than leaving it a
 * guess.
 *
 * Measured peaks, idle vs playback: lane 0 went 2-3 -> 7358-9359 with no noise
 * floor at all (a lane that cannot hear the room is not a microphone -- that is
 * the reference), lanes 1 and 3 went 30-334 -> 537-12353 (the two MEMS mics), and
 * lane 2 never left 3-9 in any condition (the grounded MIC4).
 *
 * Getting this wrong hands the AFE a microphone as its reference, which fails in
 * a confusing way rather than an obvious one: the filter never converges and the
 * echo simply stays.
 */
#define AFE_INPUT_FORMAT "RMNM"

/* Frames the ES7210 delivers per channel, per feed. Set by the AFE. */
static size_t s_feed_frames;

static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static audio_aec_out_t s_on_output;

/*
 * Drains the AFE.
 *
 * A separate task because feed and fetch are a producer/consumer pair -- the AFE
 * also runs its own internal task between them -- and calling both from the
 * capture task would deadlock the moment the ring buffer filled.
 *
 * Core 0: the capture and playback tasks already share core 1 with LVGL, and the
 * echo canceller is the most arithmetic-heavy thing in the firmware. Priority
 * below the audio tasks, because a late cancellation costs a frame of echo while
 * a late codec read costs audio outright.
 */
static void fetch_task(void *arg)
{
    while (1) {
        afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
        if (res == NULL || res->data_size <= 0) {
            /* AFE returns NULL on internal error; do not spin on it. */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (s_on_output != NULL) {
            s_on_output(res->data, (size_t)res->data_size / sizeof(int16_t));
        }

        /* So the 8 kB above is a measured number rather than a guess. */
        static uint32_t n;
        if ((++n % 500) == 0) {
            ESP_LOGI(TAG, "fetch stack high water: %u B",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
    }
}

esp_err_t audio_aec_start(audio_aec_out_t on_output)
{
    s_on_output = on_output;

    /*
     * models = NULL deliberately. Every model-based algorithm stays off --
     * wakenet, the NS model, the VAD model -- because this project has no `model`
     * partition to load them from, and esp_srmodel_init() would have nothing to
     * read. AEC itself needs no model.
     *
     * AFE_TYPE_VC, not AFE_TYPE_SR: the header describes VC as voice
     * communication at 16 kHz including nonlinear noise suppression, which is
     * exactly this -- audio destined for a remote listener rather than for a
     * local wake-word engine.
     */
    afe_config_t *cfg = afe_config_init(AFE_INPUT_FORMAT, NULL, AFE_TYPE_VC,
                                        AFE_MODE_LOW_COST);
    if (cfg == NULL) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return ESP_FAIL;
    }

    cfg->aec_init = true;
    /*
     * LOW_COST, not the VOIP_HIGH_PERF the defaults pick.
     *
     * Measured: high-perf took the largest free internal block from 63,488 B to
     * 3,840 B -- about 60 kB, despite AFE_MEMORY_ALLOC_MORE_PSRAM and a 6-frame
     * ring. That left too little for even the capture task's own 4 kB stack, so
     * nothing worked at all. Internal RAM is the binding resource on this board
     * and the display already holds 29,824 B of it.
     */
    cfg->aec_mode = AEC_MODE_VOIP_LOW_COST;

    /*
     * PSRAM, and this is not optional here.
     *
     * The AFE's default allocation is internal-leaning, and internal RAM is the
     * binding resource on this board -- it is shared with Wi-Fi, lwIP and TLS,
     * the display already holds 29,824 B of it, and the 32-bit TDM path costs
     * another ~10 kB. With the default, the AFE came up correctly and then task
     * creation failed for want of a 4 kB stack, which is a confusing way to
     * discover an out-of-memory condition.
     *
     * The device has 8 MB of octal PSRAM at 80 MHz and the AFE's buffers are
     * streamed sequentially, so they cache well.
     */
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    /*
     * The default ring buffer is 50 frames. At 256 samples x 4 channels x 2 bytes
     * that is over 100 kB of buffering for audio we consume immediately -- the
     * fetch task drains continuously and the capture task feeds in 1280-frame
     * blocks, so five frames of slack is already generous.
     *
     * This is the difference between the AFE fitting and not fitting.
     */
    cfg->afe_ringbuf_size = 6;

    /* Off because they need models we cannot load, or because Deepgram already
     * does the job better with the whole utterance in hand. */
    cfg->wakenet_init = false;
    cfg->vad_init = false;   /* Deepgram does turn detection server-side */
    cfg->ns_init = false;    /* VC type already applies nonlinear suppression */
    cfg->agc_init = false;   /* would fight the level calibration in ui.c */

    /*
     * Let the AFE settle any remaining conflicts itself. It documents that it
     * will modify parameters to remove contradictions between algorithms, which
     * is preferable to us guessing at the interactions.
     */
    cfg = afe_config_check(cfg);
    afe_config_print(cfg);

    s_afe = esp_afe_handle_from_config(cfg);
    if (s_afe == NULL) {
        ESP_LOGE(TAG, "no AFE implementation for this config");
        afe_config_free(cfg);
        return ESP_FAIL;
    }

    /* Measured across create, because "the AFE came up and then a 4 kB task
     * allocation failed" is a confusing way to report running out of memory. */
    size_t before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    s_afe_data = s_afe->create_from_config(cfg);
    size_t after = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "internal largest-free across AFE create: %u -> %u B",
             (unsigned)before, (unsigned)after);
    afe_config_free(cfg);
    if (s_afe_data == NULL) {
        ESP_LOGE(TAG, "AFE create failed");
        return ESP_FAIL;
    }

    s_feed_frames = (size_t)s_afe->get_feed_chunksize(s_afe_data);
    int feed_ch = s_afe->get_feed_channel_num(s_afe_data);
    int fetch_ch = s_afe->get_fetch_channel_num(s_afe_data);
    ESP_LOGI(TAG, "AFE up: feed %u frames x %d ch, fetch %d ch, format %s",
             (unsigned)s_feed_frames, feed_ch, fetch_ch, AFE_INPUT_FORMAT);

    if (feed_ch != 4) {
        /* The format string and the codec's frame have to agree, or every feed
         * misinterprets the interleaving. */
        ESP_LOGE(TAG, "AFE wants %d channels, the ES7210 frame carries 4", feed_ch);
        return ESP_FAIL;
    }

    /* 8 kB for the same reason the capture task needed it: the AFE runs work on
     * whichever stack calls into it, and 4 kB is not enough. */
    BaseType_t ok = xTaskCreatePinnedToCore(fetch_task, "aec_fetch", 8192, NULL,
                                           5, NULL, 0);
    if (ok != pdPASS) {
        /* Named explicitly: the AFE reports success and then this fails, which
         * without a message reads as the AFE having failed. */
        ESP_LOGE(TAG, "no memory for the fetch task -- internal RAM exhausted");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void audio_aec_feed(const int16_t *interleaved, size_t frames)
{
    if (s_afe_data == NULL || s_feed_frames == 0) {
        return;
    }
    /*
     * The AFE takes exactly its chunk size per call, so a block that is not a
     * whole multiple leaves a remainder. The caller reads in multiples of the
     * chunk size to avoid that; anything left over is dropped rather than
     * misaligning every subsequent frame, which is the same reasoning as the
     * carry bytes in audio_io.c.
     */
    size_t whole = frames / s_feed_frames;
    for (size_t i = 0; i < whole; i++) {
        s_afe->feed(s_afe_data, &interleaved[i * s_feed_frames * 4]);
    }
}

size_t audio_aec_feed_frames(void)
{
    return s_feed_frames;
}
