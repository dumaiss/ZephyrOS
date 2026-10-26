#!/usr/bin/env python3
"""The shared sound-series replay harness (tests/snd_harness.py).

Every libsnd package (97-103) replays its card program on the production
image and compares the card's exact write log against the behavioral
goldens 97 froze.  The three facilities:

  - replay:  boot the production image with the emulated Afternoon Blend
             card, the fixtures on it, run a program from the shell and
             collect the SoundPeer writes;
  - compare: split the writes into ticks by the DAC marker and compare
             them tick by tick with a golden trace; a mismatch names the
             tick and the first differing byte;
  - timing:  CPU T-states between marker pairs, attributing each step to
             the bank its PC was in, so interrupt service (the kernel
             bank) is excluded - the same attribution tests/gfxprof.py
             uses.  A profile mode is not needed here: callers list the
             function histogram themselves.

The marker is the DAC port E4h with value A5h, which no song writes; a
program emitting ticks writes it once before each tick's bytes.  The
split treats everything between consecutive markers as that tick.

Trace format (tests/snd/golden/*.trace):

    # comment lines
    tick N: c<chip>:<hh> ...

one line per tick, the writes in issue order, chip one of 0-3.  The ZVG
traces use chip 0 for the chosen PSG; the caller maps ports.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from test_hwtest_sim import HwtestReplay  # noqa: E402
from replay import SHELL_PROMPT  # noqa: E402
from z80sim import SoundPeer, IocCommandPeer, Ctc  # noqa: E402

MARKER_PORT = 0xE4          # the DAC port, in every ksound_info record
MARKER_VALUE = 0xA5         # no song writes it

STEP_BUDGET = 12000000

# The golden-trace grammar: `tick N: c<chip>:<hh> ...`.
TICK_RE = re.compile(r"^tick (\d+):(.*)$")
WRITE_RE = re.compile(r"c([0-3]):([0-9a-fA-F]{2})")


def parse_golden(path):
    """{tick: [(chip, byte), ...]} from a golden trace file."""
    golden = {}
    with open(path, encoding="ascii") as handle:
        for line in handle:
            match = TICK_RE.match(line.strip())
            if not match:
                continue
            tick = int(match.group(1))
            writes = [(int(chip), int(value, 16))
                      for chip, value in WRITE_RE.findall(match.group(2))]
            golden[tick] = writes
    return golden


def split_ticks(writes, marker=(MARKER_PORT, MARKER_VALUE)):
    """Split (port, value, tstate) writes into per-tick lists.

    Returns (ticks, marks, orphans, trailing): ticks is a list of
    per-tick (port, value) lists - everything between two consecutive
    markers - marks the markers' T-states, orphans the writes before the
    first marker, and trailing the writes after the last one (for
    example a release's silence, which no marker frames).
    """
    ticks = []
    marks = []
    current = None
    orphans = []
    trailing = []
    for port, value, tstate in writes:
        if (port, value) == marker:
            marks.append(tstate)
            if current is not None:
                ticks.append(current)
            current = []
        elif current is not None:
            current.append((port, value))
        else:
            orphans.append((port, value))
    if current is not None:
        if current:
            trailing.extend(current)
    return ticks, marks, orphans, trailing


def port_to_chip(port):
    """The PSG a card port belongs to, or None."""
    if SoundPeer.FIRST_PORT <= port <= SoundPeer.LAST_PORT - 1:
        return port - SoundPeer.FIRST_PORT
    return None


def compare_ticks(ticks, golden, chip_map=None):
    """Compare split ticks with a golden trace.

    `chip_map` maps golden chip numbers to the chips the replay wrote
    (for a ZVG moved to another PSG); None keeps them.  Returns None on
    a match, else a message naming the tick and the first differing
    byte.
    """
    if chip_map is None:
        chip_map = {}
    if len(ticks) != len(golden):
        return ("tick count %d != golden %d" % (len(ticks), len(golden)))
    for tick, writes in enumerate(ticks):
        expected = golden.get(tick)
        if expected is None:
            return "tick %d missing from golden" % tick
        decoded = [(port_to_chip(port), value) for port, value in writes]
        wanted = [(chip_map.get(chip, chip), value) for chip, value in expected]
        if len(decoded) != len(wanted):
            return ("tick %d: %d writes, golden %d (%s vs %s)"
                    % (tick, len(decoded), len(wanted),
                       " ".join("%02x" % value for _port, value in decoded),
                       " ".join("%02x" % value for _chip, value in wanted)))
        for index, (got, want) in enumerate(zip(decoded, wanted)):
            if got != want:
                return ("tick %d write %d: chip %s byte %02x, golden "
                        "chip %s byte %02x"
                        % (tick, index,
                           "?" if got[0] is None else str(got[0]), got[1],
                           want[0], want[1]))
    return None


class SndReplay(HwtestReplay):
    """The production image with the Afternoon Blend card, shared by the
    sound series' replays."""

    IMAGE_SUBDIR = "build"
    IMAGE_NAME = "konaos.bin"

    def load_program(self, exe):
        with open(exe, "rb") as handle:
            return handle.read()

    def start(self, files, command, budget=STEP_BUDGET):
        """Boot with the card and `files`, run `command`, return
        (z, since_writes) with the command line fed."""
        z = self.make_cpu()
        z.sound_peer = SoundPeer()
        z.hid = z.ioc_peer = IocCommandPeer(fs2_media=True,
                                            fs2_files=files)
        self.boot_to_shell(z, prompt=SHELL_PROMPT)
        since = len(z.sound_peer.writes)
        z.hid.hid_queue.extend(command)
        return z, since

    def run_to_pattern(self, z, pattern, since=0, budget=STEP_BUDGET):
        super(SndReplay, self).run_to_pattern(z, pattern, limit=budget,
                                              since=since)

    def collect(self, z, since):
        """The card's (port, value, tstate) writes since `since`."""
        return z.sound_peer.writes[since:]

    def profiled(self, z, until, budget=STEP_BUDGET, every=0x1FF):
        """Step one instruction at a time until `until(z)` is true,
        attributing each step's T-states to the program's code.

        Returns (intervals, marks): for each marker pair, the program
        T-state total and the marker T-states.  A step belongs to the
        program when its PC is in the program's code window
        (0100h-1FFFh, the mode-11 latch-bank region the EXE's `_CODE`
        occupies); the kernel's interrupt path (the E000h vector page
        and the bank-7 kernel code at 2000h-5FFFh) is excluded, as
        tests/gfxprof.py attributes by address.
        """
        marks = []
        intervals = []
        state = {"current": 0}
        inner = z.sound_peer.write

        def write(port, value):
            inner(port, value)
            if ((port & 0xFF, value & 0xFF) ==
                    (MARKER_PORT, MARKER_VALUE)):
                marks.append(z.tstates)
                intervals.append(state["current"])
                state["current"] = 0

        z.sound_peer.write = write
        steps = 0
        while steps < budget:
            before = z.tstates
            pc_before = z.pc
            self.skip_delay(z)
            self.feed(z)
            z.step()
            if 0x0100 <= pc_before < 0x2000:
                state["current"] += z.tstates - before
            if steps % every == 0:
                self.pump(z)
                if until(z):
                    return intervals, marks
            steps += 1
        raise AssertionError("profiled run exceeded %d steps" % budget)
