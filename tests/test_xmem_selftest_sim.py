#!/usr/bin/env python3
"""Extended-memory outcome acceptance replay (work packages 43/44b).

Boots `build-xmem-selftest/zephyros-xmem-selftest.bin` -- the production
boot path with the SIO/console service set and the SDK outcome program
booting as `/SYSTEM/xmem_selftest.zex` -- and asserts the recorded
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
IMAGE_NAME = "zephyros-xmem-selftest.bin"

PASS = "xmem-selftest: PASS\r\n"
FAIL = "xmem-selftest: FAIL"
CHILD_PASS = "xmem-child: PASS\r\n"
PANIC = "PANIC"

# The launcher's exit report, printed after the program's exit.  It is
# the exit-release proof: the program leaves its two anchor objects live
# in bank 5, and `free-banks 6` says that bank is a free primary bank
# again (a bank with a live auxiliary slot can never be one).
REPORT = "monitor: exit status 0x0000 active 0 free-banks 6\r\n"

# The child's report, asserted in order.  It runs three times: from the
# parent (whose own bank is 1) and once per far call of the far-code
# program (whose child gets the lowest free bank, 2).  Every run proves
# the child starts with P = 0 (its own words, mode 10 over its bank) and
# that a child inherits no handle and no segment id.
CHILD_LINES = [
    "xmem-child: begin\r\n",
    "xmem-child: latch 11\r\n",
    "xmem-child: PASS\r\n",
    "xmem-child: begin\r\n",
    "xmem-child: latch 12\r\n",
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
# /SYSTEM/xmem_far.zex, SYS_EXECed by the parent above.
FAR_OUTCOMES = [
    "seg-info-1", "seg-info-2", "seg-info-unknown",
    "seg-map-token", "seg-map-page", "seg-map-several", "seg-map-refused",
    "far-object",
    "far-call-1", "far-latch-1", "far-exec-status",
    "far-call-2", "far-latch-2", "far-banks-differ",
    "far-aperture-closed", "far-marker", "far-buffer",
    "far-mappable-alloc", "far-mappable-same-bank",
]

FAR_CHECKS = len(FAR_OUTCOMES)
FAR_PASS = "xmem-far: PASS\r\n"
FAR_FAIL = "xmem-far: FAIL"

# The far code's own console line: a syscall from the far bank whose
# buffer is an aperture literal, printed once per far call.
FAR_CONSOLE = "xmem-far: far code says hello\r\n"

# The boot lanes every image prints before the boot program runs.
BOOT_LINES = [
    "ZephyrOS 0.0\r\n",
    "platform: Zephyr-80\r\n",
    "kernel entry: OK\r\n",
    "serial: OK\r\n",
    "console: service OK\r\n",
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
        """The far code prints its far-area literal once per far call: a
        console syscall from the far bank with an aperture buffer."""
        _z, sio = self.boot()
        self.assertEqual(sio.count(FAR_CONSOLE), 2)

    def test_exit_released_the_parents_objects(self):
        """The launcher's report is the end-to-end release proof: the
        parent's anchors and the far program's segments and objects were
        all released (six free banks)."""
        _z, sio = self.boot_to_report()
        self.assertIn(REPORT, sio)

    def test_uncertainty_guard(self):
        """The aperture outcomes depend on the hardware latch, so the
        replay also pins that no FAIL verdict slipped through."""
        _z, sio = self.boot()
        self.assertNotIn("xmem-selftest: FAIL ", sio)
        self.assertNotIn("xmem-child: FAIL ", sio)
        self.assertNotIn("xmem-far: FAIL ", sio)


if __name__ == "__main__":
    unittest.main()
