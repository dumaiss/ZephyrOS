#!/usr/bin/env python3
"""Process/EXE acceptance tests: replay the process and exe2 images in the
Z80 simulator.

One suite (tests/target/process/process_test.c) over two images, because
no single image's cold half holds it:

* the process image (KOS_PROCESS_SELFTEST): exetest from ROMFS, repeated
  exec/exit cycles, a two-bank run, the raw-entry return, every malformed
  container rejected with its exact code, a truncated store entry,
  allocator exhaustion and argument errors, ROMFS resource imports;
* the exe2 image (KOS_EXE2_SELFTEST): the EXE v2 loader outcomes - BSS
  zeroed with the bank left as it was, DATA segments, a container over
  64 KiB, segment-allocation rollback.

Each boots the production path (SIO and console drivers from the recovery
store), reports `process selftest: OK` from the late hook with a zero
result word, then runs the store's monitor as the boot program.  Skipped
when an image has not been built.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from test_subsys_sim import (  # noqa: E402
    BOOT_EXIT, CONSOLE_BOUND, FAIL_MARKER, MONITOR_BANNER, MONITOR_PROMPT,
    SubsystemSimBase)

PROCESS_OK = "process selftest: OK"
EXE2_OK = "process selftest: exe v2 outcomes OK"
EXE2_LINES = ("process selftest: exe2 BSS/bank OK",
              "process selftest: exe2 segment OK",
              "process selftest: exe2 big container OK",
              "process selftest: exe2 rollback OK")


class ProcessBootBase(SubsystemSimBase):
    """One boot per image serves every assertion of that image."""

    STORE = ()

    @classmethod
    def setUpClass(cls):
        base = cls()
        base.setUp()
        cls.syms = base.syms
        cls.entries = base.entries
        z = base.make_cpu()
        base.start_kernel(z)
        base.run_to(z, CONSOLE_BOUND)
        base.run_to(z, PROCESS_OK)
        cls.boot_base = base
        cls.z = z
        cls.out = bytes(z.sio_tx)

    def check_suite(self, markers):
        for marker in (CONSOLE_BOUND,) + markers + (PROCESS_OK,):
            self.assertIn(marker.encode("ascii"), self.out)
        self.assertNotIn(FAIL_MARKER, self.out)
        self.assertEqual(self.boot_base.read(self.z,
                                             "_process_selftest_result"), 0)

    def check_store(self):
        for name in ("sio.mod", "console-serial.mod", "drivers.man",
                     "monitor.exe") + self.STORE:
            self.assertIn(name, self.entries)

    def check_monitor_relaunch(self):
        start = len(self.z.sio_tx)
        self.boot_base.run_to(self.z, MONITOR_PROMPT, since=start)
        self.assertIn(MONITOR_BANNER.encode("ascii"), bytes(self.z.sio_tx))
        start = len(self.z.sio_tx)
        self.z.sio_rx.extend(b"Q\r")
        self.boot_base.run_to(self.z, BOOT_EXIT, since=start)
        start = len(self.z.sio_tx)
        self.boot_base.run_to(self.z, MONITOR_PROMPT, since=start)
        self.assertGreaterEqual(
            bytes(self.z.sio_tx).count(MONITOR_BANNER.encode("ascii")), 2)


class TestProcessImage(ProcessBootBase):
    IMAGE_SUBDIR = "build-process-selftest"
    IMAGE_NAME = "konaos-process-selftest.bin"
    STORE = ("exetest.exe", "exe-return.exe", "exe-truncated.exe",
             "exe-wrong-type.dat")

    def test_the_lifecycle_groups_pass(self):
        self.check_suite(())
        # The v2 loader outcomes are the exe2 image's.
        self.assertNotIn(EXE2_OK.encode("ascii"), self.out)

    def test_the_store_carries_the_fixtures(self):
        self.check_store()
        self.assertNotIn("exe2probe.exe", self.entries)

    def test_monitor_runs_after_the_suite(self):
        self.check_monitor_relaunch()


class TestExe2Image(ProcessBootBase):
    IMAGE_SUBDIR = "build-exe2-selftest"
    IMAGE_NAME = "konaos-exe2-selftest.bin"
    STORE = ("exe2probe.exe", "exe2seg.exe", "exe2big.exe")

    def test_the_v2_loader_outcomes_pass(self):
        self.check_suite(EXE2_LINES + (EXE2_OK,))

    def test_the_store_carries_the_v2_programs(self):
        self.check_store()
        self.assertNotIn("exetest.exe", self.entries)

    def test_monitor_runs_after_the_suite(self):
        self.check_monitor_relaunch()


if __name__ == "__main__":
    unittest.main()
