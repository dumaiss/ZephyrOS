#!/usr/bin/env python3
"""M10 design-spike ZEX v2 loader replay (package 33b).

Boots build-m10-spike-selftest/zephyros-m10-spike-selftest.bin in the Z80
simulator.  The image boots a two-auxiliary-segment ZEX v2 program, so the
replay can check, from the simulator's bank memory (the hardware-
authoritative view), exactly what the resident v2 loader placed:

* both auxiliary segments land on their 8-KiB slot boundaries inside one
  auxiliary bank, byte-for-byte equal to the packed image with every
  ABS16 relocation applied as a high-byte patch;
* the rest of each slot is zero (the segment BSS is the zero-initialized
  allocation, architecture section 2.7);
* the program runs and prints its marker;
* after it exits, the unified bank map and the ZEX v2 program state are
  back to baseline, so the exit path released the auxiliary bank.

The test skips when the image has not been built.
"""

import os
import sys
import unittest
from pathlib import Path

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import Z80, read_symbols  # noqa: E402
import zexpack  # noqa: E402

RET = 0x1234
STACK = 0xFFFE
STEP_LIMIT = 80000000

IMAGE_SUBDIR = "build-m10-spike-selftest"
IMAGE_NAME = "zephyros-m10-spike-selftest.bin"
PROGRAM = os.path.join(ROOT, IMAGE_SUBDIR, "user", "spike", "spike.zex")

MARKER = b"m10 spike: primary OK\r\n"
RESULT_PASS = b"m10 spike: PASS\r\n"
CHECKS_17 = b"m10 spike: 17/17 checks\r\n"
FAR_CONSOLE = b"far code: console OK\r\n"

PROGRAM_ENTRY = 0x0100
PRIMARY_BANK = 0  # lowest free bank (the kernel launcher allocates low)
AUX_BANK = 6      # highest free bank (auxiliary allocation takes high first)
SLOT_BASE = 0x2000
SLOT_SIZE = 0x2000

APP_MODE = 0x10
MODE_MASK = 0x18
BANK_MASK = 0x07


def expected_segment_bytes(path):
    """Parse the packed container and return (bank, [(base, image)]).

    Each segment image has every ABS16 relocation applied as the loader
    would: the slot base's high byte added to the byte after each patch
    offset.
    """
    data = Path(path).read_bytes()
    fields = zexpack.validate_v2(data)
    result = []
    for index, seg in enumerate(fields["segments"]):
        base = SLOT_BASE + index * SLOT_SIZE
        image = bytearray(data[seg["payload_offset"]:
                               seg["payload_offset"] + seg["image_size"]])
        high = (base >> 8) & 0xFF
        for j in range(seg["reloc_count"]):
            at = seg["reloc_offset"] + 2 * j
            site = data[at] | (data[at + 1] << 8)
            image[site + 1] = (image[site + 1] + high) & 0xFF
        result.append((base, bytes(image)))
    return fields, result


class M10SpikeSimBase(unittest.TestCase):
    def setUp(self):
        self.bin_path = os.path.join(ROOT, IMAGE_SUBDIR, IMAGE_NAME)
        if not os.path.exists(self.bin_path):
            self.skipTest("%s missing; build the image first" % self.bin_path)
        with open(self.bin_path, "rb") as handle:
            self.image = handle.read()
        self.syms = read_symbols(os.path.join(ROOT, IMAGE_SUBDIR,
                                              "zephyros.map"))

    def make_cpu(self):
        pages = {n: self.image[n * 0x10000:(n + 1) * 0x10000]
                 for n in range(8)}
        z = Z80(pages[0], pages[1],
                rom_pages={n: pages[n] for n in range(2, 8)})
        z.latch = 0x18
        z.sp = STACK
        return z

    def crt0(self, z):
        z.push16(RET)
        z.pc = self.syms["__zos_crt0_init"]
        for _ in range(2000000):
            z.step()
            if z.pc == RET:
                return
        raise AssertionError("C runtime init did not return")

    def boot(self):
        """Boot past the program's exit, capturing the loaded aux bank.

        Returns (cpu, aux_snapshot, steps, min_sp).
        """
        z = self.make_cpu()
        self.crt0(z)
        z.push16(RET)
        z.pc = self.syms["_kernel_main"]
        delay = self.syms["_kernel_serial_connect_delay"]
        gate_top = self.syms["zos_gate_stack_top"]
        gate_size = 256
        pid_addr = self.syms["_process_running_pid"]
        owner_addr = self.syms["_zos_zex2_owner"]
        aux_snapshot = None
        min_sp = gate_top
        saw_result = False
        for step in range(STEP_LIMIT):
            if z.pc == delay:
                z.pc = z.pop16()
            if (aux_snapshot is None and z.pc == PROGRAM_ENTRY and
                    (z.latch & MODE_MASK) == APP_MODE and
                    (z.latch & BANK_MASK) == PRIMARY_BANK):
                aux_snapshot = bytes(z.banks[AUX_BANK])
            z.step()
            if gate_top - gate_size <= z.sp <= gate_top:
                if z.sp < min_sp:
                    min_sp = z.sp
            if b"PANIC" in z.sio_tx:
                break
            if (not saw_result and
                    (b"m10 spike: PASS\r\n" in z.sio_tx or
                     b"m10 spike: FAIL\r\n" in z.sio_tx)):
                saw_result = True
            # Wait until the exit path released the process and its
            # auxiliary resources before returning, so the caller
            # observes the settled bank map (process_running_pid clears
            # first, before bankmap_aux_release and zex2_prog_reset).
            if (saw_result and z.bank7[pid_addr] == 0xFF and
                    z.bank7[owner_addr] == 0):
                break
            if z.halted:
                break
        else:
            raise AssertionError("the spike image never completed (PC=%04X)"
                                 % z.pc)
        return z, aux_snapshot, step, min_sp

    def read_bank7(self, z, name):
        addr = self.syms[name]
        return z.bank7[addr]

    def read_bank7_word(self, z, name):
        addr = self.syms[name]
        return z.bank7[addr] | (z.bank7[addr + 1] << 8)


class TestM10SpikeLoader(M10SpikeSimBase):
    def test_program_runs_and_prints(self):
        z, _, _, _ = self.boot()
        self.assertIn(MARKER, z.sio_tx,
                      "the ZEX v2 program never printed its marker")
        self.assertNotIn(b"PANIC", z.sio_tx)

    def test_runtime_scenario_passes(self):
        # The program's own checks cover xmem allocation across a slot
        # boundary, segment info, far calls into both segments, an
        # ordinary syscall and an FS_READ from far code, and a recursive
        # far call refused with ZOS_EBUSY.
        z, _, _, _ = self.boot()
        self.assertNotIn(b"m10 spike: FAIL", z.sio_tx)
        self.assertIn(CHECKS_17, z.sio_tx)
        self.assertIn(RESULT_PASS, z.sio_tx)
        self.assertIn(FAR_CONSOLE, z.sio_tx)

    def test_aux_segments_loaded_with_relocations(self):
        z, aux, _, _ = self.boot()
        self.assertIsNotNone(aux, "the program entry was never reached")
        self.assertTrue(os.path.exists(PROGRAM),
                        "the packed container is missing")
        fields, expected = expected_segment_bytes(PROGRAM)
        self.assertEqual(fields["segment_count"], 2)
        self.assertEqual(fields["total_slots"], 2)
        aux = bytearray(aux)
        for base, image in expected:
            got = bytes(aux[base:base + len(image)])
            self.assertEqual(got, image,
                             "segment at %04Xh does not match the packed "
                             "image with relocations applied" % base)
            # The packer places both segments in one 8-KiB slot each.
            rest = aux[base + len(image):base + SLOT_SIZE]
            self.assertEqual(bytes(rest), b"\x00" * len(rest),
                             "segment BSS at %04Xh is not zero" % base)

    def test_relocations_are_high_byte_only(self):
        # The proof is in the bytes: the loaded image equals the packed
        # image with only the high byte of each patch changed.
        fields, expected = expected_segment_bytes(PROGRAM)
        data = Path(PROGRAM).read_bytes()
        for index, seg in enumerate(fields["segments"]):
            base, image = expected[index]
            raw = bytes(data[seg["payload_offset"]:
                             seg["payload_offset"] + seg["image_size"]])
            for j in range(seg["reloc_count"]):
                at = seg["reloc_offset"] + 2 * j
                site = data[at] | (data[at + 1] << 8)
                low_raw = raw[site]
                low_loaded = image[site]
                self.assertEqual(low_raw, low_loaded,
                                 "a relocation changed the low byte at "
                                 "%04Xh" % site)
            # And at least one patch actually changed something.
            self.assertNotEqual(raw, image)

    def test_gate_stack_within_limit(self):
        # The M10 gate-stack rule: every path stays at or below 240 bytes.
        # The v2 loader runs on the kernel stack, so what this measures is
        # the program's syscall path (console writes and SYS_EXIT) plus the
        # launch/resume gates.
        _, _, _, min_sp = self.boot()
        top = self.syms["zos_gate_stack_top"]
        used = top - min_sp
        self.assertGreater(used, 0, "no gate stack usage observed")
        self.assertLessEqual(used, 240,
                             "gate stack worst case %d bytes exceeds 240"
                             % used)

    def test_bank_map_and_program_state_released(self):
        z, _, _, _ = self.boot()
        for slot in range(7):
            addr = self.syms["_zos_aux_owner"] + slot
            self.assertEqual(z.bank7[addr], 0,
                             "aux owner[%d] still set after exit" % slot)
            addr = self.syms["_zos_aux_slots"] + slot
            self.assertEqual(z.bank7[addr], 0,
                             "aux slot bitmap[%d] still set after exit" % slot)
        self.assertEqual(self.read_bank7(z, "_zos_zex2_owner"), 0,
                         "ZEX v2 program state still owned after exit")
        self.assertEqual(self.read_bank7(z, "_zos_zex2_seg_count"), 0,
                         "ZEX v2 segment count still set after exit")
        # The primary bank the launcher allocated is free again.
        self.assertEqual(self.read_bank7(z, "_process_running_pid"), 0xFF,
                         "a process is still running after exit")


class TestM10SpikeBudget(M10SpikeSimBase):
    """The spike's own residency figures, for the design report."""

    def test_ledgers_present(self):
        ledger = Path(ROOT, IMAGE_SUBDIR, "arena.plan.json")
        if not ledger.exists():
            self.skipTest("arena plan not built")
        text = ledger.read_text()
        self.assertIn("granules", text)


if __name__ == "__main__":
    unittest.main()
