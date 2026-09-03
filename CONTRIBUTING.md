# Contributing

Bug reports and pull requests are welcome. This document covers only the things
you cannot guess from reading the code — everything else is ordinary ESP-IDF
work.

## The toolchain is pinned, exactly

**ESP-IDF v5.5.5.** Not "5.5 or later":
`components/tcp_transport/transport_ws.c` is a fork of one upstream file and
carries an `#error` that fails the build on a MAJOR.MINOR change, and
`components/tcp_transport/baseline.sha256` pins the exact upstream file it was
forked from. CI builds in `espressif/idf:v5.5.5`, and that is the version any
finding should be reported against.

`dependencies.lock` is committed so component versions are reproducible. If your
change needs a different component version, commit the regenerated lock file with
it and say why in the PR.

## Never commit a `sdkconfig`

`sdkconfig` and `sdkconfig.old` are gitignored, and that is load-bearing:
`CONFIG_DEEPGRAM_API_KEY`, `CONFIG_WIFI_SSID` and `CONFIG_WIFI_PASSWORD` are
first-boot seeds, so a `sdkconfig` you have used contains your real API key and
your real Wi-Fi password. **`sdkconfig.defaults` is the only tracked
configuration**, it seeds nothing secret, and a change to build defaults belongs
there. Same reason: do not attach a `.bin` you built with a seed set to an issue.
See [SECURITY.md](SECURITY.md).

## After an ESP-IDF bump

The vendored WebSocket transport needs re-doing by hand, in this order:

1. `./components/tcp_transport/check-patch.sh` — it will report which side moved.
2. Re-apply the hunks recorded in `components/tcp_transport/local.patch` to the
   new upstream `transport_ws.c`. The patches are labelled `LOCAL PATCH 1` and
   `LOCAL PATCH 2` in the file itself, with the reasoning inline.
3. Update the `#error` version guard at the top of `transport_ws.c`.
4. `./components/tcp_transport/check-patch.sh --update`, then **read the diff
   before committing it**. That script's baseline *is* the record of what was
   changed, so an accidental `--update` silently launders a real drift into the
   new normal.

The check compares SHA-256 digests rather than diff text, because Apple/FreeBSD
`diff` and GNU `diffutils` emit different (both correct) unified diffs for the
same pair of files. `local.patch` remains the human-readable record of *what*
changed; `baseline.sha256` is what decides pass or fail.

## What has to pass before you push

All of it runs on a laptop with no board attached, and CI runs the first two:

```bash
./host/run.sh        # orb geometry: the C port vs. the upstream TypeScript
./host/prompt.sh     # assembles the system prompt, as the device sends it
./host/store.sh      # the conversation store, against a fake NOR flash
./components/tcp_transport/check-patch.sh
```

- **Touching `main/orb_geometry.c` means `host/run.sh` must still pass.** That
  file is deliberately free of LVGL and ESP-IDF so it can compile on the host;
  keep it that way. Tolerance is 0.02 px against the reference.
- **Touching `main/history_store.c` means `host/store.sh` must still pass.** It
  compiles the real module against a fake NOR flash that clears bits rather than
  copying them and can cut a write short at any byte, which is the only way to
  test the claim the module actually makes: that a record is either complete or
  invisible. A device cannot demonstrate that on cue.
- **Touching `main/prompt/*.md` or `main/agent_prompt.c`** — diff the assembled
  prompt before and after with `host/prompt.sh`, and check both forms
  (`--resumed` too). Prompt changes are behaviour changes.
- **Adding or enlarging locals, buffers or a task's stack** — measure the stack
  frame before flashing. `idf.py size` reports static RAM and cannot see stack at
  all, so it is not evidence here.

## House rules the layout defends

- **Nothing outside `main/ui.c` and the faces may call `lv_*`.** That is why the
  catalogs (`voices.c`, `faces.c`, `orb_colors.c`) are separate from the code
  that draws them — `dg_agent.c` needs them to build tool schemas and must not
  drag in LVGL. See [docs/architecture.md](docs/architecture.md).
- A colour stays a plain `0xRRGGBB` until `main/orb_raster.c`.
- The persona is [one markdown file per block](main/prompt/), not a Kconfig
  string. Edit the blocks.

## Style

4-space indent, ~80 columns, `/* */` for block comments, `snake_case`. There is
an `.editorconfig` and a `.clang-format`; match the file you are editing.

Comments in this codebase explain **why**, not what — several of them are the only
record of a measurement that cost a day, and a few say explicitly "read this
before removing this gate again." Please keep that habit, and if you remove such a
comment, say in the PR what re-measurement replaced it.

## Pull requests

- One concern per PR.
- Say how you verified it. "Builds" is not verification for anything touching
  audio, timing or RAM; the telemetry line (`TLM …`) is the instrument, and a
  before/after pair of it is the ideal evidence.
- If the change is a measurement that overturns an earlier one, write it up in
  `docs/notes/` the way the existing notes are written — the finding is often
  worth more than the diff.
