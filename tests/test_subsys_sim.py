#!/usr/bin/env python3
"""Subsystem acceptance tests: replay the subsystem image in the Z80
simulator.

Coverage (all executing the real linked bytes, on the current boot path):

* the machine boots the production path -- `vfs_init()`, the SIO and
  console driver images placed from the ROMFS recovery store by
  `driver_load_all` -- and the console driver binds before the
  acceptance hook runs;
* the late hook runs the ROMFS-reader selftest: `romfs selftest: OK`
  (the process/EXE lifecycle has its own images, tests/test_process_sim.py);
* the target result word is zero (no FAIL marker);
* the launcher then runs the recovery-store monitor as the boot program
  through the process path; the scripted session ends with the
  launcher's `boot: /system/monitor.exe exit 0` line and the monitor
  starts again (a second banner and prompt).

Skipped when the subsystem image has not been built.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from replay import Replay  # noqa: E402

STEP_LIMIT = 400000000

# Acceptance markers, in boot order.
ROMFS_OK = "romfs selftest: OK"
BOOT_EXIT = "boot: /system/monitor.exe exit 0"
MONITOR_BANNER = "KonaOS native monitor"
MONITOR_PROMPT = "Z80> "
CONSOLE_BOUND = "console: driver OK"

FAIL_MARKER = b"FAIL"

# Target result words: the acceptance tests publish one byte each; a
# non-zero value is a precise failure code.
RESULT_SYMBOLS = (
    "_romfs_selftest_result",     # ROMFS reader (bank-7 data)
)


class SubsystemSimBase(Replay, unittest.TestCase):
    IMAGE_SUBDIR = "build-subsys-selftest"
    IMAGE_NAME = "konaos-subsys-selftest.bin"

    def setUp(self):
        self.load_image()
        self.entries = self.romfs_entries()

    def run_to(self, z, marker, limit=STEP_LIMIT, since=0):
        """Step until `marker` (bytes or str) appears in the serial
        output."""
        return self.run_until(z, [marker], limit=limit, since=since)


class TestSubsystemBoot(SubsystemSimBase):
    """One boot serves every assertion: the subsystem image runs the whole
    acceptance surface in a single pass, so re-running the boot per test
    would only cost simulator time."""

    BOOT = None

    @classmethod
    def setUpClass(cls):
        base = SubsystemSimBase()
        base.setUp()
        cls.syms = base.syms
        cls.entries = base.entries
        z = base.make_cpu()
        base.start_kernel(z)
        base.run_to(z, CONSOLE_BOUND)
        base.run_to(z, ROMFS_OK)
        cls.boot_base = base
        cls.z = z
        cls.out = bytes(z.sio_tx)

    def test_acceptance_groups_pass_on_the_current_boot_path(self):
        for marker in (CONSOLE_BOUND, ROMFS_OK):
            self.assertIn(marker.encode("ascii"), self.out)
        self.assertNotIn(FAIL_MARKER, self.out)

    def test_every_target_result_word_is_zero(self):
        for symbol in RESULT_SYMBOLS:
            self.assertIn(symbol, self.syms,
                          "%s is not linked in the subsystem image" % symbol)
            value = self.boot_base.read(self.z, symbol)
            self.assertEqual(value, 0x00,
                             "%s reports failure 0x%02X"
                             % (symbol, value))

    def test_monitor_session_runs_and_relaunches_after_exit(self):
        # The store's monitor is the boot program here, and the launcher
        # resets the console before it starts: the session's input goes
        # in once the monitor's prompt is up.
        start = len(self.z.sio_tx)
        self.boot_base.run_to(self.z, MONITOR_PROMPT, since=start)
        self.assertIn(MONITOR_BANNER.encode("ascii"), bytes(self.z.sio_tx))
        start = len(self.z.sio_tx)
        self.z.sio_rx.extend(b"Q\r")
        self.boot_base.run_to(self.z, BOOT_EXIT, since=start)
        self.assertIn(BOOT_EXIT.encode("ascii"), bytes(self.z.sio_tx))
        # A normal exit is followed by one relaunch, not by an idle loop.
        start = len(self.z.sio_tx)
        self.boot_base.run_to(self.z, MONITOR_PROMPT, since=start)
        out = bytes(self.z.sio_tx)
        self.assertGreaterEqual(out.count(MONITOR_BANNER.encode("ascii")), 2)



class TestSubsystemManifest(SubsystemSimBase):
    def test_manifest_carries_the_current_boot_set_and_fixtures(self):
        for name in ("sio.mod", "console-serial.mod", "drivers.man",
                     "monitor.exe", "crosspage.bin"):
            self.assertIn(name, self.entries)


if __name__ == "__main__":
    unittest.main()
