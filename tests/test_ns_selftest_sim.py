#!/usr/bin/env python3
"""Namespace acceptance replay.

Boots build-ns-selftest/konaos-ns-selftest.bin -- the production boot
path with the namespace objects linked in at the placement production will
use, the DEVICE publication table built by the host from the three driver
containers, and the SDK client booting as /system/ns.exe -- and checks the
recorded transcript:

* the standard handles exist before any bind and the console driver is
  reachable through the handle `/dev/console` publishes;
* `bind` on the host-built table reports the protocol type the manifest
  declares (SERIAL, INPUT, VIDEO, CONSOLE), a grouping name refuses with
  KOS_EISDIR and a call on the bound handle reaches the publishing
  endpoint (the synthetic publisher's deterministic answers);
* the `/system` FILE provider reads, seeks and reports metadata, refuses a
  write with KOS_EROFS, and a root-level name has no provider owner;
* the handle table fills to VFS_HANDLE_MAX, reports -KOS_EMFILE and
* releases
  every entry the client unbinds;
* `ns_enum` pages `/`, `/dev` and `/system` with its cookie, and the
  `/system` names and sizes match the packed image;
* `chdir`/`getcwd` traverse the namespace through provider validation (the
  VFS root, `/system`, a grouping container and `..`) and refuse a
  bindable object, an absent name and a provider the image does not carry,
  with the CWD untouched by a refusal;
* the child the client execs gets its own two standard handles, inherits
  the parent's CWD and cannot move the parent's CWD with its own `chdir`,
  and none of the parent's bound handles;
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

from replay import STEP_LIMIT, Replay  # noqa: E402
import romfspack  # noqa: E402

IMAGE_SUBDIR = "build-ns-selftest"
IMAGE_NAME = "konaos-ns-selftest.bin"

SELFTEST_OK = "ns selftest: OK\r\n"
SELFTEST_FAIL = "ns selftest: FAIL\r\n"
PANIC = "PANIC"

# The transcript the client and the loader produce, in order.  The client
# numbers are the handles `bind` returns: the standard handles are 1 and 2,
# and the DEVICE table's bounds hand out 3 and up in call order.
TRANSCRIPT = [
    "KonaOS 0.0\r\n",
    "platform: Zephyr-80\r\n",
    "kernel entry: OK\r\n",
    "serial: OK\r\n",
    "console: driver OK\r\n",
    "ns: probe\r\n",
    "ns: /dev/console -> 6 handle 3\r\n",
    "ns: /dev/input/keyboard -> 4\r\n",
    "ns: /dev/video -> 2\r\n",
    "ns: call /dev/video whoami -> 4E53\r\n",
    "ns: call ping -> 4E53 1234\r\n",
    "ns: /dev/serial/0 -> 1\r\n",
    "ns: handle table 10 bound, refuse -23\r\n",
    "ns: file read 24, first sio.mod\r\n",
    "ns: file size 72\r\n",
    "ns: /dev entry serial type 0 container\r\n",
    "ns: /dev entry console type 6\r\n",
    "ns: /dev entry input type 0 container\r\n",
    "ns: /dev entry video type 2\r\n",
    "ns: /system entry sio.mod\r\n",
    "ns: /system entry console-serial.mod\r\n",
    "ns: /system entry nsdev.mod\r\n",
    "ns: /system entry drivers.man\r\n",
    "ns: /system entry ns.exe\r\n",
    "ns: /system entry nschild.exe\r\n",
    "ns: /system 7 entries\r\n",
    "ns: cwd /system\r\n",
    "ns: cwd /dev\r\n",
    "nschild: stdout ok\r\n",
    "nschild: cwd /dev\r\n",
    "nschild: cwd /system\r\n",
    "nschild: /dev/console -> 6 handle 3\r\n",
    "nschild: unbind 3 -> 0\r\n",
    "nschild: OK\r\n",
    "ns: exec /system/nschild.exe -> 0\r\n",
]

# The namespace owns `/` from the first instruction, so no boot line
# reports a mount outcome.
MOUNT_LINE = "vfs: "

# /system/drivers.man: one 24-byte record per loaded image, load order.
MAN_RECORD = 24
MAN_SIZE = MAN_RECORD * 3


# The launcher's line when the client - the image's boot program - exits
# with its verdict: the monitor follows it (the acceptance policy).
EXIT_LINE = b"boot: /system/ns.exe exit 0\r\n"


class NsSelftestReplay(Replay, unittest.TestCase):
    IMAGE_SUBDIR = IMAGE_SUBDIR
    IMAGE_NAME = IMAGE_NAME

    def setUp(self):
        self.load_image()

    def boot(self, limit=STEP_LIMIT):
        """Boot until the launcher reports the client's exit status (the
        verdict line arrives before it)."""
        z = self.make_cpu()
        self.start_kernel(z)
        for _ in range(limit):
            self.skip_delay(z)
            self.feed(z)
            z.step()
            if z.halted:
                return z
            tail = bytes(z.sio_tx).split(EXIT_LINE, 1)
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
        # The namespace checks plus the CWD/inheritance outcomes.
        self.assertGreaterEqual(total, 90)
        self.assertIn(EXIT_LINE.decode("ascii"), sio)

    def test_boot_manifest_matches_the_read(self):
        """The client read /system/drivers.man; compare what it saw with
        the packed image: the name, the size and the first record."""
        romfs = os.path.join(ROOT, IMAGE_SUBDIR, "recovery.romfs")
        if not os.path.exists(romfs):
            self.skipTest("%s missing; build the image first" % romfs)
        with open(romfs, "rb") as handle:
            _header, entries = romfspack.parse_image(handle.read())
        manifest = next(e for e in entries if e["name"] == "drivers.man")
        self.assertEqual(len(manifest["payload"]), MAN_SIZE)
        first = manifest["payload"][:MAN_RECORD].split(b"\x00")[0]
        self.assertEqual(first, b"sio.mod")

        sio = self.boot().sio_tx.decode("ascii", "replace")
        self.assertIn("ns: file read %d, first sio.mod\r\n" % MAN_RECORD, sio)
        self.assertIn("ns: file size %d\r\n" % MAN_SIZE, sio)

    def test_the_image_carries_the_namespace_routed_family(self):
        self.assertIn("_vfs_ns_bind", self.syms)
        self.assertIn("_vfs_ns_file_bind", self.syms)
        self.assertIn("_kos_object_dispatch", self.syms)
        self.assertIn("_ipc_syscall_call_object", self.syms)
        # Cold half, the placement production will use.
        self.assertGreaterEqual(self.syms["_vfs_ns_bind"], 0x6000)
        self.assertLess(self.syms["_vfs_ns_bind"], 0xE000)


if __name__ == "__main__":
    unittest.main()
