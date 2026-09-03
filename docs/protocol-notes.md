# Protocol notes

Things about the Agent API and ESP-IDF's WebSocket client that cost time to
learn and are worth keeping written down.

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
- **The conversation is replayed, not remembered by the server.** Every new
  socket is a new Agent session with no memory of the last one, so `Settings`
  carries the recent turns in `agent.context.messages` as
  `{"type":"History","role":"user"|"assistant","content":...}`, oldest first --
  which is what makes a reconnect, a voice change and a reboot all invisible
  rather than a fresh greeting. The greeting is suppressed whenever there is
  history, and `agent_prompt_ctx_t.notes` tells the model it is resuming, or it
  reads the replay as a conversation it is joining.
- **What is replayed is much smaller than what is stored.** The device holds
  25-40 turns; at most `HISTORY_REPLAY_MAX_TURNS` (6) go on the wire, newest
  first so the oldest context is what gets dropped. The limit is not the frame
  size -- `Settings` has always been ~19 kB and always fragmented across
  `buffer_size`, and the endpoint has always been fine with that. It is that
  each replayed turn costs about ten small cJSON allocations in internal RAM, at
  the one moment internal RAM is most stretched. Raising it to 16 turns took
  `Settings` to 20,265 bytes and made sessions flap on `esp-aes: Failed to
  allocate memory`. See [persistence.md](persistence.md).

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

---

[Back to the README](../README.md)
