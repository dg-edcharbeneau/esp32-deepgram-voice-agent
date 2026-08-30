#!/usr/bin/env python3
"""Generate a self-contained HTML memory board from an ESP-IDF build.

Reads the build's map file through esp_idf_size, the app partition size from the
generated partition table, and the real .bin size, then writes one HTML file
ready to hand to the Artifact tool.

    source /path/to/esp-idf/export.sh
    .claude/skills/esp-ram-board/ramboard.py -o /tmp/ram-board.html
"""

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def die(msg):
    sys.exit(f"ramboard: {msg}")


def size_json(map_file, *extra):
    """Run esp_idf_size and return its parsed JSON."""
    cmd = [sys.executable, "-m", "esp_idf_size", "--format", "json", *extra, str(map_file)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        die(f"esp_idf_size failed ({' '.join(extra) or 'overview'}):\n{proc.stderr.strip()}")
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError:
        die(f"esp_idf_size returned no JSON. Is the toolchain sourced?\n{proc.stdout[:400]}")


def app_partition_size(build_dir):
    """Bytes in the app partition the image is flashed to, or None."""
    idf_path = os.environ.get("IDF_PATH")
    table = build_dir / "partition_table" / "partition-table.bin"
    if not idf_path or not table.is_file():
        return None
    gen = Path(idf_path) / "components" / "partition_table" / "gen_esp32part.py"
    if not gen.is_file():
        return None
    proc = subprocess.run([sys.executable, str(gen), str(table)], capture_output=True, text=True)
    if proc.returncode != 0:
        return None
    units = {"K": 1024, "M": 1024 * 1024, "": 1}
    for line in proc.stdout.splitlines():
        if line.startswith("#") or not line.strip():
            continue
        cols = [c.strip() for c in line.split(",")]
        if len(cols) < 5 or cols[1] != "app":
            continue
        m = re.fullmatch(r"(0x[0-9a-fA-F]+|\d+)([KM]?)", cols[4])
        if not m:
            continue
        base = int(m.group(1), 0)
        return base * units[m.group(2)]
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default="build", help="build directory (default: build)")
    ap.add_argument("-o", "--out", default="ram-board.html", help="output HTML path")
    ap.add_argument("--top", type=int, default=12, help="archives to chart individually (default: 12)")
    ap.add_argument("--title", default=None, help="page title (default: derived from project name)")
    args = ap.parse_args()

    build_dir = Path(args.build_dir).resolve()
    if not build_dir.is_dir():
        die(f"no build directory at {build_dir} — run idf.py build first")

    maps = sorted(build_dir.glob("*.map"))
    if not maps:
        die(f"no .map file in {build_dir} — run idf.py build first")
    map_file = maps[0]
    project = map_file.stem

    desc_file = build_dir / "project_description.json"
    target = "unknown target"
    idf_ver = ""
    if desc_file.is_file():
        desc = json.loads(desc_file.read_text())
        target = desc.get("target", target)
        idf_ver = desc.get("git_revision", "") or ""

    ov = size_json(map_file)
    archives = size_json(map_file, "--archives")

    # DIRAM vs split D/IRAM: targets differ, so take whichever the map reports.
    if ov.get("diram_total"):
        ram_label = "DIRAM"
        ram_total = ov["diram_total"]
        ram_used = ov["used_diram"]
        sections = [
            (".text", ov.get("diram_text", 0), "IRAM-resident code"),
            (".data", ov.get("diram_data", 0), "initialized variables"),
            (".bss", ov.get("diram_bss", 0), "zero-initialized"),
            (".vectors", ov.get("diram_vectors", 0), "exception vectors"),
        ]
    else:
        ram_label = "DRAM"
        ram_total = ov.get("dram_total", 0)
        ram_used = ov.get("used_dram", 0)
        sections = [
            (".data", ov.get("dram_data", 0), "initialized variables"),
            (".bss", ov.get("dram_bss", 0), "zero-initialized"),
            (".rodata", ov.get("dram_rodata", 0), "constants in RAM"),
            (".other", ov.get("dram_other", 0), "other"),
        ]
    if not ram_total:
        die("the map file reports no RAM totals — unexpected esp_idf_size output")
    sections = sorted((s for s in sections if s[1] > 0), key=lambda s: s[1], reverse=True)

    ranked = sorted(
        ((name, vals.get("ram_st_total", 0)) for name, vals in archives.items()),
        key=lambda kv: kv[1],
        reverse=True,
    )
    ranked = [(n, v) for n, v in ranked if v > 0]
    top = ranked[: args.top]
    rest_count = len(ranked) - len(top)
    rest_bytes = ram_used - sum(v for _, v in top)

    flash_ranked = sorted(
        ((name, vals.get("flash_total", 0)) for name, vals in archives.items()),
        key=lambda kv: kv[1],
        reverse=True,
    )
    flash_top = [(n, v) for n, v in flash_ranked if v > 0][:3]

    bin_file = build_dir / f"{project}.bin"
    bin_size = bin_file.stat().st_size if bin_file.is_file() else ov.get("total_size", 0)
    part_size = app_partition_size(build_dir)

    data = {
        "project": project,
        "target": target,
        "idf": idf_ver,
        "ramLabel": ram_label,
        "ramTotal": ram_total,
        "ramUsed": ram_used,
        "ramFree": ram_total - ram_used,
        "sections": [{"k": k, "v": v, "d": d} for k, v, d in sections],
        "archives": [{"k": k, "v": v} for k, v in top],
        "restCount": rest_count,
        "restBytes": rest_bytes,
        "binSize": bin_size,
        "partSize": part_size,
        "flashTop": [{"k": k, "v": v} for k, v in flash_top],
        "title": args.title or f"{ram_label} Headroom Board",
    }

    template = (HERE / "template.html").read_text()
    html = template.replace("__DATA__", json.dumps(data, indent=2))
    html = html.replace("__TITLE__", data["title"])
    out = Path(args.out).resolve()
    out.write_text(html)

    pct = ram_used / ram_total * 100
    print(f"{project} / {target}")
    print(f"  {ram_label}: {ram_used:,} used of {ram_total:,} ({pct:.1f}%) — {ram_total - ram_used:,} free")
    if part_size:
        print(f"  flash: {bin_size:,} of {part_size:,} app partition ({bin_size / part_size * 100:.1f}%)")
    print(f"  wrote {out}")
    print("\nLink-time only. Task stacks, heap, and TLS buffers come out of the free")
    print("figure at runtime — confirm on device before treating it as headroom.")


if __name__ == "__main__":
    main()
