"""M9 VFS bootstrap acceptance tests: replay the m9-vfs-selftest image in
the Z80 simulator and byte-compare the mounted /SYSTEM contents with the
packed artifacts.

The image boots the full production module set through VFS paths, runs the
normal mount pass against tests/m9/mounts-optional-fail.json (whose optional
/DATA mount names an unregistered provider), then runs the target VFS
selftest, whose transcript goes over the direct early SIO.

The target selftest covers the path-based provider contract of
correction-02: flat mount selection, case-folded lookup, ENOENT, the
deeper-path ENOTDIR/ENOENT rule, EROFS from the null mutation operations,
root and provider-directory enumeration, and the executable gate.  The
host sides of the same contract -- the 14-operation provider ABI and the
`/SYSTEMX` non-match -- are pinned in tests/test_m9_vfs.py; the target
result byte and the FAIL marker here prove the linked behavior.
"""

import json
import os
import re
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import Z80, IocCommandPeer, V9958Peer, read_symbols  # noqa: E402
import romfspack  # noqa: E402

RET = 0x1234
STACK = 0xFFFE
STEP_LIMIT = 60000000
ISR_STACK_BASE = 0xDF00

M9_IMAGE_SUBDIR = "build-m9-vfs-selftest"
M9_IMAGE_NAME = "zephyros-m9-vfs-selftest.bin"

REQUIRED_IMAGE_SUBDIR = "build-m9-required-mount-selftest"
REQUIRED_IMAGE_NAME = "zephyros-m9-required-mount.bin"

OPTIONAL_FAIL_LINE = "vfs: /DATA unavailable 0x03"
FAIL_LINE = "m9 vfs selftest: FAIL "

# Host-owned scratch for direct kernel-API calls.  The bank-7 window is
# fully derived, so a fixed address cannot be safe; setUp derives the first
# free arena granule above the packed boot set from arena.plan.json.
SCRATCH_SIZE = 0x200

ZOS_EINVAL = 1
ZOS_ENODEV = 3
ZOS_ENOENT = 5
REQUIRED_FAIL_LINE = "vfs: required mount /DATA failed 0x03"
REQUIRED_PANIC = "PANIC: " + REQUIRED_FAIL_LINE
SELFTEST_OK = "m9 vfs selftest: OK"


class M9VfsSimBase(unittest.TestCase):
    IMAGE_DIR = M9_IMAGE_SUBDIR
    IMAGE_NAME = M9_IMAGE_NAME

    def setUp(self):
        self.bin_path = os.path.join(ROOT, self.IMAGE_DIR, self.IMAGE_NAME)
        if not os.path.exists(self.bin_path):
            self.skipTest("%s missing; build the image first" % self.bin_path)
        with open(self.bin_path, "rb") as handle:
            self.image = handle.read()
        self.syms = read_symbols(
            os.path.join(ROOT, self.IMAGE_DIR, "zephyros.map"))
        self.romfs_path = os.path.join(ROOT, self.IMAGE_DIR,
                                       "recovery.romfs")
        if not os.path.exists(self.romfs_path):
            self.skipTest("%s missing; build the image first"
                          % self.romfs_path)
        with open(self.romfs_path, "rb") as handle:
            _header, self.entries = romfspack.parse_image(handle.read())
        self.scratch = self._derive_scratch()

    def _derive_scratch(self):
        """Pick the first free arena granule above the packed boot set.

        The scratch buffer must not overlap the linked kernel, a loaded
        module or the runtime data, so it is derived from the image's
        arena plan rather than a fixed address."""
        plan_path = os.path.join(ROOT, self.IMAGE_DIR, "arena.plan.json")
        self.assertTrue(os.path.exists(plan_path),
                        "arena.plan.json missing; build the image first")
        with open(plan_path) as fh:
            arena = json.load(fh)["arena"]
        scratch = arena["start"] + arena["used_granules"] * arena["granule_size"]
        self.assertGreaterEqual(scratch, arena["start"])
        self.assertLessEqual(scratch + SCRATCH_SIZE, arena["end"],
                             "scratch %04X is not in the free arena space"
                             % scratch)
        self.assertLessEqual(scratch + SCRATCH_SIZE, ISR_STACK_BASE,
                             "scratch %04X reaches the ISR stack" % scratch)
        return scratch

    def call2(self, z, symbol, hl, de, limit=STEP_LIMIT):
        """Call `int f(const char *a, const char *b)`; result in DE."""
        z.push16(RET)
        z.hl = hl
        z.de = de
        z.pc = self.syms[symbol]
        for _ in range(limit):
            z.step()
            if z.pc == RET:
                return z.de
        raise AssertionError("%s did not return" % symbol)

    def vfs_stat(self, z, path):
        """vfs_stat(path) -> (result, struct zfs_stat bytes)."""
        path_at = self.scratch
        stat_at = self.scratch + 0x100
        raw = path.encode("ascii") + b"\x00"
        z.bank7[path_at:path_at + len(raw)] = raw
        z.bank7[stat_at:stat_at + 16] = b"\x00" * 16
        result = self.call2(z, "_vfs_stat", path_at, stat_at)
        return result - 0x10000 if result & 0x8000 else result, \
            bytes(z.bank7[stat_at:stat_at + 16])

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
        if z.pc == self.syms["_kernel_serial_connect_delay"]:
            z.pc = z.pop16()
        z.step()

    def boot_to_selftest(self, z, limit=STEP_LIMIT):
        self.start_kernel(z)
        for _ in range(limit):
            self.step_once(z)
            if (b"m9 vfs selftest: OK\r\n" in z.sio_tx or
                    b"m9 vfs selftest: FAIL" in z.sio_tx):
                return
        raise AssertionError("VFS selftest never completed (PC=%04X)" % z.pc)

    def run_to_halt(self, z, limit=STEP_LIMIT):
        self.start_kernel(z)
        for _ in range(limit):
            self.step_once(z)
            if z.halted:
                return bytes(z.sio_tx)
        raise AssertionError("kernel never halted (PC=%04X)" % z.pc)


class TestM9VfsAcceptance(M9VfsSimBase):
    def test_boot_transcript_optional_failure_and_selftest(self):
        z = self.make_cpu()
        self.boot_to_selftest(z)
        sio = z.sio_tx.decode("ascii", "replace")

        for line in ("ZephyrOS 0.0", "platform: Zephyr-80",
                     "kernel entry: OK", "serial: OK"):
            self.assertIn(line + "\r\n", sio)
        # The optional mount that names an unregistered provider is
        # tolerated, diagnosed and leaves the namespace usable.
        self.assertIn(OPTIONAL_FAIL_LINE + "\r\n", sio)
        self.assertIn(SELFTEST_OK + "\r\n", sio)
        self.assertNotIn("FAIL", sio)
        self.assertNotIn("PANIC", sio)
        self.assertEqual(z.bank7[self.syms["_m9_vfs_selftest_result"]], 0)

    def test_no_selftest_case_fails(self):
        """Every selftest case reports success: the result byte is 0 and
        no FAIL marker appears for any of the case codes.

        Each code below is a named case in tests/target/include/
        m9_selftest.h; a regression turns its own marker on, so the
        assertion names the case instead of only reporting "FAIL"."""
        cases = {
            0x01: "boot baseline (READY phase, module, console, ROMFS)",
            0x02: "optional-mount failure state",
            0x03: 'stat("/")',
            0x04: 'stat("/SYSTEM")',
            0x06: "per-entry stat/read byte compare",
            0x07: "case-insensitive resolution",
            0x08: "path normalization",
            0x09: "ENOENT",
            0x0A: "ENOTDIR below a file",
            0x0B: "EROFS from the read-only provider",
            0x0C: "root and provider directory enumeration",
            0x0D: "executable gate",
            0x0E: "acceptance console line",
            0x0F: "flat mount selection (/SYSTEMX)",
            0x10: "deeper-path ENOTDIR/ENOENT rule",
            0x11: "null mutation operations",
            0x12: "open flag validation",
        }
        z = self.make_cpu()
        self.boot_to_selftest(z)
        sio = z.sio_tx.decode("ascii", "replace")
        for code, name in cases.items():
            self.assertNotIn("%s0x%02X" % (FAIL_LINE, code), sio,
                             "selftest case failed: %s" % name)
        self.assertEqual(z.bank7[self.syms["_m9_vfs_selftest_result"]], 0)

    def test_flat_mount_selection_in_the_linked_kernel(self):
        """`/SYSTEMX` must not select `/SYSTEM` (flat first-component
        selection, architecture section 32.8).

        This drives the linked kernel bytes directly: the target selftest
        already covers the same rule through its FLAT case."""
        z = self.make_cpu()
        self.boot_to_selftest(z)

        result, stat = self.vfs_stat(z, "/SYSTEM")
        self.assertEqual(result, 0, "/SYSTEM must stat")
        self.assertEqual(stat[0], 2, "/SYSTEM is a directory (ZFS_TYPE_DIR)")

        # A longer first component that shares the prefix must not match.
        result, _ = self.vfs_stat(z, "/SYSTEMX")
        self.assertNotEqual(result, 0,
                            "/SYSTEMX must not resolve through /SYSTEM")
        self.assertEqual(result, -ZOS_ENODEV,
                         "/SYSTEMX is unowned: no / filesystem, no such "
                         "mountpoint")

        # A mountpoint plus a component reaches the provider.
        result, stat = self.vfs_stat(z, "/SYSTEM/sio.zmod")
        self.assertEqual(result, 0, "/SYSTEM/sio.zmod must stat")
        self.assertEqual(stat[0], 1, "a ROMFS entry is a file")

        # Deeper paths: ENOTDIR below a file, ENOENT for an unknown first
        # component.
        self.assertEqual(self.vfs_stat(z, "/SYSTEM/sio.zmod/x")[0],
                         -(0x11), "a path below a file is ENOTDIR")
        self.assertEqual(self.vfs_stat(z, "/SYSTEM/nope/x")[0],
                         -ZOS_ENOENT,
                         "an unknown first component is ENOENT")

        # Case-folded resolution of both components.
        self.assertEqual(self.vfs_stat(z, "/system/SIO.ZMOD")[0], 0)

    def test_system_contents_match_the_packed_artifacts(self):
        """The /SYSTEM entries VFS serves are byte-identical to the packed
        production artifacts the image was built from."""
        modules = os.path.join(ROOT, "build", "modules")
        expected = {
            "sio.zmod": "sio.zmod",
            "ioc.zmod": "ioc.zmod",
            "hid.zmod": "hid.zmod",
            "v9958.zmod": "v9958.zmod",
            "v9958-6x8.font": "v9958-6x8.font",
            "monitor.zex": "monitor.zex",
        }
        self.assertTrue(self.entries, "recovery image has no entries")
        for entry in self.entries:
            self.assertIn(entry["name"], expected)
            packed = os.path.join(modules, expected[entry["name"]])
            if not os.path.exists(packed):
                self.skipTest("%s missing; build the artifacts first" % packed)
            with open(packed, "rb") as handle:
                self.assertEqual(entry["payload"], handle.read(),
                                 "packed bytes differ for %s" % entry["name"])


class TestM9RequiredMount(M9VfsSimBase):
    """A required non-system mount whose provider is absent is a boot
    configuration failure (architecture section 32.32.4): the mount pass
    diagnoses it on the early console and panics before the monitor
    launcher can run."""

    IMAGE_DIR = REQUIRED_IMAGE_SUBDIR
    IMAGE_NAME = REQUIRED_IMAGE_NAME

    def test_required_failure_panics_before_the_monitor(self):
        z = self.make_cpu()
        tx = self.run_to_halt(z)
        sio = tx.decode("ascii", "replace")

        for line in ("ZephyrOS 0.0", "platform: Zephyr-80",
                     "kernel entry: OK", "serial: OK"):
            self.assertIn(line + "\r\n", sio)
        # The system mount succeeded first, so the failure is the
        # configured required mount and not a corrupt recovery image.
        self.assertNotIn("romfs: corrupt recovery image", sio)
        self.assertIn(REQUIRED_FAIL_LINE + "\r\n", sio)
        self.assertIn(REQUIRED_PANIC + "\r\n", sio)
        # A required failure takes the fatal path, not the tolerant
        # `unavailable` diagnostic.
        self.assertNotIn(OPTIONAL_FAIL_LINE + "\r\n", sio)
        # The fatal mount pass stops the boot before the monitor.
        self.assertNotIn("console: dynamic driver failed", sio)
        self.assertNotIn("monitor", sio)

    def test_mount_state_reports_system_mounted_required_failed(self):
        z = self.make_cpu()
        self.run_to_halt(z)
        base = self.syms["_vfs_mount_state"]
        # The entry stride is pinned by a _Static_assert in
        # kernel/vfs/vfs.h; read it from there so it cannot drift.
        with open(os.path.join(ROOT, "kernel", "vfs", "vfs.h")) as fh:
            text = fh.read()
        match = re.search(r"sizeof\(struct vfs_mount_runtime\)\s*==\s*(\d+)",
                          text)
        self.assertIsNotNone(match,
                             "vfs_mount_runtime size is not pinned")
        runtime_size = int(match.group(1))
        self.assertEqual(z.bank7[base], 1,
                         "the system entry must be MOUNTED")
        failed = base + runtime_size
        self.assertEqual(z.bank7[failed], 2,
                         "the required entry must be FAILED")
        self.assertEqual(z.rd16(failed + runtime_size - 2), 0xFFFD,
                         "the required entry must record -ZOS_ENODEV")


if __name__ == "__main__":
    unittest.main()
