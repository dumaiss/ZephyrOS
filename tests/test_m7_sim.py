#!/usr/bin/env python3
"""Host-side M7 functional tests: replay the M7 acceptance image in the
Z80 simulator.

Coverage (all executing the real linked bytes):

* production-style boot of the M7 image (SIO from ROMFS, console bound),
  the kernel-side diagnostic selftest (`m7 selftest: OK`), and the
  temporary launcher exec'ing `monitor.zex` from ROMFS;
* a scripted interactive monitor session through the emulated SIO link:
  unknown command, help, system/bank info, 16-byte-row dumps of the
  monitor bank and selected banks, scratch-byte modify/readback, range
  and trailing-argument rejections, latch-port rejections, port input,
  line overflow with bell + recovery, backspace editing, CRLF handling,
  Q exit, repeated launch/exit cycles, and stable launcher counts;
* focused kernel tests of SYS_DIAG_* through the real bank-7 dispatcher:
  accepted reads/writes/port I/O, every rejection class with exact
  ZOS_EINVAL, side-effect freedom (canary, process table), and exact
  latch/SP/IFF restoration;
* byte identity of the ROMFS `monitor.zex` payload with the packed
  artifact.

Skipped when the M7 image / packed modules have not been built.
"""

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import Z80, read_symbols  # noqa: E402
import romfspack  # noqa: E402
import serial_smoke  # noqa: E402

RET = 0x1234
STEP_LIMIT = 40000000
STACK = 0xFFFE

ZOS_EINVAL = 0x02
PROCESS_FREE = 0x00
PROCESS_NO_PROCESS = 0xFF

DIAG = {
    "mem_read": 0x28,
    "mem_write": 0x29,
    "io_in": 0x2A,
    "io_out": 0x2B,
    "bank_info": 0x2C,
    "system_info": 0x2D,
}

# Constants shared with the physical smoke tool: the simulator proves
# these exact expectations against the real linked bytes.
PROMPT = serial_smoke.M7_PROMPT
BANNER = serial_smoke.M7_BANNER
M7_OK = serial_smoke.M7_SELFTEST_OK
MONITOR_EXIT = serial_smoke.M7_EXIT
SYSINFO_LINE = serial_smoke.M7_SYSINFO
BANK_LINE = serial_smoke.M7_BANK_INFO


class M7SimBase(unittest.TestCase):
    IMAGE_SUBDIR = "build-m7-selftest"
    IMAGE_NAME = "zephyros-m7-selftest.bin"

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
            header, entries = romfspack.parse_image(fh.read())
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
        if z.pc == self.syms["_kernel_serial_connect_delay"]:
            z.pc = z.pop16()

    def boot_to_prompt(self, z, limit=STEP_LIMIT):
        """Replay crt0 + kernel_main until the monitor prompt is up."""
        z.push16(RET)
        z.pc = self.syms["__zos_crt0_init"]
        for _ in range(limit):
            z.step()
            if z.pc == RET:
                break
        else:
            raise AssertionError("__zos_crt0_init did not return")
        z.push16(RET)
        z.pc = self.syms["_kernel_main"]
        for _ in range(limit):
            z.step()
            self.skip_delay(z)
            if PROMPT in bytes(z.sio_tx):
                return bytes(z.sio_tx)
        raise AssertionError("monitor prompt never appeared (PC=%04X)" % z.pc)

    def run_until(self, z, predicate, limit=STEP_LIMIT):
        for _ in range(limit):
            z.step()
            self.skip_delay(z)
            tx = bytes(z.sio_tx)
            if predicate(tx):
                return tx
        raise AssertionError("expected output never appeared (PC=%04X)"
                             % z.pc)

    def command(self, z, line, expect, limit=STEP_LIMIT):
        """Send one command and step until `expect` (bytes or compiled
        regex) appears in the newly produced output."""
        start = len(z.sio_tx)
        z.sio_rx.extend(line.encode("ascii") + b"\r")
        if isinstance(expect, re.Pattern):
            check = lambda tx: expect.search(tx[start:])
        else:
            check = lambda tx: expect in tx[start:]
        return self.run_until(z, check, limit)

    def send_raw(self, z, data, expect, limit=STEP_LIMIT):
        start = len(z.sio_tx)
        z.sio_rx.extend(data)
        return self.run_until(z, lambda tx: expect in tx[start:], limit)

    def process_state(self, z):
        base = self.syms["_process_table"]
        states = [z.bank7[base + i * 8] for i in range(7)]
        running = z.bank7[self.syms["_process_running_pid"]]
        return states, running


class TestM7BootAndMonitorSession(M7SimBase):
    def test_full_scripted_session(self):
        z = self.make_cpu()
        tx = self.boot_to_prompt(z)

        # Boot: dynamic console, kernel-side diagnostic selftest, banner.
        self.assertIn(b"console: dynamic sio0.b OK\r\n", tx)
        self.assertIn(M7_OK, tx)
        self.assertNotIn(b"FAIL", tx)
        self.assertNotIn(b"PANIC", tx)
        self.assertIn(BANNER, tx)
        self.assertEqual(z.bank7[self.syms["_m7_selftest_result"]], 0x00)
        match = re.search(rb"scratch=([0-9A-F]{4})", tx)
        self.assertIsNotNone(match, "banner does not print the scratch "
                                    "address")
        scratch = int(match.group(1), 16)

        # Unknown command.
        tx = self.command(z, "S", b"ERR: COMMAND\r\n")
        self.assertIn(b"ERR: COMMAND\r\n", tx)

        # Help.
        tx = self.command(z, "H", b"Q              quit\r\n")
        self.assertIn(b"R              system/bank information\r\n", tx)
        self.assertIn(b"DB bank addr len  dump bank 0-6\r\n", tx)
        self.assertIn(b"I port         input port (10-FF)\r\n", tx)

        # Help rejects trailing arguments too.
        self.command(z, "H EXTRA", b"ERR: ARG\r\n")

        # System/bank/process information through the diagnostic record.
        # The bank line is the last of the three: waiting for it proves
        # the complete record was printed.
        tx = self.command(z, "R", BANK_LINE)
        self.assertIn(SYSINFO_LINE, tx)
        self.assertIn(b"process: pid 0 bank 0 flags 0001\r\n", tx)

        # Current-bank dump: 16-byte rows whose bytes match the loaded
        # ZEX image exactly (the monitor's own bank 0).
        packed = self.entries["monitor.zex"]["payload"]
        row_bytes = packed[20:36]
        expected = b"0100: " + (" ".join("%02X" % b for b in row_bytes)
                               + " ").encode("ascii")
        tx = self.command(z, "D 0100 20", b"0110: ")
        self.assertIn(expected, tx)

        # Lowercase commands and hex digits are accepted.
        tx = self.command(z, "d 100 10", expected)

        # Bank dump: page zero of bank 0 opens with the RST 30h JP.
        tx = self.command(z, "DB 0 0030 1", b"0030: C3 ")
        self.assertIn(b"0030: C3 ", tx)
        # Bank 1 is a valid application bank for reads.
        tx = self.command(z, "DB 1 0100 1", b"0100: ")
        # Bank 7 is masked out.
        tx = self.command(z, "DB 7 0100 1", b"ERR: ARG\r\n")
        tx = self.command(z, "MB 7 0100 01", b"ERR: ARG\r\n")

        # Scratch-byte modify/readback in the monitor's own bank.  The
        # readback expectation includes the separator so the snapshot is
        # taken after the complete hex byte was emitted.
        tx = self.command(z, "M %04X A5" % scratch, b"OK\r\n")
        self.assertIn(b"OK\r\n", tx)
        readback = ("%04X: A5 " % scratch).encode("ascii")
        tx = self.command(z, "D %04X 1" % scratch, readback)
        self.assertIn(readback, tx)

        # Modify protects native page zero; M needs a value.
        self.command(z, "M 00FF 00", b"ERR: ARG\r\n")
        self.command(z, "M 0100", b"ERR: ARG\r\n")
        # Trailing / malformed arguments.
        self.command(z, "D 0100 10 GARBAGE", b"ERR: ARG\r\n")
        self.command(z, "D 12345 10", b"ERR: ARG\r\n")
        self.command(z, "DB 1 0100", b"ERR: ARG\r\n")

        # Harmless port input (SIO0/A data is not the console).
        tx = self.command(z, "I 20", re.compile(rb"IN=[0-9A-F]{2}\r\n"))
        self.assertRegex(tx[-16:], rb"IN=[0-9A-F]{2}\r\n")

        # The latch-aliased port range is rejected without executing.
        self.command(z, "O 00 18", b"ERR: ARG\r\n")
        self.command(z, "O 0F 00", b"ERR: ARG\r\n")
        self.command(z, "I 0F", b"ERR: ARG\r\n")

        # Line overflow rings the bell, keeps the first 80 characters,
        # and the monitor recovers on the next line.
        tx = self.send_raw(z, b"D 0100 10" + b"X" * 100 + b"\r",
                           b"ERR: ARG\r\n")
        self.assertIn(b"\x07", tx)
        tx = self.command(z, "D 0100 10", expected)

        # Backspace editing: the invalid final digit is corrected.
        tx = self.send_raw(z, b"D 0100 1X\x080\r", expected)

        # CRLF input: the LF after CR is swallowed (no empty command).
        tx = self.command(z, "R\r\n", BANK_LINE)
        self.assertNotIn(b"ERR", tx[-200:])

        # Q with trailing garbage is rejected; plain Q exits and the
        # launcher reports the recovered counts.
        self.command(z, "Q X", b"ERR: ARG\r\n")
        self.command(z, "q", MONITOR_EXIT)
        states, running = self.process_state(z)
        self.assertEqual(states, [PROCESS_FREE] * 7)
        self.assertEqual(running, PROCESS_NO_PROCESS)

        # Second cycle: monitor is re-exec'd through the same exec path.
        self.run_until(z, lambda tx: tx.count(PROMPT) >= 2)
        tx = self.command(z, "R", BANK_LINE)
        self.command(z, "Q", MONITOR_EXIT)
        states, running = self.process_state(z)
        self.assertEqual(states, [PROCESS_FREE] * 7)
        self.assertEqual(running, PROCESS_NO_PROCESS)

        # Exactly two identical launcher lines: cycles are stable.
        full = bytes(z.sio_tx)
        self.assertEqual(full.count(MONITOR_EXIT), 2)

    def test_romfs_monitor_payload_matches_packed_artifact(self):
        packed_path = os.path.join(ROOT, "build", "modules", "monitor.zex")
        if not os.path.exists(packed_path):
            self.skipTest("build/modules/monitor.zex missing")
        with open(packed_path, "rb") as fh:
            packed = fh.read()
        self.assertEqual(self.entries["monitor.zex"]["payload"], packed)


class TestM7DiagnosticBoundary(M7SimBase):
    """Focused kernel tests of the SYS_DIAG_* dispatcher paths."""

    REQ = 0x1F00  # caller-window scratch (bank 0 low RAM in mode 11)

    def setUp(self):
        super().setUp()
        self.dispatch = self.syms["_kernel_syscall_dispatch"]

    def put_req(self, z, data):
        # The caller window is the low RAM of the currently selected
        # bank while mode 11 is active.
        window = z._sram(z.latch & 0x07)
        for i, byte in enumerate(data):
            window[self.REQ + i] = byte

    def call_diag(self, z, number, block):
        z.bank7[self.syms["_zos_syscall_number"]] = number
        z.bank7[self.syms["_zos_syscall_args"]] = block & 0xFF
        z.bank7[self.syms["_zos_syscall_args"] + 1] = (block >> 8) & 0xFF
        self.call(z, self.dispatch)
        carry = z.bank7[self.syms["_zos_syscall_result_c"]] != 0
        error = z.bank7[self.syms["_zos_syscall_result_a"]]
        hl = (z.bank7[self.syms["_zos_syscall_result_hl"]] |
              (z.bank7[self.syms["_zos_syscall_result_hl"] + 1] << 8))
        return (error if carry else None), hl

    def call(self, z, addr, limit=2000000):
        z.push16(RET)
        z.pc = addr
        for _ in range(limit):
            z.step()
            if z.pc == RET:
                return
        raise AssertionError("dispatch at %04X did not return" % addr)

    def test_mem_read_write_round_trip_and_exact_state(self):
        z = self.make_cpu(latch=0x18 | 3)
        z.iff1 = z.iff2 = True
        latch_before = z.latch
        sp_before = z.sp

        self.put_req(z, [2, 0x5A, 0x00, 0x0B])
        error, hl = self.call_diag(z, DIAG["mem_write"], self.REQ)
        self.assertIsNone(error)
        self.assertEqual(hl, 0)
        self.assertEqual(z.banks[2][0x0B00], 0x5A)

        self.put_req(z, [2, 0, 0x00, 0x0B])
        error, hl = self.call_diag(z, DIAG["mem_read"], self.REQ)
        self.assertIsNone(error)
        self.assertEqual(hl, 0x5A)

        self.assertEqual(z.latch, latch_before, "latch not restored")
        self.assertEqual(z.sp, sp_before, "SP not restored")
        self.assertTrue(z.iff1 and z.iff2)

    def test_every_rejection_is_einval_and_side_effect_free(self):
        z = self.make_cpu(latch=0x18)
        latch_before = z.latch
        sp_before = z.sp
        self.bank7_snapshot = bytes(
            z.bank7[self.syms["_zos_user_copy_staging"]:
                    self.syms["_zos_user_copy_staging"] + 8])
        z.banks[1][0x0B01] = 0x11
        z.sio_tx.clear()

        cases = (
            # (number, block bytes, block address)
            (DIAG["mem_read"], [7, 0, 0x00, 0x0B], self.REQ),
            (DIAG["mem_read"], [0xFF, 0, 0x00, 0x0B], self.REQ),
            (DIAG["mem_read"], [1, 0, 0x00, 0xE0], self.REQ),
            (DIAG["mem_read"], [1, 0, 0xFF, 0xFF], self.REQ),
            (DIAG["mem_read"], [1, 1, 0x00, 0x0B], self.REQ),
            (DIAG["mem_write"], [7, 0xAA, 0x01, 0x0B], self.REQ),
            (DIAG["mem_write"], [1, 0xAA, 0xFF, 0x00], self.REQ),
            (DIAG["mem_write"], [1, 0xAA, 0x00, 0xE0], self.REQ),
            (DIAG["io_in"], [0x00], self.REQ),
            (DIAG["io_in"], [0x0F], self.REQ),
            (DIAG["io_out"], [0x00, 0x18], self.REQ),
            (DIAG["io_out"], [0x0F, 0x18], self.REQ),
            # Argument block outside the caller window.
            (DIAG["mem_read"], [1, 0, 0x00, 0x0B], 0x2000),
            (DIAG["system_info"], [0], 0x2000),
        )
        for number, block_bytes, block in cases:
            self.put_req(z, block_bytes)
            error, hl = self.call_diag(z, number, block)
            self.assertEqual(error, ZOS_EINVAL,
                             "number %02X block %r was not rejected"
                             % (number, block_bytes))
            self.assertEqual(hl, 0)
            self.assertEqual(z.latch, latch_before,
                             "latch changed by a rejected request")
        # The rejected bank-7 write did not reach bank 1's canary, and
        # the rejected latch-port outputs produced no SIO traffic.
        self.assertEqual(z.banks[1][0x0B01], 0x11)
        self.assertEqual(bytes(z.sio_tx), b"")
        self.assertEqual(z.sp, sp_before)

    def test_bank_info_and_system_info(self):
        z = self.make_cpu(latch=0x18)
        error, hl = self.call_diag(z, DIAG["bank_info"], 0)
        self.assertIsNone(error)
        self.assertEqual(hl >> 8, 0x7F)
        self.assertLessEqual(hl & 0xFF, 6)

        self.put_req(z, [0] * 8)
        error, hl = self.call_diag(z, DIAG["system_info"], self.REQ)
        self.assertIsNone(error)
        self.assertEqual(hl, 8)
        record = bytes(z.bank0[self.REQ:self.REQ + 8])
        self.assertEqual(record, bytes((8, 1, 0x10, 0x18, 0x00, 0xE0, 7, 2)))

    def test_io_in_reads_the_queued_byte_and_out_reaches_the_simulated_port(self):
        z = self.make_cpu(latch=0x18 | 5)
        latch_before = z.latch
        z.sio_rx.extend(b"\x3C")
        self.put_req(z, [0x22])
        error, hl = self.call_diag(z, DIAG["io_in"], self.REQ)
        self.assertIsNone(error)
        self.assertEqual(hl, 0x3C)

        self.put_req(z, [0x22, 0x5A])
        error, hl = self.call_diag(z, DIAG["io_out"], self.REQ)
        self.assertIsNone(error)
        self.assertIn(0x5A, bytes(z.sio_tx))
        self.assertEqual(z.latch, latch_before)
        self.assertEqual(z.latch & 0x07, 5, "selected bank changed")


if __name__ == "__main__":
    unittest.main()
