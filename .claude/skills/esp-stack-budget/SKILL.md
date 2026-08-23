---
name: esp-stack-budget
description: Check stack frame growth in ESP-IDF / FreeRTOS firmware before flashing. Use whenever adding or enlarging local buffers, arrays, or structs in firmware code; when a function that builds strings or JSON gains another block; when changing a task's stack size or adding an xTaskCreate; before flashing after any such change; or when diagnosing a device that reboot-loops, dies at a specific point in startup, or logs "Stack canary watchpoint triggered". Also use when a change has been checked with idf.py size or nm and declared safe on RAM, since those measure static RAM and cannot see stack at all.
---

# Stack budget for ESP-IDF firmware

Static-RAM tools and stack are **different measurements**, and confusing them
bricked this project's board once already. `idf.py size` and `nm` on `.bss`/
`.data` will report a change as free while it silently overflows a task stack.
Nothing fails at build time; the device crashes at runtime on one code path.

## Run the check

```bash
source /path/to/esp-idf/export.sh          # toolchain must be on PATH
.claude/skills/esp-stack-budget/stackcheck.py --top 10
```

It disassembles the built ELF, reads each function's frame from its prologue
(Xtensa `entry a1, 0xNNN`; RISC-V / call0 `addi sp, sp, -N`), ranks them, and
lists the task stacks declared in first-party source so you can compare.

Defaults to first-party code only — the largest frame in the whole image usually
belongs to a framework or managed component, which makes an unfiltered ranking
useless as a gate. `--all` includes everything.

As a gate, e.g. in CI or before a flash:

```bash
stackcheck.py --fail-over 3500     # exit 1 if any first-party frame exceeds it
```

## Reading the result

A frame must fit its task's stack **and leave room for everything it calls**.
Past roughly 40% of the stack it runs on, look at what sits beneath it — cJSON
recursion, TLS, `snprintf`, and websocket sends are all hungry, and none of it
shows in the frame itself.

To find which task a function runs on, follow its callers back to an
`xTaskCreate` or a component's `task_stack` config. The script prints the
declared stacks; the matching is yours to do.

## What went wrong here, concretely

`send_settings()` in `main/dg_agent.c` builds the agent's function schema on the
websocket task, which has `task_stack = 6144`. The established pattern gave every
declared function **two** stack buffers — a catalog and a description:

| function | catalog | description | total |
|---|---|---|---|
| `set_face` | 512 | 700 | 1,212 |
| `set_voice` | 768 | 900 | 1,668 |
| `set_color` | 512 | 768 | 1,280 |

Two fit. Adding a third put the frame at **4,208 B of 6,144 (68%)**, and cJSON's
recursion beneath it tripped the canary on the first session:

```
Guru Meditation Error: Core 1 panic'ed (Unhandled debug exception).
Debug exception reason: Stack canary watchpoint triggered (websocket_task)
```

Three things worth carrying forward:

- **The pattern was the trap, not the change.** 2,880 of the pre-change 2,928-byte
  frame was already those buffers — 98% of it. The cost was O(n) in declared
  functions with no budget recorded anywhere, so it would have failed for whoever
  added the third function, whatever it was.
- **`-Werror=format-truncation` pushed the wrong way.** A truncation error on
  `snprintf` invites a *bigger* buffer. On a constrained task that makes the real
  problem worse. Move the buffer off the stack instead of growing it.
- **Recovery needed physical access.** The board rebooted faster than esptool
  could sync — 30 connect attempts, both `usb_reset` and `default_reset`, all
  failed. It took holding BOOT and tapping RESET. Budget for that before flashing
  firmware you cannot inspect on a board you cannot reach.

## The fix that generalises

Transient buffers over a few hundred bytes go on the heap, in PSRAM where the
target has it, not on the stack:

```c
enum { DESC_LEN = 1024 };
char *desc = heap_caps_malloc(DESC_LEN, MALLOC_CAP_SPIRAM);
const char *use = "short static fallback";   /* allocation can fail */
if (desc != NULL) {
    int n = snprintf(desc, DESC_LEN, "prefix... ");
    if (n > 0 && (size_t)n < DESC_LEN) {
        describe_into(desc + n, DESC_LEN - (size_t)n);   /* append, one buffer */
        use = desc;
    }
}
cJSON_AddStringToObject(obj, "description", use);
free(desc);   /* cJSON copied it; free(NULL) is fine */
```

Writing the prefix and appending into its tail uses **one** allocation where the
stack pattern used two buffers. That change took `send_settings()` back to
2,928 B, and the catalog then grew from 3 to 13 entries with the frame unmoved —
which is the property to aim for. Growth should not touch the stack at all.

## Checklist

- [ ] Added or enlarged a local buffer, array, or struct? Run the script.
- [ ] Over a few hundred bytes and transient? Heap or PSRAM, not the stack.
- [ ] Frame past ~40% of its task's stack? Check what it calls.
- [ ] Fixed a truncation warning by enlarging a buffer? Re-run the script.
- [ ] Verified only with `idf.py size` or `nm`? That says nothing about stack.
