#!/usr/bin/env python3
"""Subsystem acceptance tests: replay the subsystem image in the Z80
simulator.

Coverage (all executing the real linked bytes, on the current boot path):

* the machine boots the production path -- derived arena, VFS mounted
  from the ROMFS recovery store, the SIO fallback module loaded through
  `module_load_source` -- and the console service binds before the
  acceptance hooks run;
* the early hook runs the syscall gate and the interrupt executive
  selftests: `syscall selftest: OK` and `irq selftest: OK`;
* the late hook runs the ROMFS-reader, process/ZEX and loader
  selftests: `romfs selftest: OK`, `process selftest: OK`,
  `loader selftest: OK`, with the loader's arena-exhaustion progress
  line;
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

Skipped when the subsystem image has not been built.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import (Ctc0Injector, Z80, read_symbols,
                    skip_serial_connect_delay)  # noqa: E402
import romfspack  # noqa: E402

RET = 0x1234
STEP_LIMIT = 400000000
STACK = 0xFFFE
CALLER_BASE = 0x1000
VECTOR_CTC0 = 0x00
COMMON_BASE = 0xE000

# Acceptance markers, in boot order.  The early hook runs before the boot
# module load, so the syscall/IRQ lines precede the console-binding line.
SYSCALL_OK = "syscall selftest: OK"
IRQ_OK = "irq selftest: OK"
ROMFS_OK = "romfs selftest: OK"
PROCESS_OK = "process selftest: OK"
LOADER_OK = "loader selftest: OK"
LOADER_PROGRESS = "loader selftest: "
MONITOR_EXIT = "monitor: exit status 0x"
MONITOR_BANNER = "ZephyrOS native monitor"
MONITOR_PROMPT = "Z80> "
CONSOLE_BOUND = "console: dynamic sio0.b OK"

FAIL_MARKER = b"FAIL"

# Target result words: the acceptance tests publish one byte each; a
# non-zero value is a precise failure code.
RESULT_SYMBOLS = (
    "_zos_selftest_result",       # syscall gate (common window)
    "_zos_irq_test_result",       # interrupt executive (common window)
    "_romfs_selftest_result",     # ROMFS reader (bank-7 data)
    "_process_selftest_result",   # process/ZEX lifecycle (bank-7 data)
    "_loader_selftest_result",    # loader (bank-7 data)
)


class SubsystemSimBase(unittest.TestCase):
    IMAGE_SUBDIR = "build-subsys-selftest"
    IMAGE_NAME = "zephyros-subsys-selftest.bin"

    def setUp(self):
        self.bin_path = os.path.join(ROOT, self.IMAGE_SUBDIR, self.IMAGE_NAME)
        if not os.path.exists(self.bin_path):
            self.skipTest("%s missing; build the image first" % self.bin_path)
        with open(self.bin_path, "rb") as fh:
            self.image = fh.read()
        self.syms = read_symbols(
            os.path.join(ROOT, self.IMAGE_SUBDIR, "zephyros.map"))
        self.romfs_path = os.path.join(ROOT, self.IMAGE_SUBDIR,
                                       "recovery.romfs")
        if not os.path.exists(self.romfs_path):
            self.skipTest("%s missing; build the image first"
                          % self.romfs_path)
        with open(self.romfs_path, "rb") as fh:
            _header, entries = romfspack.parse_image(fh.read())
        self.entries = {entry["name"]: entry for entry in entries}

    def make_cpu(self, latch=0x18):
        pages = {n: self.image[n * 0x10000:(n + 1) * 0x10000]
                 for n in range(8)}
        z = Z80(pages[0], pages[1],
                rom_pages={n: pages[n] for n in range(2, 8)})
        z.latch = latch
        z.sp = STACK
        return z

    def skip_delay(self, z):
        skip_serial_connect_delay(
            z, self.syms["_kernel_serial_connect_delay"])

    def result_byte(self, z, symbol):
        """Read one target status byte from the window it lives in: the
        assembly-owned acceptance state is in the common area, the
        C-owned state in bank-7 data."""
        address = self.syms[symbol]
        return (z.bank0[address] if address >= COMMON_BASE
                else z.bank7[address])

    def start_kernel(self, z):
        z.push16(RET)
        z.pc = self.syms["__zos_crt0_init"]
        for _ in range(2000000):
            z.step()
            if z.pc == RET:
                break
        else:
            raise AssertionError("C runtime init did not return")
        z.push16(RET)
        z.wr(self.syms["_irq_active_source"], 0x00)
        z.pc = self.syms["_kernel_main"]
        self.watch = Ctc0Injector(self.syms)

    def run_to(self, z, marker, limit=STEP_LIMIT, since=0):
        """Step until `marker` (bytes) appears in the serial output."""
        needle = marker.encode("ascii") if isinstance(marker, str) else marker
        watch = getattr(self, "watch", None)
        for i in range(limit):
            self.skip_delay(z)
            if watch is not None:
                watch(z)
            z.step()
            if i % 0x3FFF == 0 and needle in bytes(z.sio_tx[since:]):
                return
        raise AssertionError(
            "did not reach %r (PC=%04X, tail=%r)"
            % (marker, z.pc, bytes(z.sio_tx)[-200:]))

    def run_to_idle(self, z, limit=STEP_LIMIT):
        """Run until the kernel idles (HALT) or the step budget ends."""
        for i in range(limit):
            self.skip_delay(z)
            z.step()
            if z.halted:
                return True
        return False


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
        base.run_to(z, LOADER_OK)
        cls.boot_base = base
        cls.z = z
        cls.out = bytes(z.sio_tx)

    def test_acceptance_groups_pass_on_the_current_boot_path(self):
        for marker in (SYSCALL_OK, IRQ_OK, CONSOLE_BOUND, ROMFS_OK,
                       PROCESS_OK, LOADER_OK):
            self.assertIn(marker.encode("ascii"), self.out)
        self.assertNotIn(FAIL_MARKER, self.out)

    def test_every_target_result_word_is_zero(self):
        for symbol in RESULT_SYMBOLS:
            self.assertIn(symbol, self.syms,
                          "%s is not linked in the subsystem image" % symbol)
            value = self.boot_base.result_byte(self.z, symbol)
            self.assertEqual(value, 0x00,
                             "%s reports failure 0x%02X"
                             % (symbol, value))

    def test_loader_exhausts_the_derived_arena(self):
        self.assertIn(b"fillers to exhaustion",
                      self.out.replace(b"\r\n", b"\n"))

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
        for name in ("sio.zmod", "monitor.zex", "crosspage.bin",
                     "m4probe.zmod", "m4fail.zmod", "m4filler.zmod",
                     "badmagic.zmod", "truncated.zmod",
                     "zextest.zex", "zex-return.zex", "zex-truncated.zex",
                     "zex-wrong-type.dat"):
            self.assertIn(name, self.entries)

    def test_malformed_fixtures_are_packed_but_invalid(self):
        bad = self.entries["badmagic.zmod"]
        short = self.entries["truncated.zmod"]
        self.assertEqual(short["size"], 0x1C)
        self.assertGreater(bad["size"], 0x1C)


if __name__ == "__main__":
    unittest.main()
