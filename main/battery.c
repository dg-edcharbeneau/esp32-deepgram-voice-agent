#include "battery.h"

#include "sdkconfig.h"

#include <string.h>

#if CONFIG_BATTERY

#include "bsp/esp32_s3_touch_amoled_1_75c.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "battery";

/* The AXP2101's fixed address. It shares SCL 14 / SDA 15 with touch, the codec
 * and the RTC, so this adds a device to the BSP's bus rather than making one. */
#define AXP2101_ADDR 0x34
#define AXP2101_HZ   400000

/*
 * The four registers this module reads, and nothing else.
 *
 * REG00 PMU status 1  -- bit 5 is VBUS (USB) present.
 * REG01 PMU status 2  -- bits 6:5 are the battery current direction:
 *                        00 standby, 01 charging, 10 discharging.
 * REG34/35 VBAT ADC   -- 14 bits across two registers, already in millivolts;
 *                        REG34 carries the high 6 bits.
 * REGA4 gauge percent -- 0-100 from the on-chip fuel gauge, 0xFF when it has
 *                        nothing to report.
 *
 * CONFIG_BATTERY_DUMP_REGS logs all of them once per sample so the first flash
 * on real hardware confirms these offsets against the board instead of against
 * a datasheet reading. Turn it off once the numbers have been seen to move.
 */
#define REG_STATUS1   0x00
#define REG_STATUS2   0x01
#define REG_VBAT_H    0x34
#define REG_GAUGE     0xA4

#define STATUS1_VBUS_GOOD   (1u << 5)
#define STATUS2_DIR_MASK    (3u << 5)
#define STATUS2_DIR_CHARGE  (1u << 5)

/* Median of five. The gauge is already filtered on-chip, but the dots quantise
 * to 25% steps, so a reading resting on a boundary would toggle a dot every
 * sample. Five samples at CONFIG_BATTERY_SAMPLE_MS is a slow filter on a slow
 * signal, which is the right trade here. */
#define MEDIAN_N 5

/* Do not move the reported percentage for less than this. Stops 49/50 flapping
 * from reaching either the screen or the log. */
#define REPORT_STEP_PCT 2

/* ...but a deadband alone installs a PERMANENT offset: measured on the bench,
 * the first sample landed on 59, the gauge then read 60 for the whole run, and
 * 60 never differed from 59 by enough to be accepted. The device said "59" while
 * the chip said 60, indefinitely. So a value that simply holds still is taken
 * exactly, however small the step -- the deadband is there to reject a signal
 * that is moving about, and a reading repeated this many times is not that. */
#define SETTLED_SAMPLES 3

/* Hysteresis on `low`: it asserts at LOW_PCT and clears five points above. */
#define LOW_CLEAR_MARGIN 5

static i2c_master_dev_handle_t s_dev;

/* Written only by the sampler, read by anyone. Each is a word, so a reader
 * either sees the old value or the new one; the struct is not a consistent
 * snapshot across fields and does not need to be -- these are four independent
 * slow signals, and a reader that catches a percentage one sample ahead of a
 * charging flag has nothing to get wrong. */
static volatile bool s_valid;
static volatile int  s_percent;
static volatile int  s_millivolts;
static volatile bool s_charging;
static volatile bool s_low;

static esp_err_t reg_read(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, len,
                                       pdMS_TO_TICKS(100));
}

static int median_of(const int *src, int n)
{
    int tmp[MEDIAN_N];
    memcpy(tmp, src, sizeof(int) * (size_t)n);
    /* Insertion sort. n is 5. */
    for (int i = 1; i < n; i++) {
        int v = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > v) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = v;
    }
    return tmp[n / 2];
}

/*
 * Deliberately allocation-free and short: it reads four registers, filters, and
 * yields. Everything expensive -- formatting, logging, drawing -- belongs to the
 * TLM loop and the frame timer, which are the two places this firmware already
 * agrees are allowed to be slow.
 */
static void sampler_task(void *arg)
{
    (void)arg;

    /* Zeroed because the shift below moves every slot, including ones not yet
     * written; only the first `filled` are ever read, but reading an
     * uninitialised int to write it back is not worth the argument. */
    int  history[MEDIAN_N] = {0};
    int  filled = 0;
    int  reported = -1;
    int  prev_pct = -1;
    int  settled = 0;

    for (;;) {
        uint8_t status1 = 0, status2 = 0, gauge = 0, vbat[2] = {0, 0};
        esp_err_t err = reg_read(REG_STATUS1, &status1, 1);
        if (err == ESP_OK) err = reg_read(REG_STATUS2, &status2, 1);
        if (err == ESP_OK) err = reg_read(REG_VBAT_H, vbat, sizeof(vbat));
        if (err == ESP_OK) err = reg_read(REG_GAUGE, &gauge, 1);

#if CONFIG_BATTERY_DUMP_REGS
        ESP_LOGI(TAG, "REG 00=%02x 01=%02x 34=%02x 35=%02x A4=%02x (%s)",
                 status1, status2, vbat[0], vbat[1], gauge, esp_err_to_name(err));
#endif

        if (err != ESP_OK) {
            /* A transient NACK on a bus three other drivers are using is not
             * news, and it must not blank an indicator that was right a second
             * ago. Keep the last good sample and try again. */
            ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(CONFIG_BATTERY_SAMPLE_MS));
            continue;
        }

        int mv = (int)(((uint16_t)(vbat[0] & 0x3F) << 8) | vbat[1]);
        bool charging = (status2 & STATUS2_DIR_MASK) == STATUS2_DIR_CHARGE;

        if (gauge > 100) {
            /* 0xFF means the gauge has no answer -- typically no cell attached,
             * which is exactly the case on a bench board running off USB. Report
             * "cannot read" rather than a number, and let the UI draw nothing. */
            s_valid = false;
            s_charging = charging;
            s_millivolts = mv;
            filled = 0;
            reported = -1;
            vTaskDelay(pdMS_TO_TICKS(CONFIG_BATTERY_SAMPLE_MS));
            continue;
        }

        /* Shift the window and take the middle. */
        for (int i = MEDIAN_N - 1; i > 0; i--) {
            history[i] = history[i - 1];
        }
        history[0] = gauge;
        if (filled < MEDIAN_N) {
            filled++;
        }
        int pct = median_of(history, filled);

        if (pct == prev_pct) {
            if (settled < SETTLED_SAMPLES) {
                settled++;
            }
        } else {
            settled = 1;
            prev_pct = pct;
        }

        if (reported < 0 || pct <= reported - REPORT_STEP_PCT ||
            pct >= reported + REPORT_STEP_PCT || settled >= SETTLED_SAMPLES) {
            reported = pct;
        }

        bool was_low = s_low;
        bool low = was_low ? (reported <= CONFIG_BATTERY_LOW_PCT + LOW_CLEAR_MARGIN)
                           : (reported <= CONFIG_BATTERY_LOW_PCT);

        bool announce = (!s_valid) || (low != was_low) || (charging != s_charging);

        s_percent = reported;
        s_millivolts = mv;
        s_charging = charging;
        s_low = low;
        s_valid = true;

        if (announce) {
            ESP_LOGI(TAG, "EVT battery -> pct=%d mv=%d chg=%d low=%d",
                     reported, mv, (int)charging, (int)low);
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_BATTERY_SAMPLE_MS));
    }
}

void battery_start(void)
{
    /* Idempotent, and calling it here rather than depending on audio_codecs.c
     * having run is the same reasoning the BSP's own entry points use. */
    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_i2c_init: %s", esp_err_to_name(err));
        return;
    }

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGE(TAG, "no I2C bus handle");
        return;
    }

    err = i2c_master_probe(bus, AXP2101_ADDR, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        /* Not fatal: the rest of the firmware does not depend on this, and a
         * board without the PMU responding should still run. */
        ESP_LOGE(TAG, "no AXP2101 at 0x%02x: %s", AXP2101_ADDR, esp_err_to_name(err));
        return;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = AXP2101_HZ,
    };
    err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_device: %s", esp_err_to_name(err));
        return;
    }

    /* Low priority on core 0, like heap_probe: a battery reading is never the
     * reason anything else should be late, and core 1 belongs to the display. */
    if (xTaskCreatePinnedToCore(sampler_task, "battery", 3072, NULL, 1, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "sampler task did not start");
        return;
    }

    ESP_LOGI(TAG, "AXP2101 at 0x%02x, sampling every %d ms",
             AXP2101_ADDR, CONFIG_BATTERY_SAMPLE_MS);
}

bool battery_get(battery_status_t *out)
{
    if (out == NULL) {
        return false;
    }
    out->valid = s_valid;
    out->percent = s_percent;
    out->millivolts = s_millivolts;
    out->charging = s_charging;
    out->low = s_low;
    return out->valid;
}

#else /* !CONFIG_BATTERY */

/* Off means absent: no task, no I2C device, no code. The empty definitions keep
 * every call site unconditional, the way heap_probe.c does it. */
void battery_start(void)
{
}

bool battery_get(battery_status_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return false;
}

#endif
