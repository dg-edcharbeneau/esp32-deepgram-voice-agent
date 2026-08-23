/*
 * esp-sr AFE wiring. See audio_aec.h for why this exists.
 *
 * WHY EVERYTHING RUNS ON ONE TASK HERE
 *
 * Neither esp-sr library creates a task of its own: libesp_audio_front_end.a and
 * libesp_audio_processor.a reference no xTaskCreate at all, only
 * heap_caps_aligned_alloc, malloc and vTaskDelay. And with a single microphone the
 * AFE deactivates its SE stage outright ("For single microphone channel, SE is
 * deactivated"). So there is no internal AFE task, afe_perferred_core and
 * afe_perferred_priority are inert, and every bit of AEC arithmetic runs inline on
 * whichever task calls feed() or fetch().
 *
 * That is why the first attempt failed. Feeding from the codec-read task put the
 * heaviest DSP in the firmware on audio_cap -- priority 7, core 1, sharing a core
 * with playback and LVGL -- where it overflowed a 4 kB stack, and would have
 * starved both even if it had fitted.
 *
 * So the pipeline gets its own task on core 0, and the codec task goes back to
 * doing nothing but reading the codec.
 */

#include "audio_aec.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_aec.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "audio_aec";

/*
 * Channel layout handed to the AFE -- which is NOT the hardware's layout.
 *
 * M = microphone, R = playback reference, N = unused. Position in this string is
 * position in the interleaved frame.
 *
 * THE REFERENCE MUST BE LAST. esp_afe_sr_iface.h's feed() carries the warning
 * "The last channel is reference signal if it has reference data", and the
 * implementation assumes it regardless of what input_format says. afe_config_init
 * happily accepted "RMNM" and even reported back "1 microphone, 1 playback", but
 * every feed() then failed and every fetch() returned ret_value -1 with an empty
 * ring -- a silent failure, because feeding kept appearing to succeed.
 *
 * The ES7210's frame arrives as [R, M, N, M] in memory, measured:
 *
 *   lane 0 ... 2-3 idle, 7358-9359 on playback, no noise floor at all -- a lane
 *              that cannot hear the room is not a microphone, so this is the
 *              reference
 *   lane 1 ... 30-334 idle, 537-12353 on playback -- a MEMS mic
 *   lane 2 ... 3-9 in every condition -- MIC4, which the netlist AC-couples to
 *              AGND, and the dead lane is what fixes the ordering rather than
 *              leaving it a guess
 *   lane 3 ... tracks lane 1 -- the other MEMS mic
 *
 * That order is not the datasheet's MIC1/2/3/4 either: a 32-bit I2S word arrives
 * MSB-first and stores little-endian, so each pair of 16-bit slots swaps.
 *
 * So audio_aec_feed() gathers [1, 3, 2, 0] into [M, M, N, R] as it copies, which
 * costs nothing because the copy was happening anyway.
 */
#define AFE_INPUT_FORMAT "MMNR"

/* Channels per frame in the ES7210's TDM output. */
#define AEC_CHANNELS 4

/*
 * Two blocks is enough: the codec task produces one per 80 ms and the AEC task
 * consumes it well inside that, so the second only covers jitter. In PSRAM
 * because internal RAM is the binding resource on this board and these are
 * streamed sequentially, which caches well.
 */
#define AEC_BLOCKS 2

typedef struct {
    int16_t *data;
    size_t frames;
} aec_block_t;

static aec_block_t s_blocks[AEC_BLOCKS];
static QueueHandle_t s_free;  /* blocks the codec task may write into */
static QueueHandle_t s_ready; /* blocks waiting to be processed */

static size_t s_feed_frames;
static size_t s_block_frames;

static uint32_t s_dbg_blocks, s_dbg_feeds, s_dbg_fetches, s_dbg_timeouts;
static uint32_t s_dbg_empty;
static int s_dbg_ret;
static float s_dbg_ringfree;

static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static audio_aec_out_t s_on_output;

/*
 * The whole pipeline: dequeue, feed, drain, hand on.
 *
 * Core 0, away from the audio tasks and LVGL on core 1. Priority above
 * session_ctl's 4 so a WebSocket reconnect cannot stall cancellation, but this is
 * the only heavy consumer on its core either way.
 *
 * feed and fetch on the SAME task is safe and deliberate: fetch_with_delay(0)
 * polls rather than blocking, so a fed block is drained immediately and the two
 * never wait on each other. Splitting them across tasks -- the first attempt --
 * bought nothing, because the library does the work wherever it is called from.
 */
static void aec_task(void *arg)
{
    while (1) {
        aec_block_t *blk = NULL;
        if (xQueueReceive(s_ready, &blk, portMAX_DELAY) != pdTRUE || blk == NULL) {
            continue;
        }

        s_dbg_blocks++;
        size_t chunks = blk->frames / s_feed_frames;
        for (size_t i = 0; i < chunks; i++) {
            s_afe->feed(s_afe_data, &blk->data[i * s_feed_frames * AEC_CHANNELS]);
            s_dbg_feeds++;

            /*
             * One fetch per feed. The AFE is a chunk-in/chunk-out pipeline, so
             * pairing them keeps its 6-frame ring from filling and stalling.
             *
             * The timeout is NOT zero. A zero-tick poll returns NULL before the
             * AFE has finished the chunk it was just handed, so the drain broke
             * immediately every time and no audio ever reached the sink -- silent,
             * because feeding kept succeeding. 50 ms is generous against a 16 ms
             * chunk while still bounded, so a wedged AFE cannot hang this task.
             */
            afe_fetch_result_t *res =
                s_afe->fetch_with_delay(s_afe_data, pdMS_TO_TICKS(50));
            if (res != NULL && res->data_size > 0 && s_on_output != NULL) {
                s_dbg_fetches++;
                s_on_output(res->data, (size_t)res->data_size / sizeof(int16_t));
            } else if (res == NULL) {
                s_dbg_timeouts++;
                ESP_LOGW(TAG, "fetch timed out with a chunk in flight");
            } else {
                /*
                 * Non-NULL but empty. This was the silent failure: neither branch
                 * above fired, so the counters read fetches=0 timeouts=0 and it
                 * looked like fetch was never called at all. ret_value is the
                 * AFE's own explanation, and ringbuff_free_pct says whether the
                 * pipeline is backed up.
                 */
                s_dbg_empty++;
                s_dbg_ret = res->ret_value;
                s_dbg_ringfree = res->ringbuff_free_pct;
            }
        }

        xQueueSend(s_free, &blk, 0);

        /* So the stack size is a measured number rather than a guess. */
        static uint32_t n;
        if ((++n % 25) == 0) {
            /* Counters, because "no output" has several possible causes and they
             * are indistinguishable from the absence of a log line. */
            ESP_LOGI(TAG, "blocks=%u feeds=%u fetches=%u timeouts=%u empty=%u "
                          "ret=%d ringfree=%.2f int=%u",
                     (unsigned)s_dbg_blocks, (unsigned)s_dbg_feeds,
                     (unsigned)s_dbg_fetches, (unsigned)s_dbg_timeouts,
                     (unsigned)s_dbg_empty, s_dbg_ret, s_dbg_ringfree,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        }
    }
}

esp_err_t audio_aec_start(audio_aec_out_t on_output, size_t block_frames)
{
    s_on_output = on_output;
    s_block_frames = block_frames;

    /*
     * models = NULL deliberately. Every model-based algorithm stays off --
     * wakenet, the NS model, the VAD model -- because this project has no `model`
     * partition to load them from and esp_srmodel_init() would have nothing to
     * read. AEC itself needs no model.
     *
     * AFE_TYPE_VC, not AFE_TYPE_SR: the header describes VC as voice
     * communication at 16 kHz including nonlinear noise suppression, which is
     * exactly this -- audio destined for a remote listener rather than a local
     * wake-word engine.
     */
    afe_config_t *cfg = afe_config_init(AFE_INPUT_FORMAT, NULL, AFE_TYPE_VC,
                                        AFE_MODE_LOW_COST);
    if (cfg == NULL) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return ESP_FAIL;
    }

    cfg->aec_init = true;

    /*
     * All three of these were measured, and all three are needed to fit. Internal
     * RAM is the binding resource: the display holds 29,824 B of it and the 32-bit
     * TDM path costs ~10 kB more.
     *
     *   aec_mode LOW_COST      the default VOIP_HIGH_PERF took the largest free
     *                          internal block from 63,488 B to 3,840 B -- less
     *                          than a task stack -- so nothing ran at all.
     *                          LOW_COST takes ~28 kB and leaves 34,816 B.
     *   MORE_PSRAM             the default allocation leans internal.
     *   ringbuf_size 6         the default 50 buffers over 100 kB of audio that
     *                          this task consumes immediately.
     */
    cfg->aec_mode = AEC_MODE_VOIP_LOW_COST;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_ringbuf_size = 6;

    /* Off because they need models we cannot load, or because Deepgram already
     * does the job better with the whole utterance in hand. */
    cfg->wakenet_init = false;
    cfg->vad_init = false; /* Deepgram does turn detection server-side */
    cfg->ns_init = false;  /* VC type already applies nonlinear suppression */
    cfg->agc_init = false; /* would fight the level calibration in ui.c */

    /* Let the AFE settle any remaining conflicts itself -- it documents that it
     * will modify parameters to remove contradictions between algorithms, which
     * beats us guessing at the interactions. */
    cfg = afe_config_check(cfg);

    s_afe = esp_afe_handle_from_config(cfg);
    if (s_afe == NULL) {
        ESP_LOGE(TAG, "no AFE implementation for this config");
        afe_config_free(cfg);
        return ESP_FAIL;
    }

    /*
     * TOTAL free, not largest-block. Measuring the largest block across create
     * said "about 28 kB" (63,488 -> 34,816) and that was simply the wrong metric
     * -- largest-block moves when the arena is carved, not by the amount consumed.
     * The total is what says whether the rest of the firmware still has room.
     */
    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t big_before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    s_afe_data = s_afe->create_from_config(cfg);
    afe_config_free(cfg);
    if (s_afe_data == NULL) {
        ESP_LOGE(TAG, "AFE create failed");
        return ESP_FAIL;
    }
    size_t free_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGW(TAG, "AFE internal cost: free %u -> %u B (consumed %u), "
                  "largest %u -> %u B",
             (unsigned)free_before, (unsigned)free_after,
             (unsigned)(free_before - free_after), (unsigned)big_before,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    s_feed_frames = (size_t)s_afe->get_feed_chunksize(s_afe_data);
    int feed_ch = s_afe->get_feed_channel_num(s_afe_data);
    ESP_LOGI(TAG, "AFE up: feed %u frames x %d ch, format %s",
             (unsigned)s_feed_frames, feed_ch, AFE_INPUT_FORMAT);

    if (feed_ch != AEC_CHANNELS) {
        ESP_LOGE(TAG, "AFE wants %d channels, the ES7210 frame carries %d",
                 feed_ch, AEC_CHANNELS);
        return ESP_FAIL;
    }
    if (s_feed_frames == 0 || (block_frames % s_feed_frames) != 0) {
        /* A block that is not a whole number of chunks would leave a remainder
         * every time, and dropping it would slide the channel interleaving --
         * the same hazard as the carry bytes in audio_io.c, one frame wider. */
        ESP_LOGE(TAG, "block of %u frames is not a multiple of the AFE's %u",
                 (unsigned)block_frames, (unsigned)s_feed_frames);
        return ESP_FAIL;
    }

    s_free = xQueueCreate(AEC_BLOCKS, sizeof(aec_block_t *));
    s_ready = xQueueCreate(AEC_BLOCKS, sizeof(aec_block_t *));
    if (s_free == NULL || s_ready == NULL) {
        ESP_LOGE(TAG, "no memory for the block queues");
        return ESP_FAIL;
    }

    const size_t block_bytes = block_frames * AEC_CHANNELS * sizeof(int16_t);
    for (int i = 0; i < AEC_BLOCKS; i++) {
        s_blocks[i].data = heap_caps_malloc(block_bytes, MALLOC_CAP_SPIRAM);
        s_blocks[i].frames = 0;
        if (s_blocks[i].data == NULL) {
            ESP_LOGE(TAG, "no PSRAM for a %u byte block", (unsigned)block_bytes);
            return ESP_ERR_NO_MEM;
        }
        aec_block_t *p = &s_blocks[i];
        xQueueSend(s_free, &p, 0);
    }

    /*
     * Stack in PSRAM, and 16 kB of it.
     *
     * The AFE runs its filtering on this stack -- 4 kB overflowed immediately when
     * the codec task was doing the feeding -- so it has to be generous. But taking
     * 16 kB of INTERNAL RAM starved the next thing to ask for any:
     * session_ctl_start() failed to create its own 5 kB task and, being wrapped in
     * ESP_ERROR_CHECK, aborted the boot. The backtrace pointed at main.c's
     * session_ctl line, which reads as a session bug and is really this.
     *
     * CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY is already enabled, and PSRAM is
     * the resource this board has 8 MB of. The trade is a slower stack for the
     * filtering; whether that matters is measurable from the frame timings and the
     * high-water mark logged in aec_task().
     */
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(aec_task, "aec", 16384, NULL, 5,
                                                    NULL, 0, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "no memory for the AEC task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void audio_aec_feed(const int16_t *interleaved, size_t frames)
{
    if (s_free == NULL || frames != s_block_frames) {
        return;
    }

    /*
     * Non-blocking, and it DROPS when the AEC task is behind rather than waiting.
     * This runs on the codec task, where a stall costs audio outright; a dropped
     * block costs one frame of echo. Same trade the task priorities already encode.
     */
    aec_block_t *blk = NULL;
    if (xQueueReceive(s_free, &blk, 0) != pdTRUE || blk == NULL) {
        return;
    }

    /*
     * Gather rather than a straight copy: the hardware's [R, M, N, M] becomes the
     * [M, M, N, R] the AFE requires, because its feed() assumes the reference is
     * the last channel. See AFE_INPUT_FORMAT above.
     *
     * The copy was happening anyway -- 10 kB per block at 12.5 blocks a second is
     * ~125 kB/s, on the order of 100 us of every second -- so the reorder is free.
     */
    for (size_t i = 0; i < frames; i++) {
        const int16_t *in = &interleaved[i * AEC_CHANNELS];
        int16_t *out = &blk->data[i * AEC_CHANNELS];
        out[0] = in[1]; /* mic  */
        out[1] = in[3]; /* mic  */
        out[2] = in[2]; /* unused, kept so the frame width matches */
        out[3] = in[0]; /* reference, and it has to be here */
    }
    blk->frames = frames;
    if (xQueueSend(s_ready, &blk, 0) != pdTRUE) {
        xQueueSend(s_free, &blk, 0); /* put it back rather than leak it */
    }
}
