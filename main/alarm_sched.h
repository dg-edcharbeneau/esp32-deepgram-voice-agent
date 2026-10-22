/*
 * The arithmetic of timers and alarms, with nothing else attached.
 *
 * No ESP-IDF, no FreeRTOS, no NVS -- only libc's time functions -- so the same
 * source compiles on a laptop and host/alarms.sh can walk it across daylight
 * saving transitions, which is where alarm clocks are wrong. alarms.c owns the
 * table, the locking, persistence and what happens when something rings; this
 * file only answers "when is next?" and "what did the user mean?".
 *
 * LOCAL TIME IS WHATEVER TZ SAYS. Every function here reads the process
 * timezone through localtime_r()/mktime(), so the caller must have applied the
 * zone (setenv + tzset) before asking. On the device that is clock.c, on the
 * host it is the test.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ALARM_MAX       8
#define ALARM_LABEL_MAX 24

/*
 * How late a timer or a one-shot alarm may still ring after the device was off
 * across its moment. Inside the window it rings as soon as the clock is known
 * -- a reboot during the pasta timer should not cost the pasta. Outside it, it
 * is dropped: "wake me at 7" ringing at noon is worse than not ringing.
 * Weekly alarms never ring late; they roll forward to the next occurrence.
 */
#define ALARM_MISS_WINDOW_S (10 * 60)

/* Every weekday bit, in tm_wday order: bit 0 is Sunday, bit 6 Saturday. */
#define ALARM_DAYS_ALL 0x7F

typedef enum {
    ALARM_EMPTY  = 0,
    ALARM_TIMER  = 1,   /* "in ten minutes": fire_epoch is the whole definition */
    ALARM_ONCE   = 2,   /* "at 7:30": the next 7:30, then gone */
    ALARM_WEEKLY = 3,   /* "at 7:30 on weekdays": hh:mm and dow_mask define it */
} alarm_kind_t;

/*
 * One entry, as it sits in the NVS blob. Fixed-width fields throughout, and the
 * epoch is spelled int64_t rather than time_t, so the host test and newlib agree
 * on the layout byte for byte.
 *
 * For a weekly alarm fire_epoch is the NEXT occurrence, recomputed on load and
 * after every ring. It is stored only because the slot has room for it; nothing
 * trusts the stored value, which is what lets a recurring alarm ring forever
 * without a single flash write.
 */
typedef struct {
    uint8_t id;          /* 1..255, 0 never used, so a zeroed slot is empty */
    uint8_t kind;        /* alarm_kind_t */
    uint8_t hh;
    uint8_t mm;
    uint8_t dow_mask;    /* weekly only */
    uint8_t reserved[3];
    int64_t fire_epoch;  /* UTC seconds */
    char label[ALARM_LABEL_MAX];
} alarm_entry_t;

/*
 * The next instant, strictly after `now`, at which local wall time reads hh:mm
 * on a weekday in `dow_mask`. Returns -1 for an empty mask or a bad time.
 *
 * DAYLIGHT SAVING IS DECIDED HERE, NOT LEFT TO mktime(). The C library's
 * normalisation of a wall time that does not exist, or exists twice, differs
 * between newlib and macOS, and an alarm clock cannot have two behaviours:
 *   - in the spring-forward gap ("2:30" on a night that skips 2:00-3:00) it
 *     rings at the first minute that does exist, 3:00;
 *   - in the fall-back overlap ("1:30" on a night that has two) it rings once,
 *     at the first of them.
 * Both are proved by converting the answer back with localtime_r().
 */
int64_t alarm_next_local(int64_t now, int hh, int mm, uint8_t dow_mask);

/*
 * True if a timer or one-shot alarm's moment passed more than
 * ALARM_MISS_WINDOW_S before `now` -- it should be dropped rather than rung.
 * Always false for a weekly alarm, which rolls forward instead.
 */
bool alarm_is_stale(const alarm_entry_t *e, int64_t now);

/* Index of the occupied entry that fires soonest, or -1 if there is none. */
int alarm_soonest(const alarm_entry_t *table, size_t n);

/*
 * "07:30", "7:30", "19:05". Twenty-four hour only: the model converts "7:30 pm"
 * before it calls, which the tool description asks for, and a parser that
 * guessed at am/pm would be guessing on the one thing that must not be wrong.
 */
bool alarm_parse_hhmm(const char *s, int *hh, int *mm);

/*
 * "daily", "weekdays", "weekends", or day names in any mix of case,
 * separators and length ("mon,wed", "Monday Friday", "tue thu sat").
 * Returns the mask, or 0 if any word was not understood -- a half-parsed
 * schedule rings on the wrong days, so it is refused whole.
 */
uint8_t alarm_parse_days(const char *s);

/*
 * Copy a label down to what the display can draw: printable ASCII, collapsed
 * whitespace, trimmed, at most cap-1 bytes. The panel's Montserrat is built for
 * 0x20-0x7E only and anything else renders as a box.
 */
void alarm_label_sanitize(char *dst, size_t cap, const char *src);

/*
 * A wall time as people read it: "8:52 PM", "12:05 AM", "12:00 PM" in 12-hour
 * (midnight is 12 AM, noon 12 PM, and no leading zero), "20:52" in 24-hour.
 * Writes at most cap-1 bytes; 9 holds either form.
 */
void alarm_fmt_time(int hh, int mm, bool h12, char *out, size_t cap);

/*
 * Days as a short phrase for speech and the display: "every day", "weekdays",
 * "weekends", or "Mon Wed Fri". Writes at most cap-1 bytes.
 */
void alarm_days_phrase(uint8_t mask, char *out, size_t cap);
