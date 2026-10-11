#!/usr/bin/env python3
"""Subsystem acceptance tests: replay the subsystem image in the Z80
simulator.

Coverage (all executing the real linked bytes, on the current boot path):

* the machine boots the production path -- `vfs_init()`, the SIO and
  console service images placed from the ROMFS recovery store by
  `service_load_all` -- and the console service binds before the
  acceptance hooks run;
* the early hook runs the syscall gate and the interrupt executive
  selftests: `syscall selftest: OK` and `irq selftest: OK`;
* the late hook runs the ROMFS-reader and process/ZEX selftests:
  `romfs selftest: OK` and `process selftest: OK`;
* every target result word is zero (no FAIL marker): the assembly-owned
  status words live in the common window, the C-owned ones in bank-7
  data;
* the interrupt-executive replay injects one CTC0 interrupt (IM2 vector
  00h) per arming window the smoke caller opens - the same injection the
  standalone M2 replay used - so the IRQ group is exercised rather than
  merely linked;
* the monitor launcher then execs the recovery-store monitor through the
  same process path; the scripted session ends with
  `monitor: exit status 0x` and the launcher re-execs the monitor (a
  second banner and prompt), exactly as the subsystem acceptance records.

The nucleus-layout group (work package 38) stays its own image: its
common-memory state cannot fit the subsystem image's ledger (work
package 42d tried the merge and reverted it; see the package record).

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

# Acceptance markers, in boot order.  The early hook runs before the boot
# module load, so the syscall/IRQ lines precede the console-binding line.
SYSCALL_OK = "syscall selftest: OK"
IRQ_OK = "irq selftest: OK"
ROMFS_OK = "romfs selftest: OK"
PROCESS_OK = "process selftest: OK"
MONITOR_EXIT = "monitor: exit status 0x"
MONITOR_BANNER = "ZephyrOS native monitor"
MONITOR_PROMPT = "Z80> "
CONSOLE_BOUND = "console: service OK"

FAIL_MARKER = b"FAIL"

# Target result words: the acceptance tests publish one byte each; a
# non-zero value is a precise failure code.
RESULT_SYMBOLS = (
    "_zos_selftest_result",       # syscall gate (common window)
    "_zos_irq_test_result",       # interrupt executive (common window)
    "_romfs_selftest_result",     # ROMFS reader (bank-7 data)
    "_process_selftest_result",   # process/ZEX lifecycle (bank-7 data)
)


class SubsystemSimBase(Replay, unittest.TestCase):
    IMAGE_SUBDIR = "build-subsys-selftest"
    IMAGE_NAME = "zephyros-subsys-selftest.bin"

    def setUp(self):
        self.load_image()
        self.entries = self.romfs_entries()
        self.watch = None

    def start_kernel(self, z, host_sync=b"", ctc0=False):
        super().start_kernel(z, host_sync, ctc0)
        self.watch = self.injector(z)
        return z

    def run_to(self, z, marker, limit=STEP_LIMIT, since=0):
        """Step until `marker` (bytes or str) appears in the serial
        output, with the host CTC0 injector armed until `run_to` is
        called with `watch=None`."""
        return self.run_until(z, [marker], limit=limit, since=since,
                              on_step=self.watch if self.watch else None)


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
        # Early hook: syscall gate, then the interrupt executive with the
        # host standing in for CTC0.  The injector is disabled as soon as
        # the IRQ group reports, so no later group can be disturbed.
        base.run_to(z, SYSCALL_OK)
        base.run_to(z, "irq selftest: ")
        base.watch = None
        base.run_to(z, CONSOLE_BOUND)
        base.run_to(z, ROMFS_OK)
        base.run_to(z, PROCESS_OK)
        cls.boot_base = base
        cls.z = z
        cls.out = bytes(z.sio_tx)

    def test_acceptance_groups_pass_on_the_current_boot_path(self):
        for marker in (SYSCALL_OK, IRQ_OK, CONSOLE_BOUND, ROMFS_OK,
                       PROCESS_OK):
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
        start = len(self.z.sio_tx)
        self.z.sio_rx.extend(b"Q\r")
        self.boot_base.run_to(self.z, MONITOR_EXIT, since=start)
        self.assertIn(b"monitor: exit status 0x", bytes(self.z.sio_tx))
        # A normal exit is followed by one relaunch, not by an idle loop.
        start = len(self.z.sio_tx)
        self.boot_base.run_to(self.z, MONITOR_PROMPT, since=start)
        out = bytes(self.z.sio_tx)
        self.assertGreaterEqual(out.count(MONITOR_BANNER.encode("ascii")), 2)



class TestSubsystemManifest(SubsystemSimBase):
    def test_manifest_carries_the_current_boot_set_and_fixtures(self):
        for name in ("sio.zmod", "console-serial.zmod", "services.man",
                     "monitor.zex", "crosspage.bin",
                     "zextest.zex", "zex-return.zex", "zex-truncated.zex",
                     "zex-wrong-type.dat"):
            self.assertIn(name, self.entries)


if __name__ == "__main__":
    unittest.main()
