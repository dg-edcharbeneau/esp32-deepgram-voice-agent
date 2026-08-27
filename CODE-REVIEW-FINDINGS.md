# Code review findings, 2026-08-27

A full review of `main/*.c` -- 19 files, ~11.2k lines. `managed_components/` and
`components/tcp_transport/` are vendored and were not in scope.

Two findings were fixed in the same commit as this file. The rest are recorded
here rather than fixed, so the next person to open one of these files does not
have to re-derive them. Nothing below is speculative: each one names the failure
scenario, and where a number appears it was measured or verified.

Severity is the review's own: **B** blocks a merge, **I** should be fixed,
**N** is a nit.

---

## Fixed

### B1. Torn 64-bit read rebooted a healthy device -- `session_ctl.c` FIXED

`session_ctl_busy_for_ms()` read `s_busy_since_us` (`int64_t`) under a comment
claiming it was "one 64-bit read". Xtensa has no 64-bit load: it is two `l32i`,
and the control task (prio 4) preempts app_main (prio 1) freely on core 0.

Past 2^32 us of uptime (~71 min) the high word is nonzero, so a read landing
between the control task's two stores could take the low word of a cleared stamp
and the high word of a fresh one -- yielding `busy_ms` of roughly 4.29e6. That
tripped `main.c`'s 30-second deadlock backstop, which **restarted the board
mid-toggle** and logged `EVT sessionstuck` about a session that was never stuck.

Fixed by narrowing the stamp to `uint32_t` ms (a 32-bit store is indivisible, so
there is nothing left to tear -- the same remedy `audio_io.c` applies to
`s_speech_us`) and by reading the flag before the stamp, with the writes ordered
stamp-before-flag on entry and flag-before-stamp on exit. The public signature
was already `uint32_t` ms, so no header or caller changed.

### B2. The uplink quiesce had a TOCTOU that defeated its purpose -- `dg_agent.c` FIXED

`audio_send_task()` tested `s_ready` and *then* raised `s_sending`.
`dg_agent_stop()` clears `s_ready` and then waits for `!s_sending`. A sender that
had passed its test but not yet raised the flag was invisible to the stop, which
went on to call `esp_websocket_client_stop()` underneath a send about to take the
client lock -- precisely the wedge the separate send task was built to prevent
(see the note above `AUDIO_QUEUE_FRAMES`). `keepalive_task()` had the identical
shape with `s_sending_ka`, and worse consequences: a keepalive is TEXT, so
`transport_ws.c`'s LOCAL PATCH 2 cannot drop it and it blocks in `poll_write`
holding the lock.

Fixed by claiming each flag *before* the readiness test at both sites. Every
interleaving now ends with either the stop waiting or the send skipping; there is
no third outcome, and both tasks are pinned to core 0 so preemption is the only
interleaving to cover. The keepalive's `continue` guard became a positive
condition so the flag could wrap the whole test.

Cost: `s_sending` is briefly raised for a frame that is then discarded, so a stop
can spin one extra 10 ms tick in its quiesce loop.

---

## Open

### I3. Six cross-task 64-bit clocks still tear

`audio_io.c` and `ui.c` both carry good notes on narrowing a shared clock to
32-bit ms. These were missed, and they are the same defect B1 was:

| Variable | Writer | Reader | Consequence of a tear |
|---|---|---|---|
| `main.c:77` `s_activity_us` | **three tasks** -- WebSocket, LVGL, app_main | app_main | idle timeout kills a live session, or never fires |
| `main.c:411` `s_test_entry_deadline_us` | WebSocket | app_main | display test enters early, or 71 min late |
| `session_ctl.c:67` `s_ready_at_us` | control task | LVGL, button | gesture wrongly refused or wrongly accepted |
| `ui.c:493` `s_reported_us` | WebSocket | LVGL | INITIALIZING dwell misjudged |
| `ui.c:307` `s_last_feed_us` | audio tasks | LVGL | one frame of wrong `idle` |
| `ui.c:349` `s_feed_us[2]` | audio tasks | LVGL | one frame of wrong per-channel `idle` |

`s_activity_us` is the worst of them: three writers, and `note_activity()` is
called from all three tasks (`main.c` lines 241, 359, 377, 457, 537, 776, 806,
889). It is also the only one where a tear has a user-visible cost rather than a
cosmetic one.

`main.c:410` `s_test_entry_pending` is a plain `bool` while every other
cross-task flag in that file is `volatile`.

The remaining `int64_t` statics -- `face_orb.c` 40/52/292/373/431, `ui.c` 1172
and 1268, `audio_io.c:677` -- are single-task and fine as they are.

### I4. `orb_init()` writes the lattice tables with no bound check

`orb_geometry.c:783-845`. The pool is sized `3 * ORB_VOICE_DOTS + 2 *
ORB_WAVE_DOTS + ...`, but `n` is accumulated by summing
`round(|cos lat| * ORB_LON_DENSITY)` over `ORB_RINGS` -- and nothing in the build
ties the two together. No `_Static_assert`, no runtime guard.

I verified they agree today: the ring loops produce exactly 456 and 384, matching
`ORB_VOICE_DOTS` and `ORB_WAVE_DOTS`. But `ORB_RINGS` (17) and
`ORB_LON_DENSITY` (42) are tuning knobs one file over, and `orb_geometry.h`
explicitly invites retuning ("Reduce a mode's tuning if a frame is too dear").
Nudge either and the result is a silent PSRAM heap overflow, not a truncated orb.

Fix: `if (n + lon_count > ORB_VOICE_DOTS) return false;` inside both ring loops.
Costs nothing and turns the worst outcome into a boot failure with a log line.
`host/run.sh` will catch any behaviour change for free.

### I5. `audio_io_capture_start()` has no re-entry guard

`audio_io.c:577`. `dg_agent_init()` and `session_ctl_start()` both refuse a
second call; this one would create a second `audio_cap` task at priority 7, and
every "one writer only, so the read-modify-write needs no lock" argument in
`ui.c` and `face_spectrum.c` would stop holding at once. Called once from
app_main today, so this is defence for a caller that does not exist yet -- but
`audio_io.h` already states "the task cannot be created twice" as a fact.

### I6. `send_settings()` keeps 2,880 B of catalog buffers live in one frame

`dg_agent.c:694` and `dg_agent.c:837`. `faces[512] + face_desc[700] +
catalog[768] + description[900]` all coexist in a single frame on the 6144-byte
WebSocket task, above cJSON's own recursion.

The comment at `dg_agent.c:726-742` records that adding a *third* such pair
tripped the stack canary and put the device in a boot loop -- and then solves it
only for `set_color`, via a PSRAM buffer with the prefix written first and the
catalog appended into its tail. The two older pairs were left as they were. That
pattern applies to both unchanged, and doing it removes the trap the comment
warns the next person about.

### I7. The oversized-JSON drop path corrupts the *following* message

`dg_agent.c:1029-1036`. On overflow the buffer is reset and the slice dropped,
but the remaining CONT slices of that same message keep accumulating from offset
0, and the `fin` slice then parses a fragment tail. The result is
"unparseable message" logged against a message that was perfectly well formed.

Fix: an `s_json_dropping` flag, set on overflow and cleared on `fin`, so the drop
actually drops.

### I8. The spectrum point-samples 24 of 512 FFT bins

`face_spectrum.c:225-226`:

```c
int fft_idx = i * (FFT_N / 2) / STRIPE_COUNT;
display_spectrum[i] = ...s_spectrum[fft_idx];
```

488 of the 512 bins are computed and thrown away, so a tone landing between taps
is invisible and the bars alias. Linear spacing also puts about 20 of the 24
bands above 4 kHz, where speech has little energy.

**Decided, deferred: average (or max) over each band's bin range, keeping linear
bin spacing.** Two lines, no new constants, no re-tuning -- the FFT is already
paid for. Log-spaced band edges would be the larger visual win but would need
`DB_MIN` and the perceptual curve re-tuned, and would diverge from
`spec_analyzer_radial`, which this face was lifted from unchanged.

### I9. `orb_raster.c`'s performance claim is ~11x optimistic, and it is load bearing

`orb_raster.c:9` states the blitter is "about 11,000 pixel blends, roughly
1.5 ms". Measured across 5,000 `face_orb` log lines from the serial captures in
`/tmp` (all predating this review):

| | min | median | p90 | max |
|---|---|---|---|---|
| `raster` us | 9,847 | **16,176** | 16,690 | 22,932 |
| `geometry` us | 1,762 | 2,249 | -- | 13,110 |

It never once came in under 9.8 ms. 4,977 of the 5,000 samples exceed 16 ms.

That matters because the paragraph immediately below the claim
(`orb_raster.c:11-16`) is what justifies the whole design -- hand-rolling the
blitter, keeping the per-pixel path "obvious", and not building the sprite atlas
the plan called for. Its argument is that drawing is small next to the panel's
own cost, so "no amount of rasteriser cleverness" is worth it. From 12,339 orb
`TLM` lines in the same captures:

- frame period: median **40 ms** (fps 25.0), not the ~55 ms the comment borrows
- `draw` avg: median **18.5 ms**
- raster alone: median **16.2 ms**

So the raster is about **40% of the frame period and ~88% of the draw callback** --
and because the orb's real frame is 40 ms rather than 55 ms, there is roughly
21 ms of copy-and-flush behind it, not the ~40 ms the comment assumes. The
premise "a small draw next to a large period" (`ui.c:1055`) does not hold for
this face, so the conclusion drawn from it should be re-tested rather than
inherited.

Two likely contributors, neither verified:

- The 11,000-blend estimate is consistent with a *mean* footprint of about 5x5 px
  over the depth ramp. It does not appear to account for `blit_dot()` making
  **two** passes over each bounding box (coverage accumulation, then blending),
  nor for the `sqrtf` per pixel in the transition annulus.
- The stated radius range, "0.3 to 4.2 px", is the un-swelled one.
  `SPRITE_MAX` was raised 14 -> 20 precisely because listening's `rMul` reaches
  ~2.6 (`orb_raster.c:45-58`), which grows a dot's footprint *area* by ~2x at
  amplitude. The estimate looks like it predates that change.

Nothing here is a defect in the code -- the numbers above are the device working
normally, and the transport drops that prompted this measurement were unrelated
congestion. The defect is that the comment reports a budget nobody can reproduce,
which is the one kind of comment in this tree that costs more than it saves.

---

## Nits

- `audio_io.c:273` -- `int16_t al = (l < 0) ? -l : l;` yields `-32768` for
  `INT16_MIN`, so a full-scale negative sample reads as the quietest possible.
  Diagnostic meter only. `int32_t al = abs((int32_t)l);`
- `audio_io.c:165` and `audio_io.c:250` -- partial allocation failure leaks the
  buffer that did succeed, and the message says "no internal RAM" when the
  failing request was for PSRAM.
- `face_spectrum.c:344-393` -- `init()` returns `ESP_ERR_NO_MEM` leaving up to
  six non-NULL dangling pointers; a retried `ui_set_face` re-allocates over them.
- `face_orb.c:219` -- "The rasteriser buckets by row band rather than trusting
  draw order" attributes the bucketing to the wrong file. It is real, but it
  lives in `cmp_draw_order()` at `orb_geometry.c:1845` and `host/README.md`
  describes it as the geometry's own sort order. So the "no re-sort" conclusion
  still broadly holds -- both halves arrive sorted the same way -- but the reason
  given is not the one that applies.
- `face_orb.c:436` -- logs from the LVGL task every 60 frames, inside the window
  `tlm_accumulate_frame()` measures. `ui.c:1046-1048` argues at length that this
  is exactly what corrupts those timings.
- `ui.c:552` -- the lazy `a1`/`a2` init is reachable from two audio tasks; a
  reader can see `a1` set and `a2` still zero, flattening the mid/high split for
  one block. Compute both in `ui_start()`.
- `dg_agent.c:125` -- `AUDIO_FRAME_BYTES 2560` silently duplicates
  `CAPTURE_FRAMES * sizeof(int16_t)` from `audio_io.c`. A `#define` in
  `audio_io.h` would keep them honest.
- `dg_agent.c:739` -- "543 B in use at fourteen colours"; the table has thirteen.

---

## Worth keeping

Recorded because a future change might otherwise undo them:

- Gating the keepalive on "the mic is shut because the agent is speaking"
  (`dg_agent.c:1229-1245`) turns a timing heuristic into a condition that
  actually *means* what the heuristic was approximating.
- Sample-alignment carries on both ends of the playback path
  (`audio_io.c:136-150`). One dropped byte producing permanent white noise is
  the bug nobody finds twice, and the fix is in the right two places.
- `orb_raster.c`'s coverage normalisation to true disc area, including the
  reasoning for *not* building the sprite atlas the plan called for.
- The negative results kept in comments -- `AgentAudioDone` arriving zero times
  across a 12-minute run, the 1.5 s quiet window releasing the mute mid-reply,
  `CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK` deadlocking the client. That is the
  most valuable documentation in the tree.
