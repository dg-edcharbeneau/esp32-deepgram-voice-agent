#include "alarms.h"

#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "alarm_sched.h"

#if CONFIG_CLOCK

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "audio_io.h"
#include "clock.h"
#include "dg_agent.h"
#include "session_ctl.h"
#include "ui.h"

static const char *TAG = "alarms";

#define NVS_NAMESPACE  "dgagent"
#define NVS_KEY_ALARMS "alarms"

/* Bump when alarm_entry_t or blob_t changes shape: an old blob is then dropped
 * whole rather than misread, exactly as history_store.c's magic does it. */
#define BLOB_VERSION 1

/* A day. Longer than that is an alarm, and the model is told so. */
#define TIMER_MAX_S (24 * 3600)

/* The longest the speaker may take to fall silent after the chime stops before
 * the session is opened regardless. The ring is empty while chiming, so this is
 * only the I2S tail -- milliseconds -- and the bound is a backstop. */
#define DRAIN_MAX_MS 3000

typedef struct {
    uint8_t version;
    uint8_t next_id;
    uint8_t reserved[6];
    alarm_entry_t e[ALARM_MAX];
} blob_t;

static SemaphoreHandle_t s_lock;
/*
 * The table, under s_lock -- in PSRAM, as is every copy of it below. Internal
 * RAM is what this board runs short of (the TLM line's intmax), and a 328-byte
 * table read once a second has no business in it.
 */
static blob_t *s_b;
/*
 * A timer set before the clock is known has no wall-clock moment, so it runs on
 * esp_timer instead: its deadline in ms here, and fire_epoch 0. alarms_tick()
 * converts it to an epoch the moment the time arrives, which is also what lets
 * it survive a reboot from then on. RAM only, under s_lock.
 */
static int64_t s_mono_ms[ALARM_MAX];
static bool s_dirty;                  /* under s_lock */
static bool s_reconciled;             /* loop only */
static uint32_t s_tz_gen;             /* loop only */

typedef enum {
    RING_IDLE,
    RING_CHIME,   /* the tone is sounding */
    RING_DRAIN,   /* tone stopped, waiting for the speaker to go quiet */
} ring_phase_t;

static volatile ring_phase_t s_phase;
static uint32_t s_phase_ms;           /* loop only */
static volatile bool s_dismissed;
/*
 * TWO caption buffers, alternated. ui.c keeps the pointer and decides "has the
 * text changed" by comparing pointers, so a second alarm written into the same
 * buffer as the first would never repaint.
 */
static char s_caption[2][ALARM_LABEL_MAX];
static int s_caption_idx;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static bool id_in_use(uint8_t id)
{
    for (int i = 0; i < ALARM_MAX; i++) {
        if (s_b->e[i].kind != ALARM_EMPTY && s_b->e[i].id == id) {
            return true;
        }
    }
    return false;
}

/* Under s_lock. The slot it landed in, or -1 with the table full. */
static int add_locked(const alarm_entry_t *proto)
{
    int slot = -1;
    for (int i = 0; i < ALARM_MAX; i++) {
        if (s_b->e[i].kind == ALARM_EMPTY) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }
    /* Ids run 1..255 and wrap; with at most eight in use one is always free. */
    uint8_t id;
    do {
        id = s_b->next_id++;
        if (s_b->next_id == 0) {
            s_b->next_id = 1;
        }
    } while (id == 0 || id_in_use(id));

    s_b->e[slot] = *proto;
    s_b->e[slot].id = id;
    s_mono_ms[slot] = 0;
    s_dirty = true;
    return slot;
}

static void remove_locked(int i)
{
    memset(&s_b->e[i], 0, sizeof(s_b->e[i]));
    s_mono_ms[i] = 0;
    s_dirty = true;
}

static int count_locked(void)
{
    int n = 0;
    for (int i = 0; i < ALARM_MAX; i++) {
        n += (s_b->e[i].kind != ALARM_EMPTY);
    }
    return n;
}

/*
 * Under s_lock, with the clock valid. Brings every instant in line with the
 * wall clock: on the first pass after boot, and again whenever the zone changes.
 *
 * A one-shot alarm keeps its saved instant at boot -- the zone is the same one
 * it was set in, and that instant is what lets a reboot across 7:30 still ring
 * it late, inside the miss window. On a zone change it is recomputed, because
 * "7:30" meant 7:30 where the user is. Weekly alarms are recomputed every time;
 * their saved instant is never trusted.
 */
static void reconcile_locked(int64_t now, bool zone_changed)
{
    for (int i = 0; i < ALARM_MAX; i++) {
        alarm_entry_t *e = &s_b->e[i];
        switch (e->kind) {
        case ALARM_ONCE:
            if (zone_changed) {
                /* A one-shot's mask is 0 ("the next hh:mm") or tomorrow's day. */
                e->fire_epoch = alarm_next_local(now, e->hh, e->mm,
                                                 e->dow_mask ? e->dow_mask : ALARM_DAYS_ALL);
                s_dirty = true;
            }
            break;
        case ALARM_WEEKLY:
            e->fire_epoch = alarm_next_local(now, e->hh, e->mm, e->dow_mask);
            break;
        default:
            break;
        }
        if (alarm_is_stale(e, now)) {
            ESP_LOGW(TAG, "EVT alarm missed id=%u kind=%u label=\"%s\" (%lld s late), dropped",
                     e->id, e->kind, e->label, (long long)(now - e->fire_epoch));
            remove_locked(i);
        }
    }
}

/* Under s_lock: the first entry that is due, or -1. */
static int due_locked(bool valid, int64_t now)
{
    const int64_t mono = esp_timer_get_time() / 1000;
    for (int i = 0; i < ALARM_MAX; i++) {
        const alarm_entry_t *e = &s_b->e[i];
        if (e->kind == ALARM_EMPTY) {
            continue;
        }
        if (s_mono_ms[i] > 0 && mono >= s_mono_ms[i]) {
            return i;
        }
        if (valid && s_reconciled && e->fire_epoch > 0 && e->fire_epoch <= now) {
            return i;
        }
    }
    return -1;
}

static void persist(void)
{
    if (!s_dirty) {
        return;   /* unlocked peek: a missed dirty is picked up next second */
    }
    /* A snapshot, so the flash write runs without the lock held. PSRAM, and
     * only for the length of a write that happens when the user changes
     * something -- esp_flash bounces a non-internal source through its own
     * buffer, so the write itself is unaffected. */
    blob_t *snap = heap_caps_malloc(sizeof(*snap), MALLOC_CAP_SPIRAM);
    if (snap == NULL) {
        return;   /* still dirty; tried again next pass */
    }
    lock();
    *snap = *s_b;
    s_dirty = false;
    unlock();

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY_ALARMS, snap, sizeof(*snap));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        /* The table in RAM is right either way; only a reboot would lose it. */
        ESP_LOGW(TAG, "could not persist alarms: %s", esp_err_to_name(err));
    }
    free(snap);
}

/* ---------------- ringing ---------------- */

static void ring(const alarm_entry_t *e)
{
    const bool timer = (e->kind == ALARM_TIMER);
    /*
     * The time it IS, not the time it was set for: an alarm can ring late --
     * inside the miss window after a reboot, or at 3:00 for a 2:30 lost to the
     * spring-forward gap -- and "It's 7:30" at 7:38 is a clock telling a lie.
     * On the main loop, so no clock_lock().
     */
    struct tm lt;
    int hh = e->hh, mm = e->mm;
    if (clock_now(NULL, &lt)) {
        hh = lt.tm_hour;
        mm = lt.tm_min;
    }
    const int h12 = (hh % 12) ? (hh % 12) : 12;

    /* What the agent will say. Plain words: it is spoken verbatim, or as the
     * greeting, and either way it is TTS input rather than a prompt. */
    char say[ALARM_LABEL_MAX + 48];
    if (timer) {
        snprintf(say, sizeof(say), e->label[0] ? "Your %s timer is done." : "Your timer is done.",
                 e->label);
    } else if (e->label[0]) {
        snprintf(say, sizeof(say), "It's %d:%02d. This is your %s alarm.", h12, mm, e->label);
    } else {
        snprintf(say, sizeof(say), "It's %d:%02d. This is your alarm.", h12, mm);
    }
    dg_agent_announce(say);

    s_caption_idx ^= 1;
    char *cap = s_caption[s_caption_idx];
    strlcpy(cap, e->label[0] ? e->label : (timer ? "timer" : "alarm"), sizeof(s_caption[0]));
    ui_flash_status(cap, (uint32_t)CONFIG_ALARM_CHIME_S * 1000 + 2000);

    ESP_LOGI(TAG, "EVT alarm ring id=%u kind=%u label=\"%s\"", e->id, e->kind, e->label);

    /*
     * A session that is up, or on its way up or down, gets no chime: the
     * microphone is open or about to be, and the tone must never reach it. The
     * announcement goes out on that session -- or, if it was stopping, on the
     * one start_soon opens once the worker is free.
     */
    if (session_ctl_is_running() || session_ctl_is_busy()) {
        session_ctl_request_start_soon();
        return;
    }
    s_dismissed = false;
    s_phase_ms = now_ms();
    s_phase = RING_CHIME;
    audio_io_chime_start();
}

/* ---------------- public ---------------- */

void alarms_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_b = heap_caps_calloc(1, sizeof(*s_b), MALLOC_CAP_SPIRAM);
    if (s_lock == NULL || s_b == NULL) {
        /* Every entry point checks s_b; the device runs on without alarms. */
        ESP_LOGE(TAG, "no memory for the alarm table; alarms disabled");
        free(s_b);
        s_b = NULL;
        return;
    }

    /* Straight into the table, which is then checked field by field: anything
     * that does not survive the check is zeroed, and a blob of the wrong size or
     * version is dropped whole. */
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(*s_b);
        const esp_err_t err = nvs_get_blob(h, NVS_KEY_ALARMS, s_b, &len);
        nvs_close(h);
        if (err == ESP_OK && len == sizeof(*s_b) && s_b->version == BLOB_VERSION) {
            for (int i = 0; i < ALARM_MAX; i++) {
                alarm_entry_t *e = &s_b->e[i];
                e->label[ALARM_LABEL_MAX - 1] = '\0';
                /*
                 * A timer with no instant was set before the clock was known and
                 * lived on esp_timer, which a reboot zeroes. Kept, it would never
                 * ring, never go stale and hold its slot for good -- so it goes,
                 * with a line saying so.
                 */
                const bool sane = e->id != 0 && e->kind >= ALARM_TIMER &&
                                  e->kind <= ALARM_WEEKLY && e->hh < 24 && e->mm < 60 &&
                                  (e->kind != ALARM_WEEKLY || (e->dow_mask & ALARM_DAYS_ALL)) &&
                                  (e->kind != ALARM_TIMER || e->fire_epoch > 0);
                if (!sane) {
                    if (e->kind == ALARM_TIMER && e->id != 0) {
                        ESP_LOGW(TAG, "EVT alarm lost id=%u: timer set before the clock "
                                      "was known did not survive the reboot", e->id);
                    }
                    memset(e, 0, sizeof(*e));
                }
            }
        } else {
            if (err != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGW(TAG, "discarding saved alarms (%s, %u B, v%u)", esp_err_to_name(err),
                         (unsigned)len, err == ESP_OK ? s_b->version : 0);
            }
            memset(s_b, 0, sizeof(*s_b));
        }
    }
    s_b->version = BLOB_VERSION;
    if (s_b->next_id == 0) {
        s_b->next_id = 1;
    }

#if CONFIG_CLOCK_TEST_TIMER_S > 0
    /* Bench aid: a timer at boot, so the whole firing path can be exercised
     * with nobody speaking to the device. */
    alarm_entry_t test = {.kind = ALARM_TIMER};
    strlcpy(test.label, "test", sizeof(test.label));
    const int slot = add_locked(&test);
    if (slot >= 0) {
        s_mono_ms[slot] = esp_timer_get_time() / 1000 + CONFIG_CLOCK_TEST_TIMER_S * 1000LL;
    }
#endif

    ESP_LOGI(TAG, "%d saved", count_locked());
}

void alarms_tick(void)
{
    if (s_b == NULL) {
        return;
    }
    int64_t now = 0;
    const bool valid = clock_now(&now, NULL);

    alarm_entry_t fired = {0};
    bool fire = false;

    lock();
    if (valid) {
        const uint32_t gen = clock_tz_generation();
        if (!s_reconciled || gen != s_tz_gen) {
            reconcile_locked(now, s_reconciled);
            s_reconciled = true;
            s_tz_gen = gen;
        }
        /* Timers that started life on the monotonic clock get a real instant as
         * soon as there is one, so a reboot from here on does not lose them. */
        const int64_t mono = esp_timer_get_time() / 1000;
        for (int i = 0; i < ALARM_MAX; i++) {
            if (s_mono_ms[i] > 0 && s_b->e[i].kind == ALARM_TIMER) {
                s_b->e[i].fire_epoch = now + (s_mono_ms[i] - mono + 999) / 1000;
                s_mono_ms[i] = 0;
                s_dirty = true;
            }
        }
    }

    /* One per pass: two alarms due in the same second ring a second apart, each
     * with its own caption; their announcements are joined in dg_agent's slot
     * (see dg_agent_announce()) rather than the second replacing the first. */
    const int i = due_locked(valid, now);
    if (i >= 0) {
        fired = s_b->e[i];
        fire = true;
        if (fired.kind == ALARM_WEEKLY && valid) {
            /* Rolls on in RAM; nothing about its definition changed. */
            s_b->e[i].fire_epoch = alarm_next_local(now, fired.hh, fired.mm, fired.dow_mask);
        } else {
            remove_locked(i);
        }
    }
    unlock();

    if (fire) {
        ring(&fired);
    }

    if (s_phase == RING_CHIME &&
        (s_dismissed || now_ms() - s_phase_ms >= (uint32_t)CONFIG_ALARM_CHIME_S * 1000)) {
        audio_io_chime_stop();
        ESP_LOGI(TAG, "EVT alarm chime %s", s_dismissed ? "dismissed" : "done");
        s_phase_ms = now_ms();
        s_phase = RING_DRAIN;
    }
    if (s_phase == RING_DRAIN &&
        (!audio_io_playback_active() || now_ms() - s_phase_ms >= DRAIN_MAX_MS)) {
        /* Silent, so the microphone may open. The announcement rides on it. */
        s_phase = RING_IDLE;
        session_ctl_request_start_soon();
    }

    persist();
}

bool alarms_ringing(void)
{
    return s_phase != RING_IDLE;
}

void alarms_dismiss(void)
{
    /* Stopped here, not on the next tick: a tap should silence it at once. The
     * tick sees the flag and moves on to opening the session. */
    if (s_phase == RING_CHIME) {
        audio_io_chime_stop();
        s_dismissed = true;
    }
}

int alarms_count(void)
{
    if (s_b == NULL) {
        return 0;
    }
    lock();
    const int n = count_locked();
    unlock();
    return n;
}

bool alarms_next(int64_t *epoch)
{
    if (s_b == NULL) {
        return false;
    }
    lock();
    const int i = alarm_soonest(s_b->e, ALARM_MAX);
    if (i >= 0) {
        *epoch = s_b->e[i].fire_epoch;
    }
    unlock();
    return i >= 0;
}

/* ---------------- agent tools ---------------- */

static const char *const WDAY[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};

/* The table could not be allocated at boot: every tool says so, plainly. */
static bool unavailable(char *out, size_t n)
{
    if (s_b != NULL) {
        return false;
    }
    snprintf(out, n, "Alarms and timers are unavailable on this device right now. Say so.");
    return true;
}

/* "1 hour 5 minutes", "10 minutes", "45 seconds". */
static void fmt_duration(int64_t s, char *out, size_t n)
{
    const int h = (int)(s / 3600);
    const int m = (int)((s % 3600) / 60);
    const int sec = (int)(s % 60);
    size_t k = 0;
    out[0] = '\0';
    if (h > 0) {
        k += (size_t)snprintf(out + k, n - k, "%d hour%s", h, h == 1 ? "" : "s");
    }
    if (m > 0 && k < n) {
        k += (size_t)snprintf(out + k, n - k, "%s%d minute%s", k ? " " : "", m, m == 1 ? "" : "s");
    }
    if ((sec > 0 || (h == 0 && m == 0)) && h == 0 && k < n) {
        snprintf(out + k, n - k, "%s%d second%s", k ? " " : "", sec, sec == 1 ? "" : "s");
    }
}

/* "today", "tomorrow", or the weekday the instant falls on. */
static const char *day_word(int64_t now, int64_t when)
{
    const time_t tn = (time_t)now, tw = (time_t)when;
    struct tm ln, lw;
    localtime_r(&tn, &ln);
    localtime_r(&tw, &lw);
    if (ln.tm_year == lw.tm_year && ln.tm_yday == lw.tm_yday) {
        return "today";
    }
    struct tm tmr = ln;
    tmr.tm_mday += 1;
    tmr.tm_hour = 12;
    tmr.tm_isdst = -1;
    mktime(&tmr);
    if (tmr.tm_year == lw.tm_year && tmr.tm_yday == lw.tm_yday) {
        return "tomorrow";
    }
    return WDAY[lw.tm_wday];
}

static void get_label(const cJSON *args, char *out)
{
    const cJSON *label = cJSON_GetObjectItemCaseSensitive(args, "label");
    alarm_label_sanitize(out, ALARM_LABEL_MAX, cJSON_IsString(label) ? label->valuestring : "");
}

void alarms_fn_set_timer(const cJSON *args, char *out, size_t n)
{
    if (unavailable(out, n)) {
        return;
    }
    const cJSON *sec = cJSON_GetObjectItemCaseSensitive(args, "seconds");
    if (!cJSON_IsNumber(sec) || sec->valuedouble < 1 || sec->valuedouble > TIMER_MAX_S) {
        snprintf(out, n, "Timers run from 1 second to 24 hours; for longer, set an alarm.");
        return;
    }
    const int64_t secs = (int64_t)sec->valuedouble;

    alarm_entry_t p = {.kind = ALARM_TIMER};
    get_label(args, p.label);

    int64_t now = 0;
    const bool valid = clock_now(&now, NULL);

    lock();
    const int slot = add_locked(&p);
    if (slot >= 0) {
        if (valid) {
            s_b->e[slot].fire_epoch = now + secs;
        } else {
            s_mono_ms[slot] = esp_timer_get_time() / 1000 + secs * 1000;
        }
    }
    const uint8_t id = (slot >= 0) ? s_b->e[slot].id : 0;
    unlock();

    if (slot < 0) {
        snprintf(out, n, "Already %d alarms and timers, the most it holds. Offer to cancel one.",
                 ALARM_MAX);
        return;
    }
    char dur[40];
    fmt_duration(secs, dur, sizeof(dur));
    snprintf(out, n, "Timer %u set for %s%s%s. Confirm briefly.", id, dur,
             p.label[0] ? ", labelled " : "", p.label);
    ESP_LOGI(TAG, "EVT alarm set id=%u timer %lld s label=\"%s\"%s", id, (long long)secs,
             p.label, valid ? "" : " (monotonic)");
}

void alarms_fn_set_alarm(const cJSON *args, char *out, size_t n)
{
    if (unavailable(out, n)) {
        return;
    }
    int64_t now = 0;
    struct tm today;
    clock_lock();
    const bool valid = clock_now(&now, &today);
    clock_unlock();
    if (!valid) {
        snprintf(out, n, "The clock is not set yet, so no alarms. Say so; a timer still works.");
        return;
    }

    const cJSON *time_s = cJSON_GetObjectItemCaseSensitive(args, "time");
    int hh, mm;
    if (!cJSON_IsString(time_s)) {
        snprintf(out, n, "Pass seconds for a timer, or time as HH:MM for an alarm.");
        return;
    }
    if (!alarm_parse_hhmm(time_s->valuestring, &hh, &mm)) {
        snprintf(out, n, "time must be 24-hour HH:MM, like 07:30 or 19:05. Convert and call again.");
        return;
    }

    alarm_entry_t p = {.kind = ALARM_ONCE, .hh = (uint8_t)hh, .mm = (uint8_t)mm};
    const cJSON *days = cJSON_GetObjectItemCaseSensitive(args, "days");
    if (cJSON_IsString(days) && strcasecmp(days->valuestring, "tomorrow") == 0) {
        /*
         * Once, but not "the next hh:mm": at 3 PM, "tomorrow at 7 PM" is not
         * today at 7. A one-shot carrying tomorrow's weekday resolves to the
         * next hh:mm on that day, which is tomorrow's.
         */
        p.dow_mask = (uint8_t)(1u << ((today.tm_wday + 1) % 7));
    } else if (cJSON_IsString(days) && days->valuestring[0] != '\0') {
        p.dow_mask = alarm_parse_days(days->valuestring);
        if (p.dow_mask == 0) {
            snprintf(out, n, "Did not understand the days. Use daily, weekdays, weekends, "
                             "or day names like mon,wed.");
            return;
        }
        p.kind = ALARM_WEEKLY;
    }
    get_label(args, p.label);
    /* mktime() and friends on the WebSocket task: under clock_lock(), so a zone
     * being applied on the main loop cannot be parsed here instead. */
    clock_lock();
    p.fire_epoch = alarm_next_local(now, hh, mm, p.dow_mask ? p.dow_mask : ALARM_DAYS_ALL);
    const char *first = day_word(now, p.fire_epoch);
    clock_unlock();

    lock();
    const int slot = add_locked(&p);
    const uint8_t id = (slot >= 0) ? s_b->e[slot].id : 0;
    unlock();

    if (slot < 0) {
        snprintf(out, n, "Already %d alarms and timers, the most it holds. Offer to cancel one.",
                 ALARM_MAX);
        return;
    }

    char repeat[40] = "";
    if (p.kind == ALARM_WEEKLY) {
        char phrase[24];
        alarm_days_phrase(p.dow_mask, phrase, sizeof(phrase));
        snprintf(repeat, sizeof(repeat), ", repeating %s", phrase);
    }
    char hm[12];
    alarm_fmt_time(hh, mm, clock_12h(), hm, sizeof(hm));
    snprintf(out, n, "Alarm %u set for %s, first %s%s. Confirm briefly.", id, hm, first,
             repeat);
    ESP_LOGI(TAG, "EVT alarm set id=%u %02d:%02d mask=%02x label=\"%s\"", id, hh, mm,
             p.dow_mask, p.label);
}

void alarms_fn_list(char *out, size_t n)
{
    if (unavailable(out, n)) {
        return;
    }
    int64_t now = 0;
    const bool valid = clock_now(&now, NULL);
    const int64_t mono = esp_timer_get_time() / 1000;
    const bool h12 = clock_12h();

    clock_lock();   /* day_word() below converts local time; order: clock, then table */
    lock();
    const int count = count_locked();
    size_t k = (size_t)snprintf(out, n, count ? "%d pending:" : "No alarms or timers.", count);
    for (int i = 0; i < ALARM_MAX && k < n; i++) {
        const alarm_entry_t *e = &s_b->e[i];
        if (e->kind == ALARM_EMPTY) {
            continue;
        }
        char what[64];
        char hm[12];
        alarm_fmt_time(e->hh, e->mm, h12, hm, sizeof(hm));
        if (e->kind == ALARM_TIMER) {
            const int64_t left = s_mono_ms[i] > 0 ? (s_mono_ms[i] - mono) / 1000
                                                  : (valid ? e->fire_epoch - now : 0);
            char dur[40];
            fmt_duration(left > 0 ? left : 0, dur, sizeof(dur));
            snprintf(what, sizeof(what), "timer, %s left", dur);
        } else if (e->kind == ALARM_WEEKLY) {
            char phrase[24];
            alarm_days_phrase(e->dow_mask, phrase, sizeof(phrase));
            snprintf(what, sizeof(what), "%s %s", hm, phrase);
        } else {
            snprintf(what, sizeof(what), "%s %s", hm, valid ? day_word(now, e->fire_epoch) : "");
        }
        k += (size_t)snprintf(out + k, n - k, " #%u %s%s%s;", e->id, what,
                              e->label[0] ? " " : "", e->label);
    }
    unlock();
    clock_unlock();
    if (count && k < n) {
        snprintf(out + k, n - k, " Summarise naturally; say ids only if asked.");
    }
}

void alarms_fn_cancel(const cJSON *args, char *out, size_t n)
{
    if (unavailable(out, n)) {
        return;
    }
    const cJSON *target = cJSON_GetObjectItemCaseSensitive(args, "cancel");
    if (!cJSON_IsString(target) || target->valuestring[0] == '\0') {
        snprintf(out, n, "Say which: an id, a label, or all. Call alarms if unsure.");
        return;
    }
    const char *t = target->valuestring;
    const bool all = (strcasecmp(t, "all") == 0);
    char *end = NULL;
    const long id = strtol(t, &end, 10);
    const bool by_id = (end != t && *end == '\0');

    int cancelled = 0;
    lock();
    for (int i = 0; i < ALARM_MAX; i++) {
        const alarm_entry_t *e = &s_b->e[i];
        if (e->kind == ALARM_EMPTY) {
            continue;
        }
        if (all || (by_id && e->id == id) || (!by_id && strcasecmp(e->label, t) == 0)) {
            ESP_LOGI(TAG, "EVT alarm cancel id=%u label=\"%s\"", e->id, e->label);
            remove_locked(i);
            cancelled++;
        }
    }
    unlock();

    if (cancelled == 0) {
        snprintf(out, n, "Nothing matches \"%s\". Call alarms and ask which one.", t);
    } else {
        snprintf(out, n, "Cancelled %d. Confirm briefly.", cancelled);
    }
}

int alarms_snapshot(alarm_view_t *out, int max)
{
    if (s_b == NULL || max <= 0) {
        return 0;
    }
    int64_t now = 0;
    const bool valid = clock_now(&now, NULL);
    const int64_t mono = esp_timer_get_time() / 1000;
    const bool h12 = clock_12h();
    int64_t key[ALARM_MAX];   /* seconds until it fires, for the sort */
    int n = 0;

    clock_lock();   /* day_word() converts local time; same order as alarms_fn_list() */
    lock();
    for (int i = 0; i < ALARM_MAX && n < max && n < ALARM_MAX; i++) {
        const alarm_entry_t *e = &s_b->e[i];
        if (e->kind == ALARM_EMPTY) {
            continue;
        }
        alarm_view_t *v = &out[n];
        memset(v, 0, sizeof(*v));
        v->id = e->id;
        strlcpy(v->label, e->label, sizeof(v->label));
        if (e->kind == ALARM_TIMER) {
            const int64_t left = s_mono_ms[i] > 0 ? (s_mono_ms[i] - mono) / 1000
                                                  : (valid ? e->fire_epoch - now : 0);
            v->timer = true;
            v->secs_left = left > 0 ? left : 0;
            key[n] = v->secs_left;
        } else {
            alarm_fmt_time(e->hh, e->mm, h12, v->time, sizeof(v->time));
            if (e->kind == ALARM_WEEKLY) {
                alarm_days_phrase(e->dow_mask, v->when, sizeof(v->when));
            } else if (valid) {
                strlcpy(v->when, day_word(now, e->fire_epoch), sizeof(v->when));
            }
            key[n] = valid ? e->fire_epoch - now : INT64_MAX;
        }
        n++;
    }
    unlock();
    clock_unlock();

    /* Soonest first. Eight entries at most, so insertion sort. */
    for (int i = 1; i < n; i++) {
        const alarm_view_t v = out[i];
        const int64_t k = key[i];
        int j = i - 1;
        while (j >= 0 && key[j] > k) {
            out[j + 1] = out[j];
            key[j + 1] = key[j];
            j--;
        }
        out[j + 1] = v;
        key[j + 1] = k;
    }
    return n;
}

bool alarms_cancel_id(uint8_t id)
{
    if (s_b == NULL || id == 0) {
        return false;
    }
    bool found = false;
    lock();
    for (int i = 0; i < ALARM_MAX; i++) {
        const alarm_entry_t *e = &s_b->e[i];
        if (e->kind != ALARM_EMPTY && e->id == id) {
            ESP_LOGI(TAG, "EVT alarm cancel id=%u label=\"%s\" (panel)", e->id, e->label);
            remove_locked(i);
            found = true;
            break;
        }
    }
    unlock();
    return found;
}

#else /* !CONFIG_CLOCK */

int alarms_snapshot(alarm_view_t *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}
bool alarms_cancel_id(uint8_t id)
{
    (void)id;
    return false;
}
void alarms_init(void) {}
void alarms_tick(void) {}
bool alarms_ringing(void) { return false; }
void alarms_dismiss(void) {}
int alarms_count(void) { return 0; }
bool alarms_next(int64_t *epoch)
{
    (void)epoch;
    return false;
}
void alarms_fn_set_timer(const cJSON *args, char *out, size_t n) { (void)args; snprintf(out, n, "No clock."); }
void alarms_fn_set_alarm(const cJSON *args, char *out, size_t n) { (void)args; snprintf(out, n, "No clock."); }
void alarms_fn_list(char *out, size_t n) { snprintf(out, n, "No clock."); }
void alarms_fn_cancel(const cJSON *args, char *out, size_t n) { (void)args; snprintf(out, n, "No clock."); }

#endif
