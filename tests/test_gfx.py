#!/usr/bin/env python3
"""The graphics foundation: KVIDEO_OP_DIRECT_INFO, libgfx and gfxtest.

Two families:

* host: the ABI's op number and the kvideo.h record (layout, constants,
  static asserts), the libgfx source discipline (the backend's port I/O
  lives in its assembly file only) and the Makefile wiring;
* replay: the production image, the V9958 peer and the outcome program
  `tests/gfx/gfxtest.c` on the card, run from the shell.  DIRECT_INFO
  is refused without the exclusive hold and answered with it; each mode
  leaves the peer's registers, the clear's HMMV and its VRAM result as
  the backend promises; no VIDEO dispatch happens between gfx_open and
  gfx_close (the endpoint's dispatch entry is watched and the frame op
  read from HL); and the console is restored after the key - also when
  the program exits without gfx_close (the kernel's handle release).

The V9958 peer is the observable (`regs`, `vram`, `commands`,
`vram_writes`, `protocol_errors`); the driver's exclusive flag is read
from its bank-6 state, as tests/test_device_release.py does.
"""

import json
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from test_hwtest_sim import KEEP_CONTENT, KEEP_NAME, HwtestReplay  # noqa: E402
from test_ioc import struct_member_order  # noqa: E402
from replay import SHELL_PROMPT  # noqa: E402
from z80sim import IocCommandPeer  # noqa: E402

GFXTEST_EXE = os.path.join(ROOT, "build", "user", "gfxtest.exe")
DEV_NAME = "GFXTEST.EXE"
MODULE_DIR = os.path.join(ROOT, "build", "modules")
PROTOCOL_H = os.path.join(ROOT, "abi", "kos", "protocol.h")
KVIDEO_H = os.path.join(ROOT, "abi", "kos", "kvideo.h")
MAKEFILE = os.path.join(ROOT, "Makefile")
GFX_SRC = os.path.join(ROOT, "user", "sdk", "gfx", "src")

# The record the driver answers.
FLAGS = 0x000F
VRAM_KIB = 128
PORTS = (0xA0, 0xA1, 0xA2, 0xA3, 0xA4)

# The modes' pinned registers after gfx_set_mode/gfx_display_enable(1).
MODE_REGS = {
    "mode6": {"r0": 0x0A, "r1": 0x40, "r8": 0x0A, "r9": 0x80,
              "r25": 0x04},
    "mode7": {"r0": 0x0E, "r1": 0x40, "r8": 0x0A, "r9": 0x80,
              "r25": 0x04},
    # R#25 is 0 CMD VDS YAE YJK WTE MSK SP2: WTE (04h) plus YJK (08h).
    "yjk": {"r0": 0x0E, "r1": 0x40, "r8": 0x0A, "r9": 0x80, "r25": 0x0C},
}

# The clear colours gfxtest uses, and the VRAM byte each mode's clear
# writes (two 4-bit pixels per byte in GRAPHIC 6).
CLEAR_COLOR = {"mode6": 0x04, "mode7": 0x1C, "yjk": 0x1C}
PACKED = {"mode6": 0x44, "mode7": 0x1C, "yjk": 0x1C}
WIDTH = {"mode6": 512, "mode7": 256, "yjk": 256}
# gfx_clear's fill: one HMMV over both pages' visible 212 lines.
CLEAR_LINES = 212 + 256

# The ops KVIDEO_OP_WRITE (1), KOS_OP_ACQUIRE (0xFE), DIRECT_INFO (6)
# and KOS_OP_RELEASE (0xFF) that a gfxtest run dispatches to the video
# endpoint.  The contract's "no VIDEO dispatch between open and close"
# is the exact consecutive subsequence ACQUIRE, DIRECT_INFO, RELEASE.
OPS_WRITE = 1
OPS_ACQUIRE = 0xFE
OPS_DIRECT_INFO = 6
OPS_RELEASE = 0xFF
OPS_RUN = (OPS_ACQUIRE, OPS_DIRECT_INFO, OPS_RELEASE)

# Registers the console's bring-up owns; the release re-runs the whole
# display init, so each must be back after the run.
OWNED_REGS = (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 23, 25, 26, 27, 46)

EXEC_BUDGET = 9000000


def read_text(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def header_value(path, name):
    for line in read_text(path).splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0] == "#define" and parts[1] == name:
            return int(parts[2].rstrip("uU"), 0)
    raise AssertionError("%s defines no %s" % (path, name))


class GfxHostTests(unittest.TestCase):
    """Constants, headers and source discipline, no emulation."""

    def test_abi_op_number(self):
        self.assertEqual(header_value(PROTOCOL_H, "KVIDEO_OP_DIRECT_INFO"),
                         6)
        self.assertEqual(header_value(PROTOCOL_H, "KVIDEO_OP_INFO"), 5)
        self.assertEqual(header_value(PROTOCOL_H, "KVIDEO_OP_RENDER"), 4)

    def test_kvideo_record(self):
        text = read_text(KVIDEO_H)
        self.assertEqual(header_value(KVIDEO_H, "KVIDEO_BACKEND_V9958"), 1)
        self.assertEqual(header_value(KVIDEO_H,
                                      "KVIDEO_DIRECT_INFO_VERSION"), 1)
        self.assertEqual(header_value(KVIDEO_H, "KVIDEO_DIRECT_IO"), 0x0001)
        self.assertEqual(header_value(KVIDEO_H, "KVIDEO_HW_WAIT"), 0x0002)
        self.assertEqual(header_value(KVIDEO_H, "KVIDEO_PALETTE"), 0x0004)
        self.assertEqual(header_value(KVIDEO_H, "KVIDEO_YJK"), 0x0008)
        self.assertEqual(struct_member_order(text, "kvideo_direct_info"),
                         ["version", "backend", "flags", "vram_kib",
                          "data_port", "command_port", "palette_port",
                          "indirect_port", "config_port", "config_initial",
                          "reserved"])
        self.assertIn("sizeof(struct kvideo_direct_info) == 19", text)
        self.assertIn("data_port) == 6", text)
        self.assertIn("config_initial) == 11", text)

    def test_libgfx_sources_and_port_discipline(self):
        """The backend's every port access lives in the assembly file:
        the C sources name no __sfr, __at, outp or inp."""
        gfx_h = os.path.join(ROOT, "user", "sdk", "include", "kos",
                             "gfx.h")
        for path in (os.path.join(GFX_SRC, "gfx.c"),
                     os.path.join(GFX_SRC, "gfx_mode.c"),
                     os.path.join(GFX_SRC, "gfx_draw.c"),
                     os.path.join(GFX_SRC, "backends", "v9958",
                                  "v9958_gfx.c"),
                     gfx_h):
            self.assertTrue(os.path.isfile(path), path)
        for name in ("gfx.c", "gfx_mode.c", "gfx_draw.c",
                     "backends/v9958/v9958_gfx.c"):
            source = read_text(os.path.join(GFX_SRC, name))
            for token in ("__sfr", "__at", "outp(", "inp("):
                self.assertNotIn(token, source,
                                 "%s reaches a port in C" % name)
        asm = read_text(os.path.join(GFX_SRC, "backends", "v9958",
                                     "v9958_io.asm"))
        self.assertIn("out\t(c),a", asm)
        self.assertIn("in\ta,(c)", asm)

    def test_makefile_wires_gfxtest_with_libgfx(self):
        makefile = read_text(MAKEFILE)
        lines = [line for line in makefile.splitlines()
                 if "exe_program,gfxtest" in line or
                 "gfxtest.c" in line]
        self.assertTrue(lines, "the Makefile declares no gfxtest")
        self.assertTrue(any("LIBS=gfx" in line for line in lines),
                        "gfxtest does not link LIBS=gfx")


class GfxReplay(HwtestReplay):
    """The production image, the card and the V9958 peer."""

    def setUp(self):
        self.load_image()
        if not os.path.exists(GFXTEST_EXE):
            self.skipTest("%s missing; run make" % GFXTEST_EXE)
        with open(os.path.join(MODULE_DIR, "v9958.report.json"),
                  encoding="utf-8") as handle:
            self.dispatch = json.load(handle)["endpoints"][0]["dispatch"]

    def start(self):
        with open(GFXTEST_EXE, "rb") as handle:
            files = {"/".join(KEEP_NAME): KEEP_CONTENT,
                     DEV_NAME: handle.read()}
        z = self.make_cpu()
        z.hid = z.ioc_peer = IocCommandPeer(fs2_media=True,
                                            fs2_files=files)
        self.boot_to_shell(z, prompt=SHELL_PROMPT)
        return z

    # -- dispatch observation ------------------------------------------
    def run_recording(self, z, ops, until, limit=EXEC_BUDGET):
        """Step with the video dispatch entry watched (HL = the frame,
        the sdcccall(0) argument), recording the op of every hit, until
        `until(z)` is true."""
        for step in range(limit):
            if z.pc == self.dispatch:
                ops.append(z.rd(z.hl))
            self.skip_delay(z)
            self.feed(z)
            z.step()
            if step % 0x1FF == 0:
                self.pump(z)
            if until(z):
                return
        self.pump(z)
        self.fail("no outcome in %u steps (PC=%04X)\nconsole: %r"
                  % (limit, z.pc, "".join(z.glyphs)[-300:]))

    def assert_hold_sequence(self, ops):
        """ACQUIRE, DIRECT_INFO, RELEASE consecutively: the only video
        dispatch between gfx_open and gfx_close is the DIRECT_INFO."""
        run = tuple(ops)
        self.assertEqual(run.count(OPS_ACQUIRE), 1, run)
        self.assertEqual(run.count(OPS_DIRECT_INFO), 1, run)
        self.assertEqual(run.count(OPS_RELEASE), 1, run)
        at = run.index(OPS_DIRECT_INFO)
        self.assertEqual(run[at - 1:at + 2], OPS_RUN, run)
        for op in run:
            self.assertIn(op, (OPS_WRITE,) + OPS_RUN, run)

    # -- mode evidence --------------------------------------------------
    def mode_on(self, mode):
        pins = MODE_REGS[mode]

        def until(z):
            return (z.vdp.regs[9] == pins["r9"] and
                    z.vdp.regs[1] == pins["r1"])
        return until

    def assert_mode_state(self, z, mode):
        """The registers, the clear's command and its VRAM result while
        the program holds the VDP and waits for the key."""
        peer = z.vdp
        pins = MODE_REGS[mode]
        self.assertEqual(peer.regs[0], pins["r0"], "R#0")
        self.assertEqual(peer.regs[1], pins["r1"], "R#1")
        self.assertEqual(peer.regs[8], pins["r8"], "R#8")
        self.assertEqual(peer.regs[9], pins["r9"], "R#9")
        self.assertEqual(peer.regs[25], pins["r25"], "R#25")
        self.assertEqual(peer.regs[15] & 0x0F, 2, "R#15 status select")
        self.assertEqual(peer.regs[23], 0, "R#23 display offset")

        fill = peer.commands[-1]
        self.assertEqual(fill["code"], 0xC0, "the clear is one HMMV")
        self.assertEqual(fill["fill"],
                         (0, 0, WIDTH[mode], CLEAR_LINES,
                          PACKED[mode]))
        # One rectangle, DY 0, NY 212 + 256: every row the HMMV covers -
        # page 0's 256 and page 1's first 212 - holds the colour.
        row = bytes([PACKED[mode]]) * (WIDTH[mode] // 2)
        for line in range(CLEAR_LINES):
            base = line * 256
            self.assertEqual(peer.vram[base:base + WIDTH[mode] // 2],
                             row, "row %u" % line)

    # -- restore --------------------------------------------------------
    def settle(self, z, until, limit=100000):
        """Step until the peer reaches a quiescent state.  The glyph
        model sees a prompt as soon as its cells are drawn, which is
        mid-render: the renderer's sprites-off bracket still has R#8
        held at 0Ah, so the restore check waits for its restore."""
        for _ in range(limit):
            if until(z):
                return
            self.skip_delay(z)
            self.feed(z)
            z.step()
        self.fail("the peer never settled (PC=%04X)" % z.pc)

    def snapshot(self, z):
        peer = z.vdp
        return {"regs": list(peer.regs), "palette":
                [list(entry) for entry in peer.palette],
                "writes": len(peer.vram_writes)}

    def assert_restored(self, z, before):
        """The release's display init rewrote every owned register,
        palette entry and the cursor tables, and the console drew
        again."""
        peer = z.vdp
        for reg in OWNED_REGS:
            self.assertEqual(peer.regs[reg], before["regs"][reg],
                             "R#%u was not restored" % reg)
        self.assertEqual(peer.palette, before["palette"],
                         "the palette was not restored")
        cursor = [address for address, _value
                  in peer.vram_writes[before["writes"]:]
                  if 0x1F800 <= address < 0x1F808]
        self.assertGreaterEqual(len(cursor), 8,
                                "the reset did not rewrite the cursor "
                                "pattern")
        self.assertEqual(peer.protocol_errors, [],
                         "the run left protocol errors behind")

    def run_mode(self, mode, leak=False):
        z = self.start()
        before = self.snapshot(z)
        ops = []
        since = len(z.glyphs)
        command = "/fs/gfxtest.exe %s%s\r" % (mode, " leak" if leak else "")
        z.hid.hid_queue.extend(command.encode("ascii"))
        self.run_recording(z, ops, self.mode_on(mode))
        self.assert_mode_state(z, mode)

        # The key: the only blocking point of the program.
        z.hid.hid_queue.extend(b"x")
        if leak:
            marker = SHELL_PROMPT
        else:
            marker = "gfxtest: done"
        self.run_recording(
            z, ops, lambda z: marker in "".join(z.glyphs[since:]))
        self.run_to(z, SHELL_PROMPT, since=since)
        # The renderer's final sprites restore (R#8 back to 08h) lands
        # after the prompt's glyphs are visible.
        self.settle(z, lambda z: z.vdp.regs[8] == 0x08)
        fresh = self.trace(z, since)
        self.assertIn("gfxtest: %s" % mode, fresh)
        if not leak:
            self.assertIn("gfxtest: done", fresh)
        self.assertNotIn("FAIL", fresh)

        self.assert_hold_sequence(ops)
        self.assert_restored(z, before)
        self.assertEqual(self.exclusive(z), 0,
                         "the exclusive flag survived the run")

    # bank-6 driver state (tests/test_device_release.py's reader).
    def exclusive(self, z):
        address = None
        with open(os.path.join(MODULE_DIR, "v9958.report.json"),
                  encoding="utf-8") as handle:
            report = json.load(handle)
        with open(os.path.join(MODULE_DIR, "drivers.plan.json"),
                  encoding="utf-8") as handle:
            plan = json.load(handle)
        entry = [item for item in plan["drivers"]
                 if item["name"] == "v9958.mod"][0]
        role = report["symbol_segments"]["_v9958_state"]
        base = {"hot": entry["hot_base"], "io": entry["io_base"] or 0,
                "boot": entry["boot_base"]}[role]
        address = base + report["symbols"]["_v9958_state"]
        return z.banks[6][address + 20]


class TestDirectInfo(GfxReplay):
    """`gfxtest info`: the refusal without the hold, the record with it."""

    def test_refused_without_the_hold_and_answered_with_it(self):
        z = self.start()
        ops = []
        since = len(z.glyphs)
        z.hid.hid_queue.extend(b"/fs/gfxtest.exe info\r")
        self.run_recording(
            z, ops, lambda z: SHELL_PROMPT in "".join(z.glyphs[since:]))
        fresh = self.trace(z, since)
        self.assertIn("gfxtest: refused -4", fresh)
        # version 1, backend V9958, flags 000F, 128 KiB, ports A0h-A4h,
        # the console's latch value 0.
        self.assertIn("gfxtest: v1 b1 fF r128 pA0 A1 A2 A3 A4 c0", fresh)
        # Plain bind -> refused DIRECT_INFO; exclusive bind -> answered.
        run = tuple(ops)
        self.assertLess(run.index(OPS_DIRECT_INFO), run.index(OPS_ACQUIRE))
        self.assertEqual(run.count(OPS_DIRECT_INFO), 2, run)
        self.assertEqual(run[run.index(OPS_ACQUIRE):
                              run.index(OPS_ACQUIRE) + 3],
                         OPS_RUN, run)


class TestGfxModes(GfxReplay):
    """`gfxtest mode6|mode7|yjk`: mode, clear, key, restored console."""

    def test_mode6(self):
        self.run_mode("mode6")

    def test_mode7(self):
        self.run_mode("mode7")

    def test_yjk(self):
        self.run_mode("yjk")


class TestGfxExitWithoutClose(GfxReplay):
    """The process exit releases the handle: the console is restored."""

    def test_mode6_leak(self):
        self.run_mode("mode6", leak=True)


if __name__ == "__main__":
    unittest.main()
