#!/usr/bin/env python3
"""Per-function T-state profiles of gfxtest's timing workloads.

Run from the repository root after `make` (the top-level build makes
gfxtest.exe and its map):

    python3 tests/gfxprof.py

The twelve timing figures run exactly as TestGfxTiming measures them -
the legacy five with the duration model off, the package-96 workloads
with the frame clock and the duration model on from the 12th marker -
and each figure's CPU window is profiled down to 90% of its T-states.
The symbol tables come from gfxtest's map (global members, including
the assembly backend) and the SDCC listings (static functions);
`app:` names execute in the program's bank, `osN:` names are
approximate (bank N, kernel map).
"""

import bisect
import collections
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import test_gfx as T  # noqa: E402
from z80sim import read_symbols  # noqa: E402

MAPFILE = os.path.join(ROOT, "build", "user", "gfxtest", "gfxtest.map")
APP_LSTS = (glob.glob(os.path.join(ROOT, "build", "user", "gfxtest",
                                   "*.lst")) +
            glob.glob(os.path.join(ROOT, "build", "user", "sdk", "gfx",
                                   "*.lst")))
OS_MAP = os.path.join(ROOT, "build", "konaos.map")

FIGURES = ["glyph", "fill", "line", "filled", "outline", "polyline",
           "row-new", "row-cached", "stripe", "page-switch", "vblank",
           "present"]


def symbols(mapfile, lsts):
    """(addresses, names) for the program: map globals first, then the
    listing's static labels re-based through any label the map knows."""
    gl = {}
    for line in open(mapfile):
        m = re.match(r"\s+([0-9A-F]{8})\s+(_\w+)\s", line)
        if m:
            gl[m.group(2)] = int(m.group(1), 16)
    out = dict((a, n) for n, a in gl.items())
    for path in lsts:
        area = None
        labels = []
        for line in open(path):
            ma = re.search(r"\.area\s+(\w+)", line)
            if ma:
                area = ma.group(1)
                continue
            m = re.match(r"\s+([0-9A-F]{8})\s+\d+\s+(_\w+)::?", line)
            if m and area == "_CODE":
                labels.append((int(m.group(1), 16), m.group(2)))
        base = next((gl[n] - off for off, n in labels if n in gl), None)
        if base is not None:
            for off, n in labels:
                out.setdefault(base + off, n)
    items = sorted(out.items())
    return [a for a, _ in items], [n for _, n in items]


class Profile(T.TestGfxTiming):
    def runTest(self):
        pass


def main():
    app_a, app_n = symbols(MAPFILE, APP_LSTS)
    entries = set(app_a)
    os_syms = sorted((a, n) for n, a in read_symbols(OS_MAP).items()
                     if isinstance(a, int))
    os_a = [a for a, _ in os_syms]
    os_n = [n for _, n in os_syms]

    p = Profile()
    p.setUp()
    z = p.start()
    ops = []
    since = len(z.glyphs)
    z.hid.hid_queue.extend(b"/fs/gfxtest.exe timing\r")
    z.hid.hid_queue.extend(b"x")  # the timing mode's final key
    p.run_recording(z, ops, lambda z: T.OPS_ACQUIRE in ops)
    peer = z.vdp
    app_bank = z.bank_of(0x0100)

    def markers():
        return [now for value, reg, now in peer.reg_writes
                if reg == 16 and value == 15]

    def bank_of(pc):
        if z.latch & 0x18 not in (0x10, 0x18):
            return z.latch & 0x07
        return z.bank_of(pc)

    def name(pc):
        if bank_of(pc) == app_bank:
            if pc >= 0x0100:
                i = bisect.bisect_right(app_a, pc) - 1
                return "app:" + (app_n[i] if i >= 0 else "?")
            return "app:page0"
        i = bisect.bisect_right(os_a, pc) - 1
        return "os%d:%s" % (bank_of(pc), os_n[i] if i >= 0 else "?")

    # The frame clock and the duration model come on after the 12th
    # marker (the start of the polyline), as in the timing test.
    hist = [collections.Counter() for _ in FIGURES]
    calls = [collections.Counter() for _ in FIGURES]
    enabled = False
    steps = 0
    for _step in range(T.EXEC_BUDGET * 2):
        pc = z.pc
        before = z.tstates
        p.skip_delay(z)
        p.feed(z)
        z.step()
        if steps % 0x1FF == 0:
            p.pump(z)
        steps += 1
        marks = markers()
        if not enabled and len(marks) >= 12:
            peer.frame_period = T.FRAME_TSTATES
            peer.duration_model = True
            enabled = True
        # Attribute the step to the newest figure pair: the test drops
        # the two leading palette entries, then pairs consecutive
        # markers.  An odd length means the pair's closing mark is
        # still ahead, so the window stays open.
        if len(marks) >= 3:
            if len(marks) % 2 == 0:
                figure = len(marks) // 2 - 2
                start, end = marks[-2], marks[-1]
            else:
                figure = (len(marks) - 3) // 2
                start, end = marks[-1], T.EXEC_BUDGET << 30
            if 0 <= figure < len(FIGURES) and start <= before < end:
                hist[figure][name(pc)] += z.tstates - before
                if pc in entries and bank_of(pc) == app_bank:
                    calls[figure][name(pc)] += 1
        if "gfxtest: timing" in "".join(z.glyphs[since:]):
            break
    else:
        p.fail("no outcome in %u steps (PC=%04X)" % (T.EXEC_BUDGET, z.pc))

    marks = markers()[2:]
    if len(marks) < 24:
        p.fail("only %u markers" % len(marks))
    for figure, (start, end) in enumerate(zip(marks[::2], marks[1::2])):
        total = end - start
        print("== %s: %d T-states" % (FIGURES[figure], total))
        covered = 0
        shown = 0
        for fn, ts in hist[figure].most_common():
            if covered * 100 >= total * 90 and shown:
                break
            print("  %9d %5.1f%% %6d %s"
                  % (ts, 100.0 * ts / total, calls[figure][fn], fn))
            covered += ts
            shown += 1
            if shown >= 30:
                break


if __name__ == "__main__":
    main()
