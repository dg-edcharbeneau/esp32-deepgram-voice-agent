#include "alarm_sched.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define DAYS_WEEKDAYS 0x3E   /* Mon..Fri */
#define DAYS_WEEKENDS 0x41   /* Sun, Sat */

static const char *const DAY_NAMES[7] = {
    "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday",
};
static const char *const DAY_SHORT[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

/*
 * The instant at which local wall time reads y-mon-mday hh:mm, or -1 if no such
 * instant exists. If two do, the earlier.
 *
 * Both readings are tried with tm_isdst pinned, and each is accepted only if
 * localtime_r() hands back exactly the wall time and the DST flag that were
 * asked for. A pinned flag that does not apply makes mktime() shift the answer
 * by the DST offset, which the round trip rejects -- so in a zone without DST,
 * or on a day well away from a transition, exactly one reading survives; in the
 * fall-back overlap both do; in the spring-forward gap neither does. That is the
 * whole DST policy, decided by what the library reports rather than by how it
 * chooses to normalise an impossible time.
 */
static int64_t resolve_exact(int y, int mon, int mday, int hh, int mm)
{
    int64_t best = -1;
    for (int dst = 0; dst <= 1; dst++) {
        struct tm want = {0};
        want.tm_year = y;
        want.tm_mon = mon;
        want.tm_mday = mday;
        want.tm_hour = hh;
        want.tm_min = mm;
        want.tm_isdst = dst;
        const time_t t = mktime(&want);
        if (t == (time_t)-1) {
            continue;
        }
        struct tm back;
        if (localtime_r(&t, &back) == NULL) {
            continue;
        }
        if (back.tm_year == y && back.tm_mon == mon && back.tm_mday == mday &&
            back.tm_hour == hh && back.tm_min == mm && back.tm_isdst == dst) {
            if (best < 0 || (int64_t)t < best) {
                best = (int64_t)t;
            }
        }
    }
    return best;
}

/*
 * As resolve_exact(), but a wall time lost to the spring-forward gap moves to
 * the first minute after it that does exist. Three hours is far more than any
 * real transition skips; a gap that would carry past midnight gives up, and the
 * caller tries the next day.
 */
static int64_t resolve_wall(int y, int mon, int mday, int hh, int mm)
{
    const int start = hh * 60 + mm;
    for (int m = start; m < 24 * 60 && m <= start + 180; m++) {
        const int64_t t = resolve_exact(y, mon, mday, m / 60, m % 60);
        if (t >= 0) {
            return t;
        }
    }
    return -1;
}

int64_t alarm_next_local(int64_t now, int hh, int mm, uint8_t dow_mask)
{
    if ((dow_mask & ALARM_DAYS_ALL) == 0 || hh < 0 || hh > 23 || mm < 0 || mm > 59) {
        return -1;
    }

    const time_t now_t = (time_t)now;
    struct tm today;
    if (localtime_r(&now_t, &today) == NULL) {
        return -1;
    }

    /* Eight days, not seven: a weekly alarm for today's weekday whose minute has
     * already passed lands on the same weekday next week. */
    for (int d = 0; d <= 7; d++) {
        /* Walk the calendar at noon, which no DST transition anywhere touches,
         * so the date arithmetic cannot be pushed across midnight. */
        struct tm day = {0};
        day.tm_year = today.tm_year;
        day.tm_mon = today.tm_mon;
        day.tm_mday = today.tm_mday + d;
        day.tm_hour = 12;
        day.tm_isdst = -1;
        if (mktime(&day) == (time_t)-1) {
            continue;
        }
        if ((dow_mask & (1u << day.tm_wday)) == 0) {
            continue;
        }
        const int64_t t = resolve_wall(day.tm_year, day.tm_mon, day.tm_mday, hh, mm);
        if (t > now) {
            return t;
        }
    }
    return -1;
}

bool alarm_is_stale(const alarm_entry_t *e, int64_t now)
{
    if (e->kind != ALARM_TIMER && e->kind != ALARM_ONCE) {
        return false;
    }
    return e->fire_epoch > 0 && e->fire_epoch + ALARM_MISS_WINDOW_S < now;
}

/*
 * Entries with no fire_epoch yet are skipped: that is a timer set before the
 * clock was known, which alarms.c runs on the monotonic clock instead and which
 * has no wall-clock moment to compare.
 */
int alarm_soonest(const alarm_entry_t *table, size_t n)
{
    int best = -1;
    for (size_t i = 0; i < n; i++) {
        if (table[i].kind == ALARM_EMPTY || table[i].fire_epoch <= 0) {
            continue;
        }
        if (best < 0 || table[i].fire_epoch < table[best].fire_epoch) {
            best = (int)i;
        }
    }
    return best;
}

bool alarm_parse_hhmm(const char *s, int *hh, int *mm)
{
    if (s == NULL) {
        return false;
    }
    while (*s == ' ') {
        s++;
    }
    int h = 0, digits = 0;
    while (isdigit((unsigned char)*s) && digits < 2) {
        h = h * 10 + (*s++ - '0');
        digits++;
    }
    if (digits == 0 || *s++ != ':') {
        return false;
    }
    if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1])) {
        return false;
    }
    const int m = (s[0] - '0') * 10 + (s[1] - '0');
    s += 2;
    while (*s == ' ') {
        s++;
    }
    if (*s != '\0' || h > 23 || m > 59) {
        return false;
    }
    *hh = h;
    *mm = m;
    return true;
}

/* One lower-case word against the day names: any prefix of two letters or more,
 * so "tu", "tue", "tues" and "tuesday" all land, with a plural "s" forgiven. */
static int day_index(const char *w, size_t len)
{
    for (int pass = 0; pass < 2; pass++) {
        if (len >= 2) {
            for (int d = 0; d < 7; d++) {
                if (len <= strlen(DAY_NAMES[d]) && strncmp(DAY_NAMES[d], w, len) == 0) {
                    return d;
                }
            }
        }
        if (len > 2 && w[len - 1] == 's') {
            len--;   /* "mondays", "weds": try again without it */
        } else {
            break;
        }
    }
    return -1;
}

static bool word_is(const char *w, size_t len, const char *lit)
{
    return strlen(lit) == len && strncmp(w, lit, len) == 0;
}

uint8_t alarm_parse_days(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    uint8_t mask = 0;
    char w[16];
    size_t len = 0;

    for (const char *p = s;; p++) {
        if (isalpha((unsigned char)*p)) {
            if (len < sizeof(w)) {
                w[len] = (char)tolower((unsigned char)*p);
            }
            len++;
            continue;
        }
        if (len > 0) {
            if (len > sizeof(w)) {
                return 0;
            }
            if (word_is(w, len, "daily") || word_is(w, len, "everyday") ||
                word_is(w, len, "day") || word_is(w, len, "days")) {
                mask |= ALARM_DAYS_ALL;   /* "daily", "every day" */
            } else if (word_is(w, len, "weekdays") || word_is(w, len, "weekday")) {
                mask |= DAYS_WEEKDAYS;
            } else if (word_is(w, len, "weekends") || word_is(w, len, "weekend")) {
                mask |= DAYS_WEEKENDS;
            } else if (word_is(w, len, "every") || word_is(w, len, "and") ||
                       word_is(w, len, "on")) {
                /* filler */
            } else {
                const int d = day_index(w, len);
                if (d < 0) {
                    return 0;
                }
                mask |= (uint8_t)(1u << d);
            }
            len = 0;
        }
        if (*p == '\0') {
            break;
        }
    }
    return mask;
}

void alarm_label_sanitize(char *dst, size_t cap, const char *src)
{
    if (cap == 0) {
        return;
    }
    size_t n = 0;
    bool space = false;
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p; p++) {
        if (*p >= 0x80) {
            continue;   /* a UTF-8 byte: the font cannot draw it, so drop it */
        }
        if (*p <= 0x20 || *p == 0x7F) {
            space = (n > 0);
            continue;
        }
        if (space) {
            if (n + 1 >= cap) {
                break;
            }
            dst[n++] = ' ';
            space = false;
        }
        if (n + 1 >= cap) {
            break;
        }
        dst[n++] = (char)*p;
    }
    dst[n] = '\0';
}

void alarm_fmt_time(int hh, int mm, bool h12, char *out, size_t cap)
{
    if (h12) {
        const int h = (hh % 12) ? (hh % 12) : 12;
        snprintf(out, cap, "%d:%02d %s", h, mm, hh < 12 ? "AM" : "PM");
    } else {
        snprintf(out, cap, "%02d:%02d", hh, mm);
    }
}

void alarm_days_phrase(uint8_t mask, char *out, size_t cap)
{
    mask &= ALARM_DAYS_ALL;
    if (mask == ALARM_DAYS_ALL) {
        snprintf(out, cap, "every day");
        return;
    }
    if (mask == DAYS_WEEKDAYS) {
        snprintf(out, cap, "weekdays");
        return;
    }
    if (mask == DAYS_WEEKENDS) {
        snprintf(out, cap, "weekends");
        return;
    }
    size_t n = 0;
    if (cap > 0) {
        out[0] = '\0';
    }
    for (int d = 0; d < 7; d++) {
        if (mask & (1u << d)) {
            const int w = snprintf(out + n, cap > n ? cap - n : 0, "%s%s",
                                   n ? " " : "", DAY_SHORT[d]);
            if (w < 0 || (size_t)w >= cap - n) {
                break;
            }
            n += (size_t)w;
        }
    }
}

/* ---------------- snooze ---------------- */

bool alarm_snooze_entry(const alarm_entry_t *rang, int64_t now, int minutes, alarm_entry_t *out)
{
    if (rang == NULL || out == NULL || rang->kind == ALARM_TIMER || minutes < 1 ||
        minutes > ALARM_SNOOZE_MAX_MIN) {
        return false;
    }
    const int64_t when = now + (int64_t)minutes * 60;
    const time_t t = (time_t)when;
    struct tm lt;
    localtime_r(&t, &lt);

    memset(out, 0, sizeof(*out));
    out->kind = ALARM_ONCE;
    out->hh = (uint8_t)lt.tm_hour;
    out->mm = (uint8_t)lt.tm_min;
    out->dow_mask = 0;   /* "the next hh:mm": a snooze is never a day away */
    /* To the second, not rounded to hh:mm's minute: "+1 min" means a minute. */
    out->fire_epoch = when;
    memcpy(out->label, rang->label, sizeof(out->label));
    out->label[ALARM_LABEL_MAX - 1] = '\0';
    return true;
}

/* ---------------- the ring cycle ---------------- */

static unsigned enter(alarm_ring_t *r, alarm_ring_phase_t phase, uint32_t now_ms)
{
    r->phase = phase;
    r->phase_ms = now_ms;
    switch (phase) {
    case ALARM_RING_CHIME:
        return ALARM_RING_CHIME_ON | ALARM_RING_HOLD_ON;
    case ALARM_RING_SPEAK:
        r->heard_ms = 0;
        r->announced = true;
        return ALARM_RING_HOLD_OFF | ALARM_RING_ANNOUNCE;
    case ALARM_RING_DRAIN:
        return ALARM_RING_CHIME_OFF;
    case ALARM_RING_SILENT:
        return ALARM_RING_CHIME_OFF | ALARM_RING_HOLD_OFF;
    case ALARM_RING_IDLE:
    default:
        return ALARM_RING_CHIME_OFF | ALARM_RING_HOLD_OFF | ALARM_RING_ENDED;
    }
}

unsigned alarm_ring_begin(alarm_ring_t *r, uint32_t now_ms, bool session_live)
{
    memset(r, 0, sizeof(*r));
    r->start_ms = now_ms;
    return enter(r, session_live ? ALARM_RING_SPEAK : ALARM_RING_CHIME, now_ms);
}

unsigned alarm_ring_step(alarm_ring_t *r, const alarm_ring_cfg_t *cfg, uint32_t now_ms,
                         bool playback, bool stop, bool snooze)
{
    if (r->phase == ALARM_RING_IDLE) {
        return 0;
    }
    if (snooze) {
        return enter(r, ALARM_RING_IDLE, now_ms);
    }
    if (stop) {
        if (r->announced) {
            /* Said already: an announcement under way finishes on its own. */
            return enter(r, ALARM_RING_IDLE, now_ms);
        }
        r->stopping = true;
        if (r->phase == ALARM_RING_CHIME || r->phase == ALARM_RING_SILENT) {
            return enter(r, ALARM_RING_DRAIN, now_ms);
        }
        /* DRAIN goes on to its one SPEAK below. */
    }

    const uint32_t in_phase = now_ms - r->phase_ms;
    if (!r->stopping && cfg->cap_ms > 0 && r->phase != ALARM_RING_SILENT &&
        now_ms - r->start_ms >= cfg->cap_ms) {
        return enter(r, ALARM_RING_SILENT, now_ms);
    }

    switch (r->phase) {
    case ALARM_RING_CHIME:
        return in_phase >= cfg->chime_ms ? enter(r, ALARM_RING_DRAIN, now_ms) : 0;
    case ALARM_RING_DRAIN:
        return (!playback || in_phase >= cfg->drain_max_ms) ? enter(r, ALARM_RING_SPEAK, now_ms)
                                                            : 0;
    case ALARM_RING_SPEAK: {
        if (playback) {
            r->heard_ms = now_ms;
        }
        const bool said = r->heard_ms != 0 && !playback && now_ms - r->heard_ms >= cfg->quiet_ms;
        if (said || in_phase >= cfg->speak_max_ms) {
            return r->stopping ? enter(r, ALARM_RING_IDLE, now_ms)
                               : enter(r, ALARM_RING_CHIME, now_ms);
        }
        return 0;
    }
    default:
        return 0;
    }
}
