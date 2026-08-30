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

/*
 * Runs in whatever context ran out of memory -- possibly an ISR, possibly with a
 * lock held. ESP_DRAM_LOGE is the ISR-safe form; it writes straight to UART
 * rather than going through the log queue, which is what makes it usable here.
 *
 * `function_name` is the allocator entry point, not the caller, so it says
 * "malloc" more often than anything useful. The SIZE and the CAPS are the
 * informative part: a 16 kB MALLOC_CAP_INTERNAL|DMA request names mbedTLS's
 * record buffer about as clearly as a backtrace would.
 */
static void IRAM_ATTR on_alloc_failed(size_t size, uint32_t caps, const char *function_name)
{
    ESP_DRAM_LOGE(TAG, "ALLOC FAILED: %u bytes caps=0x%08x from %s | free=%u largest=%u",
                  (unsigned)size, (unsigned)caps,
                  function_name ? function_name : "?",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
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
    uint32_t reported_free = UINT32_MAX;

    for (;;) {
        uint32_t free_now = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        uint32_t largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

        if (free_now < s_min_free) {
            s_min_free = free_now;
            s_min_largest = largest;
        }

        /* Report on the way DOWN only. A new floor is the interesting event; the
         * recovery afterwards is not, and logging both doubles the noise. */
        if (free_now + REPORT_STEP < reported_free) {
            reported_free = free_now;
            ESP_LOGW(TAG, "floor %.2f s: free=%u largest=%u",
                     (double)esp_timer_get_time() / 1000000.0,
                     (unsigned)free_now, (unsigned)largest);
        } else if (free_now > reported_free + (REPORT_STEP * 4)) {
            /* Rearm well above the last floor, so a single noisy sample cannot
             * start a chatter of report/rearm/report. */
            reported_free = free_now;
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

    ESP_LOGI(TAG, "armed: failure hook + %d ms sampler (free=%u largest=%u)",
             SAMPLE_MS,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

#else /* !CONFIG_HEAP_PROBE */

/* Default off, and off means absent: no hook, no task, no 2,560 B stack. The
 * empty definition keeps the call site in app_main unconditional. */
void heap_probe_start(void)
{
}

#endif
