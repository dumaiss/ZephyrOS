"""Hardware exerciser acceptance replay: the
production image, with a card and without one.

Both replays type the owner's line - `hwtest` - at the shell, which is
the boot program, once with a card and once with the drive empty.  Each
then asserts, from the transcripts alone, that the store's exerciser
reached its verdict, that the programs its stages run reported through
it, that the shell came back to its prompt, and that the card is exactly
as the run found it.  The runner mirrors its own lines to `/dev/serial/0`
(the lane `tools/serial_smoke.py` watches), so the lane transcript is
asserted beside the console one.

A third replay copies one of the store's programs onto the card and runs
it from there as the shell's child (`FS_CHILD.EXE tail-42`: the shell's
`path` starts with `.`, which is the card, so the bare name resolves
through the card provider).  `SYS_EXEC` stages the caller's path in the
fixed core, outside the window the bank-6 filesystem provider replaces,
so the provider can resolve it.  The replay holds that behaviour: the
program loads and reports from the card medium, and the FS2 request log
proves the provider read it there.

The programs' individual checks are pinned by the xmem, filesystem,
namespace and exerciser suites; this module is the production outcome
replay `make check` runs, so it checks outcomes only.  It is the longest
replay in the suite by far: the exerciser and its nested programs run
for tens of millions of steps.

Skipped when the production image has not been built.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from maptools import evaluate, parse_c_header  # noqa: E402
from replay import SHELL_PROMPT, ConsoleReplay  # noqa: E402

# The shell's prompt follows the root it started in: the card, or the
# store when the drive is empty.
SHELL_PROMPT_NO_CARD = "/system> "
from z80sim import Ctc, IocCommandPeer  # noqa: E402

# The FS2 requests the card program's load must produce: the open, its
# reads and the close (see tools/z80sim.py's driver constants).
CMD_FS2_OPEN_RO = 0x35
CMD_FS2_READ = 0x36
CMD_FS2_CLOSE = 0x37

IMAGE_SUBDIR = "build"
IMAGE_NAME = "konaos.bin"

# The line the owner types at the shell.  The runner's own lines are
# what reaches the serial lane; the shell's echo stays on the console.
COMMAND = b"hwtest\r"

# The runner's own contract, on the lane it mirrors to.  The summary is
# the stop marker in either verdict.
BEGIN = b"hwtest: begin\r\n"
LANE_LINES = (BEGIN, b"hwtest: xmem+far PASS\r\n", b"hwtest: files PASS\r\n",
              b"hwtest: timer PASS\r\n",
              b"hwtest: namespace+handles PASS\r\n")
SUMMARY = b"hwtest: 4/4 "
PASS = b"hwtest: 4/4 PASS"
FILE_ABSENT = b"hwtest: files card absent\r\n"
NOTE_ABSENT = b" (card absent)"

# The stage programs report their own verdicts through the console the
# owner watches; the counts are the ones the standalone replays pin (the
# far program's includes the banked-call checks).
CONSOLE_EVIDENCE = ("xmem-selftest: 61 checks, 0 failures",
                    "xmem-far: 37 checks, 0 failures",
                    "fs-selftest: 37 checks, 0 failures",
                    "fs-child: PASS")
CONSOLE_ABSENT_EVIDENCE = ("xmem-selftest: 61 checks, 0 failures",
                           "xmem-far: 37 checks, 0 failures",
                           "fs-selftest: 8 checks, 0 failures",
                           "fs-child: card absent")

# The card the replays load: one file outside the scratch directory,
# which the exerciser must find untouched.  Everything the stages write
# lives below `/fs/HWTEST` and is removed again.
KEEP_NAME = ("KEEP.TXT",)
KEEP_CONTENT = b"keep me on the card"

# The exerciser and its nested programs run for tens of millions of
# steps, so the lane is inspected once per chunk: the replay helpers
# inspect it once per step, which costs more than the emulation itself
# at this length.
CHUNK = 500000
STEP_BUDGET = 100000000
PANIC = b"PANIC"

# The exerciser's rate probe (tests/hwtest/hwtest.c): the busy loop is
# calibrated in T-states, and the replay below pins that constant against
# the emulated clock's own measurement of the loop.
RUNNER = os.path.join(ROOT, "tests", "hwtest", "hwtest.c")
_RUNNER_CONSTANTS = parse_c_header(RUNNER)
TIMER_RATE_TSTATES = evaluate(_RUNNER_CONSTANTS["TIMER_RATE_TSTATES"],
                              _RUNNER_CONSTANTS)


def _clock_reads(z):
    """Timestamp every CTC port read (the driver's `now`).

    The rate probe's busy loop is the only stretch of the timer stage
    that reads no clock, so it is one gap between two consecutive reads -
    and its cost is what `TIMER_RATE_TSTATES` has to name."""
    reads = []
    inner = z.ctc.read

    def read(port):
        reads.append(z.tstates)
        return inner(port)

    z.ctc.read = read
    return reads


def _busy_loop_tstates(reads, expected):
    """The clock-read gap that is the rate probe's busy loop."""
    gaps = [second - first for first, second in zip(reads, reads[1:])]
    return min(gaps, key=lambda gap: abs(gap - expected))


class HwtestReplay(ConsoleReplay, unittest.TestCase):
    """The production image and the emulated device peers it boots."""

    IMAGE_SUBDIR = IMAGE_SUBDIR
    IMAGE_NAME = IMAGE_NAME

    def setUp(self):
        self.load_image()

    def start(self, card, extra_files=None):
        """Boot to the shell's prompt with an IOC card present or not.

        The emulated CTC delivers the timer driver's channel-2 interrupt,
        so the production clock runs for real in every session; the
        exerciser's timer stage and `timer.exe` read it."""
        z = self.make_cpu()
        Ctc().attach(z)
        files = None
        if card:
            files = {"/".join(KEEP_NAME): KEEP_CONTENT}
            files.update(extra_files or {})
        z.hid = z.ioc_peer = IocCommandPeer(fs2_media=card, fs2_files=files)
        self.boot_to_shell(z, prompt=(SHELL_PROMPT if card
                                      else SHELL_PROMPT_NO_CARD))
        return z

    def run_exerciser(self, z, budget=STEP_BUDGET):
        """Step in chunks until the fresh lane output carries the summary
        line, and return that transcript."""
        start = len(z.sio_tx)
        steps = 0
        while steps < budget:
            for _ in range(CHUNK):
                self.skip_delay(z)
                self.feed(z)
                z.step()
            steps += CHUNK
            fresh = bytes(z.sio_tx)[start:]
            if SUMMARY in fresh or PANIC in fresh:
                self.pump(z)
                return fresh
        self.pump(z)
        self.fail("the exerciser reached no summary in %u steps (PC=%04X)\n"
                  "lane: %r\nconsole: %r"
                  % (budget, z.pc, bytes(z.sio_tx)[start:][-300:],
                     "".join(z.glyphs)[-300:]))


class TestExerciserWithACard(HwtestReplay):
    """`hwtest` typed at the shell with the card in the drive."""

    def test_it_passes_and_leaves_the_card_as_it_found_it(self):
        z = self.start(card=True)
        peer = z.ioc_peer
        since = len(z.glyphs)
        reads = _clock_reads(z)
        z.hid.hid_queue.extend(COMMAND)

        lane = self.run_exerciser(z)

        # The store's runner reported every stage on its own lane.
        self.assertTrue(lane.startswith(BEGIN), lane[:48])
        for line in LANE_LINES:
            self.assertIn(line, lane)
        self.assertIn(PASS, lane)
        self.assertNotIn(b"FAIL", lane)
        self.assertNotIn(NOTE_ABSENT, lane)

        # Back at the shell's prompt, with the stages' own console
        # transcripts and their nested execs beside the runner's line.
        self.run_to(z, SHELL_PROMPT, since=since)
        trace = self.trace(z)
        for marker in CONSOLE_EVIDENCE:
            self.assertIn(marker, trace)
        self.assertIn(PASS.decode("ascii"), trace)
        self.assertNotIn("FAIL", trace)

        # The card is exactly as the run found it: the keep file the
        # exerciser must not touch, and no trace of the scratch
        # directory the files stage created and removed.
        self.assertEqual({parts: bytes(content)
                          for parts, content in peer.fs2_files.items()},
                         {KEEP_NAME: KEEP_CONTENT})
        self.assertEqual(peer.fs2_dirs, {()},
                         "the card carries directories the stages left")

        # The timer stage's rate probe is calibrated, not guessed: the
        # clock the replay advances over the busy loop - the one stretch
        # of the stage that reads no clock, so it is a single gap between
        # two reads - must be the cost `TIMER_RATE_TSTATES` names.  A
        # constant that drifted from the compiled loop would make the
        # stage's factor-2 window meaningless.
        loop = _busy_loop_tstates(reads, TIMER_RATE_TSTATES)
        print("hwtest: timer rate probe, busy loop %d T-states "
              "(constant %d)" % (loop, TIMER_RATE_TSTATES))
        self.assertLess(abs(loop - TIMER_RATE_TSTATES),
                        TIMER_RATE_TSTATES // 10)


class TestExerciserWithoutACard(HwtestReplay):
    """The same line with the drive empty: the absent-card branch."""

    def test_it_passes_with_the_absent_card_branch(self):
        z = self.start(card=False)
        peer = z.ioc_peer
        since = len(z.glyphs)
        z.hid.hid_queue.extend(COMMAND)

        lane = self.run_exerciser(z)

        self.assertTrue(lane.startswith(BEGIN), lane[:48])
        self.assertIn(FILE_ABSENT, lane)
        self.assertIn(PASS + NOTE_ABSENT + b"\r\n", lane)
        self.assertNotIn(b"FAIL", lane)

        self.run_to(z, SHELL_PROMPT_NO_CARD, since=since)
        trace = self.trace(z)
        # The console echoed the line it read, so the owner sees the
        # command as typed, and the runner's summary keeps the branch it
        # took.
        self.assertIn(COMMAND.decode("ascii").rstrip("\r"), trace)
        self.assertIn("hwtest: files card absent", trace)
        self.assertIn("hwtest: 4/4 PASS (card absent)", trace)
        for marker in CONSOLE_ABSENT_EVIDENCE:
            self.assertIn(marker, trace)
        self.assertNotIn("FAIL", trace)

        # No card: the namespace rows took their absent branch and
        # nothing was created anywhere behind the run.
        self.assertEqual(peer.fs2_files, {})
        self.assertEqual(peer.fs2_dirs, {()})


class TestMonitorRunsAStoreProgram(HwtestReplay):
    """`X` in the store's monitor runs a program and prints its status."""

    def test_it_reports_the_childs_exit_status_and_tail(self):
        z = self.start(card=True)
        # The monitor is the shell's child here; a program with a tail:
        # the child reports the tail it received and exits 42, which the
        # monitor must print as its u16 status before prompting again.
        self.send_line(z, "monitor", "Z80> ")
        self.send_line(z, "X /system/fs_child.exe tail-42", "fs-child: PASS")
        self.run_to(z, "exec: exit status 0x002A")
        trace = self.trace(z)
        self.assertIn('fs-child: tail "tail-42" argc 1', trace)
        self.assertIn("exec: exit status 0x002A", trace)


class TestTheShellRunsAProgramFromTheCard(HwtestReplay):
    """The shell loads a program that lives on the card medium itself."""

    def test_it_reads_the_program_from_the_card(self):
        # The store's `fs_child.exe` payload, byte for byte, laid down on
        # the emulated card: the image the shell is asked to run.
        payload = bytes(self.romfs_entries()["fs_child.exe"]["payload"])
        z = self.start(card=True, extra_files={"FS_CHILD.EXE": payload})
        peer = z.ioc_peer

        since = len(z.glyphs)
        self.send_line(z, "FS_CHILD.EXE tail-42", "fs-child: PASS")
        self.run_to(z, SHELL_PROMPT, since=since)
        trace = self.trace(z)
        self.assertIn('fs-child: tail "tail-42" argc 1', trace)
        # The shell reports a child that left a status.
        self.assertIn("[exit 42]", trace)
        self.assertNotIn("command not found", trace)

        # The provider answered the load from the card: before the fix
        # the request log held no FS2 traffic at all.
        kinds = [entry[0] for entry in peer.requests]
        self.assertIn(CMD_FS2_OPEN_RO, kinds)
        self.assertIn(CMD_FS2_READ, kinds)
        self.assertIn(CMD_FS2_CLOSE, kinds)
        self.assertGreaterEqual(kinds.count(CMD_FS2_READ), 2)
        # The load wrote nothing back: the card holds the files it
        # started with plus the empty scratch file the child's own card
        # row creates and leaves behind (its parent in the fs acceptance
        # image is what removes it).
        self.assertEqual({parts: bytes(content)
                          for parts, content in peer.fs2_files.items()},
                         {("FS_CHILD.EXE",): payload,
                          ("CHILD.TMP",): b"",
                          KEEP_NAME: KEEP_CONTENT})


if __name__ == "__main__":
    unittest.main()
