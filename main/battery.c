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
 * The registers this module reads, and nothing else. Offsets and field layouts
 * cross-checked against XPowersLib (src/XPowersAXP2101.hpp, src/XPowersParams.hpp,
 * src/REG/AXP2101Constants.h), which is the reference implementation for this
 * part, and confirmed against this board.
 *
 * REG00 PMU status 1  -- bit 5 is VBUS (USB) present.
 * REG01 PMU status 2  -- two separate fields:
 *                        [7:5] battery current direction, a THREE bit field:
 *                              000 standby, 001 charging, 010 discharging.
 *                        [2:0] the charge state machine: 0 trickle, 1 pre,
 *                              2 constant current, 3 constant voltage,
 *                              4 done, 5 not charging.
 * REG34/35 VBAT ADC   -- 14 bits across two registers, already in millivolts;
 *                        REG34 carries the high 6 bits.
 * REG64 CV target     -- [2:0] the voltage charging stops at: 1=4.0 V, 2=4.1,
 *                        3=4.2, 4=4.35, 5=4.4. Logged once at startup and never
 *                        touched: it explains a cell that stops part-full, and
 *                        changing it is a write to a chip that also owns the
 *                        display rails.
 * REGA4 gauge percent -- 0-100 from the on-chip fuel gauge, 0xFF when it has
 *                        nothing to report.
 *
 * CONFIG_BATTERY_DUMP_REGS logs the sampled ones every sample, which is how the
 * offsets were confirmed on hardware in the first place.
 */
#define REG_STATUS1   0x00
#define REG_STATUS2   0x01
#define REG_VBAT_H    0x34
#define REG_CV_TARGET 0x64
#define REG_GAUGE     0xA4

#define STATUS1_VBUS_GOOD   (1u << 5)

/*
 * THREE bits, not two.
 *
 * The first version masked 0x60 and ignored bit 7, which happened to agree on
 * this board because bit 7 reads 0 -- but XPowersLib compares the whole of
 * [7:5] against 1, and a set bit 7 would have made this report charging when
 * the chip was saying something else entirely.
 */
#define STATUS2_DIR_SHIFT   5
#define STATUS2_DIR_STANDBY 0x00
#define STATUS2_DIR_CHARGE  0x01
#define STATUS2_DIR_DISCHG  0x02

/* The charge state machine in [2:0]. CV and DONE are the two that matter here:
 * both are reached with the cable still in. */
#define STATUS2_CHG_MASK    0x07
#define STATUS2_CHG_CC      2
#define STATUS2_CHG_CV      3
#define STATUS2_CHG_DONE    4
#define STATUS2_CHG_STOP    5

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
static volatile bool s_full;
static volatile int  s_chg_state = -1;
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

        const unsigned dir = (unsigned)status2 >> STATUS2_DIR_SHIFT;
        const unsigned chg = (unsigned)status2 & STATUS2_CHG_MASK;
        const bool vbus = (status1 & STATUS1_VBUS_GOOD) != 0;

        /*
         * CHARGING MEANS THE CELL IS STILL FILLING, not "current is measurably
         * flowing in this instant".
         *
         * Direction alone was wrong on the bench: as the charge tapers into
         * constant-voltage the direction field falls back to standby, so the
         * bolt vanished while the charger was still working -- which reads as a
         * charger that gave up. The state machine is the honest source: trickle,
         * pre-charge, CC and CV are all still charging. DONE is not, and is
         * reported separately, because "full" and "not charging" mean opposite
         * things to whoever is holding the device.
         *
         * Gated on VBUS so a stale state field cannot claim charging on a device
         * running off the cell -- the direction field is the corroborating
         * reading, not the only one.
         */
        const bool active_phase = (chg <= STATUS2_CHG_CV);
        bool charging = vbus && (dir == STATUS2_DIR_CHARGE || active_phase);
        bool full = vbus && (chg == STATUS2_CHG_DONE);

        if (gauge > 100) {
            /* 0xFF means the gauge has no answer -- typically no cell attached,
             * which is exactly the case on a bench board running off USB. Report
             * "cannot read" rather than a number, and let the UI draw nothing. */
            s_valid = false;
            s_charging = false;
            s_full = false;
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

        bool announce = (!s_valid) || (low != was_low) ||
                        (charging != s_charging) || (full != s_full) ||
                        ((int)chg != s_chg_state);

        s_percent = reported;
        s_millivolts = mv;
        s_charging = charging;
        s_full = full;
        s_chg_state = (int)chg;
        s_low = low;
        s_valid = true;

        if (announce) {
            /* chgst is the raw state machine, because "charging stopped at 70%"
             * is answered by which state it stopped IN and nothing else. */
            ESP_LOGI(TAG, "EVT battery -> pct=%d mv=%d chg=%d full=%d low=%d "
                          "chgst=%u dir=%u vbus=%d",
                     reported, mv, (int)charging, (int)full, (int)low,
                     chg, dir, (int)vbus);
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

    /*
     * The charge target, read once and reported.
     *
     * This is the number that explains a cell which charges to 70% and stops:
     * the chip stops at the configured voltage, and 4.0 V or 4.1 V leaves a
     * 4.2 V cell genuinely part-full. Read only -- raising it is a write to the
     * chip that also feeds the panel and codec rails, and it is a decision
     * about someone's battery, not a default to change quietly.
     */
    uint8_t cv = 0;
    static const char *const cv_name[] = {
        "reserved", "4.0V", "4.1V", "4.2V", "4.35V", "4.4V",
    };
    if (reg_read(REG_CV_TARGET, &cv, 1) == ESP_OK) {
        const unsigned sel = cv & 0x07;
        ESP_LOGI(TAG, "charge target REG64=%02x -> %s", cv,
                 (sel < (sizeof(cv_name) / sizeof(cv_name[0]))) ? cv_name[sel] : "?");
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
    out->full = s_full;
    out->chg_state = s_chg_state;
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
