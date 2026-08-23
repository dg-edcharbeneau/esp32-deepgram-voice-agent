#!/usr/bin/env python3
"""Turn a device capture into answers.

Reads TLM (1 Hz state) and EVT (transitions) lines and reports the four things
the test session exists to establish: what the audio bands actually do, whether
each behaviour is ever visible, what the frame rate is per behaviour, and whether
the session and internal RAM held up.

    ./analyse.py /tmp/runA.log
"""
import re, sys
from collections import defaultdict

# --- parsing -----------------------------------------------------------------

def parse(path):
    tlm, evt, faults = [], [], []
    for line in open(path, 'rb').read().decode('utf-8', 'replace').splitlines():
        if 'TLM ' in line:
            body = line.split('TLM ', 1)[1]
            row = {}
            for tok in body.split():
                if '=' not in tok:
                    continue
                k, v = tok.split('=', 1)
                row[k] = v
            tlm.append(row)
        elif 'EVT ' in line:
            evt.append(line.split('EVT ', 1)[1].strip())
        elif re.search(r'socket closed|transport error|Reconnect after|'
                       r'Could not lock|E \(\d+\)', line):
            faults.append(line.strip())
    return tlm, evt, faults

def pair(row, key):
    """Fields are avg/max pairs."""
    try:
        a, m = row[key].split('/')
        return float(a), float(m)
    except (KeyError, ValueError):
        return 0.0, 0.0

def num(row, key, cast=float):
    try:
        return cast(row[key])
    except (KeyError, ValueError):
        return 0

# --- audio segments ----------------------------------------------------------

# A sample counts as active if any band is above the gate's own resolution.
ACTIVE = 0.02
# Samples of quiet needed to end a segment. The protocol leaves ~3 s between
# steps precisely so this can separate them.
GAP = 2

def segment(tlm):
    """Split into runs of consecutive active samples, tagged with the source."""
    segs, cur, quiet = [], None, 0
    for row in tlm:
        _, lo = pair(row, 'low')
        _, mi = pair(row, 'mid')
        _, hi = pair(row, 'high')
        active = max(lo, mi, hi) > ACTIVE
        if active:
            quiet = 0
            if cur is None:
                cur = {'rows': [], 'src': row.get('src', '?')}
            cur['rows'].append(row)
        elif cur is not None:
            quiet += 1
            if quiet >= GAP:
                segs.append(cur)
                cur, quiet = None, 0
    if cur is not None:
        segs.append(cur)
    return segs

def report_segments(segs):
    print("=" * 78)
    print("AUDIO SEGMENTS  (each run of speech, in order -- map these to the protocol)")
    print("=" * 78)
    if not segs:
        print("  none: no sample had a band above %.2f" % ACTIVE)
        return
    print(f"  {'#':>2} {'src':<6} {'s':>3}  "
          f"{'amp avg/max':>12}  {'low':>11}  {'mid':>11}  {'high':>11}  clip")
    for i, seg in enumerate(segs, 1):
        rows = seg['rows']
        def agg(key):
            avgs = [pair(r, key)[0] for r in rows]
            maxs = [pair(r, key)[1] for r in rows]
            return sum(avgs) / len(avgs), max(maxs)
        a = agg('amp'); lo = agg('low'); mi = agg('mid'); hi = agg('high')
        clipped = [n for n, v in (('low', lo[1]), ('mid', mi[1]), ('high', hi[1]))
                   if v >= 0.99]
        print(f"  {i:>2} {seg['src']:<6} {len(rows):>3}  "
              f"{a[0]:>5.3f}/{a[1]:<6.3f}  {lo[0]:>4.2f}/{lo[1]:<6.2f}  "
              f"{mi[0]:>4.2f}/{mi[1]:<6.2f}  {hi[0]:>4.2f}/{hi[1]:<6.2f}  "
              f"{','.join(clipped) if clipped else '-'}")
    print()
    print("  A band reaching 1.00 is CLIPPED: it has stopped carrying information")
    print("  at exactly the moments it matters. Aim for a max near 0.75-0.85.")

def report_floor(tlm):
    """The noise floor, from samples where every band is shut."""
    quiet = [r for r in tlm if max(pair(r, 'low')[1], pair(r, 'mid')[1],
                                   pair(r, 'high')[1]) <= 0.0]
    print()
    print("=" * 78)
    print("NOISE FLOOR  (samples with the gate fully shut)")
    print("=" * 78)
    if not quiet:
        print("  none -- the gate never fully shut, which is itself the finding")
        return
    amps = [pair(r, 'amp')[1] for r in quiet]
    print(f"  {len(quiet)} of {len(tlm)} samples gated shut")
    print(f"  full-band amp while shut: max {max(amps):.3f}, mean {sum(amps)/len(amps):.3f}")
    print("  The gate is doing its job if amp is non-trivial here but bands are 0.")

# --- behaviour ---------------------------------------------------------------

def report_behaviour(evt):
    print()
    print("=" * 78)
    print("BEHAVIOUR DWELL  (how long each state was actually on screen)")
    print("=" * 78)
    dwell = defaultdict(list)
    order = []
    for e in evt:
        m = re.match(r'beh (\w+)->(\w+) after=([\d.]+)s', e)
        if m:
            dwell[m.group(1)].append(float(m.group(3)))
            order.append((m.group(1), m.group(2), float(m.group(3))))
    if not order:
        print("  no behaviour transitions recorded")
        return
    print(f"  {'state':<14} {'n':>3} {'total s':>8} {'min':>7} {'med':>7} {'max':>7}")
    for st, ds in sorted(dwell.items(), key=lambda kv: -sum(kv[1])):
        ds_sorted = sorted(ds)
        med = ds_sorted[len(ds_sorted) // 2]
        print(f"  {st:<14} {len(ds):>3} {sum(ds):>8.1f} {min(ds):>7.2f} "
              f"{med:>7.2f} {max(ds):>7.2f}")
    print()
    print("  A state with a median under ~0.3 s is shorter than the 280 ms blend:")
    print("  it never fully arrives on screen and would need a minimum dwell.")
    never = [s for s in ('IDLE', 'INITIALIZING', 'LISTENING', 'THINKING',
                         'SPEAKING', 'CONNECTING', 'BUFFERING', 'DISCONNECTED')
             if s not in dwell]
    if never:
        print(f"  NEVER OBSERVED: {', '.join(never)}")

def report_fps(tlm):
    print()
    print("=" * 78)
    print("FRAME RATE BY BEHAVIOUR")
    print("=" * 78)
    by = defaultdict(list)
    for r in tlm:
        by[r.get('beh', '?')].append((num(r, 'fps'), pair(r, 'draw')))
    print(f"  {'state':<14} {'n':>3} {'fps min':>8} {'fps avg':>8} "
          f"{'draw avg':>9} {'draw max':>9}")
    for st, vals in sorted(by.items(), key=lambda kv: -len(kv[1])):
        fps = [v[0] for v in vals if v[0] > 0]
        davg = [v[1][0] for v in vals]
        dmax = [v[1][1] for v in vals]
        if not fps:
            continue
        print(f"  {st:<14} {len(vals):>3} {min(fps):>8.1f} "
              f"{sum(fps)/len(fps):>8.1f} {sum(davg)/len(davg):>9.1f} "
              f"{max(dmax):>9.1f}")

def report_health(tlm, evt, faults):
    print()
    print("=" * 78)
    print("HEALTH")
    print("=" * 78)
    if tlm:
        intmax = [num(r, 'intmax', int) for r in tlm]
        intmax = [v for v in intmax if v > 0]
        if intmax:
            print(f"  internal largest-block: floor {min(intmax)} B, "
                  f"median {sorted(intmax)[len(intmax)//2]} B")
            if min(intmax) < 30000:
                print("    WARNING: below the ~29 kB a render buffer needs. This is the")
                print("    condition under which WebSocket writes started failing before.")
        stalled = [r for r in tlm if num(r, 'frames', int) == 0]
        if stalled:
            print(f"  {len(stalled)} telemetry windows with ZERO frames -- the UI stalled")
        drops = [num(r, 'drop', int) for r in tlm]
        if drops and max(drops) > 0:
            print(f"  audio dropped: {max(drops)} B")
    faces = [e for e in evt if e.startswith('face ')]
    setface = [e for e in evt if e.startswith('setface ')]
    taps = [e for e in evt if e in ('tap', 'hold', 'bootclick')]
    print(f"  face switches: {len(faces)}  {faces if faces else ''}")
    print(f"  set_face calls: {len(setface)}  {setface if setface else ''}")
    print(f"  input events: {len(taps)}  {taps if taps else ''}")
    print(f"  faults/errors: {len(faults)}")
    for f in faults[:8]:
        print(f"    {f}")
    if len(faults) > 8:
        print(f"    ... and {len(faults) - 8} more")

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    tlm, evt, faults = parse(sys.argv[1])
    print(f"parsed {len(tlm)} TLM samples, {len(evt)} events, {len(faults)} faults")
    if tlm:
        print(f"span: up={tlm[0].get('up')}s .. up={tlm[-1].get('up')}s")
    report_segments(segment(tlm))
    report_floor(tlm)
    report_behaviour(evt)
    report_fps(tlm)
    report_health(tlm, evt, faults)

main()
