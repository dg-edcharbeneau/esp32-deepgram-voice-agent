#!/bin/sh
# Exercise main/boot_button.c on a laptop: every way BOOT can be pressed while an
# alarm rings -- click, double click, the 3-second hold that forgets wi-fi --
# and that each means what it always did once the alarm is gone.
#
#   ./button.sh             run the checks
#   HOST_LOG=1 ./button.sh  ... and show what the module logs
#
# The real module is compiled; iot_button and everything it calls are fakes in
# host/button_stubs that record what they were asked to do.
set -e
cd "$(dirname "$0")"

mkdir -p build
# Compiled from a copy, as prompt.sh does it: a quoted include resolves against
# the including FILE's directory before any -I, so left in main/ the module
# would pull in the real alarms.h, ui.h and the rest instead of the fakes.
cp ../main/boot_button.c build/boot_button.c
# -Wno-unused-parameter as the firmware build has it: callbacks ignore half
# their arguments by design.
cc -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Ibutton_stubs -I../main \
   -o build/button_test button_test.c build/boot_button.c
if [ -n "$HOST_LOG" ]; then
    exec env HOST_LOG=1 ./build/button_test
fi
exec ./build/button_test
