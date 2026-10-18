"""The V9958 vertical-blank event on the production image.

A program holding /dev/video exclusively subscribes to the VDP's frame
interrupt with `notify`, enables the interrupt itself (IE0 in R#1) and
spins without a system call: each frame's interrupt (vector FFh, the
source IRQ_VDP_FF) is acknowledged by the V9958 driver, which posts the
event, and the interrupt's return delivers it into the program.  The
simulator's V9958 model raises the frame flag once a period, clears it on
a status-register-0 read and holds /INT while the flag and IE0 are set.
"""

import os
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "tests"))

from test_hwtest_sim import KEEP_CONTENT, KEEP_NAME, HwtestReplay  # noqa: E402
from replay import SHELL_PROMPT  # noqa: E402
from z80sim import Ctc, IocCommandPeer  # noqa: E402

VBLANK_EXE = os.path.join(ROOT, "build", "user", "vblank.exe")
# 60 Hz at 10 MHz.
FRAME_TSTATES = 166667


@unittest.skipUnless(os.path.exists(VBLANK_EXE), "run make")
class TestVblankEvent(HwtestReplay):

    def test_a_spinning_program_receives_every_frame(self):
        z = self.make_cpu()
        Ctc().attach(z)
        with open(VBLANK_EXE, "rb") as handle:
            exe = handle.read()
        files = {"/".join(KEEP_NAME): KEEP_CONTENT, "VBLANK.EXE": exe}
        z.hid = z.ioc_peer = IocCommandPeer(fs2_media=True, fs2_files=files)
        self.boot_to_shell(z, prompt=SHELL_PROMPT)
        z.vdp.frame_period = FRAME_TSTATES
        since = len(z.glyphs)
        z.hid.hid_queue.extend(b"/fs/vblank.exe\r")
        self.run_to_pattern(z, r"vblank: (PASS|FAIL)", since=since)
        fresh = self.trace(z, since)
        self.assertIn("vblank: PASS", fresh, fresh[-300:])
        self.assertRegex(fresh, r"vblank: \d+ frames in \d+ calls")
        # Every frame interrupt was acknowledged once: no storm (an
        # unacknowledged level interrupt would be offered again and again).
        self.assertGreaterEqual(z.vdp.frame_irqs, 5)
        self.assertLessEqual(z.vdp.frame_irqs, z.vdp.frames)
        # The hold ended with the frame interrupt off and status register
        # 2 selected again (the display init the release runs).
        self.assertEqual(z.vdp.regs[1] & 0x20, 0)
        self.assertEqual(z.vdp.regs[15] & 0x0F, 2)


if __name__ == "__main__":
    unittest.main()
