#!/bin/sh
# Verify the C geometry port against the upstream TypeScript.
#
# Requires node >= 22 (for native TypeScript type stripping) and a C compiler.
# Nothing here touches the device or the firmware build.
set -e
cd "$(dirname "$0")"

echo "building C port..."
cc -O2 -std=c11 -Wall -Wextra -I../main -o orb_dump orb_dump.c ../main/orb_geometry.c -lm

echo "dumping reference (node)..."
node orb_ref.mjs > ref.tsv

echo "dumping port (C)..."
./orb_dump > port.tsv

echo
python3 compare.py ref.tsv port.tsv
