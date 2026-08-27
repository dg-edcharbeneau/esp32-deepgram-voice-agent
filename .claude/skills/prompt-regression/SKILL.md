---
name: prompt-regression
description: Verify what the device will actually say before flashing it, by diffing the assembled system prompt against git. Use whenever editing anything in main/prompt/, changing agent_prompt.c or its block table, adding or removing a prompt block, changing a {{placeholder}}, or touching main/CMakeLists.txt's EMBED_TXTFILES. Use before flashing after any such change. Use when a firmware change is claimed to be behaviour-neutral or a pure refactor — removing a Kconfig gate, unwrapping an #if, reordering code — since the prompt is where "neutral" quietly stops being true. Also use when the device sounds subtly wrong, greets when it should not, or asserts something about its own build that is not so.
---

# Prompt regression for the Deepgram agent

The persona is **~12 kB of the device's behaviour with no compiler behind it.**
A dropped block, a gate that flipped, or a `{{placeholder}}` that stopped
expanding all build cleanly, flash cleanly, and produce a device that talks
slightly wrong. There is no error — just a worse conversation, discovered later
and blamed on the model.

It is also the cheapest thing in this project to verify. `main/agent_prompt.c`
compiles on the host against the stubs in `host/prompt_stubs/`, so the exact
string the device would send can be printed in about a second, with no device and
no firmware build.

## Run the check

```bash
.claude/skills/prompt-regression/promptdiff.sh          # vs HEAD
.claude/skills/prompt-regression/promptdiff.sh main     # vs another ref
```

Exit 0 identical, 1 changed, 2 could not run — so it gates.

**The baseline comes from git, not from having remembered to capture one.** That
is the whole reason the script exists: the manual version of this check is
`./host/prompt.sh > before.txt` *before* you start editing, which is the step
that gets skipped, because you rarely know at the first edit that the prompt is
in scope.

To see the prompt itself rather than a diff:

```bash
./host/prompt.sh              # what this working tree would send
./host/prompt.sh --resumed    # what a session reopened by a voice change sends
```

## Reading the result

```
ref HEAD   11967 bytes from 9 blocks (16090 allocated)
worktree   11967 bytes from 9 blocks (16090 allocated)
```

**The block count is the load-bearing number.** Bytes drifting by a few is an
edited sentence; the count dropping means a block left the prompt entirely, which
is the failure that looks like nothing. Nine is current.

Then judge the diff by intent:

- **Refactor, or a change you believe is behaviour-neutral** — the diff must be
  **empty**. Anything it shows is a bug you have not found yet.
- **Deliberate prompt edit** — the diff *is* the artifact to review. Read it as
  the model will: it arrives with no formatting, so a heading that lost its
  blank line above it merges into the paragraph before it.

## Three ways this breaks that are not the prompt's fault

- **Two lists must agree.** `EMBED_TXTFILES` in `main/CMakeLists.txt` is what the
  device embeds; `host/prompt_blobs.py` **globs `main/prompt/` instead**. So a
  new `.md` reaches the host harness automatically but not the firmware, and
  passing `promptdiff.sh` proves nothing about the device until CMakeLists names
  the file too. The C table in `agent_prompt.c` is a third list, and it is the
  one that decides order.
- **A new Kconfig gate needs a host default.** `host/prompt_stubs/sdkconfig.h`
  is where it goes. Without it the host build fails on an undeclared
  `CONFIG_`, which is a different failure from the device's and easy to misread
  as the harness being broken.
- **The byte/block summary goes to stderr, the prompt to stdout.** Capturing
  stderr into one side of a comparison and not the other invents a difference
  that does not exist. `promptdiff.sh` redirects both sides identically; hand-run
  comparisons should too.

## What went wrong here, concretely

Retiring three expired menuconfig options — the unbuildable Nova-3 + Aura choice,
`MIC_GATE_WHILE_AGENT_SPEAKS`, and the one-line `DEEPGRAM_AGENT_PROMPT` override
— deleted **148 lines across 11 files**, including two `#if` gates *inside the
prompt's block table* and one whole block file (`prompt/barge-in.md`).

Byte-identical output, 11,967 bytes from 9 blocks, was what turned "this should
not change behaviour" into a checked claim. Nothing else in the change could have
demonstrated that: the firmware built clean before and after, and `idf.py size`
cannot see a persona.

Two traps met on the way, both worth avoiding rather than rediscovering:

- **The phantom diff.** The first comparison "failed" on a one-line difference
  that was the stderr summary landing in the baseline only. Two minutes spent
  suspecting a real regression that was never there.
- **Grep for the symbol, not just the code.** Removing a `CONFIG_` gate left its
  name in three *comments* — `Kconfig.projbuild`'s help for a neighbouring
  option, `agent_prompt.h`, and `ui.c` — each describing a switch that no longer
  exists. `grep -rn <SYMBOL> main/ host/` after the edit, and expect prose hits,
  not just `#if` hits.

## What this cannot check

The harness sees the prompt. It does not see the rest of what the model is told,
so these still need the board and, in two cases, your voice:

- **The function schemas** are built in `dg_agent.c`'s `send_settings()`, not in
  the prompt. Unwrapping an `#if` around `set_voice` is invisible here — say
  *"change your voice to Hannah"*, then *"reset your voice"*.
- **`{{placeholder}}` values at runtime** come from NVS, so the host prints the
  factory defaults. A saved voice or agent name will differ on a used device.
- **That `Settings` was accepted at all.** The greeting speaking is the proof;
  a malformed `Settings` is silent on this path.
- **Stack cost.** `agent_prompt_build()` is deliberately flat, but
  `send_settings()` is not — see `.claude/skills/esp-stack-budget/`.

## Checklist

- [ ] Touched `main/prompt/`, `agent_prompt.c`, or `EMBED_TXTFILES`? Run the script.
- [ ] Called a change behaviour-neutral? The diff must be empty, not small.
- [ ] Block count moved? Find the block before doing anything else.
- [ ] Added a `.md`? Name it in `EMBED_TXTFILES` *and* the C table, not just the directory.
- [ ] Added a `CONFIG_` gate? Give it a default in `host/prompt_stubs/sdkconfig.h`.
- [ ] Removed a `CONFIG_` symbol? Grep for it in comments and help text too.
- [ ] Changed the function schemas? The harness cannot see it — that one needs a voice.
