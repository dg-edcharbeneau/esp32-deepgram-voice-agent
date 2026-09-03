# A conversation that outlives the device

Where the last few turns live, why they are on flash rather than in NVS, and
what it takes to forget them.

## What survives what

The device has always survived a *network* disconnect. `dg_agent` keeps the
conversation in RAM and replays it into `agent.context.messages` on every
connect, suppressing the greeting when there is anything to replay — which is
why `esp_websocket_client`'s five-second retry is invisible, and why a voice
change (which needs a whole new session, because this account ignores
`UpdateSpeak`) does not restart the conversation.

What it did not survive was losing power. The ring was `.bss`, so a brownout, a
crash, an unplugged cable or the 30-second stuck-session reboot in `main.c` all
came back to a device that greeted you as a stranger. That is the gap
`main/history_store.c` closes.

| what happens | conversation | why |
| --- | --- | --- |
| Wi-Fi drops, socket retries | kept | replayed from RAM, always has been |
| idle timeout stops the session | kept | the device went quiet; nobody ended anything |
| tap or BOOT stops the session | kept | **changed** — see below |
| voice change reloads the session | kept | replayed, plus a "you are resuming" note in the prompt |
| brownout, crash, unplug, reboot | kept to within ~1.5 s | read back off flash at `dg_agent_init()` |
| `new_conversation`, confirmed | gone | the one voice path that forgets |
| hold-again on a stopped device | gone | the one gesture that forgets |
| `idf.py flash`, or a swap to the sibling firmware | kept | data partitions are not written |
| `idf.py erase-flash` | gone | so are the credentials and the key |

**A deliberate stop no longer ends the conversation.** It used to, on the reading
that stopping on purpose means being finished. But a tap is how you stop the
device streaming, and people reach for it for reasons that have nothing to do
with being done talking — so the one gesture that was easiest to hit was also the
only one that destroyed something. Now nothing clears the conversation
implicitly.

## Why not NVS, where every other setting lives

The `nvs` partition is `0x6000`: six 4 kB pages, one always held back for
compaction, 126 entries of 32 bytes each. That is **630 entries for the whole
device**, and it is already shared with the Wi-Fi driver's calibration blob
(`nvs.net80211`), the credentials, the API key and the agent settings. A
`nvs_get_stats()` line is logged at boot so this is a number you can read rather
than a claim you have to take.

Two things make it the wrong home. A kilobyte-scale blob writes its new copy
before erasing the old, so a rewrite wants roughly twice its entry count free at
that instant in a five-page arena — which means compaction on most writes. And
**compaction rewrites its neighbours**: the erase budget a per-turn history write
spends is the budget holding the Wi-Fi credentials, on a board whose only
recovery from losing them is holding BOOT for three seconds. Depth of transcript
is not worth that.

SPIFFS on `storage` was the other obvious answer and is worse: the component
costs flash, `CONFIG_SPIFFS_CACHE` holds a few kB of *internal* RAM for the
lifetime of the mount, and the first mount scans a 7 MB partition's object
lookup at boot — all to hold one file that is never named, listed or shared.

## Raw slots in `storage`

`storage` is declared in `partitions.csv` and mounted by nobody — not here, and
not by `spec_analyzer_radial`, the sibling firmware the partition table is kept
byte-identical to (it is a single-file project with no SPIFFS in it at all).
Seven megabytes, of which this uses thirty-two kilobytes.

The layout is a ring of eight single-sector records:

    magic 'DGH1' | seq | len | crc32 | payload

A read scans all eight headers and takes the highest sequence number whose magic
*and* CRC both check. A write erases the **next** slot, writes the payload, and
writes the header **last** — so a power loss anywhere in the middle leaves the
previous record whole and the new one failing its magic check. That is atomicity
without a journal, and it is the entire reason for the A/B ring: an in-place
rewrite has a window where neither copy is good, and this has none.

Wear works out to eight sectors of a hundred thousand cycles, so roughly eight
hundred thousand writes — and none of them anywhere near the credentials.

That ordering claim is tested rather than asserted. `host/store.sh` compiles the
real module against a fake NOR flash that clears bits rather than copying them
and can be told to cut a write short, which is what a brownout looks like from
the flash's side. Both windows are covered: cut mid-payload, and cut mid-header.

## A byte arena, not an array of turns

The old shape was six slots of 160 characters, and it was wrong in both
directions at once. It truncated exactly the long assistant turns that carry the
context, while six twelve-byte "yes please" turns spent 960 bytes saying
nothing.

Sizing the store in **bytes** and evicting whole oldest turns until the new one
fits gets 25–40 real turns out of `HISTORY_BYTES` (3 kB) instead of six, in less
memory than a 40 × 512 array would need. Each turn is stored NUL-terminated —
one byte per turn that buys the whole of `history_to_json()`, since the content
can then be handed to cJSON in place with no copy onto a stack this file works
hard to keep small. The arena itself is PSRAM, allocated once in
`dg_agent_init()`.

**The ceiling is internal heap, not the wire** — and the first version of this
section had it wrong, which cost a flapping session on the bench.

The obvious constraint looks like framing: `esp_websocket_client` chunks a send
at its `buffer_size`, 4096 here. That one is a red herring — `Settings` has never
been close, since the system prompt alone measures 12,038 bytes, so the message
has always gone out as `TEXT(FIN=0)` + `CONT(FIN=1)` and the endpoint has always
been fine with it. The next candidate is `CONFIG_LWIP_TCP_SND_BUF_DEFAULT`
(23040), which a 20 kB message technically fits.

What actually binds is memory. Measured on the device, 2026-09-01: at 16 turns
and a 6 kB budget the `Settings` message came out at **20,265 bytes** and two of
five connects died with

```
E esp-aes: Failed to allocate memory
E esp-tls-mbedtls: write error :-0x0001
E dg_agent: failed to send Settings
```

each followed by a five-second reconnect and the same coin toss again. Across
that capture the TLM line's `intmax` — largest free **internal** block — swung
between 27,648 and 6,144 bytes, and the writes failed on the dips. Sending
`Settings` is the worst possible moment to ask for internal memory: the TLS
handshake has just completed, and building the message churns roughly ten small
cJSON allocations per replayed turn through the same heap.

So `HISTORY_REPLAY_BYTES` (1280) and `HISTORY_REPLAY_MAX_TURNS` (6) are a hard
limit, not a backstop. They restore the envelope the firmware ran in for months
before the arena existed — six turns of at most 160 characters, about 1.2 kB of
history on a ~14 kB base. The arena still holds 25–40 turns on flash; only the
replay is small.

Raising either means re-running that capture. `send_json` logs the byte count and
TLM logs `intmax`, but the failure is a coin toss against a fragmented heap — a
short run that happens to work proves nothing.

When the limit binds, `history_to_json()` chooses turns **newest first** and
emits them oldest first, so what gets dropped is the oldest context rather than
the most recent, and it logs that it dropped any.

## Who may touch the arena

Three tasks reach it, and only one of them writes turns. `history_add()` runs on
the WebSocket event task; `dg_agent_flush_history()` reads the whole thing on
`session_ctl`'s worker; `dg_agent_clear_history()` empties it from whichever task
asked. A mutex covers the arena and its index together, because a turn landing
mid-copy writes a record whose index and arena disagree — and since the record is
validated on load, the cost is not one garbled turn but the **entire history
discarded at the next boot**, which is the one thing this feature exists to
prevent.

That lock is safe to hold, unlike almost anything else in `dg_agent.c`. It is
ordered inside nothing: neither holder calls into `esp_websocket_client` while
holding it, so it cannot join the transmit-mutex inversion
[session-control.md](session-control.md) describes. And the flush copies under
the lock but writes to flash outside it, so the WebSocket task waits on a
`memcpy`, never on a sector erase.

## When the write happens, and on which task

Not on the task that noticed the turn. `history_add()` runs on the WebSocket
client's own event task, and a sector erase is tens of milliseconds with the
flash cache disabled on both cores — spending that there stalls audio delivery
and the KeepAlive at the exact moment the agent is about to speak.

Not on the timer task either. `esp_timer` runs at priority 22, above lwIP.

So a turn only sets a flag and arms a 1.5 s debounce; the timer callback asks
`session_ctl`'s worker, which exists for slow blocking work off the audio core,
and that is where the write actually happens. The debounce is restarted by every
turn, so a user turn and the reply it provokes coalesce into **one** write. There
are two unconditional flushes on top of it: at the end of `do_stop()`, and when
the socket drops on its own — the leading indicator of a link that is about to
stay down and a board about to be power-cycled to fix it.

The debounce restarts on every turn, so it carries a cap
(`HISTORY_FLUSH_MAX_DEFER_MS`, 5 s). Without one, a brisk back-and-forth of turns
closer together than the debounce would push the deadline ahead of itself
indefinitely and keep the whole conversation out of flash — "1.5 s of exposure"
would quietly become the length of the conversation.

The cost, then, is an exposure window of 1.5 s in an ordinary exchange and at
most 5 s in a fast one — the cap bounds the deadline rather than merely
declining to restart the debounce, so that second number is the one that
happens. The alternative — writing only at a clean stop — survives
none of the crashes this feature exists for.

The retry can also give up: three failed writes latch persistence off and say so
once. `history_store.h` documents a missing partition as something to run without
rather than refuse over, and an unbounded retry would turn exactly that case into
a worker wakeup every debounce for the life of the device.

**One subtlety worth not rediscovering.** Every other request reaches that worker
as a task-notification *value*, sent with `eSetValueWithOverwrite`. A flush sent
the same way would have replaced a pending toggle, and a tap would silently do
nothing. So the flush rides as a separate flag with an `eNoAction` wakeup, and
the worker checks it before its `switch` — before, because the `default` arm
continues straight back to the wait, so a check at the bottom would miss exactly
the flush-only wakeups it exists for.

## Forgetting, twice confirmed

**By voice.** `new_conversation` is a client-side function like `set_voice` and
`reset_name`, and the only one here that destroys something. Its description
tells the model to ask first — but a description is a request, so the
confirmation is enforced in code: the first call arms and does nothing, and only
a second call actually forgets. A model that skips straight to calling it cannot
lose the conversation.

Three things have to hold for that second call to count, and each one closes a
way the first draft could be talked past:

- **Within sixty seconds.** Otherwise an arm from ten minutes ago turns an
  unrelated later request into an immediate wipe.
- **Exactly one user turn since arming**, that being the answer. A time window
  alone is not a confirmation — nothing stops the model calling the function
  twice in one breath, and two calls inside the window would then wipe with
  nobody having been asked.
  Exactly one, not at least one, so that an unanswered question from earlier in
  the minute cannot be cashed in later — once the conversation moves past the
  answer, the arm is stale. The cost is that a confirmation split over two
  utterances ("Yes." "Go ahead.") does not count and the device asks again. That
  is the direction to be wrong in: asking twice is recoverable, wiping is not.

**Be clear about what this buys.** It guarantees the device *asked* before it
wiped, that somebody spoke after being asked, and that it was recent. It does
**not** guarantee the answer was yes — "no" is a user turn like any other and
nothing here can read it, so a model that calls the function again after being
told no will wipe. No client-side counter can prevent that; it is the same class
as a model calling any other function wrongly. What the two-call scheme removes
is the failure where nobody was asked at all.

It counts the **user's** turns rather than the agent's on purpose: how many
`ConversationText` events the agent emits per reply is Deepgram's business and
could change, and a rule built on it would fail closed — forget would become
unreachable and the device would ask forever. For the same reason the counter is
maintained in the `ConversationText` handler rather than in `history_add()`,
which returns early when there is no arena and while the history is frozen.

Clearing sets `s_reload_pending`, because the turns are gone from the device but
this session still holds them server-side. The reload is deferred to
`AgentAudioDone` like every other one, so the socket does not vanish mid-sentence
— and the next `Settings` carries no context, so the greeting fires.

**With a backstop, unlike every other user of that deferral.** `main.c` records
that Deepgram sent `AgentAudioDone` *zero times* across a 12-minute run. For a
voice change that just means the setting applies a bit later. Here it would mean
the model still holding every turn after saying it forgot them — and, worse, the
freeze that stops the agent's own confirmation being recorded back into the
history it just cleared is released only at `SettingsApplied`, so nothing said
for the rest of the session would be recorded or persisted. Silently. So the
forget path arms an eight-second timer as well, and whichever fires first wins.

**On screen**, for a device whose session is down: hold, and hold again. See
[session-control.md](session-control.md#holding-on-a-stopped-device) for why the
confirmation is a second hold rather than a tap.

Both paths funnel through `dg_agent_clear_history()`, which **does not touch
flash** — deliberately. Its two callers are the function handler, on the
WebSocket event task, and the gesture, on the LVGL task with the LVGL lock held,
and a sector erase is tens of milliseconds neither of them can spend. So it
clears RAM and marks the result dirty, and the worker writes the record through
the same path every ordinary turn uses.

What lands is a record holding **zero turns** rather than an erased slot, so
"this was deliberately forgotten" and "this device has never talked to anyone"
stay distinguishable in a serial capture. Losing power inside the debounce costs a
clear that has to be asked for again, which is the harmless direction to fail
in — the opposite mistake would forget a conversation nobody confirmed.

## What it looks like from the outside

One word in the middle of the screen, which is the whole user-facing answer to
"was my conversation lost":

| centre reads | means |
| --- | --- |
| `connecting` | opening a session with nothing to resume |
| `resuming` | opening a session that will pick up where it left off |
| `stopped` | stopped, nothing saved |
| `stopped, saved` | stopped, and there is a conversation to come back to |
| `forget? hold again` | a hold is offering to clear it; five seconds to answer |
| `forgotten` | it is gone, from RAM and from flash |

(All ASCII: `lv_font_montserrat_24` is built over 0x20–0x7F here, so anything
prettier renders as a placeholder box.)

---

[Back to the README](../README.md)
