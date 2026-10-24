#!/bin/sh
# Exercise main/clock_face.c on a laptop: the orb clock lights the right dots
# for a time, its rings sit where the mockup puts them, and the meridian and the
# text fade behave.
#
#   ./clock.sh            run the checks
#   ./clock.sh DIR        ... and write one frame at 2:38 to DIR as a PGM image
#
# The module is plain C over libm, so it compiles as is.
set -e
cd "$(dirname "$0")"

mkdir -p build
cc -O2 -std=gnu11 -Wall -Wextra -I../main \
   -o build/clock_test clock_test.c ../main/clock_face.c -lm
./build/clock_test "$@"
