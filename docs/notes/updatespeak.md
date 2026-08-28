# `UpdateSpeak` returns `SpeakUpdated` but the voice never changes

> **Historical finding**, investigated 2026-08-21, against
> `/v1/agent/converse` as it behaved then. It is why a voice change reopens
> the session instead of updating it in place; see `docs/voice-commands.md`.
> Re-measure before assuming the server side is unchanged.

## Summary

Sending `UpdateSpeak` to `/v1/agent/converse` is acknowledged with `SpeakUpdated`
and has **no effect on the synthesised voice**. The agent continues speaking in
the voice from the original `Settings` message for the rest of the session.

No `Error` and no `Warning` is emitted. The same model id, sent in a `Settings`
message instead, works correctly — so the model name, the account entitlement,
and the audio output format are all fine. Only the in-place update is ignored.

Observed 2026-08-21.

## Environment

| | |
|---|---|
| Endpoint | `wss://agent.deepgram.com/v1/agent/converse` |
| Client | ESP-IDF 5.5.5 `esp_websocket_client` on ESP32-S3 |
| Audio output | `linear16`, 16000 Hz, `container: none` |
| Listen | `deepgram` / `v2` / `flux-general-en` |
| Think | `open_ai` / `gpt-4o-mini` |

## Expected

Per [Update Speak](https://developers.deepgram.com/docs/voice-agent-update-speak),
`UpdateSpeak` switches the Speak model mid-conversation; with Flux TTS the new
voice takes effect on the agent's next turn.

## Actual

`SpeakUpdated` is returned, and every subsequent turn is still in the original
voice. Verified by listening, across multiple turns well after the update — this
is not the documented "a turn already being spoken finishes in the voice that
started it" behaviour.

## Reproduction

1. Open a session with `agent.speak.provider` = `{type: deepgram, version: v2,
   model: flux-haley-en}`. Confirm the agent speaks in Haley.
2. With the session idle (no function call, no turn in progress), send:

```json
{"type":"UpdateSpeak","speak":{"provider":{"type":"deepgram","version":"v2","model":"flux-cliff-en"}}}
```

3. Server replies `{"type":"SpeakUpdated"}`.
4. Prompt the agent to speak again. It is still Haley, not Cliff.

Haley and Cliff are chosen because they are maximally distinguishable — an
American woman versus a deep, raspy American man.

## What was eliminated

Each of these was tested separately, and the behaviour is identical in all cases:

- **Not function-calling related.** First encountered while applying a voice from
  a client-side `FunctionCallRequest`, but it reproduces with a bare
  `UpdateSpeak` sent from a timer with no function call anywhere in the session.
- **Not Flux-specific.** Reproduces with an Aura v1 provider too:
  `{"type":"UpdateSpeak","speak":{"provider":{"type":"deepgram","model":"aura-2-zeus-en"}}}`
  — also acknowledged, also ignored.
- **Not a malformed message.** The Flux payload above is character-for-character
  the example in the documentation.
- **Not a bad model id or a missing entitlement.** `flux-cliff-en` in the initial
  `Settings` message produces Cliff's voice correctly. Only the `UpdateSpeak`
  route fails.
- **Not a silently-reported failure.** The client handles `Error` and `Warning`
  and logs every unrecognised message type. Nothing is emitted around the
  update — the only server message received is `SpeakUpdated`.
- **Not a stale local variable.** The full outgoing JSON was logged from the
  socket write path; the wire content is as shown above.

## Log excerpt

Bare `UpdateSpeak`, no function call in the session:

```
voices: voice: flux-haley-en (saved)
dg_agent: SettingsApplied -- session #1 is live
main:     TEST B: bare UpdateSpeak -> flux-cliff-en (deep male)
dg_agent: -> {"type":"UpdateSpeak","speak":{"provider":{"type":"deepgram","version":"v2","model":"flux-cliff-en"}}}
dg_agent: sent UpdateSpeak (102 bytes)
dg_agent: speak config updated          <- SpeakUpdated received
main:     TEST C: after UpdateSpeak
   ... subsequent turns still in flux-haley-en
```

Aura v1 variant, same session shape:

```
dg_agent: -> {"type":"UpdateSpeak","speak":{"provider":{"type":"deepgram","model":"aura-2-zeus-en"}}}
dg_agent: sent UpdateSpeak (88 bytes)
dg_agent: speak config updated
   ... subsequent turns still in flux-haley-en
```

## Impact

`UpdateSpeak` is the only documented way to change the voice without dropping the
session, so any feature that lets a user change the agent's voice mid-conversation
cannot work as designed.

## Workaround in use

Persist the chosen voice, then reopen the session so a fresh `Settings` message
carries it, replaying recent turns through `agent.context.messages` as
`{"type":"History", ...}` entries so the conversation survives. This works, but
it costs a reconnect (~1 s) per change and only preserves as much context as is
replayed.

## Note on verification method

The primary evidence is listening — the two voices are unmistakably different.
A spectral probe on the received PCM was also run, but it is reported here only
for completeness and **should not be treated as evidence**: measuring the peak
bin between 70–350 Hz separated the known-good male and female voices by roughly
19 Hz against a comparable spread, which is too weak to discriminate. It tracks a
harmonic rather than F0. The audible test is the reliable one.
