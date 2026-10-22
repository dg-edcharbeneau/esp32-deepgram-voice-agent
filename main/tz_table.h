/*
 * IANA timezone names to the POSIX rules newlib can apply.
 *
 * newlib has no zone database, only TZ rule strings: "America/Chicago" means
 * nothing to it, "CST6CDT,M3.2.0,M11.1.0" means everything. People and phones
 * speak IANA -- the model knows "I'm in Chicago" is America/Chicago, and a
 * browser reports its zone that way -- so this table is the translation, in
 * flash rodata at no RAM cost.
 *
 * It is a curated subset, about a hundred zones covering where people are, not
 * all six hundred. A name it does not know is refused rather than guessed, and
 * a caller holding a POSIX rule already may pass that straight through.
 *
 * host/alarms.sh checks every row against the laptop's own zone database across
 * a full year, so a rule typed wrong here fails there rather than on the night
 * the clocks change.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *iana;
    const char *posix;
    bool h12;   /* the zone reads 12-hour clocks by default */
} tz_entry_t;

/*
 * The POSIX rule for an IANA name, case-insensitively, or NULL. A few aliases
 * the model and older browsers still use (Asia/Calcutta, US/Eastern, UTC) are
 * rows of their own.
 */
const char *tz_lookup(const char *iana);

/*
 * Resolve whatever the user or the model supplied into a rule newlib will
 * accept, writing it to `out`. An IANA name goes through the table; anything
 * that already looks like a POSIX rule (letters followed by an offset) is taken
 * as given, with any <+05>-style quoted name rewritten to letters, since
 * newlib's support for the quoted form is not something to bet the clock on.
 * Returns false, leaving `out` empty, for anything else.
 */
bool tz_resolve(const char *in, char *out, size_t cap);

/*
 * Whether people in this zone read 12-hour clocks by default. False for a name
 * the table does not hold, and for a raw POSIX rule, which says nothing about
 * where it is. The user's own choice, when there is one, wins: see clock_12h().
 */
bool tz_default_12h(const char *iana);

/* The table itself, for the host test. */
const tz_entry_t *tz_table(size_t *count);
