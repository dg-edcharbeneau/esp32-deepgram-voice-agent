/*
 * Exercises main/alarm_sched.c where an alarm clock goes wrong: the two nights a
 * year when local time skips or repeats an hour, the week wrapping round, and a
 * device that was off when something was meant to ring.
 *
 * The real module is compiled; it touches nothing but libc's time functions, so
 * no stubs are needed. Every expectation is written as a UTC instant so that it
 * is checked against the calendar, not against the library under test.
 *
 * The dates are the real 2026 transitions:
 *   America/Chicago  springs forward Sun 8 Mar 02:00 -> 03:00,
 *                    falls back      Sun 1 Nov 02:00 -> 01:00.
 *   Europe/London    springs forward Sun 29 Mar 01:00 -> 02:00,
 *                    falls back      Sun 25 Oct 02:00 -> 01:00.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alarm_sched.h"
#include "tz_table.h"

static int failures;

static const char *s_tz = "";

static void check(const char *what, int ok)
{
    printf("%-58s %-24s %s\n", what, s_tz, ok ? "ok" : "FAILED");
    if (!ok) {
        failures++;
    }
}

static void use_tz(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
    s_tz = tz;
}

/* A UTC instant from its calendar fields. */
static int64_t utc(int y, int mon, int d, int h, int mi)
{
    struct tm t = {0};
    t.tm_year = y - 1900;
    t.tm_mon = mon - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    return (int64_t)timegm(&t);
}

static int next_is(int64_t now, int hh, int mm, uint8_t mask, int64_t want)
{
    const int64_t got = alarm_next_local(now, hh, mm, mask);
    if (got != want) {
        printf("    got %lld, want %lld (off by %lld s)\n",
               (long long)got, (long long)want, (long long)(got - want));
    }
    return got == want;
}

#define SUN (1u << 0)
#define MON (1u << 1)
#define TUE (1u << 2)
#define WED (1u << 3)
#define FRI (1u << 5)
#define SAT (1u << 6)

/*
 * Each zone runs twice: once by IANA name, through the laptop's own zone
 * database, and once as the POSIX string the device actually applies -- newlib
 * has no database, only the rule. Both must give the same answers, which also
 * checks the rule string tz_table.c hands the device for that zone.
 */
static void chicago(const char *tz)
{
    use_tz(tz);

    /* An ordinary day, CDT is UTC-5. */
    check("chicago: 07:30 later today",
          next_is(utc(2026, 10, 20, 11, 0), 7, 30, ALARM_DAYS_ALL, utc(2026, 10, 20, 12, 30)));
    check("chicago: 07:30 already passed today -> tomorrow",
          next_is(utc(2026, 10, 20, 13, 0), 7, 30, ALARM_DAYS_ALL, utc(2026, 10, 21, 12, 30)));
    check("chicago: the exact minute is not 'next'",
          next_is(utc(2026, 10, 20, 12, 30), 7, 30, ALARM_DAYS_ALL, utc(2026, 10, 21, 12, 30)));

    /* Sat 24 Oct -> Mon 26 Oct, across the weekend. */
    check("chicago: Monday alarm from a Saturday",
          next_is(utc(2026, 10, 24, 15, 0), 7, 0, MON, utc(2026, 10, 26, 12, 0)));
    /* Mon 26 Oct 08:00 local, Monday 07:00 has gone: a week on, and that week
     * crosses the fall-back, so it is 07:00 CST = 13:00 UTC. */
    check("chicago: same weekday passed -> next week, after DST ends",
          next_is(utc(2026, 10, 26, 13, 0), 7, 0, MON, utc(2026, 11, 2, 13, 0)));
    check("chicago: Wed|Fri from a Thursday -> Friday",
          next_is(utc(2026, 10, 22, 15, 0), 6, 45, WED | FRI, utc(2026, 10, 23, 11, 45)));

    /* Spring forward: 02:30 does not exist on 8 Mar. Midnight CST is 06:00 UTC;
     * the first minute that exists after 02:30 is 03:00 CDT = 08:00 UTC. */
    check("chicago: 02:30 in the spring gap rings at 03:00",
          next_is(utc(2026, 3, 8, 6, 0), 2, 30, ALARM_DAYS_ALL, utc(2026, 3, 8, 8, 0)));
    /* And the day after it is a normal 02:30 CDT. */
    check("chicago: ... and at 02:30 the next day",
          next_is(utc(2026, 3, 8, 8, 0), 2, 30, ALARM_DAYS_ALL, utc(2026, 3, 9, 7, 30)));

    /* Fall back: 01:30 happens twice on 1 Nov, 06:30 UTC (CDT) then 07:30 UTC
     * (CST). It rings at the first. */
    check("chicago: 01:30 in the fall overlap rings at the first",
          next_is(utc(2026, 11, 1, 5, 0), 1, 30, ALARM_DAYS_ALL, utc(2026, 11, 1, 6, 30)));
    /* Having rung, the repeat of 01:30 an hour later is not a second ring. */
    check("chicago: ... and not again at the repeated 01:30",
          next_is(utc(2026, 11, 1, 6, 30), 1, 30, ALARM_DAYS_ALL, utc(2026, 11, 2, 7, 30)));
}

static void london(const char *tz)
{
    use_tz(tz);

    /* 01:30 does not exist on 29 Mar; 02:00 BST = 01:00 UTC. */
    check("london: 01:30 in the spring gap rings at 02:00",
          next_is(utc(2026, 3, 29, 0, 0), 1, 30, ALARM_DAYS_ALL, utc(2026, 3, 29, 1, 0)));
    /* 01:30 happens twice on 25 Oct, 00:30 UTC (BST) then 01:30 UTC (GMT). */
    check("london: 01:30 in the fall overlap rings at the first",
          next_is(utc(2026, 10, 24, 23, 0), 1, 30, ALARM_DAYS_ALL, utc(2026, 10, 25, 0, 30)));
    check("london: Saturday|Sunday from a Friday",
          next_is(utc(2026, 10, 23, 12, 0), 9, 0, SAT | SUN, utc(2026, 10, 24, 8, 0)));
}

static void plain(void)
{
    use_tz("UTC0");
    check("utc: empty mask has no next",
          alarm_next_local(utc(2026, 1, 1, 0, 0), 7, 0, 0) == -1);
    check("utc: 24:00 is refused",
          alarm_next_local(utc(2026, 1, 1, 0, 0), 24, 0, ALARM_DAYS_ALL) == -1);
    /* Sat 31 Dec 2026 -> the Tuesday is 5 Jan 2027, across the year. */
    check("utc: Tuesday from New Year's Eve, across the year",
          next_is(utc(2026, 12, 31, 12, 0), 7, 0, TUE, utc(2027, 1, 5, 7, 0)));
}

static void missed(void)
{
    s_tz = "";
    const int64_t now = utc(2026, 10, 20, 12, 0);
    alarm_entry_t e = {.id = 1, .kind = ALARM_ONCE};

    e.fire_epoch = now - ALARM_MISS_WINDOW_S + 60;
    check("missed: a one-shot just inside the window still rings", !alarm_is_stale(&e, now));
    e.fire_epoch = now - ALARM_MISS_WINDOW_S - 60;
    check("missed: a one-shot outside the window is dropped", alarm_is_stale(&e, now));
    e.kind = ALARM_TIMER;
    check("missed: so is a timer", alarm_is_stale(&e, now));
    e.kind = ALARM_WEEKLY;
    check("missed: a weekly alarm is never stale, it rolls on", !alarm_is_stale(&e, now));

    alarm_entry_t t[ALARM_MAX] = {0};
    check("soonest: empty table has none", alarm_soonest(t, ALARM_MAX) == -1);
    t[2] = (alarm_entry_t){.id = 3, .kind = ALARM_ONCE, .fire_epoch = 500};
    t[5] = (alarm_entry_t){.id = 6, .kind = ALARM_TIMER, .fire_epoch = 200};
    t[6] = (alarm_entry_t){.id = 7, .kind = ALARM_TIMER, .fire_epoch = 0};
    check("soonest: earliest wins, a timer with no epoch is skipped",
          alarm_soonest(t, ALARM_MAX) == 5);
}

static void parsing(void)
{
    int h = -1, m = -1;
    check("hhmm: 07:30", alarm_parse_hhmm("07:30", &h, &m) && h == 7 && m == 30);
    check("hhmm: 7:05", alarm_parse_hhmm("7:05", &h, &m) && h == 7 && m == 5);
    check("hhmm: 23:59", alarm_parse_hhmm(" 23:59 ", &h, &m) && h == 23 && m == 59);
    check("hhmm: 24:00 refused", !alarm_parse_hhmm("24:00", &h, &m));
    check("hhmm: 7:5 refused", !alarm_parse_hhmm("7:5", &h, &m));
    check("hhmm: 7:30 pm refused", !alarm_parse_hhmm("7:30 pm", &h, &m));
    check("hhmm: 730 refused", !alarm_parse_hhmm("730", &h, &m));

    check("days: daily", alarm_parse_days("daily") == ALARM_DAYS_ALL);
    check("days: every day", alarm_parse_days("every day") == ALARM_DAYS_ALL);
    check("days: weekdays", alarm_parse_days("Weekdays") == 0x3E);
    check("days: weekends", alarm_parse_days("weekend") == 0x41);
    check("days: mon,wed,fri", alarm_parse_days("mon,wed,fri") == (MON | WED | FRI));
    check("days: Monday and Friday", alarm_parse_days("Monday and Friday") == (MON | FRI));
    check("days: tues thurs sat",
          alarm_parse_days("tues thurs sat") == ((1u << 2) | (1u << 4) | (1u << 6)));
    check("days: mondays, weds", alarm_parse_days("mondays, weds") == (MON | WED));
    check("days: 's' alone is ambiguous and refused", alarm_parse_days("s") == 0);
    check("days: one bad word refuses the lot", alarm_parse_days("mon funday") == 0);
    check("days: empty", alarm_parse_days("") == 0);

    char buf[ALARM_LABEL_MAX];
    alarm_label_sanitize(buf, sizeof(buf), "  pasta\t\ntimer  ");
    check("label: whitespace collapsed and trimmed", strcmp(buf, "pasta timer") == 0);
    alarm_label_sanitize(buf, sizeof(buf), "caf\xc3\xa9 au lait");
    check("label: UTF-8 bytes dropped", strcmp(buf, "caf au lait") == 0);
    alarm_label_sanitize(buf, sizeof(buf), "a very long label that will not fit at all");
    check("label: truncated to fit", strlen(buf) == sizeof(buf) - 1);

    alarm_fmt_time(20, 52, true, buf, sizeof(buf));
    check("time: 20:52 is 8:52 PM", strcmp(buf, "8:52 PM") == 0);
    alarm_fmt_time(0, 5, true, buf, sizeof(buf));
    check("time: midnight is 12 AM", strcmp(buf, "12:05 AM") == 0);
    alarm_fmt_time(12, 0, true, buf, sizeof(buf));
    check("time: noon is 12 PM", strcmp(buf, "12:00 PM") == 0);
    alarm_fmt_time(7, 30, true, buf, sizeof(buf));
    check("time: 7:30 AM, no leading zero", strcmp(buf, "7:30 AM") == 0);
    alarm_fmt_time(7, 30, false, buf, sizeof(buf));
    check("time: 24-hour keeps the zero", strcmp(buf, "07:30") == 0);

    alarm_days_phrase(ALARM_DAYS_ALL, buf, sizeof(buf));
    check("phrase: every day", strcmp(buf, "every day") == 0);
    alarm_days_phrase(0x3E, buf, sizeof(buf));
    check("phrase: weekdays", strcmp(buf, "weekdays") == 0);
    alarm_days_phrase(MON | WED | FRI, buf, sizeof(buf));
    check("phrase: Mon Wed Fri", strcmp(buf, "Mon Wed Fri") == 0);
}

/*
 * Every row of tz_table.c against the laptop's own zone database: the UTC
 * offset under the IANA name and under the device's rule must agree at every
 * sample, four a day for a year from November 2026 -- the table holds the rules
 * in force from then on, not the history (see its header). That is what makes a hand-written table safe
 * to ship -- a transition on the wrong Sunday or an offset off by half an hour
 * fails here, by name, instead of on a device on the night the clocks change.
 */
static long offset_at(const char *tz, int64_t t)
{
    use_tz(tz);
    struct tm lt;
    time_t tt = (time_t)t;
    localtime_r(&tt, &lt);
    return lt.tm_gmtoff;
}

static void table(void)
{
    size_t n = 0;
    const tz_entry_t *rows = tz_table(&n);
    const int64_t start = utc(2026, 11, 2, 0, 0);
    const int64_t end = utc(2027, 11, 2, 0, 0);
    int bad_rows = 0;

    for (size_t i = 0; i < n; i++) {
        /* The one row known to be inexact, and documented as such in the table:
         * Morocco's Ramadan offset moves every year and has no POSIX form. */
        if (strcmp(rows[i].iana, "Africa/Casablanca") == 0) {
            continue;
        }
        int64_t first_bad = -1;
        long want = 0, got = 0;
        for (int64_t t = start; t < end; t += 6 * 3600) {
            want = offset_at(rows[i].iana, t);
            got = offset_at(rows[i].posix, t);
            if (want != got) {
                first_bad = t;
                break;
            }
        }
        if (first_bad >= 0) {
            time_t ft = (time_t)first_bad;
            struct tm g;
            gmtime_r(&ft, &g);
            printf("    %s: %s gives %+ld s, tzdata %+ld s, at %04d-%02d-%02d %02d:00Z\n",
                   rows[i].iana, rows[i].posix, got, want,
                   g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour);
            bad_rows++;
        }
    }
    s_tz = "";
    char what[64];
    snprintf(what, sizeof(what), "tz table: %zu rows agree with tzdata for a year", n);
    check(what, bad_rows == 0);

    char out[48];
    check("tz resolve: IANA name",
          tz_resolve("america/chicago", out, sizeof(out)) &&
          strcmp(out, "CST6CDT,M3.2.0,M11.1.0") == 0);
    check("tz resolve: POSIX rule passes through",
          tz_resolve("EST5EDT,M3.2.0,M11.1.0", out, sizeof(out)) &&
          strcmp(out, "EST5EDT,M3.2.0,M11.1.0") == 0);
    check("tz resolve: quoted names become letters",
          tz_resolve("<+0545>-5:45", out, sizeof(out)) && strcmp(out, "LCL-5:45") == 0);
    check("tz resolve: unknown IANA name refused",
          !tz_resolve("Antarctica/Troll", out, sizeof(out)) && out[0] == '\0');
    check("tz resolve: a bare city refused", !tz_resolve("Chicago", out, sizeof(out)));
    check("tz resolve: empty refused", !tz_resolve("", out, sizeof(out)));
    check("tz 12h: New York reads 12-hour", tz_default_12h("America/New_York"));
    check("tz 12h: Sao Paulo reads 24-hour", !tz_default_12h("America/Sao_Paulo"));
    check("tz 12h: London reads 24-hour", !tz_default_12h("Europe/London"));
    check("tz 12h: a POSIX rule says nothing, so 24-hour",
          !tz_default_12h("EST5EDT,M3.2.0,M11.1.0"));
}

int main(void)
{
    chicago("America/Chicago");
    chicago("CST6CDT,M3.2.0,M11.1.0");
    london("Europe/London");
    london("GMT0BST,M3.5.0/1,M10.5.0");
    plain();
    missed();
    parsing();
    table();
    if (failures) {
        printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    return 0;
}
