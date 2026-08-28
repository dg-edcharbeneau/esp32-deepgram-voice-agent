# Verifying a boot

What a healthy session looks like on the serial console, and how to read the
counters when it is not healthy.

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

---

[Back to the README](../README.md)
