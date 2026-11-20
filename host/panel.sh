#!/bin/sh
# Exercise main/panel_geom.c on a laptop: every touch-panel control is hit where
# it is drawn, the gaps and rounded corners hit nothing, and the colour picker's
# dial holds exactly main/orb_colors.c's catalog, linked in with host stubs.
#
#   ./panel.sh            run the checks
#   ./panel.sh DIR        ... and write each page's shapes to DIR/466 and DIR/800 as PGM images
#
# Built and run once per panel size: 466 (the S3) and 800 (the P4).
#
# panel_geom.c is plain C over libm; orb_colors.c takes voices_stubs/ for NVS,
# logging and cJSON.
set -e
cd "$(dirname "$0")"

mkdir -p build
for size in 466 800; do
    echo "== PANEL_SIZE $size"
    cc -O2 -DPANEL_SIZE=$size -std=gnu11 -Wall -Wextra -Ivoices_stubs -I../main -o build/panel_test_$size panel_test.c ../main/panel_geom.c ../main/orb_colors.c -lm
    if [ $# -gt 0 ]; then
        mkdir -p "$1/$size"
        ./build/panel_test_$size "$1/$size"
    else
        ./build/panel_test_$size
    fi
done
