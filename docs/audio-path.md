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

**Off by default, and not yet measured on the device.** Two engines are built
behind `CONFIG_MIC_NS_ENABLE` so they can be compared on the same board in the
same room; the loser gets deleted.

The canceller above removes the device's *own speaker*. It does nothing about the
room — fans, HVAC, traffic — which until now went up the wire to Deepgram at full
level. A denoiser is a different stage solving a different problem, and it sits
**after** the AEC and **before** the tap, the uplink VAD and the sink:

- after the AEC, because a non-linear denoiser upstream of an adaptive filter
  wrecks its convergence — the filter has to see the same raw microphone the
  reference lane is echoing into;
- before the tap, so the orb draws the room the way Deepgram hears it;
- before the VAD, so `CONFIG_AEC_UPLINK_VAD_PEAK` is measured against a floor
  that has had the room taken out of it.

### One engine, and a removed one

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

### The acoustic result — neither engine helped

Measured 2026-09-11: one speaker, four fixed lines, same room and noise for all
three arms, plus an attempt to talk over the agent each time.

| arm | transcription |
|---|---|
| NS off | **4/4 lines correct** |
| Speex | dropped word **onsets** — "A quick brown fox" for "The quick brown fox", "Seashells seashells" for "She sells seashells" — and lost one utterance entirely |
| esp-sr | 3/3 correct within the capture window |

**Neither denoiser improved transcription, and Speex made it worse.** At this
room's noise level Deepgram already handles the audio unaided, so the denoiser
spends frame rate and buys nothing. Note the scope: this is a statement about
*this* noise level. A genuinely loud environment has not been tested, and that
is the only condition under which NS is likely to pay.

ERLE was unaffected, as predicted — NS sits downstream of the canceller.
`L=7438 → out=63` with the agent speaking, comparable across arms.

### NS goes below the uplink VAD

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

### Correction: esp-sr adds 64 ms, not 10 ms

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

- A genuinely noisy room. Everything above says NS does not pay at this noise
  level; it does not say it never pays.
- `CONFIG_MIC_NS_ESPSR_MODE` was only tried at its default (2, aggressive).
  Start milder if this is revisited -- onset damage is a property of aggressive
  suppression, not of the library that was removed.
- Long-run heap behaviour. All arms were sampled from a boot-fresh heap; a board
  twenty minutes into use fragments further.

### Bench notes worth keeping

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
