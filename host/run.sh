#!/bin/sh
# Verify the C geometry port against the upstream TypeScript.
#
# Requires node >= 22 (for native TypeScript type stripping) and a C compiler.
#
# gnu11 rather than c11: strict ISO mode defines __STRICT_ANSI__, and glibc then
# hides M_PI from math.h, which orb_geometry.c uses. macOS exposes it either way,
# so c11 worked here and broke on Linux CI. gnu11 is also the dialect ESP-IDF
# compiles this file with, which is the right one for a parity harness anyway.
# Nothing here touches the device or the firmware build.
set -e
cd "$(dirname "$0")"

echo "building C port..."
cc -O2 -std=gnu11 -Wall -Wextra -I../main -o orb_dump orb_dump.c ../main/orb_geometry.c -lm

echo "dumping reference (node)..."
node orb_ref.mjs > ref.tsv

echo "dumping port (C)..."
./orb_dump > port.tsv

echo
python3 compare.py ref.tsv port.tsv
