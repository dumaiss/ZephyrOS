#!/usr/bin/env python3
"""Host-side M6 functional test: replay the process/ZEX acceptance images
in the Z80 simulator.

Coverage (all executing the real linked bytes):

* production-style boot of the M6 success image (SIO loaded from ROMFS,
  console bound) followed by the complete target selftest: zextest runs
  from ROMFS, prints through the console syscall, exits with PASS|bank,
  the parent resumes, repeated cycles and a reserved-bank two-bank run
  pass, a raw entry return exits with ZOS_EFAULT, and every malformed
  fixture is rejected with its exact code;
* byte-identity of the loaded ZEX image in banks 0 and 1 against the
  packed .zex payload (the "identical bytes in two banks" criterion);
* the process table is all-FREE afterwards (no slot/bank leak);
* the failure image: the truncated ZEX is rejected exactly, then the
  valid copy runs;
* focused tests of the common copy gates (complete latch/IFF/stack
  restoration, all transfer directions, zero fill, bank-7 rejection);
* a negative control: one mutated image byte makes the target selftest
  fail with the status-mismatch code (the checks are real).

Skipped when the images / packed modules have not been built.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import Z80, read_symbols  # noqa: E402
import romfspack  # noqa: E402

RET = 0x1234
STEP_LIMIT = 120000000
STACK = 0xFFFE

ZOS_EINVAL = 0x02

PROCESS_FREE = 0x00
PROCESS_NO_PROCESS = 0xFF

ZEX_LOAD_BASE = 0x0100
ZEX_STACK_TOP = 0xE000


def le16(data, at):
    return data[at] | (data[at + 1] << 8)


def run_until_halt(z, limit=STEP_LIMIT, skip_delay=None):
    for _ in range(limit):
        z.step()
        if skip_delay is not None and z.pc == skip_delay:
            z.pc = z.pop16()
        if z.halted:
            return
    raise AssertionError("replay did not halt (PC=%04X)" % z.pc)


class M6SimBase(unittest.TestCase):
    IMAGE_SUBDIR = "build-m6-selftest"
    IMAGE_NAME = "zephyros-m6-selftest.bin"

    def setUp(self):
        self.bin_path = os.path.join(ROOT, self.IMAGE_SUBDIR, self.IMAGE_NAME)
        if not os.path.exists(self.bin_path):
            self.skipTest("%s missing; build the image first" % self.bin_path)
        with open(self.bin_path, "rb") as fh:
            self.image = fh.read()
        self.syms = read_symbols(os.path.join(
            ROOT, self.IMAGE_SUBDIR, "zephyros.map"))
        self.romfs_path = os.path.join(ROOT, self.IMAGE_SUBDIR,
                                       "recovery.romfs")
        if not os.path.exists(self.romfs_path):
            self.skipTest("%s missing; build the image first"
                          % self.romfs_path)
        with open(self.romfs_path, "rb") as fh:
            stream = fh.read()
        _header, entries = romfspack.parse_image(stream)
        self.entries = {entry["name"]: entry for entry in entries}

    def make_cpu(self, latch=0x18):
        pages = {n: self.image[n * 0x10000:(n + 1) * 0x10000]
                 for n in range(8)}
        z = Z80(pages[0], pages[1],
                rom_pages={n: pages[n] for n in range(2, 8)})
        z.latch = latch
        z.sp = STACK
        return z

    def boot_kernel(self, z):
        z.push16(RET)
        z.pc = self.syms["__zos_crt0_init"]
        for _ in range(STEP_LIMIT):
            z.step()
            if z.pc == RET:
                break
        else:
            raise AssertionError("__zos_crt0_init did not return")
        z.push16(RET)
        z.pc = self.syms["_kernel_main"]
        run_until_halt(z, skip_delay=self.syms["_kernel_serial_connect_delay"])

    def call(self, z, addr, a=None, hl=None, de=None, bc=None,
             limit=STEP_LIMIT):
        z.push16(RET)
        if a is not None:
            z.a = a
        if hl is not None:
            z.hl = hl
        if de is not None:
            z.de = de
        if bc is not None:
            z.bc = bc
        z.pc = addr
        for _ in range(limit):
            z.step()
            if z.pc == RET:
                return
        raise AssertionError("function at %04X did not return (PC=%04X)"
                             % (addr, z.pc))

    def process_table_state(self, z):
        base = self.syms["_process_table"]
        return [z.bank7[base + i * 8] for i in range(7)]

    def zex_payload(self, name):
        entry = self.entries[name]
        return entry["payload"]

    def assert_clean_table(self, z):
        self.assertEqual(self.process_table_state(z), [PROCESS_FREE] * 7,
                         "process table leaked a slot")
        self.assertEqual(z.bank7[self.syms["_process_running_pid"]],
                         PROCESS_NO_PROCESS)


class TestM6AcceptanceReplay(M6SimBase):
    def test_success_image_runs_the_full_selftest(self):
        z = self.make_cpu()
        self.boot_kernel(z)
        tx = bytes(z.sio_tx)

        self.assertIn(b"ZephyrOS 0.0\r\n", tx)
        self.assertIn(b"console: dynamic sio0.b OK\r\n", tx)
        self.assertIn(b"zextest: bank=0 pid=0\r\n", tx)
        self.assertIn(b"zextest: OK\r\n", tx)
        self.assertIn(b"m6 selftest: zextest status 0xA500\r\n", tx)
        self.assertIn(b"m6 selftest: repeated cycles OK\r\n", tx)
        self.assertIn(b"m6 selftest: zextest status 0xA501\r\n", tx)
        self.assertIn(b"m6 selftest: two-bank OK\r\n", tx)
        self.assertIn(b"m6 selftest: raw return EFAULT\r\n", tx)
        self.assertIn(b"m6 selftest: malformed rejected\r\n", tx)
        self.assertIn(b"m6 selftest: OK\r\n", tx)
        self.assertNotIn(b"FAIL", tx)
        self.assertNotIn(b"PANIC", tx)
        self.assertEqual(z.bank7[self.syms["_m6_selftest_result"]], 0x00)
        self.assert_clean_table(z)

        # Byte-identity: bank 1 still holds the zextest image from the
        # two-bank run and must equal the packed payload exactly.  Bank 0
        # was reused afterwards by the raw-return control, so it must
        # hold that program's bytes instead.
        payload = self.zex_payload("zextest.zex")
        image = payload[20:]
        self.assertEqual(
            bytes(z.banks[1][ZEX_LOAD_BASE:ZEX_LOAD_BASE + len(image)]),
            image, "bank 1 content differs from the packed ZEX")
        ret_payload = self.zex_payload("zex-return.zex")
        self.assertEqual(
            bytes(z.bank0[ZEX_LOAD_BASE:
                         ZEX_LOAD_BASE + len(ret_payload) - 20]),
            ret_payload[20:],
            "bank 0 does not hold the last loaded program's bytes")
        # Both banks carry the same deterministic page-zero vector.
        vector = bytes((0xC3, self.syms["zos_syscall_entry"] & 0xFF,
                        self.syms["zos_syscall_entry"] >> 8))
        self.assertEqual(bytes(z.bank0[0x30:0x33]), vector)
        self.assertEqual(bytes(z.banks[1][0x30:0x33]), vector)

        # The reserved stack never overlaps image/BSS (declared layout).
        header = payload[:20]
        image_size = le16(header, 0x0A)
        bss_size = le16(header, 0x0C)
        stack_size = le16(header, 0x10)
        self.assertLessEqual(ZEX_LOAD_BASE + image_size + bss_size,
                             ZEX_STACK_TOP - stack_size)

    def test_mutated_image_byte_fails_the_selftest(self):
        """Negative control: the target checks are real."""
        z = self.make_cpu()
        payload = self.zex_payload("zextest.zex")
        sentinel = payload.index(bytes((0xDE, 0xAD, 0xBE, 0xEF)))
        entry = self.entries["zextest.zex"]
        logical = entry["offset"] + sentinel
        page = 2 + logical // 0xC000
        offset = logical % 0xC000
        z.rom[page][offset] ^= 0xFF
        self.boot_kernel(z)
        tx = bytes(z.sio_tx)
        self.assertIn(b"zextest: FAIL 0x02\r\n", tx)
        self.assertIn(b"m6 selftest: FAIL 0x03 detail 0x0102\r\n", tx)
        self.assertNotIn(b"m6 selftest: OK", tx)
        self.assertEqual(z.bank7[self.syms["_m6_selftest_result"]], 0x03)
        self.assert_clean_table(z)


class TestM6FailureImageReplay(M6SimBase):
    IMAGE_SUBDIR = "build-m6-failure-selftest"
    IMAGE_NAME = "zephyros-m6-failure.bin"

    def test_failure_image_rolls_back_and_runs_the_valid_copy(self):
        z = self.make_cpu()
        self.boot_kernel(z)
        tx = bytes(z.sio_tx)

        self.assertIn(b"console: dynamic sio0.b OK\r\n", tx)
        self.assertIn(b"m6 failure selftest: OK\r\n", tx)
        self.assertNotIn(b"FAIL", tx)
        self.assertNotIn(b"PANIC", tx)
        # The valid copy ran exactly once; the truncated entry never did.
        self.assertEqual(tx.count(b"zextest: bank=0 pid=0\r\n"), 1)
        self.assertEqual(z.bank7[self.syms["_m6_selftest_result"]], 0x00)
        self.assert_clean_table(z)

        payload = self.zex_payload("zexok.zex")
        bank0 = z.bank0
        self.assertEqual(
            bytes(bank0[ZEX_LOAD_BASE:ZEX_LOAD_BASE + len(payload) - 20]),
            payload[20:], "the valid copy did not load byte-identically")


class TestUserCopyGates(M6SimBase):
    """Focused replay of kernel/memory/user_copy.asm."""

    OP_READ = 1
    OP_WRITE = 2
    OP_ZERO = 3
    REQ = 0xD900  # bank-7 scratch below the ISR stack

    def setUp(self):
        super().setUp()
        self.gate = self.syms["_zos_bank_copy"]
        self.staging = self.syms["_zos_user_copy_staging"]

    def put_request(self, z, bank, op, addr, buffer, length):
        req = self.REQ
        z.bank7[req + 0] = bank
        z.bank7[req + 1] = op
        z.bank7[req + 2] = addr & 0xFF
        z.bank7[req + 3] = (addr >> 8) & 0xFF
        z.bank7[req + 4] = buffer & 0xFF
        z.bank7[req + 5] = (buffer >> 8) & 0xFF
        z.bank7[req + 6] = length & 0xFF
        z.bank7[req + 7] = (length >> 8) & 0xFF
        return req

    def test_write_and_read_round_trip_with_exact_state_restore(self):
        z = self.make_cpu(latch=0x18 | 3)
        z.iff1 = True
        z.iff2 = True
        data = bytes(range(16))
        z.bank0[self.staging:self.staging + len(data)] = data
        latch_before = z.latch
        sp_before = z.sp

        req = self.put_request(z, 2, self.OP_WRITE, 0x0B00, self.staging,
                               len(data))
        self.call(z, self.gate, hl=req)
        self.assertEqual(z.a, 0)
        self.assertEqual(bytes(z.banks[2][0x0B00:0x0B10]), data)
        self.assertEqual(z.latch, latch_before, "latch not restored")
        self.assertEqual(z.sp, sp_before, "SP not restored")
        self.assertTrue(z.iff1 and z.iff2, "IFF not restored")

        # Read back into a cleared staging buffer.
        for i in range(len(data)):
            z.bank0[self.staging + i] = 0x55
        req = self.put_request(z, 2, self.OP_READ, 0x0B00, self.staging,
                               len(data))
        self.call(z, self.gate, hl=req)
        self.assertEqual(z.a, 0)
        self.assertEqual(bytes(z.bank0[self.staging:self.staging + 16]), data)
        self.assertEqual(z.latch, latch_before)

    def test_zero_fill_and_length_edges(self):
        z = self.make_cpu()
        req = self.put_request(z, 4, self.OP_ZERO, 0x1000, 0, 32)
        self.call(z, self.gate, hl=req)
        self.assertEqual(z.a, 0)
        self.assertEqual(bytes(z.banks[4][0x1000:0x1020]), b"\x00" * 32)
        # One byte.
        req = self.put_request(z, 4, self.OP_ZERO, 0x1024, 0, 1)
        self.call(z, self.gate, hl=req)
        self.assertEqual(z.banks[4][0x1024], 0)
        # Zero length is a no-op and must not touch the mapping.
        latch = z.latch
        req = self.put_request(z, 4, self.OP_WRITE, 0x1000, self.staging, 0)
        self.call(z, self.gate, hl=req)
        self.assertEqual(z.a, 0)
        self.assertEqual(z.latch, latch)

    def test_bank_seven_and_bad_ops_are_rejected(self):
        z = self.make_cpu()
        latch = z.latch
        sp = z.sp
        for bank, op in ((7, self.OP_READ), (7, self.OP_WRITE),
                         (2, 0), (2, 4)):
            req = self.put_request(z, bank, op, 0x1000, self.staging, 8)
            self.call(z, self.gate, hl=req)
            self.assertEqual(z.a, ZOS_EINVAL,
                             "bank %d op %d was not rejected" % (bank, op))
        self.assertEqual(z.latch, latch)
        self.assertEqual(z.sp, sp)

    def test_iff_disabled_stays_disabled(self):
        for enabled in (0, 1):
            z = self.make_cpu()
            z.iff1 = bool(enabled)
            z.iff2 = bool(enabled)
            req = self.put_request(z, 5, self.OP_ZERO, 0x0200, 0, 4)
            self.call(z, self.gate, hl=req)
            self.assertEqual(z.iff1, bool(enabled),
                             "IFF1 not restored for enabled=%d" % enabled)
            self.assertEqual(z.iff2, bool(enabled),
                             "IFF2 not restored for enabled=%d" % enabled)


if __name__ == "__main__":
    unittest.main()
