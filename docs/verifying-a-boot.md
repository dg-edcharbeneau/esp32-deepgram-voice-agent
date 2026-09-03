# Verifying a boot

What a healthy session looks like on the serial console, and how to read the
counters when it is not healthy.

## What a healthy boot looks like

Because `agent.greeting` is spoken as soon as Deepgram applies the `Settings`
message, a session produces audio with no microphone attached at all — the
greeting round-trips through Deepgram's LLM and TTS and comes back as PCM. That
is the end-to-end proof:

```
I (673) history_store: no saved history
I (1100) battery: AXP2101 at 0x34, sampling every 5000 ms
I (1105) battery: charge target REG64=03 -> 4.2V
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
I (15000) history_store: saved slot 0 seq 1: 214 bytes
```

**On a device that has talked before**, the first and last of those change, and
they are the two lines that say persistence is actually working:

```
I (673)  history_store: loaded slot 3 seq 12: 1841 bytes
I (676)  dg_agent: resumed 14 turns, 1839 bytes
I (4805) dg_agent: replaying 6 of 14 turns (send budget)
```

and the centre of the screen reads `resuming` rather than `connecting`, with no
greeting spoken. A device that says `no saved history` on every boot is not
resuming, whatever the screen shows.

The one to watch for is
`history_store: no 'storage' partition -- history will not persist`, which means
the store never found its partition and the device is running without memory
across reboots. It is deliberately not fatal, so nothing else will tell you.

Two of those lines are the battery's whole boot output, and both are worth
reading rather than skipping:

- **`AXP2101 at 0x34`** means the PMU answered a probe. `no AXP2101 at 0x34` in
  its place is not fatal — the rest of the firmware runs, the indicator simply
  never appears and `get_battery` says it cannot read the battery — but it means
  the shared I2C bus or the chip is not where this build expects.
- **`charge target REG64=03 -> 4.2V`** is the voltage the charger stops at, read
  once and never written. This is the line that explains a cell which charges
  part-way and stops: a target of 4.0 V or 4.1 V leaves a 4.2 V cell genuinely
  short, and the gauge then parks well under 100% with nothing wrong. Pair it
  with `chgst=` on the TLM line, which is the charge state machine — `2` is
  constant-current, `3` constant-voltage, `4` done, `5` not charging. A plateau
  at `chgst=4` with `mv` near the target is the charger finishing normally; a
  plateau at `chgst=5` well below it is something else stopping it.

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

---

[Back to the README](../README.md)
