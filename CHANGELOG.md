# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- **Power, by design.** No current figures -- the AXP2101 has no current
  reading and no meter was to hand -- so each measure saves by how it works and
  was checked for behaviour on the board. See `docs/power.md`.
  - **The CPU drops to 80 MHz asleep** (`CONFIG_PM_ENABLE`): held at 240 while
    the panel is awake or a session is live.
  - **The radio follows the session:** no modem sleep in a session, minimum
    with none, maximum while the panel sleeps too.
  - **The speaker amplifier turns off** after 5 s of silence and back on before
    the next sound (`CONFIG_AUDIO_PA_IDLE_OFF_MS`, 0 keeps it on).
  - **The sleeping clock dims, then goes off:** full brightness for 2 minutes
    asleep, 40 % for 3 more, then the panel off -- 5 minutes 45 seconds after
    the last word (`CONFIG_UI_SLEEP_DIM_AFTER_MIN`, `_DIM_PCT`,
    `CONFIG_UI_SLEEP_OFF_AFTER_MIN`). A touch, a session or an alarm brings it
    back. It also flushes only its own square.
  - The console UART is 1 Mbaud, the most it allows with power management on;
    the USB console, which is what is read, has no baud rate.
- **Power audit tooling:** TLM gains `slp= bri= ps= vbus= dir=`; Wi-Fi power
  save changes are logged; `CONFIG_POWER_STATE_WALK` (bench only) holds each
  state for a USB meter; `host/analyse.py --power` summarises a capture.

## [0.9.0] - 2026-10-24

Experimental pre-release. Verified on one bench board through screen captures
and bench self-tests; the stopwatch's gestures and the touch panel's swipe down
from the clock have not yet been tried by a finger.

### Added

- **A touch panel for settings that were voice-only.** Swipe down from the top
  edge: four pages, swiped sideways -- Sound (mic gain and volume as linked +/-
  pairs with a level meter between them), Colour
  (the 13 orb colours on a clock face), Toggles (noise reduction, 12/24-hour)
  and Timers & alarms (the soonest first, cancel in two taps). Nothing is new
  behaviour: each control calls what the voice tool or the BOOT button already
  did, and the agent needs no reload. The session carries on underneath; the
  panel closes on a swipe up, 8 s untouched, or for an alarm. The drag must
  start on the top edge, so it can never touch the centre button. Volume and
  mic gain apply at once and are saved 2 s after the last press, off the LVGL
  task. Painted into the face's canvas from precomputed shape masks rather than
  LVGL widgets: 10-60 ms a repaint, and no change to internal RAM measured on
  the board. `host/panel.sh` checks the geometry; `CONFIG_UI_PANEL_SELFTEST`
  (bench only) drives every control for the log. `panel=` joins the TLM line.
- **Screen captures of every orb state.** `CONFIG_UI_SCREENSHOT_TOUR` (bench
  only) runs the display test on its own and prints three captures per step
  over serial; `host/shot2png.py` turns the log into PNGs and a contact sheet.
- **A stopwatch**, after Ed's "Orb stopwatch" design, built on the orb clock:
  the minute ring counts seconds, the time shows centiseconds, the latest lap
  sits under it, and the sphere spins while it runs. Swipe sideways on the orb
  clock to get there and back, or say "start a stopwatch"; two round buttons
  play/pause and lap/reset. It runs on in the background, holds the panel
  awake while it counts on screen, and rides on the `alarms` tool (a
  `stopwatch` argument) rather than a fifth tool. A press on the clock is now
  decided on release: a tap puts the face back, a sideways swipe brings the
  stopwatch.
- **Timers and alarms ring until stopped, on a ring screen of their own,
  and alarms snooze.** After Ed's "Timer alarm" design: a small dotted orb
  with rings pulsing out of it, the label, and the overtime counting up
  (`TIME'S UP · +5:11`, or the alarm's time). A timer gets one big Stop; an
  alarm a Stop quarter beside a "+1 min" snooze quarter -- each tap adds a
  minute, the button goes solid then fades over 1.5 s, and the snooze commits
  at the dark end. It now rings until stopped or snoozed: an 8 s chime, the
  agent saying what it is, a chime again, falling silent after 10 minutes
  (`CONFIG_ALARM_RING_MAX_S`) with the screen still up. The microphone uplink
  is held while it chimes into a live session. Stop before the agent has said
  it still gets it said once; a press elsewhere on the ring screen does
  nothing, where any press used to dismiss. `host/alarms.sh` walks the ring
  cycle and the snooze arithmetic; `CONFIG_UI_RING_SELFTEST` (bench only)
  drives the screen and captures it.
- **The orb clock replaces the sleep clock.** While the device sleeps and the
  time is known, the panel shows Ed's "Orb clock" design: a small dotted orb
  turning in the middle with a meridian sweeping it, hour and minute rings of
  dots lit up to the time (the current one large and pulsing), the time with
  AM/PM and the next alarm, and Wi-Fi and battery across the top. It takes the
  orb colour; on white it is the mockup exactly. Painted into the canvas from
  `main/clock_face.c`, which `host/clock.sh` checks; still 5 fps. Against
  burn-in the whole clock drifts a few pixels, continuously and too slowly to
  see, where the old one jumped a step a minute.
  `CONFIG_UI_CLOCK_PREVIEW` (bench only) shows it in four set states for
  captures.
- **The battery dots at the panel's edge are off by default, and the Wi-Fi
  glyph only warns.** The orb clock's status row shows both readings whenever
  the device sleeps or the clock is swiped up. The glyph at 9 o'clock now shows
  only while the signal is weak, conversation or not. `CONFIG_BATTERY_SHOW_DOTS`
  brings the dots back; the readings, the TLM line and the voice tools are
  unchanged.
- **Swipe up from the bottom edge for the clock on demand**, awake, with the
  session carrying on underneath. Any touch puts the face back and does nothing
  else, so it cannot also end a conversation.
- **Sleep keeps full brightness**, so the clock can be read across a room. Only
  the battery-critical sleep still dims to 15 %.
- **The orb colour survives a reboot.** The last one chosen, by voice or on the
  touch panel, is saved by name once it has held for 2 s, and the device boots
  in it. `CONFIG_UI_DEFAULT_ORB_COLOR` is now only the out-of-box colour.
- **The orb's colour is the whole screen's.** The status word, the sleep clock,
  the hints, the ring round the button and every touch panel page take it, not
  only the orb: the white buttons become the colour and the greys take its hue.
  White is unchanged. The spectrum face, the QR code and the amber interrupt
  ring keep their own colours. `set_color`'s description now says so.

## [0.8.0] - 2026-10-23

Experimental pre-release.

### Fixed

- **Barge-in at volume 70 and above.** Talking over the agent had stopped
  working above volume 50, with nothing changed on the device. The firmware is
  not what changed: 0.7.0 and the 30 August build that first recorded barge-in
  working both fail the same way today, on the same settings and the same echo
  levels. Two device-side settings were what made it break, and are what fix it:

  - **`CONFIG_MIC_IN_GAIN` 24 -> 18 dB.** At 24 dB the agent's echo, with a
    voice on top of it, pinned the microphone at full scale; a clipped echo
    cannot be cancelled, and the interruption never left the device intact --
    replaying the recorded uplink into Flux STT on its own found no turn there
    either. 30 dB was tried and is worse: the device heard itself read the time
    and answered it.
  - **`CONFIG_AEC_UPLINK_VAD_PEAK` 400 -> 200.** The gate's threshold is a
    sample value and scales with the gain; 200 is the old 400 at 18 dB. Left at
    400, the gate cut a talking-over voice into fragments and interruptions only
    landed in the agent's pauses.

  Measured on the board at volume 70: no clipping, residual echo under 100 in
  the agent's quiet passages, interruptions detected reliably, best with noise suppression on (the default).
  One half remains Deepgram-side: in session
  `01a165b8-6a6f-7180-96d0-c5210810b4a2` Flux STT finds a turn in the replayed
  uplink at ~22.7 s where the live agent did not barge in. See
  [docs/audio-path.md](docs/audio-path.md).

### Changed

- **The agent's model is gpt-5.4-mini** (was gpt-4o-mini). After a working
  barge-in on "okay", gpt-4o-mini resumed the story it had been cut off from, and
  ignored a prompt rule against it; gpt-5.4-mini stopped and stayed stopped, with
  no noticeable change in reply latency.

- **Noise suppression is on by default.** `CONFIG_MIC_NS_ENABLE` and
  `CONFIG_MIC_NS_DEFAULT_ON` both default to `y`, so a fresh checkout builds a
  device that denoises from boot. The BOOT button's double click still overrides
  it at runtime and the choice persists.

  The measurements that argued against this are kept rather than dropped: in the
  room it was tested in the denoiser did not improve transcription -- the no-NS
  baseline already scored 4/4 -- and it costs about a quarter of the frame rate
  (20.0 -> 14.8 fps), 7.4 kB of internal RAM, and 64 ms of uplink latency. It is
  on for louder rooms than that one, and 4/4 is the bar to beat before anyone
  calls it a win. See [docs/audio-path.md](docs/audio-path.md).

### Added

- **Mic gain by voice (experimental).** "Set your microphone gain to 21" changes
  the ES7210's gain at once, 6 to 30 dB in its 3 dB steps, and saves it across
  resets. The uplink gate rescales with it so it keeps meaning the same loudness
  (200 at 18 dB, 283 at 21, 399 at 24), and the echo-reference lane is put back
  to `CONFIG_AEC_REF_GAIN_DB` after every change. For re-measuring the gain on a
  new board or in a new room; 18 dB is the measured value to come back to.
  `CONFIG_MIC_GAIN_TOOL`, on; turning it off ignores the saved gain. About 190
  bytes of Settings. Not yet exercised by voice on the board.

- **`CONFIG_AUDIO_RECORD`, a recording of what the device heard and sent.**
  Debug only, off by default. Each session's first 60 s go into PSRAM -- the raw
  microphone before cancellation, and exactly what reached Deepgram -- and are
  printed after the session ends; `host/rec2wav.py` turns a captured log into a
  WAV labelled with the request_id and aligned to the server's "seconds into
  call". `host/flux_replay.py` streams such a recording back into Flux STT. The
  two together are what split the barge-in failure into its device and server
  halves.

- **A clock, timers and alarms.** The agent can say the time and the date, set
  its timezone, and set timers and one-shot or weekly alarms. Alarms go off with
  nobody talking to the device, and the panel shows a large clock while it
  sleeps. Four tools: `get_time`, `set_clock`, `set_alarm`, `alarms`. All of it
  is `CONFIG_CLOCK`, on by default. See [docs/clock.md](docs/clock.md).

  **Time** comes from SNTP. Without it the device says it does not know the
  time, rather than inventing 1970. **This board has no RTC**, though its board
  package's comment lists one; a bus scan proved it. A PCF85063 driver is kept
  behind `CONFIG_CLOCK_RTC`, default off. The TLM line gains `clk=` (0 none,
  1 RTC, 2 SNTP) and `alm=` (pending count).

  **12 or 24-hour** follows the timezone (12-hour across most of the Americas,
  Australia, India and a few more) until the user says otherwise by voice, and
  that choice is saved. The sleep clock shows `8:52` with a small `PM` under
  it, right-aligned.

  **Timezones** are IANA names, set by voice or in a new setup-portal field
  that is filled in from the phone. A 130-row table maps them to the POSIX rules
  newlib needs. `host/alarms.sh` checks every row against tzdata across a full
  year, and its first run caught three Canadian zones that tzdata 2026e moves to
  permanent summer time on 1 Nov 2026.

  **When one rings,** a locally synthesised chime plays first, so neither the
  network nor a session is needed for it. The chime is generated inside the
  playback task rather than queued, because the ring has exactly one writer.
  After `CONFIG_ALARM_CHIME_S`, or a tap anywhere, the chime stops and the
  speaker drains. Only then does a session open, so the echo canceller never
  warms up on a tone. The agent then says what it was: as the greeting of a
  fresh conversation, or as an `InjectAgentMessage` (`behavior: queue`) sent
  from the keepalive task, retried on `InjectionRefused`.
  `session_ctl_request_start_soon()` makes sure an alarm landing during a stop
  or the cooldown is late rather than lost.

  **Only definitions reach flash.** A weekly alarm's next time is recomputed,
  never stored as truth, so it never writes NVS. Daylight saving is handled
  explicitly: a spring-forward 02:30 rings at 03:00, and a fall-back 01:30 rings
  once.

  **Cost:** +51 kB flash (including a 72 px digits-only font of about 15 kB) and
  +864 B of static internal RAM. The alarm table and its copies live in PSRAM.
  `Settings` grows by 1,049 bytes (19,290 to 20,339 with history replayed): four
  tools as raw cJSON nodes, and one prompt sentence. A first cut with six tools
  measured 21,349, past the 20 kB at which sessions flapped before, and was cut.
  Ten fresh-boot sessions went live; one dipped to 1,728 B of DMA-capable
  memory, so the margin is thin.

- **Microphone noise suppression, off by default and not yet measured on the
  device.** The canceller removes the device's own speaker; nothing removed the
  *room*, so fans, HVAC and traffic went up the wire to Deepgram at full level.
  `CONFIG_MIC_NS_ENABLE` adds a denoiser after the AEC and before the tap, the
  uplink VAD and the sink -- after the canceller because a non-linear stage
  upstream of an adaptive filter wrecks its convergence, before the rest so the
  orb, the VAD threshold and Deepgram all see the same cleaned signal.

  It uses esp-sr's `ns_pro`, which was already linked. Its one awkwardness is
  frame size: `ns_pro_create` takes 10 ms frames only and
  `AUDIO_IO_CAPTURE_FRAMES` is 1024 under the AEC, so `1024 % 160 = 64` and it
  cannot walk the block in place the way `aec_process` does. It carries an input
  and an output FIFO instead, costing 7,104 B of internal staging and a standing
  64 ms of added uplink latency.

  **SpeexDSP was evaluated and removed.** It was vendored precisely because it
  accepts any frame size and so needed no FIFO, and that was not enough: it cost
  57% of the frame rate against esp-sr's 26%, and it damaged speech -- eating
  quiet word onsets ("A quick brown fox" for "The quick brown fox") and losing
  an utterance where the no-NS baseline transcribed 4/4. Its state could not be
  moved out of PSRAM to recover the speed: doing so dropped `intmax` to 10,240
  and the device could not open a session at all, TLS failing on every retry.
  The measurements are kept in `docs/audio-path.md` so it is not re-proposed.

  The `mic peak` line gains an `ns=` field beside `out=`. They stay separate on
  purpose: `out=` is post-AEC and `ns=` post-denoiser, so ERLE remains readable
  on its own and a regression in one cannot be blamed on the other.

  **Measured on the board**, 150 s per arm from cold boot on a live session:

  | arm | fps | `int` mean | `intmax` floor | rx overruns |
  |---|---|---|---|---|
  | off | 20.00 | 60,331 | 26,624 | +0 |
  | Speex | 8.63 (-57%) | 60,903 | 29,696 | +0 |
  | esp-sr | 14.80 (-26%) | 52,894 (-7,438) | 21,504 | +0 |

  **They do not rank, they trade.** esp-sr is 2.2x cheaper on CPU and costs
  7.4 kB of internal RAM -- the resource this board has least of. Speex costs
  none and more than half the frame rate. Neither moved `rxovf`. Flash is
  +18,592 B (Speex) / +17,872 B (esp-sr); with the feature off the binary is
  byte-for-byte what it was.

  **Acoustically, neither engine helped.** One speaker, four fixed lines, same
  room: NS off transcribed 4/4 correctly; Speex dropped word onsets ("A quick
  brown fox", "Seashells seashells") and lost an utterance; esp-sr was 3/3 in
  window. At this noise level Deepgram already handles the audio, so the
  denoiser spends frame rate for nothing. A genuinely loud room is untested and
  is the only condition where NS is likely to pay.

  **NS runs below the uplink VAD**, which took two attempts. Placing it above
  was justified as giving the VAD a cleaner floor; that was backwards -- the
  denoiser attenuates interrupting speech along with the room, so fewer blocks
  cleared `VAD_PEAK` and barge-in got perceptibly harder to trigger. Blocks
  discarded during playback: 22 (off) -> 49 (NS above) -> 16 (NS below). With
  the stage moved, the VAD decides on the post-AEC signal exactly as it did
  before, and the denoiser still sits ahead of the tap and the sink.

  **Correction: esp-sr adds 64 ms of uplink latency, not the 10 ms first
  claimed.** 10 ms is only the input residue; the output FIFO holds a further
  full capture block between drains, measured at 1024 samples of standing
  occupancy -- straight onto the interruption path.

- **Noise suppression toggles from the BOOT button.** A double click turns it on
  and off at runtime and the setting persists across reboots;
  `CONFIG_MIC_NS_DEFAULT_ON` is just the power-on state. The status caption
  confirms the press, and `mic peak` says `ns=off` for an idle stage rather than
  `ns=0`, which would read as total suppression.

  The engine's 7,104 B of staging is allocated at init and held either way --
  deferring it to the first press would mean a multi-kB internal allocation
  mid-session on a fragmented heap, which is what drops sessions on this board.
  The toggle buys back CPU, not RAM.

  The gesture is compiled out along with the feature rather than left answering
  "not built": registering a double click makes `iot_button` hold back every
  single click until the double-click window closes, and the single click is the
  session toggle -- the escape hatch. Builds without the denoiser keep it sharp.

- **`rxovf` in the telemetry line.** `audio_codecs_rx_overruns()` has counted
  silently dropped I2S RX frames since the DMA sizing work and nothing ever read
  it. It is now on the TLM line, which is how the bench above could show the
  denoiser was not starving the capture ring.

  Three measurement traps hit along the way, all recorded in
  [docs/audio-path.md](docs/audio-path.md): a first esp-sr build that measured
  +96 B of flash because `1024 % 160 != 0` is a compile-time constant and the
  compiler had deleted the whole engine; a serial reader left holding the port,
  which makes `idf.py flash` fail with `The chip stopped responding` and look
  like dead hardware; and a stale capture log that made a fixed crash appear to
  still be happening.

## [0.7.0] - 2026-09-01

### Added

- **Conversations survive a reboot.** The device already survived a dropped
  socket -- the last turns were replayed into every new `Settings` message --
  but that ring lived in `.bss`, so a brownout, a crash or an unplug came back
  to a device that greeted you as a stranger. The turns now live in a packed
  byte arena in PSRAM and are written to flash.

  The store (`main/history_store.c`) is a ring of eight single-sector records in
  the `storage` partition, which was declared in `partitions.csv` and mounted by
  nobody. The header is written **last**, so a record is either complete or
  invisible and a power loss mid-write leaves the previous one whole. Not NVS:
  that partition is 630 entries shared with the Wi-Fi calibration blob, and its
  compaction rewrites neighbours -- per-turn history writes would have spent the
  erase budget holding the credentials.

  Writes are deferred twice. A turn sets a flag and a 1.5 s debounce coalesces an
  exchange into one write; `session_ctl`'s worker does the blocking erase,
  because neither the WebSocket event task nor `esp_timer` can afford tens of
  milliseconds of flash with the cache off. `host/store.sh` compiles the real
  module against a fake NOR flash that clears bits rather than copying and can
  cut a write short -- both power-loss windows are covered.

- **The status word gets out from under your thumb.** The label sat in the middle
  of the screen, which is also the 70 px touch target, so reaching for the
  control hid the words saying what the control would do. While a finger is down
  it moves to twelve o'clock and bends along the bezel, a glyph at a time
  (`main/arc_text.c`), lingering a second after release and fading out -- a tap
  lasts a tenth of a second, and text that snapped back on release would flick
  away before anyone read it.

- **`new_conversation`**, and hold-again on a stopped device. The two ways to
  forget deliberately. The function's confirmation is enforced in code rather
  than promised in a description: the first call only arms, and the second
  counts only inside sixty seconds and after exactly one user turn.

### Changed

- **A tap no longer ends the conversation.** It used to clear the history, on the
  reading that stopping on purpose means being finished -- but a tap is how you
  stop the device streaming, and people reach for it for reasons that have
  nothing to do with being done talking. Every stop now behaves as the idle
  timeout always did. The centre reads `stopped, saved` when there is something
  to come back to, and `resuming` rather than `connecting` when there is.

- **Both indicators moved to the sides**: charge down the right straddling 3
  o'clock, the wifi glyph at 9. They sat at 1-2 and 10-11 o'clock, which is
  exactly where the status caption's ends land, and the top charge dot blacked
  its own box through the tail of `forget? hold again` every frame.

### Fixed

- **Sessions dropping at random.** Measured, not guessed: a Mac probing the same
  AP saw 0 failures in 316 samples while the device dropped repeatedly, so it was
  never the network. Every drop was `esp-aes: Failed to allocate memory` on a TLS
  read or write.

  The cause took the right instrument to see. `MALLOC_CAP_DMA` is a strict subset
  of `MALLOC_CAP_INTERNAL`, and every figure this firmware logged was the
  internal one -- so `intmax=7680` read as "a 1,600 B request should fit" when
  the DMA-capable largest block was actually **832 B**. The failing allocation is
  `heap_caps_aligned_alloc(align, <=1600, MALLOC_CAP_DMA)` from
  `esp_aes_dma_core.c`, twice per TLS record, taken because
  `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` puts the record buffers in PSRAM where DMA
  cannot reach them.

  `DRAW_ROWS` 32 -> 16 fixes it: 14,912 B of internal RAM plus the same again in
  SPI DMA transfer buffer, taking the DMA largest block from 14,848 to 31,744 --
  about 20x the request that was failing. Zero allocation failures across 394
  active-session samples, against 10 in 61 before. The cost is twice as many
  flush transactions per frame.

- **A session no longer dies when a send cannot take the client lock.**
  `CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y` gives sends their own mutex instead
  of the one the receive loop holds. The timeout still occurs; it is no longer
  fatal.

### Instrumentation

- `dma=` and `dmamax=` on the TLM line, and both pools in `main/heap_probe.c`'s
  failure hook and 50 ms sampler. The probe stays compiled out by default. It
  already existed and was switched off; turning it on is what made an invisible
  fault visible in one capture.

### Known

- The lock timeout above still happens, roughly twice per 400 active samples.
- Verification is ~400 active-session samples, not the ~3,000 needed to
  distinguish "rare" from "gone". Read the fixes as *no longer reproducible in
  normal use*.
- The orb colour is not persisted -- unlike the voice and agent name, it resets
  to `CONFIG_UI_DEFAULT_ORB_COLOR` on every restart.

## [0.6.0] - 2026-08-31

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
