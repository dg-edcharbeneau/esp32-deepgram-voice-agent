#!/bin/sh
# Exercise main/panel_geom.c on a laptop: every touch-panel control is hit where
# it is drawn, the gaps and rounded corners hit nothing, and the colour picker's
# clock positions map onto the thirteen orb colours and back.
#
#   ./panel.sh            run the checks
#   ./panel.sh DIR        ... and write each page's shapes to DIR as PGM images
#
# The module is plain C over libm, so it compiles as is.
set -e
cd "$(dirname "$0")"

mkdir -p build
cc -O2 -std=gnu11 -Wall -Wextra -I../main \
   -o build/panel_test panel_test.c ../main/panel_geom.c -lm
./build/panel_test "$@"
