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

## What is NOT compared, and why

`idle` is excluded. `buildVoice` gives it `idleW = 1`, which switches on the body
layer -- float, breath, squash-and-stretch, spin drift, and a projector roll --
and that layer is not ported yet. Verifying it needs expo-thinking-orbs' own
`core.ts`, whose `makeProj` takes `roll`/`orient` arguments that
thinking-orbs' does not. `ref/core.ts` here carries a clearly-marked shim that
throws rather than silently guessing if either is ever non-zero.

Note that the body *breath* is separate and IS ported: the reference computes it
ungated by `idleW`, so it applies to every behaviour.

## Two findings worth keeping

- `hashD` must be computed in **double**. It is a chaotic sine hash; its argument
  reaches ~6200, where float32 has ~4e-4 of precision, and the subsequent
  multiply by 43758 turns that into an unrelated number. In float32 the scatter
  table is noise that still looks plausible on screen.
- The body breath (`bodyScale`, 3%) is not gated on idle in the reference. Missing
  it produced a systematic ~4 px error in x and y that no single behaviour made
  obvious.
