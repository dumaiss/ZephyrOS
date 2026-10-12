"""Hardware exerciser acceptance replay (work package 45b): the
production image, with a card and without one.

Both replays type the owner's line - `X /system/hwtest.exe` - into the
running production system, once over the serial command lane
(`/dev/serial/0`, the lane `tools/serial_smoke.py --mode hwtest` drives)
and once over the HID keyboard lane the V9958 console reads (work
package 42e).  Each then asserts, from the transcripts alone, that the
store's exerciser reached its verdict, that the programs its stages run
reported through it, that the monitor came back to the prompt, and that
the card is exactly as the run found it.

A third replay copies one of the store's programs onto the card and runs
it from there through the monitor (`X /fs/FS_CHILD.EXE tail-42`).  That
is the path correction 01 repaired: `SYS_EXEC` used to stage the caller's
path in bank-7 `_DATA`, inside the window the bank-6 filesystem provider
replaces, so the provider never resolved it and the monitor answered
`ERR: 02` without touching the card.  The replay holds the fixed
behaviour: the program loads and reports from the card medium, and the
FS2 request log proves the provider read it there.

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

from replay import ConsoleReplay  # noqa: E402
from z80sim import IocCommandPeer  # noqa: E402

# The FS2 requests the card program's load must produce: the open, its
# reads and the close (see tools/z80sim.py's driver constants).
CMD_FS2_OPEN_RO = 0x35
CMD_FS2_READ = 0x36
CMD_FS2_CLOSE = 0x37

IMAGE_SUBDIR = "build"
IMAGE_NAME = "konaos.bin"

# The line the owner types and the lane echo it produces.
COMMAND = b"X /system/hwtest.exe\r"
ECHO = b"X /system/hwtest.exe\r\n"

# The runner's own contract, on the lane it mirrors to.  The summary is
# the stop marker in either verdict.
BEGIN = b"hwtest: begin\r\n"
LANE_LINES = (BEGIN, b"hwtest: xmem+far PASS\r\n", b"hwtest: files PASS\r\n",
              b"hwtest: namespace+handles PASS\r\n")
SUMMARY = b"hwtest: 3/3 "
PASS = b"hwtest: 3/3 PASS"
FILE_ABSENT = b"hwtest: files card absent\r\n"
NOTE_ABSENT = b" (card absent)"
MONITOR_STATUS = "exec: exit status 0x0000"

# The stage programs report their own verdicts through the console the
# owner watches; the counts are the ones the standalone replays pin (the
# far program's includes the banked-call checks, package 49b).
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


class HwtestReplay(ConsoleReplay, unittest.TestCase):
    """The production image and the emulated device peers it boots."""

    IMAGE_SUBDIR = IMAGE_SUBDIR
    IMAGE_NAME = IMAGE_NAME

    def setUp(self):
        self.load_image()

    def start(self, card, extra_files=None):
        """Boot to the monitor prompt with an IOC card present or not."""
        z = self.make_cpu()
        files = None
        if card:
            files = {"/".join(KEEP_NAME): KEEP_CONTENT}
            files.update(extra_files or {})
        z.hid = z.ioc_peer = IocCommandPeer(fs2_media=card, fs2_files=files)
        self.boot_to_prompt(z)
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
    """`X /system/hwtest.exe` typed on the serial command lane."""

    def test_it_passes_and_leaves_the_card_as_it_found_it(self):
        z = self.start(card=True)
        peer = z.ioc_peer
        z.sio_rx.extend(COMMAND)

        lane = self.run_exerciser(z)

        # The monitor echoed the line on the lane it was typed on, and
        # the store's runner reported every stage there too.
        self.assertTrue(lane.startswith(ECHO), lane[:48])
        for line in LANE_LINES:
            self.assertIn(line, lane)
        self.assertIn(PASS, lane)
        self.assertNotIn(b"FAIL", lane)
        self.assertNotIn(NOTE_ABSENT, lane)

        # Back at the prompt with the runner's status, then the console
        # evidence: the stages' own transcripts and their nested execs.
        self.run_to(z, MONITOR_STATUS)
        trace = self.trace(z)
        self.assertIn(MONITOR_STATUS, trace)
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


class TestExerciserWithoutACard(HwtestReplay):
    """The same command on the console lane: the HID keyboard."""

    def test_it_passes_with_the_absent_card_branch(self):
        z = self.start(card=False)
        peer = z.ioc_peer
        # The production console's own lane.  Nothing reaches the serial
        # lane, so its transcript starts at the runner's first line.
        z.hid.hid_queue.extend(COMMAND)

        lane = self.run_exerciser(z)

        self.assertTrue(lane.startswith(BEGIN), lane[:48])
        self.assertIn(FILE_ABSENT, lane)
        self.assertIn(PASS + NOTE_ABSENT + b"\r\n", lane)
        self.assertNotIn(b"FAIL", lane)

        self.run_to(z, MONITOR_STATUS)
        trace = self.trace(z)
        self.assertIn(MONITOR_STATUS, trace)
        # The monitor echoed the HID line it read, so the owner sees the
        # command as typed.
        self.assertIn(COMMAND.decode("ascii").rstrip("\r"), trace)
        self.assertIn("hwtest: files card absent", trace)
        self.assertIn("hwtest: 3/3 PASS (card absent)", trace)
        for marker in CONSOLE_ABSENT_EVIDENCE:
            self.assertIn(marker, trace)
        self.assertNotIn("FAIL", trace)

        # No card: the namespace rows took their absent branch and
        # nothing was created anywhere behind the run.
        self.assertEqual(peer.fs2_files, {})
        self.assertEqual(peer.fs2_dirs, {()})


class TestMonitorRunsAStoreProgram(HwtestReplay):
    """`X` runs a stored program, waits for it and prints its status."""

    def test_it_reports_the_childs_exit_status_and_tail(self):
        z = self.start(card=True)
        # A stored program with a tail: the child reports the tail it
        # received and exits 42, which the monitor must print as its
        # u16 status before prompting again.
        self.send_line(z, "X /system/fs_child.exe tail-42", "fs-child: PASS")
        self.run_to(z, "exec: exit status 0x002A")
        trace = self.trace(z)
        self.assertIn('fs-child: tail "tail-42" argc 1', trace)
        self.assertIn("exec: exit status 0x002A", trace)


class TestMonitorRunsAProgramFromTheCard(HwtestReplay):
    """`X` loads a program that lives on the card medium itself."""

    def test_it_reads_the_program_from_the_card(self):
        # The store's `fs_child.exe` payload, byte for byte, laid down on
        # the emulated card: the image the monitor is asked to run.
        payload = bytes(self.romfs_entries()["fs_child.exe"]["payload"])
        z = self.start(card=True, extra_files={"FS_CHILD.EXE": payload})
        peer = z.ioc_peer

        self.send_line(z, "X /fs/FS_CHILD.EXE tail-42", "fs-child: PASS")
        self.run_to(z, "exec: exit status 0x002A")
        trace = self.trace(z)
        self.assertIn('fs-child: tail "tail-42" argc 1', trace)
        self.assertIn("exec: exit status 0x002A", trace)
        self.assertNotIn("ERR: 02", trace)

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
