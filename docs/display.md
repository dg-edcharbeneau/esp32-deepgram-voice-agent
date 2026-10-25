# The display

The two faces, where their audio data comes from, what they cost in RAM, and
how to see every state without talking to the device.

## What is on the screen

The 466x466 AMOLED shows one of two interchangeable **faces**, and you switch
between them by asking out loud — see [Changing things by asking](voice-commands.md).

Both are driven by the session rather than by their own I2S read, because the
codec is already owned by `audio_io`.

One word sits in the middle of whichever face is up, saying what the session is
doing — `listening`, `resuming`, `stopped, saved`. It moves to the bezel and
curves along it while a finger is on the button, because the middle of the screen
is also the touch target; see
[the status word](session-control.md#the-status-word-gets-out-from-under-the-thumb).

### The orb (default)

A dotted shell of 456 dots, a C port of the voice orb from
[thinking-orbs](https://github.com/Jakubantalik/thinking-orbs) and
[expo-thinking-orbs](https://github.com/mahdidavoodi7/expo-thinking-orbs), both
MIT. It has a vocabulary rather than a level meter:

| | what the shell does |
|---|---|
| you speaking | wavefronts travel **inward**, depth set by your volume |
| agent speaking | wavefronts travel **outward** from a slightly smaller shell |
| between turns | a quasi-periodic breath, a differential twist, and occasional gestures |
| connecting | a ladder — `connecting`, `buffering`, `initializing` — each visibly fuller and brighter than the last, so progress reads without reading the label |
| stopped | drawn in, dim, with a faint ping every 5 s |

Amplitude scales how **deep** a gesture goes, never how fast. Driving rate from
level is frequency modulation, and reads as vibration rather than as a voice.

The dots are white by default and can be recoloured by asking — see "Changing
the orb's colour by asking" in [voice-commands.md](voice-commands.md).
The colour chosen last, by voice or on the touch panel, survives a reboot;
`CONFIG_UI_DEFAULT_ORB_COLOR` picks what it boots as until one has been chosen.

The geometry is verified numerically rather than by eye. `host/run.sh` runs the
upstream TypeScript under node's native type stripping and diffs the C
dot-for-dot: 14 frames, 456/456 dots, worst deviation 0.0043 px. That harness
caught two bugs that would each have survived a look at the screen — `hashD`
needs double precision, and the reference's body breath is not gated on idle.

### The spectrum analyser

The original radial FFT display, ported from the sibling `spec_analyzer_radial`
project: 24 bands mirrored into 48 bars, bass at 12 o'clock, treble at 6, with a
warm-to-violet sweep for agent audio and a narrow cyan-to-blue for yours.

**The bar count is a memory decision, not a visual one.** Every `lv_draw_line`
allocates a mask buffer sized to that bar's screen width at its angle, from
internal RAM, and the sizes vary — which is what fragments. At 96 bars the
largest free internal block fell to 11,776 B with a live session; at 48 it is
32,768 B, against the orb's 43,008. Do not raise `STRIPE_COUNT` back without
re-measuring. Its FFT and esp-dsp buffers initialise on first activation, so a
device that never selects it never pays for them.

### Where the audio comes from

**Agent audio is tapped where it reaches the speaker**, not where it arrives from
the network. Deepgram delivers a whole turn faster than it plays — that is what
the 384 kB ring buffer absorbs — so a visualizer fed from `on_audio()` would race
ahead and finish while the speaker was still talking. The tap sits in
`playback_task()`, paced by the blocking codec write. It still leads by about
90 ms, the depth of the I2S DMA.

**The mic tap sits after the half-duplex gate**, so the display shows what
actually goes upstream. It also sits after the canceller, so the orb draws the
*cancelled* signal rather than the raw microphone.

Because the gate is off by default, the two sources can be live together — the
agent speaking and someone talking over it — which is the point. The uplink VAD
deliberately does not gate the tap: the ring keeps drawing the room even on
blocks withheld from the network, because a ring that freezes while the agent
talks reads as broken.

**There is no FFT in the default path.** The orb needs a scalar RMS for gesture
depth and three bands — from two cascaded one-pole crossovers at 250 Hz and
2 kHz — for a post-pass where low is bulk swell, mid a travelling ripple, and
high ink with no motion at all. That split is what stops a loud vowel and a sharp
consonant looking identical.

Gains are **per source**, and that is not tidiness: agent playback runs about 3x
hotter than the microphone (RMS 0.31 against 0.095), so one shared gain either
pins the agent at its ceiling or leaves the mic barely moving. Measured peaks are
now within a few percent of each other on both paths.

### Threading and memory

Analysis, layout and drawing happen in one LVGL timer on a task pinned to core 1
at priority **4**, below `audio_play` (6) and `audio_cap` (7). The adapter
defaults to 6, which would round-robin against playback; the override matters,
because a starved LVGL task drops frames while a starved audio task drops audio.
Watch `drop=` in the telemetry line to confirm it is holding.

Internal RAM is the binding constraint once the display is up, and the case that
bites is a WebSocket reconnect — a TLS handshake wanting a burst of it with the
render buffer already allocated. What matters is the **largest contiguous block**,
not the total: total free has been observed above 44 kB while the largest block
sat at 11 kB. Hence the telemetry reports `int`, `intmax`, `dma`, `dmamax`,
`ifree` and `iblocks`; the LVGL render chunk is 16 rows; LVGL uses the C library
allocator (its builtin one emits a 64 kB internal array); and every large orb
array lives in PSRAM. Note the threshold trap there:
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` is 4096, so several allocations each below
it still land internal and have to be combined into one larger block to reach
PSRAM.

**`dma` and `dmamax` are the ones to read, and that took a fault to learn.**
`MALLOC_CAP_DMA` is a strict subset of `MALLOC_CAP_INTERNAL` -- not every
internal region is reachable by the DMA engine -- and the two diverge badly under
load. Measured on this board: the DMA largest free block fell to 832 B while
plain internal still reported 7,680, and a 1,600 B request from esp-aes then
failed and dropped the Deepgram session. Every figure this firmware logged had
been the internal one, so `intmax=7680` read as "a 1,600 B request should fit"
when it never could. `DRAW_ROWS` is 16 rather than 32 for exactly this reason;
see [persistence.md](persistence.md).

If largest-block sags, `DRAW_ROWS` is no longer the first lever -- it has already
been spent. Cut `DRAW_ROWS` in
[main/ui.c](../main/ui.c) before tuning anything else.

### Why both indicators are on the sides

They used to sit across the top — the charge row at 1 to 2 o'clock, the wifi
glyph at 10 to 11 — and that is exactly where the status caption's ends land when
it swings out to the rim. The top charge dot sat on the tail of
`forget? hold again` and notched it out on every frame.

Drawing the caption last hid it, but hiding a collision is not the same as not
having one: every one of these overlays blacks its own ground before drawing, so
whichever went second simply erased part of the other. **The top of the panel
belongs to the caption; the indicators have the sides.** Both still follow the
curve, on the same radius 200 as before.

### The battery dots

**Off by default.** The orb clock's status row shows the charge whenever the
device sleeps or the clock is swiped up -- see
[the clock page](clock.md#the-sleep-clock) -- and these dots predate it.
`CONFIG_BATTERY_SHOW_DOTS` brings them back; the rest of this section is how they
work when it does.

Four dots following the display's outer curve **down the right side**,
straddling 3 o'clock, each worth a quarter of the charge and filling clockwise
so the row reads downward the way a clock face does. Spent dots go dim rather than disappearing: a row that keeps
its length reads as "three of four" at a glance, where a shrinking row of two
dots is indistinguishable from a screen that has decided to draw two dots. A
bolt continues the same arc past the last dot while current is going into the
cell.

**They take the orb's colour**, all of them, so `set_color` moves the row along
with everything else -- an indicator in its own fixed palette reads as a foreign
object on the glass. Spent dots are the same hue at a quarter brightness rather
than a neutral grey: it keeps the row reading as one object, and the held/spent
contrast survives on every colour in the catalog, which a fixed grey does not.
The bolt is at full brightness like a held dot, because it is a state rather
than a quantity.

They follow the curve because this panel is **round**: the corner an icon like
this normally lives in does not exist here, it is bezel -- and a straight row
across a round screen reads as something that fell off a rectangular one. The
positions are precomputed constants at radius 200 (33 px in from the edge) and
75/85/95/105 degrees clockwise from 12 -- straddling 3 o'clock, down the right
side -- with the bolt at 115; four sines per frame
to arrive at four constants would be a cost with nothing to show for it. Change
the radius or the angles and change them together -- nothing checks at runtime
that they still lie on a circle.

`draw_battery()` in [main/ui.c](../main/ui.c) draws them, **after the face** and
for the same reason `draw_indicators()` does: the orb clears only the boxes its
own dots occupied last frame, so an overlay drawn first comes out moth-eaten.

It also **clears what it drew**, which the first version did not, and the bench
found within a minute: the canvas keeps its pixels, so hiding the indicator by
not drawing left the last dots and the last bolt up permanently -- the bolt
outlived the cable, and the dots were only erased where an orb dot happened to
be painted over them. So the overlay blacks its whole box every frame before
drawing into them, and blacks them once more on the frame it stops being shown.
The **bolt's** box is cleared whether or not a bolt is in it: a region that
appears only while charging cannot erase the bolt it left behind.

Five small boxes, not one bounding box, and that matters on an arc -- the row's
bounding box is ~90x120, about 10,800 pixels, as much as the orb's entire
per-frame clear. Per-shape boxes cost ~1,300, and it is the same thing
`orb_raster.c` does for the same reason.

They are on screen while the session is **stopped** or the display is **asleep**,
and whenever the charge is **low** or the cell is **charging** -- the first two
being the states where someone is looking at the device rather than talking to
it, the last two being the ones they need to know about regardless. Not over a
live conversation at full charge: the face is the point then, and a permanent
gauge is clutter that also costs an invalidated box every frame. Nothing is
drawn before the first sample, so boot never flashes an empty battery.

The reading itself comes from `main/battery.c` on its own task; the UI never
touches I2C, because a blocking transfer on the frame timer lands straight in
the frame budget. `CONFIG_BATTERY_SHOW_DOTS` turns them off, separately from
`CONFIG_UI_SHOW_INDICATORS` -- that one is a bench aid, this is not.

### The signal icon

**A weak-signal warning only.** The orb clock's status row is where the signal
is read at rest, so this icon shows while the signal is **weak** and not
otherwise -- mid-conversation included, the one time it is worth interrupting
the face for.

The familiar wifi glyph -- a dot with three arcs fanning out above it -- at
**9 o'clock**, on the same radius 200 the charge dots use, so the two indicators
balance across the panel. **Four elements for four bars**, and they
line up exactly: the dot alone is one bar, each arc past it is one more. That is
the progression every phone and laptop draws, so a device showing
dot-plus-one-arc is showing what its owner already reads as poor reception. Unlit
parts dim to a quarter of the same hue rather than disappearing, the same rule
the charge dots follow.

The first version of this was the charge row mirrored -- four dots on the
opposite curve. Structurally that was nearly free, and it told the owner nothing:
two identical rows on the same glass, with only which side of the panel you were
looking at to distinguish "how full" from "how connected". **The wifi glyph needs
no legend at all**, which is the whole argument for it.

It is drawn **upright**, not rotated to the tangent like the dots it sits
opposite. A wifi glyph tipped 45 degrees stops being the familiar shape and
starts being a decoration; the arcs have to fan straight up to read. The apex
sits a little below the radius-200 mark so the glyph's mass, rather than its
lowest point, lands there.

Three arcs at outer radii 10/16/22 and a 3 px stroke -- `lv_draw_arc()` puts the
outer edge at `radius` and grows inward, the same convention the touch ring
records. Six px of pitch at three px of stroke leaves three px of gap, which is
what keeps them from reading as one thick band at arm's length. The fan spans
**90 degrees** (LVGL angles: 225 to 315, since 0 is 3 o'clock and up is 270)
rather than the 120 some icon sets use, because a wider fan at this size closes
the gap between the innermost arc's ends and the dot until the two touch.

**One box for the whole glyph**, which is the opposite call from the charge row's
five and for the opposite reason: four dots strung along an arc have a bounding
box ten times their ink, while this glyph fills its own box -- the arcs span it
horizontally, the dot holds the bottom. Per-shape boxes here would be four
overlapping rectangles adding up to more than the one they sit in. About 37x30,
~1,100 px, a little less than the charge row's five boxes cost together. Measured
on hardware it is free: frame draw time sits at 15.0 ms with the glyph up against
14.9 ms without, which is inside the noise.

What the two overlays *do* share is `arc_tint()` and the dim ratio -- the "one
colour, one dim step" rule -- and the wipe-then-draw discipline the charge dots
learned the hard way. Read the paragraphs above about clearing what you drew;
they apply here identically.

The bar count arrives **already bucketed** from `wifi_sta_get_signal()`, which is
also where the dBm thresholds and their hysteresis live -- beacon RSSI walks
several dB between beacons, so a display that bucketed the raw value would blink
an arc in and out for as long as the device sat at that distance. Promotion needs
3 dB of margin; demotion is immediate. `draw_signal()` only clamps and draws.

On screen only while the signal is **weak** -- that same hysteresed flag.
Nothing is drawn while there is no association: during provisioning the panel is
showing a QR code, and a signal icon beside it would be describing a link that
does not exist yet. `CONFIG_WIFI_SIGNAL_SHOW_BARS` turns the icon off and leaves
the TLM line and the voice tool intact.

### The sleep clock

While the panel sleeps -- 5 fps, full brightness, 30 s after a session stops --
and the time is known, it shows the orb clock: a small dotted sphere with a
meridian sweeping it, hour and minute rings of dots lit up to the time, the time
and the next alarm in the middle, and Wi-Fi and battery across the top. It is
painted into the canvas in the orb colour, and a touch removes it on the same
frame it wakes the panel. **Swipe up from the bottom edge** to bring it up on
demand; a touch puts the face back. The whole clock drifts a few pixels, too
slowly to see, because a static picture held for hours marks an AMOLED. The details are in
[the clock page](clock.md#the-sleep-clock).

### The stopwatch

A sideways swipe on the orb clock, or "start a stopwatch", brings it up: the
clock's status row and sphere, the minute ring counting seconds, the time in the
middle with its centiseconds, the latest lap under it, and two round buttons --
play/pause, and lap/reset. It owns the frame as the clock does, at 10 fps while
it counts. See [the clock page](clock.md#the-stopwatch).

### The ring screen

While a timer or alarm rings, the panel shows the ring screen instead of the
face: a small dotted orb near the top with rings pulsing out of it, the label,
the overtime counting up, and the way to end it in the bottom half -- one big
**Stop** for a timer; a **Stop** quarter beside a **+1 min** snooze quarter for an
alarm. It owns the frame as the touch panel and the clock do, ahead of both, at
10 fps, and takes the orb colour. The details, and how snooze counts up and
fades before it commits, are on [the clock page](clock.md#the-ring-screen).

### The touch panel

**Swipe down from the top edge** and a panel of settings covers the face. Swipe
sideways for the next page, swipe up to close it; it also closes itself 8 s
after the last touch, and gives way to a ringing alarm, the setup QR code and
the display test. The conversation carries on underneath: the agent keeps
talking and the microphone keeps listening.

| page | controls | the same as |
|---|---|---|
| Sound | mic gain (left) and volume (right) as linked pairs: + above, - below, and between them a bridge showing the value as a level meter, its number and its name; the button that cannot go further turns dark at a limit | `set_mic_gain`, `set_volume` |
| Colour | the 13 orb colours on a clock face, 11 + "+2"; the one in force is ringed and named | `set_color` |
| Toggles | noise reduction on/off; 12-hour/24-hour | BOOT double click; `set_clock {hours}` |
| Timers & alarms | the soonest first, one at a time, ‹ › for the rest; Cancel takes two taps | the `alarms` tool |

Nothing here is new behaviour -- every control calls what the voice tool or BOOT
already did, through `main/controls.c`. Two rules come with that:

- **No flash writes on the LVGL task.** A commit runs with the cache off and
  stalls the audio path. Volume and mic gain apply on the press and are saved
  by session_ctl's worker 2 s after the last one (`EVT settings saved`), so a
  run of presses costs one write; noise reduction and the clock format already
  deferred their saves.
- **The agent is never told.** None of these is in Settings, so there is no
  reload and no gap -- unlike the name and the voice, which stay voice-only for
  exactly that reason.

The look follows a reference sheet: buttons fill a disc inset 34 px from the
edge, split by 36 px gaps, every corner rounded to 8 px; primary white,
secondary `#1F2229`, pressed `#C8CCD4`, disabled `#16181D`. The sound page's
bridge is 76 px tall, `#4E5462` for the level and `#2B2E37` for the rest, and is
a readout only -- a tap on it does nothing. Volume's meter reads out of 100, like
its number; mic gain's spans 6-30 dB. Colours are rounded, not truncated, to
RGB565: truncating takes more off red and blue than green and turned those
neutral greys green on the panel.

**In the orb's colour.** Those are the colours on a white orb; on any other, the
panel and the rest of the screen wear it, by three rules in
[main/ui_tone.h](../main/ui_tone.h). White things become the colour, and the dark
ink on them stays dark. Dark surfaces and grey text take its hue at their own
brightness, since a straight multiply halves it on purple or blue and the dark
buttons vanish into the black. White text on a dark surface becomes the colour,
lifted towards white on the darker colours (purple most) so it keeps 3.6:1 or
better on the meter, the darkest thing text sits on. Each rule returns its input
unchanged on white, which `host/panel.sh` checks.

**Painted, not built from widgets.** LVGL has no shape for a segment of a
circle, and its widgets allocate mask buffers from internal RAM while they draw.
So the shapes are signed distance functions in `main/panel_geom.c`, which also
do the hit testing -- what is drawn is exactly what can be touched -- and
`main/ui_panel.c` paints straight into the face's PSRAM canvas; the sound page's
side-by-side halves reuse the top half's mask transposed. Evaluating them
per pixel took 812 ms for the sound page on the board, where the LVGL task
shares a core with the echo canceller; the four quarters are one shape mirrored
and the halves another, so each is rasterised once at boot into a coverage mask
(108 kB PSRAM, ~80 ms on a background task) and a paint is a lookup: 10-60 ms,
and only when something changed. Text and the stroked icons go through an LVGL
layer, in Montserrat at 18, 28 and 40 px (`font_panel_*.c`, generated). With
the panel open the largest internal block held at 23,552 throughout, the same as
closed. `host/panel.sh` checks the geometry, including that the tile and mirror
shortcuts the painter takes are exact; `CONFIG_UI_PANEL_SELFTEST` drives every
control on the board for the log -- the sound page at its limits included -- and
prints every repaint as a screen capture for `host/shot2png.py`.

### Telemetry

One machine-parseable line per second, plus `EVT` lines on every transition:

```
TLM up=123.4 face=orb beh=SPEAKING src=agent sess=ready frames=22 fps=22.8
    draw=17.4/20.1 amp=0.41/0.56 low=0.32/0.49 mid=0.55/0.71 high=0.48/0.62
    ... int=65851 intmax=49152 dma=58063 dmamax=49152 ifree=65851 iblocks=5 ialloc=521
    bat=62 mv=3841 chg=0 chgst=5 rssi=-58 bars=3 ch=6 clk=2 alm=1
EVT beh IDLE->SPEAKING after=4.20s
```

`clk=` is where the wall clock came from (0 none yet, 1 RTC, 2 SNTP), and `alm=`
how many timers and alarms are pending; see [the clock](clock.md). `panel=` is
the touch panel's page, or -1 while it is closed.

`bat=`, `rssi=`, `bars=` and `ch=` are **-1 when there is no reading** -- no
association, or no sample yet. Zero would be a lie in both cases: 0% and 0 dBm
are the extremes of their scales, and 0 dBm in particular would read as the best
signal ever measured.

It is emitted from the **main task**, not the UI: console writes block, and
logging from the frame timer would corrupt the timings it reports. `avg/max`
pairs everywhere, because speech and frame timing are both mostly-quiet with
occasional peaks and a periodic spot sample misses exactly the peaks that matter.

This replaced five logs on five drifting cadences and found four bugs within
minutes of existing — including that the orb had been permanently in its
listening pose, because the old test was "did a PCM block arrive recently" and
the capture tap fires every 80 ms whether or not anyone is speaking.
`CONFIG_UI_TELEMETRY_MS` turns it down.

## Seeing every orb state: the display test

Say **"up up down down left right left right"** and the agent hands the screen
over. Each tap advances one step; the last one restores the previous face and
starts a fresh session.

Nine steps — the orb's eight states, then `frozen`. It exists because most of
these are otherwise nearly impossible to look at: several last a second or two
mid-turn, the connection rungs need a network failure to provoke, and
**`THINKING` cannot occur in a real session at all.** `AgentThinking` has never
arrived from the stack, so until this mode existed that pose was
parity-verified geometry nobody had ever seen render.

**The microphone stays live; nothing is streamed.** The orb's amplitude scales
how *deep* each gesture goes, so at zero, `listening`, `speaking` and `idle` are
nearly the same picture — the test needs real levels to be worth anything. But
there is no session to stream to, so `audio_io_capture_set_monitor()` feeds the
display tap with the network sink shut. Its `continue` lands before the sink,
which is what makes writing to a socket that is not there impossible rather than
merely unlikely.

Only one of the four modifiers gets a step, which is not obvious: `face_orb.c`
reads `frozen` and `press_active`, while `idle` and `stopped` belong to the
spectrum. On the orb those two are already states — `idle` is step 0, and
`stopped` is what `resolve_behaviour()` turns into `DISCONNECTED` at step 7 — so
separate steps would only add poses identical to plain `listening`.
`press_active` needs no step either; every tap shows it on the way past.

**Capturing it as images.** `CONFIG_UI_SCREENSHOT_TOUR` (bench only) runs the
test on its own 10 s after boot and prints three captures of each step over
serial -- 1.5, 2.5 and 3.5 s in, so the animated states are seen at three points
of their motion -- run-length encoded, about 0.25 s a frame. Then

```
host/shot2png.py capture.log OUTDIR
```

writes a PNG per capture and `contact.png`, one row per state. The set in
[images/orb-states/](images/orb-states/) was taken that way on 2026-10-24, in a
quiet room -- which is why `listening` and `speaking` are dim; one
`initializing` frame lost lines on the link and is missing. It captures the
face's canvas, so LVGL labels on top of it -- the step's name, the status word
-- are not in the images, and the mic-driven states show the room as it was. A
capture with a line lost on the link is reported and skipped, not written
wrong.

**Seeing it move.** The captures are stills at panel resolution, three to a
state, so they cannot show motion. The
[orb page](https://dg-edcharbeneau.github.io/esp32-deepgram-voice-agent/orb.html)
on the project site runs the face live instead, on `orb_geometry.c` compiled
unchanged to WebAssembly: every state, the 280 ms transitions between them,
simulated speech driving `listening` and `speaking`, the thirteen colours, and
slow motion. It is `docs/orb.html`, built from `host/orb_web.html` by
`host/orb_web.sh` (needs Emscripten); rebuild and commit it after changing the
geometry, or the page shows the old maths.

The trigger is the fragile part, and deliberately loose. The phrase has to
survive speech-to-text — it can arrive merged, hyphenated, or with `B A` on the
end — so the function description matches the *shape* of the utterance, a run of
repeated directions, rather than any exact string.

---

[Back to the README](../README.md)
