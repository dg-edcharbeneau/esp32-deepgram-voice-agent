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
