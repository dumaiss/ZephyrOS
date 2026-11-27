"""ColecoGo on the Zephyr-80 Mk2 board: the launch reaches the cartridge.

The Mk2 production image boots on the simulator with this tree's
colecogo.exe, a synthetic COLECO.ROM and a 256-byte cartridge on the
card.  The synthetic BIOS carries the standard BIOS's bytes at every site
ColecoGo patches and, at 0000h, `LD HL,(800Ah); JP (HL)`, the jump to the
cartridge's start address; the cartridge starts with the AA55h header and
its code is `EI; JR $`.  From the shell (`cd /fs`, then `colecogo GAME.ROM`)
the run must reach the cartridge entry in the working mapping, with the
eight pages of the ColecoVision space mapped (the adapted BIOS, the FFh
expansion area, the cleared RAM, the cartridge with its FFh fill), the
image entered with interrupts masked and IM 1, the V9958 interrupt
routed to the NMI, and afterwards no NMI and no CTC interrupt while the
cartridge loops with interrupts enabled.
"""

from __future__ import annotations

import pathlib
import re
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
KONA_TESTS = ROOT.parents[1] / "tests"
sys.path.insert(0, str(KONA_TESTS))

from replay import app_board, board_replay  # noqa: E402

EXE = ROOT / "build" / "user" / "colecogo.exe"
PAGE = 0x2000
CART_ENTRY = 0x8024
BOOT_BUDGET = 60_000_000
BUDGET = 60_000_000
AFTER = 400_000

# colecogo.c bios_patches: (offset, expected, replacement).
BIOS_PATCHES = (
    (0x0173, 0xFF, 0xE0), (0x017C, 0xFF, 0xE0), (0x018E, 0xFF, 0xE0),
    (0x023E, 0xFF, 0xE0), (0x0242, 0xFF, 0xE0), (0x0246, 0xFF, 0xE0),
    (0x024A, 0xFF, 0xE0), (0x0336, 0xFF, 0xE0), (0x0355, 0xFF, 0xE0),
    (0x1988, 0x0F, 0x07), (0x18D7, 0xBF, 0xA1), (0x18DC, 0xBF, 0xA1),
    (0x18DF, 0xBE, 0xA0), (0x1C93, 0xBF, 0xA1), (0x1C98, 0xBF, 0xA1),
    (0x1CAB, 0xBE, 0xA0), (0x1CCC, 0xBF, 0xA1), (0x1CD1, 0xBF, 0xA1),
    (0x1D0A, 0xBF, 0xA1), (0x1D0D, 0xBF, 0xA1), (0x1D12, 0xBE, 0xA0),
    (0x1D40, 0xBF, 0xA1), (0x1D43, 0xBF, 0xA1), (0x1D47, 0xBE, 0xA0),
    (0x1D58, 0xBF, 0xA1),
)


def synthetic_bios():
    bios = bytearray(b"\x5A" * PAGE)
    for offset, expected, _ in BIOS_PATCHES:
        bios[offset] = expected
    bios[0:4] = b"\x2A\x0A\x80\xE9"          # LD HL,(800Ah); JP (HL)
    return bytes(bios)


def adapted(bios):
    out = bytearray(bios)
    for offset, _, replacement in BIOS_PATCHES:
        out[offset] = replacement
    return bytes(out)


def cartridge():
    cart = bytearray(256)
    cart[0:2] = b"\xAA\x55"
    cart[0x0A:0x0C] = CART_ENTRY.to_bytes(2, "little")
    cart[0x24:0x27] = b"\xFB\x18\xFE"         # EI; JR $
    return bytes(cart)


@unittest.skipUnless(app_board() == "mk2",
                     "the Zephyr-80 handoff is tests/test_handoff.py's")
@unittest.skipUnless(EXE.is_file(), "make BOARD=mk2 builds colecogo.exe")
class PersonalityLaunchTest(unittest.TestCase):
    def test_launch_reaches_the_cartridge_entry(self) -> None:
        replay = board_replay("mk2")
        bios, cart = synthetic_bios(), cartridge()
        z = replay.make_cpu()
        z.hid.fs2_add("COLECOGO.EXE", EXE.read_bytes())
        z.hid.fs2_add("COLECO.ROM", bios)
        z.hid.fs2_add("GAME.ROM", cart)
        replay.boot_to_shell(z, limit=BOOT_BUDGET)
        replay.send_line(z, "cd /fs", "/> ")
        start = len(z.glyphs)
        z.hid.hid_queue.extend(b"colecogo GAME.ROM\r")

        def at_entry(_step):
            return True if z.mapper.dat_bank == 1 else None

        step = replay.drive(z, BUDGET, watch={CART_ENTRY}, hook=at_entry)
        replay.pump(z)
        text = "".join(z.glyphs[start:])
        self.assertIsNotNone(step, text)
        self.assertRegex(text, r"Images validated; starting ColecoVision "
                               r"\(no return\)\.")
        self.assertIsNone(re.search(r"ColecoGo: ", text), text)

        expect = {0: adapted(bios), 1: b"\xFF" * PAGE, 2: b"\xFF" * PAGE,
                  3: b"\x00" * PAGE,
                  4: cart + b"\xFF" * (PAGE - len(cart)),
                  5: b"\xFF" * PAGE, 6: b"\xFF" * PAGE, 7: b"\xFF" * PAGE}
        for n, want in expect.items():
            got = bytes(z.rd(n * PAGE + i) for i in range(PAGE))
            self.assertEqual(got, want, "logical page %d" % n)
        self.assertEqual(len(set(z.mapper.entries[8:16])), 8)
        self.assertFalse(z.iff1)
        self.assertEqual(z.im, 1)
        self.assertEqual(z.vdp.latch & 0x01, 0x01)   # V9958 INT -> NMI
        self.assertEqual(z.vdp.regs[1], 0x00)         # no frame interrupt

        nmi = z.nmi_count
        expiries = list(z.ctc.expiries)
        delivered = list(z.ctc.delivered)
        replay.drive(z, AFTER)
        self.assertIn(z.pc, (CART_ENTRY + 1, CART_ENTRY + 2))
        self.assertTrue(z.iff1)
        self.assertEqual(z.nmi_count, nmi)
        self.assertEqual(z.rt_hz, 0)
        self.assertEqual([c.running for c in z.ctc.channels], [False] * 4)
        self.assertEqual(list(z.ctc.expiries), expiries)
        self.assertEqual(list(z.ctc.delivered), delivered)


if __name__ == "__main__":
    unittest.main()
