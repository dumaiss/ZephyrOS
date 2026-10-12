#!/usr/bin/env python3
"""Filesystem outcome acceptance replay (work package 42b, correction 01).

Boots `build-fs-selftest/zephyros-fs-selftest.bin` in the Z80 simulator with
the host FS2 peer serving the card, and reads the transcript of the one SDK
outcome program the image boots (`/SYSTEM/fs_selftest.zex`):

- with a writable card, every outcome line reports OK, the verdict carries
  the pinned check count, the nested-exec child reports its own stdout and
  the image hands the console to the monitor; the child also runs once
  copied onto the card itself, so an exec whose path only the bank-6
  filesystem provider can read stays covered (correction 01);
- with no card, the program takes its card-absent branch, `/SYSTEM` stays
  usable and the verdict carries the reduced count;
- the handles the child leaked are gone once it exits: the driver's card
  slots and the kernel's descriptor table are read straight out of the
  machine at the moment the parent proves the slots are reusable.

Production is not touched by this image: it only adds a boot configuration,
the store manifest and the two programs.

Run with: `python3 -m unittest tests.test_fs_selftest_sim`.
"""

import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import zmodpack  # noqa: E402
from replay import STEP_LIMIT, Replay  # noqa: E402
from z80sim import IocCommandPeer

IMAGE_SUBDIR = "build-fs-selftest"
IMAGE_NAME = "zephyros-fs-selftest.bin"
ZMOD = os.path.join("build-fs-selftest", "modules", "iocfat.zmod")

PASS = "fs-selftest: PASS\r\n"
FAIL = "fs-selftest: FAIL"
PROMPT = "Z80> "
ABSENT = "fs-selftest: card absent: /FS not served\r\n"
READONLY = "fs-selftest: card not writable\r\n"
CHILD_PASS = "fs-child: PASS\r\n"
CHILD_ABSENT = "fs-child: card absent\r\n"
EXEC_STATUS = "fs-selftest: exec-status OK\r\n"

# The outcome suite's size: every `check()` the program reaches.
CARD_CHECKS = 37
ABSENT_CHECKS = 8

# kernel/vfs/vfs.h: 12 descriptors, `owner` is 0xFF when the slot is free,
# and the kernel pins sizeof(struct vfs_descriptor) at 21 bytes.
VFS_FD_MAX = 12
VFS_DESC_FREE = 0xFF
VFS_DESC_STRIDE = 21

# One outcome line per completed check on a writable card.
CARD_OUTCOMES = (
    "startup-version", "startup-tail", "console-write",
    "fs-open-create", "fs-write-blocks", "fs-close-write",
    "fs-stat-type", "fs-stat-size",
    "fs-open-read", "fs-read-head", "fs-seek-set",
    "fs-read-cross-block", "fs-seek-end", "fs-read-eof", "fs-seek-cur",
    "fs-read-tail", "fs-close-read",
    "enum-root-system", "enum-root-fs", "enum-card-file", "enum-card-subdir",
    "fs-mkdir", "fs-mkdir-stat", "fs-rename", "fs-rename-stat",
    "fs-rename-old-gone", "fs-unlink", "fs-rmdir", "fs-unlink-gone",
    "system-stat", "system-open", "system-read", "system-close",
    "exec-status", "exec-card-copy", "exec-card-status",
    "exec-child-handles-released",
)

# The outcomes a run without a card still reaches.
ABSENT_OUTCOMES = (
    "startup-version", "startup-tail", "console-write",
    "system-stat", "system-open", "system-read", "system-close",
    "exec-status",
)


class FsSelftestSimBase(Replay, unittest.TestCase):
    IMAGE_SUBDIR = IMAGE_SUBDIR
    IMAGE_NAME = IMAGE_NAME

    def setUp(self):
        self.load_image()

    def make_cpu(self, **peer_kwargs):
        z = super().make_cpu()
        peer = IocCommandPeer(**peer_kwargs)
        z.hid = peer
        z.ioc_peer = peer
        z.peer = peer
        z.vdp = z.vdp_peer
        z.glyphs = []
        z.glyph_pos = 0
        z.glyph_last = None
        return z

    def run_kernel(self, z, stop, limit=STEP_LIMIT):
        """Boot until one of `stop` appears in the serial transcript."""
        self.start_kernel(z)
        self.run_until(z, stop, limit=limit, halt_ok=True, panic=None)
        return bytes(z.sio_tx)

    def boot(self, **peer_kwargs):
        """Boot until the monitor prompt (or a failure marker): the
        program's verdict line arrives before the monitor runs."""
        z = self.make_cpu(**peer_kwargs)
        self.run_kernel(z, [PROMPT, FAIL, "PANIC"])
        return z, z.sio_tx.decode("ascii", "replace")

    def driver_base(self):
        """Base of the driver's data areas: its hot segment's image end."""
        with open(os.path.join(ROOT, ZMOD), "rb") as handle:
            info = zmodpack.parse_container(handle.read(), "iocfat.zmod")
        hot = [s for s in info["segments"] if s["role"] == "hot"][0]
        return hot["base"] + hot["image_size"]


class TestFsSelftestWritableCard(FsSelftestSimBase):
    def test_boot_and_outcomes(self):
        z, sio = self.boot()

        for line in ("ZephyrOS 0.0", "platform: Zephyr-80",
                     "kernel entry: OK", "serial: OK",
                     "console: service OK"):
            self.assertIn(line + "\r\n", sio)
        self.assertNotIn("PANIC", sio)
        self.assertNotIn(READONLY, sio)
        self.assertNotIn(ABSENT, sio)

        for label in CARD_OUTCOMES:
            self.assertIn("fs-selftest: %s OK\r\n" % label, sio)
        self.assertNotIn(FAIL, sio)
        self.assertIn("fs-selftest: %u checks, 0 failures\r\n" % CARD_CHECKS,
                      sio)
        self.assertIn(PASS, sio)

        # The nested-exec child reported through its own standard handle and
        # handed its exit status back.
        self.assertIn(CHILD_PASS, sio)
        self.assertIn('fs-child: tail "tail-42" argc 1\r\n', sio)
        self.assertIn("fs-child: leaked 3 handles\r\n", sio)
        self.assertNotIn("fs-child: FAIL", sio)

        # The image then handed the console to the monitor.
        self.assertIn("ZephyrOS native monitor\r\n", sio)
        self.assertIn(PROMPT, sio)

    def test_the_child_leaks_nothing_behind_it(self):
        z = self.make_cpu()
        # Stop before the parent's own probe open: at this instant every
        # handle the child leaked is the only thing that could still be
        # live anywhere.
        self.run_kernel(z, [EXEC_STATUS, FAIL, "PANIC"])
        sio = z.sio_tx.decode("ascii", "replace")
        self.assertIn(EXEC_STATUS, sio)
        self.assertNotIn(FAIL, sio)

        # The driver's pools: two file slots and one directory slot, all
        # free once the child's leaked handles are gone.
        base = self.driver_base()
        bank = z.banks[6]
        for slot in range(2):
            self.assertEqual(bank[base + 16 * slot], 0,
                             "driver file slot %d is still in use" % slot)
        self.assertEqual(bank[base + 32], 0,
                         "the driver's directory slot is still in use")

        # The kernel's descriptor table: only the two standard handles the
        # running parent owns may be live.
        table = self.syms["_vfs_descriptors"]
        live = [index for index in range(VFS_FD_MAX)
                if z.bank7[table + VFS_DESC_STRIDE * index] != VFS_DESC_FREE]
        self.assertEqual(len(live), 2,
                         "descriptor table holds %r live entries" % live)


class TestFsSelftestAbsentCard(FsSelftestSimBase):
    def test_card_absent_keeps_system_readable(self):
        z, sio = self.boot(fs2_media=False)

        self.assertNotIn("PANIC", sio)
        self.assertIn(ABSENT, sio)
        self.assertNotIn(FAIL, sio)
        for label in ABSENT_OUTCOMES:
            self.assertIn("fs-selftest: %s OK\r\n" % label, sio)
        self.assertIn("fs-selftest: %u checks, 0 failures\r\n"
                      % ABSENT_CHECKS, sio)
        self.assertIn(PASS, sio)

        # The child still ran, still reported through its own stdout and
        # still handed back its exit status; only the card file was
        # refused (the `/FS` namespace bind needs no card).
        self.assertIn(CHILD_PASS, sio)
        self.assertIn(CHILD_ABSENT, sio)
        self.assertIn("fs-child: leaked 2 handles\r\n", sio)
        self.assertNotIn("fs-child: FAIL", sio)

        self.assertIn(PROMPT, sio)


if __name__ == "__main__":
    unittest.main()
