# Geometry parity harness

Diffs `main/orb_geometry.c` against the upstream TypeScript it was transcribed
from, so a transcription error is caught here rather than by squinting at a
466 px panel.

    ./run.sh

`main/orb_geometry.c` is deliberately free of LVGL and ESP-IDF, which is what
lets the same source compile for the host.

## What is compared

Ten frames -- every behaviour at a fixed `t`, two of them at a non-zero
amplitude, plus one mid-transition frame where the wavefront sign convention is
decided. Each is compared dot by dot in draw order across all six output fields
(x, y, z, radius, ink, alpha).

Tolerance is 0.02 px. Observed worst deviation is 0.0043, which is float32
against the reference's doubles -- the port keeps the shell in `float` because
the device has a single-precision FPU.

## Everything is compared

Fourteen frames: all eight behaviours, two at non-zero amplitude, four idle
timestamps chosen to catch epochs with and without a gesture playing, and one
mid-transition frame where the wavefront sign convention is decided.

`idle` was excluded at first because `idleW = 1` switches on the body layer --
float, breath, squash-and-stretch, spin drift and a projector `roll` -- and
thinking-orbs' `makeProj` has no `roll` argument at all. `ref/core.ts` now carries
a faithful transcription of expo-thinking-orbs' version instead: `roll` is a 2-D
rotation in SCREEN space applied after yaw and tilt and before the scale, and it
leaves z untouched, so it cannot reorder the painter's sort. `orient` is still
unimplemented and throws rather than guess, since nothing here supplies one.

## Three findings worth keeping

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
