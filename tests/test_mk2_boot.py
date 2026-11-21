#!/usr/bin/env python3
"""The Zephyr-80 Mk2 boot proof image on the simulator's Mk2 board.

The image (tests/target/mk2boot/mk2boot.asm, `make mk2boot`) runs the
boot sequence of architecture 2.7.5 without the nucleus: pass-mode start
from ROM, both DAT banks programmed, the SRAM common page at entries 7
and 15, mapping enabled from ROM page 0 (mapped identically in both
views), the payload copied from a mapped ROM window into the common page
and entered there, the early-console banner, DAT_BANK switched both ways
from common code with a check value read through each map, and the IOC
reset (CMD_RESET on the command lane) back to the pass-mode ROM view,
where the second boot prints the banner again.

Skipped when the image has not been built.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import z80sim  # noqa: E402
from z80sim import IocCommandPeer, StopSet, Z80, ioc_crc16  # noqa: E402

IMAGE = os.path.join(ROOT, "build-mk2boot", "mk2boot.bin")
MAP = os.path.join(ROOT, "build-mk2boot", "mk2boot.map")
LIMIT = 2000000

COMMON_PAGE = 0x7F
SYS_CHECK_PAGE = 0x41
WRK_CHECK_PAGE = 0x42
DAT_IMAGE = [0x00, 0x07, 0x40, 0x05, 0x41, 0x43, 0x44, 0x7F,
             0x00, 0x45, 0x46, 0x06, 0x42, 0x47, 0x48, 0x7F]


def boot_lines(count):
    return ("KonaOS Mk2 boot %02X\r\n"
            "sys:SYS-05 wrk:WRK-06 sys:SYS-05 ram sys:%02X wrk:%02X\r\n"
            % (count, 0x50 + count, 0xA0 + count))


TRANSCRIPT = (boot_lines(1) + "reboot\r\n" + boot_lines(2) +
              "done\r\n").encode("ascii")


@unittest.skipUnless(os.path.isfile(IMAGE), "make mk2boot builds the image")
class TestMk2Boot(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(IMAGE, "rb") as handle:
            cls.rom = handle.read()
        cls.syms = z80sim.read_symbols(MAP)

    def machine(self, seed=1):
        z = Z80(self.rom, b"", board="mk2", mapper_seed=seed)
        z.ioc_peer = IocCommandPeer()
        return z

    def run_to(self, z, address):
        reason, _ = z.run(LIMIT, stops=StopSet([address]), check_first=False)
        self.assertEqual(reason, "stop", "pc %04X never reached" % address)

    def test_boots_maps_hands_off_and_reboots(self):
        for seed in (1, 2, 3, 4):
            z = self.machine(seed)
            reason, _ = z.run(LIMIT, halt=True)
            self.assertEqual(reason, "halt", "seed %d" % seed)
            self.assertEqual(bytes(z.sio_tx), TRANSCRIPT, "seed %d" % seed)
            self.assertEqual(z.ioc_peer.host_resets, 1)
            self.assertEqual(z.ioc_peer.requests,
                             [(IocCommandPeer.CMD_RESET, 0x01, 0x00, b"")])

    def test_mapping_switch_runs_in_rom_page_zero(self):
        z = self.machine()
        self.run_to(z, self.syms["boot_mapped"])
        self.assertEqual((z.mapper.map_enable, z.mapper.dat_bank), (1, 0))
        self.assertEqual(list(z.mapper.entries), DAT_IMAGE)
        # The next instruction is the same ROM byte in both views.
        self.assertEqual(z.mapper.translate(z.pc), ("rom", z.pc))

    def test_payload_runs_from_the_common_page(self):
        z = self.machine()
        payload = self.syms["payload"]
        size = self.syms["payload_end"] - payload
        self.run_to(z, self.syms["banner_start"])
        common = COMMON_PAGE << 13
        self.assertEqual(z.mapper.translate(z.pc),
                         ("sram", common | (z.pc & 0x1FFF)))
        self.assertEqual(bytes(z.phys[common:common + size]),
                         self.rom[payload:payload + size])
        # SP 0000h: the first push lands at FFFEh, the common page's top.
        self.assertEqual(z.sp, 0x0000)
        self.assertEqual(z.mapper.translate(0xFFFE),
                         ("sram", common | 0x1FFE))
        # The software mirror matches the hardware state.
        self.assertEqual(list(z.phys[common | 0x1E00:common | 0x1E10]),
                         DAT_IMAGE)
        self.assertEqual(z.phys[common | 0x1E10], 0x02)

    def test_check_values_are_read_through_each_map(self):
        z = self.machine()
        z.run(LIMIT, halt=True)
        self.assertEqual(z.phys[SYS_CHECK_PAGE << 13], 0x52)
        self.assertEqual(z.phys[WRK_CHECK_PAGE << 13], 0xA2)
        self.assertEqual((z.mapper.map_enable, z.mapper.dat_bank), (1, 0))
        self.assertEqual(z.phys[(COMMON_PAGE << 13) | 0x1E10], 0x02)

    def test_ioc_reset_returns_to_the_pass_mode_rom_view(self):
        z = self.machine()
        reason, _ = z.run(LIMIT, stops=StopSet([0x0000]), check_first=False)
        self.assertEqual(reason, "stop")
        self.assertIn(b"reboot\r\n", bytes(z.sio_tx))
        self.assertEqual((z.mapper.map_enable, z.mapper.dat_bank), (0, 0))
        self.assertNotEqual(list(z.mapper.entries), DAT_IMAGE)
        self.assertEqual(z.mapper.translate(0x0000), ("rom", 0x0000))
        self.assertEqual(z.mapper.translate(0xE000), ("rom", 0xE000))
        self.assertFalse(z.iff1)
        self.assertEqual(z.rt_hz, 0)
        # The request on the lane is a well-formed CMD_RESET frame.
        start, raw = z.ioc_peer.captures[-1]
        frame = raw[start:start + 9]
        self.assertEqual(frame[:7], bytes([0xA5, 0x5A, 0x03, 0x00,
                                           IocCommandPeer.CMD_RESET,
                                           0x01, 0x00]))
        crc = ioc_crc16(frame[2:7])
        self.assertEqual(frame[7:9], bytes([crc >> 8, crc & 0xFF]))


if __name__ == "__main__":
    unittest.main()
