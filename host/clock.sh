#!/bin/sh
# Exercise main/clock_face.c on a laptop: the orb clock lights the right dots
# for a time, its rings sit where the mockup puts them, and the meridian and the
# text fade behave.
#
#   ./clock.sh            run the checks
#   ./clock.sh DIR        ... and write one frame at 2:38 to DIR/466 and DIR/800 as PGM images
#
# Built and run once per panel size: 466 (the S3) and 800 (the P4).
#
# The module is plain C over libm, so it compiles as is.
set -e
cd "$(dirname "$0")"

mkdir -p build
for size in 466 800; do
    echo "== PANEL_SIZE $size"
    cc -O2 -DPANEL_SIZE=$size -std=gnu11 -Wall -Wextra -I../main -o build/clock_test_$size clock_test.c ../main/clock_face.c -lm
    if [ $# -gt 0 ]; then
        mkdir -p "$1/$size"
        ./build/clock_test_$size "$1/$size"
    else
        ./build/clock_test_$size
    fi
done
