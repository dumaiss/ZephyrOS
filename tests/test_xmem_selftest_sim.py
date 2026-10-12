#!/usr/bin/env python3
"""Extended-memory outcome acceptance replay (work packages 43/44b).

Boots `build-xmem-selftest/konaos-xmem-selftest.bin` -- the production
boot path with the SIO/console driver set and the SDK outcome program
booting as `/system/xmem_selftest.exe` -- and asserts the recorded
transcript:

* the boot lanes and the program's verdict line (every check passed, the
  pinned check count);
* every extended-memory outcome: sizes and slot rounding, the largest
  object, oversized rejection, exhaustion and fragmentation, stale-handle
  rejection, no pre-zeroing, boundary and whole-object transfers, range
  rejection, multiple objects per bank;
* the aperture outcomes: the token/address pair, direct access, buffer
  identity (a syscall buffer in the aperture names the far bank), the
  non-mappable refusal;
* the nested exec from far context: the child reports its own latch page
  (P = 0), the parent's latch comes back exactly, the parent's mapped
  objects are untouched, and the child's leaked objects are released at
  exit (capacity before == capacity after);
* far code (package 44b): the far-code program the parent SYS_EXECs runs
  its own two FAR_CODE segments through the aperture - map, plain CALL
  and return, far code calling primary code, syscalls with an aperture
  buffer, distinct banks per segment (the latch each call returns), the
  free aperture slot holding mappable data, exec from far context (the
  child starts with P = 0 and refuses the parent's handle and segment id)
  and exit from far context (the parent's capacity check and the
  launcher's `free-banks 6` report prove the release);
* banked calls (package 49b): the far module's `__banked` entries called
  as plain C through the SDK's `___sdcc_bcall_ehl` trampoline - 0-3
  arguments of 8, 16 and 32 bits, a mixed call and a recursion - and the
  segment 2 entry calling back into segment 1, which has to restore its
  caller's bank.  A `__banked` definition in the primary bank names no
  far segment, so it reports KOS_RT_EFARMAP.  The replay also counts
  SYS_SEG_MAP: the trampoline spends exactly one per segment id (the
  token cache), however many banked calls the program makes;
* the launcher's report: exit status 0, no active process and six free
  banks - every object, every segment and every bank slot was released by
  exit.

Skipped when the image has not been built.

Run with: `python3 -m unittest tests.test_xmem_selftest_sim`.
"""

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from replay import STEP_LIMIT, Replay  # noqa: E402

IMAGE_SUBDIR = "build-xmem-selftest"
IMAGE_NAME = "konaos-xmem-selftest.bin"

PASS = "xmem-selftest: PASS\r\n"
FAIL = "xmem-selftest: FAIL"
CHILD_PASS = "xmem-child: PASS\r\n"
PANIC = "PANIC"

# The launcher's exit report, printed after the program's exit.  It is
# the exit-release proof: the program leaves its two anchor objects live
# in bank 5, and `free-banks 6` says that bank is a free primary bank
# again (a bank with a live auxiliary slot can never be one).
REPORT = "monitor: exit status 0x0000 active 0 free-banks 6\r\n"

# The child's report, asserted in order.  It runs twice: from the parent
# (whose own bank is 1) and once from the far-code program's segment 1
# entry, whose child gets the lowest free bank, 2.  Every run proves the
# child starts with P = 0 (its own words, mode 10 over its bank) and that
# a child inherits no handle and no segment id.
CHILD_LINES = [
    "xmem-child: begin\r\n",
    "xmem-child: latch 11\r\n",
    "xmem-child: PASS\r\n",
    "xmem-child: begin\r\n",
    "xmem-child: latch 12\r\n",
    "xmem-child: PASS\r\n",
]

# Every outcome the parent's program reaches, in order.
OUTCOMES = [
    "startup-version", "startup-tail", "console-write",
    "anchor-alloc", "anchor-info",
    "alloc-zero-rejected", "alloc-flags-rejected", "alloc-oversized",
    "alloc-mappable-oversized",
    "size-1", "size-8191", "size-8192", "size-8193",
    "boundary-first", "boundary-last", "boundary-slot", "whole-object",
    "range-read-end", "range-read-straddle", "range-write-end",
    "range-offset", "range-buffer",
    "stale-free", "stale-reused", "stale-read-live", "stale-info-live",
    "largest-alloc", "largest-head", "largest-tail", "largest-whole",
    "no-zeroing",
    "multi-alloc", "multi-contents", "multi-distinct",
    "fill-banks", "fragmentation-32k", "fragmentation-16k",
    "fragmentation-hole", "fragmentation-refill",
    "exhaustion-16k-count", "exhaustion-16k", "exhaustion-16k-refill",
    "exhaustion-mappable-count", "exhaustion-mappable",
    "exhaustion-mappable-refill",
    "map-token", "map-address-low", "map-address-high", "map-token-page",
    "map-non-mappable",
    "aperture-open", "aperture-direct", "aperture-xmem-read",
    "identity-write", "identity-read",
    "exec-status", "exec-latch-restored", "exec-parent-object-intact",
    "exec-child-released",
    "aperture-close",
    "far-program-status", "far-program-released",
    "hold-anchors",
]

CHECKS = len(OUTCOMES)

# The far-code program's outcomes (package 44b), in order; it is
# /system/xmem_far.exe, SYS_EXECed by the parent above.
FAR_OUTCOMES = [
    "seg-info-1", "seg-info-2", "seg-info-unknown",
    "seg-map-token", "seg-map-page", "seg-map-several", "seg-map-refused",
    "far-object",
    "far-call-1", "far-latch-1", "far-exec-status",
    "far-call-2", "far-latch-2", "far-banks-differ",
    "far-aperture-closed", "far-marker", "far-buffer",
    "far-mappable-alloc", "far-mappable-same-bank",
    "bk-0-8", "bk-0-16", "bk-0-32",
    "bk-1-8", "bk-1-16", "bk-1-32",
    "bk-2-8", "bk-2-16", "bk-2-32",
    "bk-3-8", "bk-3-16", "bk-3-32",
    "bk-3-mix", "bk-recursion",
    "bk-2-bank", "bk-2-value",
    "bk-invalid-segment", "bk-aperture-closed",
]

FAR_CHECKS = len(FAR_OUTCOMES)
FAR_PASS = "xmem-far: PASS\r\n"
FAR_FAIL = "xmem-far: FAIL"

# The far code's own console line: a syscall from the far bank whose
# buffer is an aperture literal.  Segment 1's plain entry runs the console
# write, the exec and the object write; segment 2's plain entry and the
# banked entries do not.
FAR_CONSOLE = "xmem-far: far code says hello\r\n"

# SYS_SEG_MAP (abi/kos/seg.h): the RST 30h the syscall gate uses, the
# number in A and the two-byte argument block in HL, whose first byte is
# the segment id.
SYS_SEG_MAP = 0x27
RST_30H = 0xF7


class SegMapCallCounter:
    """Counts every SYS_SEG_MAP the image issues, by segment id.

    The far-code program's own checks map segment 1, segment 2 and two
    ids its container does not have, and segment 1's and segment 2's
    banked calls each spend one more: the token cache.  Any other call
    would be a hit that went to the kernel again.
    """

    def __init__(self):
        self.calls = {}

    def __call__(self, cpu):
        if cpu.rd(cpu.pc) != RST_30H or cpu.a != SYS_SEG_MAP:
            return
        segment = cpu.rd(cpu.hl)
        self.calls[segment] = self.calls.get(segment, 0) + 1


# The boot lanes every image prints before the boot program runs.
BOOT_LINES = [
    "KonaOS 0.0\r\n",
    "platform: Zephyr-80\r\n",
    "kernel entry: OK\r\n",
    "serial: OK\r\n",
    "console: driver OK\r\n",
    "xmem-selftest: begin\r\n",
]


class XmemSelftestReplay(Replay, unittest.TestCase):
    IMAGE_SUBDIR = IMAGE_SUBDIR
    IMAGE_NAME = IMAGE_NAME

    def setUp(self):
        self.load_image()

    def boot(self, limit=STEP_LIMIT):
        """Boot until the verdict or the launcher's exit report (the
        image's boot program runs once; the launcher would relaunch it)."""
        z = self.make_cpu()
        self.start_kernel(z)
        self.run_until(z, [PASS, FAIL, REPORT, PANIC],
                       limit=limit, halt_ok=True, panic=None)
        return z, z.sio_tx.decode("ascii", "replace")

    def boot_to_report(self, limit=STEP_LIMIT):
        """Boot past the program's verdict to the launcher's exit report,
        which is printed after the exited program's objects are gone."""
        z = self.make_cpu()
        self.start_kernel(z)
        self.run_until(z, [REPORT, PANIC], limit=limit, halt_ok=True,
                       panic=None)
        return z, z.sio_tx.decode("ascii", "replace")


class TestXmemSelftest(XmemSelftestReplay):
    def test_boot_lanes_and_verdict(self):
        z, sio = self.boot()

        for line in BOOT_LINES:
            self.assertIn(line, sio)
        self.assertNotIn(PANIC, sio)
        self.assertNotIn(FAIL, sio)
        for label in OUTCOMES:
            self.assertIn("xmem-selftest: %s OK\r\n" % label, sio)
        match = re.search(r"xmem-selftest: (\d+) checks, (\d+) failures\r\n",
                          sio)
        self.assertIsNotNone(match, sio)
        checks, failures = int(match.group(1)), int(match.group(2))
        self.assertEqual(failures, 0)
        self.assertEqual(checks, CHECKS)
        self.assertIn(PASS, sio)

    def test_outcomes_are_in_order(self):
        """Every outcome line arrives once, in the program's order."""
        _z, sio = self.boot()
        position = 0
        for label in OUTCOMES:
            line = "xmem-selftest: %s OK\r\n" % label
            found = sio.find(line, position)
            self.assertGreaterEqual(
                found, 0, "missing or out of order: %r in\n%s" % (line, sio))
            position = found + len(line)

    def test_nested_exec_from_far_context(self):
        z, sio = self.boot()
        position = 0
        for line in CHILD_LINES:
            found = sio.find(line, position)
            self.assertGreaterEqual(
                found, 0, "missing or out of order: %r in\n%s" % (line, sio))
            position = found + len(line)
        self.assertNotIn("xmem-child: FAIL", sio)

    def test_far_program_outcomes(self):
        """The far-code program's own verdict and every outcome line."""
        _z, sio = self.boot()
        self.assertIn(FAR_PASS, sio)
        self.assertNotIn(FAR_FAIL, sio)
        for label in FAR_OUTCOMES:
            self.assertIn("xmem-far: %s OK\r\n" % label, sio)
        match = re.search(r"xmem-far: (\d+) checks, (\d+) failures\r\n",
                          sio)
        self.assertIsNotNone(match, sio)
        checks, failures = int(match.group(1)), int(match.group(2))
        self.assertEqual(failures, 0)
        self.assertEqual(checks, FAR_CHECKS)

    def test_far_outcomes_are_in_order(self):
        _z, sio = self.boot()
        position = 0
        for label in FAR_OUTCOMES:
            line = "xmem-far: %s OK\r\n" % label
            found = sio.find(line, position)
            self.assertGreaterEqual(
                found, 0, "missing or out of order: %r in\n%s" % (line, sio))
            position = found + len(line)

    def test_far_code_syscall_buffer_is_the_far_bank(self):
        """The far code prints its far-area literal once: segment 1's
        entry makes a console syscall from the far bank with an aperture
        buffer, and the fixture here pins which segments make it - the
        banked entries must not."""
        _z, sio = self.boot()
        self.assertEqual(sio.count(FAR_CONSOLE), 1)

    def test_exit_released_the_parents_objects(self):
        """The launcher's report is the end-to-end release proof: the
        parent's anchors and the far program's segments and objects were
        all released (six free banks)."""
        _z, sio = self.boot_to_report()
        self.assertIn(REPORT, sio)

    def test_the_token_cache_costs_one_segmap_per_segment(self):
        """The trampoline maps a segment once (SYS_SEG_MAP) and reuses the
        token: the program's own checks spend one call each on segments 1
        and 2, the two refused ids spend one each, and the many banked
        calls - including the recursion and segment 2's call back into
        segment 1 - spend exactly one more per segment."""
        counter = SegMapCallCounter()
        z = self.make_cpu()
        self.start_kernel(z)
        self.run_until(z, [PASS, FAIL, REPORT, PANIC], limit=STEP_LIMIT,
                       halt_ok=True, panic=None, on_step=counter)
        self.assertEqual(counter.calls, {0: 1, 1: 2, 2: 2, 3: 1})

    def test_uncertainty_guard(self):
        """The aperture outcomes depend on the hardware latch, so the
        replay also pins that no FAIL verdict slipped through."""
        _z, sio = self.boot()
        self.assertNotIn("xmem-selftest: FAIL ", sio)
        self.assertNotIn("xmem-child: FAIL ", sio)
        self.assertNotIn("xmem-far: FAIL ", sio)


if __name__ == "__main__":
    unittest.main()
