#!/usr/bin/env python3
"""Namespace acceptance replay (work package 42a, correction 01).

Boots build-ns-selftest/zephyros-ns-selftest.bin -- the production boot
path with the namespace objects linked in at the placement production will
use, the DEVICE publication table built by the host from the three service
containers, and the SDK client booting as /SYSTEM/ns.zex -- and checks the
recorded transcript:

* the standard handles exist before any bind and the console service is
  reachable through the handle `/dev/console` publishes;
* `bind` on the host-built table reports the protocol type the manifest
  declares (SERIAL, INPUT, VIDEO, CONSOLE), a grouping name refuses with
  ZOS_EISDIR and a call on the bound handle reaches the publishing
  endpoint (the synthetic publisher's deterministic answers);
* the `/SYSTEM` FILE provider reads, seeks and reports metadata, refuses a
  write with ZOS_EROFS, and a root-level name has no provider owner;
* the handle table fills to VFS_FD_MAX, reports -ZOS_EMFILE and releases
  every entry the client unbinds;
* `ns_enum` pages `/`, `/dev` and `/SYSTEM` with its cookie, and the
  `/SYSTEM` names and sizes match the packed image;
* the child the client execs gets its own two standard handles and none of
  the parent's bound handles;
* the client's own verdict line reports every check, and the monitor
  reports a zero exit status.

Skipped when the image has not been built.
"""

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import Z80, read_symbols, skip_serial_connect_delay  # noqa: E402
import romfspack  # noqa: E402

RET = 0x1234
STACK = 0xFFFE
STEP_LIMIT = 60000000

IMAGE_SUBDIR = "build-ns-selftest"
IMAGE_NAME = "zephyros-ns-selftest.bin"

SELFTEST_OK = "ns selftest: OK\r\n"
SELFTEST_FAIL = "ns selftest: FAIL\r\n"
PANIC = "PANIC"

# The transcript the client and the loader produce, in order.  The client
# numbers are the handles `bind` returns: the standard handles are 1 and 2,
# and the DEVICE table's bounds hand out 3 and up in call order.
TRANSCRIPT = [
    "ZephyrOS 0.0\r\n",
    "platform: Zephyr-80\r\n",
    "kernel entry: OK\r\n",
    "serial: OK\r\n",
    "console: service OK\r\n",
    "ns: probe\r\n",
    "ns: /dev/console -> 6 handle 3\r\n",
    "ns: /dev/input/keyboard -> 4\r\n",
    "ns: /dev/video -> 2\r\n",
    "ns: call /dev/video whoami -> 4E53\r\n",
    "ns: call ping -> 4E53 1234\r\n",
    "ns: /dev/serial/0 -> 1\r\n",
    "ns: handle table 10 bound, refuse -23\r\n",
    "ns: file read 24, first sio.zmod\r\n",
    "ns: file size 72\r\n",
    "ns: /dev entry serial type 0 container\r\n",
    "ns: /dev entry console type 6\r\n",
    "ns: /dev entry input type 0 container\r\n",
    "ns: /dev entry video type 2\r\n",
    "ns: /SYSTEM entry sio.zmod\r\n",
    "ns: /SYSTEM entry console.zmod\r\n",
    "ns: /SYSTEM entry nsdev.zmod\r\n",
    "ns: /SYSTEM entry services.man\r\n",
    "ns: /SYSTEM entry ns.zex\r\n",
    "ns: /SYSTEM entry nschild.zex\r\n",
    "ns: /SYSTEM 6 entries\r\n",
    "nschild: stdout ok\r\n",
    "nschild: /dev/console -> 6 handle 3\r\n",
    "nschild: unbind 3 -> 0\r\n",
    "nschild: OK\r\n",
    "ns: exec /SYSTEM/nschild.zex -> 0\r\n",
]

# The M9 mount pass is gone with the M9 VFS it belonged to (package 42b):
# the namespace owns `/` from the first instruction, so no boot line
# reports a mount outcome any more.  The image must not grow one back.
MOUNT_LINE = "vfs: "

# /SYSTEM/services.man: one 24-byte record per loaded image, load order.
MAN_RECORD = 24
MAN_SIZE = MAN_RECORD * 3


def symbols(map_path):
    found = {}
    with open(map_path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = re.match(r"\s*([0-9A-Fa-f]{4,8})\s+(\S+)(?:\s+\S+)?\s*$",
                             line)
            if match:
                found[match.group(2)] = int(match.group(1), 16)
    return found


class NsSelftestReplay(unittest.TestCase):
    def setUp(self):
        self.bin_path = os.path.join(ROOT, IMAGE_SUBDIR, IMAGE_NAME)
        if not os.path.exists(self.bin_path):
            self.skipTest("%s missing; build the image first" % self.bin_path)
        with open(self.bin_path, "rb") as handle:
            self.image = handle.read()
        self.syms = symbols(os.path.join(ROOT, IMAGE_SUBDIR,
                                        "zephyros.map"))

    def boot(self, limit=STEP_LIMIT):
        pages = {n: self.image[n * 0x10000:(n + 1) * 0x10000]
                 for n in range(8)}
        z = Z80(pages[0], pages[1],
                rom_pages={n: pages[n] for n in range(2, 8)})
        z.latch = 0x18
        z.sp = STACK
        z.push16(RET)
        z.pc = self.syms["__zos_crt0_init"]
        for _ in range(2000000):
            z.step()
            if z.pc == RET:
                break
        z.push16(RET)
        z.pc = self.syms["_kernel_main"]
        delay = self.syms["_kernel_serial_connect_delay"]
        for _ in range(limit):
            skip_serial_connect_delay(z, delay)
            z.step()
            if z.halted:
                return z
            # The monitor reports the client's exit status after the verdict.
            tail = z.sio_tx.split(b"monitor: exit status", 1)
            if len(tail) == 2 and b"\r\n" in tail[1]:
                break
        return z

    def test_boot_transcript_and_verdict(self):
        z = self.boot()
        sio = z.sio_tx.decode("ascii", "replace")

        position = 0
        for line in TRANSCRIPT:
            found = sio.find(line, position)
            self.assertGreaterEqual(found, 0,
                                    "missing or out of order: %r in\n%s"
                                    % (line, sio))
            position = found + len(line)

        self.assertNotIn(MOUNT_LINE, sio)
        self.assertIn(SELFTEST_OK, sio)
        self.assertNotIn(SELFTEST_FAIL, sio)
        self.assertNotIn("FAIL", sio)
        self.assertNotIn(PANIC, sio)
        # Every check the client planned passed, and the monitor reports
        # the client's own exit status.
        match = re.search(r"ns: (\d+)/(\d+) checks\r\n", sio)
        self.assertIsNotNone(match, sio)
        passed, total = int(match.group(1)), int(match.group(2))
        self.assertEqual(passed, total)
        self.assertGreaterEqual(total, 60)
        self.assertIn("monitor: exit status 0x0000", sio)

    def test_boot_manifest_matches_the_read(self):
        """The client read /SYSTEM/services.man; compare what it saw with
        the packed image: the name, the size and the first record."""
        romfs = os.path.join(ROOT, IMAGE_SUBDIR, "recovery.romfs")
        if not os.path.exists(romfs):
            self.skipTest("%s missing; build the image first" % romfs)
        with open(romfs, "rb") as handle:
            _header, entries = romfspack.parse_image(handle.read())
        manifest = next(e for e in entries if e["name"] == "services.man")
        self.assertEqual(len(manifest["payload"]), MAN_SIZE)
        first = manifest["payload"][:MAN_RECORD].split(b"\x00")[0]
        self.assertEqual(first, b"sio.zmod")

        sio = self.boot().sio_tx.decode("ascii", "replace")
        self.assertIn("ns: file read %d, first sio.zmod\r\n" % MAN_RECORD, sio)
        self.assertIn("ns: file size %d\r\n" % MAN_SIZE, sio)

    def test_the_image_carries_the_namespace_routed_family(self):
        self.assertIn("_vfs_ns_bind", self.syms)
        self.assertIn("_vfs_ns_file_bind", self.syms)
        self.assertIn("_zos_object_dispatch", self.syms)
        self.assertIn("_ipc_syscall_call_object", self.syms)
        # Cold half, the placement production will use.
        self.assertGreaterEqual(self.syms["_vfs_ns_bind"], 0x6000)
        self.assertLess(self.syms["_vfs_ns_bind"], 0xE000)


if __name__ == "__main__":
    unittest.main()
