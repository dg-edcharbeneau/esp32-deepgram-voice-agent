# Host harnesses

Two things that run on a laptop instead of the board: a geometry parity check,
and a dump of the assembled system prompt.

## Geometry parity

Diffs `main/orb_geometry.c` against the upstream TypeScript it was transcribed
from, so a transcription error is caught here rather than by squinting at a
466 px panel.

    ./run.sh

`main/orb_geometry.c` is deliberately free of LVGL and ESP-IDF, which is what
lets the same source compile for the host.

### What is compared

Fourteen frames: all eight behaviours at a fixed `t`, two of them at a non-zero
amplitude, four idle timestamps chosen to catch epochs with and without a
gesture playing, and one mid-transition frame where the wavefront sign
convention is decided.

Each frame is 456 dots, compared dot by dot across all six output fields
(x, y, z, radius, ink, alpha) -- 38,304 numbers a run.

Tolerance is 0.02 px. Observed worst deviation is 0.0043, which is float32
against the reference's doubles -- the port keeps the shell in `float` because
the device has a single-precision FPU.

Draw order is compared first, because it is part of the contract. It is expected
to disagree: the reference sorts on depth alone, where the port buckets by row
band and then depth for PSRAM locality. So `compare.py` falls back to matching
the two frames as a MULTISET -- every reference dot must find one unmatched port
dot agreeing on all six fields -- and prints "same dots, port's own draw order"
when that is what it found.

### What is not compared

`voice_pass()` -- the band-driven swell, ripple, fatten and ink shift. It is this
project's own addition rather than a transcription: the reference's `buildVoice`
takes no band input at all, so there is no upstream frame to diff it against.
`orb_dump.c` therefore passes no bands, and the pass composes over the finished
dot list precisely so it can stay out of the way of what is checked here.

### Why `idle` took a second attempt

`idle` was excluded at first because `idleW = 1` switches on the body layer --
float, breath, squash-and-stretch, spin drift and a projector `roll` -- and
thinking-orbs' `makeProj` has no `roll` argument at all. `ref/core.ts` now carries
a faithful transcription of expo-thinking-orbs' version instead: `roll` is a 2-D
rotation in SCREEN space applied after yaw and tilt and before the scale, and it
leaves z untouched, so it cannot reorder the painter's sort. `orient` is still
unimplemented and throws rather than guess, since nothing here supplies one.

### Three findings worth keeping

- `hashD` must be computed in **double**. It is a chaotic sine hash; its argument
  reaches ~6200, where float32 has ~4e-4 of precision, and the subsequent
  multiply by 43758 turns that into an unrelated number. In float32 the scatter
  table is noise that still looks plausible on screen.
- The body breath (`bodyScale`, 3%) is not gated on idle in the reference. Missing
  it produced a systematic ~4 px error in x and y that no single behaviour made
  obvious.
- A shim that **throws** beats a shim that guesses. The `roll` guard is what
  located the idle boundary precisely, instead of quietly producing plausible
  numbers that were wrong.

## The system prompt

    ./prompt.sh                 as this build's sdkconfig would send it
    ./prompt.sh --resumed       what a session reopened by a voice change sends
    ./prompt.sh --nova          the Nova-3 + Aura build
    ./prompt.sh --barge-in      with MIC_GATE_WHILE_AGENT_SPEAKS off

A prompt edit is a words change, and waiting on a flash to read the words back is
the slowest possible way to review one.

This compiles `main/agent_prompt.c` itself, so the block order, the build gating
and the `{{placeholder}}` expansion are the shipping code rather than a copy of
it. What is faked is only the ESP-IDF around it — `prompt_stubs/` supplies
`heap_caps_malloc`, the log macros and the Kconfig defines — plus the three
catalogs, which are tables of strings the assembler never interprets.

`prompt_blobs.py` generates the `_binary_*_start` / `_binary_*_end` symbols that
`EMBED_TXTFILES` makes on the device. It emits assembly rather than C because
`_end` is a label PAST the data, which C cannot express: a pointer variable at
that symbol would hand `agent_prompt.c` the address of the pointer, and every
block would come out the wrong length.

One wrinkle worth knowing before you edit `prompt.sh`: `agent_prompt.c` is
compiled from a copy in `build/`. A quoted include resolves against the
including file's own directory before any `-I`, so left in `main/` it would pull
in the real `faces.h` and with it cJSON.
