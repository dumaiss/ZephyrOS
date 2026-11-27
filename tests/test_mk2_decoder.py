#!/usr/bin/env python3
"""The Zephyr-80 Mk2 memory configuration of the simulator.

The expected values come from the LS612 DAT architecture (2.7.2-2.7.5):
128 pages of 8 KiB, 00h-3Fh ROM and 40h-7Fh SRAM, sixteen entries (system
0-7, working 8-15), `PA = page << 13 | (LA & 1FFFh)`, entry writes at low
port 00h-0Fh with I/O A15 = 0, control at 800Fh (D0 DAT_BANK, D1
MAP_ENABLE), pass mode at reset, ROM writes ignored; the IO controller's
CMD_RESET request drives that reset.  `OUT (n),A` drives A
on A8-A15, so the guest programs below write entries and control with real
Z80 instructions, and the model under test decodes the 16-bit port.

Every test runs on the simulator's C core.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import z80sim  # noqa: E402
from z80sim import Z80  # noqa: E402

ROM_SIZE = 0x80000
SRAM_BASE = 0x80000
RESERVED_LOW = 0x400     # guest code lives below this in ROM page 0


def rom_byte(pa):
    return (pa * 31 + (pa >> 13) * 17 + 5) & 0xFF


ROM_IMAGE = bytes(rom_byte(pa) for pa in range(ROM_SIZE))


# ---- a tiny assembler ------------------------------------------------------

def ld_a(n):
    return [0x3E, n]


def ld_b(n):
    return [0x06, n]


def ld_bc(nn):
    return [0x01, nn & 0xFF, nn >> 8]


def ld_hl(nn):
    return [0x21, nn & 0xFF, nn >> 8]


def out_n(n):                   # OUT (n),A
    return [0xD3, n]


def in_n(n):                    # IN A,(n)
    return [0xDB, n]


OUT_C_A = [0xED, 0x79]
OUTI = [0xED, 0xA3]
OTIR = [0xED, 0xB3]
HALT = [0x76]


def st_a(nn):                   # LD (nn),A
    return [0x32, nn & 0xFF, nn >> 8]


def ld_a_mem(nn):               # LD A,(nn)
    return [0x3A, nn & 0xFF, nn >> 8]


def entry_write(index, page):   # OUT (index),A drives A = page on A8-A15
    return ld_a(page) + out_n(index)


def control(value):             # BC=800Fh, OUT (C),A
    return ld_bc(0x800F) + ld_a(value) + OUT_C_A


def expected_pa(entries, bank, enable, la):
    """The architecture's translation, independent of the model."""
    if not enable:
        return la
    return (entries[bank * 8 + (la >> 13)] << 13) | (la & 0x1FFF)


def device_of(pa):
    return "sram" if pa >= SRAM_BASE else "rom"


def make(code, cls=None, seed=1, strict=True):
    """A Mk2 board running `code` from ROM 0000h.  `strict=False` for
    a program that deliberately writes an entry with another B."""
    rom = bytearray(ROM_IMAGE)
    rom[0:len(code)] = bytes(code)
    return (cls or Z80)(bytes(rom), b"", board="mk2", mapper_seed=seed,
                        mapper_strict=strict)


def go(z, stop=None):
    """Run until HALT, or until pc reaches `stop` (before executing it)."""
    if stop is None:
        return z.run(100000, halt=True)
    return z.run(100000, stops=z80sim.StopSet([stop]), check_first=False)


class TestReset(unittest.TestCase):
    def test_reset_selects_system_bank_and_pass_mode(self):
        z = make(HALT)
        self.assertEqual(z.board, "mk2")
        self.assertEqual(z.mapper.dat_bank, 0)
        self.assertEqual(z.mapper.map_enable, 0)
        self.assertEqual(len(z.mapper.entries), 16)
        self.assertTrue(all(0 <= e < 128 for e in z.mapper.entries))

    def test_entries_differ_on_every_reset(self):
        z = make(HALT)
        seen = [tuple(z.mapper.entries)]
        for _ in range(6):
            z.mapper.dat_bank = 1
            z.mapper.map_enable = 1
            z.mapper.reset()
            self.assertEqual((z.mapper.dat_bank, z.mapper.map_enable),
                             (0, 0))
            seen.append(tuple(z.mapper.entries))
        self.assertEqual(len(set(seen)), len(seen))
        for pattern in seen:
            self.assertGreaterEqual(len(set(pattern)), 8)
        other = make(HALT, seed=2)
        self.assertNotEqual(tuple(other.mapper.entries), seen[0])

    def test_pass_mode_shows_the_first_64k_of_rom(self):
        for seed in (1, 2, 3):
            z = make(HALT, seed=seed)
            for la in range(RESERVED_LOW, 0x10000, 0x111):
                self.assertEqual(z.mapper.translate(la), ("rom", la))
                self.assertEqual(z.rd(la), ROM_IMAGE[la],
                                 "seed %d la %04X" % (seed, la))
            self.assertEqual(z.rd(0xFFFF), ROM_IMAGE[0xFFFF])

    def test_boot_never_depends_on_the_entries(self):
        # A pass-mode program that reads logical 2000h and E000h.
        code = ld_a_mem(0x2000) + [0x47] + ld_a_mem(0xE000) + [0x4F] + HALT
        results = set()
        for seed in range(1, 6):
            z = make(code, seed=seed)
            go(z)
            results.add((z.b, z.a, z.c))
            self.assertEqual(z.b, ROM_IMAGE[0x2000])
            self.assertEqual(z.a, ROM_IMAGE[0xE000])
        self.assertEqual(len(results), 1)

    def test_entry_writes_do_not_leave_pass_mode(self):
        code = []
        for i in range(16):
            code += entry_write(i, 0x40 + i)
        code += HALT
        z = make(code)
        go(z)
        self.assertEqual(list(z.mapper.entries),
                         [0x40 + i for i in range(16)])
        self.assertEqual(z.mapper.map_enable, 0)
        for la in range(RESERVED_LOW, 0x10000, 0x777):
            self.assertEqual(z.rd(la), ROM_IMAGE[la])

    def test_ioc_reset_request_resets_the_board(self):
        code = []
        for i in range(16):
            code += entry_write(i, 0x40 + i)
        code += control(0x03) + [0xED, 0x5E, 0xFB] + HALT   # IM 2, EI
        z = make(code)
        go(z)
        z.rt_hz = 8000
        self.assertEqual((z.mapper.dat_bank, z.mapper.map_enable), (1, 1))
        peer = z80sim.IocCommandPeer()
        peer.cpu = z
        body = bytes([0x03, 0x00, peer.CMD_RESET, 0x01, 0x00])
        crc = z80sim.ioc_crc16(body)
        frame = b"\xa5\x5a" + body + bytes([crc >> 8, crc & 0xFF])
        peer.data_write(frame[0])               # preload, then RTS
        peer.ctrl_write(0x05)
        peer.ctrl_write(0x6A)
        for value in frame[1:] + b"\xff\xff":
            peer.data_write(value)
        peer.ctrl_write(0x05)
        peer.ctrl_write(0x00)                   # RTS released
        self.assertEqual(peer.host_resets, 1)
        self.assertIsNone(peer.reply_pending)
        self.assertEqual(peer.replies, [])
        self.assertEqual((z.pc, z.im, z.iff1, z.iff2, z.halted),
                         (0, 0, False, False, False))
        self.assertEqual(z.rt_hz, 0)
        self.assertEqual((z.mapper.dat_bank, z.mapper.map_enable), (0, 0))
        self.assertEqual(z.rd(0x0000), code[0])


class TestTranslation(unittest.TestCase):
    OFFSETS = (0x0000, 0x0001, 0x0123, 0x0FFF, 0x1000, 0x1FFE, 0x1FFF)

    def program(self, entries, enable_control):
        code = []
        for i, page in enumerate(entries):
            code += entry_write(i, page)
        code += control(enable_control)
        return make(code + HALT), len(code)

    def pages(self):
        """Distinct pages for the 16 entries, ROM and SRAM mixed."""
        out = []
        for i in range(16):
            out.append(0x40 + 3 * i + 1 if i % 3 else 0x10 + 5 * i)
        return out

    def check_map(self, z, entries, bank):
        for slot in range(8):
            for off in self.OFFSETS:
                la = (slot << 13) | off
                pa = expected_pa(entries, bank, 1, la)
                self.assertEqual(
                    z.mapper.translate(la), (device_of(pa), pa),
                    "bank %d slot %d off %04X" % (bank, slot, off))
                if pa >= SRAM_BASE:
                    value = (la * 7 + bank + 1) & 0xFF
                    before = bytes(z.phys[0:SRAM_BASE])
                    z.wr(la, value)
                    self.assertEqual(z.phys[pa], value)
                    self.assertEqual(z.rd(la), value)
                    self.assertEqual(bytes(z.phys[0:SRAM_BASE]), before)
                else:
                    before = z.phys[pa]
                    z.wr(la, before ^ 0xFF)          # ROM write ignored
                    self.assertEqual(z.phys[pa], before)
                    self.assertEqual(z.rd(la), before)

    def test_all_sixteen_entries_map_their_region(self):
        entries = self.pages()
        for bank in (0, 1):
            z, end = self.program(entries, 0x02 | bank)
            go(z, stop=end)
            self.assertEqual((z.mapper.dat_bank, z.mapper.map_enable),
                             (bank, 1))
            self.assertEqual(list(z.mapper.entries), entries)
            self.check_map(z, entries, bank)

    def test_every_page_number_translates(self):
        z = make(HALT)
        z.mapper.map_enable = 1
        for page in range(128):
            for slot in (0, 3, 7):
                z.mapper.entries[slot] = page
                la = (slot << 13) | 0x0ABC
                self.assertEqual(
                    z.mapper.translate(la),
                    (device_of(page << 13), (page << 13) | 0x0ABC))

    def test_dat_bank_switch_changes_the_map_and_back(self):
        # Slots 0 and 8 stay ROM page 0 so the guest keeps running.
        a = [0, 0x41, 0x52, 0x43, 0x54, 0x45, 0x56, 0x47]
        b = [0, 0x60, 0x6F, 0x7A, 0x1B, 0x7C, 0x2D, 0x7E]
        entries = a + b
        code = []
        for i, page in enumerate(entries):
            code += entry_write(i, page)
        ends = []
        for value in (0x02, 0x03, 0x02):
            code += control(value)
            ends.append(len(code))
        z = make(code + HALT)
        go(z, stop=ends[0])
        stages = []
        for bank in (0, 1, 0):
            self.assertEqual((z.mapper.dat_bank, z.mapper.map_enable),
                             (bank, 1))
            self.assertEqual(list(z.mapper.entries), entries)
            stages.append([z.mapper.translate(s << 13) for s in range(8)])
            for slot in range(8):
                pa = expected_pa(entries, bank, 1, slot << 13)
                self.assertEqual(stages[-1][slot], (device_of(pa), pa))
            if len(stages) < 3:
                go(z, stop=ends[len(stages)])
        self.assertEqual(stages[0], stages[2])
        self.assertNotEqual(stages[0], stages[1])
        for slot in range(1, 8):
            self.assertNotEqual(stages[0][slot], stages[1][slot])

    def test_control_can_reach_pass_mode_again(self):
        code = entry_write(0, 0) + entry_write(1, 0x45) + control(0x02)
        end = len(code)
        z = make(code + control(0x00) + HALT)
        go(z, stop=end)
        self.assertEqual(z.mapper.translate(0x2000), ("sram", 0x45 << 13))
        go(z)
        self.assertEqual(z.mapper.map_enable, 0)
        self.assertEqual(z.mapper.translate(0x2000), ("rom", 0x2000))

    def test_common_page_shared_by_entries_7_and_15(self):
        entries = [0, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x7F,
                   0, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x7F]
        code = []
        for i, page in enumerate(entries):
            code += entry_write(i, page)
        code += control(0x02)
        code += ld_a(0x5A) + st_a(0xE010)          # stored through bank 0
        code += control(0x03)
        code += ld_a_mem(0xE010) + [0x57]          # LD D,A: read in bank 1
        code += ld_a(0xA5) + st_a(0xFFFF)
        code += control(0x02)
        code += ld_a_mem(0xFFFF) + [0x5F] + HALT
        z = make(code)
        go(z)
        self.assertEqual(z.d, 0x5A)
        self.assertEqual(z.e, 0xA5)
        self.assertEqual(z.phys[(0x7F << 13) | 0x10], 0x5A)
        self.assertEqual(z.phys[(0x7F << 13) | 0x1FFF], 0xA5)
        # The other six upper slots differ between the banks.
        self.assertNotEqual(z.mapper.translate(0xC000),
                            ("sram", 0x56 << 13))

    def test_code_runs_across_a_bank_switch_from_mapped_memory(self):
        # Code at 0000h executes from ROM page 0 in both banks while the
        # data window E000h shows a different page in each bank.
        entries = [0, 0, 0, 0, 0, 0, 0, 0x60,
                   0, 0, 0, 0, 0, 0, 0, 0x61]
        code = []
        for i, page in enumerate(entries):
            code += entry_write(i, page)
        code += control(0x02) + ld_a(0x11) + st_a(0xE000)
        code += control(0x03) + ld_a(0x22) + st_a(0xE000) + HALT
        z = make(code)
        go(z)
        self.assertEqual(z.phys[0x60 << 13], 0x11)
        self.assertEqual(z.phys[0x61 << 13], 0x22)


class TestPortDecode(unittest.TestCase):
    BASE = [0, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
            0, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56]

    def prologue(self):
        code = []
        for i, page in enumerate(self.BASE):
            code += entry_write(i, page)
        return code

    def test_out_n_a_drives_a_on_the_high_address_byte(self):
        # LD A,2Ah / OUT (02h),A is the I/O write at 2A02h: entry 2.
        z = make(self.prologue() + ld_a(0x2A) + out_n(0x02) + HALT)
        go(z)
        self.assertEqual(z.mapper.entries[2], 0x2A)
        expect = list(self.BASE)
        expect[2] = 0x2A
        self.assertEqual(list(z.mapper.entries), expect)

    def test_high_bits_a8_to_a14_alias_the_entry_write(self):
        for a in (0x00, 0x01, 0x2A, 0x55, 0x7F):
            z = make(self.prologue() + ld_a(a) + out_n(0x0A) + HALT)
            go(z)
            self.assertEqual(z.mapper.entries[10], a)

    def test_a15_set_never_writes_an_entry(self):
        for low in range(0x10):
            for a in (0x80, 0xAA, 0xFF):
                if low == 0x0F:
                    continue                    # control, tested below
                z = make(self.prologue() + ld_a(a) + out_n(low) + HALT)
                go(z)
                self.assertEqual(list(z.mapper.entries), self.BASE,
                                 "low %02X A %02X" % (low, a))
                self.assertEqual(
                    (z.mapper.dat_bank, z.mapper.map_enable), (0, 0))

    def test_out_c_a_uses_b_for_the_high_byte(self):
        z = make(self.prologue() + ld_bc(0x0003) + ld_a(0x33) + OUT_C_A +
                 ld_bc(0x8004) + ld_a(0x34) + OUT_C_A +
                 ld_bc(0x2A05) + ld_a(0xC5) + OUT_C_A + HALT, strict=False)
        go(z)
        expect = list(self.BASE)
        expect[3] = 0x33
        expect[5] = 0x45                         # data bit 7 is dropped
        self.assertEqual(list(z.mapper.entries), expect)

    def test_ports_outside_the_slot_write_no_entry(self):
        code = self.prologue()
        for low in (0x10, 0x1F, 0x22, 0x80, 0xFF):
            code += ld_a(0x33) + out_n(low)
        z = make(code + HALT)
        go(z)
        self.assertEqual(list(z.mapper.entries), self.BASE)

    def test_control_write_at_800f_leaves_the_entries(self):
        values = (0x00, 0x01, 0x02, 0x03, 0xFC, 0xFD, 0xFE, 0xFF, 0x7F)
        code = self.prologue()
        ends = []
        for v in values:
            code += control(v)
            ends.append(len(code))
        z = make(code + HALT)
        for v, end in zip(values, ends):
            go(z, stop=end)
            self.assertEqual(list(z.mapper.entries), self.BASE)
            self.assertEqual(z.mapper.dat_bank, v & 1, "value %02X" % v)
            self.assertEqual(z.mapper.map_enable, (v >> 1) & 1,
                             "value %02X" % v)

    def test_control_decodes_a15_and_low_byte_only(self):
        code = self.prologue()
        ends = []
        for bc, a in ((0xFF0F, 0x03),     # A8-A14 alias: the control
                      (0x800E, 0x00),     # low byte 0Eh: not the control
                      (0x8000, 0x00),     # A15 set: no entry write
                      (0x7F0F, 0x00)):    # A15 clear: entry 15
            code += ld_bc(bc) + ld_a(a) + OUT_C_A
            ends.append(len(code))
        z = make(code + HALT, strict=False)
        go(z, stop=ends[0])
        self.assertEqual((z.mapper.dat_bank, z.mapper.map_enable), (1, 1))
        go(z, stop=ends[2])
        self.assertEqual((z.mapper.dat_bank, z.mapper.map_enable), (1, 1))
        self.assertEqual(list(z.mapper.entries), self.BASE)
        go(z, stop=ends[3])
        self.assertEqual((z.mapper.dat_bank, z.mapper.map_enable), (1, 1))
        expect = list(self.BASE)
        expect[15] = 0
        self.assertEqual(list(z.mapper.entries), expect)

    def test_block_output_counts_b_down_before_the_address(self):
        # OUTI and OTIR drive the decremented B on A8-A15.
        z = make(self.prologue() + ld_hl(0x0300) + ld_bc(0x8002) + OUTI +
                 ld_bc(0x8103) + OUTI + ld_bc(0x0204) + OTIR + HALT,
                 strict=False)
        z.phys[0x300] = 0x2B
        z.phys[0x303] = 0x6C
        go(z)
        expect = list(self.BASE)
        expect[2] = 0x2B                         # B-1 = 7Fh: A15 = 0
        # 8103h counts down to 80h: A15 = 1, no write to entry 3.
        # OTIR at 0204h: the first byte goes to 0104h (entry 4), the
        # second to 0004h (entry 4 again): the last byte stays.
        expect[4] = 0x6C
        self.assertEqual(list(z.mapper.entries), expect)

    def test_mapper_ports_do_not_read_back(self):
        z = make(self.prologue() + in_n(0x02) + [0x47] + in_n(0x0F) +
                 [0x4F] + HALT)
        go(z)
        self.assertEqual((z.b, z.c), (0xFF, 0xFF))


class TestStrictEntryWrites(unittest.TestCase):
    """Wiring-agnostic entry writes (2.7.4): the page on D0-D6 and on
    A8-A14.  The strict mapper, the default, stops on a write whose data
    byte and A8-A14 disagree."""

    def test_agreeing_writes_pass(self):
        z = make(ld_a(0x45) + out_n(0x03) +              # OUT (n),A
                 ld_a(0x46) + [0x47] + [0x0E, 0x04] +    # LD B,A; LD C,4
                 OUT_C_A + control(0x02) + HALT)
        self.assertTrue(z.mapper.strict)
        go(z)
        self.assertEqual(z.mapper.entries[3:5], [0x45, 0x46])
        self.assertEqual(z.mapper.map_enable, 1)

    def test_a_disagreeing_write_stops_the_run(self):
        for code in (ld_bc(0x0003) + ld_a(0x33) + OUT_C_A,
                     ld_hl(0x0300) + ld_bc(0x0102) + OUTI):
            z = make(code + HALT)
            z.phys[0x300] = 0x2B
            before = list(z.mapper.entries)
            with self.assertRaises(SystemExit) as ctx:
                go(z)
            self.assertIn("A8-A14", str(ctx.exception))
            self.assertEqual(list(z.mapper.entries), before)

    def test_the_control_register_is_not_an_entry_write(self):
        z = make(control(0x01) + HALT)          # 800Fh: B = 80h, A = 01h
        go(z)
        self.assertEqual((z.mapper.dat_bank, z.mapper.map_enable), (1, 0))

    def test_non_strict_takes_the_write(self):
        z = make(ld_bc(0x0003) + ld_a(0x33) + OUT_C_A + HALT, strict=False)
        go(z)
        self.assertEqual(z.mapper.entries[3], 0x33)

    def test_the_reference_mapper_agrees(self):
        mapper = z80sim.Mk2Mapper(1)
        self.assertTrue(mapper.io_write(0x4505, 0x45))
        with self.assertRaises(SystemExit):
            mapper.io_write(0x0005, 0x45)
        self.assertTrue(mapper.io_write(0x800F, 0x02))
        loose = z80sim.Mk2Mapper(1, strict=False)
        self.assertTrue(loose.io_write(0x0005, 0x45))
        self.assertEqual(loose.entries[5], 0x45)


class TestBoardSelection(unittest.TestCase):
    def test_mk1_is_the_default_and_keeps_its_latch(self):
        z = Z80(bytes(0x10000), bytes(0x10000))
        self.assertEqual(z.board, "mk1")
        self.assertIsNone(z.mapper)
        self.assertEqual(z.latch, 0)

    def test_mk2_has_no_mk1_latch(self):
        z = make(HALT)
        self.assertFalse(hasattr(z, "latch"))
        self.assertFalse(hasattr(z, "decoder"))
        with self.assertRaises(SystemExit):
            z.bank_of(0)
        # Port 00h is mapper entry 0 here, never a latch.
        z = make(ld_a(0x21) + out_n(0x00) + HALT)
        go(z)
        self.assertEqual(z.mapper.entries[0], 0x21)
        self.assertFalse(hasattr(z, "latch"))

    def test_unknown_board_is_rejected(self):
        with self.assertRaises(ValueError):
            Z80(bytes(16), bytes(16), board="mk3")

    def test_image_sizes(self):
        z = Z80(b"\x01\x02", b"\x03", board="mk2")
        self.assertEqual(z.phys[0:3], b"\x01\x02\xff")
        self.assertEqual(z.phys[SRAM_BASE], 3)
        self.assertEqual(len(z.phys), 0x100000)
        with self.assertRaises(ValueError):
            Z80(bytes(ROM_SIZE + 1), b"", board="mk2")


if __name__ == "__main__":
    unittest.main()
