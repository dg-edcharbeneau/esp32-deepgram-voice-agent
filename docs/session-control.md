# Starting, stopping and interrupting a session

The BOOT button and the touch panel, the control task behind them, and the idle
timeout that ends a conversation nobody is having.

## Ending, starting, and interrupting a conversation

The **inner circle** is the control, about 70 px in radius, and it is the only
live part of the screen. **Tap** it and one of two things happens:

| when you tap | what happens |
| --- | --- |
| the agent is speaking | **interrupt** — the reply stops and the mic stays open |
| anything else | **toggle** — end the current conversation, or open a new one |

**Hold it for a second** to force a restart from either state, for when a session
is up but wedged. Touches outside the circle are ignored.

**One target, two meanings, and never both.** The interrupt used to have a target
of its own: everything *outside* the button, which is most of a 466 px panel. That
was backwards. The gesture nobody aims at collected every brush of the bezel, and
each stray touch cost a sentence — the same complaint
`docs/notes/echo-cancellation.md` records from
the other side, that "`UI_INTERRUPT` fired on any short click outside the centre
button". Moving it onto the button is what makes it safe, because the two meanings
are never both plausible at once: while the agent is talking a tap can only mean
*stop talking*, and the rest of the time it can only mean *start or end this*. The
split is a single `if`/`else` in `on_gesture()` — an `if`/`else` cannot fall
through into the toggle, which is the property being bought. An interrupt must
never be able to also hang up.

The branch tests `audio_io_playback_active()`, not a state of the UI's own: true
while the ring holds audio and for `PLAYBACK_TAIL_MS` past the last write, which is
the same thing as "you can still hear it". Being wrong in the generous direction is
the safe way to be wrong here — the worst case is a tap that interrupts nothing,
against a tap that ends the conversation.

There is one timer behind it. The interrupt makes its own branch condition false
within ~300 ms of the flush, so the second tap of an impatient double-tap arrives
at a quiet device and reads as *hang up*. `INTERRUPT_GRACE_MS` (1.5 s, matching
`session_ctl`'s cooldown) refuses the toggle for that window and logs
`EVT tap ignored (interrupt grace)`. Ending a conversation is a considered gesture;
it still works a beat later.

The **BOOT button diverges here on purpose**: its click is an unconditional
toggle, whatever the audio is doing. It is the escape hatch — the thing that still
works on a device whose display or session is misbehaving — so what it does must
not depend on the state of the audio path. The interrupt lives on the screen.

### Seeing the button

`CONFIG_UI_SHOW_INDICATORS` (default off) draws the touch target as a thin ring at
exactly the hit radius, and puts a line under the status word naming what a tap
does right now — `interrupt`, `stop`, `start`, or `advance` during the display
test. The ring changes colour with it, so the affordance and the action agree.

It shows; it does not change. Every gesture behaves identically with the flag off.
The cost is a frame: the ring is drawn after the face and invalidates its own
bounding box, which the orb otherwise avoids — it invalidates only the dots it
moved. Measured, it adds 176 B to `frame_timer_cb`'s stack frame (192 → 368 B
against an 8 kB LVGL task stack) and about 480 B of flash.

A press has to show, because a tap landing during the cooldown otherwise gives no
sign it was seen. Each face signals it in its own grammar: the spectrum lights
its inner ring cyan, and the orb lifts the whole shell's amplitude, which reuses
the "event" coupling every one of its behaviours already has.

One rough edge, half fixed: the spectrum draws a ring at exactly the hit radius,
so its affordance and its target coincide. The orb paints across about 410 px with
no centre landmark at all, so it looks like a much larger button than it is.
`CONFIG_UI_SHOW_INDICATORS` answers this on the bench, but not on a shipped
device, where the orb is still a 466 px picture wrapped around a 70 px control.

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

The board's two physical buttons are **RESET**, wired to `EN`/`CHIP_PU` and
therefore a hardware reset that firmware never sees, and **BOOT** on **GPIO 0**.
The BSP is no help here — `BSP_CAPS_BUTTONS 0`, and neither its Kconfig nor its
README mentions a pin — so the pin was recovered from the factory firmware
instead. The flash dump in the sibling project is a xiaozhi-esp32 build whose
board class is `WaveshareEsp32s3TouchAMOLED1inch75`, and upstream's
`main/boards/waveshare/esp32-s3-touch-amoled-1.75/config.h` declares exactly one
button, `BOOT_BUTTON_GPIO GPIO_NUM_0`. That header's 1.75**C** branch is the one
that matches this board: its `LCD_RST 1` / `TOUCH_RST 2` / `MCLK 16` are our
`BSP_LCD_RST` / `BSP_LCD_TOUCH_RST` / `BSP_I2S_MCLK` exactly.

BOOT is now wired up in [main/boot_button.c](../main/boot_button.c) — click to
start/stop, hold three seconds to forget the saved network — which works because
[main/session_ctl.c](../main/session_ctl.c) takes plain `toggle()` / `restart()`
requests from any task. The screen keeps the gestures below unchanged; a
"hold even longer" gesture was never an option, because `LONG_PRESSED` would trip
restart on the way past. (The threshold is 1000 ms, set by
`lv_indev_set_long_press_time()` in `ui.c`. LVGL's own default is 400 ms, which is
well inside an ordinary tap.)

**GPIO 0 is a strapping pin.** Held low *through* a reset it puts the ROM into
USB download mode and the app never starts, so BOOT-held-while-pressing-RESET is
not a Wi-Fi reset — the forget gesture is a press *after* boot. Polling the pin,
which is what `iot_button` does, avoids the trap by construction.

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
described in [audio-path.md](audio-path.md), not a click. `audio_io_reset()` clears it,
and must run *after* `dg_agent_stop()` has brought the WebSocket task to a halt.
If a restarted session ever comes back as loud static, that is the first thing to
check.

## Stopping when nobody is talking

The device uplinks 16 kHz mono for as long as a session is open — roughly
32 kB/s — and reconnects on its own if the socket drops. A board left powered on
a desk therefore streams, and bills, indefinitely.
`CONFIG_SESSION_IDLE_TIMEOUT_S` (default 15, 0 disables) stops the session once
nothing has happened for that long; tapping the screen or the BOOT button starts
it again.

The trade is reconnect latency on the next word — measured, 1.1–6.0 s of
connecting plus 0.5 s of buffering — so a short timeout costs a wait before the
device can hear you.

Two details worth knowing. `session_ctl_request_stop()` is a distinct request
rather than a toggle, because a toggle that raced the running flag could *start*
a session and turn a cost-saving measure into an unattended one that bills until
someone notices. And history is kept, unlike a deliberate tap-to-stop: the device
stopped for want of conversation rather than because anyone ended it, so picking
it up again resumes instead of re-greeting.

Activity means an end-of-turn, Deepgram reporting the user started speaking, or
the speaker being busy — deliberately not the local microphone level, which would
tie session lifetime to whether the display came up.

---

[Back to the README](../README.md)
