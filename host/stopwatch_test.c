/*
 * Host checks for main/stopwatch.c; run by host/alarms.sh.
 */
#include <stdio.h>
#include <string.h>

#include "stopwatch.h"

static int failures;

static void check(const char *what, int ok)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        failures++;
    }
}

static int fmt_is(uint32_t ms, const char *want_main, const char *want_frac)
{
    char m[12], f[12];
    sw_fmt(ms, m, sizeof(m), f, sizeof(f));
    if (strcmp(m, want_main) != 0 || strcmp(f, want_frac) != 0) {
        printf("    %u ms gave \"%s\" \"%s\"\n", (unsigned)ms, m, f);
        return 0;
    }
    return 1;
}

int main(void)
{
    sw_t sw = {0};
    check("stopwatch: starts ready at zero", sw.state == SW_READY && sw_elapsed(&sw, 5000) == 0);

    sw_toggle(&sw, 1000);
    check("stopwatch: running counts from the start", sw.state == SW_RUNNING &&
                                                          sw_elapsed(&sw, 3500) == 2500);
    sw_toggle(&sw, 4000);
    check("stopwatch: pause holds the total", sw.state == SW_PAUSED &&
                                                  sw_elapsed(&sw, 9000) == 3000);
    sw_toggle(&sw, 10000);
    check("stopwatch: resume adds on from there", sw_elapsed(&sw, 10500) == 3500);

    check("stopwatch: lap while running counts", sw_lap(&sw, 11000) && sw.laps == 1 &&
                                                     sw.lap_ms == 4000);
    check("stopwatch: a second lap keeps only the latest", sw_lap(&sw, 12000) && sw.laps == 2 &&
                                                               sw.lap_ms == 5000);
    check("stopwatch: reset refused while running", !sw_reset(&sw) && sw.state == SW_RUNNING);

    sw_toggle(&sw, 13000);
    check("stopwatch: lap refused while paused", !sw_lap(&sw, 13500) && sw.laps == 2);
    check("stopwatch: reset when paused clears time and laps",
          sw_reset(&sw) && sw.state == SW_READY && sw.laps == 0 && sw_elapsed(&sw, 99999) == 0);
    check("stopwatch: reset when ready does nothing", !sw_reset(&sw));
    check("stopwatch: lap when ready does nothing", !sw_lap(&sw, 1) && sw.laps == 0);

    /* Started just before the 32-bit ms wrap. */
    sw_t w = {0};
    sw_toggle(&w, 0xFFFFF000u);
    check("stopwatch: runs across the ms wrap", sw_elapsed(&w, 0x00001000u) == 0x2000u);

    check("stopwatch: format 0", fmt_is(0, "00:00", ".00"));
    check("stopwatch: format 12:34.56", fmt_is((12 * 60 + 34) * 1000 + 560, "12:34", ".56"));
    check("stopwatch: format 59:59.99", fmt_is(3599990, "59:59", ".99"));
    check("stopwatch: past an hour drops the centiseconds", fmt_is(3723000, "1:02:03", ""));

    printf("\n%s\n", failures ? "FAILURES" : "all stopwatch checks pass");
    return failures ? 1 : 0;
}
