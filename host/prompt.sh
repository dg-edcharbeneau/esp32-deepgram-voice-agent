#!/bin/sh
# Print the assembled system prompt, no device involved.
#
#   ./prompt.sh                 the prompt as this build's sdkconfig would send it
#   ./prompt.sh --resumed       what a session reopened by a voice change sends
#
# Everything here compiles main/agent_prompt.c itself, so what it prints is what
# the device assembles.
set -e
cd "$(dirname "$0")"

ARGS=""
for arg in "$@"; do
    case "$arg" in
        --resumed)  ARGS="--resumed" ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

mkdir -p build
python3 prompt_blobs.py ../main/prompt > build/prompt_blobs.S

# Compiled from a copy, because a quoted include resolves against the including
# FILE's directory before any -I: left in main/, agent_prompt.c would pull in the
# real faces.h and with it cJSON. Copied fresh every run, so it cannot drift.
cp ../main/agent_prompt.c build/agent_prompt.c
cc -O2 -std=gnu11 -Wall -Wextra -Iprompt_stubs -I../main \
   -o build/prompt_dump prompt_dump.c build/agent_prompt.c build/prompt_blobs.S
exec ./build/prompt_dump $ARGS
