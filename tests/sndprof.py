#!/usr/bin/env python3
"""Per-symbol and per-instruction T-state profiles of sndtest's ticks.

Run from the repository root after `make`:

    python3 tests/sndprof.py timing
    python3 tests/sndprof.py --window 9 --pc timing
    python3 tests/sndprof.py --file SHORT.ZVG=tests/snd/fixtures/run-shortwaits.zvg \\
        zvg /fs/short.zvg 0 step

The words after the options are sndtest's arguments.  A window is one
marker pair (the DAC marker brackets each measured tick); window k is
the k-th pair, counted from 0.  Only the application's mapping is
attributed (latch mode 10), so interrupt time is out, as in
tests/test_snd.py's budgets.  The simulator charges a conditional
branch its taken cost.

Names come from sndtest's map (globals) and from listings of libsnd's
and sndtest's sources (local labels, assembled into a scratch directory
with `sdasz80 -l`; SDCC's own .lst for C), re-based through a global
label each listing shares with the map.  `--pc` prints the window's
per-instruction costs against those listings.
"""

import argparse
import bisect
import collections
import glob
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import snd_harness as H  # noqa: E402

MAPFILE = os.path.join(ROOT, "build", "user", "sndtest", "sndtest.map")
PROGRAM = os.path.join(ROOT, "build", "user", "sndtest.exe")
ASM_SOURCES = (glob.glob(os.path.join(ROOT, "user", "sdk", "snd", "src",
                                      "*.asm")) +
               glob.glob(os.path.join(ROOT, "tests", "snd", "*.asm")))
C_LISTINGS = (glob.glob(os.path.join(ROOT, "build", "user", "sdk", "snd",
                                     "*.lst")) +
              glob.glob(os.path.join(ROOT, "build", "user", "sndtest",
                                     "*.lst")))
DONE = ("PASS", "FAIL", " bytes")


def map_globals():
    out = {}
    for line in open(MAPFILE):
        m = re.match(r"\s+([0-9A-F]{8})\s+([A-Za-z_]\w*)\s", line)
        if m:
            out[m.group(2)] = int(m.group(1), 16)
    return out


def listing_lines(path):
    """[(offset, text, label, is_code)] for the listing's _CODE area.
    A line is `addr [bytes] [[cycles]] lineno source`; a relocated
    byte is marked rNN/sNN."""
    area = None
    rows = []
    for line in open(path, errors="replace"):
        ma = re.search(r"\.area\s+(\w+)", line)
        if ma:
            area = ma.group(1)
            continue
        if area != "_CODE":
            continue
        m = re.match(r"\s+([0-9A-F]{8})((?:\s?[0-9A-F]{2}"
                     r"(?:[rsRSuU][0-9A-F]{2})*)*)\s*"
                     r"(?:\[\s*\d+\])?\s+(\d+)\s(.*)$", line)
        if not m:
            continue
        text = m.group(4).strip()
        ml = re.match(r"([A-Za-z_.$][\w.$]*)::?", text)
        rows.append((int(m.group(1), 16), text, ml.group(1) if ml else None,
                     bool(m.group(2).strip())))
    return rows


def symbols(scratch):
    gl = map_globals()
    names = dict((a, n) for n, a in gl.items())
    lines = {}
    paths = list(C_LISTINGS)
    for src in ASM_SOURCES:
        base = os.path.splitext(os.path.basename(src))[0]
        rel = os.path.join(scratch, base + ".rel")
        subprocess.run(["sdasz80", "-plosgff", "-I" + os.path.dirname(src),
                        rel, src], cwd=scratch, check=True,
                       stdout=subprocess.DEVNULL)
        paths.append(os.path.join(scratch, base + ".lst"))
    for path in paths:
        rows = listing_lines(path)
        base = next((gl[label] - off for off, _t, label, _c in rows
                     if label in gl), None)
        if base is None:
            continue
        tag = os.path.basename(path)
        for off, text, label, code in rows:
            if label and not label.endswith("$"):
                names.setdefault(base + off, label)
            if code:
                lines.setdefault(base + off, "%s: %s" % (tag, text))
    items = sorted(names.items())
    return [a for a, _ in items], [n for _, n in items], lines


class Profile(H.SndReplay):
    def runTest(self):
        pass


def main():
    ap = argparse.ArgumentParser(prog="sndprof.py")
    ap.add_argument("--file", action="append", default=[],
                    help="NAME=PATH, a file on the simulated card")
    ap.add_argument("--window", type=int, help="profile only this pair")
    ap.add_argument("--pc", action="store_true",
                    help="per-instruction costs of --window")
    ap.add_argument("--top", type=int, default=8)
    ap.add_argument("args", nargs="+", help="sndtest's arguments")
    opt = ap.parse_args()
    if opt.pc and opt.window is None:
        ap.error("--pc needs --window")

    with tempfile.TemporaryDirectory() as scratch:
        addrs, names, lines = symbols(scratch)
    files = {"SNDTEST.EXE": open(PROGRAM, "rb").read()}
    for item in opt.file:
        name, path = item.split("=", 1)
        files[name] = open(path, "rb").read()

    p = Profile()
    p.setUp()
    command = ("/fs/sndtest.exe %s\r" % " ".join(opt.args)).encode()
    z, since = p.start(files, command)
    marks = [0]
    inner = z.sound_peer.write

    def write(port, value):
        inner(port, value)
        if (port & 0xFF, value & 0xFF) == (H.MARKER_PORT, H.MARKER_VALUE):
            marks[0] += 1
    z.sound_peer.write = write

    hist = collections.defaultdict(collections.Counter)
    per_pc = collections.Counter()
    steps = 0
    while steps < 200_000_000:
        pc = z.pc
        mapped = (z.latch & 0x18) == 0x10
        before = z.tstates
        p.skip_delay(z)
        p.feed(z)
        z.step()
        window = marks[0] // 2
        if mapped and marks[0] % 2 == 1 and pc >= 0x0100 and \
                (opt.window is None or window == opt.window):
            cost = z.tstates - before
            i = bisect.bisect_right(addrs, pc) - 1
            hist[window][names[i] if i >= 0 else "?"] += cost
            if opt.pc:
                per_pc[pc] += cost
        if opt.window is not None and window > opt.window:
            break
        if steps % 0x1FF == 0:
            p.pump(z)
            text = p.trace(z, since)
            if any(word in text for word in DONE):
                break
        steps += 1

    for window in sorted(hist):
        h = hist[window]
        print("== window %d: %d" % (window, sum(h.values())))
        for name, cost in h.most_common(opt.top):
            print("   %7d  %s" % (cost, name))
    if opt.pc:
        print("== per instruction, window %d" % opt.window)
        for pc in sorted(per_pc):
            print("   %6d  %04X  %s" % (per_pc[pc], pc,
                                       lines.get(pc, "?")))


if __name__ == "__main__":
    main()
