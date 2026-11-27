#!/usr/bin/env python3
"""SYS_PERSONALITY on the Zephyr-80 Mk2 board (docs/abi.md section 8.4).

The Mk2 production image (`make BOARD=mk2`) runs the personality probe
from the card (tests/target/process/persona.c, build-mk2/sys/user):

* refusals - a count of 7, a page past its object's end, a nonzero
  reserved byte, a handle no object has, a freed object - each answer
  -KOS_EINVAL and leave KonaOS running: the program prints them and exits
  0 to the shell;
* `persona go` holds the RT interrupt at 1 kHz, then makes the call.  At
  the switch's first instruction the RT clock and every CTC channel are
  stopped, interrupts are masked and the sound card was reset; at the
  entry the working bank shows the eight object pages (each page's
  first and last 256 bytes), interrupts are masked, IM 1; and in the
  image's `EI; JR $` loop no NMI or maskable interrupt arrives.

Skipped when the image or the probe has not been built.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from test_mk2_programs import (BUDGET, COMMON_PAGE, PAGE,  # noqa: E402
                               Mk2ProgramCase, built, program)

EINVAL = 2
PAGES = 8
LOOP = 0x0001            # persona.c: EI at 0000h, JR $ at 0001h
AFTER = 400_000          # steps run in the image afterwards


def tag(n):
    """persona.c PERSONA_TAG."""
    return (0xA1 + n * 0x11) & 0xFF


@unittest.skipUnless(built("persona"), "make BOARD=mk2 builds the programs")
class TestPersonality(Mk2ProgramCase):
    def boot_probe(self):
        return self.boot({"PERSONA.EXE": program("persona")})

    def test_refusals_leave_konaos_running(self):
        z = self.boot_probe()
        fresh = self.run_line(z, "/fs/persona.exe")
        self.assertIn("persona: refused %s\n"
                      % " ".join(["-%d" % EINVAL] * 5), fresh)
        self.assertEqual(self.status(fresh), 0, fresh)
        self.assertIn("refused", self.run_line(z, "/fs/persona.exe"))

    def test_go_stops_the_sources_and_enters_the_image(self):
        z = self.boot_probe()
        switch = self.syms["_kos_personality_switch"]
        sound_before = len(z.sound_peer.writes)
        seen = {}

        def hook(_step):
            if z.pc == switch and z.mapper.dat_bank == 0:
                seen["switch"] = dict(
                    iff1=z.iff1, rt_hz=z.rt_hz,
                    ctc=[c.running for c in z.ctc.channels],
                    sound=len(z.sound_peer.writes), nmi=z.nmi_count)
                return None
            if z.pc == 0x0000 and z.mapper.dat_bank == 1:
                return True
            return None

        since = len(z.glyphs)
        z.hid.hid_queue.extend(b"/fs/persona.exe go\r")
        step = self.drive(z, BUDGET, watch={switch, 0x0000}, hook=hook)
        self.pump(z)
        trace = self.trace(z, since)
        self.assertIsNotNone(step, trace)
        self.assertIn("persona: refused", trace)
        self.assertIn("persona: go", trace)
        self.assertNotIn("persona: returned", trace)

        # Before the remap: the RT clock ran and is stopped, the CTC is
        # stopped, interrupts are masked, the sound card was reset.
        at = seen["switch"]
        self.assertGreater(at["nmi"], 0)
        self.assertEqual(at["rt_hz"], 0)
        self.assertEqual(z.hid.rt_requests[-1][0], 0)
        self.assertFalse(at["iff1"])
        self.assertEqual(at["ctc"], [False] * 4)
        self.assertGreater(at["sound"], sound_before)

        # At the entry: the whole working mapping is the object's pages.
        self.assertEqual(z.mapper.map_enable, 1)
        entries = z.mapper.entries[8:16]
        self.assertEqual(len(set(entries)), PAGES)
        self.assertNotIn(COMMON_PAGE, entries)
        for n in range(PAGES):
            base = n * PAGE
            first = bytes(z.rd(base + i) for i in range(256))
            last = bytes(z.rd(base + PAGE - 256 + i) for i in range(256))
            want = bytes([tag(n)]) * 256
            if n == 0:
                want = b"\xFB\x18\xFE" + want[3:]
            self.assertEqual(first, want, "page %d" % n)
            self.assertEqual(last, bytes([tag(n)]) * 256, "page %d" % n)
        self.assertFalse(z.iff1)
        self.assertEqual(z.im, 1)

        # Afterwards: interrupts enabled by the image, and nothing comes.
        nmi = z.nmi_count
        expiries = list(z.ctc.expiries)
        delivered = list(z.ctc.delivered)
        self.drive(z, AFTER)
        self.assertIn(z.pc, (LOOP, LOOP + 1))
        self.assertTrue(z.iff1)
        self.assertEqual(z.nmi_count, nmi)
        self.assertEqual(z.rt_hz, 0)
        self.assertEqual(list(z.ctc.expiries), expiries)
        self.assertEqual(list(z.ctc.delivered), delivered)
        self.assertEqual(z.mapper.entries[8:16], entries)


if __name__ == "__main__":
    unittest.main()
