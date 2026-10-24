# The clock, timers and alarms

The device knows the time, says it when asked, and keeps timers and alarms that
go off with nobody talking to it. They chime, wake the panel, and then the agent
says what they were. While the panel sleeps it shows a large clock.

All of it is `CONFIG_CLOCK` (default on). Off removes the code and the four tools
with it.

| file | what it holds |
| --- | --- |
| [main/clock.c](../main/clock.c) | wall time: SNTP, an optional RTC, the timezone, 12/24-hour, `get_time` / `set_clock` |
| [main/tz_table.c](../main/tz_table.c) | about 130 IANA names mapped to the POSIX rules newlib understands |
| [main/alarms.c](../main/alarms.c) | the table, NVS, firing, the ring sequence, the four alarm tools |
| [main/alarm_sched.c](../main/alarm_sched.c) | next-occurrence arithmetic and parsing; plain C, host-tested |
| [main/font_clock_72.c](../main/font_clock_72.c) | 72 px Montserrat, digits and colon only, generated |

## Where the time comes from

**SNTP**, started the first time the station gets an address
(`CONFIG_CLOCK_NTP_SERVER`, then lwIP's hourly schedule). On the bench it syncs
about 200 ms after the address arrives.

**This board has no RTC.** The board package's header lists one on the shared
I2C bus, and the first version of this feature drove a PCF85063 at 0x51 on that
word. A scan of the bus finds 0x18 (ES8311), 0x34 (AXP2101), 0x40 (ES7210),
0x5A (touch) and 0x6B (QMI8658 IMU), and nothing at 0x51. The driver is kept
behind `CONFIG_CLOCK_RTC`, default off, for a board that does carry one. It
reads the RTC at boot, honours the oscillator-stop flag, and writes every sync
back. So on this board a reboot without Wi-Fi has no time until the network is
back.

Until a source has answered, the clock is simply not valid. The tools say "the
clock is not set yet", the face shows no clock, alarms refuse to be set, and
timers still work on the monotonic clock (see below). Anything before
2025-01-01 counts as unset.

The TLM line carries `clk=`: 0 none, 1 RTC, 2 SNTP.

## Timezones

newlib has no zone database, only POSIX rule strings such as
`CST6CDT,M3.2.0,M11.1.0`. People and phones speak IANA (`America/Chicago`), so
`tz_table.c` translates. A name it does not hold is refused, never guessed, and a
POSIX rule is accepted as given.

**Two ways to set it.** By voice, through `set_clock` ("I'm in Chicago"); or in
the setup portal, where the field is filled in from the phone's own zone. Both
save the *name* to NVS (`dgagent/tz`), not the rule. The rule is looked up again
on every boot, so when a zone's rules change a reflash fixes it without anyone
setting the zone again.

**Applied on the main loop only.** `setenv("TZ")` + `tzset()` run in
`clock_tick()`. newlib re-parses TZ with a full `sscanf` the first time it is read
after a change, which is over a kilobyte of stack, and the WebSocket task that
runs the tools is the last place to spend that. The tool stores the request and
the loop applies it within a second. A `get_time` landing inside that second says
so rather than reading the old zone.

**The table holds the rules from November 2026 on, not the history.** A rule
string cannot say "until". tzdata 2026e has British Columbia, Alberta and
Manitoba keeping summer time permanently from 1 Nov 2026. So Vancouver, Edmonton
and Winnipeg are written as the fixed offsets they become, and are an hour off
before that date. Casablanca is an hour fast during Ramadan, which moves every
year by observation and has no POSIX form.

`host/alarms.sh` checks every row against the laptop's own tzdata, four samples a
day for a year. That is how the Canadian change was found: the rows were first
typed from older rules and failed there by name.

Quoted abbreviations (`<+05>-5`) are rewritten to letters (`LCL-5`). newlib's
support for the quoted form was not something to bet the clock on.

## Timers and alarms

| kind | example | stored as | after it rings |
| --- | --- | --- | --- |
| timer | "ten minutes, pasta" | an instant | gone |
| once | "7:30" | hh:mm and its next instant | gone |
| weekly | "7:30 on weekdays" | hh:mm and a weekday mask | rolls to the next one |

At most eight, in one NVS blob (`dgagent/alarms`, about 360 B). The tools edit the
table under a mutex on the WebSocket task and mark it dirty. `alarms_tick()` on
the main loop does the NVS write, so a tool never blocks on flash.

**Only definitions reach flash.** A weekly alarm's next instant is recomputed on
load and after every ring and never trusted from storage. An alarm that rings
every weekday for a year never writes the flash once.

**A timer set before the clock is known** runs on `esp_timer`, and gets a real
instant as soon as there is one. Only then can it survive a reboot.

**Off across the moment it was due:** a timer or one-shot alarm still rings if
it is at most 10 minutes late (`ALARM_MISS_WINDOW_S`), and is dropped and logged
beyond that. "Wake me at 7" ringing at noon is worse than not ringing. Weekly
alarms never ring late; they roll forward.

**A zone change re-times once and weekly alarms**, because "7:30" meant 7:30
where the user is. Timers are durations and keep their instant.

### Daylight saving is decided here

| case | behaviour |
| --- | --- |
| spring-forward gap (02:30 on a night that skips 02:00-03:00) | rings at 03:00, the first minute that exists |
| fall-back overlap (01:30 on a night that has two) | rings once, at the first |

`alarm_next_local()` gets this by resolving the wall time with `tm_isdst` pinned
to each value. It accepts only answers that `localtime_r()` converts back exactly,
rather than trusting how one C library normalises an impossible time. newlib and
macOS disagree on that. The host test runs both edges in Chicago and London,
each once by IANA name and once as the POSIX rule the device applies.

## When it rings

```mermaid
sequenceDiagram
    participant A as alarms_tick
    participant P as playback_task
    participant S as session_ctl
    participant D as dg_agent
    A->>D: dg_agent_announce("Your pasta timer is done.")
    A->>P: audio_io_chime_start()
    Note over P: tone synthesised whenever the ring is empty
    A->>P: chime_stop() after ALARM_CHIME_S, or a tap
    A->>S: request_start_soon() once the speaker is silent
    S->>D: dg_agent_start()
    D-->>D: no history: announcement IS the greeting
    D-->>D: history: InjectAgentMessage from keepalive_task
```

**The chime first, because it needs nothing.** No network, no session. It is
what actually gets someone's attention. The caption shows the label, the panel is
held awake, and `CONFIG_ALARM_CHIME_S` (8 s) later the voice follows.

**The tone is generated inside the playback task, never queued.**
`audio_io_play()` has exactly one writer, the WebSocket task. The carry byte and
the queue stamp are only correct because of that. A second producer would
interleave with the greeting the moment a session came up, which is the
permanent white-noise failure the carry exists to prevent. So `playback_task`
fills its own buffer with the tone whenever the ring is empty, and agent audio
always wins.

**The chime stops, and the speaker drains, before the session opens.** The
microphone opens with the session. A pure tone is the worst thing the echo
canceller could warm up on: it would count towards `AEC_WARMUP_BLOCKS` and be
declared converged without ever hearing speech. Its residue can also clear the
uplink VAD and read as the user barging in.

**With a session already up there is no chime.** The announcement goes out on
that session.

**How the agent says it.** It depends on the session:

- **A fresh conversation, with no history:** the announcement replaces
  `agent.greeting`. It is spoken exactly once on `SettingsApplied`, with nothing
  to race.
- **Anything else:** `{"type":"InjectAgentMessage","behavior":"queue",...}`.
  `queue` rather than the default, because the default is also refused while the
  *agent* is mid-turn. It goes from `keepalive_task` and no other task, because
  that task is already the text sender `dg_agent_stop()` waits for. An inject
  from anywhere else holds the client lock outside the stop's quiesce.
- **`InjectionRefused`** (the user was mid-turn) is retried every 2 s, up to four
  times, by timer. It is never retried on `AgentAudioDone`, which this stack
  almost never sends.

The model is told why the session exists, through a prompt note.

**An announcement expires after two minutes unsaid.** A device that took five
minutes to reach Deepgram should not then announce a timer as though it had just
ended.

**`session_ctl_request_start_soon()`** is a flag, not a notification. A plain
start is dropped during a stop or inside the 1.5 s cooldown, and an alarm that
fires right after the idle timeout stopped the session lands in exactly that
window. A gesture dropped there costs a second tap; an alarm has nobody to tap
again.

**Battery critical wins.** The panel stays dim, but the chime, the caption and
the announcement all still happen.

### Stopping it

While it chimes, **a press anywhere on the panel** stops it, not just the 70 px
button. So does the BOOT button. Either way the press is swallowed: it cannot also
toggle the session or offer to forget the conversation. The announcement still
follows, because that is how you learn which alarm it was.

## The sleep clock

While the panel sleeps and the time is known, it shows the **orb clock**, after
Ed's "Orb clock" mockup:

- **A small dotted sphere** in the middle: 180 dots, 96 px across the radius,
  turning slowly on a fixed tilt. A **meridian** sweeps across its near side and
  lights the dots it passes.
- **Two rings of dots round it**: 12 for the hours at 146 px, 60 for the minutes
  at 186 px. Every dot up to the current one is lit and twinkles; the current one
  is large, with a pulsing halo. 2:38 lights hours 12 to 2 and minutes 0 to 38.
- **The time** in the middle, `2:38` with a small spaced `PM` under it (24-hour:
  `14:38` and no AM/PM), and the next alarm under that ("alarm 7:30 AM", or
  "alarm Wed 7:30 AM" if it is not today).
- **Wi-Fi and battery across the top**: the Wi-Fi glyph lit one element per bar,
  a battery whose fill is the charge (red at 20 % or under), the percentage, and
  a bolt while charging. This row is where battery and Wi-Fi are read at rest:
  the older battery dots at 3 o'clock are off by default, and the Wi-Fi glyph at
  9 o'clock only appears, on the face, while the signal is weak.

**On demand**, too: swipe up from the bottom edge and it comes up straight away,
awake, with the session carrying on underneath. Any touch puts the face back,
and does nothing else. Before the first time sync the swipe does nothing.

It takes the orb colour like the rest of the screen: on white it is the mockup
exactly. It is hidden under the QR code and the display test. A touch wakes the
panel on the press, and the clock is gone on that frame. Before the first time
sync there is nothing for the rings to show, so the panel keeps the plain orb.

- **Painted, not labels.** `main/clock_face.c` works out the dots from the
  mockup's own formulas -- pure C, checked on a laptop by `host/clock.sh` --
  and `main/ui_clock.c` paints them into the canvas, every frame from black,
  with the text drawn through an LVGL layer on top. The mockup keeps the sphere
  off the digits with a black text-shadow; LVGL text has none, so the dots under
  the text fade out instead.
- **Full brightness, 5 fps** -- awake on demand too. A frame costs 25-60 ms,
  which at the awake 33 ms period would hold the LVGL task busy on the core the
  echo canceller shares. It is there to be read across a room, and at the old
  15 % brightness it was not. The frame rate and the radio's power save are what sleep
  still saves. The one sleep that dims is the battery-critical one.
- **Burn-in.** The whole clock drifts round a 3 px Lissajous, 17 minutes across
  and 23 down, so no pixel holds the same ink for hours. Continuous, not stepped:
  the old clock's 3 px jump a minute (12 back every fifth) made a screen-wide
  ring of dots read as the whole face jumping off centre. At under 0.02 px a
  second the drift cannot be seen; the dots take the fraction, the text whole
  pixels.
- **What a frame costs** is in the log once a minute, as `EVT clock draw`.
- **The fonts** are digits and a colon at 52 px, and `A`, `M`, `P` at 15 px --
  generated, because full faces would spend flash on letters a clock never draws:

  ```bash
  npx lv_font_conv@1.5.2 --no-compress --no-prefilter --bpp 4 --size 52 \
      --font managed_components/lvgl__lvgl/scripts/built_in_font/Montserrat-Medium.ttf \
      -r 0x30-0x3A --format lvgl -o main/font_clock_52.c
  npx lv_font_conv@1.5.2 --no-compress --no-prefilter --bpp 4 --size 15 \
      --font managed_components/lvgl__lvgl/scripts/built_in_font/Montserrat-Medium.ttf \
      -r 0x41,0x4D,0x50 --format lvgl -o main/font_clock_15.c
  ```

  The mockup's Inter SemiBold is a later swap, made by regenerating these.
- **Seeing it without waiting.** `CONFIG_UI_CLOCK_PREVIEW` (bench only) shows it
  in four set states 10 s after boot and prints captures over serial for
  `host/shot2png.py`.

## The tools

The schemas are constant JSON in flash, one raw cJSON node each, and none of the
handlers runs on the WebSocket task's frame. `clock_tool()` is out of line and
replies through a PSRAM buffer.

| tool | arguments | notes |
| --- | --- | --- |
| `get_time` | none | local time in the user's format, weekday, date, zone |
| `set_clock` | `zone?`, `hours?` | IANA name, refused if `tz_table.c` does not hold it; `hours` 12 or 24. Either or both, checked before either applies |
| `set_alarm` | `seconds`, or `time` (24 h `HH:MM`) + `days?`; `label?` | `seconds` makes it a timer (1 s to 24 h). `days`: daily, weekdays, weekends, or `mon,wed`; a half-understood list is refused whole |
| `alarms` | `cancel?` | lists ids, times and time left; with `cancel`, cancels an id, a label, or `all` |

**Four, not six, because of the `Settings` message, and measured.** The first
version had a tool per verb and a sentence per parameter. That was 1,723 bytes,
plus a 301-byte prompt paragraph, and it took a resumed session's `Settings` from
19,290 to 21,349 bytes on the board. That is past the 20 kB at which sessions
flapped before (see `HISTORY_REPLAY_BYTES` in `dg_agent.c`), and a sixth 4 kB TLS
record. Merging the verbs and dropping descriptions that only repeat a parameter's
name brought it to 909 + 138 bytes: 20,339 in total.

Five fresh boots of each build, with a 1,238-byte history replayed:

| build | `Settings` | lowest DMA-capable block while sending |
| --- | --- | --- |
| `main` | 19,290 | 3,584 / 4,608 / 3,584 / 4,096 / 8,704 |
| `clock` | 20,339 | 4,608 / 1,728 / 4,864 / 4,608 / 4,864 |

The medians match. One `clock` boot dipped to 1,728 B, just above the ~1,600 B
that `esp-aes` asks for, and all ten sessions went live. Fresh boots cannot
reproduce the fragmentation of a device that has been up for hours (see
`docs/verifying-a-boot.md`), so treat the margin as thin.

## 12 or 24-hour

`clock_12h()` says which:
- **Saved choice:** what the user asked for by voice ("use 24-hour time"), saved in
  NVS as `dgagent/clk12`.
- **Zone default:** otherwise, the zone's own convention, from the last column of
  `tz_table.c`. 12-hour for most of the Americas, the US Pacific, Australia, New
  Zealand, India, Pakistan, Bangladesh, the Philippines, Egypt and Saudi Arabia.
  Brazil, Argentina, Uruguay and Chile are 24-hour, like everywhere else. A raw
  POSIX rule says nothing about where it is, so it gets 24-hour.

One formatter, `alarm_fmt_time()` in `alarm_sched.c`, writes every time the device
shows or says. Midnight is 12 AM, noon is 12 PM, and the 12-hour form has no
leading zero. `get_time` hands the model that one form only, which is the form it
then speaks. Alarm *input* stays 24-hour `HH:MM`: the model converts, and a
parser guessing at "7:30" would be guessing on the thing that must not be wrong.

On the sleep face, 12-hour shows `8:52` centred, with a small spaced `PM` centred
under it.

## Kconfig

| option | default | |
| --- | --- | --- |
| `CLOCK` | y | everything on this page |
| `CLOCK_RTC` | n | a PCF85063 at 0x51; this board has none |
| `CLOCK_NTP_SERVER` | `pool.ntp.org` | |
| `CLOCK_DEFAULT_TZ` | `UTC` | until the user sets one |
| `ALARM_CHIME_S` | 8 | seconds of chime before the session opens |
| `CLOCK_TEST_TIMER_S` | 0 | bench aid: a timer this long after boot |
