"""M9 writable-provider acceptance replay.

Runs the m9-provider-selftest image in the Z80 simulator.  The image boots
SIO-only, loads the test-only `ramfs` provider through driver ABI v3, the
registration fixtures (`m9.probe`, `m9.dup`, `m9.fail`) through the target
boot hook, and mounts ramfs at `/` (required) and `/R2` (optional).  The
target selftest then covers registration/rollback, mount state, the full
mutation set, the root merge, the section-33 size set, 32-bit seek, stale
handles, a failed confirmed write, cross-mount rename and the provider
counters.

The FAIL marker and the result byte prove the linked behavior; the host
sides (mount-generation rules, provider ABI shape) are in
tests/test_m9_vfs.py.
"""

import os
import re
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import (Z80, IocCommandPeer, V9958Peer, read_symbols,
                    skip_serial_connect_delay)  # noqa: E402
import romfspack  # noqa: E402

RET = 0x1234
STACK = 0xFFFE
STEP_LIMIT = 120000000
ISR_STACK_BASE = 0xDF00

PROVIDER_IMAGE_SUBDIR = "build-m9-provider-selftest"
PROVIDER_IMAGE_NAME = "zephyros-m9-provider-selftest.bin"

SELFTEST_OK = "m9 provider selftest: OK"
FAIL_LINE = "m9 provider selftest: FAIL "

# Case name -> code, from tests/target/include/m9_provider_selftest.h.  Used
# to name the failing case in the assertion message.
CASES = {
    0x01: "boot baseline",
    0x02: "provider registry and rollback",
    0x03: "mount state / refuse-mount control",
    0x04: "mkdir/rmdir",
    0x05: "open flags and EISDIR/ENOTDIR",
    0x06: "write and stat size",
    0x07: "read-back compare",
    0x08: "32-bit seek",
    0x09: "rename within a mount",
    0x0A: "unlink",
    0x0B: "root merge and shadowing",
    0x0C: "section-33 sizes",
    0x0D: "stale handle",
    0x0E: "failed confirmed write",
    0x0F: "cross-mount rename (EXDEV)",
    0x10: "mountpoint/EROFS errors",
    0x11: "provider counters",
    0x7F: "unrecognized mapping",
}


class M9ProviderSimBase(unittest.TestCase):
    def setUp(self):
        self.bin_path = os.path.join(ROOT, PROVIDER_IMAGE_SUBDIR,
                                     PROVIDER_IMAGE_NAME)
        if not os.path.exists(self.bin_path):
            self.skipTest("%s missing; build the image first" % self.bin_path)
        with open(self.bin_path, "rb") as handle:
            self.image = handle.read()
        self.syms = read_symbols(
            os.path.join(ROOT, PROVIDER_IMAGE_SUBDIR, "zephyros.map"))
        self.romfs_path = os.path.join(ROOT, PROVIDER_IMAGE_SUBDIR,
                                       "recovery.romfs")
        if not os.path.exists(self.romfs_path):
            self.skipTest("%s missing; build the image first"
                          % self.romfs_path)
        with open(self.romfs_path, "rb") as handle:
            _header, self.entries = romfspack.parse_image(handle.read())

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

    def start_kernel(self, z):
        self.crt0(z)
        z.push16(RET)
        z.pc = self.syms["_kernel_main"]

    def step_once(self, z):
        skip_serial_connect_delay(
            z, self.syms["_kernel_serial_connect_delay"])
        z.step()

    def boot_to_selftest(self, z, limit=STEP_LIMIT):
        self.start_kernel(z)
        for _ in range(limit):
            self.step_once(z)
            if (b"m9 provider selftest: OK\r\n" in z.sio_tx or
                    b"m9 provider selftest: FAIL" in z.sio_tx):
                return
        raise AssertionError("provider selftest never completed (PC=%04X)"
                             % z.pc)

    def run_to_halt(self, z, limit=STEP_LIMIT):
        self.start_kernel(z)
        for _ in range(limit):
            self.step_once(z)
            if z.halted:
                return bytes(z.sio_tx)
        raise AssertionError("kernel never halted (PC=%04X)" % z.pc)


class TestM9ProviderAcceptance(M9ProviderSimBase):
    def test_boot_and_selftest(self):
        z = self.make_cpu()
        self.boot_to_selftest(z)
        sio = z.sio_tx.decode("ascii", "replace")

        for line in ("ZephyrOS 0.0", "platform: Zephyr-80",
                     "kernel entry: OK", "serial: OK"):
            self.assertIn(line + "\r\n", sio)
        # Both ramfs mounts succeed, so the normal mount pass prints no
        # `unavailable` line.
        self.assertNotIn("unavailable", sio)
        self.assertIn(SELFTEST_OK + "\r\n", sio)
        self.assertNotIn("FAIL", sio)
        self.assertNotIn("PANIC", sio)
        self.assertEqual(
            z.bank7[self.syms["_m9_provider_selftest_result"]], 0)

    def test_no_case_fails(self):
        z = self.make_cpu()
        self.boot_to_selftest(z)
        sio = z.sio_tx.decode("ascii", "replace")
        for code, name in CASES.items():
            self.assertNotIn("%s0x%02X" % (FAIL_LINE, code), sio,
                             "provider selftest case failed: %s" % name)
        self.assertEqual(
            z.bank7[self.syms["_m9_provider_selftest_result"]], 0)

    def test_mount_state_reports_root_and_r2_mounted(self):
        z = self.make_cpu()
        self.boot_to_selftest(z)
        base = self.syms["_vfs_mount_state"]
        with open(os.path.join(ROOT, "kernel", "vfs", "vfs.h")) as fh:
            text = fh.read()
        match = re.search(
            r"sizeof\(struct vfs_mount_runtime\)\s*==\s*(\d+)", text)
        self.assertIsNotNone(match, "vfs_mount_runtime size is not pinned")
        stride = int(match.group(1))
        # Manifest order: /SYSTEM (system), / (required), /R2 (optional).
        self.assertEqual(z.bank7[base], 1,
                         "the system entry must be MOUNTED")
        self.assertEqual(z.bank7[base + stride], 1,
                         "the / entry must be MOUNTED")
        self.assertEqual(z.bank7[base + 2 * stride], 1,
                         "the /R2 entry must be MOUNTED")


if __name__ == "__main__":
    unittest.main()
