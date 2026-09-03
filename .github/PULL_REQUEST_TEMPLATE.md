## What and why

<!-- One concern per PR. If this overturns an earlier measurement, say so. -->

## How it was verified

<!--
"Builds" is not verification for anything touching audio, timing or RAM.
Check what applies:
-->

- [ ] `idf.py build` against ESP-IDF v5.5.5
- [ ] Flashed and exercised on a real ESP32-S3-Touch-AMOLED-1.75C
- [ ] `./host/run.sh` passes (required if `main/orb_geometry.c` changed)
- [ ] `./host/prompt.sh` diffed before/after (required if `main/prompt/*` or
      `main/agent_prompt.c` changed)
- [ ] `./host/store.sh` passes (required if `main/history_store.c` changed)
- [ ] `./components/tcp_transport/check-patch.sh` passes
- [ ] Stack frames measured (required if locals, buffers or a stack size grew)
- [ ] Before/after `TLM` telemetry attached below

<!-- Paste the telemetry pair, or say why it does not apply. -->

## Anything a reviewer should know

<!-- Comments you removed that recorded a measurement, and what replaced them. -->
