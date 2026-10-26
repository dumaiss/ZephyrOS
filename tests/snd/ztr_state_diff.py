#!/usr/bin/env python3
"""The Phase-1 state diff: the engine's channel state vs the oracle's.

Run from the repository root after `make` and an oracle build:

    python3 tests/snd/ztr_state_diff.py night-market 3000

Boots sndtest's ztrdump mode (which prints the channel state after
every tick, in the oracle's text) and diffs it against ztr_trace's
state dump: per tick, per channel, per field.  Exit 0 when equal, else
print the first differing fields and exit 1.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tests"))

import snd_harness as H  # noqa: E402

ZTR_TRACE = os.path.join(ROOT, "build", "host", "snd", "ztr_trace")
PROGRAM = os.path.join(ROOT, "build", "user", "sndtest.exe")

FIELDS = ("n", "v", "p", "f", "m0", "m1", "m2", "m3", "m4")


def parse_line(line):
    """One state line -> ((tick, channel), field tuple of ints).

    The oracle prints n/p/f/m in hex but v (the macro volume) as %u.
    """
    parts = line.replace("=", " ").split()
    # tick N: chI n=.. v=.. p=.. f=.. m=a b c d e
    if len(parts) != 17 or parts[0] != "tick":
        return None
    tick = int(parts[1].rstrip(":"))
    channel = int(parts[2][2:])
    values = (int(parts[4], 16), int(parts[6], 10), int(parts[8], 16),
              int(parts[10], 16)) + tuple(int(word, 16)
                                          for word in parts[12:17])
    return (tick, channel), values


def oracle_state(fixture, ticks):
    out = os.path.join("/tmp", "ztr-state-oracle.txt")
    subprocess.run([ZTR_TRACE, fixture, str(ticks), out], check=True,
                   stdout=subprocess.DEVNULL, cwd=ROOT)
    state = {}
    with open(out, encoding="ascii") as handle:
        for line in handle:
            parsed = parse_line(line.strip())
            if parsed is not None:
                state[parsed[0]] = parsed[1]
    return state


def engine_state(fixture_name, ticks):
    replay = H.SndReplay()
    replay.load_image()
    program = replay.load_program(PROGRAM)
    with open(os.path.join(ROOT, "tests", "snd", "fixtures",
                           fixture_name), "rb") as handle:
        song = handle.read()
    command = (b"/fs/sndtest.exe ztrdump /fs/run.ztr %d\r" % ticks)
    z, since = replay.start({"SNDTEST.EXE": program, "RUN.ZTR": song},
                            command)
    since_glyph = len(z.glyphs)
    steps = 0
    while steps < 60000000:
        replay.skip_delay(z)
        replay.feed(z)
        z.step()
        if steps % 0x1FF == 0:
            replay.pump(z)
            if "PASS" in "".join(z.glyphs[since_glyph:]):
                break
        steps += 1
    state = {}
    for line in "".join(z.glyphs[since_glyph:]).split("\n"):
        parsed = parse_line(line.strip())
        if parsed is not None:
            state[parsed[0]] = parsed[1]
    return state


def main(argv):
    if len(argv) != 3:
        print("usage: ztr_state_diff.py SONG TICKS")
        return 2
    song = argv[1]
    ticks = int(argv[2])
    fixture = os.path.join(ROOT, "tests", "snd", "fixtures",
                           song + ".ztr")
    oracle = oracle_state(fixture, ticks)
    engine = engine_state(song + ".ztr", ticks)
    print("oracle state entries: %d, engine: %d"
          % (len(oracle), len(engine)))
    keys = sorted(set(oracle) | set(engine))
    bad = 0
    for (tick, channel) in keys:
        want = oracle.get((tick, channel))
        got = engine.get((tick, channel))
        if want is None or got is None:
            bad += 1
            if bad <= 5:
                print("tick %d ch%d: missing (engine %r, oracle %r)"
                      % (tick, channel, got, want))
            continue
        for index, field in enumerate(FIELDS):
            if got[index] != want[index]:
                bad += 1
                if bad <= 5:
                    print("tick %d ch%d %s: engine %s, oracle %s"
                          % (tick, channel, field, got[index],
                             want[index]))
    if bad == 0:
        print("STATE EQUAL (%d ticks)" % ticks)
        return 0
    print("state diffs: %d" % bad)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
