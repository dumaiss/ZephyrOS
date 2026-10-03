#!/usr/bin/env python3
"""Host-side functional test: replay the target-side syscall self-test in
a minimal Z80 simulator.

Runs the real smoke-caller round trip from the selftest image for
application banks 0 and 6 and asserts the result byte is ZOS_ST_OK.  This
is the regression net that caught the gate's BC-clobber and the missing
fail-code stores; it exercises the exact machine code that runs on the
target.

Skipped when the self-test image has not been built
(`make syscall-selftest`).
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from z80sim import Z80, read_symbols  # noqa: E402


def run_selftest_bank(page0, page1, syms, bank):
    z = Z80(page0, page1)
    run = syms["_zos_selftest_run"]
    driver = syms["_zos_run_syscall_selftest"]
    code = bytes(page1[a] for a in range(driver, driver + 0x80))
    call_bytes = bytes([0xCD, run & 0xFF, (run >> 8) & 0xFF])
    ret_site = driver + code.index(call_bytes) + 3

    z.latch = 0x18
    z.sp = 0xFEFE
    z.pc = run
    z.a = bank
    z.push16(ret_site)
    z.sp = 0xFEFC

    for _ in range(2000000):
        z.step()
        if z.halted or z.pc == ret_site:
            break
    else:
        self_error = "simulation did not return to the driver (PC=%04X)" % z.pc
        raise AssertionError(self_error)

    return z.bank0[syms["_zos_selftest_result"]]


class TestSyscallSelftestSim(unittest.TestCase):
    def setUp(self):
        self.bin_path = os.path.join(ROOT, "build-selftest", "zephyros-selftest.bin")
        if not os.path.exists(self.bin_path):
            self.skipTest("selftest image missing; run `make syscall-selftest`")
        self.image = open(self.bin_path, "rb").read()
        map_path = os.path.join(ROOT, "build-selftest", "zephyros.map")
        self.syms = read_symbols(map_path)

    def test_bank0_round_trip(self):
        result = run_selftest_bank(
            self.image[:0x10000], self.image[0x10000:], self.syms, 0
        )
        self.assertEqual(result, 0x00, "bank 0 round trip failed with code 0x%02X" % result)

    def test_bank6_round_trip(self):
        result = run_selftest_bank(
            self.image[:0x10000], self.image[0x10000:], self.syms, 6
        )
        self.assertEqual(result, 0x00, "bank 6 round trip failed with code 0x%02X" % result)


if __name__ == "__main__":
    unittest.main()
