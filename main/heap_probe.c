#include "heap_probe.h"

#include "sdkconfig.h"

#if CONFIG_HEAP_PROBE

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "heap_probe";

/* How often the sampler looks. The TLM line's 1 s is what hid the shape of the
 * collapse; 50 ms is 20 samples across the two seconds it took. */
#define SAMPLE_MS 50

/* Only report when the floor moves by more than this, or every line would be a
 * line. 2 kB is well above the churn seen at steady state (44-47 kB). */
#define REPORT_STEP 2048

static volatile uint32_t s_min_free = UINT32_MAX;
static volatile uint32_t s_min_largest = UINT32_MAX;
/* The one that decides whether esp-aes can bounce a TLS record. See the note in
 * sampler_task() for why this is the tracked figure and internal free is not. */
static volatile uint32_t s_min_dma_largest = UINT32_MAX;

/*
 * Runs in whatever context ran out of memory -- possibly an ISR, possibly with a
 * lock held. ESP_DRAM_LOGE is the ISR-safe form; it writes straight to UART
 * rather than going through the log queue, which is what makes it usable here.
 *
 * `function_name` is the allocator entry point, not the caller, so it says
 * "malloc" more often than anything useful. The SIZE and the CAPS are the
 * informative part.
 *
 * BOTH POOLS, and that is the whole point of this build. MALLOC_CAP_DMA is a
 * STRICT SUBSET of MALLOC_CAP_INTERNAL -- not every internal region can be
 * reached by the DMA engine -- so a device reporting 7 kB of internal largest
 * block can have far less than that available to a DMA request. Every number in
 * this project's telemetry so far has been the INTERNAL one, and the allocation
 * that actually fails here is
 *
 *     heap_caps_aligned_alloc(align, <=1600, MALLOC_CAP_DMA)
 *
 * from esp_aes_dma_core.c, twice per TLS record. Reporting only the internal
 * figure is how "largest=7680 yet a 1600 B request failed" looked like a
 * contradiction rather than a measurement of the wrong pool.
 */
static void IRAM_ATTR on_alloc_failed(size_t size, uint32_t caps, const char *function_name)
{
    ESP_DRAM_LOGE(TAG,
                  "ALLOC FAILED: %u bytes caps=0x%08x from %s | "
                  "int free=%u largest=%u | dma free=%u largest=%u",
                  (unsigned)size, (unsigned)caps,
                  function_name ? function_name : "?",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

/*
 * Deliberately allocation-free: it only reads counters. The last three
 * instruments added to this firmware each broke something -- two watchdog trips
 * and a callback registered after i2s_channel_enable that silently reported zero
 * -- so this one yields on every iteration and touches nothing.
 */
static void sampler_task(void *arg)
{
    (void)arg;
    uint32_t reported = UINT32_MAX;

    for (;;) {
        multi_heap_info_t info;
        heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);

        const uint32_t int_free = (uint32_t)info.total_free_bytes;
        const uint32_t int_largest = (uint32_t)info.largest_free_block;
        const uint32_t blocks = (uint32_t)info.free_blocks;
        const uint32_t dma_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
        const uint32_t dma_largest =
            (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);

        if (dma_largest < s_min_dma_largest) {
            s_min_dma_largest = dma_largest;
        }
        if (int_free < s_min_free) {
            s_min_free = int_free;
            s_min_largest = int_largest;
        }

        /*
         * TRACKED ON THE DMA LARGEST BLOCK, not on internal free bytes. The
         * question this build exists to answer is whether a <=1600 B
         * MALLOC_CAP_DMA request can be satisfied, and free-bytes-anywhere does
         * not answer it: the arena can hold 50 kB in pieces too small or too
         * far from the DMA engine to serve one.
         *
         * free_blocks comes along because it separates the two causes. A floor
         * arriving with the block count RISING is fragmentation -- the same
         * bytes cut into more pieces. Flat, with one more allocation
         * outstanding, it is a single large claim.
         */
        if (dma_largest + REPORT_STEP < reported) {
            reported = dma_largest;
            ESP_LOGW(TAG, "floor %.2f s: dma largest=%u free=%u | "
                          "int largest=%u free=%u blocks=%u",
                     (double)esp_timer_get_time() / 1000000.0,
                     (unsigned)dma_largest, (unsigned)dma_free,
                     (unsigned)int_largest, (unsigned)int_free, (unsigned)blocks);
        } else if (dma_largest > reported + (REPORT_STEP * 4)) {
            /* Rearm well above the last floor, so a single noisy sample cannot
             * start a chatter of report/rearm/report. */
            reported = dma_largest;
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    }
}

void heap_probe_start(void)
{
    esp_err_t err = heap_caps_register_failed_alloc_callback(on_alloc_failed);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not register the failure hook: %s", esp_err_to_name(err));
    }

    /* Low priority: this must never be the reason something else is late. Core 0
     * to keep it off the display's core. */
    if (xTaskCreatePinnedToCore(sampler_task, "heap_probe", 2560, NULL, 1, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "sampler task did not start");
        return;
    }

    /*
     * The baseline both pools start from. Worth having on its own line: the gap
     * between the internal figure and the DMA one, right here at boot before
     * anything has fragmented, is the number that says how much of "free
     * internal RAM" was ever available to a DMA request in the first place.
     */
    ESP_LOGI(TAG, "armed: failure hook + %d ms sampler | "
                  "int free=%u largest=%u | dma free=%u largest=%u",
             SAMPLE_MS,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

#else /* !CONFIG_HEAP_PROBE */

/* Default off, and off means absent: no hook, no task, no 2,560 B stack. The
 * empty definition keeps the call site in app_main unconditional. */
void heap_probe_start(void)
{
}

#endif
