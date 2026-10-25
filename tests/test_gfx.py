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

The image package adds: the streaming upload (four rectangles in chunk
sizes 1, 7, 256 and 4096, the short/overlong/nested refusals, each
transfer's VRAM bytes checked at the release), the palette (the G6
entries while the display is held, then the G7 refusal with no palette
port writes), and PView (the g7 and g6 pictures centered on the peer's
VRAM with the g6 palette applied, the restored console after each, and
a bad header touching no VIDEO dispatch at all).

The primitives package adds: the batch (ordering, the 32-record queue
auto-flushing on the 33rd command while the hold is intact), the fill
and copy rectangles (HMMV/HMMM, the four overlap directions with their
DIX/DIY blocks pinned, copies across the pages, the G6 odd-x refusal
queuing nothing), and gfx_clear as the visible page's fill.

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
PVIEW_EXE = os.path.join(ROOT, "UserSoftware", "PView", "build", "user",
                         "pview.exe")
FIXTURES = os.path.join(ROOT, "tests", "gfx", "fixtures")
DEV_NAME = "GFXTEST.EXE"
PVIEW_DEV = "PVIEW.EXE"
MODULE_DIR = os.path.join(ROOT, "build", "modules")
PROTOCOL_H = os.path.join(ROOT, "abi", "kos", "protocol.h")
KVIDEO_H = os.path.join(ROOT, "abi", "kos", "kvideo.h")
MAKEFILE = os.path.join(ROOT, "Makefile")
GFX_SRC = os.path.join(ROOT, "user", "sdk", "gfx", "src")
EXE_MK = os.path.join(ROOT, "user", "sdk", "exe.mk")
GFX_H = os.path.join(ROOT, "user", "sdk", "include", "kos", "gfx.h")

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
# gfx_clear's fill: one HMMV over the visible page's 212 lines.
CLEAR_LINES = 212

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


def upload_byte(i):
    """gfxtest_upload_byte: the upload mode's payload byte at offset i
    (two identical 4-bit pixels per GRAPHIC 6 byte)."""
    return ((i * 7 + 5) & 0x0F) * 0x11


# gfxtest upload's four rectangles (first byte address, rows, bytes per
# row, row stride, payload length), all GRAPHIC 6 (2 pixels per byte).
UPLOAD_RECTS = (
    (0x10000, 4, 256, 256, 1024),   # chunk 1: full width, page 1
    (0x10820, 6, 64, 256, 384),     # chunk 7: x 64, y 264
    (0x12C00, 8, 128, 256, 1024),   # chunk 256: x 0, y 300
    (0x00000, 16, 256, 256, 4096),  # chunk 4096: full width, page 0
)


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
                     os.path.join(GFX_SRC, "gfx_batch.c"),
                     os.path.join(GFX_SRC, "gfx_upload.c"),
                     os.path.join(GFX_SRC, "gfx_palette.c"),
                     os.path.join(GFX_SRC, "backends", "v9958",
                                  "v9958_gfx.c"),
                     gfx_h):
            self.assertTrue(os.path.isfile(path), path)
        for name in ("gfx.c", "gfx_mode.c", "gfx_draw.c", "gfx_batch.c",
                     "gfx_upload.c", "gfx_palette.c",
                     "backends/v9958/v9958_gfx.c"):
            source = read_text(os.path.join(GFX_SRC, name))
            for token in ("__sfr", "__at", "outp(", "inp("):
                self.assertNotIn(token, source,
                                 "%s reaches a port in C" % name)
        asm = read_text(os.path.join(GFX_SRC, "backends", "v9958",
                                     "v9958_io.asm"))
        self.assertIn("out\t(c),a", asm)
        self.assertIn("in\ta,(c)", asm)

    def test_upload_and_palette_surface(self):
        """The public header declares the palette and upload API, the
        backend's assembly layer exports their port helpers, and exe.mk
        builds the new sources into libgfx."""
        gfx_h = read_text(GFX_H)
        self.assertIn("struct gfx_rgb {", gfx_h)
        for name in ("gfx_set_palette", "gfx_upload_begin",
                     "gfx_upload_write", "gfx_upload_end"):
            self.assertIn(name, gfx_h)
        exe_mk = read_text(EXE_MK)
        self.assertIn("gfx_upload.c", exe_mk)
        self.assertIn("gfx_palette.c", exe_mk)
        asm = read_text(os.path.join(GFX_SRC, "backends", "v9958",
                                     "v9958_io.asm"))
        for name in ("_gfx_v9958_io_set_vram",
                     "_gfx_v9958_io_stream_vram",
                     "_gfx_v9958_io_palette_byte"):
            self.assertIn(name, asm)

    def test_batch_and_primitives_surface(self):
        """The public header declares the batch and the rectangle
        primitives; the queue is 32 16-byte records and exe.mk builds
        the batch member into libgfx."""
        gfx_h = read_text(GFX_H)
        for name in ("gfx_begin", "gfx_end", "gfx_flush",
                     "gfx_fill_rect", "gfx_copy_rect"):
            self.assertIn(name, gfx_h)
        internal = read_text(os.path.join(GFX_SRC, "gfx_internal.h"))
        self.assertIn("GFX_BATCH_ENTRIES  32u", internal)
        self.assertIn('sizeof(struct gfx_cmd) == 16', internal)
        self.assertIn("struct gfx_cmd {", internal)
        self.assertIn("GFX_OP_FILL 1u", internal)
        self.assertIn("GFX_OP_COPY 2u", internal)
        exe_mk = read_text(EXE_MK)
        self.assertIn("gfx_batch.c", exe_mk)

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

    def start(self, files=None):
        if files is None:
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
        # One rectangle, DY 0, NY 212: every row of the visible page
        # holds the colour.  A row is 256 bytes in both modes (512
        # dots of 4 bits, 256 of 8): the whole row holds the colour.
        row = bytes([PACKED[mode]]) * 256
        for line in range(CLEAR_LINES):
            base = line * 256
            self.assertEqual(peer.vram[base:base + 256], row,
                             "row %u" % line)

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


class TestGfxUpload(GfxReplay):
    """`gfxtest upload`: four streamed rectangles in chunk sizes 1, 7,
    256 and 4096, then the short, overlong and nested refusals."""

    def test_streams_refusals_and_restore(self):
        z = self.start()
        before = self.snapshot(z)
        ops = []
        since = len(z.glyphs)
        z.hid.hid_queue.extend(b"/fs/gfxtest.exe upload\r")
        # Each rectangle's stream starts its own pattern at offset 0
        # (gfxtest's `at` restarts per run), so every row is checked
        # against the matching slice from the pattern's beginning.
        self.run_recording(z, ops, lambda z: OPS_RELEASE in ops)
        peer = z.vdp
        pattern = bytes(upload_byte(i) for i in range(4096))
        for base, rows, row_bytes, stride, _length in UPLOAD_RECTS:
            for row in range(rows):
                start = base + row * stride
                self.assertEqual(peer.vram[start:start + row_bytes],
                                 pattern[row * row_bytes:
                                         (row + 1) * row_bytes],
                                 "rect %04X row %u" % (base, row))
        # The short stream wrote 16 bytes of row 350 and 4 of row 351.
        self.assertEqual(peer.vram[0x15E00:0x15E10], pattern[0:16])
        self.assertEqual(peer.vram[0x15F00:0x15F04], pattern[16:20])
        # The overlong stream's first chunk filled all four rows; the
        # extra byte was refused whole.
        for row in range(4):
            base = 0x16800 + row * 256
            self.assertEqual(peer.vram[base:base + 16],
                             pattern[row * 16:(row + 1) * 16],
                             "overlong row %u" % row)

        self.run_recording(z, ops, lambda z:
                           SHELL_PROMPT in "".join(z.glyphs[since:]))
        self.settle(z, lambda z: z.vdp.regs[8] == 0x08)
        fresh = self.trace(z, since)
        self.assertIn("gfxtest: upload -2 -2 -2", fresh)
        self.assertIn("gfxtest: upload PASS", fresh)
        self.assert_hold_sequence(ops)
        self.assert_restored(z, before)
        self.assertEqual(self.exclusive(z), 0,
                         "the exclusive flag survived the run")


class TestGfxPalette(GfxReplay):
    """`gfxtest palette`: the G6 entries while the display is held, a
    key, then the G7 refusal without a single palette port write."""

    def test_entries_then_g7_refusal(self):
        z = self.start()
        before = self.snapshot(z)
        ops = []
        since = len(z.glyphs)
        pcount = len(z.vdp.palette_writes)
        z.hid.hid_queue.extend(b"/fs/gfxtest.exe palette\r")
        # Key 1: the program blocks with the three entries on the peer.
        self.run_recording(z, ops, lambda z:
                           len(z.vdp.palette_writes) >= pcount + 6)
        self.assertEqual(z.vdp.regs[0], 0x0A, "key 1 still G6")
        self.assertEqual(z.vdp.regs[16], 3,
                         "R#16 ends on the last entry written")
        self.assertEqual(z.vdp.palette[1], [0x07, 0x00], "entry 1")
        self.assertEqual(z.vdp.palette[2], [0x70, 0x00], "entry 2")
        self.assertEqual(z.vdp.palette[3], [0x00, 0x04], "entry 3")
        self.assertEqual(len(z.vdp.palette_writes), pcount + 6)

        # Key 1: switch to GRAPHIC 7 and refuse the palette in C.
        pins = MODE_REGS["mode7"]
        z.hid.hid_queue.extend(b"x")
        self.run_recording(z, ops, lambda z:
                           z.vdp.regs[0] == pins["r0"] and
                           z.vdp.regs[9] == pins["r9"])
        self.assertEqual(len(z.vdp.palette_writes), pcount + 6,
                         "the G7 refusal must not reach the port")

        # Key 2: close, the result line, the restored console.
        z.hid.hid_queue.extend(b"x")
        self.run_recording(z, ops, lambda z:
                           SHELL_PROMPT in "".join(z.glyphs[since:]))
        self.settle(z, lambda z: z.vdp.regs[8] == 0x08)
        fresh = self.trace(z, since)
        self.assertIn("gfxtest: palette -7", fresh)
        self.assertIn("gfxtest: palette PASS", fresh)
        self.assertNotIn("FAIL", fresh)
        self.assert_hold_sequence(ops)
        self.assert_restored(z, before)
        self.assertEqual(self.exclusive(z), 0,
                         "the exclusive flag survived the run")


class TestPView(GfxReplay):
    """`pview`: the pictures land centred on the peer's VRAM (the g6
    one with its file palette), the console is restored after each, and
    a bad header never reaches the video endpoint."""

    def setUp(self):
        super().setUp()
        if not os.path.exists(PVIEW_EXE):
            self.skipTest("%s missing; run make -C UserSoftware/PView"
                          % PVIEW_EXE)

    def pview_keyed(self, z, ops, card_file):
        """Run pview on `card_file`; stop at the key block, where the
        picture is on the peer and the display is enabled while held.

        The console leaves R#1 at 40h, so the stop waits for the run's
        own display cycle: the mode setup's display-off (00h) followed
        by gfx_display_enable's 40h."""
        since = len(z.glyphs)
        z.hid.hid_queue.extend(("/fs/pview.exe /fs/%s\r" % card_file)
                               .encode("ascii"))
        seen_off = []

        def picture_on(z):
            if OPS_ACQUIRE not in ops:
                return False
            if z.vdp.regs[1] == 0x00:
                seen_off.append(True)
            return bool(seen_off) and z.vdp.regs[1] == 0x40
        self.run_recording(z, ops, picture_on)
        return since

    def pview_done(self, z, ops, since, before):
        """The key: close, the done line, the restored console."""
        z.hid.hid_queue.extend(b"x")
        self.run_recording(z, ops, lambda z:
                           SHELL_PROMPT in "".join(z.glyphs[since:]))
        self.settle(z, lambda z: z.vdp.regs[8] == 0x08)
        fresh = self.trace(z, since)
        self.assertIn("pview: done", fresh)
        self.assertNotIn("FAIL", fresh)
        self.assert_hold_sequence(ops)
        self.assert_restored(z, before)
        self.assertEqual(self.exclusive(z), 0,
                         "the exclusive flag survived the run")

    def assert_cleared(self, peer, row_bytes, rect, value, where):
        """Every visible byte of page 0 outside `rect` (x, y, width,
        height in bytes and rows) holds `value`."""
        x, y, width, height = rect
        for row in range(212):
            line = peer.vram[row * row_bytes:(row + 1) * row_bytes]
            if y <= row < y + height:
                line = line[:x] + line[x + width:]
            self.assertEqual(set(line), {value},
                             "%s: row %u not cleared" % (where, row))

    def test_pictures_centered_palette_and_bad_header(self):
        with open(os.path.join(FIXTURES, "parrot7.img"), "rb") as handle:
            g7 = handle.read()
        with open(os.path.join(FIXTURES, "parrot6.img"), "rb") as handle:
            g6 = handle.read()
        with open(PVIEW_EXE, "rb") as handle:
            # The card's filesystem is 8.3, so the on-card names use
            # the .img extension (the host-side fixtures keep their
            # .kimg names).
            files = {"/".join(KEEP_NAME): KEEP_CONTENT,
                     PVIEW_DEV: handle.read(),
                     "TEST7.IMG": g7, "TEST6.IMG": g6,
                     "BAD.IMG": b"not a picture\x00\x00"}
        z = self.start(files)
        before = self.snapshot(z)

        # The 256-colour picture: centred at x 96, y 90, no palette.
        ops = []
        since = self.pview_keyed(z, ops, "test7.img")
        peer = z.vdp
        payload = g7[16:]
        for row in range(32):
            base = (90 + row) * 256 + 96
            self.assertEqual(peer.vram[base:base + 64],
                             payload[row * 64:(row + 1) * 64],
                             "g7 row %u" % row)
        self.assertEqual(peer.regs[0], 0x0E, "g7 R#0")
        self.assertEqual([list(entry) for entry in peer.palette],
                         before["palette"], "no palette for a g7 file")
        # Around the picture the screen is cleared (byte 0, black): no
        # console bitmap shows through.
        self.assert_cleared(peer, 256, (96, 90, 64, 32), 0x00, "g7")
        self.pview_done(z, ops, since, before)

        # The 16-colour picture: centred at x 224, y 90, with the
        # file's 16 palette entries applied.
        ops = []
        since = self.pview_keyed(z, ops, "test6.img")
        peer = z.vdp
        palette_bytes = g6[16:64]
        payload = g6[64:]
        for entry in range(16):
            red, green, blue = palette_bytes[entry * 3:entry * 3 + 3]
            self.assertEqual(peer.palette[entry],
                             [((red >> 5) << 4) | (blue >> 5),
                              green >> 5], "entry %u" % entry)
        for row in range(32):
            base = (90 + row) * 256 + 112
            self.assertEqual(peer.vram[base:base + 32],
                             payload[row * 32:(row + 1) * 32],
                             "g6 row %u" % row)
        self.assertEqual(peer.regs[0], 0x0A, "g6 R#0")
        # Cleared to the palette's darkest entry, two pixels a byte.
        levels = [palette_bytes[i * 3] * 2 + palette_bytes[i * 3 + 1] * 4 +
                  palette_bytes[i * 3 + 2] for i in range(16)]
        dark = levels.index(min(levels))
        self.assert_cleared(peer, 256, (112, 90, 32, 32),
                            (dark << 4) | dark, "g6")
        self.pview_done(z, ops, since, before)

        # The bad header: the error line, and no video dispatch at all.
        ops = []
        since = len(z.glyphs)
        z.hid.hid_queue.extend(b"/fs/pview.exe /fs/bad.img\r")
        self.run_recording(z, ops, lambda z:
                           SHELL_PROMPT in "".join(z.glyphs[since:]))
        fresh = self.trace(z, since)
        self.assertIn("pview: /fs/bad.img: bad header", fresh)
        # Only the console's own WRITE renders: pview never opened the
        # video (no ACQUIRE, DIRECT_INFO or RELEASE).
        self.assertTrue(all(op == OPS_WRITE for op in ops),
                        "a bad header must not reach the video "
                        "endpoint: %s" % (ops,))


class TestGfxPrims(GfxReplay):
    """`gfxtest prims`: the batch, the rectangle primitives, the
    refusals, against the peer's executed commands and VRAM."""

    def test_batch_primitives_and_refusals(self):
        z = self.start()
        before = self.snapshot(z)
        ops = []
        since = len(z.glyphs)
        z.hid.hid_queue.extend(b"/fs/gfxtest.exe prims\r")

        # The command count is taken at the hold: the console's own
        # commands (the command echo) end there, and every later
        # command is gfxtest's.  gfx_set_mode's R#46 stop write reads
        # back as a code-0 entry, filtered below.
        self.run_recording(z, ops, lambda z: OPS_ACQUIRE in ops)
        c0 = len(z.vdp.commands)

        def draws():
            return [c for c in z.vdp.commands[c0:]
                    if c["code"] != 0x00]

        # First the black, shown screen the owner watches: the visible
        # page's clear; the counts below start after it.
        self.run_recording(z, ops, lambda z: len(draws()) >= 1)
        self.assertEqual(draws()[0]["fill"], (0, 0, 256, 212, 0x00),
                         "the black clear")
        c0 = z.vdp.commands.index(draws()[0]) + 1

        # The ordering batch and the 33-fill batch: the 33rd queued
        # fill flushes the first 32 while the batch is still open, so
        # 34 commands land before the key with the hold intact.
        self.run_recording(z, ops, lambda z: len(draws()) >= 34)
        self.assertNotIn(OPS_RELEASE, ops,
                         "the auto-flush ran inside the batch")
        self.assertEqual(z.vdp.regs[1] & 0x40, 0x40,
                         "the display is on while prims draws")
        cmds = draws()
        self.assertEqual(len(cmds), 34, [c["fill"] for c in cmds])
        self.assertEqual(cmds[0]["fill"], (0, 0, 40, 8, 0x11),
                         "the first fill")
        self.assertEqual(cmds[1]["fill"], (8, 2, 16, 4, 0x22),
                         "the overlapping fill")
        for i in range(32):
            self.assertEqual(cmds[2 + i]["fill"],
                             (i * 6, 100, 4, 2, i + 1),
                             "fill %u" % i)

        # Key 1: gfx_end flushes the 33rd fill; then the copies and the
        # page fills run; the program blocks at key 2.
        z.hid.hid_queue.extend(b"x")
        self.run_recording(z, ops, lambda z: len(draws()) >= 43)
        cmds = draws()
        self.assertEqual(len(cmds), 43)
        self.assertEqual(cmds[34]["fill"], (192, 100, 4, 2, 33),
                         "the 33rd fill ran at gfx_end")
        # The four overlap copies: DIX/DIY chosen from the overlap and
        # the block carrying each rectangle's far edge.
        copies = ((204, 22, 0x0C, 203, 23, 207, 25),
                  (196, 22, 0x08, 200, 23, 196, 25),
                  (204, 18, 0x04, 203, 20, 207, 18),
                  (196, 18, 0x00, 200, 20, 196, 18))
        for index, (dx, dy, arg, sx, sy, bx, by) in enumerate(copies):
            entry = cmds[35 + index]
            self.assertEqual(entry["code"], 0xD0, "copy %u" % index)
            block = entry["block"]
            self.assertEqual(block[0] | (block[1] << 8), sx,
                             "copy %u SX" % index)
            self.assertEqual(block[2] | (block[3] << 8), sy,
                             "copy %u SY" % index)
            self.assertEqual(block[4] | (block[5] << 8), bx,
                             "copy %u DX" % index)
            self.assertEqual(block[6] | (block[7] << 8), by,
                             "copy %u DY" % index)
            self.assertEqual(block[8] | (block[9] << 8), 4,
                             "copy %u NX" % index)
            self.assertEqual(block[10] | (block[11] << 8), 4,
                             "copy %u NY" % index)
            self.assertEqual(block[13], arg, "copy %u ARG" % index)
        # The page commands: a fill and its copy, each way.  Without
        # overlap the direction is the forward one per axis (sx < dx
        # takes DIX, sy < dy takes DIY) and the block still carries
        # the far edge.
        self.assertEqual(cmds[39]["fill"], (0, 266, 6, 3, 0x55))
        block = cmds[40]["block"]
        self.assertEqual(block[13], 0x04)
        self.assertEqual((block[0] | (block[1] << 8),
                          block[2] | (block[3] << 8),
                          block[4] | (block[5] << 8),
                          block[6] | (block[7] << 8)),
                         (5, 266, 55, 60))
        self.assertEqual(cmds[41]["fill"], (150, 40, 6, 3, 0x66))
        block = cmds[42]["block"]
        self.assertEqual(block[13], 0x08)
        self.assertEqual((block[0] | (block[1] << 8),
                          block[2] | (block[3] << 8),
                          block[4] | (block[5] << 8),
                          block[6] | (block[7] << 8)),
                         (150, 42, 150, 298))

        # The VRAM outcome while the display is held: the ordering
        # fills, the 33 queued fills, the four copy destinations equal
        # to the uploaded pattern, the source intact, and the page
        # copies across the page boundary.
        peer = z.vdp
        for row in (0, 1, 6, 7):
            line = peer.vram[row * 256:(row + 1) * 256]
            self.assertEqual(line[0:40], bytes([0x11]) * 40,
                             "order row %u" % row)
        for row in range(2, 6):
            line = peer.vram[row * 256:(row + 1) * 256]
            self.assertEqual(line[0:8], bytes([0x11]) * 8,
                             "order row %u left" % row)
            self.assertEqual(line[8:24], bytes([0x22]) * 16,
                             "order row %u middle" % row)
            self.assertEqual(line[24:40], bytes([0x11]) * 16,
                             "order row %u right" % row)
        for i in range(33):
            col = i * 6
            for row in (100, 101):
                self.assertEqual(peer.vram[row * 256 + col:
                                           row * 256 + col + 4],
                                 bytes([i + 1]) * 4, "fill %u" % i)
        pattern = bytes(upload_byte(i) for i in range(16))
        for row in range(4):
            self.assertEqual(peer.vram[(20 + row) * 256 + 200:
                                       (20 + row) * 256 + 204],
                             pattern[row * 4:(row + 1) * 4],
                             "source row %u" % row)
            for dx, dy, _arg, _sx, _sy, _bx, _by in copies:
                self.assertEqual(peer.vram[(dy + row) * 256 + dx:
                                           (dy + row) * 256 + dx + 4],
                                 pattern[row * 4:(row + 1) * 4],
                                 "copy to %u,%u row %u" % (dx, dy, row))
        for row in range(3):
            self.assertEqual(peer.vram[(60 + row) * 256 + 50:
                                       (60 + row) * 256 + 56],
                             bytes([0x55]) * 6, "page 1->0 row %u" % row)
            self.assertEqual(peer.vram[0x10000 + (40 + row) * 256 + 150:
                                       0x10000 + (40 + row) * 256 + 156],
                             bytes([0x66]) * 6, "page 0->1 row %u" % row)

        # Key 2: the G6 batch runs exactly one fill (the odd x refusal
        # queued nothing), the zero sizes are silent no-ops, and the
        # console is restored.  The count is cut at the release's
        # dispatch: after it the console's restore redraw adds its own
        # commands.
        release_at = []

        def until_prompt(z):
            if OPS_RELEASE in ops and not release_at:
                release_at.append(len(z.vdp.commands))
            return SHELL_PROMPT in "".join(z.glyphs[since:])
        z.hid.hid_queue.extend(b"x")
        self.run_recording(z, ops, until_prompt)
        self.settle(z, lambda z: z.vdp.regs[8] == 0x08)
        cmds = [c for c in z.vdp.commands[c0:release_at[0]]
                if c["code"] != 0x00]
        self.assertEqual(len(cmds), 45,
                         "the refusals queued nothing")
        self.assertEqual(cmds[43]["fill"], (2, 0, 2, 2, 0x11),
                         "the G6 fill packs two pixels per byte")
        # The YJK refusals (x 2, a copy to x 6) queue nothing; the
        # aligned fill at x 4 is the last command.
        self.assertEqual(cmds[44]["fill"], (4, 0, 4, 2, 0x00),
                         "the aligned YJK fill")
        fresh = self.trace(z, since)
        self.assertIn("gfxtest: prims -2 0 0 0 -2 -2 0", fresh)
        self.assertIn("gfxtest: prims PASS", fresh)
        self.assertNotIn("FAIL", fresh)
        self.assert_hold_sequence(ops)
        self.assert_restored(z, before)
        self.assertEqual(self.exclusive(z), 0,
                         "the exclusive flag survived the run")


if __name__ == "__main__":
    unittest.main()
