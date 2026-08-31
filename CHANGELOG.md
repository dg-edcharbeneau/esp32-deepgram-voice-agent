# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Wi-Fi signal strength.** The link carries the whole session, and when it
  degraded the only visible symptom was dropped audio with nothing in the log
  saying the radio was the cause. `wifi_sta_get_signal()` now reads the RSSI of
  the associated AP -- from `esp_wifi_sta_get_ap_info()`, which returns a value
  the driver already holds, so there is no scan, no bus transaction and no
  sampler task -- and three things use it:
  - The familiar wifi glyph -- a dot with three arcs above it -- at 10 to 11
    o'clock, in the orb's own colour, balancing the charge dots across the
    panel. Four elements for four bars: the dot alone is one, each arc past it
    is one more, the progression every phone draws. On screen while stopped or
    asleep, and whenever the signal is weak. Free on the frame budget: 15.0 ms
    draw with it up against 14.9 ms without.
  - `rssi=`, `bars=` and `ch=` on the TLM line, `-1` each when there is no
    association.
  - `get_signal_strength`, so "how's your wifi?" and "why do you keep cutting
    out?" are answered out loud. The dBm never reaches the model: a number with
    no scale invites an invented one, so the bucket becomes the sentence.

  dBm is bucketed to 0-4 bars with **3 dB of hysteresis on promotion only** --
  beacon RSSI walks several dB between beacons, so a device sitting at a bucket
  boundary would otherwise flicker between two answers, while a link that has
  genuinely gone bad should be reported at once.

  Separately, the driver's own RSSI threshold
  (`CONFIG_WIFI_SIGNAL_WEAK_DBM`, -80 dBm) logs one line per excursion below it,
  which puts a cause in the log immediately before the dropped audio that
  follows. It is one-shot and re-armed in the handler -- without that there would
  be exactly one warning per association -- but the line itself is
  edge-triggered, because re-arming while still below the threshold makes the
  driver fire about once a second. Measured on hardware: thirteen identical lines
  for one ten-second dip before the gate, three lines for three dips after it.

  Two limits, both verified rather than assumed: the driver's averaging is
  coarser than the 1 Hz sample, so not every dip visible in `rssi=` raises an
  event, and the 3 dB recovery margin can swallow a second crossing if the link
  only recovers part-way. `rssi=` is on the TLM line every second regardless --
  the event is a convenience, the TLM line is the record.

  `CONFIG_WIFI_SIGNAL` (default on) compiles the whole thing out, and
  `CONFIG_WIFI_SIGNAL_SHOW_BARS` drops just the row.

### Changed

- The two overlays now share the tint and the dim ratio (`UI_ARC_OVERLAY` in
  `main/ui.c`) -- the "one colour, one dim step" rule that keeps an indicator
  from reading as a foreign object on the glass. Each keeps its own geometry and
  its own boxes, because a row of dots along a curve and a glyph that fills its
  own box want opposite answers on how to clear themselves. No visible change to
  the charge dots.

## [0.5.0] - 2026-08-31

### Fixed

- **Charging stopped being reported while the charger was still working.** The
  bolt was keyed to the AXP2101's current-*direction* field, which falls back to
  standby once the charge tapers into constant-voltage -- so a cell topping off
  read as a charger that had given up. `charging` now comes from the chip's
  charge **state machine** (trickle, pre-charge, CC, CV are all charging),
  gated on VBUS so it can never be true on battery.
- The direction field is three bits at `[7:5]`, not two at `[6:5]`. The old mask
  ignored bit 7 and agreed with the reference driver only because bit 7 reads 0
  on this board.

### Added

- "Done charging, still plugged in" as its own answer from `get_battery`.
  Previously that state fell into "not charging", which is also true on battery
  and reads as a fault.
- The charge target voltage (`REG64[2:0]`, one of 4.0/4.1/4.2/4.35/4.4 V) is
  logged once at startup, and `chgst=` (the raw charge state, 0-5) joins the TLM
  line. Between them they answer why a cell stops part-full: the state it
  stopped in says whether it reached the configured target or something else
  intervened. Still read-only -- raising the target is a write to the chip that
  also feeds the panel rails.

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
