# The display

The two faces, where their audio data comes from, what they cost in RAM, and
how to see every state without talking to the device.

## What is on the screen

The 466x466 AMOLED shows one of two interchangeable **faces**, and you switch
between them by asking out loud — see [Changing things by asking](voice-commands.md).

Both are driven by the session rather than by their own I2S read, because the
codec is already owned by `audio_io`.

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
`CONFIG_UI_DEFAULT_ORB_COLOR` picks what it boots as.

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
sat at 11 kB. Hence the telemetry reports `int`, `intmax`, `ifree` and `iblocks`;
the LVGL render chunk is 32 rows rather than 64; LVGL uses the C library
allocator (its builtin one emits a 64 kB internal array); and every large orb
array lives in PSRAM. Note the threshold trap there:
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` is 4096, so several allocations each below
it still land internal and have to be combined into one larger block to reach
PSRAM. If largest-block sags towards 40 kB, cut `DRAW_ROWS` in
[main/ui.c](../main/ui.c) before tuning anything else.

### The battery dots

Four dots following the display's outer curve from **1 to 2 o'clock**, each
worth a quarter of the charge, filling clockwise so the row reads the way a
clock face does. Spent dots go dim rather than disappearing: a row that keeps
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
30/40/50/60 degrees clockwise from 12, with the bolt at 70; four sines per frame
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

### Telemetry

One machine-parseable line per second, plus `EVT` lines on every transition:

```
TLM up=123.4 face=orb beh=SPEAKING src=agent sess=ready frames=22 fps=22.8
    draw=17.4/20.1 amp=0.41/0.56 low=0.32/0.49 mid=0.55/0.71 high=0.48/0.62
    ... int=65851 intmax=49152 ifree=65851 iblocks=5 ialloc=521
    bat=62 mv=3841 chg=0 chgst=5
EVT beh IDLE->SPEAKING after=4.20s
```

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

The trigger is the fragile part, and deliberately loose. The phrase has to
survive speech-to-text — it can arrive merged, hyphenated, or with `B A` on the
end — so the function description matches the *shape* of the utterance, a run of
repeated directions, rather than any exact string.

---

[Back to the README](../README.md)
