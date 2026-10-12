#!/usr/bin/env python3
"""Nucleus-layout acceptance replay (work package 38, requirements 5-6).

Boots build-nucleus-selftest/konaos-nucleus-selftest.bin -- the
production boot path with the revision-13 gate and upcall-shim acceptance
surface linked in -- and checks the recorded evidence, not just the
transcript:

* the smoke caller runs in application bank 0 with the driver page
  selected (mode 10 with P = 1) and issues SYS_DIAG_SYSTEM_INFO, whose
  body is cold-half code: only a gate that clears P can dispatch it, and
  only the cold half carries the deterministic record the caller compares
  against;
* CTC0 is delivered twice: once in the caller's own mode-10 window, and
  once while the upcall shim has bank 6 mapped in mode 11.  The handler
  records the latch it observed (P = 0, mode 11, the interrupted bank) and
  that it ran on the fixed-core ISR stack;
* the upcall shim reaches the bank-6 stub through the window, passes the
  argument through, publishes the target's result and restores the exact
  latch - including the page the caller had selected;
* bank 7's bytes at the stub's window address are not the stub, so a shim
  that failed to select the page could not look like a pass.

The simulator injects the CTC0 deliveries itself (there is no timer
model), arming on the test's own source cell and instruction windows, so
the replay drives the real gate bytes.

Skipped when the image has not been built.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from replay import Replay, STEP_LIMIT  # noqa: E402

RET = 0x1234
COMMON_BASE = 0xE000

IMAGE_SUBDIR = "build-nucleus-selftest"
IMAGE_NAME = "konaos-nucleus-selftest.bin"

OK_MARKER = "nucleus selftest: OK"
FAIL_MARKER = "nucleus selftest: FAIL"

# Recorded evidence (mirrors tests/target/include/nucleus_selftest.h).
APP_BANK = 0x00
PAGE_DRIVER = 0x01
MODE_APPLICATION = 0x10
MODE_OS = 0x18
EV_ALL = 0x0F
STUB_MAGIC = 0xA55A
STUB_WITNESS = 0x3C
STUB_STATUS_DELIVERED = 0x01
STUB_BASE = 0x7800
DRIVER_BANK = 6


class NucleusCtc0Injector:
    """Host stand-in for CTC0 in the nucleus-layout replay.

    The simulator models no timer, so the two deliveries are injected by
    the host: the smoke caller (copied into the caller window) and the
    bank-6 upcall stub both arm the kernel's source cell and enable
    interrupts; one IM2 interrupt at vector 00h is injected while the
    program counter is inside one of those instruction windows.
    """

    def __init__(self, syms, blob_base, stub_base):
        self.source_cell = syms["_irq_active_source"]
        self.blob = (blob_base,
                     blob_base + (syms["kos_nucleus_blob_end"] -
                                  syms["kos_nucleus_blob_start"]))
        self.stub = (stub_base,
                     stub_base + (syms["kos_nucleus_stub_end"] -
                                  syms["kos_nucleus_stub_start"]))
        self.armed = False
        self.pending = 0
        self.deliveries = 0

    def _in_window(self, pc):
        return any(start <= pc < end for start, end in (self.blob, self.stub))

    def __call__(self, cpu):
        op = cpu.rd(cpu.pc)
        in_window = self._in_window(cpu.pc)
        if in_window and op == 0xFB:  # EI
            self.pending = 2
        if self.armed and cpu.iff1 and not cpu.int_pending:
            if cpu.rd(self.source_cell) == 0xFF:
                cpu.assert_int(0x00)
                self.deliveries += 1
        if self.pending > 0:
            self.pending -= 1
            if self.pending == 0:
                self.armed = True
        elif in_window and op == 0xF3 and not cpu.halted:  # DI
            self.armed = False


class NucleusSimBase(Replay, unittest.TestCase):
    IMAGE_SUBDIR = IMAGE_SUBDIR
    IMAGE_NAME = IMAGE_NAME

    def setUp(self):
        self.load_image()

    def boot(self, z, limit=STEP_LIMIT):
        """Run the kernel to the acceptance marker.  Returns the
        injector and the step count."""
        watch = NucleusCtc0Injector(self.syms, 0x1000, STUB_BASE)
        self.start_kernel(z, ctc0=True)
        steps = self.run_until(z, (OK_MARKER, FAIL_MARKER, b"PANIC"),
                               limit=limit, on_step=watch, halt_ok=True)
        return watch, steps


class TestNucleusLayout(NucleusSimBase):
    """One boot serves every assertion (the image runs the whole surface in
    a single pass), so the replay is split by assertion, not by run."""

    def reg16(self, symbol):
        """One word of the image's published evidence."""
        return self.read16(self.boot_once(), symbol)

    def reg(self, symbol):
        """One byte of the image's published evidence (the mixin's `read`
        takes the machine as its first argument)."""
        return self.read(self.boot_once(), symbol)

    def boot_once(self):
        if getattr(self, "_watch", None) is None:
            z = self.make_cpu()
            watch, steps = self.boot(z)
            self._z = z
            self._watch = watch
            self._sio = bytes(z.sio_tx).decode("ascii", "replace")
            self._steps = steps
        return self._z

    def test_boot_and_acceptance_marker(self):
        self.boot_once()
        for line in ("KonaOS 0.0", "platform: Zephyr-80",
                     "kernel entry: OK", "serial: OK"):
            self.assertIn(line + "\r\n", self._sio)
        self.assertIn(OK_MARKER, self._sio)
        self.assertNotIn("FAIL", self._sio)
        self.assertNotIn("PANIC", self._sio)
        self.assertEqual(self.reg("_kos_nucleus_result"), 0)
        self.assertEqual(self._watch.deliveries, 2,
                         "the replay injected %d CTC0 deliveries, not 2"
                         % self._watch.deliveries)
        print("nucleus selftest: %d steps, %d CTC0 deliveries"
              % (self._steps, self._watch.deliveries))

    def test_syscall_and_irq_gates_normalize_the_page_field(self):
        self.boot_once()
        # The caller's latch (mode 10, bank 0, page 1) came back exactly.
        self.assertEqual(self.reg("kos_nucleus_p_latch"),
                         MODE_APPLICATION | APP_BANK |
                         (PAGE_DRIVER << 5))
        # The mode-10 delivery: the handler is cold-half code, so this can
        # only pass with P = 0; it recorded the latch it observed.
        self.assertEqual(self.reg("_kos_nucleus_irq_evidence"), EV_ALL)
        self.assertEqual(self.reg("_kos_nucleus_irq_latch"),
                         MODE_OS | APP_BANK)

    def test_irq_during_an_upcall_normalizes_and_restores(self):
        self.boot_once()
        # The delivery arrived while the shim had bank 6 mapped; the
        # handler still ran (bank-7 cold code) with P = 0 ...
        self.assertEqual(
            self.reg("_kos_nucleus_upcall_irq_evidence"), EV_ALL)
        self.assertEqual(self.reg("_kos_nucleus_upcall_irq_latch"),
                         MODE_OS | APP_BANK)
        # ... and the interrupted page came back before the stub resumed.
        self.assertEqual(self.reg("_kos_nucleus_stub_status"),
                         STUB_STATUS_DELIVERED)
        self.assertEqual(self.reg("_kos_nucleus_stub_latch"),
                         MODE_OS | APP_BANK | (PAGE_DRIVER << 5))
        self.assertEqual(self.reg("_kos_nucleus_stub_latch_after"),
                         MODE_OS | APP_BANK | (PAGE_DRIVER << 5))

    def test_upcall_shim_selects_the_page_and_publishes_the_result(self):
        self.boot_once()
        # The shim's own request state: the target ran inside the window.
        self.assertEqual(self.reg("_kos_nucleus_stub_witness"),
                         STUB_WITNESS)
        self.assertEqual(self.reg16("_kos_nucleus_stub_args"),
                         self.syms["_kos_nucleus_upcall_arg"])
        self.assertEqual(self.reg16("kos_upcall_result"), STUB_MAGIC)
        # The install helper copied the stub into bank 6 through the window.
        stub_size = (self.syms["kos_nucleus_stub_end"] -
                     self.syms["kos_nucleus_stub_start"])
        want = self.linked_bank7(self.syms["kos_nucleus_stub_start"],
                                  stub_size)
        self.assertEqual(
            bytes(self._z.banks[DRIVER_BANK][STUB_BASE:STUB_BASE +
                                               stub_size]),
            want, "the bank-6 stub is not the linked stub image")

    def test_upcall_shim_restores_the_caller_latch(self):
        self.boot_once()
        # The fixed install helper recorded the caller's latch, and the
        # driver only reaches the OK marker when the shim restored exactly
        # that value after the call.
        self.assertEqual(self.reg("kos_nucleus_upcall_latch0"),
                         MODE_OS | APP_BANK)

    def test_the_page_selection_is_load_bearing(self):
        """Bank 7's bytes at the stub's window address differ from the
        stub, so executing the unselected page could not look like a pass,
        and the two copied routines live in the fixed core."""
        self.boot_once()
        stub_size = (self.syms["kos_nucleus_stub_end"] -
                     self.syms["kos_nucleus_stub_start"])
        stub = self.linked_bank7(self.syms["kos_nucleus_stub_start"],
                                  stub_size)
        at_window = self.linked_bank7(STUB_BASE, stub_size)
        self.assertNotEqual(stub, at_window,
                            "bank 7 carries the stub bytes at 0x%04X; the "
                            "page-selection proof would be vacuous"
                            % STUB_BASE)
        for name in ("kos_nucleus_stub_start", "kos_nucleus_blob_start"):
            self.assertLess(self.syms[name], self.syms["s__WINDOW"],
                            "%s is not in the fixed core" % name)


if __name__ == "__main__":
    unittest.main()
