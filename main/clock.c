#include "clock.h"

#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "alarm_sched.h"
#include "tz_table.h"

#if CONFIG_CLOCK

#include "bsp/esp32_s3_touch_amoled_1_75c.h"
#include "driver/i2c_master.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "clock";

#define NVS_NAMESPACE "dgagent"
#define NVS_KEY_TZ    "tz"
#define NVS_KEY_FMT   "clk12"

/* Long enough for the longest name in the table and any sane POSIX rule. */
#define TZ_NAME_MAX 48

/*
 * Anything before this is a clock that has not been set: the RTC's reset value
 * is 2000-01-01 and the system clock's is 1970. 2025-01-01 00:00:00 UTC.
 */
#define VALID_EPOCH 1735689600LL

/*
 * The PCF85063 the board package lists on the shared bus. Its address is fixed
 * by the part. Registers are BCD; the seven time registers are contiguous from
 * 0x04, so one transaction reads or writes the whole time.
 *
 * REG00 Control_1  -- bit 1 selects 12-hour mode; this module wants 24.
 * REG04 Seconds    -- bit 7 is OS, "the oscillator stopped": set at power-on
 *                     and whenever the supply dipped too far to keep time, and
 *                     cleared only by writing the seconds. A set OS means the
 *                     time in the other registers is not to be believed.
 * REG05..0A        -- minutes, hours, day, weekday, month, year (00-99).
 */
#define RTC_ADDR      0x51
#define RTC_HZ        400000
#define RTC_REG_CTRL1 0x00
#define RTC_REG_TIME  0x04
#define RTC_CTRL1_12H 0x02
#define RTC_OS        0x80

static i2c_master_dev_handle_t s_rtc;

static volatile clock_source_t s_source;
static volatile bool s_net_up;       /* set on the event task, read by the loop */
static volatile bool s_synced;       /* set by SNTP on the lwIP task */
static bool s_sntp_started;          /* loop only */

/* The zone in force, and a requested one waiting for the loop to apply. */
static portMUX_TYPE s_tz_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_tz_name[TZ_NAME_MAX];
static char s_tz_pending[TZ_NAME_MAX];
static volatile bool s_tz_dirty;

/*
 * 12 or 24-hour. The mode is what the user said, if anything: 0 follow the
 * zone, 1 twelve, 2 twenty-four. s_zone_h12 is the zone's own default, cached
 * when the zone is applied so clock_12h() is a load rather than a table scan
 * every second. A requested mode waits in s_fmt_pending (-1 none) for the loop
 * to save, like the zone -- but is honoured at once by clock_12h(), so a
 * get_time straight after "use 24-hour time" already answers in it.
 */
#define FMT_AUTO 0
#define FMT_12   1
#define FMT_24   2
static volatile uint8_t s_fmt_mode;
static volatile int s_fmt_pending = -1;
static volatile bool s_zone_h12;
static volatile uint32_t s_tz_gen;   /* bumped on every applied change */
static SemaphoreHandle_t s_time_lock; /* see clock_lock() */

/* ---------------- the RTC ---------------- */

#if CONFIG_CLOCK_RTC
static uint8_t from_bcd(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
#endif
static uint8_t to_bcd(int v)       { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/*
 * Days since 1970-01-01 for a proleptic Gregorian date -- Howard Hinnant's
 * days_from_civil. newlib has no timegm(), and mktime() would read the date as
 * local time, which the RTC is not: it holds UTC.
 */
#if CONFIG_CLOCK_RTC
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
#endif

#if CONFIG_CLOCK_RTC
static bool rtc_read(int64_t *epoch)
{
    uint8_t reg = RTC_REG_TIME;
    uint8_t r[7];
    if (s_rtc == NULL ||
        i2c_master_transmit_receive(s_rtc, &reg, 1, r, sizeof(r), pdMS_TO_TICKS(100)) != ESP_OK) {
        return false;
    }
    if (r[0] & RTC_OS) {
        ESP_LOGW(TAG, "RTC oscillator stopped since last set; ignoring its time");
        return false;
    }
    const int sec = from_bcd(r[0] & 0x7F);
    const int min = from_bcd(r[1] & 0x7F);
    const int hour = from_bcd(r[2] & 0x3F);
    const int day = from_bcd(r[3] & 0x3F);
    const int mon = from_bcd(r[5] & 0x1F);
    const int year = 2000 + from_bcd(r[6]);
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59) {
        ESP_LOGW(TAG, "RTC holds no valid date (%02x %02x %02x %02x %02x %02x)",
                 r[6], r[5], r[3], r[2], r[1], r[0]);
        return false;
    }
    *epoch = days_from_civil(year, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
    return *epoch >= VALID_EPOCH;
}
#endif

/* Writing the seconds register also clears OS, which is the only way to. */
static void rtc_write(int64_t epoch)
{
    if (s_rtc == NULL) {
        return;
    }
    const time_t t = (time_t)epoch;
    struct tm g;
    gmtime_r(&t, &g);
    const uint8_t w[8] = {
        RTC_REG_TIME,
        to_bcd(g.tm_sec), to_bcd(g.tm_min), to_bcd(g.tm_hour), to_bcd(g.tm_mday),
        (uint8_t)g.tm_wday, to_bcd(g.tm_mon + 1), to_bcd(g.tm_year % 100),
    };
    const esp_err_t err = i2c_master_transmit(s_rtc, w, sizeof(w), pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC write failed: %s", esp_err_to_name(err));
    }
}

static void rtc_start(void)
{
#if CONFIG_CLOCK_RTC
    /* Idempotent; see battery_start() for why it is called rather than assumed. */
    if (bsp_i2c_init() != ESP_OK) {
        return;
    }
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        return;
    }
    esp_err_t err = i2c_master_probe(bus, RTC_ADDR, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        /* Not fatal, and said once: the clock simply waits for SNTP. */
        ESP_LOGW(TAG, "no RTC at 0x%02x (%s); time will come from SNTP only",
                 RTC_ADDR, esp_err_to_name(err));
        return;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RTC_ADDR,
        .scl_speed_hz = RTC_HZ,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_rtc) != ESP_OK) {
        s_rtc = NULL;
        return;
    }

    /* 24-hour mode, or the hours register means something else entirely. A
     * write to an RTC's control register is not the PMU: nothing hangs off it. */
    uint8_t reg = RTC_REG_CTRL1;
    uint8_t ctrl1 = 0;
    if (i2c_master_transmit_receive(s_rtc, &reg, 1, &ctrl1, 1, pdMS_TO_TICKS(100)) == ESP_OK &&
        (ctrl1 & RTC_CTRL1_12H)) {
        const uint8_t w[2] = {RTC_REG_CTRL1, (uint8_t)(ctrl1 & ~RTC_CTRL1_12H)};
        i2c_master_transmit(s_rtc, w, sizeof(w), pdMS_TO_TICKS(100));
        ESP_LOGW(TAG, "RTC was in 12-hour mode; switched to 24 (ctrl1 %02x)", ctrl1);
    }

    int64_t epoch = 0;
    if (rtc_read(&epoch)) {
        const struct timeval tv = {.tv_sec = (time_t)epoch};
        settimeofday(&tv, NULL);
        s_source = CLOCK_SRC_RTC;
        ESP_LOGI(TAG, "RTC at 0x%02x: time set, epoch %lld", RTC_ADDR, (long long)epoch);
    } else {
        ESP_LOGI(TAG, "RTC at 0x%02x: no trustworthy time yet", RTC_ADDR);
    }
#endif
}

/* ---------------- SNTP ---------------- */

/* lwIP's task: record and leave. The RTC write is I2C and belongs to the loop. */
/*
 * THE CHIP'S OWN TIMEBASE, CHECKED AGAINST THE SERVER AT EVERY SYNC. Between
 * two syncs the server's time moved by some amount and esp_timer by another;
 * the difference is how far the chip's clock drifted in that interval.
 * esp_timer is what timers count down on and what carries the wall clock
 * through light sleep -- on this board on the internal RC oscillator, there
 * being no 32 kHz crystal -- so this is the number that says whether deep idle
 * keeps time. Reported as the worst drift seen, in ms per hour, for TLM.
 */
static volatile int64_t s_sync_server_us;   /* lwIP task writes, loop reads */
static volatile int64_t s_sync_mono_us;
static int64_t s_last_server_us;
static int64_t s_last_mono_us;
static int s_drift_worst_ms_h;

static void on_sntp_sync(struct timeval *tv)
{
    s_sync_mono_us = esp_timer_get_time();
    s_sync_server_us = (int64_t)tv->tv_sec * 1000000 + tv->tv_usec;
    s_synced = true;
}

static void drift_note(void)
{
    const int64_t server = s_sync_server_us;
    const int64_t mono = s_sync_mono_us;
    if (s_last_server_us != 0) {
        const int64_t span = server - s_last_server_us;
        if (span > 600LL * 1000000) {   /* under 10 min says too little */
            const int64_t drift_us = (mono - s_last_mono_us) - span;
            /* us of drift per ms of span, times 3600: ms per hour. */
            const int ms_h = (int)(drift_us * 3600 / (span / 1000));
            if (abs(ms_h) > abs(s_drift_worst_ms_h)) {
                s_drift_worst_ms_h = ms_h;
            }
            ESP_LOGI(TAG, "EVT clock drift %+d ms/h over %lld s", ms_h,
                     (long long)(span / 1000000));
        }
    }
    s_last_server_us = server;
    s_last_mono_us = mono;
}

int clock_drift_ms_per_hour(void)
{
    return s_drift_worst_ms_h;
}

/* The event task, whose stack is small: a flag, and clock_tick() starts SNTP. */
static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    s_net_up = true;
}

/* ---------------- the zone ---------------- */

static void tz_apply(const char *name)
{
    char rule[TZ_NAME_MAX];
    if (!tz_resolve(name, rule, sizeof(rule))) {
        ESP_LOGW(TAG, "zone \"%s\" not recognised; using UTC", name);
        name = "UTC";
        strcpy(rule, "UTC0");
    }
    clock_lock();
    setenv("TZ", rule, 1);
    tzset();
    clock_unlock();
    portENTER_CRITICAL(&s_tz_lock);
    strlcpy(s_tz_name, name, sizeof(s_tz_name));
    portEXIT_CRITICAL(&s_tz_lock);
    s_zone_h12 = tz_default_12h(name);
    s_tz_gen++;
    ESP_LOGI(TAG, "zone %s (%s)", name, rule);
}

static void tz_save(const char *name)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_TZ, name);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        /* The zone is in force either way; only the next boot loses it. */
        ESP_LOGW(TAG, "could not persist zone: %s", esp_err_to_name(err));
    }
}

static void fmt_save(uint8_t mode)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_FMT, mode);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not persist clock format: %s", esp_err_to_name(err));
    }
}

/* ---------------- public ---------------- */

bool clock_tz_saved(char *out, size_t cap)
{
    nvs_handle_t h;
    if (cap == 0 || nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = cap;
    const bool ok = nvs_get_str(h, NVS_KEY_TZ, out, &len) == ESP_OK;
    nvs_close(h);
    if (!ok) {
        out[0] = '\0';
    }
    return ok;
}

bool clock_save_tz(const char *name)
{
    char rule[TZ_NAME_MAX];
    if (name == NULL || strlen(name) >= TZ_NAME_MAX || !tz_resolve(name, rule, sizeof(rule))) {
        return false;
    }
    tz_save(name);
    return true;
}

void clock_lock(void)
{
    if (s_time_lock != NULL) {
        xSemaphoreTake(s_time_lock, portMAX_DELAY);
    }
}

void clock_unlock(void)
{
    if (s_time_lock != NULL) {
        xSemaphoreGive(s_time_lock);
    }
}

void clock_start(void)
{
    s_time_lock = xSemaphoreCreateMutex();
    char name[TZ_NAME_MAX] = CONFIG_CLOCK_DEFAULT_TZ;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(name);
        if (nvs_get_str(h, NVS_KEY_TZ, name, &len) != ESP_OK) {
            strlcpy(name, CONFIG_CLOCK_DEFAULT_TZ, sizeof(name));
        }
        uint8_t mode = FMT_AUTO;
        if (nvs_get_u8(h, NVS_KEY_FMT, &mode) == ESP_OK && mode <= FMT_24) {
            s_fmt_mode = mode;
        }
        nvs_close(h);
    }
    tz_apply(name);

    rtc_start();

    if (esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            on_got_ip, NULL, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "could not watch for an IP address; no SNTP");
    }
}

void clock_tick(void)
{
    /*
     * Once, on the first address. GOT_IP fires on every reconnect, and the SNTP
     * client keeps its own schedule (CONFIG_LWIP_SNTP_UPDATE_DELAY) from then on,
     * so a second init would only tear down a working one.
     */
    if (s_net_up && !s_sntp_started) {
        s_sntp_started = true;
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_CLOCK_NTP_SERVER);
        cfg.sync_cb = on_sntp_sync;
        const esp_err_t err = esp_netif_sntp_init(&cfg);
        ESP_LOGI(TAG, "SNTP %s (%s)", err == ESP_OK ? "started" : "failed",
                 CONFIG_CLOCK_NTP_SERVER);
    }

    if (s_synced) {
        s_synced = false;
        drift_note();
        struct timeval tv;
        gettimeofday(&tv, NULL);
        const bool first = (s_source != CLOCK_SRC_SNTP);
        s_source = CLOCK_SRC_SNTP;
        rtc_write(tv.tv_sec);
        if (first) {
            struct tm lt;
            localtime_r(&tv.tv_sec, &lt);
            ESP_LOGI(TAG, "EVT clock sntp %04d-%02d-%02d %02d:%02d:%02d local",
                     lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                     lt.tm_hour, lt.tm_min, lt.tm_sec);
        }
    }

    if (s_tz_dirty) {
        char name[TZ_NAME_MAX];
        portENTER_CRITICAL(&s_tz_lock);
        strlcpy(name, s_tz_pending, sizeof(name));
        s_tz_dirty = false;
        portEXIT_CRITICAL(&s_tz_lock);
        tz_apply(name);
        tz_save(name);
        ESP_LOGI(TAG, "EVT tz -> %s", name);
    }

    const int fmt = s_fmt_pending;
    if (fmt >= 0) {
        s_fmt_mode = (uint8_t)fmt;
        s_fmt_pending = -1;
        fmt_save((uint8_t)fmt);
        ESP_LOGI(TAG, "EVT clock format -> %s",
                 fmt == FMT_12 ? "12h" : fmt == FMT_24 ? "24h" : "auto");
    }
}

bool clock_12h(void)
{
    const int pending = s_fmt_pending;
    const int mode = (pending >= 0) ? pending : s_fmt_mode;
    if (mode == FMT_12) {
        return true;
    }
    if (mode == FMT_24) {
        return false;
    }
    return s_zone_h12;
}

bool clock_now(int64_t *epoch, struct tm *local)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if ((int64_t)tv.tv_sec < VALID_EPOCH) {
        return false;
    }
    if (epoch != NULL) {
        *epoch = tv.tv_sec;
    }
    if (local != NULL) {
        localtime_r(&tv.tv_sec, local);
    }
    return true;
}

clock_source_t clock_source(void)
{
    return s_source;
}

uint32_t clock_tz_generation(void)
{
    return s_tz_gen;
}

void clock_tz_name(char *out, size_t cap)
{
    portENTER_CRITICAL(&s_tz_lock);
    strlcpy(out, s_tz_dirty ? s_tz_pending : s_tz_name, cap);
    portEXIT_CRITICAL(&s_tz_lock);
}

/* ---------------- agent tools ---------------- */

static const char *const WDAY[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};
static const char *const MONTH[12] = {
    "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December",
};

void clock_fn_get_time(char *out, size_t n)
{
    struct tm lt;
    clock_lock();
    const bool valid = clock_now(NULL, &lt);
    clock_unlock();
    if (!valid) {
        snprintf(out, n,
                 "The clock is not set yet; it syncs from the internet shortly "
                 "after connecting. Say you do not know the time yet.");
        return;
    }
    /* A zone change asked for in the last second has not been applied yet --
     * the loop does that -- so the time read above is still in the old zone. */
    if (s_tz_dirty) {
        snprintf(out, n, "The timezone is still being changed. Call get_time again.");
        return;
    }
    char zone[TZ_NAME_MAX];
    clock_tz_name(zone, sizeof(zone));
    /* One form only, the one the user reads -- it is also the one the model
     * then says, which two forms side by side left to chance. */
    char hm[12];
    alarm_fmt_time(lt.tm_hour, lt.tm_min, clock_12h(), hm, sizeof(hm));
    snprintf(out, n,
             "Local time %s, %s %s %d %d, timezone %s. Say only what was asked, naturally.",
             hm, WDAY[lt.tm_wday], MONTH[lt.tm_mon], lt.tm_mday, lt.tm_year + 1900, zone);
}

void clock_set_12h(bool h12)
{
    s_fmt_pending = h12 ? FMT_12 : FMT_24;
}

/*
 * Either argument or both. Both are checked before either is applied, so a good
 * zone beside a bad format changes nothing -- the model is told what was wrong
 * and calls again, rather than leaving half of what it said it did.
 */
void clock_fn_set_clock(const cJSON *args, char *out, size_t n)
{
    const cJSON *zone = cJSON_GetObjectItemCaseSensitive(args, "zone");
    const cJSON *hours = cJSON_GetObjectItemCaseSensitive(args, "hours");
    const bool has_zone = cJSON_IsString(zone) && zone->valuestring[0] != '\0';
    const bool has_hours = cJSON_IsNumber(hours);

    if (!has_zone && !has_hours) {
        snprintf(out, n, "Pass zone, hours (12 or 24), or both.");
        return;
    }
    char rule[TZ_NAME_MAX];
    if (has_zone && (strlen(zone->valuestring) >= TZ_NAME_MAX ||
                     !tz_resolve(zone->valuestring, rule, sizeof(rule)))) {
        snprintf(out, n,
                 "Not a timezone this device knows. Retry with a major-city IANA "
                 "name such as America/Chicago, or ask which city they are near.");
        return;
    }
    if (has_hours && hours->valueint != 12 && hours->valueint != 24) {
        snprintf(out, n, "hours must be 12 or 24.");
        return;
    }

    if (has_zone) {
        portENTER_CRITICAL(&s_tz_lock);
        strlcpy(s_tz_pending, zone->valuestring, sizeof(s_tz_pending));
        s_tz_dirty = true;
        portEXIT_CRITICAL(&s_tz_lock);
    }
    if (has_hours) {
        clock_set_12h(hours->valueint == 12);
    }

    if (has_zone && has_hours) {
        snprintf(out, n, "Timezone set to %s, %d-hour time. Confirm briefly.",
                 zone->valuestring, hours->valueint);
    } else if (has_zone) {
        snprintf(out, n, "Timezone set to %s. Confirm briefly. Alarms keep their local times.",
                 zone->valuestring);
    } else {
        snprintf(out, n, "Now showing %d-hour time. Confirm briefly.", hours->valueint);
    }
}

#else /* !CONFIG_CLOCK */

/* Off means absent, as battery.c does it: the stubs keep every call site plain. */
void clock_start(void) {}
void clock_tick(void) {}
bool clock_now(int64_t *epoch, struct tm *local)
{
    (void)epoch;
    (void)local;
    return false;
}
clock_source_t clock_source(void) { return CLOCK_SRC_NONE; }
uint32_t clock_tz_generation(void) { return 0; }
bool clock_12h(void) { return false; }
void clock_set_12h(bool h12) { (void)h12; }
void clock_lock(void) {}
void clock_unlock(void) {}
bool clock_tz_saved(char *out, size_t cap)
{
    if (cap > 0) {
        out[0] = '\0';
    }
    return false;
}
bool clock_save_tz(const char *name)
{
    (void)name;
    return false;
}
void clock_tz_name(char *out, size_t cap)
{
    if (cap > 0) {
        out[0] = '\0';
    }
}
void clock_fn_get_time(char *out, size_t n) { snprintf(out, n, "No clock."); }
void clock_fn_set_clock(const cJSON *args, char *out, size_t n)
{
    (void)args;
    snprintf(out, n, "No clock.");
}

#endif
