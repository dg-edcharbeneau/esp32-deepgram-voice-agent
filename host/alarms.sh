#!/bin/sh
# Exercise main/alarm_sched.c on a laptop: next-occurrence arithmetic across both
# daylight saving transitions in two zones, the week and year wrapping, the
# missed-alarm window, the parsers the agent's tools lean on, and every row of
# the IANA-to-POSIX timezone table against the laptop's own zone database; and
# main/stopwatch.c.
#
#   ./alarms.sh
#
# The module is plain C over libc's time functions, so it compiles as is.
set -e
cd "$(dirname "$0")"

mkdir -p build
cc -O2 -std=gnu11 -Wall -Wextra -I../main \
   -o build/alarms_test alarms_test.c ../main/alarm_sched.c ../main/tz_table.c
./build/alarms_test
cc -O2 -std=gnu11 -Wall -Wextra -I../main \
   -o build/stopwatch_test stopwatch_test.c ../main/stopwatch.c
./build/stopwatch_test
