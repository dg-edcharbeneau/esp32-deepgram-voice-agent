#!/usr/bin/env python3
"""Report stack frame sizes from a built ESP-IDF ELF.

Static-RAM tools (idf.py size, nm on .bss/.data) cannot see stack usage. This
can. See SKILL.md for why that distinction has already cost this project a
bricked board.

  ./stackcheck.py [--top N] [--min-bytes N] [--fail-over N] [--elf PATH]

Exit 1 if --fail-over is set and exceeded, 2 on setup problems.
"""
import argparse, glob, os, re, subprocess, sys

FN    = re.compile(r"^[0-9a-f]+ <(.+)>:$")
# Xtensa windowed ABI: `entry a1, 0xNN` allocates the whole frame.
ENTRY = re.compile(r"\bentry\s+a1,\s*(0x[0-9a-f]+|\d+)")
# RISC-V, and Xtensa call0: sp decrement in the prologue.
ADDI  = re.compile(r"\baddi\s+(?:sp|a1),\s*(?:sp|a1),\s*-(\d+)")


def find_objdump(elf):
    pats = [
        "~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp-elf-objdump",
        "~/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-objdump",
    ]
    cands = []
    for p in pats:
        cands += sorted(glob.glob(os.path.expanduser(p)), reverse=True)
    for name in ("xtensa-esp32s3-elf-objdump", "xtensa-esp-elf-objdump",
                 "riscv32-esp-elf-objdump"):
        for d in os.environ.get("PATH", "").split(os.pathsep):
            c = os.path.join(d, name)
            if os.access(c, os.X_OK):
                cands.append(c)
    for c in cands:
        try:
            subprocess.run([c, "-d", elf], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, check=True, timeout=180)
            return c
        except Exception:
            continue
    return None


def frames(objdump, elf):
    out = subprocess.run([objdump, "-d", elf], capture_output=True, text=True,
                         timeout=600).stdout
    got, cur, done = {}, None, False
    for line in out.splitlines():
        m = FN.match(line)
        if m:
            cur, done = m.group(1), False
            continue
        if cur is None or done:
            continue
        m = ENTRY.search(line) or ADDI.search(line)
        if m:
            v = m.group(1)
            got[cur] = int(v, 16) if v.startswith("0x") else int(v)
            done = True
    return got


def project_symbols():
    """Symbol names defined by first-party objects, so the ranking can exclude
    framework and managed-component frames. Without this the biggest frame in the
    image belongs to somebody else's code and a --fail-over gate is unusable."""
    names = set()
    pats = ["build/esp-idf/main/**/*.obj", "build/esp-idf/components/**/*.obj"]
    objs = []
    for p in pats:
        objs += glob.glob(p, recursive=True)
    if not objs:
        return None
    nm = None
    for c in sorted(glob.glob(os.path.expanduser(
            "~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp-elf-nm")),
            reverse=True):
        if os.access(c, os.X_OK):
            nm = c
            break
    if nm is None:
        return None
    for o in objs:
        try:
            out = subprocess.run([nm, "--defined-only", o], capture_output=True,
                                 text=True, timeout=60).stdout
        except Exception:
            continue
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[-2].upper() in ("T", "W"):
                names.add(parts[-1])
    return names or None


def declared_stacks():
    """Task stacks in FIRST-PARTY source only. managed_components ships test
    apps whose numbers are noise here."""
    roots = [d for d in ("main", "components") if os.path.isdir(d)]
    hits = []
    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames
                           if d not in ("managed_components", "build", "test", "tests")]
            for fn in filenames:
                if not fn.endswith((".c", ".h")):
                    continue
                path = os.path.join(dirpath, fn)
                try:
                    text = open(path, errors="replace").read()
                except OSError:
                    continue
                for i, line in enumerate(text.splitlines(), 1):
                    if re.search(r"\.task_stack\s*=\s*\d+", line) or \
                       re.search(r"xTaskCreate\w*\s*\(", line):
                        nums = re.findall(r"\b(\d{3,6})\b", line)
                        if nums:
                            hits.append((path, i, line.strip()[:90], max(map(int, nums))))
    return hits


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--min-bytes", type=int, default=256)
    ap.add_argument("--fail-over", type=int, default=0)
    ap.add_argument("--elf", default=None)
    ap.add_argument("--all", action="store_true",
                    help="include framework and managed-component frames "
                         "(default: first-party only)")
    a = ap.parse_args()

    elf = a.elf or next(iter(sorted(glob.glob("build/*.elf"))), None)
    if not elf or not os.path.isfile(elf):
        print("stackcheck: no ELF. Build first, or pass --elf.", file=sys.stderr)
        return 2
    od = find_objdump(elf)
    if not od:
        print(f"stackcheck: no working objdump for {elf}. Source the IDF "
              f"export.sh so the toolchain is on PATH.", file=sys.stderr)
        return 2

    got = frames(od, elf)
    if not got:
        print("stackcheck: parsed no frames -- unrecognised disassembly.",
              file=sys.stderr)
        return 2

    scope = "all code"
    if not a.all:
        syms = project_symbols()
        if syms:
            kept = {k: v for k, v in got.items()
                    if k in syms or k.split("$")[0] in syms}
            if kept:
                got, scope = kept, "first-party only (--all for everything)"
        else:
            scope = "all code (no first-party objects found)"

    print(f"ELF:      {elf}")
    print(f"objdump:  {os.path.basename(od)}")
    print(f"scope:    {scope}")
    print(f"frames:   {len(got)} functions\n")

    ranked = sorted(got.items(), key=lambda kv: -kv[1])
    print(f"{'FRAME':>9}  FUNCTION  (>= {a.min_bytes} B, top {a.top})")
    for n, s in [kv for kv in ranked if kv[1] >= a.min_bytes][:a.top]:
        print(f"{s:>7} B  {n}")

    big_name, big = ranked[0]
    print(f"\nLargest frame: {big} B  ({big_name})")

    stacks = declared_stacks()
    if stacks:
        print("\nTask stacks declared in first-party source:")
        for path, line, text, _ in sorted(set(stacks)):
            print(f"  {path}:{line}  {text}")
        smallest = min(v for *_, v in stacks)
        print(f"\n  Largest frame is {100.0 * big / smallest:.0f}% of the smallest "
              f"declared stack ({smallest} B).")
        print("  A frame must leave room for everything it CALLS. Past roughly 40%")
        print("  of the stack it runs on, check what sits beneath it before shipping.")

    if a.fail_over and big > a.fail_over:
        print(f"\nFAIL: largest frame {big} B exceeds --fail-over {a.fail_over} B",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
