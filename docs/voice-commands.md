# Changing things by asking

Everything the agent can change about itself mid-conversation -- volume, name,
face, orb colour, voice -- and what each one costs.

## Changing the volume by asking

There are two tools, because "turn it down a bit" and "set your volume to 50" are
different requests and one signed delta cannot serve both:

- `adjust_volume` takes a signed `delta` — relative, both directions.
- `set_volume` takes an absolute `level`.

Both funnel through `audio_io_set_volume()`, so the clamp, the register write and
the NVS save happen in exactly one place. The level persists across reboots under
the same `dgagent` NVS namespace as the voice.

The scale runs **20 to 100**, and 20 is the floor rather than 0 on purpose:
`esp_codec_dev` maps volume 0 to -96 dB, which is silence, and an agent that has
muted itself cannot be asked to unmute. There is no mute. Asking for less than 20
lands on 20, and the function response tells the agent to say so out loud rather
than silently ignoring the number it was given.

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
`audio_io_set_volume()` and put a mutex there — outside `esp_codec_dev`, so it
cannot invert against the WebSocket client's own recursive lock.

## Changing its name by asking

The agent is called **Grammer** out of the box. "Call yourself Blake" changes
that, and it survives a reboot; "go back to your old name" undoes it.

Precedence is the voice's rule exactly: `CONFIG_AGENT_NAME` is a factory default
used on first boot and after an NVS erase, and once someone has renamed it the
saved name wins on every later boot. `reset_name` is what brings the Kconfig
value back.

Mechanically it is a third pattern, and the reason is worth keeping. `set_voice`
reloads the session because the voice is a `Settings` field this account will not
change in place. `set_face` does not, because Deepgram has never heard of the
display. The name is neither: it is **text inside the system prompt**, reaching
the model as `{{name}}` in [main/prompt/identity.md](../main/prompt/identity.md). So
the function response is enough for the session in progress, and `{{name}}`
carries it from the next `Settings` onward — no reload, no history replay, and
the two agree again the moment anything reconnects.

There is no catalog, because any name is valid. That puts the whole burden on the
function description, which is written against what actually goes wrong: the name
arrives through speech-to-text, so the model is repeating something it *heard*,
and left alone it hands over the entire sentence it heard it in.

[main/agent_name.c](../main/agent_name.c) trims and validates before storing.
Control characters are refused outright — a newline in a "name" would forge a
heading in the assembled prompt — and the 31-character cap is doing two jobs: a
name is spoken aloud, and this is a string a stranger says out loud that ends up
in the system prompt.

## Changing the face by asking

"Show me the bars instead" works, and so does "go back to the orb". `set_face`
takes a name from a two-entry catalog.

Mechanically it is the sibling of `adjust_volume`, not of `set_voice`:
**Deepgram knows nothing about the display.** No `Settings` field, so no session
reload, no history replay and no gap — the confirmation is phrased as already
done, because it is.

The catalog lives in [main/faces.c](../main/faces.c) with no LVGL in it, mirroring
the `voices.c` / `voices.h` split for exactly the same reason: `dg_agent` has to
build the function schema, and its blurbs are written for the model rather than
for a reader, so indirect phrasings resolve without anyone naming a face
exactly. `ui_set_face()` stores an index that the frame timer applies **before**
anything draws, so the incoming face owns a whole frame instead of painting over
half of the outgoing one's output.

## Changing the orb's colour by asking

"Make it purple" works, and so does "put it back to normal". `set_color` takes a
name from a thirteen-entry catalog in
[main/orb_colors.c](../main/orb_colors.c) — the accent colours of the Vira palette —
and is the sibling of `set_face` in every mechanical respect: no `Settings` field,
no session reload, applied by the frame timer, and not persisted across a reboot.

**The orb only.** The spectrum colours its bands by frequency and by which half of
the conversation is live, so a single tint would destroy information rather than
restyle it. The function description says so, which is what stops the model
offering a colour change when someone complains about the bars.

Colour is one multiply per channel on the ink the geometry already resolved, at
the single point in [main/orb_raster.c](../main/orb_raster.c) where that ink became
three RGB565 channels. Two consequences worth knowing:

- **White is an exact identity**, not an approximation of the original. Verified by
  sweeping 1,000,001 ink values against the expression it replaced — zero
  mismatches — so the default costs nothing and colour is entirely opt-in. It is
  done in float for this reason: the tempting `(lum * ch) >> 8` is a level low at
  small ink, which moves the *default* appearance.
- **The shell still reads as a surface.** The ink spans a real brightness ramp
  (87..249 across the parity dump's 6,384 dots), and a multiplicative tint keeps
  that as luminance instead of flattening the shell to one flat colour.

Names are the palette's, with one deliberate departure: its three greens and two
teals are named by relative intensity, which reads fine off a swatch and badly out
loud — "lime" against "acid lime" is a coin toss spoken aloud. So its `acid lime`
is `lime` here, its `lime` is `green`, its `bright teal` is `teal`, and its muted
`#80cbc4` teal is dropped.

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
([dg_agent.c](../main/dg_agent.c)). Small on purpose: every entry is re-sent on
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

All 36 Flux voices live in [main/voices.c](../main/voices.c), each with a `featured`
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

---

[Back to the README](../README.md)
