---
name: esp-ram-board
description: Generate a visual memory board for an ESP-IDF build — internal RAM headroom, section composition, and which archives hold the RAM — as a publishable HTML artifact. Use whenever someone asks what the memory footprint of a build is, how much RAM or flash is left, where the memory went, which component is the biggest, whether a new feature or component will fit, or asks to see, chart, or visualize the memory usage. Also use when comparing footprint before and after a change, or when reporting build size to someone who is not going to read a terminal table.
---

# Memory board for ESP-IDF builds

`idf.py size` prints the right numbers in a shape nobody can hold in their head:
five nested tables where the one figure that matters — internal RAM left — sits
in the middle of a column. This skill renders the same data as a board, then
publishes it so it can be shared.

## Generate it

```bash
source /path/to/esp-idf/export.sh          # toolchain must be on PATH
.claude/skills/esp-ram-board/ramboard.py -o /tmp/ram-board.html
```

It reads the build's `.map` through `esp_idf_size`, the app partition size from
the generated partition table, and the real `.bin` size, then writes one
self-contained HTML file and prints the headline figures to the terminal.

```
--build-dir DIR   build directory (default: build)
--top N           archives charted individually (default: 12)
--title TEXT      page title

--rt-free-min N   lowest free internal heap observed, bytes
--rt-free-avg N   mean free internal heap, bytes
--rt-largest N    largest free internal block, bytes
--rt-samples N    how many samples that came from
--rt-note TEXT    one line on what the device was doing
```

Pass the `--rt-*` values and the board gains a **Measured on device** card that
puts link-time free, runtime mean, runtime low-water, and largest contiguous
block on one scale. That card is the answer to "how much is left"; everything
else on the page is the explanation.

Then publish the file with the Artifact tool and give the user the link. Read
the printed numbers yourself and say what they mean in your reply — the board is
the evidence, not the answer.

## What it shows, in the order it matters

1. **Internal RAM free** as the headline, with flash as the deliberately smaller
   tile. On any board with a multi-megabyte app partition, that asymmetry is the
   finding: flash is not the constraint, RAM is.
2. **Section composition** — a stacked bar over the full RAM region, with free
   space as a segment rather than an absence, so headroom is a shape and not a
   subtraction the reader has to do.
3. **Top archives by RAM**, the rest folded into one neutral bar. Framework and
   radio components normally dominate; first-party code is a small slice.
4. **The caveat**, which is the part that keeps the board honest — see below.

Segments and bars carry hover tooltips with exact byte counts, and the archive
chart has a table view. Both themes are designed; the blue ordinal ramp is
validated for light and dark surfaces.

## Say what it cannot see

Everything on the board is **link time**. The free figure is what the image
leaves before a single task starts. Task stacks, WiFi and driver buffers, LVGL
draw buffers, and every heap allocation come out of it afterward.

TLS is usually the sharpest edge: `MBEDTLS_SSL_IN_CONTENT_LEN` and its out
counterpart are held for the life of a connection, plus a handshake peak while
certificates are parsed — tens of kilobytes that no static tool reports. Check
those two sdkconfig values when the board looks comfortable but the device does
not.

Never present the headroom number as available memory. Pair it with a runtime
reading — free internal heap and largest free block, which is where
fragmentation shows up — and if there is no live capture, say so plainly.

**In this project** that runtime reading is the `TLM` log line from `main.c`:
`int=` free internal, `intmax=` largest free block. Capturing it means flashing
and reading the serial port, which needs the board attached — a human-in-the-loop
step, not something to claim without doing it. With the board connected:

```bash
idf.py -p /dev/cu.usbmodem101 flash
stty -f /dev/cu.usbmodem101 raw -echo
cat /dev/cu.usbmodem101 > /tmp/boot.log &        # only one reader per port
# let it run through a real session, then:
grep -ao "int=[0-9]* intmax=[0-9]*" /tmp/boot.log |   awk -F'[= ]' '{if($2<mn||NR==1)mn=$2; s+=$2; m=$4}                 END{printf "min=%d avg=%d largest=%d n=%d\n", mn, s/NR, m, NR}'
```

Kill the `cat` before flashing again — the port takes one reader.

A capture is a **sample, not a bound**. A path that never ran cannot show up in
it, so the low-water mark is the worst you have *seen*, not the worst that
exists. Say it that way.

## Related

- **`esp-stack-budget`** is the other half and answers a different question.
  This skill measures *static* RAM; that one measures *stack*. A change can look
  free here and still overflow a task stack. If the question is "will this
  buffer fit," it belongs to that skill, not this one.

## Checklist

- [ ] Sourced `export.sh` before running — the script needs `esp_idf_size` and
      `IDF_PATH`.
- [ ] Built first? The script reads the map file, not the source.
- [ ] Published the HTML as an Artifact and handed over the link.
- [ ] Stated the headline numbers in the reply, not just the link.
- [ ] Said out loud that these are link-time figures.
- [ ] Attributed anything you inferred rather than measured — the flash tenant
      note names the largest archive in the ELF, which is not the same as
      proving what is inside it.
