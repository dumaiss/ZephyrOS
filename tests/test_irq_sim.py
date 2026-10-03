#!/usr/bin/env python3
"""Host-side functional tests for the M2 interrupt executive.

Two layers:

1. Focused IM2 entry/return replay (normal image, always available):
   one interrupt is delivered with a full documented register context and
   the assembly gate must restore it bit-exactly for both memory modes,
   including alternate registers and the FFh guard path.

2. Full target-side self-test replay (irq-selftest image, skipped until
   `make irq-selftest` has run): executes the real `zos_run_irq_selftest`
   C driver - registry semantics, mode-10 delivery from banks 0 and 6,
   repeated delivery, the syscall-gate IFF restore probe, and the mode-11
   unclaimed path - injecting one CTC0 interrupt (vector 00h) per EI
   window that the smoke caller arms.

Physical hardware remains authoritative for CTC timing/acknowledgement
and daisy-chain behavior; these tests cover context save/restore and
executive policy only.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from z80sim import Z80, read_symbols  # noqa: E402

CALLER_BASE = 0x1000
VECTOR_CTC0 = 0x00
STEP_LIMIT = 5000000


def _find_call_site(page1, syms, caller, target):
    """Locate the return address after `call target` inside `caller`."""
    code = bytes(page1[a] for a in range(caller, caller + 0x100))
    call_bytes = bytes([0xCD, target & 0xFF, (target >> 8) & 0xFF])
    return caller + code.index(call_bytes) + 3


class TestIRQEntrySim(unittest.TestCase):
    """Focused replay of the real linked common entry/return bytes."""

    def setUp(self):
        image_path = os.path.join(ROOT, "build", "zephyros.bin")
        map_path = os.path.join(ROOT, "build", "zephyros.map")
        if not os.path.exists(image_path):
            self.skipTest("normal image missing; run make")
        image = open(image_path, "rb").read()
        self.page0, self.page1 = image[:0x10000], image[0x10000:]
        self.syms = read_symbols(map_path)

    def run_irq(self, latch, vector, stub):
        z = Z80(self.page0, self.page1)
        z.latch = latch
        z.i = 0xFD
        z.im = 2
        z.iff1 = z.iff2 = True
        z.pc = 0x1234
        z.sp = 0x4000 if (latch & 0x18) == 0x10 else 0xC800
        z.a, z.f = 0xA5, 0xD7
        z.b, z.c, z.d, z.e, z.h, z.l = 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC
        z.ix, z.iy = 0x1357, 0x2468
        z.alt_a, z.alt_f = 0x5A, 0x43
        z.alt_b, z.alt_c = 0xAA, 0xAA
        z.alt_d, z.alt_e = 0xBB, 0xBB
        z.alt_h, z.alt_l = 0xCC, 0xCC
        before = (z.pc, z.sp, z.a, z.f, z.b, z.c, z.d, z.e, z.h, z.l,
                  z.ix, z.iy,
                  z.alt_a, z.alt_f, z.alt_b, z.alt_c, z.alt_d, z.alt_e,
                  z.alt_h, z.alt_l, z.latch)

        # irq_arch_init installs vector 00h at runtime; this focused test
        # installs the stub target directly.  The C dispatcher is replaced
        # with RET so the test isolates the assembly entry/return path
        # while still crossing onto the bank-7 ISR stack.
        z.bank0[0xFD00] = stub & 0xFF
        z.bank0[0xFD01] = stub >> 8
        z.bank7[self.syms["_irq_dispatch"]] = 0xC9
        z.assert_int(vector)
        for _ in range(1000):
            z.step()
            if z.pc == 0x1234:
                break
        else:
            self.fail("IRQ entry did not return (PC=%04X)" % z.pc)

        after = (z.pc, z.sp, z.a, z.f, z.b, z.c, z.d, z.e, z.h, z.l,
                 z.ix, z.iy,
                 z.alt_a, z.alt_f, z.alt_b, z.alt_c, z.alt_d, z.alt_e,
                 z.alt_h, z.alt_l, z.latch)
        self.assertEqual(after, before)
        # No nesting: acceptance cleared both IFFs and RETI keeps them off.
        self.assertFalse(z.iff1)
        self.assertFalse(z.iff2)

    def test_ctc0_from_application_banks_0_and_6(self):
        for bank in (0, 6):
            with self.subTest(bank=bank):
                self.run_irq(0x10 | bank, 0x00, self.syms["irq_ctc0_stub"])

    def test_ctc0_from_os_mode(self):
        self.run_irq(0x1B, 0x00, self.syms["irq_ctc0_stub"])

    def test_ff_guard_path(self):
        target = self.page0[0xFDFF] | (self.page0[0xFE00] << 8)
        self.assertEqual(target, self.syms["irq_ff_unexpected"])
        self.run_irq(0x18, 0xFF, target)

    def test_arch_init_preserves_linked_vector_targets(self):
        z = Z80(self.page0, self.page1)
        z.latch = 0x18
        z.sp = 0xFEFE
        return_pc = 0x1234
        z.push16(return_pc)
        z.pc = self.syms["_irq_arch_init"]

        for _ in range(1024):
            z.step()
            if z.pc == return_pc:
                break
        else:
            self.fail("_irq_arch_init did not return")

        self.assertEqual(z.i, 0xFD)
        self.assertEqual(z.im, 2)
        self.assertTrue(z.iff1)
        self.assertTrue(z.iff2)
        self.assertEqual(z.rd16(0xFD00), self.syms["irq_ctc0_stub"])
        self.assertEqual(z.rd16(0xFDFF), self.syms["irq_ff_unexpected"])


def run_irq_selftest(page0, page1, syms):
    """Replay the full C driver with one injected interrupt per EI window."""
    z = Z80(page0, page1)
    z.latch = 0x18          # mode 11 over bank 0 (post-boot state)
    z.sp = 0xFFFE           # boot/runtime stack FF00h-FFFFh (never the
                            # FE80h-FEFFh IRQ transition stack)

    # Execute the real irq_init(): builds the registry, fills the IM2 page,
    # sets I=FDh, selects IM2, and enables interrupts.
    ret_init = _find_call_site(page1, syms, syms["_kernel_main"], syms["_irq_init"])
    z.pc = syms["_irq_init"]
    z.push16(ret_init)
    for _ in range(STEP_LIMIT):
        z.step()
        if z.pc == ret_init:
            break
    else:
        raise AssertionError("irq_init did not return (PC=%04X)" % z.pc)

    # Run the C driver.  Inject exactly one CTC0 interrupt whenever the
    # smoke caller (mode 10 over the caller window) or the mode-11 helper
    # enables interrupts with the source cell armed (FFh) - including the
    # HALT delivery windows.  The syscall-gate IFF probe runs with the
    # source cell at 00h and must NOT receive an injected interrupt.
    ret_site = _find_call_site(
        page1, syms, syms["_kernel_main"], syms["_zos_run_irq_selftest"]
    )
    z.pc = syms["_zos_run_irq_selftest"]
    z.push16(ret_site)

    blob_start = syms["zos_irq_test_blob_start"]
    blob_end = syms["zos_irq_test_blob_end"]
    blob_size = blob_end - blob_start
    helper_start = syms["_zos_irq_test_mode11"]
    helper_end = syms["zos_irq_test_mode11_end"]
    source_cell = syms["_irq_active_source"]
    # The cell is a simulator-only injection marker.  Its linked fill byte
    # is FFh, so establish an inactive value before the smoke caller arms it.
    z.wr(source_cell, VECTOR_CTC0)

    # Inject only when the smoke caller/helper itself enables interrupts
    # and the EI one-instruction delay has elapsed.  This avoids
    # delivering interrupts into the driver's own unlock-EI windows, the
    # syscall gate's EI, or the sentinel setup that precedes each window.
    armed = False
    pending_arm = 0
    for _ in range(STEP_LIMIT):
        op = z.rd(z.pc)
        in_blob = CALLER_BASE <= z.pc < CALLER_BASE + blob_size
        in_helper = helper_start <= z.pc < helper_end
        if (in_blob or in_helper) and op == 0xFB:
            pending_arm = 2  # EI executes now; one more instruction runs
        if armed and z.iff1 and not z.int_pending:
            if z.rd(source_cell) == 0xFF:
                z.assert_int(VECTOR_CTC0)
        z.step()
        if pending_arm > 0:
            pending_arm -= 1
            if pending_arm == 0:
                armed = True
        elif (in_blob or in_helper) and op == 0xF3 and not z.halted:
            armed = False
        if z.pc == ret_site:
            break
    else:
        raise AssertionError("irq selftest did not return to the driver (PC=%04X)" % z.pc)

    return z.bank0[syms["_zos_irq_test_result"]]


class TestIrqSelftestSim(unittest.TestCase):
    """Full target-side self-test replay (requires the irq-selftest image)."""

    def setUp(self):
        self.bin_path = os.path.join(ROOT, "build-irq-selftest", "zephyros-irq-selftest.bin")
        if not os.path.exists(self.bin_path):
            self.skipTest("irq-selftest image missing; run `make irq-selftest`")
        self.image = open(self.bin_path, "rb").read()
        map_path = os.path.join(ROOT, "build-irq-selftest", "zephyros.map")
        self.syms = read_symbols(map_path)

    def test_irq_selftest_full_round_trip(self):
        result = run_irq_selftest(
            self.image[:0x10000], self.image[0x10000:], self.syms
        )
        self.assertEqual(
            result, 0x00, "irq selftest failed with code 0x%02X" % result
        )


if __name__ == "__main__":
    unittest.main()
