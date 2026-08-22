# deepgram_agent

Wi-Fi + Deepgram Agent API session on the Waveshare **ESP32-S3-Touch-AMOLED-1.75C**.

The full loop on-device: **mic → Deepgram (STT/LLM/TTS) → speaker**, over one
WebSocket.

1. join a 2.4 GHz network and wait for a real DHCP lease
2. hold an authenticated session against `wss://agent.deepgram.com/v1/agent/converse`
3. capture from the ES7210 and stream it up
4. play the agent's reply out of the ES8311

The greeting still proves the output half by itself — Deepgram speaks
`agent.greeting` before the mic has said anything — so a boot that plays the
greeting but never answers you narrows the fault to capture.

## What a healthy boot looks like

Because `agent.greeting` is spoken as soon as Deepgram applies the `Settings`
message, a session produces audio with no microphone attached at all — the
greeting round-trips through Deepgram's LLM and TTS and comes back as PCM. That
is the end-to-end proof:

```
I (1234) wifi: connecting to "YourSSID"
I (3456) wifi: got ip 192.168.1.87
I (3460) dg_agent: connecting to wss://agent.deepgram.com/v1/agent/converse
I (4800) dg_agent: socket open
I (4805) dg_agent: sent Settings (412 bytes)
I (4810) main: agent session connected
I (5100) dg_agent: Welcome, request_id=9f3c...
I (5300) dg_agent: SettingsApplied -- session is live
I (5305) main: agent session ready
I (5900) dg_agent: assistant: Hi! I am running on an ESP32. ...
I (7400) dg_agent: agent finished speaking
I (7405) main: turn complete, 96000 audio bytes received
I (9000) audio_io: mic peak L=1842 R=17 
I (13400) main: ready | turns=1 mic=64000 B rx=96000 B played=96000 B dropped=0 B | heap=8412300 B
```

The counters separate the failure modes:

| Symptom | Reading |
|---|---|
| `rx=0` | no agent audio at all — a session problem, not audio |
| `rx` climbing, `played=0` | codec open or the playback task |
| `dropped` non-zero | ring buffer too shallow for the reply, or playback not draining (also logs a rate-limited warning) |
| `mic=0` | capture task not running, or gated the whole time |
| `mic` climbing but no reply | audio is going up but Deepgram is not hearing speech — check mic peaks |

`mic peak L=… R=…` is per-channel on purpose. The downmix averages L and R, so
if the board wires only one ES7210 input, a combined meter would read "quiet"
instead of showing you which channel is live — as in the sample line above,
where only L carries signal.

## What is on the screen

The 466x466 AMOLED shows a radial spectrum analyzer, ported from the sibling
`spec_analyzer_radial` project and driven by the session rather than by its own
I2S read (the codec is already owned by `audio_io`). Bass sits at 12 o'clock,
treble at 6, mirrored about the vertical axis, with the session state in the
middle.

The ring shows whichever half of the conversation is live:

| | palette | centre |
|---|---|---|
| agent speaking | full warm-to-violet sweep | `speaking` |
| you speaking | narrow cyan-to-blue | `listening` |
| nothing | flat bars, breathing inner ring | session state |

**Agent audio is tapped where it reaches the speaker**, not where it arrives
from the network. Deepgram delivers a whole turn faster than it plays — that is
what the 384 kB ring buffer absorbs — so a visualizer fed from `on_audio()`
would race ahead and finish while the speaker was still talking. The tap sits in
`playback_task()`, which is paced by the blocking codec write. It still leads by
about 90 ms, the depth of the I2S DMA.

**The mic tap sits after the half-duplex gate**, so the ring shows what actually
goes upstream. With `CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS` set, that also means
the two sources are never live at once.

Everything else — FFT, layout, drawing — happens in one LVGL timer on a task
pinned to core 1 at priority **4**, below `audio_play` (6) and `audio_cap` (7).
The adapter defaults to 6, which would round-robin against playback; the
override matters, because a starved LVGL task drops frames while a starved audio
task drops audio. Watch `dropped` in the status line to confirm it is holding.

Internal RAM is the binding constraint once the display is up, and the case that
bites is a WebSocket reconnect — a TLS handshake wanting a burst of it with the
render buffer already allocated. Hence the status line reports internal free and
largest-block, the LVGL render chunk is 32 rows rather than 64, LVGL uses the
C library allocator (its builtin one emits a 64 kB internal array), and the FFT
scratch lives in PSRAM. If largest-block sags towards 40 kB, cut `DRAW_ROWS` in
[main/spectrum_ui.c](main/spectrum_ui.c) before tuning anything else.

## Ending and starting a conversation

The **inner circle** is the control — the same circle the ring is drawn around,
about 70 px in radius. **Tap** it to toggle: end the current conversation, or
open a new one. **Hold it for a second** to force a restart from either state,
for when a session is up but wedged. It lights up cyan while your finger is on
it, since a press that lands during the cooldown otherwise gives no sign it was
seen. Touches outside the circle are ignored — the whole panel used to be live,
and brushing the bezel was enough to end a conversation.

There is also a **1.5 s cooldown** after each action completes, and requests are
refused outright while one is in progress rather than queued. Queueing was the
old behaviour and it was worse than useless: `eSetValueWithOverwrite` collapsed a
flurry of presses into one *extra* toggle that landed after the current one
finished. Ignored presses are logged (`request ignored (busy|cooldown)`).

The hit area is decided once, on `LV_EVENT_PRESSED`, and both gestures are gated
on that decision — testing at release would let a press that started on the bezel
drift inward and count, and `LONG_PRESSED` has no release point to test. Note
that the gate flag must **not** be cleared on `LV_EVENT_RELEASED`: LVGL sends
RELEASED *before* SHORT_CLICKED (`lv_indev.c`, `indev_proc_release`), so clearing
it there makes every tap a silent no-op. Only the highlight is cleared on
release.

Ending is a real teardown — the socket closes, the mic stops streaming, and
queued speech is dropped, so the agent goes quiet within about 100 ms rather than
playing out its buffer. Starting again opens a *new* Deepgram session, so the
agent has no memory of the previous conversation and re-speaks its greeting.
While stopped the bars sit flat, the idle breathing stops, and the centre reads
`stopped`.

The board's second physical button would have been the obvious control, but its
GPIO is not stated anywhere in the BSP (`BSP_CAPS_BUTTONS 0`), its Kconfig, or its
README — the only evidence it exists at all is a string in the factory firmware.
Rather than commit to a guessed pin, this uses the CST9217 touch panel, which the
BSP already supports. [main/session_ctl.c](main/session_ctl.c) takes plain
`toggle()` / `restart()` requests, so wiring a GPIO button alongside the gestures
is a few lines once the pin is known.

**The gestures must be `LV_EVENT_SHORT_CLICKED` and `LV_EVENT_LONG_PRESSED`.**
`LV_EVENT_CLICKED` is sent on release *regardless of long press*, so pairing it
with `LONG_PRESSED` fires the tap action on every hold too. `SHORT_CLICKED` is
emitted only when LVGL's `long_pr_sent` flag is clear — the same flag
`LONG_PRESSED` sets — which makes the two mutually exclusive by construction.

### Why a control task rather than doing it in the callback

Three separate reasons, any one of which is sufficient:

- The gesture arrives on the **LVGL task with the LVGL lock held**. Closing a
  socket there stalls every render and invites lock inversion.
- `esp_websocket_client_stop/close/destroy` **refuse to run from the client's own
  event task** — they compare the calling task handle and return `ESP_FAIL`. So a
  teardown triggered by a server event has to be handed off regardless.
- Stopping blocks. Typically 150–400 ms, but `esp_transport_connect()` runs
  holding the client's mutex, so a stop that lands mid-handshake waits out
  `network_timeout_ms`. That is why it was lowered to 5 s, and why `stopping` is
  painted before the blocking work starts.

The worker runs at priority 4 pinned to **core 0** — off the core the audio tasks
live on, and far below Wi-Fi (23), lwIP (18) and esp_timer (22).

### Why the session restarts in place

`dg_agent_stop()` closes the socket but keeps the client handle;
`dg_agent_init()` is the one-time setup and registers the event handler exactly
once. Destroying and rebuilding the client per session would be the obvious
alternative, and it is a trap: it leaves `s_client` dangling against the
priority-7 capture task, and the natural fix — a transmit mutex — **deadlocks**.
This build has no separate WS transmit lock, so sends take the same recursive
mutex the WebSocket task holds across its loop. A lock ordered outside it gives
you the control task holding ours and waiting on the client's, while the client
task holds its own and waits on ours via `send_settings()`. It hangs permanently
and silently, since neither party spins and only the idle tasks are watchdogged.

Restarting in place costs ~8–10 kB kept allocated while stopped and removes the
entire dangling-pointer surface. The client re-initialises its transport list on
every start, so a new session really is new.

### The carry byte, again

`audio_io_flush()` empties the playback ring but cannot touch the odd-byte carry
(`s_in_carry`), which is owned by the WebSocket task. Stop a session with a byte
in flight and it gets stitched onto the first byte of the *next* session,
shifting every following sample by 8 bits — the permanent full-scale noise
described in the audio section below, not a click. `audio_io_reset()` clears it,
and must run *after* `dg_agent_stop()` has brought the WebSocket task to a halt.
If a restarted session ever comes back as loud static, that is the first thing to
check.

## Changing the volume by asking

"Turn it down a bit" works. `adjust_volume` takes a signed delta, so it moves in
both directions, and the level persists across reboots under the same `dgagent`
NVS namespace as the voice.

It is a much simpler feature than the voice change, for one reason: **Deepgram
knows nothing about the volume.** It is a single ES8311 register write, absent
from the `Settings` message entirely — so there is no session reload, no history
replay and no gap. The confirmation sentence is itself spoken at the new level,
which is the feedback.

### Scale — and why the stock curve had to go

`esp_codec_dev`'s default curve maps 0-100 onto **-50..0 dB**. On this speaker
that wastes most of the control. Measured on the actual hardware:

| stock volume | effective dB | audible result |
|---|---|---|
| 20-60 | -36 to -16 | essentially silent |
| 70 | -11 | hardly audible |
| 80 | -6 | low |
| 90 | -1 | medium |
| 100 | +4 | max |

**Sixty of the hundred steps did nothing**, and the entire usable range was
crammed into the top thirty. The audible floor sits around -12 to -16 dB
effective; the stock curve spends more than half its travel below that.

So the curve is now replaced unconditionally — not just when adding gain — with
one spanning `CONFIG_AUDIO_OUT_RANGE_DB` (default **20 dB**) below the top:

| volume | effective dB | equivalent to old volume |
|---|---|---|
| 20 | -12 | 68 |
| 40 | -8 | 76 |
| 60 | -4 | 84 |
| 80 | 0 | 92 |
| 100 | +4 | 100 |

Maximum is unchanged; the whole 20-100 travel now lands inside the band you can
actually hear, and each 10 points is 2 dB — a real, roughly even step. Linear in
dB is the perceptually even axis, so equal numeric steps sound like equal
changes.

The range is expressed as **dB below the top** rather than as an absolute floor,
so raising `AUDIO_OUT_EXTRA_GAIN_DB` shifts the whole scale up and preserves the
travel instead of stretching it.

The runtime floor is 20 rather than 0 on purpose: `esp_codec_dev` special-cases
volume 0 to **-96 dB** whatever the curve says, which is silence rather than
quiet — and **an agent that has muted itself cannot be asked to unmute.** Muting,
if it is ever wanted, needs a route back that is not the voice.

The boot line reports the resolved level and the dB it asked the curve for:

```
audio_io: codecs open: 16000 Hz, 16-bit, 2 ch | volume 50 (saved) = -10 dB (range 20 dB, gain +0 dB), mic gain 24 dB
```

`CONFIG_AUDIO_OUT_VOLUME` is the factory default, same arrangement as the voice.

### Calling the codec from the WebSocket task is safe here

`esp_codec_dev_set_out_vol()` runs on the WebSocket task while `audio_play` is
inside `esp_codec_dev_write()` on the *same handle*, and `esp_codec_dev` has no
lock of any kind. That is still safe, but the reason is board-specific and worth
recording:

- The two paths touch disjoint peripherals — `write()` is **I2S**, `set_out_vol()`
  is one **I2C** register write (`ES8311_DAC_REG32`, a plain write, unlike
  `set_mute` which is read-modify-write).
- The one field both read is `dev->sw_vol`, and it is **NULL on this board**
  because the ES8311 supplies a hardware volume. That is the crux: the
  software-volume path, which *would* mutate the PCM buffer underneath the
  writer, is never taken. On a codec without hardware volume this would race.
- The I2C driver is itself thread-safe — a bus-wide lock with a bounded 100 ms
  timeout, which also serialises against touch polling on the same bus.
- Cost is ~300-500 us. The codec is registered at 100 kHz; the 400 kHz
  `CONFIG_BSP_I2C_CLK_SPEED_HZ` applies only to the touch panel.

If a touch or UI volume control is ever added, funnel both callers through
`audio_io_adjust_volume()` and put a mutex there — outside `esp_codec_dev`, so it
cannot invert against the WebSocket client's own recursive lock.

## Changing the voice by asking

The agent can change its own voice. Say "switch to a British woman" and it picks
one, applies it, and remembers it across reboots. "Go back to your default voice"
undoes it.

This is the first runtime-configurable setting on a device where everything else
is compile-time, and it is built on three pieces:

- **A client-side function call.** `agent.think.functions` in the `Settings`
  message declares `set_voice` and `reset_voice`. Client-side is signalled by the
  *absence* of an `endpoint` field — with one, Deepgram would call a web service
  instead of asking the device.
- **A session reload.** A new `Settings` message is the only thing that actually
  changes the voice (see below), so the device saves the choice, lets the agent
  finish saying what it is doing, then reopens the socket.
- **History replay.** `agent.context.messages` carries the last few turns into
  the new session as `{"type":"History","role":...,"content":...}` entries, so
  the conversation continues across the reload instead of starting over.
- **NVS.** Namespace `dgagent`, key `tts_voice`. This is the first application
  use of NVS in the project; `nvs_flash_init()` was already there for Wi-Fi
  calibration.

### `UpdateSpeak` does not work — measured, not assumed

The documented way to do this is `UpdateSpeak`, which changes the voice in place
with no reconnect. **On this account it returns `SpeakUpdated` and then changes
nothing.** That was established by elimination, and it is worth writing down so
nobody re-implements it:

- Reproduced with a **bare `UpdateSpeak` sent nowhere near a function call**, so
  it is not a function-calling problem.
- Reproduced with **both** a Flux v2 provider (`flux-cliff-en`) and an Aura v1
  provider (`aura-2-zeus-en`), so it is not Flux-specific.
- The JSON matches Deepgram's own documented example character for character.
- No `Error` and no `Warning` is emitted — the server acknowledges and ignores.
- Meanwhile the *same* model id in a `Settings` message works, which is what the
  fallback relies on.

If that is ever fixed upstream, reinstating it means sending `UpdateSpeak` and
skipping the reload — the catalog, persistence and function plumbing stay as-is.

### So the voice is applied by reopening the session

1. The function call arrives; the voice is validated and written to NVS.
2. `FunctionCallResponse` tells the agent to say it is changing voice — that
   sentence is spoken in the **old** voice, which is the audible cue.
3. On `AgentAudioDone`, `on_reload_required` fires and `session_ctl` tears the
   session down and starts a new one, roughly a **1 second gap**.
4. The new `Settings` carries the new voice plus the replayed history, and
   **omits the greeting** so it resumes rather than re-introducing itself.

Deferring to `AgentAudioDone` is what stops the socket disappearing mid-sentence.

### The history buffer

The last **6 turns**, each truncated to **160 characters**
([dg_agent.c](main/dg_agent.c)). Small on purpose: every entry is re-sent on
every connect, `Settings` is already ~1.8 kB with the voice catalog in it, and
this project has spent real effort keeping the uplink healthy. Measured: 1763 B
without history, 2144 B with five turns.

It is cleared only when the user **deliberately** ends a conversation (a screen
tap). A long-press restart, a reload, and an automatic reconnect all keep it —
which incidentally fixes the most visible symptom of a dropped socket, where the
agent used to re-greet and forget everything.

### Threading

Sending from inside `handle_json()` — the WebSocket task, mid event dispatch —
is safe: `client->lock` is recursive and that task already owns it, which is why
`send_settings()` has always sent from `WEBSOCKET_EVENT_CONNECTED`. What must
never happen there is a stop/close/destroy; the client refuses those by comparing
task handles. That is why the reload goes out as an `on_reload_required`
callback that only posts to `session_ctl`.

`session_ctl_request_reload()` deliberately **skips the debounce** the touch
gestures use: it comes from the agent acting on the user's words, not from a
finger that might have brushed the bezel, so dropping it would strand the device
on the old setting.

### `CONFIG_DEEPGRAM_LOG_WIRE_JSON`

Off by default. Turn it on to print every JSON body sent to Deepgram — the
fastest way to confirm that what you think you are sending is what actually goes
on the wire. It was how the `UpdateSpeak` behaviour above got pinned down. The
API key is not in the body; it rides in an HTTP header.

### The catalog

All 36 Flux voices live in [main/voices.c](main/voices.c), each with a `featured`
flag. Only the 13 featured ones are offered to the model as an enum — the schema
and its descriptions ride in every `Settings` message, so the full list would be
a couple of kilobytes per reconnect and a harder choice for the LLM. Settings
grows from 587 to 1761 bytes as it is. The other 23 (where the Irish, Australian,
Indian, Singaporean and Filipino accents live) stay selectable as the
`CONFIG_DEEPGRAM_FLUX_VOICE` factory default, and widening the offer later is a
one-flag change.

`CONFIG_DEEPGRAM_FLUX_VOICE` is now the **factory default** — used on first boot
and after an NVS erase. `ESP_ERR_NVS_NOT_FOUND` on read is the ordinary
first-boot case, not an error.

## Speech stack: Flux or Nova-3 + Aura

`menuconfig` -> "Speech stack" picks between:

- **Flux** (default) — `flux-general-en` STT with model-integrated end-of-turn
  detection, and Flux TTS (`CONFIG_DEEPGRAM_FLUX_VOICE`, default `flux-kit-en`).
- **Nova-3 + Aura** — the previous stack, kept so the two can be compared on one
  build.

Both halves of Flux live inside the Agent API, so this is a `Settings` change
rather than a second set of sockets. What selects Flux is
`agent.{listen,speak}.provider.version = "v2"` — **the model name alone is not
enough**, and `v1` is assumed when the field is absent.

```json
"listen": { "provider": { "type": "deepgram", "version": "v2", "model": "flux-general-en" } },
"speak":  { "provider": { "type": "deepgram", "version": "v2", "model": "flux-kit-en" } }
```

### Why this works on this board at all

Both Flux STT and Flux TTS support **linear16 at 16 kHz**. That is the whole
reason the swap is cheap here: the ES7210 and ES8311 share one duplex I2S and
cannot be clocked differently, so a stack that insisted on 24 kHz output would
have needed on-device resampling. Flux TTS *defaults* to 24 kHz — the explicit
`audio.output.sample_rate` in the `Settings` message is what keeps it at 16 kHz,
and it is not optional here.

`audio.output.container` is set to `"none"` explicitly. It is already the
default, but Flux TTS *rejects* containers and compressed encodings rather than
ignoring them, so stating it turns a possible silent format mismatch into a loud
one.

### Two things that differ from v1

- **No `agent.language` on the Flux path.** `language` is a v1 listen-provider
  option; Flux uses `language_hints`, and `flux-general-en` implies English. The
  field is still sent on the Nova branch. If `SettingsApplied` ever stops
  arriving after a Settings change, this is the first thing to check.
- **Turn events stay internal.** Flux's `TurnInfo` / `StartOfTurn` /
  `EndOfTurn` belong to `/v2/listen`. Inside the Agent API they are consumed by
  the orchestrator and are *not* surfaced to the client, so the event decoding in
  `dg_agent.c` is unchanged. `UserStartedSpeaking` still fires — verified on
  hardware, and it matters because the barge-in path depends on it.

`CONFIG_DEEPGRAM_FLUX_EOT_THRESHOLD` and `CONFIG_DEEPGRAM_FLUX_EOT_TIMEOUT_MS`
tune turn detection; both are omitted from the message when left at their empty
/ zero defaults, which lets the server choose. Start there.

### Capture chunk size

`CAPTURE_FRAMES` is 1280 — **80 ms** at 16 kHz, the chunk size Flux recommends.
Besides matching the model, it cuts mic sends from ~31/s to ~12.5/s, which
directly reduces the write pressure behind the reconnect failure described
below. Cost is ~4.6 kB more internal RAM for the capture buffers.

One side effect: the spectrum ring's *mic* feed now arrives in 80 ms bursts
against a 32 ms FFT hop, so the mic-driven ring updates a little lumpier.
`spectrum_ui`'s `feed()` accumulates arbitrary chunk sizes so it stays correct,
and agent-driven visuals are unaffected — those come off the playback tap.

## Speaker volume

`AUDIO_OUT_VOLUME` is 100, and it is worth knowing what that does and does not
mean.

`esp_codec_dev`'s default volume curve maps 0-100 onto **-50 dB to 0 dB**, so 100
is unity gain, not the loudest the hardware goes. The value then passes through
`hw_gain` = `20*log10(codec_dac_voltage / pa_voltage)` = `20*log10(3.3/5.0)` =
**-3.6 dB**, which `es8311_set_vol()` *subtracts*, and lands in the ES8311's DAC
volume register — whose range is **-95.5 dB to +32 dB**. So volume 100 puts the
register at roughly 198 of 255, leaving about 28 dB unused.

`AUDIO_OUT_EXTRA_GAIN_DB` (default 0) opens that up by installing a volume curve
whose top point is above 0 dB instead of at it. It is digital gain on the PCM, so
it clips: speech has a high crest factor and takes a few dB happily, but past
that the loud passages distort before the quiet ones get usefully louder. Raise
it in ~6 dB steps and stop at the first hint of harshness.

Two ordering notes, both easy to get wrong:

- `esp_codec_dev_set_out_vol()` **must be called after the codec is open**. It
  runs `_verify_codec_ready()` first and returns without storing anything if the
  codec is closed, so a call placed too early is silently discarded and the
  device runs at `dev->volume`, which `calloc` left at 0. It works here only
  because `es8311_codec_new()` opens the codec itself, before
  `bsp_audio_codec_speaker_init()` returns.
- `esp_codec_dev_set_vol_curve()` must precede `esp_codec_dev_set_out_vol()`,
  since the curve is what converts the number into dB.

The boot line reports both: `codecs open: 16000 Hz, 16-bit, 2 ch | volume 100
(+0 dB), mic gain 24 dB`.

## Configure and build

Credentials live in Kconfig, not in source, and `sdkconfig` is gitignored.

```bash
. ~/Documents/source-iot/esp/esp-idf-5.5.5/export.sh
idf.py set-target esp32s3
idf.py menuconfig      # -> "Deepgram Agent Device": SSID, password, API key
idf.py build
idf.py -p /dev/cu.usbmodem* flash
idf.py -p /dev/cu.usbmodem* -b 2000000 monitor
```

The console runs at **2 000 000 baud**, matching `spec_analyzer_radial` so the
same monitor command works for both projects.

## Layout

| File | Role |
|---|---|
| [main/main.c](main/main.c) | boot order, session callbacks, status loop |
| [main/wifi_sta.c](main/wifi_sta.c) | station bring-up, blocks on `IP_EVENT_STA_GOT_IP` |
| [main/dg_agent.c](main/dg_agent.c) | Agent API client: `Settings`, event decoding, KeepAlive |
| [main/audio_io.c](main/audio_io.c) | both codecs: ES7210 capture, ES8311 playback, mono↔stereo, gating |
| [main/spectrum_ui.c](main/spectrum_ui.c) | radial FFT display: panel bring-up, sample handoff, ring render |
| [main/session_ctl.c](main/session_ctl.c) | stop/start worker: teardown order, gesture requests |
| [main/Kconfig.projbuild](main/Kconfig.projbuild) | SSID / password / API key / prompt / greeting |
| [sdkconfig.defaults](sdkconfig.defaults) | board hardware, TLS, Wi-Fi buffer sizing |
| [components/tcp_transport/](components/tcp_transport/) | one-line override of IDF's WS handshake — see below |

## Protocol notes worth keeping

- **`Settings` must be the first message.** The Agent endpoint accepts no query
  parameters — every option, including audio formats, is in that JSON. The
  server ignores everything sent before it answers `SettingsApplied`, so the
  client tracks `READY` separately from `CONNECTED`.
- **Audio is raw binary frames**, both directions. No JSON envelope, no base64.
- **`KeepAlive` during silence.** Deepgram drops an Agent socket that has been
  quiet for ~10 s. `dg_agent.c` sends one every 5 s from its own task; once a
  microphone is streaming, the audio itself keeps the session open.
- **Text frames get reassembled.** `esp_websocket_client` delivers at most
  `buffer_size` bytes per event, so a long `ConversationText` arrives in
  slices. Parsing each slice alone silently loses every long message.

## Trap: a short send timeout silently kills the session

Symptom: the agent re-speaks its greeting every few seconds. The greeting is only
spoken once per session, on `SettingsApplied`, so a repeated greeting always
means the socket dropped and the client reconnected into a **new** session —
never that something "triggered" the agent. `SettingsApplied` is logged with a
session number so this is unambiguous:

```
E websocket_client: esp_transport_write() returned 0, transport_error=ESP_OK, tls_error_code=0, tls_flags=0, errno=0
I websocket_client: Reconnect after 5000 ms
I dg_agent: SettingsApplied -- session #2 is live
I dg_agent: assistant: Hi! I am running on an ESP32. ...
```

The cause is that **`esp_transport_ssl_write()` returns `0`, not an error, when
its write poll times out** (`transport_ssl.c`: `if ((poll =
esp_transport_poll_write(t, timeout_ms)) <= 0) return poll;`). The WebSocket
client treats a zero-length write as fatal — `if (wlen < 0 || (wlen == 0 &&
need_write != 0))` — and tears the connection down. Note the giveaway in the
message: `transport_error=ESP_OK`, `tls_error_code=0`, `errno=0`. Nothing
actually failed. The socket just wasn't writable in time.

Mic audio is sent 31 times a second from the capture task, so it is by far the
most likely write to hit that poll, and it is the one whose timeout matters. A
200 ms deadline — which looks reasonable, since blocking the priority-7 capture
task stalls `esp_codec_dev_read()` — dropped the session every few seconds. At
`SEND_TIMEOUT` (2 s) the same setup runs indefinitely: measured 150 s, one
session, `dropped=0`, 3.3 MB of mic audio streamed.

So the trade is not "one lost 32 ms chunk vs. a 2 s stall". It is "one lost chunk
vs. the entire conversation". If capture stalls ever do become a real problem,
the fix is to move the send off the capture task onto its own queue and sender —
not to shorten the timeout.

### The other half: the TCP send buffer

A generous timeout only helps if the socket becomes writable again inside it.
With the stock send buffer it did not, and the same reconnect loop came back at
a lower rate — this time with the poll genuinely timing out after the full 2 s:

```
E transport_ws: Error transport_poll_write(0)
```

The buffers here were asymmetric. `CONFIG_LWIP_TCP_WND_DEFAULT` had been raised
to 32768 for the inbound agent audio, but `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` was
left at the stock **5760** — four times MSS. Against a *continuous* 32 kB/s
upstream mic stream that is **180 ms** of audio. One Wi-Fi retransmission burst
longer than that fills it, the socket reports unwritable, and the session dies.

`CONFIG_LWIP_TCP_SND_BUF_DEFAULT=23040` (16 x MSS, ~700 ms) fixes it. It is a
ceiling rather than a preallocation, so the memory is only consumed when the
link is actually backed up — internal free dips briefly under load and recovers.

Measured on the same AP at RSSI -68:

| | sessions in ~2-4 min | poll-write timeouts |
|---|---|---|
| 200 ms send timeout, 5760 send buffer | 6 in 120 s | many |
| 2 s send timeout, 5760 send buffer | 2 in 40 s | occasional |
| 2 s send timeout, 23040 send buffer | **1 in 240 s** | **0** |

The lesson generalises: any continuous uplink on this device needs the send
buffer sized to it. Raising only the receive window is half a fix.

### It is not the microphone

Worth stating because it is the intuitive suspect: a hot mic feeding background
noise to Deepgram cannot produce the greeting. It would produce
`ConversationText` with `role: "user"` and then an LLM-generated reply, which
would be logged as `user: ...` / `assistant: ...`. In 150 s of a quiet room there
were **no `user:` lines at all**, and idle mic peaks sit around 20-30 against
1200-2200 for speech — a healthy ratio at `MIC_IN_GAIN=24`.

## Known issue: ESP-IDF's `Host` header vs. the Agent endpoint

A stock ESP-IDF WebSocket client **cannot reach `agent.deepgram.com`**. The
handshake fails with HTTP 404 before authentication is even considered:

```
E transport_ws: Sec-WebSocket-Accept not found
E websocket_client: esp_transport_connect() failed with -1, esp_ws_handshake_status_code=404
```

The path is fine. The `Host` header is not. ESP-IDF's `transport_ws.c` always
writes `Host: <host>:<port>`, and Deepgram's Agent edge routes strictly on a
port-less `Host`. Measured against `/v1/agent/converse` with an identical
request otherwise:

| Host header sent to `/v1/agent/converse` | Response |
|---|---|
| `agent.deepgram.com` | **401** `dg-error: Invalid credentials.` — routed correctly |
| `agent.deepgram.com:443` (what ESP-IDF sends) | **404** |
| `agent.deepgram.com:0` | 404 |
| both, duplicated | 400 |

It is the `agent.deepgram.com` ingress, not the Agent service. The same path on
the regional endpoints answers 401 either way, and `api.deepgram.com` is
unaffected across every endpoint tested:

| Host | port-less | with `:443` |
|---|---|---|
| `agent.deepgram.com` | 401 | **404** |
| `api.eu.deepgram.com` | 401 | 401 |
| `api.au.deepgram.com` | 401 | 401 |
| `api.deepgram.com` (`/v1/listen`, `/v2/listen`, `/v1/speak`, REST) | 401/400 | 401/400 |

The failing 404 and the working 401 also come back from *different* services —
different `dg-request-id` formats (UUIDv4 vs UUIDv7) and different header sets —
so the port-suffixed request is being matched to a catch-all vhost rather than
reaching the agent origin at all.

Two consequences worth knowing: the 404 is indistinguishable from a genuinely
wrong path, which is why this is so hard to diagnose; and pointing at
`api.eu.deepgram.com` or `api.au.deepgram.com` is a patch-free workaround if you
can accept the region.

RFC 7230 §5.4 permits the default port in `Host`, so ESP-IDF is not strictly
wrong — and no `esp_websocket_client_config_t` field can change it, because the
port is baked into a `snprintf` format string. ESP-IDF is also inconsistent with
itself: `esp_http_client`'s `_get_host_header()` already omits the port when it
is 80 or 443. `transport_ws.c` just never got the same treatment.

`components/tcp_transport/` is the local workaround: it overrides IDF's
component by name to change that one format string. Only `transport_ws.c` and
`Kconfig` are copied; the other sources and both include directories are
referenced out of `$IDF_PATH`, so the override stays a single-file diff rather
than a fork. Delete the whole directory once upstream omits the port.

> Adding or removing that override changes component resolution, which a
> configured build directory caches. Run `idf.py fullclean` (or delete `build/`)
> afterwards, or the old `transport_ws.c` keeps getting compiled and the 404
> persists with no sign of why.


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
this board. One constant in [dg_agent.h](main/dg_agent.h) drives the `Settings`
message and both codecs.

**Channels are converted at the edges.** The codecs open with 2 channels,
matching the proven configuration; Deepgram is mono both ways. Playback
duplicates each mono sample into L+R, capture averages L+R back down to one.

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
192 kB PSRAM stream buffer with a zero timeout and returns; a task on core 1
drains it, doubles it to stereo, and writes to the codec. The buffer holds mono,
so it covers ~6 s — Deepgram sends a turn faster than it plays, and the buffer
absorbs the difference. Drops are counted rather than ignored, because the
symptom is a gap you can hear.

Both codecs are opened once and left open; reopening per turn clicks.

**Flushing is done by the reader.** `xStreamBufferReset()` fails outright while a
task is blocked reading — which the drain task normally is — so calling it from
`audio_io_flush()` silently did nothing. Instead the flush sets a flag and the
drain task, which waits in 50 ms slices rather than forever, discards the queued
audio and its carry byte itself. Dropping the carry matters: keeping it would
misalign the first sample of the next reply.

### Echo: why capture is gated

Speaker and mic sit centimetres apart and there is no echo cancellation here, so
anything the agent says is captured and sent straight back — and the agent starts
answering itself. `CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS` (default **on**) drops
capture while playback is active, plus a 300 ms tail for audio already in the I2S
DMA.

It is a crude fix and it costs barge-in: with it on you cannot interrupt the
agent, and `UserStartedSpeaking` will not fire mid-reply. Deepgram's echo
cancellation is the real answer — see the Voice Agent "Audio Preprocessing &
Barge-In" docs — at which point turn this off and the `audio_io_flush()` barge-in
path already wired up starts earning its keep.

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

## Tuning

| Option | Default | When to change it |
|---|---|---|
| `CONFIG_MIC_IN_GAIN` | 24 dB | raise if mic peaks stay near zero while you talk |
| `CONFIG_AUDIO_OUT_VOLUME` | 80 | speaker too quiet or clipping |
| `CONFIG_MIC_LEVEL_LOG` | on | turn off once the mic is trusted |
| `CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS` | on | off once echo cancellation exists |
