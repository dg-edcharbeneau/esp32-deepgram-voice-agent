# Code review findings, 2026-08-27

A full review of `main/*.c` -- 19 files, ~11.2k lines -- and a second pass over
`components/tcp_transport/transport_ws.c`, the one vendored file this project
has deliberately modified. `managed_components/` is upstream and untouched.

All twelve findings from the main/ review and all eight nits are fixed, across
four commits, plus two findings and three nits from a later pass over the
vendored transport -- see the end. They are kept here rather than deleted so the
reasoning survives the diff: each entry names the failure scenario it was fixed
for, which is what stops a future change quietly reinstating it.

Nothing below is speculative. Where a number appears it was measured, and two
entries record corrections to the review as originally written -- I4's failure
mode and I8's band arithmetic -- because both were wrong in ways that mattered.

Severity is the review's own: **B** blocked a merge, **I** should be fixed,
**N** was a nit. One thing remains unseen rather than unfixed: `txdrop=` only
moves on a link that cannot drain the uplink, so its presence is build-verified
and its value is not.

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
### I3. Six cross-task 64-bit clocks tore FIXED

`audio_io.c` and `ui.c` both carried good notes on narrowing a shared clock to
32-bit ms. These six were missed, and they were the same defect B1 was -- two
`l32i` loads with a preemption point between them:

| Variable | Writer | Reader | Consequence of a tear |
|---|---|---|---|
| `main.c` `s_activity_us` | **three tasks** -- WebSocket, LVGL, app_main | app_main | idle timeout kills a live session, or never fires |
| `main.c` `s_test_entry_deadline_us` | WebSocket | app_main | display test enters early, or 71 min late |
| `session_ctl.c` `s_ready_at_us` | control task | LVGL, button | gesture wrongly refused or wrongly accepted |
| `ui.c` `s_reported_us` | WebSocket | LVGL | INITIALIZING dwell misjudged |
| `ui.c` `s_last_feed_us` | audio tasks | LVGL | one frame of wrong `idle` |
| `ui.c` `s_feed_us[2]` | audio tasks | LVGL | one frame of wrong per-channel `idle` |

All six are now `uint32_t` milliseconds, with their comparisons rewritten as
wrap-safe unsigned subtraction. `IDLE_US`, `INITIALIZING_MAX_US` and
`TEST_ENTRY_WAIT_US` became the `_MS` equivalents; `resolve_behaviour()` converts
`now_us` once at the top rather than at each use.

`s_activity_us` was the only one with a user-visible cost, and it keeps **three
writers** by design -- `note_activity()` is called from all three tasks, every
call means "something happened", so last-write-wins is the correct rule. Its
comment now says so explicitly rather than implying single ownership, because
every other flag in that file documents its owner.

`s_test_entry_pending` was a plain `bool` among `volatile` neighbours and is now
`volatile` too. `now_ms()` had to move above `note_activity()` in `main.c`, which
previously used `esp_timer_get_time()` directly.

The remaining `int64_t` statics -- `face_orb.c` 40/52/292/373/431, `ui.c` 1172
and 1268, `audio_io.c:677` -- are single-task and were left alone.

### I4. `orb_init()` wrote the lattice tables with no bound check FIXED

`orb_geometry.c:783-845`. The pool is sized `3 * ORB_VOICE_DOTS + 2 *
ORB_WAVE_DOTS + ...`, but `n` is accumulated by summing
`round(|cos lat| * ORB_LON_DENSITY)` over `ORB_RINGS` -- and nothing in the build
tied the two together. No `_Static_assert`, no runtime guard.

They agree today: the ring loops produce exactly 456 and 384. But `ORB_RINGS`
(17) and `ORB_LON_DENSITY` (42) are tuning knobs one file over, and
`orb_geometry.h` explicitly invites retuning ("Reduce a mode's tuning if a frame
is too dear").

**The two loops fail differently, and both were verified on the host by bumping a
density and compiling the pre-guard source.**

- The **wave** loop overruns a *separate* allocation. `s_wave_unit` is its own
  `malloc` of `3 * ORB_WAVE_DOTS` doubles, so `WAVE_LON_DENSITY` 40 -> 44 gives a
  genuine heap-buffer-overflow -- ASan reports it as an 8-byte write past the
  block. On the device that is heap corruption with no diagnostic.
- The **shell** loop is worse, because it corrupts *silently*. Every shell table
  lives in one pooled allocation, so writing past `ORB_VOICE_DOTS` never leaves
  the block -- it lands in the next sub-array. `ORB_LON_DENSITY` 42 -> 44 writes
  `s_cos_lon[456..477]`, which is `s_sin_lon[0..21]`; the sine writes then land in
  `s_scatter`, and the scatter loop cascades into `s_wave_cos_lon`. ASan reports
  **nothing** -- the pool is 3,034 floats, so `n` would have to grow ~6.7x to
  escape it. Nothing in this project's toolchain would catch this; it would
  present as an orb that draws wrongly.

Fixed with `if (n + lon_count > ORB_VOICE_DOTS) return false;` in both loops
(`ORB_WAVE_DOTS` for the wave). `orb_init()` already returns `bool`, `face_orb`'s
`init()` turns false into `ESP_ERR_NO_MEM`, and `select_face()` logs the face by
name -- so the boot fails loudly and the device continues headless. No
`ESP_LOGE`, because this file stays free of ESP-IDF headers so `host/run.sh` can
compile it.

Verified: both guards fire when their density is bumped, the shipped values still
return true, and `host/run.sh` parity is unchanged at 0.0043 px worst deviation.

### I5. `audio_io_capture_start()` had no re-entry guard FIXED

`audio_io.c:577`. `dg_agent_init()` and `session_ctl_start()` both refuse a
second call; this one would have created a second `audio_cap` task at priority 7,
and every "one writer only, so the read-modify-write needs no lock" argument in
`ui.c` and `face_spectrum.c` would stop holding at once -- `s_level_peak`'s
peak-hold, the FFT window's hop fill, and the seqlock's publish counter all
assume a single producer.

Fixed by keeping the task handle (it was being discarded) and refusing a second
call with `ESP_ERR_INVALID_STATE`. Defence for a caller that does not exist yet,
but `audio_io.h` already stated "the task cannot be created twice" as a property
of the module, and now something enforces it.

### I6. `send_settings()` kept 2,880 B of catalog buffers in one frame FIXED

`faces[512] + face_desc[700] + catalog[768] + description[900]` all coexisted in
a single frame on the 6144-byte WebSocket task, above cJSON's own recursion.

The comment beside `set_color` recorded that adding a *third* such pair tripped
the stack canary and put the device in a boot loop -- recovery needed BOOT held
while RESET was tapped, because the board rebooted faster than esptool could
sync -- and then solved it only for `set_color`. The two older pairs were left in
place, so the trap the comment warned about was still armed.

Both now use the one-buffer PSRAM pattern `set_color` introduced: the prefix is
written with `snprintf`, the catalog appended into the tail of the same
allocation, and the string freed immediately since cJSON copies it. The wire text
is byte-identical -- including the trailing full stop the single `snprintf` used
to supply, which is re-appended explicitly so this is a move off the stack and
not a change to what the model reads.

Measured with `.claude/skills/esp-stack-budget/stackcheck.py` either side:

| | before | after |
|---|---|---|
| `send_settings` frame | 2,944 B | no such symbol -- inlined |
| `on_ws_event` frame (its caller) | 192 B | 192 B |
| deepest point on that path | **3,136 B of 6,144** (51%) | **192 B** (3%) |

With the buffers gone the function is small enough that GCC inlines it into
`on_ws_event` outright, so there is no `send_settings` frame in the image at all.
`stackcheck.py --fail-over 3500` passes, and the largest first-party frame is now
`blit_dot` at 1,728 B.

Buffer sizes are set for growth rather than for today, since PSRAM is not the
constrained resource: 1024 B for faces (377 B in use) and 2048 B for voices
(705 B in use at thirteen featured). `voices.c` keeps the other twenty-three
catalog entries specifically so widening the offer is "a one-flag change" -- all
thirty-six would be 1,743 B, so that flag can now be flipped without landing on
`voices_describe()`'s truncation path.


---

### I7. The oversized-JSON drop path corrupted the *following* message FIXED

`dg_agent.c`. On overflow the buffer was reset and the slice dropped, but the
remaining CONT slices of that same message kept accumulating from offset 0, and
the `fin` slice then parsed a fragment tail -- so an oversized message was
logged as "unparseable message" against a message that was perfectly well formed.
The drop lasted one slice when it needed to last one message.

Fixed with an `s_json_dropping` flag: set on overflow, and cleared at `fin`
rather than at the next message's first slice, so a following message that never
presents an offset-0 TEXT slice still starts clean. Also cleared on socket close
and in `dg_agent_start()`, alongside the existing `s_json_len` resets.

### I8. The spectrum point-sampled 24 of 512 FFT bins FIXED

`face_spectrum.c`:

```c
int fft_idx = i * (FFT_N / 2) / STRIPE_COUNT;
display_spectrum[i] = ...s_spectrum[fft_idx];
```

488 of the 512 bins were computed and thrown away. Bands are 21.33 bins wide
(333 Hz at 1024/16 kHz), so the taps sat 333 Hz apart and anything between two of
them was invisible however loud it was. Band 0 sampled bin 0, which is DC.

*(Correcting this entry as first written: it claimed linear spacing puts "about 20
of the 24 bands above 4 kHz". It is exactly 12 of 24 -- linear spacing puts half
above the midpoint by definition. The criticism that survives is narrower: only
the first three bands cover the sub-1 kHz region where speech has most of its
energy.)*

Fixed by taking the **max** over each band's bin range, and skipping bin 0.

**Max rather than the mean the plan said**, and the difference is not cosmetic.
`s_spectrum` is in dB, so a mean is a geometric mean of magnitudes: one
full-scale bin among twenty at the floor averages to **-85.7 dB**, dimmer than
the single-bin reading it replaced -- it would have made the display *less*
responsive to exactly the tonal content this finding is about. It would also drag
every bar down and put `DB_MIN` and the perceptual curve back in play, which is
the retuning cost the log-spacing option was rejected for. Max keeps the value
meaning "the level of one bin", so the scale, `DB_MIN` and the `sqrt` curve all
still mean what they meant. The approved option's text was "take max or mean over
each band's bin range", so this is inside it, but the wording deserves the note.

Log-spaced band edges remain the larger visual win and remain undone: they need
`DB_MIN` and the curve re-tuned, and would diverge from `spec_analyzer_radial`,
which this face was lifted from unchanged.

**Not yet seen on the panel.** This is the one change in the set whose effect is
visual, and it has been verified only by building. Expect livelier bars,
especially at the bass end, and band 0 no longer showing DC.

### I9. `orb_raster.c`'s performance claim was ~11x optimistic FIXED

`orb_raster.c` stated the blitter was "about 11,000 pixel blends, roughly
1.5 ms". Measured across 5,000 `face_orb` log lines from the captures in `/tmp`:

| | min | median | p90 | max |
|---|---|---|---|---|
| `raster` us | 9,847 | **16,176** | 16,690 | 22,932 |
| `geometry` us | 1,762 | 2,249 | -- | 13,110 |

It never once came in under 9.8 ms. From 12,339 orb `TLM` lines: frame period a
median **40 ms** (fps 25.0), `draw` avg a median **18.5 ms**. So the raster is
~40% of the frame period and ~88% of the draw callback.

That mattered because the paragraph below the claim was the stated justification
for hand-rolling the blitter, keeping the per-pixel path "obvious", and rejecting
the sprite atlas the plan called for -- an argument that drawing is small next to
the panel's own cost. It also borrowed the *spectrum* face's budget (~16 ms draw
in a ~55 ms frame); this face's frame is 40 ms, leaving ~21 ms behind the draw
rather than ~40.

The comment now carries the measured numbers, notes that the estimate looks like
a mean 5x5 footprint that accounts for neither `blit_dot`'s two passes per box
nor the `sqrtf` per annulus pixel, and that it predates `SPRITE_MAX` going
14 -> 20. It states plainly what is and is not undermined: direct-to-canvas still
beats the pipeline it replaced, but "too cheap to be worth improving" is no
longer supported, and anyone who wants that conclusion back has numbers to beat.

No code changed *in that pass*. The device was working normally throughout; the
defect was a comment reporting a budget nobody could reproduce.

### I9a. The atlas question I9 reopened, answered ANSWERED

The sprite atlas the plan originally called for was rejected partly on the
strength of the 1.5 ms figure. With the real number an order of magnitude higher,
the question had to be asked again rather than inherited.

**Still no, for a reason the first pass never reached.** Counted over the 12,343
real dots in `host/port.tsv`, a shell frame is ~30k box pixels, ~5.7k needing the
`sqrtf`, and ~10k that actually blend. So the old "about 11,000 pixel blends" was
roughly *right* -- what is wrong by an order of magnitude is the cost of each one.
The canvas is 434 kB in PSRAM, the blends are scattered read-modify-writes into
it, and `clear_box()` moves the same boxes again beforehand; at ~5k distinct cache
lines per pass, that is the shape of the bill. **An atlas removes coverage
arithmetic and adds atlas reads. It cannot remove a single canvas write.**

**What did help: the bounding boxes were 58% larger than they had to be.**
Coverage is zero at `d >= r + 0.5` and pixels sample at their centre, so a pixel
can only carry ink when `x - r - 1 <= ix <= x + r`. The box was
`floor(x-r-1)..ceil(x+r+1)` -- up to two whole rows and columns of
guaranteed-zero pixels on every dot.

Verified by extracting the shipped `blit_dot` verbatim into a host harness,
building both variants, and rendering all 12,343 dots through each:

```
  box pixels    : old 835472 -> new 348422  (58% fewer)
  canvas differs: 0 of 217156 pixels
  RESULT: byte-identical canvas
```

Byte-identical because every dropped pixel had coverage exactly zero, so it
contributed nothing to `sum` and the area normalisation is unchanged too. It pays
three times: pass one does 58% less arithmetic, pass two iterates 58% fewer
pixels for the same blends, and the stored box is what `clear_box()` erases next
frame, so the clear moves 58% less PSRAM. A marginal bonus at high amplitude --
the clip threshold moves from r > 8.5 to r > 9, so slightly fewer large dots get
truncated by `SPRITE_MAX`.

**Measured on the device, and the estimate held.** `ORB_RASTER_PHASE_TIMING`
splits the frame into its clear and its blit; 407 samples across three behaviours:

| state | boxpx | clear | blit | raster | was | saved |
|---|---|---|---|---|---|---|
| idle (n=373) | 11,575 | 1,973 | 9,503 | **11,476** | 16,165 | 29% |
| listening (n=9) | 12,076 | 2,053 | 9,956 | **12,010** | 17,071 | 30% |
| speaking (n=25) | 16,757 | 2,511 | 12,726 | **15,237** | 19,341 | 21% |

Box pixels fell 60% and the frame only 29% -- and that gap is what separates the
two costs, because the tightening provably left the *blended* pixel count alone.
Solving before-against-after gives **~270 ns per box pixel** and **~939 ns per
blended pixel**, the latter 225 cycles at 240 MHz for a two-byte
read-modify-write. **73% of the frame is that one term**, and no atlas touches it.

**A second, accidental confirmation.** The clear is pure `memset` with no
arithmetic in it at all, and its cost per pixel *fell* from 170 ns to 150 ns as
speech grew the boxes. Arithmetic per pixel cannot get cheaper when there is more
of it; only locality can. That is a pure-memory signature, and it arrived from an
experiment designed to measure something else.

**The prediction, and why it missed.** Before measuring I predicted speaking at
12,730 us; it came in at 15,237, +20%. Most of the gap was a wrong input -- the
live session ran hotter than the host test frame (16,757 box pixels against the
12,631 assumed). Correcting for that the model still over-predicts by ~11%,
because the per-pixel costs are not constants: they improve with size, exactly as
the clear's 170→150 ns shows. Being wrong in that direction is more evidence for
the conclusion rather than less.

Worth recording that the experiment I *designed* for this was inconclusive by its
own pre-registered criterion -- clear and blit came out 4.8x apart per box pixel,
which I had said in advance would be ambiguous rather than decisive. The answer
came from the before/after pair instead.


---

## Nits, all fixed

- `audio_io.c` -- `int16_t al = (l < 0) ? -l : l;` gave `-32768` for `INT16_MIN`,
  so a full-scale negative sample read as the quietest possible one. The peak
  accumulators are `int32_t` now. Diagnostic meter only.
- `audio_io.c` -- both task entry points leaked the buffer that succeeded when its
  partner failed, and blamed "internal RAM" when the failing request was for
  PSRAM. Both now free what they got and name the right pool with its size.
- `face_spectrum.c` -- `init()` returned `ESP_ERR_NO_MEM` leaving up to six
  non-NULL dangling pointers, and `select_face()` leaves the face un-ready so a
  second attempt would allocate over them. It now frees and nulls all six.
- `face_orb.c` -- "The rasteriser buckets by row band" named the wrong file: the
  bucketing is `cmp_draw_order()` in `orb_geometry.c`, which `host/README.md`
  describes as the geometry's own sort order. The comment now says what is
  actually true -- both halves arrive sorted the same way, which is not the same
  as their concatenation being sorted -- and says to measure the qsort before
  adding one, since the raster beneath it is the frame's most expensive part.
- `face_orb.c` -- the geometry/raster log ran on the LVGL task every 60 frames,
  inside the window `tlm_accumulate_frame()` averages, spoiling one frame every
  2.4 s. It is behind `ORB_LOG_TIMINGS`, off by default, along with its three
  `esp_timer_get_time()` calls. Kept rather than deleted because it is where I9's
  numbers came from.
- `ui.c` -- the lazy `a1`/`a2` init was reachable from both audio tasks, so one
  could see `a1` set and `a2` still zero and collapse the mid and high bands into
  the input for a block. They are file-scope now, resolved in `build_ui()` before
  any tap is attached.
- `dg_agent.c` -- `AUDIO_FRAME_BYTES 2560` duplicated `CAPTURE_FRAMES *
  sizeof(int16_t)` with only a comment tying them together.
  `AUDIO_IO_CAPTURE_FRAMES` / `_BYTES` are published from `audio_io.h` and both
  sites derive from them.
- `dg_agent.c` -- "543 B in use at fourteen colours"; the table has thirteen.

## The vendored transport, reviewed separately

`components/tcp_transport/transport_ws.c` is ESP-IDF 5.5.5's copy plus two local
patches. Reviewed by diffing against upstream -- three hunks at the time -- and
tracing every load-bearing assumption through both the transport and
`esp_websocket_client`.

### Both patches are correct, for stronger reasons than they claimed

- **The `FIN + BINARY` guard is airtight.** `esp_websocket_client_send_bin` routes
  through `send_with_opcode`, which ORs in FIN, so single-frame audio is
  droppable. For a *fragmented* send, `send_with_exact_opcode` clears FIN on the
  first chunk, sends middles as CONT, and gives the last chunk `0 | FIN` = `0x80`
  -- whose low nibble is CONT, not BINARY. So **no chunk of a fragmented message
  can ever be dropped**. `send_bin_partial` never sets FIN at all. TEXT is `0x01`.
  PING/CLOSE are excluded by `len > 0`, PONG by opcode.
- **`return len` matches the success contract.** `_ws_write` returns the payload
  write's return value on success, so the drop is indistinguishable to the caller.
- **`dg_agent.c`'s 401 reasoning is exactly right.** `http_status_code` has one
  assignment site, reached only after the response header parses, and is never
  cleared between attempts -- the truncated-header path returns without touching
  it. "401 means a 401 was really read, absence proves nothing" holds.

### T1. The drop was invisible to the telemetry that would show it FIXED

The counter was a function-local `static`, and `TLM`'s `updrop=` is the *ring
buffer* counter. Two different loss mechanisms, one on the line. Observed during
this branch's testing: the log showed `send queue full ... (100 since boot)` while
`updrop=` read `0`.

Fixed by promoting the counter to file scope with an atomic increment
(`__atomic_add_fetch` returns the new value, so the existing burst-logging reads
it without a second load), publishing one accessor from a new
`components/tcp_transport/include/transport_ws_local.h`, forwarding it through
`dg_agent_transport_dropped()`, and adding `txdrop=` to the TLM line beside
`updrop=`. Read as a pair: `updrop` says the queue never drained, `txdrop` says it
drained into a socket that would not take it.

The header is named `transport_ws_local_*` rather than `esp_transport_ws_*` so a
call site that outlives the patches is obvious at the point of use.

### T2. Nothing recorded which IDF the file was forked from FIXED

It includes the private `esp_transport_internal.h`, and the CMakeLists takes the
sibling sources *and* both include directories from `$ENV{IDF_PATH}` -- so a file
pinned at 5.5.5 compiled against whatever IDF was exported.

Two guards, because they catch different things:

- an `#error` in `transport_ws.c` gated on MAJOR.MINOR, so 5.5.x passes and 5.6+
  stops the build naming the re-derivation steps. Verified to fire at 5.6, 6.0 and
  5.4, and stay silent at 5.5.
- `local.patch` plus `check-patch.sh`, which regenerates the diff against
  `$IDF_PATH` and byte-compares. This catches upstream editing the file *in place*
  within 5.5.x, which the version guard cannot see. The diff is generated with
  explicit `--label`s so it carries no mtimes and is byte-stable. Verified against
  all three paths: clean pass, detected drift, and `--update` re-baselining.

### Nits, fixed

- The counter's non-atomic increment, folded into T1.
- The `Host` patch tests the *port*, but the default port belongs to the
  *scheme* -- `ws://h:443` and `wss://h:80` would get a technically wrong header.
  Comment only: `ws_connect()` receives just host and port and `transport_ws_t`
  carries no scheme flag, so fixing it means plumbing a field for a combination
  nothing deploys.
- The CMakeLists had silently dropped upstream's
  `if(${IDF_TARGET} STREQUAL "linux")` esp_timer linkage. Restored verbatim, so
  the override is a true single-file diff again.

### Recorded, not changed

- **The infinite-timeout capping.** A caller passing `timeout_ms < 0` means "block
  until writable" and now gets a 150 ms poll for self-contained binary. No caller
  here does that, so changing it back would be a change for a hypothetical. It has
  a comment.
- **An upstream bug.** `_ws_write` masks the caller's buffer in place and, on a
  header-write failure, returns `-1` without reverting it, leaving the caller's
  data XORed. Unreachable here only because `esp_websocket_client` always
  `memcpy`s into its own `tx_buffer` first -- which is also why passing a `const`
  rodata string like `KEEPALIVE` is safe. Not fixed: diverging further from
  upstream to fix a bug we cannot hit works against the single-file-diff
  discipline that makes this vendoring maintainable.

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
