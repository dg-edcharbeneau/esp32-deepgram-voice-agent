# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.4.0] - 2026-08-31

### Added

- **Battery.** The board's AXP2101 power-management IC has a fuel gauge, and
  nothing was reading it. A low-priority sampler (`main/battery.c`) now reads
  charge, cell voltage and charge direction off it every 5 s and three things
  use the result:
  - Four dots following the display's outer curve from 1 to 2 o'clock, a quarter
    of the charge each, in the orb's own colour -- spent ones at a quarter
    brightness of the same hue -- with a bolt while charging. On screen while stopped or asleep, and
    whenever the charge is low or the cell is charging -- not over a live
    conversation at full charge.
  - `bat=`, `mv=` and `chg=` on the TLM line, `bat=-1` when there is no reading.
  - `get_battery`, so "how much charge is left?" is answered out loud.

  Below `CONFIG_BATTERY_CRITICAL_PCT` (8%) the panel is held asleep -- it is the
  largest draw on the board -- while the session is left alone. The driver only
  ever *reads* the AXP2101: that chip also owns the display and codec rails, so
  a driver that configures regulators is a way to brown the panel out.
  `CONFIG_BATTERY` (default on) compiles the whole thing out.

  The overlay clears its own boxes every frame and once more when it stops being
  shown -- the canvas keeps its pixels, so an indicator that hides by simply not
  drawing leaves its last dots and its last bolt on screen for good.

  Verified on hardware: the PMU answers at 0x34, the gauge and voltage
  registers read 60% / 3.96 V, the charge/discharge bits in REG01 6:5 flip with
  the cable, the dots show and hide correctly, and the spoken answer came back
  right.

## [0.3.0] - 2026-08-30

### Added

- `set_volume`: "set your volume to 50" now lands on 50. The existing
  `adjust_volume` stays for relative changes ("a bit louder"); both go through
  the new `audio_io_set_volume()`. A request below the floor of 20 clamps to 20
  and the agent says so, since there is no mute.

## [0.2.0] - 2026-08-30

### Added

- **Full duplex, and it is the default.** The microphone stays open while the
  agent speaks, so it can be interrupted by talking. esp-sr's standalone AEC
  (`AEC_MODE_FD_LOW_COST`) runs in the capture path and the ES7210 moves to
  4-channel TDM so the hardware echo-reference lane is powered and sample
  aligned. `CONFIG_AEC_ENABLE` (default on) and
  `CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS` (default off) control it.
- `CONFIG_AEC_UPLINK_VAD`: during playback, only blocks whose **post-AEC** level
  clears a threshold are sent upstream. Without it the continuous 32 kB/s uplink
  starves the TLS path of internal DMA memory and the session drops -- the
  failure that ended the previous full-duplex attempt. It is a bandwidth fix,
  not an echo fix.
- `main/audio_codecs.c`: the BSP's audio bring-up with all four ES7210 inputs
  enabled, which is what powers the echo-reference lane.
- `main/heap_probe.c` (`CONFIG_HEAP_PROBE`, default off): allocation-failure hook
  and a 50 ms floor sampler. It names the size and caps of a failed allocation,
  which the `TLM` line cannot.
- `main/prompt/full-duplex.md`, selected against `half-duplex.md` by the same
  Kconfig symbol as the gate, so the model is never told it can be talked over by
  a build that gates the microphone.
- `vadsup` in the `TLM` line: capture blocks withheld from the uplink.

### Changed

- `CONFIG_AUDIO_OUT_VOLUME` defaults to 70 rather than 100. A preference, not a
  constraint -- 100 audibly clips on this speaker but full duplex works there.
- The microphone level log samples every 500 ms while the agent speaks instead of
  every 3 s, and reports the **post-AEC** level. On a fixed timer it mostly
  sampled silence, which is the only moment the canceller cannot be judged in.
- Documentation throughout `docs/` and the landing page now describe a device
  that can be talked over.

### Fixed

- `main/audio_io.c` claimed the canceller costs **16 bytes** of internal RAM,
  carried forward unchecked from an archived bench. Measured on hardware it costs
  **~14.7 kB**, and about 6 fps.

### Notes

Measured on this board: 22.4 dB mean ERLE at volume 70 and 23.2 dB at 100,
barge-in firing at both, and a ~22 minute session with zero allocation failures
and zero dropped frames. Worst observed largest free internal block is 9,216 B,
against the 1,630 B allocation whose failure kills the link.

The tap on the centre button still interrupts, in both builds. It works in any
room and needs no canceller.

## [0.1.0] - 2026-08-28

### Added

- Apache-2.0 license, `NOTICE`, `SECURITY.md`, `CODE_OF_CONDUCT.md` and
  `CONTRIBUTING.md`.
- GitHub Actions CI: firmware build in `espressif/idf:v5.5.5`, the vendored
  `tcp_transport` drift check, and the two host harnesses (`host/run.sh`,
  `host/prompt.sh`) as gates.
- A release workflow that publishes a single flashable `deepgram_agent-merged.bin`
  on a `v*` tag.
- `.editorconfig`, `.clang-format`, issue and pull-request templates.

### Changed

- `README.md` is now a landing page; the engineering detail moved into `docs/`
  unchanged. Investigation notes moved to `docs/notes/`.
- `components/tcp_transport/check-patch.sh` decides pass/fail on SHA-256 digests
  (`baseline.sha256`) instead of on committed diff text, which was not portable
  between Apple/FreeBSD `diff` and GNU `diffutils` and so could not run in CI.
  `local.patch` is unchanged and remains the human-readable record.
- The host harnesses compile with `-std=gnu11`. Under glibc, `-std=c11` defines
  `__STRICT_ANSI__` and `math.h` then hides `M_PI`, which `orb_geometry.c` uses;
  macOS exposes it either way, so the harnesses had only ever run there.

First tagged release. The firmware itself predates this file -- see the git
history for everything before it.
