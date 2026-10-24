/*
 * See screenshot.h. Format, one capture:
 *
 *   SHOT begin n=<seq> w=466 h=466 fmt=rgb565rle tag=<tag>
 *   SHOT <line#> <base64>        (repeated)
 *   SHOT end n=<seq> lines=<count> runs=<count>
 *
 * The payload is runs of (uint16 count, uint16 pixel), little-endian, row-major
 * across the whole frame. A face is mostly black, so this is a tenth of the raw
 * 434 kB or less, and the serial link is the slow part.
 */
#include "screenshot.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

static const char *TAG = "shot";

#define MAX_W 466
#define MAX_H 466
#define RAW_PER_LINE 96   /* bytes of payload per printed line: 24 runs */

static uint16_t *s_buf;          /* PSRAM: the copied frame */
static int s_w, s_h;
static char s_tag[48];
static volatile bool s_busy;
static TaskHandle_t s_task;

static void dump_task(void *arg)
{
    (void)arg;
    static uint8_t raw[RAW_PER_LINE];
    static unsigned char b64[((RAW_PER_LINE + 2) / 3) * 4 + 1];
    uint32_t seq = 0;

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const int64_t t0 = esp_timer_get_time();
        printf("\nSHOT begin n=%u w=%d h=%d fmt=rgb565rle tag=%s\n", (unsigned)seq, s_w, s_h,
               s_tag);

        size_t fill = 0;
        uint32_t lines = 0, runs = 0;
        const size_t total = (size_t)s_w * s_h;
        size_t i = 0;
        while (i < total) {
            const uint16_t v = s_buf[i];
            uint32_t n = 1;
            while (i + n < total && n < 0xFFFF && s_buf[i + n] == v) {
                n++;
            }
            i += n;
            raw[fill++] = (uint8_t)(n & 0xFF);
            raw[fill++] = (uint8_t)(n >> 8);
            raw[fill++] = (uint8_t)(v & 0xFF);
            raw[fill++] = (uint8_t)(v >> 8);
            runs++;
            if (fill == RAW_PER_LINE || i >= total) {
                size_t olen = 0;
                mbedtls_base64_encode(b64, sizeof(b64), &olen, raw, fill);
                b64[olen] = '\0';
                printf("SHOT %u %s\n", (unsigned)lines, b64);
                lines++;
                fill = 0;
            }
        }
        printf("SHOT end n=%u lines=%u runs=%u\n", (unsigned)seq, (unsigned)lines, (unsigned)runs);
        fflush(stdout);
        ESP_LOGI(TAG, "EVT shot %u printed: %u runs in %lld ms", (unsigned)seq, (unsigned)runs,
                 (long long)((esp_timer_get_time() - t0) / 1000));
        seq++;
        s_busy = false;
    }
}

bool screenshot_init(void)
{
    s_buf = heap_caps_malloc((size_t)MAX_W * MAX_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (s_buf == NULL) {
        ESP_LOGE(TAG, "no PSRAM for a screen capture");
        return false;
    }
    /* Core 0 and the bottom priority: printing is slow and nothing waits on it
     * but the tour, which simply holds its step until the line is clear. */
    if (xTaskCreatePinnedToCore(dump_task, "shot_dump", 4096, NULL, 1, &s_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "screen capture task did not start");
        return false;
    }
    ESP_LOGW(TAG, "SCREEN CAPTURE ENABLED (CONFIG_UI_SCREENSHOT_TOUR): bench build");
    return true;
}

bool screenshot_busy(void)
{
    return s_busy;
}

bool screenshot_capture(const uint16_t *px, uint32_t stride_px, int w, int h, const char *tag)
{
    if (s_buf == NULL || s_busy || w > MAX_W || h > MAX_H) {
        return false;
    }
    for (int y = 0; y < h; y++) {
        memcpy(s_buf + (size_t)y * w, px + (size_t)y * stride_px, (size_t)w * sizeof(uint16_t));
    }
    s_w = w;
    s_h = h;
    strlcpy(s_tag, tag, sizeof(s_tag));
    s_busy = true;
    xTaskNotifyGive(s_task);
    return true;
}
