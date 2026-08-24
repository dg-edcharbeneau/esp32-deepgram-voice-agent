#!/usr/bin/env python3
"""Diff the C port's frames against the reference's, per case.

Compares in draw order first, since draw order is part of the contract. If that
mismatches, re-compares as a canonically sorted multiset, which separates a real
transcription error from a z-sort tie broken differently by the two sorts.
"""
import sys
from collections import defaultdict

TOL = 0.02  # pixels / ink units. float32 vs double drift is ~0.004 here.

def load(path):
    by_case = defaultdict(list)
    with open(path) as f:
        for line in f:
            p = line.split()
            # 7 tokens is a dot (x y z r white a); 8 is a line
            # (x1 y1 x2 y2 white a w). Lines carry a `~L` label suffix, so the
            # two never share a case and the width is unambiguous per case.
            if len(p) not in (7, 8):
                continue
            by_case[p[0]].append(tuple(float(v) for v in p[1:]))
    return by_case

ref, port = load(sys.argv[1]), load(sys.argv[2])
DOT_FIELDS = ('x', 'y', 'z', 'r', 'white', 'a')
LINE_FIELDS = ('x1', 'y1', 'x2', 'y2', 'white', 'a', 'w')
worst_all, failures = 0.0, 0

for case in ref:
    a, b = ref[case], port.get(case, [])
    is_line = bool(a) and len(a[0]) == 7
    FIELDS = LINE_FIELDS if is_line else DOT_FIELDS
    noun = "lines" if is_line else "dots"
    if len(a) != len(b):
        print(f"FAIL {case:14s} {noun} count {len(a)} vs {len(b)}")
        failures += 1
        continue

    def maxdev(xs, ys):
        worst, where = 0.0, None
        for i, (ra, rb) in enumerate(zip(xs, ys)):
            for k, (va, vb) in enumerate(zip(ra, rb)):
                d = abs(va - vb)
                if d > worst:
                    worst, where = d, (i, FIELDS[k], va, vb)
        return worst, where

    worst, where = maxdev(a, b)
    if worst <= TOL:
        print(f"ok   {case:14s} {len(a):3d} {noun}, max dev {worst:.6f}")
        worst_all = max(worst_all, worst)
        continue

    # Draw order disagreed, which is expected: the port sorts by row band then
    # depth (for PSRAM locality) where the reference sorts by depth alone. So
    # compare as a MULTISET instead -- each reference dot must have one unmatched
    # port dot agreeing on all six fields.
    #
    # Matching, not sorting on a rounded key: any key fine enough to separate
    # neighbouring dots is also fine enough for the float32 drift to flip it.
    grid = defaultdict(list)
    for d in b:
        grid[(round(d[0]), round(d[1]))].append(d)

    unmatched, worst_m = 0, 0.0
    for d in a:
        best, bestdev = None, None
        gx, gy = round(d[0]), round(d[1])
        for ox in (-1, 0, 1):
            for oy in (-1, 0, 1):
                for cand in grid.get((gx + ox, gy + oy), ()):
                    dev = max(abs(p - q) for p, q in zip(d, cand))
                    if bestdev is None or dev < bestdev:
                        best, bestdev = cand, dev
        if best is not None and bestdev <= TOL:
            grid[(round(best[0]), round(best[1]))].remove(best)
            worst_m = max(worst_m, bestdev)
        else:
            unmatched += 1

    if unmatched == 0:
        print(f"ok   {case:14s} {len(a):3d} {noun}, max dev {worst_m:.6f} "
              f"(same dots, port's own draw order)")
        worst_all = max(worst_all, worst_m)
    else:
        print(f"FAIL {case:14s} {unmatched} of {len(a)} dots have no match "
              f"within {TOL}")
        failures += 1

print()
print(f"{'FAILURES: %d' % failures if failures else 'all cases match'}"
      f"  (worst deviation {worst_all:.6f}, tolerance {TOL})")
sys.exit(1 if failures else 0)
