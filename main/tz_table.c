#include "tz_table.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

/*
 * Rules as of the 2024b tz database, written in the form posix_tz_db
 * (github.com/nayarsystems/posix_tz_db) generates from it -- with one change:
 * every zone that database names with a quoted numeric abbreviation, <+04>-4,
 * is given a lettered one here, GST-4. The abbreviation is never shown or
 * spoken; only the offset and the rule matter, and lettered names parse
 * everywhere.
 *
 * THE RULES IN FORCE FROM NOVEMBER 2026 ON, not the history. A rule string
 * cannot say "until": British Columbia, Alberta and Manitoba keep summer time
 * permanently from 1 Nov 2026 (tzdata 2026e), so Vancouver, Edmonton and
 * Winnipeg are written as the fixed offsets they become -- and a device in
 * one of them before that date is an hour off, once, rather than forever after.
 *
 * THE LAST COLUMN IS THE CLOCK FORMAT the zone's people read by default: true
 * for 12-hour (most of the Americas, the US Pacific, Australia, New Zealand,
 * India, Pakistan, Bangladesh, the Philippines, Egypt, Saudi Arabia), false for
 * 24-hour. A default only -- "use 24-hour time" overrides it, and is saved.
 * Brazil, Argentina, Uruguay and Chile write clocks in 24-hour and are marked
 * so, though their neighbours are not.
 *
 * Ireland is written as GMT0IST rather than the database's IST-1GMT0 with its
 * negative summer offset: the same wall clock all year, in the form every C
 * library has handled for decades.
 */
static const tz_entry_t TZ[] = {
    /* North America */
    {"America/New_York",      "EST5EDT,M3.2.0,M11.1.0", true},
    {"America/Detroit",       "EST5EDT,M3.2.0,M11.1.0", true},
    {"America/Toronto",       "EST5EDT,M3.2.0,M11.1.0", true},
    {"America/Indiana/Indianapolis", "EST5EDT,M3.2.0,M11.1.0", true},
    {"America/Kentucky/Louisville",  "EST5EDT,M3.2.0,M11.1.0", true},
    {"America/Chicago",       "CST6CDT,M3.2.0,M11.1.0", true},
    {"America/Winnipeg",      "EST5", true},
    {"America/Regina",        "CST6", true},
    {"America/Denver",        "MST7MDT,M3.2.0,M11.1.0", true},
    {"America/Boise",         "MST7MDT,M3.2.0,M11.1.0", true},
    {"America/Edmonton",      "CST6", true},
    {"America/Phoenix",       "MST7", true},
    {"America/Los_Angeles",   "PST8PDT,M3.2.0,M11.1.0", true},
    {"America/Vancouver",     "MST7", true},
    {"America/Tijuana",       "PST8PDT,M3.2.0,M11.1.0", true},
    {"America/Anchorage",     "AKST9AKDT,M3.2.0,M11.1.0", true},
    {"America/Juneau",        "AKST9AKDT,M3.2.0,M11.1.0", true},
    {"Pacific/Honolulu",      "HST10", true},
    {"America/Halifax",       "AST4ADT,M3.2.0,M11.1.0", true},
    {"America/St_Johns",      "NST3:30NDT,M3.2.0,M11.1.0", true},
    {"America/Puerto_Rico",   "AST4", true},
    {"US/Eastern",            "EST5EDT,M3.2.0,M11.1.0", true},
    {"US/Central",            "CST6CDT,M3.2.0,M11.1.0", true},
    {"US/Mountain",           "MST7MDT,M3.2.0,M11.1.0", true},
    {"US/Arizona",            "MST7", true},
    {"US/Pacific",            "PST8PDT,M3.2.0,M11.1.0", true},
    {"US/Alaska",             "AKST9AKDT,M3.2.0,M11.1.0", true},
    {"US/Hawaii",             "HST10", true},

    /* Central and South America, Caribbean */
    {"America/Mexico_City",   "CST6", true},
    {"America/Monterrey",     "CST6", true},
    {"America/Cancun",        "EST5", true},
    {"America/Guatemala",     "CST6", true},
    {"America/Costa_Rica",    "CST6", true},
    {"America/El_Salvador",   "CST6", true},
    {"America/Panama",        "EST5", true},
    {"America/Jamaica",       "EST5", true},
    {"America/Havana",        "CST5CDT,M3.2.0/0,M11.1.0/1", true},
    {"America/Santo_Domingo", "AST4", true},
    {"America/Bogota",        "COT5", true},
    {"America/Lima",          "PET5", true},
    {"America/Guayaquil",     "ECT5", true},
    {"America/Caracas",       "VET4", true},
    {"America/La_Paz",        "BOT4", true},
    {"America/Santiago",      "CLT4CLST,M9.1.6/24,M4.1.6/24", false},
    {"America/Sao_Paulo",     "BRT3", false},
    {"America/Argentina/Buenos_Aires", "ART3", false},
    {"America/Buenos_Aires",  "ART3", false},
    {"America/Montevideo",    "UYT3", false},

    /* Europe */
    {"Europe/London",         "GMT0BST,M3.5.0/1,M10.5.0", false},
    {"Europe/Dublin",         "GMT0IST,M3.5.0/1,M10.5.0", false},
    {"Europe/Lisbon",         "WET0WEST,M3.5.0/1,M10.5.0", false},
    {"Atlantic/Canary",       "WET0WEST,M3.5.0/1,M10.5.0", false},
    {"Atlantic/Reykjavik",    "GMT0", false},
    {"Europe/Paris",          "CET-1CEST,M3.5.0,M10.5.0/3", false},
    {"Europe/Berlin",         "CET-1CEST,M3.5.0,M10.5.0/3", false},
    {"Europe/Madrid",         "CET-1CEST,M3.5.0,M10.5.0/3", false},
    {"Europe/Rome",           "CET-1CEST,M3.5.0,M10.5.0/3", false},
    {"Europe/Amsterdam",      "CET-1CEST,M3.5.0,M10.5.0/3", false},
    {"Europe/Brussels",       "CET-1CEST,M3.5.0,M10.5.0/3", false},
    {"Europe/Luxembourg",     "CET-1CEST,M3.5.0,M10.5.0/3", false},
    {"Europe/Zurich",         "CET-1CEST,M3.5.0,M10.5.0/3", false},
    {"Europe/Vienna",         "CET-1CEST,M3.5.0,M10.5.0/3", false},
    {"Europe/Prague",         "CET-1CEST,M3.5.0,M10.5.0/3", false},
    {"Europe/Budapest",       "CET-1CEST,M3.5.0,M10.5.0/3", false},
    {"Europe/Warsaw",         "CET-1CEST,M3.5.0,M10.5.0/3", false},
    {"Europe/Copenhagen",     "CET-1CEST,M3.5.0,M10.5.0/3", false},
    {"Europe/Stockholm",      "CET-1CEST,M3.5.0,M10.5.0/3", false},
    {"Europe/Oslo",           "CET-1CEST,M3.5.0,M10.5.0/3", false},
    {"Europe/Belgrade",       "CET-1CEST,M3.5.0,M10.5.0/3", false},
    {"Europe/Zagreb",         "CET-1CEST,M3.5.0,M10.5.0/3", false},
    {"Europe/Athens",         "EET-2EEST,M3.5.0/3,M10.5.0/4", false},
    {"Europe/Helsinki",       "EET-2EEST,M3.5.0/3,M10.5.0/4", false},
    {"Europe/Kyiv",           "EET-2EEST,M3.5.0/3,M10.5.0/4", false},
    {"Europe/Kiev",           "EET-2EEST,M3.5.0/3,M10.5.0/4", false},
    {"Europe/Bucharest",      "EET-2EEST,M3.5.0/3,M10.5.0/4", false},
    {"Europe/Sofia",          "EET-2EEST,M3.5.0/3,M10.5.0/4", false},
    {"Europe/Riga",           "EET-2EEST,M3.5.0/3,M10.5.0/4", false},
    {"Europe/Tallinn",        "EET-2EEST,M3.5.0/3,M10.5.0/4", false},
    {"Europe/Vilnius",        "EET-2EEST,M3.5.0/3,M10.5.0/4", false},
    {"Europe/Istanbul",       "TRT-3", false},
    {"Europe/Minsk",          "MSK-3", false},
    {"Europe/Moscow",         "MSK-3", false},

    /* Africa */
    /* An hour fast during Ramadan, when Morocco drops to UTC+0 on dates set
     * each year by observation -- no POSIX rule can say that. */
    {"Africa/Casablanca",     "WAT-1", false},
    {"Africa/Algiers",        "CET-1", false},
    {"Africa/Lagos",          "WAT-1", false},
    {"Africa/Accra",          "GMT0", false},
    {"Africa/Cairo",          "EET-2EEST,M4.5.5/0,M10.5.4/24", true},
    {"Africa/Johannesburg",   "SAST-2", false},
    {"Africa/Nairobi",        "EAT-3", false},
    {"Africa/Addis_Ababa",    "EAT-3", false},

    /* Middle East and Asia */
    {"Asia/Jerusalem",        "IST-2IDT,M3.4.4/26,M10.5.0", false},
    {"Asia/Beirut",           "EET-2EEST,M3.5.0/0,M10.5.0/0", false},
    {"Asia/Riyadh",           "AST-3", true},
    {"Asia/Qatar",            "AST-3", false},
    {"Asia/Baghdad",          "AST-3", false},
    {"Asia/Tehran",           "IRST-3:30", false},
    {"Asia/Dubai",            "GST-4", false},
    {"Asia/Baku",             "AZT-4", false},
    {"Asia/Kabul",            "AFT-4:30", false},
    {"Asia/Karachi",          "PKT-5", true},
    {"Asia/Tashkent",         "UZT-5", false},
    {"Asia/Almaty",           "ALMT-5", false},
    {"Asia/Kolkata",          "IST-5:30", true},
    {"Asia/Calcutta",         "IST-5:30", true},
    {"Asia/Colombo",          "IST-5:30", false},
    {"Asia/Kathmandu",        "NPT-5:45", false},
    {"Asia/Dhaka",            "BST-6", true},
    {"Asia/Yangon",           "MMT-6:30", false},
    {"Asia/Bangkok",          "ICT-7", false},
    {"Asia/Ho_Chi_Minh",      "ICT-7", false},
    {"Asia/Jakarta",          "WIB-7", false},
    {"Asia/Singapore",        "SGT-8", false},
    {"Asia/Kuala_Lumpur",     "MYT-8", false},
    {"Asia/Manila",           "PST-8", true},
    {"Asia/Shanghai",         "CST-8", false},
    {"Asia/Hong_Kong",        "HKT-8", false},
    {"Asia/Taipei",           "CST-8", false},
    {"Australia/Perth",       "AWST-8", true},
    {"Asia/Seoul",            "KST-9", false},
    {"Asia/Tokyo",            "JST-9", false},

    /* Oceania */
    {"Australia/Darwin",      "ACST-9:30", true},
    {"Australia/Adelaide",    "ACST-9:30ACDT,M10.1.0,M4.1.0/3", true},
    {"Australia/Brisbane",    "AEST-10", true},
    {"Australia/Sydney",      "AEST-10AEDT,M10.1.0,M4.1.0/3", true},
    {"Australia/Melbourne",   "AEST-10AEDT,M10.1.0,M4.1.0/3", true},
    {"Australia/Hobart",      "AEST-10AEDT,M10.1.0,M4.1.0/3", true},
    {"Pacific/Guam",          "ChST-10", true},
    {"Pacific/Auckland",      "NZST-12NZDT,M9.5.0,M4.1.0/3", true},
    {"Pacific/Fiji",          "FJT-12", false},

    /* No zone at all */
    {"UTC",                   "UTC0", false},
    {"Etc/UTC",               "UTC0", false},
    {"GMT",                   "GMT0", false},
};

#define TZ_COUNT (sizeof(TZ) / sizeof(TZ[0]))

const char *tz_lookup(const char *iana)
{
    if (iana == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < TZ_COUNT; i++) {
        if (strcasecmp(TZ[i].iana, iana) == 0) {
            return TZ[i].posix;
        }
    }
    return NULL;
}

/*
 * Loose on purpose: three or more letters (or a quoted name), then a sign or a
 * digit. Enough to tell "EST5EDT" from "Chicago" and "America/Chicago", which is
 * all it is for -- a malformed rule that gets past it fails safe in newlib, as
 * UTC, and the TLM line and the agent's own reading of the time say so.
 */
static bool looks_posix(const char *s)
{
    size_t i = 0;
    if (s[0] == '<') {
        const char *close = strchr(s, '>');
        if (close == NULL) {
            return false;
        }
        i = (size_t)(close - s) + 1;
    } else {
        while (isalpha((unsigned char)s[i])) {
            i++;
        }
        if (i < 3) {
            return false;
        }
    }
    return s[i] == '+' || s[i] == '-' || isdigit((unsigned char)s[i]);
}

bool tz_resolve(const char *in, char *out, size_t cap)
{
    if (cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (in == NULL || in[0] == '\0') {
        return false;
    }

    const char *rule = tz_lookup(in);
    if (rule != NULL) {
        if (strlen(rule) >= cap) {
            return false;
        }
        strcpy(out, rule);
        return true;
    }
    if (strchr(in, '/') != NULL || !looks_posix(in)) {
        return false;   /* an IANA name the table does not hold: refuse, never guess */
    }

    /* Copy, replacing each <...> with a placeholder name of letters. */
    size_t n = 0;
    for (const char *p = in; *p; p++) {
        if (*p == '<') {
            const char *close = strchr(p, '>');
            if (close == NULL) {
                out[0] = '\0';
                return false;
            }
            static const char NAME[] = "LCL";
            if (n + sizeof(NAME) - 1 >= cap) {
                out[0] = '\0';
                return false;
            }
            memcpy(out + n, NAME, sizeof(NAME) - 1);
            n += sizeof(NAME) - 1;
            p = close;
            continue;
        }
        if (isspace((unsigned char)*p) || n + 1 >= cap) {
            out[0] = '\0';
            return false;
        }
        out[n++] = *p;
    }
    out[n] = '\0';
    return true;
}

bool tz_default_12h(const char *iana)
{
    if (iana == NULL) {
        return false;
    }
    for (size_t i = 0; i < TZ_COUNT; i++) {
        if (strcasecmp(TZ[i].iana, iana) == 0) {
            return TZ[i].h12;
        }
    }
    return false;
}

const tz_entry_t *tz_table(size_t *count)
{
    *count = TZ_COUNT;
    return TZ;
}
