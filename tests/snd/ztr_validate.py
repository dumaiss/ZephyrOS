#!/usr/bin/env python3
"""The single-pass ZTR validation: golden writes + oracle state.

Run from the repository root after `make`:

    python3 tests/snd/ztr_validate.py night-market 3000 RunawayCircuit 3000

For each song this boots sndtest's ztrdump mode ONCE.  Its DAC markers
attribute the register writes per tick (compared with the golden
trace), and its printed per-tick channel state is compared with the
oracle's (ztr_trace SONG TICKS STATE): per tick, per channel, per
field.  Exit 0 only when both hold for every song.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tests"))
sys.path.insert(0, HERE)

import snd_harness as H  # noqa: E402
from ztr_state_diff import parse_line, oracle_state  # noqa: E402
from z80sim import read_symbols  # noqa: E402

PROGRAM = os.path.join(ROOT, "build", "user", "sndtest.exe")
MAPFILE = os.path.join(ROOT, "build", "user", "sndtest", "sndtest.map")
SYMS = read_symbols(MAPFILE)


def app_mapped(z):
    return (z.latch & 0x18) == 0x10


def engine_snapshot(z, tick):
    """Read the assembly-owned pages at snd_ztr_tick's return."""
    core = z.rd(SYMS["_snd_ztr_core_page"]) << 8
    macro_pages = [z.rd(SYMS["_snd_ztr_macro_page"] + slot) << 8
                   for slot in range(5)]
    state = {}
    for channel in range(16):
        base = channel << 4
        pitch = z.rd(core + base + 11) | (z.rd(core + base + 12) << 8)
        values = (z.rd(core + base + 3), z.rd(core + base + 6), pitch,
                  z.rd(core + base + 2))
        positions = tuple(z.rd(page + base) |
                          (z.rd(page + base + 1) << 8)
                          for page in macro_pages)
        state[(tick, channel)] = values + positions
    return state


def engine_run(song_name, ticks, batch=False, profile=False):
    """One target boot: (per-tick writes, per-tick engine state).

    State is read directly at snd_ztr_tick's return, while the application
    mapping is active.  This is the STEER.md breakpoint form of the debug
    export and avoids making the target print 16 lines per tick.
    """
    replay = H.SndReplay()
    replay.load_image()
    program = replay.load_program(PROGRAM)
    with open(os.path.join(ROOT, "tests", "snd", "fixtures", song_name),
              "rb") as handle:
        song = handle.read()
    command = b"/fs/sndtest.exe ztr /fs/run.ztr%s\r" % (
        b" batch" if batch else b"")
    z, since = replay.start({"SNDTEST.EXE": program, "RUN.ZTR": song},
                            command)
    tick_entry = SYMS["_snd_ztr_tick"]
    tick_return = None
    captured = 0
    marker_count = 0
    costs = [0] * ticks
    stack_base = [0] * ticks
    stack_min = [0xFFFF] * ticks
    original_write = z.sound_peer.write

    def write(port, value):
        nonlocal marker_count
        original_write(port, value)
        if ((port & 0xFF, value & 0xFF) ==
                (H.MARKER_PORT, H.MARKER_VALUE)):
            marker_count += 1
            if profile and marker_count % 2 == 1:
                index = marker_count // 2
                if index < ticks:
                    stack_base[index] = z.sp
                    stack_min[index] = z.sp

    z.sound_peer.write = write
    steps = 0
    state = {}
    while steps < 300000000:
        pc_mapped = app_mapped(z)
        before = z.tstates
        replay.skip_delay(z)
        replay.feed(z)
        if app_mapped(z):
            if tick_return is None and z.pc == tick_entry:
                tick_return = z.rd16(z.sp)
            elif tick_return is not None and z.pc == tick_return:
                state.update(engine_snapshot(z, captured))
                captured += 1
                tick_return = None
                if captured >= ticks and marker_count >= ticks * 2:
                    break
        z.step()
        if (profile and pc_mapped and marker_count % 2 == 1 and
                marker_count // 2 < ticks):
            index = marker_count // 2
            costs[index] += z.tstates - before
            stack_min[index] = min(stack_min[index], z.sp)
        if captured >= ticks and marker_count >= ticks * 2:
            break
        if steps % 0x3FFF == 0:
            replay.pump(z)
        steps += 1
    buckets, marks, orphans, trailing = H.split_ticks(
        replay.collect(z, since))
    # two markers per iteration: engine tick t = buckets[2t] + [2t+1];
    # the final marker closes no bucket, so the last tick may end with
    # only its first half present - merge what the markers framed.
    merged = []
    for t in range(ticks):
        entry = []
        for i in (2 * t, 2 * t + 1):
            if i < len(buckets):
                entry.extend(buckets[i])
        merged.append(entry)
    if profile:
        depths = [base - low for base, low in zip(stack_base, stack_min)]
        return merged, marks, state, costs, depths
    return merged, marks, state


def golden_writes(song, merged):
    golden = H.parse_golden(os.path.join(
        ROOT, "tests", "snd", "golden", song + ".trace"))
    bad = 0
    first = None
    for t in range(len(merged)):
        want = golden.get(t, [])
        got = [(H.port_to_chip(port), value) for port, value in merged[t]]
        if got != want:
            bad += 1
            if first is None:
                first = (t, got, want)
    return bad, first


def state_diff(engine, oracle):
    bad = 0
    first = None
    keys = sorted(set(engine) | set(oracle))
    for (tick, channel) in keys:
        want = oracle.get((tick, channel))
        got = engine.get((tick, channel))
        if want is None or got is None:
            bad += 1
            if first is None:
                first = ("missing", tick, channel, got, want)
            continue
        for index, field in enumerate(
                ("n", "v", "p", "f", "m0", "m1", "m2", "m3", "m4")):
            if got[index] != want[index]:
                bad += 1
                if first is None:
                    first = (field, tick, channel, got[index], want[index])
    return bad, first


def main(argv):
    if len(argv) < 3 or len(argv) % 2 != 1:
        print("usage: ztr_validate.py SONG TICKS [SONG TICKS ...]")
        return 2
    failed = 0
    for index in range(1, len(argv), 2):
        song = argv[index]
        ticks = int(argv[index + 1])
        fixture = os.path.join(ROOT, "tests", "snd", "fixtures",
                               song + ".ztr")
        oracle = oracle_state(fixture, ticks)
        merged, marks, state = engine_run(song + ".ztr", ticks)
        bad_w, first_w = golden_writes(song, merged)
        bad_s, first_s = state_diff(state, oracle)
        print("%s: %d ticks, %d markers, writes %d mismatches, "
              "state %d mismatches"
              % (song, ticks, len(marks), bad_w, bad_s))
        if first_w is not None:
            print("  first write diff: tick %d ours %s golden %s"
                  % (first_w[0],
                     " ".join("%02x" % value for _c, value in first_w[1]),
                     " ".join("%02x" % value for _c, value in first_w[2])))
        if first_s is not None:
            print("  first state diff: %s (tick %d ch%d: engine %s, "
                  "oracle %s)" % first_s)
        if bad_w or bad_s:
            failed += 1
    if failed == 0:
        print("VALIDATION PASS")
        return 0
    print("VALIDATION FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
