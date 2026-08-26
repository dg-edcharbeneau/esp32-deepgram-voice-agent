# Echo cancellation: the 70 kB was the wrong number

## Summary

`a4fa137` measured echo cancellation at **~70 kB of internal RAM**, found that it
did not fit alongside the display, and closed the question. That measurement is
correct and it measured the wrong thing: it measured esp-sr's **AFE**, the full
audio front-end, not the canceller inside it.

esp-sr also ships a **standalone AEC** — `aec_create_from_config()`,
`aec_process()`, `aec_destroy()` — with no ring buffers, no feed/fetch tasks and
no model stages. Espressif publishes its footprint at **8.2–26.9 kB of internal
RAM** depending on mode, with the bulk in PSRAM and a `caps` field to steer the
split.

Against **66,219 B** of free internal RAM with a live session, that is the
difference between "does not fit" and "fits with room to spare".

**But cancellation is now the ONLY route.** A second measurement, taken after this
document was first written, kills the cheap alternative it originally recommended:
the signal-to-echo ratio at the microphone is **-11 dB**, so the user is four times
quieter than the echo. No energy-ratio gate can work at any threshold. See "The
number that decides everything" below; it supersedes the fallback this document
used to propose.

**OUTCOME: full duplex does not work on this board, and the device ships half
duplex with a touch interrupt.** The canceller is real, measured and correct --
17.3 dB of ERLE against Espressif's own 18.3 on their vectors -- and it does stop
the device answering itself. It fails for two independent reasons that only appear
together on live hardware, both recorded under "Why full duplex was abandoned" at
the end. Everything above that section is still true and still worth having; it is
the road that led there.

**The mode recommendation in this document was wrong, and has been corrected by
measurement.** It originally chose `AEC_MODE_SR_HIGH_PERF` on the grounds that it
is cheapest on internal RAM. The SR modes apply **linear filtering only** — no
nonlinear processing — and on Espressif's own test vectors they made the signal
*worse*. `AEC_MODE_FD_LOW_COST` is the answer, and it turns out to be cheaper on
internal RAM as well. See "Priced on the bench" below.

The acoustic questions here — the -11 dB signal-to-echo ratio, the linearity
sweep — have been measured on the device. The canceller has been measured too:
**17.3 dB of ERLE, against Espressif's own output at 18.3 dB on identical
frames.** That says the integration is correct and the library performs as
advertised. It says nothing about this room, because the vectors are Espressif's
echo path, not ours.

Written 2026-08-25, prompted by turning `CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS` off
and hearing the agent answer itself. Revised the same day after an adversarial
review of every option.

## Environment

- Waveshare ESP32-S3-Touch-AMOLED-1.75C, ESP-IDF 5.5.5
- 16 kHz linear16 both directions, one duplex I2S shared by ES7210 and ES8311
- Display running, orb face, ~25 fps
- Deepgram Agent API over one TLS WebSocket

## What was tried, and what happened

`CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS=n`. The agent barged in on itself,
continuously.

The loop is closed and self-sustaining:

1. The speaker plays a reply. The mic is centimetres away with no cancellation,
   so it captures that reply.
2. `mic_to_agent()` streams it to Deepgram, which cannot tell it from a person.
3. Deepgram fires `UserStartedSpeaking`.
4. [`main.c:157`](main/main.c#L157) `on_user_started_speaking()` calls
   `audio_io_flush()` — "stop mid-sentence rather than talk over the user" — and
   the reply is cut.
5. The agent now answers what it just heard itself say. Go to 1.

Every reply triggers the next, so it does not settle. Note the failure is not
only Deepgram's turn detection: `ui.c`'s local speech gate is a second path into
`LISTENING` off the same audio, and it fires too. Both are symptoms. The cause is
that the microphone signal contains the loudspeaker signal.

This is exactly what the Kconfig help predicts, and the prediction should be left
in place:

> Do not turn this off expecting barge-in. Deepgram supplies the detection, not
> the cancellation, so with the gate off it fires on the agent's own voice and
> the agent interrupts itself — worse than no barge-in, because it corrupts the
> conversation rather than limiting it.

## The correction: AFE is not AEC

`a4fa137` measured, on this firmware, same build, AEC the only difference:

```
AEC off ... 77,959 B free internal, largest block 49,152 B
AEC on  ....  7,367 B free internal, largest block  3,584 B
```

It reached that with `AEC_MODE_VOIP_LOW_COST`, `AFE_MEMORY_ALLOC_MORE_PSRAM`,
`afe_ringbuf_size` cut from 50 to 6, and wakenet, NS and VAD all disabled — and
concluded, reasonably, that there is no combination of levers that frees 70 kB.

What that build instantiated was `esp_afe_sr_iface` — the front-end that owns
ring buffers, a feed/fetch contract, multi-channel bookkeeping and the slots for
model-based stages. Turning the stages off does not remove the machinery around
them.

The canceller on its own is a different object with a different price. Same
component, same algorithm, no front-end:

```c
aec_handle_t *aec_create_from_config(aec_config_t *config);
int  aec_get_chunksize(const aec_handle_t *handle);
void aec_process(const aec_handle_t *handle,
                 int16_t *indata, int16_t *refdata, int16_t *outdata);
void aec_destroy(aec_handle_t *handle);
```

`aec_config_t` carries `mic_num`, `ref_num`, `out_num`, `filter_length`,
`sample_rate`, `mode`, `nlp_level` — and `caps`, a `MALLOC_CAP_*` mask, so where
the memory lands is an argument rather than a fixed property.

`a4fa137` stays as written. It is a correct measurement of the AFE and it is why
nobody should reach for the AFE again.

## The number that decides everything

Added after the first draft, and it overturns this document's original
recommendation.

`ui.c` publishes a *shaped* amplitude: `shape_to_byte()` is `powf(v, 0.7f) * 255`
([ui.c:508](main/ui.c#L508)) applied to `rms * AMP_GAIN_MIC` with
`CONFIG_ORB_AMP_GAIN_MIC=600`, so `rms = amp^(1/0.7) / 6`. Inverting every level
this project has measured, in consistent units:

| | shaped amp | RMS, LSB of 32767 |
|---|---|---|
| quiet room ([ui.c:417](main/ui.c#L417)) | 0.009 – 0.014 | 6.5 – 12.3 |
| normal voice ([ui.c:417](main/ui.c#L417)) | 0.031 – 0.093 | 38 – 184 |
| **echo, measured 2026-08-25** | 0.050 – 0.344 | **76 – 1189** |

**The signal-to-echo ratio at the microphone is -11.1 dB** on geometric means:
+7.7 dB at best (loud user, quiet echo), -29.9 dB at worst. The person talking to
the device arrives about four times quieter than the device's own voice.

### Why that kills every energy-ratio gate

The microphone sums power, not amplitude: `mic^2 = echo^2 + speech^2`. So the
excess a talker produces over the echo alone is not their level — it is what
their power adds on top:

| talker, relative to echo | excess in the mic |
|---|---|
| equal level (0 dB) | **+3.01 dB** |
| typical (-11.1 dB) | **+0.33 dB** |

A third of a decibel. Any practical detector's own error bars are several dB
wide, so there is no threshold that opens for a person and stays shut in an empty
room. This holds regardless of which reference feeds the comparison, which is why
it kills the software-envelope gate and the hardware-reference gate together.

### What it does NOT kill

Cancellation subtracts a *correlated estimate* rather than comparing energies, so
it is not bound by this arithmetic. Working from the same numbers:

| goal | required ERLE |
|---|---|
| residual echo 6 dB under a typical voice | **17.1 dB** |
| typical echo below the room noise floor | **30.5 dB** |
| worst-case echo below the room noise floor | **42.5 dB** |

17 to 30 dB is ordinary for a linear canceller with a good reference. That is the
case for continuing — conditional on the echo path being linear, which is the one
thing nobody has measured. See "The go/no-go".

**A units warning, because this project has been bitten by exactly this before.**
The three targets above compare RMS against RMS. It is tempting to compare the
echo's RMS-derived LSB against the `mic peak` figures in the serial log (26-35 in
a quiet room) and conclude ~30 dB suffices in the worst case; that mixes a peak
statistic with an RMS one and flatters the answer by more than 10 dB. `a4fa137`
records the same class of error with largest-free-block against total-free.

## The price list

From [ESP-SR — Acoustic Echo Cancellation, ESP32-S3][s3], at 16 kHz, single
channel:

| Mode | Internal RAM | PSRAM | CPU |
|---|---|---|---|
| `AEC_MODE_SR_HIGH_PERF` | **8.2 kB** | 100.1 kB | 14.1 % |
| `AEC_MODE_SR_LOW_COST` | 18.8 kB | 64.0 kB | 7.2 % |
| `AEC_MODE_FD_HIGH_PERF` | 20.3 kB | 126.2 kB | 25.3 % |
| `AEC_MODE_VOIP_LOW_COST` | 26.9 kB | 64.1 kB | 27.3 % |
| `AEC_MODE_FD_LOW_COST` | 30.9 kB | 90.0 kB | 19.6 % |
| `AEC_MODE_VOIP_HIGH_PERF` | 69.2 kB | 66.6 kB | 31.6 % |

Sorted by the number that decides it here. Two things to read off this table
before trusting it:

**The last row is the one `a4fa137` measured.** 69.2 kB against a measured 70 kB
is close enough to confirm the reading: that build was paying VOIP_HIGH_PERF-ish
prices through the AFE. The rest of the table is what was never priced.

**`HIGH_PERF` is cheaper on internal RAM than `LOW_COST`, in both families.** The
names describe cancellation quality and CPU, not memory, and the high-performance
modes push more of their state to PSRAM. Since internal RAM is the binding
constraint here and PSRAM is not — there is ~5.9 MB of it free — the naming works
against the reader. **But the published internal figures are wrong for both
LOW_COST rows** — measured, `FD_LOW_COST` costs 16 bytes, not 30.9 kB. The table
below is kept because it is what Espressif publishes; the measured column in
"Priced on the bench" is what to trust.

**Which family — and this was answered wrongly here at first.** The original
reasoning was that the SR modes suit automatic speech recognition, which is what
this device feeds, so SR was "the right one, and it happens to be the cheap one".

Both halves are false. The ESP-SR page says SR is *"linear filtering only"* while
FD is *"linear filtering + nonlinear processing, suitable for Full-Duplex
dialogue"* — and barge-in is precisely the full-duplex case. Measured on the
bench, the SR modes score **negative**. FD is also cheaper on internal RAM, not
dearer. See "Priced on the bench".

**A claim that turned out to be false, kept here because it was acted on.** It
was reported that `SR_HIGH_PERF` is the only mode honouring `caps`: Disassembly of the shipped library shows its seventeen
`heap_caps_aligned_calloc` calls all take `config->caps`, so `MALLOC_CAP_SPIRAM`
moves essentially the whole allocation to PSRAM and leaves the internal draw near
200 bytes with no single block above ~3 kB. `SR_LOW_COST` ignores the field and
hard-wires its 18.8 kB internal regardless of what you ask for.

Measured, that is wrong: `SR_LOW_COST` takes 500 B internal and `FD_LOW_COST`
takes 16 B. It came from a subagent's disassembly, was flagged unverified, was
relied on anyway to pick a mode, and is now refuted by the bench.

### Caveat on the numbers

The S3 page's own test-setting line reads:

> Test setting: ESP32-P4 @ 240 MHz, `CONFIG_ESP32S3_DATA_CACHE_64KB=y`,
> `CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y`.

It names a P4 while citing S3 cache symbols, so it is wrong in one direction or
the other. The [P4 page][p4] states *"ESP32-P4 @ 400 MHz"* and carries **the same
internal/PSRAM figures for every SR and VOIP mode**, different CPU percentages,
and different FD figures.

That pattern is consistent with RAM being a property of the algorithm and CPU
being a property of the chip — which is the reading this document takes. So:
**treat the RAM column as sound and the CPU column as needing measurement here.**
The roughly 2× gap between the two pages' CPU figures is about what 240 MHz
against 400 MHz would predict, which is mild corroboration and not proof.

## Priced on the bench

Measured 2026-08-25 on this board, esp-sr 2.5.1, against Espressif's own AEC test
vectors rather than against the room. `aec_in_far.wav` and `aec_in_near.wav` are a
far/near pair; `aec_test_fd.wav` is what Espressif's own AEC produces from them.
Nothing is played and nothing is recorded — the files are read from flash straight
into `aec_process()`, so the same bytes go in every run.

All modes at `filter_length = 4`, `sample_rate = 16000`, 1 mic / 1 ref / 1 out,
`caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`, `nlp_level = AEC_NLP_LEVEL_AGGR`.
Baseline before any of them: 97,811 B free internal, 86,016 B largest block.

| mode | internal | PSRAM | largest block | NLP applied | **ERLE** | whole-file |
|---|---|---|---|---|---|---|
| **`FD_LOW_COST`** | **-16 B** | 123.4 kB | **unchanged** | **yes (512)** | **17.3 dB** | 2.6 |
| `FD_HIGH_PERF` | 8,472 B | 141.5 kB | 81,920 -> 73,728 | yes (512) | 17.9 dB | 2.8 |
| `SR_HIGH_PERF` | 8,616 B | 102.5 kB | 81,920 -> 73,728 | **no (0)** | **-5.9 dB** | -1.1 |
| `SR_LOW_COST` | 500 B | 84.4 kB | unchanged | **no (0)** | **-5.6 dB** | -0.7 |
| *Espressif's own output* | | | | | *18.3 dB* | *2.3* |

`aec_get_chunksize()` returns **512** for every mode, matching `handle.frame_size`
— so `CAPTURE_FRAMES` should be 1024, two frames exactly, rather than today's 1280.
`aec_destroy()` returns everything; the largest leak seen was 104 B of
fragmentation noise.

### The two columns, and why there are two

**ERLE is the real figure**, measured over single-talk frames only: far end active,
near end silent. **Whole-file** is total energy reduction across the entire vector,
and it is *not* ERLE — the first version of this bench reported only that, and it
was misleading. Espressif's own output scores 2.3 by it, and their canceller is
plainly better than 2.3 dB. The reason is that their vector contains near-end
speech, a canceller must not remove the talker, and so whole-file energy reduction
conflates removing the echo with removing everything.

Nothing labels the single-talk frames, so they are inferred, and the inference is
worth stating because the result rests on it:

- during echo-only, `near ~= g * far` for the echo path's gain `g`;
- during double-talk the talker only ADDS energy, never subtracts.

So across far-active frames the LOW percentile of `near/far` is the echo path
alone. The bench takes the 20th percentile as `g` and admits frames within 6 dB of
it. Deliberately conservative: misclassifying double-talk as echo-only would
*understate* ERLE, which is the safe direction to be wrong in.

On these vectors that finds **210 echo-only frames of 1669 (13%)**, with
`g = 0.4588` — an echo return loss of 6.8 dB on Espressif's own path. Every mode
and the reference are scored on the identical frame set.

**Both columns are reproducible**: two runs, same board, gave identical figures to
the decimal.

### NLP level, on the mode that ships

The per-mode table above was taken entirely at `AEC_NLP_LEVEL_AGGR`, which the
bench hardcoded, while the firmware runs `NORMAL` -- so the 17.3 dB quoted as
justification described a configuration the binary did not use. Measured
separately, same vectors, same echo-only frames:

| `nlp_level` | ERLE |
|---|---|
| `NORMAL` *(what ships)* | **16.1 dB** |
| `AGGR` *(esp-sr's default)* | 17.3 dB |
| `VERYAGGR` | 18.0 dB |
| *Espressif's own output* | *18.3 dB* |

**NORMAL costs 1.2 dB against AGGR**, and that is the whole price of choosing the
setting that damages speech least. Non-linear processing suppresses hardest during
double-talk, which is the barge-in moment, so the aggressive settings trade away
the thing being built. 1.2 dB is a cheap insurance premium — and it can be changed
on a live handle with `aec_set_nlp_level()` if a person talking over the device
turns out to need it.

Note 16.1 dB sits just under the 17.1 dB needed to put residual echo 6 dB below a
typical voice. Marginal by design rather than by accident.

### What the ranking does settle

**FD, not SR, and it is not close.** The SR modes score negative — they add energy
rather than removing it. The cause is visible rather than inferred:
`aec_nlp_process()` documents "or 0 if NLP is not applied", and it returns **512
for both FD modes and 0 for both SR modes**. Linear filtering alone, on a vector
containing double-talk, does not help.

This confirms the ESP-SR page ("SR: linear filtering only"; "FD: linear filtering
+ nonlinear processing, suitable for Full-Duplex dialogue") and **contradicts the
header**, whose `aec_create()` doc comment says "recommend to set
AEC_MODE_SR_LOW_COST". Barge-in is the full-duplex case; take the page's advice,
not the header's.

### The published table is wrong for the LOW_COST modes

Published against measured internal RAM, same chip, `caps` set to PSRAM:

| mode | published | measured | |
|---|---|---|---|
| `FD_LOW_COST` | 30.9 kB | **-16 B** | published figure is ~2000x the truth |
| `SR_LOW_COST` | 18.8 kB | 500 B | |
| `SR_HIGH_PERF` | 8.2 kB | 8,616 B | matches |
| `FD_HIGH_PERF` | 20.3 kB | 8,472 B | |

The HIGH_PERF rows are close to published; the LOW_COST rows are not remotely.
The likely explanation is that the published figures were taken without `caps`
steering, but that is a guess — what is certain is that **the row that made
FD_LOW_COST look unaffordable is off by three orders of magnitude**, and the mode
this document rejected on memory grounds is in fact the cheapest of the four.

**The `caps` claim in this document was also wrong.** It said only `SR_HIGH_PERF`
honours the field and that `SR_LOW_COST` "ignores it and hard-wires 18.8 kB
internal". Measured, `SR_LOW_COST` takes 500 B and `FD_LOW_COST` takes 16 B. That
claim came from a subagent's disassembly, was flagged unverified, and is now
refuted. One caveat on the reverse test: running `FD_LOW_COST` with
`MALLOC_CAP_DEFAULT` gave 284 B internal, barely different — because this build
sets `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`, so large allocations already
prefer PSRAM. The `caps` flag could not be isolated by that comparison.

### Why largest-block is the number to watch, and why it is fine

`a4fa137` failed because largest contiguous internal block collapsed 49,152 ->
3,584 and mbedTLS could not get a handshake buffer. Both LOW_COST modes leave
largest block **completely unchanged**; the HIGH_PERF modes cost 8 kB of it.
Note the baseline here is 86,016 B because the bench runs at boot, before a
session; with a live session this board measures 17,408-31,744 B. FD_LOW_COST
taking essentially nothing internal is what makes that irrelevant.

### How to re-run it

`CONFIG_AEC_BENCH=y`, then `idf.py flash` and `idf.py storage-flash` once. The
vectors live in the `storage` SPIFFS partition, which was already in
`partitions.csv` and unused, so no layout change was needed. Only three of the
five vectors fit: 6.8 MB is 93% of the 7 MB partition and spiffsgen refuses it,
where three files is 70%. `aec_test_sr.wav` is the one left out; swap it in for
`aec_test_fd.wav` to check SR against its reference.

## The budget it has to fit

Measured on this build during a clean boot capture, from `main.c`'s `TLM` line:

| | Free internal | Largest block |
|---|---|---|
| Session live | **66,219 B** | 45,056 B |
| Session stopped | 83,207 B | 45,056 B |

Total free heap at the same moment is 5,986,027 B, essentially all of it PSRAM.

The live-session row is the one that matters. TLS buffers are held while a
session is up, and a canceller has to coexist with them — `a4fa137`'s "~78 kB
free" was the idle figure, which is ~17 kB more generous than the state the
canceller actually has to live in.

`SR_HIGH_PERF` at 8.2 kB would leave ~58 kB. `SR_LOW_COST` at 18.8 kB would leave
~47 kB. The AFE left 7,367 B.

**But total free is not what broke it last time.** With the AFE enabled the
session never connected at all — `sess=stopped, turns=0, rx=0, mic=0` — because
mbedTLS could not get a buffer for the handshake, failing with
`esp_transport_write() returned 0`. Largest *contiguous* block went 49,152 →
3,584. It is 45,056 B today.

So the gate for any future attempt is two numbers, not one: free internal **and**
largest block, both with a session live, and the handshake completing is the
actual pass condition. `a4fa137` already learned this the expensive way — an
earlier commit in that series reported the AFE at "~28 kB" from the change in
largest-block alone, which is the wrong metric because largest-block moves when
the arena is carved rather than by the amount consumed.

## What an implementation would require

Not a design. A list of what is already known, so the next session does not
rediscover it.

**1. The reference lane needs 4-channel TDM capture.** The board wires an echo
reference from the ES8311's outputs through a differential RC network into ES7210
MIC3P/MIC3N, every component populated. Nothing enables it: the BSP builds the
ES7210 with `mic_selected` at 0 and falls back to MIC1|MIC2, and SDOUT2 — the
non-TDM route to MIC3 — is cut on this board (R48 is NC), so **4-channel TDM is
the only way the reference reaches the S3**.

`9479446` proved it and the code is recoverable: `git show 9479446` brings back
`main/audio_codecs.{c,h}` (206 + 46 lines) plus 135 lines of `audio_io.c` -- all
since removed again, see "Where the removed code lives". Two
non-obvious facts live in that diff:

- The S3's I2S RX cannot be put in TDM mode independently of TX — they share
  BCLK/WS in full duplex and must be configured identically. So the ES7210's
  4×16-bit TDM frame is read as **2 channels × 32-bit**, each 32-bit word
  carrying two 16-bit slots.
- `es7210_set_fs()` halves the requested width in TDM mode, so you ask for **32**
  to get 16-bit slots. Asking for 16 gives `bits >>= 1` → 8, and
  `es7210_set_bits()` has no case 8.

**2. Slot order is `[R, M, N, M]` — gather `[1,3,2,0]`.** A 32-bit word arrives
MSB-first and stores little-endian, so each pair swaps in memory. The ordering
was established rather than guessed, using the permanently dead lane: MIC4 is
AC-coupled to AGND and reads 3–9 always, which identifies its position and
therefore everyone else's. Per-lane peaks are in the README's echo section.

**3. `aec_process()` wants planar channels**, "ch0 ch0 ch0…, ch1 ch1 ch1…", not
interleaved. The capture loop already de-interleaves
([`audio_io.c:218`](main/audio_io.c#L218)); a planar gather costs nothing extra
because the copy is happening anyway.

**4. Alignment is free on this board — but linearity is not.** The reference is
captured by the same ADC, on the same clock, in the same frame as the mics, so it
is sample-aligned by construction. That is normally the hard part of retrofitting
AEC and it is genuinely solved here.

What is *not* solved, and what the first draft of this document glossed over: the
reference taps the **ES8311 line outputs, upstream of the NS4150B amplifier**. So
does the software playback tap. Neither sees what the amplifier and the speaker
actually did. Any distortion they add is present in the microphone and absent from
the reference, and no linear filter of any length removes it. It is a hard ceiling
on achievable ERLE, and the microphone is sitting at **-2.3 dBFS** on echo alone
(25,102 of 32,767) through 24 dB of PGA at volume 100 — which is exactly where
clipping and compression would be expected.

This also removes the main reason to prefer the hardware lane over the software
tap. The hardware lane is still better — it is sample-aligned where the software
tap's lead is DMA *occupancy*, swinging from 0 at turn start to ~90 ms at steady
state and reset by every flush — but not because it sees the amplifier. It does
not.

**5. `filter_length`** is documented with a recommended value of 4 for
`aec_create()`. `aec_get_chunksize()` gives the frame size the handle wants,
which will not necessarily match `CAPTURE_FRAMES`.

## Other candidates, priced

`a4fa137` signed off with "Speex/WebRTC AEC3 are far smaller than esp-sr's AFE
and would be the thing to price next". Priced, with one correction.

**SpeexDSP MDF** — computed from `speex_echo_state_init_mc` in
[`libspeexdsp/mdf.c`][mdf], for frame 320 / filter 1600 (a 100 ms tail at 16 kHz,
which the [Speex manual][speex] suggests for a small room), fixed-point, one mic
and one reference:

**115.8 kB total**, dominated by `st->W` at 41.6 kB, then `st->X` at 7.7 kB and
`st->play_buf` at 3.2 kB.

Allocation is steerable through the `speex_alloc` macros, so most of that could
be PSRAM — but then the adaptive filter's hot loop runs against PSRAM every
frame. Larger than esp-sr's AEC, not already ported, and it would be this
project's own port to maintain. Second choice. An unofficial ESP32 port exists
([rjsachse/ESP32-SpeexDSP][speexesp]) — noted as a lead, not read in depth, and
Arduino-oriented.

**WebRTC — correcting the pointer, and it is better than expected.** **AEC3 is not
the small one.** It is the float canceller from the browser stack, sized for
desktops with real memory. The small one is **AECM**, the fixed-point mobile
variant, and priced against this sdkconfig it comes to roughly **29.5 kB total —
~4.3 kB internal and ~25.2 kB PSRAM, with no single internal allocation above
404 B.** That is competitive with esp-sr on memory. It stays second choice only
because it is the least integrated: no ESP-IDF component, no Espressif support,
and extracting it from the WebRTC tree is itself the project.

It has one property nothing else here has, and it decides where it belongs: AECM
carries a **400 ms binary-spectrum delay estimator** plus an `msInSndCardBuf`
hint, so it is the only candidate that can tolerate a reference whose lead wanders
from 0 to 90 ms. **If the 4-channel hardware lane ever fails its gate and the
software tap is the only reference available, AECM is the right answer and esp-sr
is not.** Note its own limit though: a Geigel double-talk detector at an ERL of
about -2.4 dB means "talk louder than the speaker" — so it needs the non-acoustic
interrupt alongside it.

**A bespoke canceller was considered and rejected.** Roughly 500 lines of
hand-written partitioned-block frequency-domain adaptive filtering on esp-dsp's
FFT, which is already a dependency. The hardest part of it — double-talk
detection — is exactly what decides the outcome and exactly what esp-sr has
already tuned. It also carries silent-wrong-answer traps: `dsps_fft2r_init_fc32`
performs both `dsps_gen_w_r2_fc32()` and `dsps_bit_rev_fc32_ansi()`, and a
private-table shortcut that omits the bit-reverse returns `ESP_OK` while producing
garbage, so the filter never converges and nothing says so.

**Ranking, on internal RAM, which is what decides it:**

1. **esp-sr standalone AEC, `FD_LOW_COST`** — 16 B internal, 123 kB PSRAM,
   largest block untouched, NLP active, and it matches Espressif's own output on
   their vectors. Measured, not published.
2. esp-sr standalone AEC, `FD_HIGH_PERF` — 8.5 kB internal, 141 kB PSRAM, scores
   marginally higher (2.8 vs 2.6) but costs 8 kB of largest block for it.
3. WebRTC AECM — ~4.3 kB internal, ~25.2 kB PSRAM. Still the right answer *if* the
   reference ever has to be the software tap, whose lead wanders 0-90 ms.
4. SpeexDSP MDF — 115.8 kB, mostly relocatable to PSRAM, own port.
5. esp-sr `SR_*` — **do not use.** Linear filtering only, no NLP, scored negative.
6. Bespoke PBFDAF — rejected, above.

## The cheap fallback does not exist

**This section previously recommended a reference-based interrupt gate as the
first thing to build. That recommendation was wrong and is withdrawn.**

The proposal was: with a reference in hand, compare per-frame mic energy against
what the reference predicts and pass audio only when the mic exceeds it by a
margin. It fails on the arithmetic above — a typical talker moves the microphone
by 0.33 dB — and it fails a second, independent way for the software variant:
`s_play_tap` fires at [audio_io.c:176](main/audio_io.c#L176) *before* the blocking
`esp_codec_dev_write()`, so its lead is DMA occupancy. That is zero at the start of
a turn, climbs to ~90 ms, and is reset to zero by every `audio_io_flush()` — it
swings across precisely the moments barge-in has to work.

Two other documents in this repo carry the same dead idea and should be read with
this section in hand: `README.md`'s echo section, and `a4fa137`'s closing note.

There is a third reason, worth recording because it is measurable rather than
theoretical. The reference lane is **quieter than the microphones**: `9479446`
measured lane 0 at 7,867-9,359 against mic lanes at 11,439-12,353, an echo return
loss of about **-2.4 dB**. The microphone hears the speaker slightly *louder* than
the reference tap does. A ratio gate calibrated on that has no headroom at all.

Building it first would spend a week and de-risk nothing. The canceller's own
double-talk detector replaces it.

## Deepgram has nothing to offer

Closed directions, so nobody re-explores them:

- **There is no interrupt or cancel message.** `/v1/agent/converse`'s client
  control-frame enum is literally `[KeepAlive]`. The `Interrupt` message that
  turns up in searches belongs to `/v2/speak`, a different endpoint.
- **`eot_threshold` is the wrong knob.** It governs `EndOfTurn`; barge-in fires on
  `StartOfTurn`. Raising it would make the capture worse, not better.
- **`InjectAgentMessage {behavior: "interrupt"}` is a substitution, not a cancel.**
  Used as a stop button it makes the device blurt something every time the user
  interrupts.

The interrupt half of barge-in was never the missing piece anyway. It is already
built and correctly wired: [dg_agent.c:842](main/dg_agent.c#L842) ->
[main.c:157](main/main.c#L157) -> `audio_io_flush()`. The device does not lack a
way to stop the agent. It lacks a way to decide which microphone audio deserves
to be sent, and that decision is local by construction.

**Caveat, and it matters here more than usual.** All of the above is external
documentation this device has never observed on the wire, and this repo already
carries one spec-versus-account divergence: see `UPDATESPEAK-ISSUE.md` and
[dg_agent.c:199](main/dg_agent.c#L199). A five-minute wire test is worth folding
into whatever gets built next.

## Also dead: winning by level alone

Recorded with numbers so it is not re-proposed:

- **Mic PGA reduction: 0.0 dB** on the echo-to-voice ratio, by construction. Echo
  and voice pass through the same ES7210 amplifier
  ([audio_io.c:477](main/audio_io.c#L477) applies one gain to the device, not per
  lane).
- **Choosing the far microphone: ~0 dB.** `9479446` measured 11,439 against 12,353
  between the two MEMS lanes — 0.67 dB — with identical idle floors, and the echo
  is correlated across the pair, so the realisable figure is smaller still.
- **A high-pass: ~0 dB**, plausibly negative given the HF tilt noted at
  [ui.c:131](main/ui.c#L131).
- **Ducking the speaker while listening: 16.0 dB of total travel, and the last 8
  are inaudible.** The volume curve maps vol 0 to `EXTRA_GAIN_DB - RANGE_DB` and
  vol 100 to `EXTRA_GAIN_DB` ([audio_io.c:456](main/audio_io.c#L456)); with
  `RANGE_DB=20`, `EXTRA=0` and a runtime floor of `VOLUME_MIN 20`
  ([audio_io.c:84](main/audio_io.c#L84)), volume 20 to 100 spans -16 dB to 0 dB.

Against a 17-42 dB requirement, the whole category is out. It survives only as a
possible *precondition* for the canceller: if the linearity measurement below
shows clipping at volume 100, lowering the volume becomes mandatory rather than
optional — and every constant in `ui.c` was calibrated at the present gain, so
that change requires a recalibration pass in the same commit.

## The go/no-go

**The single highest-value unknown is whether the speaker path is linear at
`CONFIG_AUDIO_OUT_VOLUME=100`.** Not the SER, which the numbers above pin from
data already in the tree. Not the RAM, which the price list answers and a TLS
handshake cheaply tests. Linearity is the one measurement that flips the decision
between building the canceller and stopping, and **no measurement of it exists
anywhere in this repo.**

It costs roughly 40 lines of accumulators on top of a restored 4-channel capture,
and it should be scheduled *before* the canceller rather than after.

**Part A, level proportionality — unattended.** With the 4-lane capture live and
the reference lane at 0 dB gain, accumulate per 80 ms block: `sum(mic^2)` over
lanes 1 and 3, `sum(ref^2)` over lane 0, and a count of samples >= 32,000 on each.
Have the agent speak the same greeting at `CONFIG_AUDIO_OUT_VOLUME` of 40, 60, 80
and 100 — [dg_agent.c:684](main/dg_agent.c#L684) sends it on every fresh session,
so this is free and repeatable. Log `10*log10(sum(mic^2) / sum(ref^2))` per run.

- Ratio flat within ~1 dB across all four volumes and no samples >= 32,000:
  **the path is linear, build the canceller.**
- Ratio rises with volume, or clip counts appear: **compression or clipping.** The
  highest volume at which the ratio is still flat becomes the ceiling.
- Not flat even at volume 40: the transducer is the limit. **Stop.**

**Part B, the residual bound — unattended.** At each volume also fit the single
scalar `k` minimising `sum((mic - k*ref)^2)` and report
`10*log10(sum(mic^2) / sum(residual^2))`. This is pessimistic — it is memoryless,
so the room's impulse response inflates the residual — but a *rising* trend as
volume drops is direct evidence that the ceiling is nonlinearity rather than room
acoustics.

**Part C, double-talk — HUMAN IN THE LOOP.** Verbatim instruction for whoever runs
it: *"Stand at your normal conversational distance from the device. Start a
session. While the agent is speaking its greeting, say clearly: 'stop, I want to
ask something else.' Do not raise your voice above normal. Repeat five times."*
Log mic and reference RMS through those blocks. This gives the actual SER at the
intended use distance, and tells you whether the +7.7 dB or the -29.9 dB end of
the range describes your users.

### If it passes

Restore the 4-channel TDM lane first (`git show 9479446`), telemetry only, gated
on the display still coming up **and a TLS handshake completing** — that, not
"the buffers allocated", is the pass condition `a4fa137` failed. Three things must
not be restored verbatim:

1. The probe downmixes `(f[0] + f[1]) / 2`, but lane 0 is the reference. Shipping
   that line mixes the echo reference into what goes to Deepgram. It must be
   `(f[1] + f[3]) / 2`.
2. `CONFIG_MIC_IN_GAIN=24` is applied to the device, so it hits all four lanes
   including the reference. The reference lane wants 0 dB via
   `esp_codec_dev_set_in_channel_gain` — and note the channel mask indexes
   MIC1..MIC4 (reference = bit 2) while *in memory* the reference is lane 0.
   Confusing the two index spaces silently attenuates a microphone.
3. Set `dma_frame_num = 120` in the I2S bring-up. 32-bit frames are 1,920 B per
   descriptor against 960, and the default would add ~11.5 kB of internal DMA —
   which is `a4fa137`'s failure mode arriving by a different route, and a one-line
   prevention.

Then the canceller: **`AEC_MODE_FD_LOW_COST`**, `caps = MALLOC_CAP_SPIRAM |
MALLOC_CAP_8BIT`, `sample_rate = 16000`, `filter_length = 4`, `mic_num = ref_num =
out_num = 1`, `nlp_level` to be tuned on hardware. `aec_get_chunksize()` returns
512 — confirmed on device, matching `handle.frame_size` — so **`CAPTURE_FRAMES`
should move from 1280 to 1024**, two AEC frames exactly, remainder zero, send rate
still ~15.6/s. Leaving it at 1280 forces an alternating 256/0 residue.

Buffers passed to `aec_process()` must come from `heap_caps_aligned_alloc(16, ...)`
— the header warns about it twice. `audio_io.c`'s capture buffers currently use
plain `heap_caps_malloc`; the aligned pattern already exists in
`face_spectrum.c`.

`partitions.csv` needs no change: esp-sr's model packing is guarded and its
`else()` branch is a no-op, and the factory partition is 8 MB against a 1,647,920 B
image.

### If it fails

The device stays half-duplex, and the honest product answer is a **non-acoustic
interrupt** — a touch on the display ring stops the reply — or push-to-talk, which
makes the echo problem structurally impossible and reads as a deliberate
interaction model rather than an apology. Both work in every room, which no
canceller does.

## A second risk no measurement settles

This section was written about the wrong mode. It described `AEC_NLP_LEVEL_AGGR`
as a risk while recommending SR — where, as the bench then showed,
`aec_nlp_process()` returns 0 and the NLP level does nothing at all. Corrected:

**Now that the mode is FD, the risk is real.** NLP is active (`aec_nlp_process()`
returns 512), and non-linear processing suppresses hardest *during double-talk*,
which is exactly the barge-in moment. `AEC_NLP_LEVEL_AGGR` may stop the empty-room
loop while also swallowing a normal-volume interruption; `NORMAL` leaks more echo.
There may be no setting that does both.

`aec_set_nlp_level()` changes it on a live handle, so this is tunable without a
rebuild — but only with a person talking over the device, and only once the
canceller is in the live audio path.

## The absolute number, and whose echo path it belongs to

**Measured: `FD_LOW_COST` achieves 17.3 dB of ERLE**, against Espressif's own
output at 18.3 dB on the identical frames. The canceller does what it claims, and
our integration gets within a decibel of its author's result.

Held against the requirements derived earlier in this document:

| target | needs | verdict |
|---|---|---|
| residual echo 6 dB under a typical voice | 17.1 dB | **met, barely** |
| typical echo below the room noise floor | 30.5 dB | not met |
| worst-case echo below the room noise floor | 42.5 dB | not met |

So on this evidence a canceller stops the agent transcribing itself in ordinary
conversation, and does *not* bury the echo in silence.

**THE CAVEAT THAT MATTERS: this is Espressif's echo path, not ours.** Their vector
has an echo return loss of 6.8 dB. This board, measured with the reference lane at
12 dB, sits at 15-17 dB. Different rooms, different speakers, different coupling —
17.3 dB does not transfer. What transfers is that the canceller is correctly wired
and performs to specification, which is exactly what a bench can establish and an
acoustic test cannot.

The remaining measurement is the live path: reference lane and microphone into
`aec_process()` on this board, in this room. That is the only thing that answers
whether *this* speaker and *this* microphone can be cancelled, and it is Stage 4.

So: **do not promise voice barge-in on this board.** The canceller is now known to
fit, known to be wired correctly, and known to be the FD family. Whether it
suppresses enough is still unmeasured.

## Open risks

Questions the next session has to answer, not assumptions to carry forward.

**Does esp-sr still pin `esp-dsp`?** It previously required exactly 1.8.0 while
this project asks for `^1.8.2`, which is recorded in
[`main/idf_component.yml`](main/idf_component.yml). A conflict there is a
dependency-resolution problem before it is an audio problem.

**Does the standalone AEC need the `model` partition?** Models are for WakeNet
and MultiNet; a signal-processing canceller should need none. But
[`partitions.csv`](partitions.csv) has no `model` partition and is deliberately
byte-identical to `spec_analyzer_radial`'s so the two firmwares share one flash
layout and can be swapped without an erase. Changing it costs more than the
bytes. The previous experiment pulled 1.1 MB of flash, most of it presumably
model data that a standalone AEC would not want.

**Does the unresolved fetch fault still apply?** `a4fa137` left one: 500 of 500
`fetch` calls returned non-NULL with `data_size <= 0` and `ret_value -1` while
`ringbuff_free_pct` sat at 0.92. That was the AFE's ring buffer, which the
standalone API does not have — so it may simply not apply. It should not be
assumed away.

**CPU on core 0.** The published percentages may be a P4's. The display holds
~25 fps on core 1 today at 18 ms draw. `a4fa137` did establish the structure that
works: capture task back to 4 kB doing nothing but reading the codec, DSP on core
0 with a PSRAM stack, and the display held 26 fps throughout.

**Whether cancellation is even enough.** Nothing here measures ERLE on this
hardware. A canceller that fits and runs but only achieves 10 dB will not stop
the loop. The gate that matters is not "does it initialise" — the AFE initialised
fine — it is "does the agent still answer itself".

**Whether 4-channel TDM survives a real session.** `9479446` proved the lane
exists; it never ran a conversation. 32-bit duplex at 1.024 MHz BCLK alongside
Wi-Fi, TLS and a 25 fps display is unproven, and the pass condition is ten minutes
with zero `esp_codec_dev_read/write` failures.

**Whether the reference lane clips.** `9479446` measured lane 0 at 7,867-9,359
while mic lanes read 11,439-12,353. Today's mic peaks are 25,102 — about 6 dB
hotter — which puts the reference near 16-19 k. Headroom is thin, not absent, and
a clipped reference is a nonlinearity in the one signal that must be clean.

## Why full duplex was abandoned

Written 2026-08-26, after taking `CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS` off with the
canceller running. Two reasons, either of which would be enough.

### 1. The link cannot carry it

With the gate on, the device sends nothing while the agent speaks. With it off, it
pushes 32 kB/s upstream continuously, straight through the inbound reply burst.
Internal RAM then falls in regular ~3,656 B steps -- about 2.5 x MSS, the size of a
queued TCP segment chain -- until a 1,630 B `MALLOC_CAP_INTERNAL|DMA|8BIT`
allocation fails, TLS drops and the WebSocket reconnects. Fourteen sessions in
200 seconds.

It is not a leak: free internal recovers fully on teardown, so the memory is held
by the connection. `sdkconfig.defaults` already documents the other side of the
same wall -- the stock 5,760 send buffer was raised to 23,040 precisely because a
full queue makes the socket unwritable and the client treats that as fatal.
Measured, moving between those walls:

| `LWIP_TCP_SND_BUF_DEFAULT` | lowest free internal | alloc failures | unwritable-socket failures |
|---|---|---|---|
| 23,040 *(project default)* | 4,299 | 3 | 1 |
| 8,640 | 12,495 | 2 | 6 |

Reducing it raises the floor and trades one failure for the other. There is no
value that avoids both, because the problem is the bandwidth full duplex creates,
not the size of the buffer absorbing it.

Returning 12.3 kB of internal RAM -- moving the capture and playback `stereo`
buffers to PSRAM, which they did not need to leave -- bought 25 seconds before the
same floor. That change is worth keeping on its own merits and is still in the
tree; it is not a fix for this.

### 2. It does not deliver barge-in anyway

The point of all of it. With the canceller running, the gate off, and a person
talking over the agent at normal distance and volume: **`UserStartedSpeaking` never
fired during playback.** The telemetry shows `mic=` climbing steadily throughout
(477k -> 694k), so the microphone audio was reaching Deepgram the whole time. It
simply was not distinguished from the residual echo.

That is the -11 dB signal-to-echo ratio arriving where the arithmetic said it
would. 16.1 dB of ERLE at `nlp=NORMAL` sits just under the 17.1 dB needed to put
residual echo 6 dB below a typical voice, and `AGGR` offers only 1.2 dB more while
suppressing hardest during double-talk -- the exact moment in question.

### A measurement error worth recording

The Stage 4 commit reported "mic peak while the agent speaks: 28-34, room floor,
against 25,102 uncancelled" as evidence the canceller worked. **That was the wrong
signal.** The `mic peak` line is computed at `audio_io.c:570`, in the downmix loop,
while `aec_process()` runs at `:604` -- so it has always shown the UNCANCELLED
microphone. The figures quoted were a quiet room during a quiet passage.

The empty-room result stands on its own and is not affected: one turn instead of
sixteen, three runs in a row, no self-triggering. The canceller does work. The
level figure printed next to it was measuring something else, and this project has
now made that class of error three times -- largest-block against total-free in
`a4fa137`, RMS against peak in the ERLE targets, and pre-AEC against post-AEC here.

### What ships

Half duplex, mic gate on, plus the touch-ring interrupt from Stage 1 -- which works
in every room, needs no canceller, and is what a device without echo cancellation
should have had from the start.

Kept in the tree, all default-off: the canceller (`CONFIG_AEC_ENABLE`), the bench
(`CONFIG_AEC_BENCH`), the heap probe (`CONFIG_HEAP_PROBE`) and the four-channel
reference lane, which is unconditional because it is the foundation any future
attempt needs. With the canceller off the branch costs ~1-3 fps against the
original 25 and IMPROVES largest free block at session start, 32,768 -> 59,392,
because of the PSRAM buffer move.

### The interrupt reopened the same window

Written 2026-08-26, on branch `fix-touch-interrupt`. The gate was on, full duplex
was abandoned, and `main` still hit this the same day:

```
E (45930) transport_ws: Error transport_poll_write(0)
E (45930) websocket_client: esp_transport_write() returned 0, transport_error=ESP_OK, ...
E (45931) dg_agent: transport error: esp_transport_write() returned 0, ...
```

**The touch-ring interrupt was defeating the gate it was supposed to make
unnecessary.** A tap flushed the ring and set the playback mute, and the mute path
in `audio_io_play()` deliberately does not stamp the queue clock -- so
`audio_io_playback_active()` fell within ~350 ms, the mic gate stopped gating, and
capture streamed 32 kB/s upstream straight through the rest of the reply, which
Deepgram was still sending. Section 1 above, exactly, just time-boxed to a turn.

Two things widened it. `end_interrupt()` hung off `AgentAudioDone`, which means
finished SENDING and sits up to 12.3 s of ring ahead of the speaker -- so a tap in
the tail, the case the feature exists for, arrived after its own clearing event and
latched the mute for the full 30 s deadline. And `UI_INTERRUPT` fired on any short
click outside the centre button, so stray touches triggered it.

The fix has two halves, and neither is the mic gate.

**The session no longer dies of congestion.** `components/tcp_transport/transport_ws.c`
LOCAL PATCH 2 drops a self-contained BINARY frame when `esp_transport_poll_write()`
times out, and reports it sent. The WebSocket client's send path treats `wlen == 0`
as a broken socket unconditionally, so a full send queue and a dead socket were the
same event to it. Only binary is droppable: TEXT carries `Settings`, and dropping
that silently hangs a session with no symptom. `poll_write < 0` stays fatal. The
poll runs on a 150 ms leash while the writes keep the caller's full deadline --
once the header is on the wire the frame is committed, and a timeout there tears it
in half.

**The keepalive no longer kills sessions either.** It is TEXT, so the patch above
cannot cover it; it blocked in `poll_write` holding the client lock, stalled the
capture task behind it, and timed out fatally. It now skips itself when audio went
upstream in the last 2 s -- congestion only happens while audio is flowing, which
is exactly when the keepalive is redundant.

### Dead ends, so nobody spends another day on them

- **`audio_io_uplink_blocked()`** -- a second predicate so the mic gate could follow
  the mute while the display kept an honest `audio_io_playback_active()`. Removed.
  The mute is released by the user SPEAKING AGAIN, so a gated mic cannot hear its
  own release. The mic now stays open through an interruption and full duplex costs
  dropped frames, not the session.
- **`AgentAudioDone` as end-of-turn** -- it does not arrive. Zero times across a
  12-minute run, once each in two earlier ones. `dg_agent` parses it correctly;
  Deepgram simply does not send it for most turns.
- **A gap in the inbound audio as end-of-turn** -- indistinguishable from a stall,
  and this link stalls for seconds. A 1.5 s quiet window released the mute mid-reply
  and the agent resumed talking.
- **`MUTE_MAX_MS` at 8 s** -- an absolute cap expires mid-reply. Back to 30 s, and
  now a pure backstop for a tap that is never followed by speech.
- **`CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK`** -- do not enable it. Its send-error
  path takes `client->lock` with `portMAX_DELAY` and wedges the client permanently.
  See the comment in `sdkconfig.defaults`.

**Still unexplained:** why the socket stalls at all. 32 kB/s to a cloud endpoint
should not saturate, `errno` is 0, RAM is ample, and Wi-Fi power save is already
`WIFI_PS_NONE`. Roughly 17% of mic frames drop. The patches above make that
survivable, not absent, and the drop counter now measures it. A related failure
survives at connect time: the 17.6 kB `Settings` TEXT frame failing with
`errno=119` (EINPROGRESS), which self-recovers by reconnecting.

**If anyone picks this up again**, the two walls above are the thing to attack, and
neither is an AEC problem: send less audio upstream, or find the residual-echo
margin somewhere other than the canceller. Raising `nlp_level` is the obvious idea
and it is worth 1.2 dB, which is not enough.

## Where the removed code lives

Everything below was built, measured and then taken back out, on the same
reasoning `12d285b` gave the first time: default-off scaffolding "made the tree
imply AEC was in play when it is not, and the next person to read `audio_io.c`
would have paid for that". The knowledge is in this document; the code is in git.

`git show <sha>` brings any of it back. All on branch `echo-cancellation`.

| artefact | what it was | commit |
|---|---|---|
| `main/aec_bench.{c,h}` | the deterministic bench: Espressif's far/near vectors through `aec_process()` per mode, ERLE with echo-only segmentation, heap deltas, NLP sweep | `4c4fd3d`, extended in `200ee86` |
| `main/audio_codecs.{c,h}` | ES7210 in 4-channel TDM so the echo-reference lane is powered and clocked | `dbd124c` (first proved in `9479446`) |
| `main/heap_probe.{c,h}` | allocation-failure hook + 50 ms heap sampler; what named the 1,630 B `INTERNAL\|DMA` failure | `7804177` |
| AEC in the capture path | `aec_process()` between capture and everything downstream, plus the convergence gate | `c477a1b` |
| linearity sweep | `CONFIG_AEC_SWEEP_VOLUME`, per-turn `LIN` lines, 128-lag correlation | `dbd124c` |
| double-talk log | `CONFIG_AEC_DOUBLETALK_LOG`, Part C — written, never produced data | `dbd124c` |
| `espressif/esp-sr` dependency | pins `esp-dsp` to exactly 1.8.0; nothing published is compatible with `^1.8.2` | `4c4fd3d` |
| SPIFFS test-vector image | `spiffs_create_partition_image(storage ...)` in the root `CMakeLists.txt`; vectors are downloads, never committed | `4c4fd3d` |

The test vectors themselves are not in the repo — they are ~1.7 MB each and
fetched from the URLs in Sources. Three of the five fit the 7 MB `storage`
partition; four do not (93% and spiffsgen refuses it).

### What was kept, and why

Not everything from this branch was scaffolding. Merged into the device because it
stands on its own:

- **The touch-ring interrupt** (`c477a1b`'s ancestor `148f480`). Works in any room,
  needs no canceller, and is what a device without echo cancellation should have.
- **Two real bugs**: `s_last_play_us` written by two tasks as a 64-bit value, the
  same fault `25e48d4` fixed for `s_speech_us`; and the producer-side carry
  surviving a stream discontinuity, which offset every sample after an
  auto-reconnect until a deliberate session stop.
- **The capture and playback `stereo` buffers in PSRAM.** They never needed to be
  internal — the codec only copies through them. Measured on the cleaned tree:
  free internal at session start 74,775 -> 83,003 and largest block
  32,768 -> 69,632, against the pre-branch build.
- **This document.**

### If you pick this up again

Start from "Why full duplex was abandoned". The canceller is not the problem and
re-deriving that will cost you a day. The two walls are bandwidth and residual
echo, and neither is fixed by tuning the AEC.


## Sources

Read, and load-bearing for the numbers above:

- [ESP-SR — Acoustic Echo Cancellation, ESP32-S3][s3] — the resource table, the
  mode list, and the self-contradictory test-setting line.
- [ESP-SR — Acoustic Echo Cancellation, ESP32-P4][p4] — the cross-check: same RAM
  figures for SR and VOIP, different CPU and FD figures, test setting stated as
  400 MHz.
- [`esp-sr/include/esp32s3/esp_aec.h`][hdr] — the standalone API, `aec_config_t`
  including `caps`, the `AEC_MODE_*` enum, and the planar channel layout.
- [`speexdsp/libspeexdsp/mdf.c`][mdf] — the allocation list the 115.8 kB is
  computed from.
- [Speex manual — programming with libspeex][speex] — frame size and tail length
  guidance.
- [Deepgram — Audio Preprocessing & Barge-In][dg] — why cancellation is the
  device's problem and not the server's. Already cited in the README.

Leads, not read in depth:

- [ESP-SR — Model Selection and Loading, ESP32-S3][models] — for the model
  partition question.
- [espressif/esp-sr][repo] and its [v2.1.1 changelog][changelog] — for the
  version actually pulled and the `esp-dsp` pin.
- [espressif/esp-box#185][box] — a comparable S3 board asking for an AEC example.
- [`speexdsp/include/speex/speex_echo.h`][speexh]
- [rjsachse/ESP32-SpeexDSP][speexesp]

The test vectors, from the page's own `Test Audio Resources` section, base URL
`https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/_downloads/<hash>/<file>`:

| file | role | hash |
|---|---|---|
| `aec_in_far.wav` | far end, the playback reference | `5f945a2ceda76a8fc345d85261eeca63` |
| `aec_in_near.wav` | near end, microphone with echo | `f5edb84d0389c362d01509694da332f2` |
| `aec_test_fd.wav` | FD expected output | `89b30961e2fc8073504a6002059ede11` |
| `aec_test_sr.wav` | SR expected output | `6996684bc7bf82f827f19815b51bf6a6` |
| `aec_test_voip.wav` | VOIP expected output | `3e51d2f8ce5c409d19ccd63def1718dd` |

All mono, 16 kHz, 16-bit, 53.2 s — verified by parsing the headers on device,
because the page states none of it.

Measured on this board by `main/aec_bench.c` on 2026-08-25 -- since removed, see
"Where the removed code lives" -- against those vectors: the per-mode table, the NLP probe, `aec_get_chunksize()` = 512, and the
refutation of the published LOW_COST internal-RAM figures. The bench is
default-off behind `CONFIG_AEC_BENCH`.

From the shipped header rather than the page —
`managed_components/espressif__esp-sr/include/esp32s3/esp_aec.h`: `aec_process()`
alongside `aec_linear_process()` and `aec_nlp_process()` (the page mentions only
the first); the warning that buffers must come from `heap_caps_aligned_alloc()`;
`aec_handle_t` exposing `frame_size` and a copy of the config; and the
`aec_create()` doc comment recommending `AEC_MODE_SR_LOW_COST`, which contradicts
the page's own recommendation of `AEC_MODE_FD_LOW_COST` and is contradicted in
turn by measurement.

In this repo, cited by commit: `a4fa137` (the AFE measurement and its numbers),
`9479446` (4-channel TDM bring-up, the per-lane peak table, the two I2S facts),
`12d285b` (why the scaffolding was removed rather than left default-off).

Device figures (66,219 / 45,056 / 83,207 / 5,986,027) are from a clean-boot
serial capture of the current build, taken 2026-08-25. **No AEC measurement on
this device is reported anywhere in this document, because none has been made.**

[s3]: https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/acoustic_echo_cancellation/README.html
[p4]: https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/acoustic_echo_cancellation/README.html
[hdr]: https://github.com/espressif/esp-sr/blob/master/include/esp32s3/esp_aec.h
[mdf]: https://github.com/xiph/speexdsp/blob/master/libspeexdsp/mdf.c
[speex]: https://www.speex.org/docs/manual/speex-manual/node7.html
[speexh]: https://github.com/xiph/speexdsp/blob/master/include/speex/speex_echo.h
[speexesp]: https://github.com/rjsachse/ESP32-SpeexDSP
[dg]: https://developers.deepgram.com/guides/deep-dives/audio-preprocessing-barge-in
[models]: https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/flash_model/README.html
[repo]: https://github.com/espressif/esp-sr
[changelog]: https://components.espressif.com/components/espressif/esp-sr/versions/2.1.1/changelog
[box]: https://github.com/espressif/esp-box/issues/185
