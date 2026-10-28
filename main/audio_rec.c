#include "audio_rec.h"

#include "sdkconfig.h"

#if CONFIG_AUDIO_RECORD

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#include "dg_agent.h"

static const char *TAG = "audio_rec";

#define FLAG_PLAYBACK 0x01
#define FLAG_SENT     0x02

typedef enum {
    REC_IDLE,    /* waiting for a session */
    REC_ARMED,   /* capture opened; the next block starts the recording */
    REC_ON,      /* recording */
    REC_DONE,    /* stopped; the dump task owns the buffers until it is IDLE */
} rec_state_t;

static int16_t *s_raw;    /* PSRAM, s_cap blocks of s_frames samples */
static int16_t *s_sent;
static uint8_t *s_flags;
static size_t s_frames;
static int s_cap;

static volatile rec_state_t s_state;
static volatile int s_n;           /* blocks recorded; capture task writes */
static int s_cur = -1;             /* capture task only */
static int64_t s_start_us;
static TaskHandle_t s_dump_task;
/*
 * A session that opens while the previous recording is still being printed.
 * The buffers are the dump's until it finishes, so that session cannot start
 * recording on time -- but it should not be lost either, which is what happened
 * on the bench: a tap a second before the boot session's dump ended went
 * unrecorded. It starts recording the moment the dump hands the buffers back,
 * and its header says how late that was.
 */
static volatile bool s_arm_pending;
static volatile bool s_capture_on;

static void finish(void)
{
    s_state = REC_DONE;
    if (s_dump_task != NULL) {
        xTaskNotifyGive(s_dump_task);
    }
}

void audio_rec_capture(bool enabled)
{
    s_capture_on = enabled;
    if (enabled) {
        if (s_state == REC_IDLE && s_raw != NULL) {
            s_state = REC_ARMED;
        } else if (s_state == REC_DONE) {
            s_arm_pending = true;    /* see s_arm_pending */
        }
    } else {
        s_arm_pending = false;
        if (s_state == REC_ON) {
            finish();
        } else if (s_state == REC_ARMED) {
            s_state = REC_IDLE;
        }
    }
}

void audio_rec_raw(const int16_t *mono, bool playback)
{
    s_cur = -1;
    if (s_state == REC_ARMED) {
        s_n = 0;
        s_start_us = esp_timer_get_time();
        s_state = REC_ON;
    }
    if (s_state != REC_ON) {
        return;
    }
    if (s_n >= s_cap) {
        finish();
        return;
    }
    const int slot = s_n;
    memcpy(s_raw + (size_t)slot * s_frames, mono, s_frames * sizeof(int16_t));
    memset(s_sent + (size_t)slot * s_frames, 0, s_frames * sizeof(int16_t));
    s_flags[slot] = playback ? FLAG_PLAYBACK : 0;
    s_cur = slot;
    s_n = slot + 1;
}

void audio_rec_sent(const int16_t *mono)
{
    if (s_cur < 0) {
        return;
    }
    memcpy(s_sent + (size_t)s_cur * s_frames, mono, s_frames * sizeof(int16_t));
    s_flags[s_cur] |= FLAG_SENT;
}

/*
 * Prints the recording once a session has ended. Low priority, and it yields
 * between lines: the console is shared, and a TLM line or a log from the audio
 * path should get through between blocks rather than wait out the whole dump.
 * Each line carries its block number so host/rec2wav.py can tell a dropped line
 * from a short recording.
 */
static void dump_task(void *arg)
{
    (void)arg;
    const size_t bytes = s_frames * sizeof(int16_t);
    const size_t b64 = 4 * ((bytes + 2) / 3) + 1;
    char *a = heap_caps_malloc(b64, MALLOC_CAP_SPIRAM);
    char *b = heap_caps_malloc(b64, MALLOC_CAP_SPIRAM);
    if (a == NULL || b == NULL) {
        ESP_LOGE(TAG, "no memory to dump with; recording disabled");
        vTaskDelete(NULL);
    }

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_state != REC_DONE) {
            continue;
        }
        int64_t welcome_us = 0;
        const char *rid = dg_agent_request_id(&welcome_us);
        const int n = s_n;
        ESP_LOGI(TAG, "dumping %d blocks (%.1f s) for request_id=%s", n,
                 n * (double)s_frames / 16000.0, rid);
        printf("\nREC begin rid=%s blocks=%d frames=%u rate=16000 start_us=%lld "
               "welcome_us=%lld\n",
               rid, n, (unsigned)s_frames, (long long)s_start_us, (long long)welcome_us);
        for (int i = 0; i < n; i++) {
            size_t la = 0, lb = 0;
            mbedtls_base64_encode((unsigned char *)a, b64, &la,
                                  (const unsigned char *)(s_raw + (size_t)i * s_frames), bytes);
            mbedtls_base64_encode((unsigned char *)b, b64, &lb,
                                  (const unsigned char *)(s_sent + (size_t)i * s_frames), bytes);
            a[la] = '\0';
            b[lb] = '\0';
            printf("REC %d %u %s %s\n", i, (unsigned)s_flags[i], a, b);
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        printf("REC end blocks=%d\n", n);
        ESP_LOGI(TAG, "dump done");
        if (s_arm_pending && s_capture_on) {
            s_arm_pending = false;
            ESP_LOGW(TAG, "a session opened during the dump; recording it from now "
                          "(its start is missing)");
            s_state = REC_ARMED;
        } else {
            s_arm_pending = false;
            s_state = REC_IDLE;
        }
    }
}

void audio_rec_init(size_t frames_per_block)
{
    s_frames = frames_per_block;
    s_cap = (int)((int64_t)CONFIG_AUDIO_RECORD_SECONDS * 16000 / (int64_t)frames_per_block);
    const size_t plane = (size_t)s_cap * s_frames * sizeof(int16_t);
    s_raw = heap_caps_malloc(plane, MALLOC_CAP_SPIRAM);
    s_sent = heap_caps_malloc(plane, MALLOC_CAP_SPIRAM);
    s_flags = heap_caps_malloc((size_t)s_cap, MALLOC_CAP_SPIRAM);
    if (s_raw == NULL || s_sent == NULL || s_flags == NULL) {
        ESP_LOGE(TAG, "no PSRAM for %d s of recording (%u B); disabled",
                 CONFIG_AUDIO_RECORD_SECONDS, (unsigned)(2 * plane));
        free(s_raw);
        free(s_sent);
        free(s_flags);
        s_raw = s_sent = NULL;
        s_flags = NULL;
        return;
    }
    /* Core 0, priority 1, beside the battery sampler: never in the audio path's
     * way, and the dump only runs between sessions anyway. */
    if (xTaskCreatePinnedToCore(dump_task, "rec_dump", 3072, NULL, 1, &s_dump_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "dump task did not start; recording disabled");
        s_state = REC_DONE;   /* never arms */
        return;
    }
    ESP_LOGW(TAG, "RECORDING ENABLED: each session's first %d s of microphone audio "
                  "(%u B PSRAM), dumped to the console when it ends",
             CONFIG_AUDIO_RECORD_SECONDS, (unsigned)(2 * plane));
}

#else /* !CONFIG_AUDIO_RECORD */

void audio_rec_init(size_t frames_per_block) { (void)frames_per_block; }
void audio_rec_capture(bool enabled) { (void)enabled; }
void audio_rec_raw(const int16_t *mono, bool playback)
{
    (void)mono;
    (void)playback;
}
void audio_rec_sent(const int16_t *mono) { (void)mono; }

#endif
