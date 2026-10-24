#!/usr/bin/env python3
"""Turn CONFIG_UI_SCREENSHOT_TOUR captures in a serial log into PNGs.

    host/shot2png.py capture.log OUTDIR

Writes one PNG per capture, named by its tag (s0-idle-shell-1.png, ...), and
contact.png: every capture in a grid, one row per orb state. Needs nothing but
the standard library. Lines are numbered on the device, so a capture with a
line lost on the serial link is reported and skipped rather than written wrong.
"""
import base64
import os
import re
import struct
import sys
import zlib


def png(path, w, h, rgb):
    """Write w x h RGB bytes as a PNG."""
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def decode(lines, w, h):
    """RLE (uint16 count, uint16 RGB565) runs -> RGB bytes."""
    payload = b"".join(base64.b64decode(l) for l in lines)
    out = bytearray()
    for i in range(0, len(payload), 4):
        n, v = struct.unpack_from("<HH", payload, i)
        r = ((v >> 11) & 0x1F) * 255 // 31
        g = ((v >> 5) & 0x3F) * 255 // 63
        b = (v & 0x1F) * 255 // 31
        out += bytes((r, g, b)) * n
    if len(out) != w * h * 3:
        raise ValueError(f"decoded {len(out) // 3} pixels, expected {w * h}")
    return bytes(out)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    text = open(sys.argv[1], "rb").read().decode("latin-1").replace("\r", "")
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)

    shots = []   # (tag, w, h, rgb)
    for m in re.finditer(r"SHOT begin n=(\d+) w=(\d+) h=(\d+) fmt=rgb565rle tag=(\S+)\n", text):
        n, w, h, tag = int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)
        end = re.compile(rf"SHOT end n={n} lines=(\d+) runs=(\d+)").search(text, m.end())
        if end is None:
            print(f"{tag}: no end line, skipped")
            continue
        body = {}
        for lm in re.finditer(r"^SHOT (\d+) (\S+)$", text[m.end():end.start()], re.M):
            body[int(lm.group(1))] = lm.group(2)
        want = int(end.group(1))
        if sorted(body) != list(range(want)):
            print(f"{tag}: {want - len(body)} of {want} lines lost on the link, skipped")
            continue
        try:
            rgb = decode([body[i] for i in range(want)], w, h)
        except (ValueError, base64.binascii.Error, struct.error) as e:
            print(f"{tag}: {e}, skipped")
            continue
        png(os.path.join(outdir, tag + ".png"), w, h, rgb)
        shots.append((tag, w, h, rgb))
        print(f"{tag}.png")

    if not shots:
        sys.exit("no complete captures in that log")

    # Contact sheet: one row per state (the tag up to its last "-k"), its
    # captures left to right, at half size, on a dark grey ground.
    rows = {}
    for tag, w, h, rgb in shots:
        rows.setdefault(tag.rsplit("-", 1)[0], []).append((w, h, rgb))
    cell, gap = shots[0][1] // 2, 8
    ncols = max(len(r) for r in rows.values())
    W = ncols * cell + (ncols + 1) * gap
    H = len(rows) * cell + (len(rows) + 1) * gap
    sheet = bytearray(b"\x20\x20\x20" * W * H)
    for ri, (_, caps) in enumerate(rows.items()):
        for ci, (w, h, rgb) in enumerate(caps):
            ox, oy = gap + ci * (cell + gap), gap + ri * (cell + gap)
            for y in range(cell):
                src = (y * 2) * w * 3
                line = rgb[src:src + w * 3]
                half = bytearray()
                for x in range(cell):
                    half += line[x * 6:x * 6 + 3]
                d = ((oy + y) * W + ox) * 3
                sheet[d:d + cell * 3] = half
    png(os.path.join(outdir, "contact.png"), W, H, bytes(sheet))
    print(f"contact.png: {len(rows)} states x up to {ncols} captures")
    for i, name in enumerate(rows):
        print(f"  row {i + 1}: {name}")


if __name__ == "__main__":
    main()
