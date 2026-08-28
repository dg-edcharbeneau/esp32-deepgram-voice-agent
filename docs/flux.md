# Speech stack: Flux

Why the Flux stack works on this board, and what differs from v1.

## Speech stack: Flux

The build is Flux-only: `flux-general-en` STT with model-integrated end-of-turn
detection, and Flux TTS (`CONFIG_DEEPGRAM_FLUX_VOICE`, default `flux-kit-en`).

A `menuconfig` choice used to offer Nova-3 + Aura as a fallback, and it was
removed because it could not be selected — `CONFIG_DEEPGRAM_FLUX_VOICE` was
declared `depends on SPEECH_STACK_FLUX` while `voices.c` referenced it
unguarded, so picking the fallback failed the build. A fallback that has never
compiled is worse than no fallback, because it is believed.

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
  option; Flux uses `language_hints`, and `flux-general-en` implies English. If
  `SettingsApplied` ever stops arriving after a Settings change, this is the
  first thing to check.
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
directly reduces the write pressure behind the reconnect failure described in
[protocol-notes.md](protocol-notes.md). Cost is ~4.6 kB more internal RAM for
the capture buffers.

One side effect: the display's *mic* feed now arrives in 80 ms bursts rather
than 32 ms ones. `ui.c`'s `feed()` accumulates arbitrary chunk sizes so it stays
correct either way, and the level pipeline peak-holds between frames precisely
because the two rates do not divide — a plain "newest value" would drop
transients that fall between frames. Agent-driven visuals are unaffected; those
come off the playback tap.

---

[Back to the README](../README.md)
