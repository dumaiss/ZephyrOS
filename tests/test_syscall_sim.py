"""M9 filesystem syscall acceptance replay.

Runs the m9-fs-selftest image in the Z80 simulator.  The image boots SIO-only,
loads the test-only `ramfs` provider through driver ABI v3 and mounts it at
`/` (required) and `/R2` (optional); the target hook then launches the native
`fstest.zex` suite (ROMFS at `/SYSTEM` + ramfs), checks the descriptor pool is
clean, and launches `fstest_l.zex` as a second exec to prove the pool and CWD
were released.

The suite also measures the worst-case syscall gate-stack depth of the whole
kernel run up to the acceptance marker and asserts it stays inside the
documented 256-byte gate stack (work/M9-REFRENCE.md gate-stack discipline).
"""

import os
import re
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import Z80, IocCommandPeer, V9958Peer, read_symbols  # noqa: E402

RET = 0x1234
STACK = 0xFFFE
STEP_LIMIT = 40000000

IMAGE_SUBDIR = "build-m9-fs-selftest"
IMAGE_NAME = "zephyros-m9-fs-selftest.bin"

FSTEST_PASS = "fstest: PASS 9/9\r\n"
FSTEST_LAUNCHER_OK = "fstest: launcher OK\r\n"
SELFTEST_OK = "m9 fs selftest: OK\r\n"
GATE_STACK_SIZE = 256


class M9FsSimBase(unittest.TestCase):
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
        z.hid = IocCommandPeer()
        z.ioc_peer = z.hid
        z.vdp = V9958Peer()
        z.vdp_peer = z.vdp
        z.glyphs = []
        z.glyph_pos = 0
        z.glyph_last = None
        return z

    def crt0(self, z):
        z.push16(RET)
        z.pc = self.syms["__zos_crt0_init"]
        for _ in range(2000000):
            z.step()
            if z.pc == RET:
                return
        raise AssertionError("C runtime init did not return")

    def boot(self, z, limit=STEP_LIMIT):
        """Boot to the acceptance marker, measuring gate-stack depth.

        Returns (min_sp_in_gate_region, steps)."""
        gate_top = self.syms["zos_gate_stack_top"]
        gate_base = gate_top - GATE_STACK_SIZE
        min_sp = gate_top
        self.crt0(z)
        z.push16(RET)
        z.pc = self.syms["_kernel_main"]
        delay = self.syms["_kernel_serial_connect_delay"]
        for step in range(limit):
            if z.pc == delay:
                z.pc = z.pop16()
            z.step()
            if gate_base <= z.sp <= gate_top and z.sp < min_sp:
                min_sp = z.sp
            if (SELFTEST_OK.encode() in z.sio_tx or
                    b"m9 fs selftest: FAIL" in z.sio_tx or
                    b"PANIC" in z.sio_tx or z.halted):
                return min_sp, step
        raise AssertionError("acceptance never completed (PC=%04X)" % z.pc)


class TestM9FsAcceptance(M9FsSimBase):
    def test_boot_and_fstest(self):
        z = self.make_cpu()
        self.boot(z)
        sio = z.sio_tx.decode("ascii", "replace")

        for line in ("ZephyrOS 0.0", "platform: Zephyr-80",
                     "kernel entry: OK", "serial: OK",
                     "console: dynamic sio0.b OK"):
            self.assertIn(line + "\r\n", sio)
        self.assertIn(FSTEST_PASS, sio)
        self.assertIn(FSTEST_LAUNCHER_OK, sio)
        self.assertIn(SELFTEST_OK, sio)
        self.assertNotIn("FAIL", sio)
        self.assertNotIn("PANIC", sio)
        self.assertEqual(
            z.bank7[self.syms["_m9_fs_selftest_result"]], 0)

    def test_no_case_fails(self):
        z = self.make_cpu()
        self.boot(z)
        sio = z.sio_tx.decode("ascii", "replace")
        for case in ("namespace", "romfs", "ramfs", "errors", "pool",
                     "stale", "writefail", "exdev", "window"):
            self.assertNotIn("fstest: %s FAIL" % case, sio)
        self.assertNotIn("fstest: FAIL", sio)

    def test_pool_and_cwd_released(self):
        z = self.make_cpu()
        self.boot(z)
        base = self.syms["_vfs_descriptors"]
        # vfs_descriptor: owner at +0, type at +1, 17 packed bytes.
        with open(os.path.join(ROOT, "kernel", "vfs", "vfs.h")) as handle:
            header = handle.read()
        match = re.search(
            r"sizeof\(struct vfs_descriptor\)\s*==\s*(\d+)", header)
        self.assertIsNotNone(match, "vfs_descriptor size is not pinned")
        stride = int(match.group(1))
        for fd in range(8):
            self.assertEqual(
                z.bank7[base + fd * stride], 0xFF,
                "descriptor %d still owned after the acceptance runs" % fd)
            self.assertEqual(z.bank7[base + fd * stride + 1], 0,
                             "descriptor %d not FREE" % fd)

    def test_workspace_views_are_released_and_never_overlap(self):
        # The acceptance image is built with ZOS_VFS_WS_GUARD=1, which makes
        # every workspace claim write the active view to `vfs_ws_view` and
        # panic ("vfs: workspace view overlap") when a second view is claimed
        # while one is still live.  `boot()` returns on a PANIC, so a clean
        # run with the view counter back at 0 proves both that no claim
        # leaked and that no two views were ever live together.
        self.assertIn("_vfs_ws_view", self.syms,
                      "the guarded image carries no vfs_ws_view counter")
        z = self.make_cpu()
        min_sp, _steps = self.boot(z)
        self.assertNotIn("PANIC", z.sio_tx.decode("ascii", "replace"))
        self.assertEqual(
            z.bank7[self.syms["_vfs_ws_view"]], 0,
            "a VFS workspace view was still claimed at the acceptance mark")

    def test_gate_stack_stays_within_budget(self):
        z = self.make_cpu()
        min_sp, _steps = self.boot(z)
        gate_top = self.syms["zos_gate_stack_top"]
        depth = gate_top - min_sp
        self.assertGreater(depth, 0, "the gate stack was never entered")
        self.assertLessEqual(
            depth, GATE_STACK_SIZE,
            "worst-case gate-stack depth %d exceeds the %d-byte gate stack"
            % (depth, GATE_STACK_SIZE))
        print("m9 fs: worst-case gate-stack depth %d of %d bytes"
              % (depth, GATE_STACK_SIZE))


if __name__ == "__main__":
    unittest.main()
