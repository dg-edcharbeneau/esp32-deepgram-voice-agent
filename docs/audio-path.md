# How the audio path works

Mic to Deepgram to speaker: the tasks, the ring buffer, the echo gate, and the
BSP init-order trap.

## How the audio path works

**One module owns both codecs.** `bsp_audio_init()` configures a single duplex
I2S channel pair, and `esp_codec_dev_open()` drives that shared clock — so the
mic and speaker cannot run at different sample rates or channel counts, and
opening one reconfigures the other. `spec_analyzer_radial`'s `bsp_extra` makes
the same call: its `bsp_extra_codec_set_fs()` closes and reopens *both* codecs
together with one `sample_info`. Split across two modules, whichever opened last
would silently redefine the other.

**That forces one sample rate.** The Agent API is happy to send 24 kHz out while
taking 16 kHz in; this hardware is not. Both directions run at
`DG_AUDIO_SAMPLE_RATE` = 16 kHz, which is what `spec_analyzer_radial` proved on
this board. One constant in [dg_agent.h](../main/dg_agent.h) drives the `Settings`
message and both codecs.

**Channels are converted at the edges.** The codecs open with 2 channels,
matching the proven configuration; Deepgram is mono both ways. Playback
duplicates each mono sample into L+R, capture averages L+R back down to one.

With `CONFIG_AEC_ENABLE` the *width* changes but the channel count does not: the
ES7210 emits a 4x16-bit TDM frame and the S3 reads it as 2 channels x 32 bits,
because RX and TX share BCLK/WS in full duplex and cannot be configured
separately. Each 32-bit word carries two 16-bit lanes, ordered
`[ref, mic, dead, mic]`, and capture averages the two microphone lanes while the
reference feeds the canceller. Playback left-justifies each mono sample into the
32-bit slot. See [notes/echo-cancellation.md](notes/echo-cancellation.md).

**Sample alignment is enforced at both ends.** This one caused intermittent
loud static, so it is worth understanding. PCM here is 16-bit, but nothing
upstream respects sample boundaries — the WebSocket transport hands over
whatever a TLS record happened to hold, and a stream buffer splits at any byte,
so odd-length chunks are routine. Truncating an odd count to whole samples
throws one byte away and shifts every following sample by 8 bits: not a click,
but full-scale noise that persists until the stream happens to realign. The same
slip occurs if a full buffer accepts an odd-length short write.

So both ends keep the orphaned byte and stitch it onto the next chunk, and drops
are rounded to whole samples — a full buffer now costs a click instead of a
burst of static. Verified on the host against random odd-length splits: byte-exact
with a roomy buffer, and with a deliberately tiny one forcing 46% drops, every
surviving sample is still correctly phased.

**Playback never blocks the socket.** `on_audio()` runs on the WebSocket task,
where a stall stops the whole session, so `audio_io_play()` drops PCM into a
384 kB PSRAM stream buffer with a zero timeout and returns; a task on core 1
drains it, doubles it to stereo, and writes to the codec. The buffer holds mono,
so it covers 12.3 s — Deepgram sends a turn faster than it plays, and the buffer
absorbs the difference. Drops are counted rather than ignored, because the
symptom is a gap you can hear.

Both codecs are opened once and left open; reopening per turn clicks.

**Flushing is done by the reader.** `xStreamBufferReset()` fails outright while a
task is blocked reading — which the drain task normally is — so calling it from
`audio_io_flush()` silently did nothing. Instead the flush sets a flag and the
drain task, which waits in 50 ms slices rather than forever, discards the queued
audio and its carry byte itself. Dropping the carry matters: keeping it would
misalign the first sample of the next reply.

### Echo, and why the microphone can stay open

The speaker and mic sit centimetres apart, so without cancellation anything the
agent says is captured and sent straight back — and the agent starts answering
itself. **`CONFIG_AEC_ENABLE` is on by default** and is what stops that: esp-sr's
standalone AEC sits in the capture path, and the ES7210 runs in 4-channel TDM so
the hardware echo-reference lane is powered and sample-aligned with the
microphones.

With it, `CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS` is off and the microphone stays
open while the agent talks. You can cut in mid-sentence; `UserStartedSpeaking`
fires during playback and the reply stops.

**Turning `CONFIG_AEC_ENABLE` off returns the device to half duplex**, where
capture is dropped for the length of playback plus a 300 ms tail for audio
already in the I2S DMA. That is the crude fix, and it costs barge-in entirely:
you cannot interrupt by voice, only by tapping. It is the right setting for
hardware where cancellation cannot reach the ERLE this board reaches — see
[notes/echo-cancellation.md](notes/echo-cancellation.md).

**The real answer is not server-side, and an earlier version of this file was
wrong to say it was.** Deepgram's
[Audio Preprocessing & Barge-In](https://developers.deepgram.com/guides/deep-dives/audio-preprocessing-barge-in)
guide has no AEC setting and explicitly pushes it to the device: platform-level
cancellation has direct access to both the microphone and the speaker output, so
time alignment is handled for it. For hardware with no browser it names exactly
this situation — a separate mic stream and speaker reference stream — and treats
server-side AEC as an advanced case needing zero clock skew.

**Waveshare's "hardware echo cancellation" is also wrong.** The ES7210 datasheet
titles it "High Performance Four Channels Audio ADC"; there is no echo canceller
in it, nor in the ES8311, nor in the NS4150B amplifier.

**What the board really gives you is an echo *reference*** — which is the part
that is genuinely hard to retrofit. The schematic netlist shows an `AEC ADC`
block tapping the ES8311's outputs through a differential RC network into ES7210
MIC3P/MIC3N, every component populated. The reference is captured by the same
ADC, on the same clock, in the same frame as the mics, so it is sample-aligned by
construction.

Nothing enables it by default: the BSP builds the ES7210 with `mic_selected` at
0, so the driver falls back to MIC1|MIC2 and MIC3 is never powered. And SDOUT2 —
the non-TDM route to MIC3 — is cut on this board (R48 is NC), so **4-channel TDM
is the only way it reaches the S3.**

Commit `9479446` proves it — the code is not in the tree any more, but
`git show 9479446` brings back the 4-channel TDM bring-up that produced these
numbers. With all four inputs selected and the frame read as 2 ch x 32-bit
standard I2S, measured peaks per lane:

| lane | idle | playback | what it is |
|---|---|---|---|
| 0 | 2–3 | 7867–9359 | **the echo reference** — note the absent noise floor |
| 1 | 30–115 | 537–11439 | a MEMS mic |
| 2 | 3 | 3–9 | MIC4, AC-coupled to AGND, dead always |
| 3 | 30–115 | 520–12353 | the other MEMS mic |

Lane 2 being permanently dead is the control that identifies the ordering rather
than guessing it — and the slot order is *not* MIC1/2/3/4, because a 32-bit word
arrives MSB-first and stores little-endian so each pair swaps in memory.

**Cancellation is where it stops, and it stops on RAM.** `espressif/esp-sr`'s
AFE does the job on paper — `input_format = "MMNR"`, `aec_init = true`, 16-bit
16 kHz, already this project's format — and it initialises without complaint.
What it needs is about **70 kB of internal RAM**, against the ~78 kB free once
the display is up. Enabling it did not degrade the session; it stopped the
session from happening at all, because mbedTLS could no longer get a buffer for
the TLS handshake:

```
esp_transport_write() returned 0
```

The whole experiment is in `a4fa137` with the measurements. Internal RAM is
the binding constraint on this board — see [Threading and
memory](#threading-and-memory) — and a 70 kB canceller
does not fit next to a 466x466 display and a TLS socket. So the gate stays on,
and the AEC scaffolding was removed rather than left in the tree implying a
capability the device does not have.

It was long assumed here that if barge-in were revisited, the cheap route would be
a **double-talk detector** rather than full cancellation: learn the room's
coupling ratio while only the agent speaks, then flag any mic level above that
ratio as a second voice, needing no adaptive filter and no 70 kB.

**That is now disproven, with numbers.** Measured on 2026-08-25 with the gate off
**at `CONFIG_AUDIO_OUT_VOLUME=100`** — the volume matters and was not recorded at
the time — the signal-to-echo ratio at the microphone is **-11 dB**, i.e. the
person is four times quieter than the device's own voice. Because the microphone sums power, a talker
at the echo's own level raises the mic by 3.0 dB and a typical talker by **0.33 dB**,
which no threshold can separate from noise. The reference lane does not rescue it:
`9479446`'s own numbers put the lane *quieter* than the microphones, an echo return
loss of about -2.4 dB.

Cancellation is the only acoustic route, and it was built and measured: esp-sr's
standalone AEC in `AEC_MODE_FD_LOW_COST` achieves **17.3 dB of ERLE** on
Espressif's own test vectors against their own output's 18.3, and with it running
the device stops answering itself in an empty room -- one turn where before there
were sixteen in twenty-four seconds.

**It did not give barge-in when first tried, and both reasons have since been
resolved.** They were: streaming the microphone through the agent's reply
saturates the TCP send queue until a TLS allocation fails and the session drops,
and even while that audio was reaching Deepgram it never distinguished a person
talking over the agent from the residual echo.

The second was a volume artifact — every measurement above was taken at volume
100. The first was bandwidth rather than cancellation, and is fixed by sending
less: `CONFIG_AEC_UPLINK_VAD` forwards a block during playback only when the
**post-AEC** level clears a threshold, so residual echo costs no uplink and a
real interruption still gets through.

**Full duplex works and is the shipping default**, measured across volumes 70
and 100. The **tap on the centre button** stays regardless: it works in any room,
needs no canceller, and remains the interrupt for a half-duplex build.

The whole investigation, including the measurement errors made along the way, is
in [notes/echo-cancellation.md](notes/echo-cancellation.md).

**Update, 2026-08-25: the 70 kB was the AFE, not the canceller.** esp-sr also
ships a standalone AEC — `aec_create_from_config()` / `aec_process()`, no ring
buffers and no model stages — published at **8.2–26.9 kB of internal RAM**
depending on mode, against 66,219 B free with a live session. That is a different
answer to the question `a4fa137` asked. Nothing has been measured on the device;
the price list, the budget, the two remaining candidates and the open risks are
in [docs/notes/echo-cancellation.md](notes/echo-cancellation.md).

## Noise suppression

**On by default**, using esp-sr's standalone `ns_pro`. `CONFIG_MIC_NS_ENABLE`
compiles it in; `CONFIG_MIC_NS_DEFAULT_ON` is the power-on state, and a double
click on the BOOT button overrides that at runtime.

The canceller above removes the device's *own speaker*. It does nothing about the
room — fans, HVAC, traffic — which otherwise goes up the wire to Deepgram at full
level. A denoiser is a different stage solving a different problem.

Order in `capture_task`, and every position in it was argued for:

```
downmix → AEC → uplink VAD verdict → NS → level log → session gate → tap → sink
```

- **After the AEC**, because a non-linear denoiser upstream of an adaptive filter
  wrecks its convergence — the filter has to see the same raw microphone the
  reference lane is echoing into.
- **Below the uplink VAD.** The first version had it above, reasoning that a
  cleaner floor would make the threshold easier to set. That was backwards and
  it was measured; see below.
- **Ahead of the tap and the sink**, so the orb draws the room the way Deepgram
  hears it.

### What it costs

| | |
|---|---|
| frame rate | **about −14%** (17.4 → 14.9; see below) |
| internal RAM | **−7.4 kB**, held whether or not it is running |
| uplink latency | **+64 ms**, from the frame-size FIFO |
| RX overruns | unchanged |

The RAM is spent at init either way, so the button only recovers the CPU.

**The honest caveat, kept deliberately:** in the room this was tested in the
denoiser did *not* improve transcription — the no-NS baseline already scored 4/4.
It is here for louder environments than that one, and the measurements that say
so are below rather than quietly dropped.


### Why esp-sr, and why not SpeexDSP

`CONFIG_MIC_NS_ENABLE` uses esp-sr's standalone `ns_pro` — already linked, no
new dependency. Its one awkwardness is frame size: `ns_pro_create` takes 10 ms
frames only (160 samples) and `AUDIO_IO_CAPTURE_FRAMES` is 1024 under the AEC,
so `1024 % 160 = 64` and it cannot walk the block in place the way
`aec_process` does. It therefore carries an input and an output FIFO, costing
7,104 B of internal staging and a standing **64 ms** of added uplink latency.

**SpeexDSP was evaluated and removed.** It was vendored from
[rjsachse/ESP32-SpeexDSP](https://github.com/rjsachse/ESP32-SpeexDSP) — itself
an Arduino wrapper over stock upstream SpeexDSP C — specifically because Speex
accepts any frame size, so a 256-sample frame divided both 1024 and 1280 exactly
and needed no FIFO at all. That advantage was real and it was not enough:

- **57% of the frame rate**, against esp-sr's 26%. Its state lived in PSRAM
  (mandatory, see below), so the FFT missed cache on every pass.
- **It damaged speech.** It ate quiet word onsets — "A quick brown fox" for
  "The quick brown fox", "Seashells seashells" for "She sells seashells" — and
  lost an utterance outright, where the no-NS baseline transcribed 4/4.
- **Its state could not be moved out of PSRAM.** Doing so to buy the frame rate
  back dropped `intmax` to 10,240 and the device then could not open a session
  at all: TLS write failed on every retry and it sat in CONNECTING forever. That
  is the same internal-RAM starvation that killed the earlier full-duplex
  attempt, and it is also why the upstream library ships `USE_PSRAM` — on a
  board like this it is load-bearing, not a convenience.

The numbers below are kept so this is not re-proposed.

### Turning it on and off from the button

`CONFIG_MIC_NS_ENABLE` compiles the denoiser in; **a double click on the BOOT
button turns it on and off at runtime**, and the choice is saved to NVS so it
survives a reboot. `CONFIG_MIC_NS_DEFAULT_ON` is only the power-on state before
anyone has touched it. The status caption confirms each press, and the `mic peak`
line reports `ns=off` rather than `ns=0` when the stage is idle — zero would read
as total suppression, which is the opposite of the truth.

Three consequences worth knowing:

- **Switching it on injects one silent block.** The FIFO is reset on the off→on
  edge so audio from before the pause cannot be spliced onto audio from after
  it, and the first block out of a reset FIFO is the priming block, which is
  silence. That is 64 ms, and it goes up the wire mid-sentence if you toggle
  while talking. Switching it *off* costs nothing.


- **The RAM is spent either way.** The engine and its 7,104 B of staging are
  allocated at init and held whether or not it is running. Deferring that to the
  first press would be worse: a multi-kB internal allocation made mid-session on
  a fragmented heap is exactly what drops sessions on this board. What the toggle
  buys back is the CPU — roughly 5 fps — not the memory.
- **The gesture is compiled out with the feature**, not left present replying
  "not built". Registering a double click makes `iot_button` withhold every
  single click until the double-click window closes, and the single click is the
  session toggle — the documented escape hatch. A build without the denoiser does
  not pay a slower escape hatch for a feature it does not have.

### What was measured

All three arms on 2026-09-07: cold boot, live session, 150 s each, ~129
steady-state telemetry samples, same face, same room, identical bench config
(`CONFIG_AUDIO_CAPTURE_ALWAYS=y`, `CONFIG_SESSION_IDLE_TIMEOUT_S=0`).

| arm | fps | Δfps | `int` mean | Δ internal | `intmax` floor | rx overruns |
|---|---|---|---|---|---|---|
| NS off | 20.00 | — | 60,331 | — | 26,624 | +0 |
| Speex (PSRAM) | 8.63 | **−11.37 (−57%)** | 60,903 | +572 | 29,696 | +0 |
| esp-sr | 14.80 | **−5.20 (−26%)** | 52,894 | **−7,438** | 21,504 | +0 |

Flash: baseline `0x1a6880`, Speex `0x1ab320` (+18,592 B), esp-sr `0x1ab050`
(+17,872 B). With `CONFIG_MIC_NS_ENABLE=n` the binary is what it was before any
of this existed.

**The engines do not rank, they trade.** esp-sr is 2.2× cheaper on CPU and costs
7.4 kB of internal RAM — the resource this board has least of, and the one whose
exhaustion drops sessions. Speex costs essentially no internal RAM and more than
half the frame rate. Neither touched the I2S ring: `rxovf` did not move on any
arm, so the capture task is not running long enough to starve DMA in either case.

The esp-sr internal cost (7,438 B measured) matches its predicted staging
(7,104 B) almost exactly, which is the corroboration that the FIFOs are what it
is paying for.

### The acoustic result

Measured 2026-09-11: one speaker, four fixed lines, same room and noise for all
three arms, plus an attempt to talk over the agent each time.

| arm | transcription |
|---|---|
| NS off | **4/4 lines correct** |
| Speex | dropped word **onsets** — "A quick brown fox" for "The quick brown fox", "Seashells seashells" for "She sells seashells" — and lost one utterance entirely |
| esp-sr | 3/3 correct within the capture window |

**Neither denoiser improved transcription in THIS room, and Speex made it
worse.** At this noise level Deepgram already handles the audio unaided, so the
denoiser spent frame rate and bought nothing. The scope was always the caveat:
it is a statement about a quiet room, and the loud room is the condition NS
exists for. That one has since been tried — see the field report below.

ERLE was unaffected, as predicted — NS sits downstream of the canceller.
`L=7438 → out=63` with the agent speaking, comparable across arms.

### Field report: the loud room, 2026-09-19

**Uninstrumented, and flagged as such.** Both arms — `CONFIG_MIC_NS_ENABLE` off
and on at mode 2 — were run by hand in one genuinely loud room, in one sitting,
by the person holding the board. **With the denoiser on the device worked well**;
that is the first evidence in NS's favour this project has, and it is evidence
for exactly the condition the feature was switched on for.

What it is not: a capture. No serial log was taken, so there are no per-arm
transcripts, no `fps`, no `vadsup`, no `rxovf`, no `lowdma`, and no `mic peak`
readings to confirm the two arms met the same ambient level. It is one
experienced pair of ears comparing two builds back to back, which is worth more
than nothing and less than the table above it.

So the quiet-room result stands as measured, this stands as reported, and the
two should not be read off the same axis. The instrumented version of this run
is still the open item; what has changed is the expected sign of its result.

### Why NS goes below the uplink VAD

The first version placed the denoiser **above** the VAD, justified as "the
threshold is measured against a floor that has had the room taken out of it."
**That reasoning was backwards.** The VAD compares its input peak against
`CONFIG_AEC_UPLINK_VAD_PEAK`, and a denoiser upstream of it attenuates the
*interrupting speech* along with the room — so fewer blocks clear the threshold
and the interruption never reaches Deepgram.

Blocks discarded by the VAD during playback, comparable runs:

| NS off | NS above the VAD | NS below the VAD |
|---|---|---|
| 22 | **49** | **16** |

The middle column was perceptible — interrupting the agent was reported as hard
to trigger. **The stage now runs below the VAD**, so the VAD decides on the
post-AEC signal exactly as it did before any of this existed and its threshold
keeps its original meaning. The denoiser still sits ahead of the tap and the
sink, so the orb and Deepgram both get cleaned audio.

Order in `capture_task` is now: downmix → AEC → uplink VAD verdict → **NS** →
level log → session gate → tap → sink.

### The 64 ms, and where it comes from

An earlier version of this document claimed the esp-sr FIFO cost "up to 10 ms"
of added latency. That was wrong: 10 ms is only the *input* residue. The output
FIFO additionally holds a full `CAPTURE_FRAMES` between drains — measured at
exactly 1024 samples of standing occupancy in steady state, i.e. **64 ms**, and
it lands directly on the interruption path.

This also explains the `ns=` values in the esp-sr logs that look impossible
(`out=516 ns=16114`): `out=` is the current block and `ns=` is the FIFO's
output, which is a block or two behind it. The two are not measuring the same
moment and should not be compared within a line on that arm.

### What is still open

The feature ships; these are the threads left, in the order I would pull them.

- **A genuinely noisy room, instrumented.** Tried by hand on 2026-09-19 and the
  denoiser helped (see the field report above), which is the first result that
  favours it — but no capture was taken, so there are no per-arm numbers and no
  proof the two arms saw the same ambient. Re-running it with a log is now about
  confirming a positive rather than hunting for one, which makes it cheaper to
  justify and no less worth doing.
- **`CONFIG_MIC_NS_ESPSR_MODE` has only been run at its default, 2 (aggressive).**
  Start milder. Eating quiet word onsets is what disqualified SpeexDSP, it is a
  property of aggressive suppression rather than of that one library, and one
  esp-sr run hinted at it ("A quick brown fox **jumped**"). A single utterance,
  so not conclusive — which is exactly why it is worth a deliberate run.
- **`CONFIG_AEC_UPLINK_VAD_PEAK` has not been re-tuned for a denoised signal.**
  Moving NS below the VAD restored barge-in to baseline, so nothing is broken,
  but the threshold is still the one chosen for the undenoised floor.
- **Long-run heap behaviour, under real conversation.** A 35-minute run is
  recorded below and found no drift at all — but nobody spoke during it, so the
  allocation churn that actually fragments this heap never happened. That is the
  part still untested.

### Long run, 2026-09-21: the heap does not drift, but nobody spoke

35 minutes (2,099 s), 2,093 telemetry samples, 2,091 with a live session, NS
running at mode 2, `CONFIG_HEAP_PROBE=y`, idle timeout pinned off.
**Zero crashes, zero reboots.**

| | 1 Hz observed min | 50 ms true floor |
|---|---|---|
| internal free | 39,551 | **22,163** |
| largest DMA block | 19,456 | **8,704** |

The finding is *when* that floor was reached: **`lowdma` hit 8,704 at t = 4.2 s
and did not move again for the remaining 35 minutes.** `dmamax` sat pinned at
19,456 the whole time, `rxovf` never left 120, and fps held 10.4–10.6 from the
five-minute mark onward. The `heap_probe` boot lines show the whole collapse
happening inside the first two seconds, as Wi-Fi, TLS and the display come up.

So the risk window is **startup, not longevity** — at least for this workload.
That is the opposite of what the open item assumed, and it is worth knowing
before anyone budgets for a slow leak that does not appear to exist.

**The caveat is the whole caveat, so it goes in bold: nobody spoke.**
`turns=0`, `played=0`, `rx=0`, no playback blocks. The device streamed 67 MB of
microphone audio upstream and was otherwise idle. What this run establishes is
that the *uplink path* — capture → AEC → NS → WebSocket — neither leaks nor
fragments over 35 minutes. What it does not touch is the churn a conversation
brings: TTS playback buffers, history writes to flash, face changes, reconnects.
The 6,144 B previously recorded over ~22 minutes almost certainly came from that
kind of use, and nothing here contradicts it.

> **This run's fps is an outlier and should not be quoted.** It averaged 10.56,
> and three later replicates of a byte-identical configuration read 15.00, 14.96
> and 14.81 — a spread of 0.19 fps. So the 10.56 is ~30% below where that build
> actually sits, and it is not normal variance. See the fps section below; note
> also that this run is the only one in the whole exercise that logged transport
> congestion (`txdrop=46`, `updrop=73`, against zero in every replicate), and
> that the board was power-cycled between it and the replicates. The heap
> findings above stand, but they were taken on an atypically slow run.

### What the denoiser actually costs in frame rate

Measured 2026-09-21, five arms in one sitting, 300 s each, ~279 steady-state
samples per arm, same face, session pinned open. Three of them are replicates of
one configuration, which is what makes the spread readable.

| arm | fps mean | min | draw avg | amp |
|---|---|---|---|---|
| NS off, probe off | **17.74** | 16.1 | 17.5 ms | 0.015 |
| NS on, probe off | **14.87** | 13.6 | 21.2 ms | 0.005 |
| NS on, probe on | 15.00 | 13.5 | 21.3 ms | 0.005 |
| NS on, probe on (replicate) | 14.96 | 13.6 | 21.4 ms | 0.005 |
| NS on, probe on (replicate) | 14.81 | 13.5 | 21.4 ms | 0.005 |

**Here the denoiser costs 2.87 fps, or −16.2%. The 2026-09-07 table above says
−5.20 fps, or −26%, and both were measured in one sitting** — so this is two
honest measurements of the same quantity disagreeing, not an artefact of
comparing across runs. (An earlier draft of this section claimed the older
numbers came from separate sittings. They did not; that was wrong.)

What reproduces and what does not is the interesting part:

| | 2026-09-07 | 2026-09-21 |
|---|---|---|
| NS on | 14.80 | 14.87 (and 15.00 / 14.96 / 14.81) |
| NS off | **20.00** | **17.74** |

**The NS-on figure is stable to within 0.2 fps across two weeks. The baseline
moved by 2.26.**

`CONFIG_AUDIO_CAPTURE_ALWAYS` was the leading suspect — the September bench set
it and the later one did not — and **it is ruled out in both rows of a full
2×2.** Measured 2026-09-22, ~280 samples per arm:

| | `CAPTURE_ALWAYS=n` | `CAPTURE_ALWAYS=y` | flag effect |
|---|---|---|---|
| **NS off** | 17.37 | 17.24 | −0.13 |
| **NS on** | 14.91 | 14.89 | −0.02 |

Both differences sit inside the ±0.6% replicate spread. The flag does nothing.
NS on reproduces the 14.81–15.00 measured the day before, and 14.80 measured
two weeks before that.

> **A note on the fourth arm, because it fails a rule stated elsewhere in this
> document.** Its raw ambient (`mic peak L` mean 46.7) ran 38% above the other
> three arms (32.6 / 34.6 / 34.1), which the ±20% rule in the bench notes would
> reject. It is kept, on the mechanism rather than on convenience: what drives
> the orb's pixel work — and therefore fps — is the **post-NS** level the
> visualiser receives, and that was 0.0052 against arm 3's 0.0071. Arm 4 drew
> *less*, so if ambient had biased it, it would have biased it **faster**, and it
> read 0.02 fps slower. The ±20% rule is about raw level because raw level is
> what the transcription and suppression-depth comparisons turn on; for fps the
> denoiser decouples the two.
>
> That decoupling is itself worth recording: **with NS on, frame rate is
> insensitive to room noise.** Raw ambient rose 38% and post-NS `amp` did not
> follow, so the display cost stayed flat. Nobody was looking for that, and it is
> a genuine second-order benefit of the denoiser.

So the shift is not a bench flag, and that makes it harder to explain rather
than easier: **none of the code that changed between the two sittings touches
the NS-off path.** The VAD reorder is inside `#if CONFIG_MIC_NS_ENABLE`, which
is compiled out in these arms; `rxovf` and `lowint`/`lowdma` are three calls a
second; `ui_flash_status` short-circuits on a NULL check.

One reason to doubt the older figure rather than the newer ones: **September ran
at `amp` ≈ 0.028 against 0.013 here.** Higher amplitude is *more* pixel work for
the orb, so that run should have been slower, and it read 20.00. That inversion
is unexplained, and three tightly-clustered recent replicates are better
evidence than one older reading.

Working position: **the denoiser costs about −14%**, from four arms across two
days (17.37 → 14.91 and 17.24 → 14.89, with 17.74 → 14.87 the day before). The
**−26% derives from a baseline that has now failed to reproduce three times**
while the NS-on figure reproduced every time. The range stays in the older
tables until someone reproduces 20.00 or finds what changed, but −14% is the
number to plan with.

Note the confound runs *against* the finding, as it did in the acoustic work:
`amp` falls 0.015 → 0.005 because the denoiser quiets what the orb draws, which
should make drawing cheaper. Draw time rose anyway, 17.5 → 21.2 ms. The CPU cost
is real and if anything understated here.

**`CONFIG_HEAP_PROBE` is free.** 14.87 → 15.00, a +0.9% difference sitting inside
a ±0.6% replicate spread. The 50 ms sampler at priority 1 on core 0 does not
measurably cost frame rate, which was a live hypothesis and is now dead.

**fps is reproducible to ±0.6% within a sitting** (15.00 / 14.96 / 14.81), so
these numbers are solid — as long as nothing is compared across sittings. That
makes the 10.56 from the 35-minute run genuinely anomalous rather than noisy,
and it remains unexplained. The two things that distinguish it: it is the only
run that logged transport congestion, and the board was power-cycled between it
and the replicates. If a ~30% frame-rate deficit that survives a reset but
clears on a power cycle is real, it matters more than the denoiser does — worth
watching for, not yet worth a theory.

### Bench notes worth keeping

- **Read `lowdma=`, not `dmamax=`.** The TLM line samples at 1 Hz and cannot see
  a collapse between samples; `heap_probe`'s 50 ms sampler can, and `lowint=` /
  `lowdma=` are its floors since boot. Measured on two consecutive 60 s and 20 s
  boots: `dmamax` read 19,456 and 20,480 live while the floors were **11,776**
  and **6,656**. Quoting the live figure overstates headroom by 2-3x. `-1` means
  `CONFIG_HEAP_PROBE` is off, not a floor of zero.

- `CONFIG_AUDIO_CAPTURE_ALWAYS=y` plus `CONFIG_SESSION_IDLE_TIMEOUT_S=0` is the
  harness: the capture task never parks, so the NS stage runs on every block with
  no voice needed, and the session holds open so the heap carries real TLS
  pressure. A boot-fresh idle heap is far more generous and not comparable to
  the AEC table above.
- **A tap on the screen toggles the session off** and silently ends a run.
- **One reader per port.** A serial reader still holding the port makes
  `idf.py flash` fail with `The chip stopped responding`, which reads like a dead
  board and is not one.
- The board does not keep one device node — it has enumerated as both
  `usbmodem1101` and `usbmodem101`. Glob for it.
- Reset and read over a **single** handle; a separate reset tool plus a `cat`
  reader splits the byte stream and produces logs that look like stale builds.

### The BSP init-order trap

`bsp_audio_codec_speaker_init()` and its microphone twin call `bsp_i2c_init()`
only inside an `if (i2s_data_if == NULL)` guard — that is, only when they are
also doing the I2S setup. Calling `bsp_audio_init()` yourself first makes that
guard false, so I2C never comes up and the codec control interface is built on a
NULL bus handle:

```
assert failed: bsp_audio_codec_speaker_init esp32_s3_touch_amoled_1_75c.c:225 (i2c_ctrl_if)
```

So `audio_io_init()` does **not** call `bsp_audio_init()`. It does not need to:
`esp_codec_dev_open()` sets the clock anyway, overriding the BSP's 22050 Hz
default. This is the order `bsp_extra` uses.

---

[Back to the README](../README.md)
