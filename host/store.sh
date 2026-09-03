#!/bin/sh
# Exercise main/history_store.c on a laptop, against a fake NOR flash.
#
#   ./store.sh          run the checks
#   HOST_LOG=1 ./store.sh   ... and show what the module logs while doing it
#
# The real module is compiled; only the flash beneath it is faked, which is what
# makes "power lost mid-write" a thing that can be tested at all.
set -e
cd "$(dirname "$0")"

mkdir -p build
cc -O2 -std=gnu11 -Wall -Wextra -Istore_stubs -I../main \
   -o build/store_test store_test.c ../main/history_store.c store_stubs/fake_flash.c
./build/store_test

# The sequence-counter check, three processes over one file-backed flash: the
# module caches the newest slot in statics, so testing what a save does WITHOUT
# a preceding load needs fresh statics over an old flash. See store_test.c.
IMG=build/fake_flash.img
rm -f "$IMG"
export FAKE_FLASH_FILE="$IMG"
if ./build/store_test --seed && ./build/store_test --blind-save && \
   ./build/store_test --verify; then
    printf '%-58s %s\n' "a blind save across a restart still wins the ring" "ok"
else
    printf '%-58s %s\n' "a blind save across a restart still wins the ring" "FAILED"
    exit 1
fi
