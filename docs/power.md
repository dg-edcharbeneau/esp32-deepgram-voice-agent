# Power

The device is stopped or asleep about 88 % of its life, so that is where its
battery goes, and where this page starts.

**There are no current figures here, on purpose.** The AXP2101 reports the
cell's voltage, its own gauge percentage, the charge state and whether USB power
is present -- no battery current and no coulomb counter -- and no meter was
available. So what follows saves power by how it works, each piece checked for
*behaviour* on the board (the right state at the right time, nothing broken),
not for milliamps. The tooling to put numbers on it is below, for when a meter
is to hand.

## What runs when

| | session live | stopped, awake | asleep (orb clock) |
| --- | --- | --- | --- |
| CPU | 240 MHz | 240 MHz | **80 MHz** |
| radio | full power | min modem sleep | **max modem sleep** |
| speaker amplifier | on | off 5 s after the last sound | off |
| microphone | open, echo cancelling | parked | parked |
| panel | 100 %, ~30 fps | 100 %, ~30 fps | 100 % at 5 fps, **40 % after 2 min, off after 5** |

## The measures

**The CPU scales** (`CONFIG_PM_ENABLE`, `esp_pm_configure` in `main.c`). Two
locks hold it at 240 MHz: the panel being awake (`ui.c`), and a live session
(`session_ctl.c`), whose echo canceller, noise suppressor and uplink run on a
64 ms budget measured at full speed. Without either -- asleep, no session -- it
drops to 80 MHz. 80 is the floor on purpose: below it the APB clock falls from
80 MHz, and with it the console UART and the timing every peripheral was built
against. The asleep orb clock's frame, 25-60 ms at 240 MHz, still fits its
200 ms at 80; its `EVT clock draw` line says by how much.

**Measured: 42 % of sleep at 80 MHz.** The IDF drops the clock only while both
cores are idle and lifts it again on any task or interrupt, so the asleep clock
still draws at 240 MHz (its 22 ms frame is unchanged) and sits at 80 between
frames. With `CONFIG_PM_PROFILING` on, `main.c` prints the IDF's lock and mode
stats 5 s and 65 s into each sleep; over that minute on the bench the CPU spent
25.0 s at 80 MHz and 35.3 s at 240. Core 1 was busy 40 % of it -- drawing the
clock (11 %) and LVGL copying the canvas out to the panel in 16-row strips, five
times a second -- and core 0 21 %, leaving idle about 1,600 times a second: the
1 kHz FreeRTOS tick. Slowing LVGL's own tick to 5 ms and its touch and refresh
timers to 50 ms asleep was tried and changed nothing (25.0 s against 25.4 s), so
it was not kept. The FreeRTOS tick is 1 kHz in `sdkconfig.defaults`, and every
driver and the speech libraries were built against it; changing it is not a
by-design change. The other lever left is the asleep frame itself: fewer frames,
or less of each, after the clock dims.

There is **no light sleep**. The LVGL adapter's tick is a 1 ms `esp_timer`, which
would wake the chip a thousand times a second; until that can be stopped asleep,
light sleep would cost the work of entering it for nothing. The BOOT button's
5 ms timer and a few 10-20 Hz idle polls (`dg_uplink`, `audio_play`, the parked
capture task) would be next in line.

**The radio follows the session.** A live session streams audio both ways and
modem sleep costs it tens of ms of jitter, so it gets none -- the
battery-critical sleep included, which keeps a session going. With no session
the radio carries only beacons and the hourly SNTP: minimum modem sleep while the
panel is awake, maximum (sleeping through the listen interval) while it sleeps.
`session_ctl` puts it back to full power itself before a session's first packet,
so a start does not wait for the loop's next pass.

**The speaker amplifier is off when nothing plays.** Its enable pin (GPIO46)
goes low once nothing has been written for `CONFIG_AUDIO_PA_IDLE_OFF_MS` (5 s),
and high again before the next write, with 20 ms for it to come up. Only the
amplifier: the ES8311 stays open and clocking silence, because reopening the
codec per turn clicks. If the amplifier's own start is ever heard, raise the
delay or set it to 0.

**The sleeping clock dims, then goes off.** It comes up at full brightness, to
be read across a room, dims to `CONFIG_UI_SLEEP_DIM_PCT` (40 %) after
`CONFIG_UI_SLEEP_DIM_AFTER_MIN` (2 min), and the panel goes off altogether after
`CONFIG_UI_SLEEP_OFF_AFTER_MIN` (5 min), drawing nothing -- the frame timer drops
to once a second -- until a touch, a session or an alarm. An AMOLED's draw is
its lit pixels times its brightness. And the clock flushes only its own 420 px
square, not the whole 466 px frame, five times a second.

From the last word, with the defaults:

| Phase | Starts | Lasts | Screen | CPU | Radio | Amp | Mic |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Conversation | -- | until 15 s of silence | orb face, 100 %, ~30 fps | 240 MHz | full power | on while sound plays | on |
| Idle, awake | 15 s | 30 s | orb face, 100 %, ~30 fps | 240 MHz | min modem sleep | off | off |
| Asleep, clock bright | 45 s | 2 min | orb clock, 100 %, 5 fps | 80 MHz between frames | max modem sleep | off | off |
| Asleep, clock dimmed | 2 min 45 s | 3 min | orb clock, 40 %, 5 fps | 80 MHz between frames | max modem sleep | off | off |
| Asleep, display off | 5 min 45 s | until woken | off | 80 MHz but for 1 Hz | max modem sleep | off | off |

## Putting numbers on it

With a USB-C power meter between the computer and the board, and the cell full
(TLM `chgst=4`), USB current is the system's own draw. Then:

- **`CONFIG_POWER_STATE_WALK`** (bench only) holds the device in one state at a
  time for 60 s from 45 s after boot -- awake on the face; asleep at 100, 40 and
  15 % and with the panel off; with the amplifier off; the radio at max modem
  sleep and off; a live session -- each announced by `EVT power step N`.
- **The TLM line** carries `slp=` (asleep), `bri=` (brightness, 0 off), `ps=`
  (radio power save: 0 none, 1 min, 2 max), `vbus=` and `dir=`.
- **`host/analyse.py --power LOG`** reports the time per power state, each walk
  step's conditions (and whether the cell was really full), and -- for a capture
  on battery -- the cell's voltage slope in mV/hour, the one on-device proxy for
  draw.
