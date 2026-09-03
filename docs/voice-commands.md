# Changing things by asking

Everything the agent can change about itself mid-conversation -- volume, name,
face, orb colour, voice -- and what each one costs, plus the two things it can
read about itself: charge and Wi-Fi signal.

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

## Asking how much charge is left

`get_battery` is one of the two tools here that change nothing -- they read. "How much
charge is left?", "are you plugged in?", "how long will you last?" all land on
it, and it answers with a percentage the agent says out loud.

The number comes from the AXP2101's own fuel gauge, sampled every 5 s by
`main/battery.c`, not from a voltage the firmware guesses a curve for. The tool
description tells the model it runs on a battery and that this is the only way
it can know the level, because otherwise it has no way to know either fact and
will happily invent a number.

Four answers, and the differences are deliberate:

| state | what it says |
|---|---|
| charging | "at 62 percent and charging" |
| done, still plugged in | "at 62 percent and done charging, still plugged in" |
| discharging | "at 62 percent, not plugged in" |
| low (<= `CONFIG_BATTERY_LOW_PCT`) | the percentage, plus a suggestion to plug in |
| no reading | says it cannot read the battery, and is told not to guess |

"Charging" comes from the AXP2101's charge **state machine** -- trickle,
pre-charge, constant-current, constant-voltage are all still charging -- and not
from its current-direction field. The direction field falls back to standby as
the charge tapers into constant-voltage, so anything keyed to it stops saying
"charging" while the charger is still working. It is also gated on VBUS, so it
can never claim charging on a device running off the cell.

"Done" is its own answer rather than falling into "not charging", which would
also be true on battery and reads as a fault. Note that the charger stops at a
**configured target voltage** (`REG64[2:0]`, one of 4.0/4.1/4.2/4.35/4.4 V,
logged once at startup), and a target below the cell's rating means "done"
arrives well short of 100% on the gauge -- so the percentage is worth saying
alongside it rather than being rounded up to "full".

The dots on screen use the same reading, and the same hysteresed `low` flag, so
what the device says and what it shows cannot disagree. See
[the display](display.md) for where they are drawn.

## Asking how the Wi-Fi is

`get_signal_strength` is the other reading tool. "How's your wifi?", "how's your
connection?" and "why do you keep cutting out?" all land on it.

`esp_wifi_sta_get_ap_info()` returns the RSSI the Wi-Fi driver already has from
the beacons it receives, so this costs no scan and no radio time -- unlike the
scan in `main/wifi_prov.c`, which starves the link for hundreds of ms and only
ever runs while there is no session. There is no sampling task; the number is
read where it is used.

**The dBm never reaches the model.** "Minus sixty-two dBm" spoken aloud tells the
listener nothing, and a number with no scale attached invites the model to invent
one, so `main/wifi_sta.c` buckets it and `dg_agent.c` turns the bucket into the
sentence:

| bars | dBm | what it says |
|---|---|---|
| 4 | >= -55 | "excellent" |
| 3 | >= -67 | "good" |
| 2 | >= -75 | "fair" |
| 1 | >= -85 | "weak", and that it may cut out |
| 0 | < -85 | "very weak", plus a suggestion to move closer to the router |
| no association | -- | says it cannot read the signal, and is told not to guess |

Buckets are **hysteresed by 3 dB on the way up only**: beacon RSSI walks several
dB between beacons, so a device sitting at a boundary would otherwise flicker
between two answers. Losing a bar is reported at once, because that is the
reading worth acting on; gaining one waits for margin.

There is a standing irony here worth knowing about when reading a log: a link bad
enough to matter may never deliver the tool call at all. What this tool describes
is always a link that was good enough to carry the question.

The wifi icon on screen uses the same bucket and the same hysteresis, so what
the device says and what it shows cannot disagree. See
[the display](display.md) for how it is drawn. Independently of the tool, the
driver's own RSSI threshold (`CONFIG_WIFI_SIGNAL_WEAK_DBM`, default -80 dBm) logs
**one** line per excursion below it -- which is what puts a cause in the log
immediately before the dropped audio that follows.

One line, not one per crossing, and the difference was measured: the driver's
threshold is one-shot, so the handler re-arms it, and re-arming while the link is
still below it makes it fire again about once a second. A ten-second dip produced
thirteen identical lines and buried the TLM output at the moment it was worth
reading. So the arming is unconditional and the *line* is edge-triggered -- gated
until `wifi_sta_get_signal()` sees the reading recover past the threshold plus the
same 3 dB, which makes the 1 Hz reader the thing that decides an excursion has
ended.

Two limits worth knowing when reading a log. The driver's own averaging is
coarser than the 1 Hz sample, so **not every dip visible in `rssi=` raises an
event** -- measured dips to -80 dBm passed without one. And the 3 dB recovery
margin can swallow a second crossing if the link only recovers part-way. Both are
acceptable because `rssi=` is on the TLM line every second regardless: the
warning is a convenience for spotting a cause quickly, the TLM line is the
record.

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

## Starting over by asking

"Let's start a new conversation" works, and it is the only function here that
destroys something. `new_conversation` takes no arguments and forgets everything
the device is holding, in RAM and on flash.

Mechanically it is a fourth pattern, because it is the only one that has to
defend against being called. The description tells the model to ask first — but a
description is a request, so **the confirmation is enforced in code**: the first
call arms and does nothing, and only a second call counts — inside sixty seconds,
and after **exactly one** user turn.

That buys three things: the device asked before it wiped, somebody spoke after
being asked, and it was recent. It does **not** verify that the answer was yes —
"no" is a user turn like any other and nothing on the device can read it. What
the scheme removes is the failure where nobody was asked at all. "Exactly one"
rather than "at least one" additionally means an unanswered question from earlier
in the minute cannot be cashed in later; the cost is that a confirmation split
over two utterances does not count and the device asks again, which is the
direction to be wrong in.

Like `set_voice` it reloads the session, because the turns are gone from the
device but Deepgram still holds them server-side. Unlike `set_voice` the reload
carries a backstop timer, because this is the one deferral that cannot afford
`AgentAudioDone` not arriving — see [persistence.md](persistence.md).

The screen has the same gesture for a device whose session is down: hold, and
hold again. [session-control.md](session-control.md#holding-on-a-stopped-device)
covers why the confirmation there is a second hold rather than a tap.

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
- **History replay.** `agent.context.messages` carries the recent turns into the
  new session as `{"type":"History","role":...,"content":...}` entries, so the
  conversation continues across the reload instead of starting over. Those turns
  now come off flash as readily as out of RAM — see
  [persistence.md](persistence.md).
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

A **3 kB packed arena** in PSRAM holding whole turns — 25-40 of them in practice,
truncated only past 512 characters each ([dg_agent.c](../main/dg_agent.c)). It is
persisted to flash, so it survives a reboot as well as a reconnect; see
[persistence.md](persistence.md) for the store, the write cadence and the
arithmetic behind the sizes.

What gets replayed into `Settings` is far smaller than what is held: at most
`HISTORY_REPLAY_MAX_TURNS` (6) and `HISTORY_REPLAY_BYTES` (1280), chosen
newest-first so the oldest context is what gets dropped. Those caps are a hard
limit set by measurement — a larger replay pushed `Settings` to 20 kB and made
the session flap, because the message is built and sent at the moment internal
heap is most fragmented. See [persistence.md](persistence.md).

**Nothing clears it implicitly.** A tap-to-stop, a long-press restart, a reload,
the idle timeout and an automatic reconnect all keep the conversation — which is
what fixes the most visible symptom of a dropped socket, where the agent used to
re-greet and forget everything. Forgetting is a deliberate, confirmed act: the
`new_conversation` function below, or hold-again on a stopped device.

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
