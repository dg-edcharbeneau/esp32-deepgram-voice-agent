# `esp_websocket_client_close()` accepts a timeout and ignores it

> **Historical finding / upstream bug report**, filed 2026-08-27 against
> `espressif/esp_websocket_client` 1.5.0 on ESP-IDF v5.5.5. It is why
> `components/tcp_transport/transport_ws.c` is vendored and patched here.

**Reported by:** Ed Charbeneau (DevRel) · **Date:** 2026-08-27
**Severity:** hangs the calling task indefinitely; on a single-control device that
means a state only a physical reset clears
**Component:** `espressif/esp_websocket_client` (observed on 1.5.0, ESP-IDF v5.5.5)
**Affects:** any client that closes a session while the socket is not draining --
which for realtime audio is the normal bad-network case, not an edge case

---

## Summary

`esp_websocket_client_close(client, timeout)` takes a `TickType_t timeout` and
never uses it for the part that can block. It forwards **`portMAX_DELAY`** to the
CLOSE-frame send:

```c
/* esp_websocket_client.c, esp_websocket_client_close_with_optional_body() */
close_ret = esp_websocket_client_send_close(client, 0, NULL, 0, portMAX_DELAY);
```

The `timeout` argument is used only for the *wait after* the frame is sent. So on
a socket that cannot accept a write, the call never returns and the caller is
stuck forever. The signature promises a bound that the implementation does not
provide.

It is also worse than a hang inside `esp_websocket_client_stop()`, because it runs
**before** it. `stop()` at least waits on a task with its own bounded timeouts;
`close()` waits on a write with no deadline at all.

## Impact

On our device -- an ESP32-S3 streaming 16 kHz mono to a voice agent over `wss://`
-- this is what a bad access point produces:

```
I (32545) session_ctl: stopping session      <- calls esp_websocket_client_close(c, 1000ms)
E (26997) websocket_client: Could not lock ws-client within 2000 timeout
E (27998) transport_ws: Error transport_poll_write(0)
   ... nothing further, for as long as anyone watched
```

The device has one control, a single touch target, and its session layer refuses
new requests while an action is in flight. So a `close()` that never returns is a
device that never accepts another tap: the panel sits on "stopping", the display
keeps rendering at 22 fps, and nothing short of the RESET pin recovers it. It
looks like a firmware freeze and is not one.

Reproduced repeatedly on 2026-08-27 against a phone hotspot that could not drain
the uplink -- ~50 dropped audio frames per minute at the transport, then a stop
that never completed.

**On what triggered it, so the report is not read as narrower than it is.** The
congestion in our case came from provisioning the device *through* the same phone
whose hotspot it then joined: the phone leaves its hotspot to reach the setup
portal, re-establishes it afterwards, and the device reboots into an access point
that is still coming up. The identical firmware never hung against an ordinary
access point.

That makes the trigger environmental and the hang a robustness bug on top of it.
The upstream defect stands on its own regardless: any socket that cannot accept a
write -- conference Wi-Fi, a saturated uplink, a link in the middle of a roam --
puts a caller of `esp_websocket_client_close()` into an unbounded wait, and the
signature says otherwise.

## Why the obvious workarounds do not work

Both were tried on hardware before settling on the third.

1. **Gate the close on the send's return value.** Does not work if you have
   ESP-IDF's `transport_ws` patched to drop a congested binary frame rather than
   tear the session down -- the drop is reported as a successful send, so the
   caller reads healthy during exactly the congestion that matters. (Unpatched,
   the same congestion kills the session instead, which is its own problem.)
2. **Gate the close on send duration.** Sound signal, but it is still a guess
   guarding a call whose failure mode is an unrecoverable device.
3. **Do not send the CLOSE frame.** What we shipped. `esp_websocket_client_stop()`
   alone is bounded by the client task's own `network_timeout_ms` and completes --
   measured at ~4 s on the same congested link that hung `close()` forever.

The cost of (3) is real but small: without a CLOSE the socket is half-open and the
server finalises the session at its own idle timer, so a session-duration-billed
API charges a few extra seconds per session.

## Suggested fix

Honour the parameter -- pass `timeout` rather than `portMAX_DELAY` to
`esp_websocket_client_send_close()`. Note this does **not** make the frame go out
on a congested socket; it makes the attempt fail in bounded time, which is all the
signature ever promised.

If the intent is genuinely "block until sent", then the parameter is misleading
and the documentation should say so, so that callers can decide not to call it.

## Notes for anyone hitting this

- The symptom is a caller stuck in `close()`, not in `stop()`. If your teardown
  logs a line before closing and nothing after, this is where to look.
- `stop_wait_task()` also waits with `portMAX_DELAY`, on `STOPPED_BIT`. That one
  is survivable in practice because the client task's waits are bounded by
  `network_timeout_ms`, but it is the next thing to suspect if skipping the CLOSE
  is not enough.
- A single-gesture device wants a deadlock backstop regardless of this bug: ours
  reboots if a session action has not completed in 30 seconds, on the grounds
  that a reboot costs about two seconds and losing the device costs a bench trip.
