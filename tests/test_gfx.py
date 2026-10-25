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
import z80sim  # noqa: E402
from z80sim import IocCommandPeer  # noqa: E402

GFXTEST_EXE = os.path.join(ROOT, "build", "user", "gfxtest.exe")
PVIEW_EXE = os.path.join(ROOT, "UserSoftware", "PView", "build", "user",
                         "pview.exe")
GFXDEMO_EXE = os.path.join(ROOT, "UserSoftware", "GfxDemo", "build",
                           "user", "gfxdemo.exe")
FIXTURES = os.path.join(ROOT, "tests", "gfx", "fixtures")
FONT_ASSET = os.path.join(ROOT, "drivers", "v9958", "assets",
                          "font_cp850_6x8.inc")
FONT4_ASSET = os.path.join(ROOT, "drivers", "v9958", "assets",
                           "font_cp850_4x6.inc")
DEV_NAME = "GFXTEST.EXE"
PVIEW_DEV = "PVIEW.EXE"
GFXDEMO_DEV = "GFXDEMO.EXE"
MODULE_DIR = os.path.join(ROOT, "build", "modules")
PROTOCOL_H = os.path.join(ROOT, "abi", "kos", "protocol.h")
KVIDEO_H = os.path.join(ROOT, "abi", "kos", "kvideo.h")
MAKEFILE = os.path.join(ROOT, "Makefile")
GFX_SRC = os.path.join(ROOT, "user", "sdk", "gfx", "src")
GFX_V9958 = os.path.join(GFX_SRC, "backends", "v9958")
# The drawing modules: every routine that draws or moves pixels.
GFX_DRAW_ASM = ("gfx_cmd.asm", "gfx_ellipse.asm", "gfx_text.asm",
                "gfx_atlas_draw.asm", "gfx_upload.asm")
EXE_MK = os.path.join(ROOT, "user", "sdk", "exe.mk")
GFX_H = os.path.join(ROOT, "user", "sdk", "include", "kos", "gfx.h")
FONTPACK = os.path.join(ROOT, "tools", "fontpack.py")

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


def load_font():
    """The pinned CP850 6x8 font, parsed from the reviewable asset."""
    from fontpack import parse_font
    return parse_font(FONT_ASSET)


def load_font4():
    """The pinned CP850 4x6 font (pixels in bits 7..4), parsed from
    the reviewable asset."""
    from fontpack import parse_font
    return parse_font(FONT4_ASSET, size=256 * 6, glyph_bytes=6)


def g6_pair(fg, bg):
    """The expansion's pair table: two font bits (the left pixel in
    bit 1) to one packed GRAPHIC 6 byte."""
    return ((bg << 4) | bg) & 0xFF, ((bg << 4) | fg) & 0xFF, \
        ((fg << 4) | bg) & 0xFF, ((fg << 4) | fg) & 0xFF


def g6_text_row(font, code, row, fg, bg):
    """One expanded glyph row's three GRAPHIC 6 bytes."""
    byte = font[code * 8 + row]
    pair = g6_pair(fg, bg)
    return bytes(pair[(byte >> shift) & 3] for shift in (6, 4, 2))


def g7_text_row(font, code, row, fg, bg):
    """One expanded glyph row's six GRAPHIC 7 bytes."""
    byte = font[code * 8 + row]
    return bytes(fg if byte & (0x80 >> bit) else bg for bit in range(6))


def g6_text_row4(font4, code, row, fg, bg):
    """One expanded 4x6 glyph row's two GRAPHIC 6 bytes (the font's
    pixels live in bits 7..4)."""
    byte = font4[code * 6 + row]
    pair = g6_pair(fg, bg)
    return bytes((pair[(byte >> 6) & 3], pair[(byte >> 4) & 3]))


def g7_text_row4(font4, code, row, fg, bg):
    """One expanded 4x6 glyph row's four GRAPHIC 7 bytes."""
    byte = font4[code * 6 + row]
    return bytes(fg if byte & (0x80 >> bit) else bg for bit in range(4))


def atlas_cell4(font4, code, fg, g6):
    """One 4x6 atlas cell's rows: two GRAPHIC 6 bytes or four
    GRAPHIC 7 bytes each."""
    for row in range(6):
        if g6:
            yield g6_text_row4(font4, code, row, fg, 0)
        else:
            yield g7_text_row4(font4, code, row, fg, 0)


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
                     os.path.join(GFX_SRC, "gfx_palette.c"),
                     os.path.join(GFX_SRC, "backends", "v9958",
                                  "v9958_gfx.c"),
                     gfx_h):
            self.assertTrue(os.path.isfile(path), path)
        for name in GFX_DRAW_ASM:
            self.assertTrue(os.path.isfile(os.path.join(GFX_V9958, name)),
                            name)
        for name in ("gfx.c", "gfx_mode.c", "gfx_palette.c", "gfx_font.c",
                     "gfx_font4.c", "gfx_atlas.c",
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
        self.assertIn("gfx_upload.asm", exe_mk)
        self.assertIn("gfx_palette.c", exe_mk)
        asm = read_text(os.path.join(GFX_V9958, "v9958_io.asm"))
        for name in ("_gfx_v9958_io_set_vram",
                     "_gfx_v9958_io_stream_vram",
                     "_gfx_v9958_io_palette_byte"):
            self.assertIn(name, asm)
        upload = read_text(os.path.join(GFX_V9958, "gfx_upload.asm"))
        for name in ("_gfx_upload_begin::", "_gfx_upload_write::",
                     "_gfx_upload_end::"):
            self.assertIn(name, upload)

    def test_batch_and_primitives_surface(self):
        """The public header declares the batch and the rectangle
        primitives; the command path's assembly holds them, the queue
        is 32 ready command blocks, and exe.mk builds it into libgfx."""
        gfx_h = read_text(GFX_H)
        for name in ("gfx_begin", "gfx_end", "gfx_flush",
                     "gfx_fill_rect", "gfx_copy_rect"):
            self.assertIn(name, gfx_h)
        cmd = read_text(os.path.join(GFX_V9958, "gfx_cmd.asm"))
        self.assertIn("GFX_BATCH_ENTRIES       = 32", cmd)
        for name in ("_gfx_begin::", "_gfx_end::", "_gfx_flush::",
                     "_gfx_fill_rect::", "_gfx_clear::",
                     "_gfx_copy_rect::"):
            self.assertIn(name, cmd)
        exe_mk = read_text(EXE_MK)
        self.assertIn("gfx_cmd.asm", exe_mk)

    def test_shapes_surface(self):
        """The public header declares the shapes API (the op enum and
        the six calls); the assembly holds the entry points and the
        command codes, and exe.mk builds the ellipse member."""
        gfx_h = read_text(GFX_H)
        for name in ("enum gfx_op {", "GFX_OP_COPY", "GFX_OP_XOR",
                     "gfx_rect", "gfx_line", "gfx_point", "gfx_ellipse",
                     "gfx_fill_ellipse"):
            self.assertIn(name, gfx_h)
        self.assertIn("enum gfx_op op);", gfx_h)
        cmd = read_text(os.path.join(GFX_V9958, "gfx_cmd.asm"))
        for name in ("_gfx_rect::", "_gfx_line::", "_gfx_point::",
                     "GFX_CMD_HMMV            = 0xC0",
                     "GFX_CMD_HMMM            = 0xD0",
                     "GFX_CMD_LMMV            = 0x80",
                     "GFX_CMD_LINE            = 0x70",
                     "GFX_CMD_PSET            = 0x50"):
            self.assertIn(name, cmd)
        ellipse = read_text(os.path.join(GFX_V9958, "gfx_ellipse.asm"))
        for name in ("_gfx_ellipse::", "_gfx_fill_ellipse::"):
            self.assertIn(name, ellipse)
        exe_mk = read_text(EXE_MK)
        self.assertIn("gfx_ellipse.asm", exe_mk)

    def test_text_and_font_surface(self):
        """The public header declares the font and text API; the
        internal header holds the two 2048-byte font sizes, the pair
        caches and the text-row args; both font tables are generated
        from the pinned assets at build."""
        gfx_h = read_text(GFX_H)
        for name in ("enum gfx_font", "gfx_font_set_glyph",
                     "gfx_font_reset_glyph", "gfx_font_reset",
                     "gfx_text", "gfx_text_n", "gfx_text_width",
                     "gfx_text_atlas", "GFX_TEXT_TRANSPARENT",
                     "struct gfx_text_style"):
            self.assertIn(name, gfx_h)
        internal = read_text(os.path.join(GFX_SRC, "gfx_internal.h"))
        self.assertIn("GFX_FONT_SIZE", internal)
        self.assertIn("GFX_FONT4_SIZE", internal)
        text = read_text(os.path.join(GFX_V9958, "gfx_text.asm"))
        for name in ("_gfx_text::", "_gfx_text_n::"):
            self.assertIn(name, text)
        exe_mk = read_text(EXE_MK)
        for member in ("gfx_font.c", "gfx_font4.c", "gfx_atlas.c",
                       "gfx_text.asm", "gfx_atlas_draw.asm"):
            self.assertIn(member, exe_mk)
        font_c = read_text(os.path.join(GFX_SRC, "gfx_font.c"))
        self.assertIn('#include "font_builtin.inc"', font_c)
        font4_c = read_text(os.path.join(GFX_SRC, "gfx_font4.c"))
        self.assertIn('#include "font4_builtin.inc"', font4_c)
        makefile = read_text(MAKEFILE)
        self.assertIn("fontpack.py table $< $@", makefile)
        self.assertIn("fontpack.py table4x6 $< $@", makefile)
        fontpack = read_text(FONTPACK)
        self.assertIn('command == "table"', fontpack)
        self.assertIn('command == "table4x6"', fontpack)

    def test_makefile_wires_gfxtest_with_libgfx(self):
        makefile = read_text(MAKEFILE)
        lines = [line for line in makefile.splitlines()
                 if "exe_program,gfxtest" in line or
                 "gfxtest.c" in line]
        self.assertTrue(lines, "the Makefile declares no gfxtest")
        self.assertTrue(any("LIBS=gfx" in line for line in lines),
                        "gfxtest does not link LIBS=gfx")


class VdpClockUnit(unittest.TestCase):
    """The peer's clock: `now` is the CPU's T-state count, and the
    duration model keeps CE busy for the modeled T-states."""

    def test_register_write_timestamps_are_tstates(self):
        z = z80sim.PythonZ80(bytearray(0x10000), bytearray(0x10000))
        z.vdp_peer = z80sim.V9958Peer()
        z.latch = 0x08  # flat SRAM: writes and fetches hit bank 0
        # ld a,2Eh (7) ; out (A1h),a (11) ; ld a,AEh (7) ;
        # out (A1h),a (11) = one R#2E write, 36 T-states;
        # three nops (12), the same write again: exactly 48 apart.
        program = bytes((0x3E, 0x2E, 0xD3, 0xA1,
                         0x3E, 0xAE, 0xD3, 0xA1,
                         0x00, 0x00, 0x00,
                         0x3E, 0x2E, 0xD3, 0xA1,
                         0x3E, 0xAE, 0xD3, 0xA1))
        for index, byte in enumerate(program):
            z.wr(0x0100 + index, byte)
        z.pc = 0x0100
        for _ in range(len(program)):
            z.step()
        writes = [now for _value, reg, now in z.vdp_peer.reg_writes
                  if reg == 0x2E]
        self.assertEqual(len(writes), 2, writes)
        self.assertEqual(writes[1] - writes[0], 48,
                         "two writes 48 T-states apart")

    def test_hmmm_ce_busy_window_is_the_model(self):
        peer = z80sim.V9958Peer()
        peer.duration_model = True
        peer.regs[0] = 0x02  # GRAPHIC 6: two dots per byte
        # A 128 x 40-dot HMMM: SX/SY/DX/DY, NX/NY, ARG, CMD.
        for reg, value in zip(range(32, 47), (0, 0, 0, 0,
                                              0, 0, 0, 0,
                                              128, 0, 40, 0, 0, 0xD0)):
            peer.regs[reg] = value
        peer.now = 1000
        peer._command_start(0xD0)
        bytes_total = (128 // 2) * 40
        expected = ((88 * bytes_total + 40) * z80sim.CPU_HZ //
                    z80sim.VDP_HZ)
        self.assertEqual(peer.commands[-1]["busy_until"] -
                         peer.commands[-1]["start"], expected,
                         "the HMMM's busy window is the model's T-states")
        peer.status_read()
        self.assertEqual(peer.status_reads[-1][1], 1,
                         "CE set while the command runs")
        peer.now = peer.busy_until
        peer.status_read()
        self.assertEqual(peer.status_reads[-1][1], 0,
                         "CE clear once the command finished")


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

    # -- text evidence ---------------------------------------------------
    def assert_text(self, z, font, text, x, y, fg, bg, g6, where):
        """Every glyph of `text`, drawn at (x, y) in the style's
        colours, decoded from the peer's VRAM against the pinned
        font."""
        peer = z.vdp
        width = 3 if g6 else 6
        for index, code in enumerate(text.encode("latin-1")):
            col = (x + index * 6) // 2 if g6 else x + index * 6
            for row in range(8):
                expected = (g6_text_row(font, code, row, fg, bg)
                            if g6 else g7_text_row(font, code, row, fg,
                                                   bg))
                base = (y + row) * 256 + col
                self.assertEqual(peer.vram[base:base + width], expected,
                                 "%s: '%s' glyph %u row %u"
                                 % (where, text, index, row))

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
        # Key 1: the program blocks with the three entries on the peer
        # and the display shown.
        self.run_recording(z, ops, lambda z:
                           len(z.vdp.palette_writes) >= pcount + 6 and
                           z.vdp.regs[1] & 0x40)
        self.assertEqual(z.vdp.regs[0], 0x0A, "key 1 still G6")
        self.assertEqual(z.vdp.regs[1] & 0x40, 0x40,
                         "the display is on at key 1")
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
                           z.vdp.regs[9] == pins["r9"] and
                           z.vdp.regs[1] & 0x40)
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
                         "the 33rd fill ran at gfx_end")        # The four overlap copies: DIX/DIY chosen from the overlap and
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

        # Key 2: the G6 batch runs the odd-x fill (LMMV) and the
        # aligned fill (HMMV), the out-of-bounds fill queues nothing,
        # the zero sizes are silent no-ops, and the console is
        # restored.  The count is cut at the release's dispatch: after
        # it the console's restore redraw adds its own commands.
        release_at = []

        def until_prompt(z):
            if OPS_RELEASE in ops and not release_at:
                release_at.append(len(z.vdp.commands))
            return SHELL_PROMPT in "".join(z.glyphs[since:])
        z.hid.hid_queue.extend(b"x")
        # The G6 mode switch keeps the G7 drawings' bytes; the
        # program's black clear makes the fills' dots observable.
        # Stop once the aligned fill has executed (the 46th command:
        # the clear, the LMMV, the aligned HMMV), before the YJK mode
        # switch and gfx_close's console restore overwrite the bytes.
        # The odd-x LMMV's dots: x 1 the low nibble of the first byte,
        # x 2 the high nibble of the second; the aligned HMMV at x 2
        # then covers the second byte whole (two dots per byte in
        # GRAPHIC 6).
        self.run_recording(z, ops, lambda z: len(draws()) >= 46)
        for row in (0, 1):
            self.assertEqual(peer.vram[row * 256],
                             0x01, "odd-x LMMV row %u" % row)
            self.assertEqual(peer.vram[row * 256 + 1],
                             0x11, "the aligned fill row %u" % row)
        self.assertEqual(peer.vram[0x0002:0x0004],
                         bytes([0x00, 0x00]), "past the aligned fill")
        self.run_recording(z, ops, until_prompt)
        self.settle(z, lambda z: z.vdp.regs[8] == 0x08)
        cmds = [c for c in z.vdp.commands[c0:release_at[0]]
                if c["code"] != 0x00]
        self.assertEqual(len(cmds), 47,
                         "the refusals queued nothing")
        self.assertEqual(cmds[43]["fill"], (0, 0, 512, 212, 0x00),
                         "the G6 clear after the mode switch")
        self.assertEqual(cmds[44]["code"], 0x80,
                         "the odd-x fill is LMMV")
        self.assertEqual(cmds[44]["logical_fill"],
                         (1, 0, 2, 2, 0x01, 0x00),
                         "the odd-x LMMV block")
        self.assertEqual(cmds[45]["fill"], (2, 0, 2, 2, 0x11),
                         "the G6 fill packs two pixels per byte")
        # The YJK refusals (x 2, a copy to x 6) queue nothing; the
        # aligned fill at x 4 is the last command.
        self.assertEqual(cmds[46]["fill"], (4, 0, 4, 2, 0x00),
                         "the aligned YJK fill")
        fresh = self.trace(z, since)
        self.assertIn("gfxtest: prims 0 0 -2 0 0 -2 -2 0", fresh)
        self.assertIn("gfxtest: prims PASS", fresh)
        self.assertNotIn("FAIL", fresh)
        self.assert_hold_sequence(ops)
        self.assert_restored(z, before)
        self.assertEqual(self.exclusive(z), 0,
                         "the exclusive flag survived the run")


# --- the shapes' reference drawings -------------------------------------

def vdp_line(x0, y0, x1, y1, color):
    """The VDP LINE command's pixels, mirroring openMSX's
    VDPCmdEngine (as the peer does): the major axis steps once per
    pixel, the minor when the error `asx` underflows, Maj + 1 dots."""
    dx = x1 - x0
    dy = y1 - y0
    adx, ady = abs(dx), abs(dy)
    pixels = []
    if adx >= ady:
        maj, mnr = adx, ady
        step_x = 1 if dx >= 0 else -1
        step_y = 1 if dy >= 0 else -1
        x, y = x0, y0
        asx = (maj - 1) >> 1
        steps = 0
        while True:
            pixels.append((x, y))
            x += step_x
            steps += 1
            if steps > maj:
                break
            if asx < mnr:
                asx += maj
                y += step_y
            asx -= mnr
            asx &= 1023
    else:
        maj, mnr = ady, adx
        step_x = 1 if dx >= 0 else -1
        step_y = 1 if dy >= 0 else -1
        x, y = x0, y0
        asx = (maj - 1) >> 1
        steps = 0
        while True:
            pixels.append((x, y))
            y += step_y
            steps += 1
            if steps > maj:
                break
            if asx < mnr:
                asx += maj
                x += step_x
            asx -= mnr
            asx &= 1023
    return pixels


class _SpanSet:
    """The C midpoint ellipse walk's per-scanline bounds: the
    leftmost and rightmost visited dot of each scanline."""

    def __init__(self, cx, cy, rx, ry):
        self.cx = cx
        self.cy = cy
        self.lines = {}
        self._walk(rx, ry, rx * rx, ry * ry)

    def _visit(self, x, y):
        left = self.cx - x
        right = self.cx + x
        for line in ((self.cy + y, self.cy - y)
                     if y != 0 else (self.cy,)):
            if line in self.lines:
                self.lines[line] = (min(self.lines[line][0], left),
                                    max(self.lines[line][1], right))
            else:
                self.lines[line] = (left, right)

    def _walk(self, rx, ry, rx2, ry2):
        x, y = 0, ry
        d1 = ry2 - rx2 * ry + rx2 // 4
        dx2, dy2 = 0, 2 * rx2 * y
        while dx2 <= dy2:
            self._visit(x, y)
            x += 1
            dx2 += 2 * ry2
            if d1 < 0:
                d1 += dx2 + ry2
            else:
                y -= 1
                dy2 -= 2 * rx2
                d1 += dx2 + ry2 - dy2
        d2 = ry2 * (x - 1) * (x - 1) + rx2 * y * y - rx2 * ry2
        while y != 0:
            self._visit(x, y)
            old_x = x
            y -= 1
            if d2 > 0:
                d2 += rx2 - 2 * rx2 * y
            else:
                x += 1
                d2 += rx2 - 2 * rx2 * y + 2 * ry2 * old_x
        self._visit(x, 0)


def fill_ellipse_pixels(cx, cy, rx, ry):
    """The filled ellipse's pixels: one span per scanline from the
    walk's leftmost to its rightmost dot (the C emit)."""
    pixels = set()
    for line, (left, right) in _SpanSet(cx, cy, rx, ry).lines.items():
        pixels.update((x, line) for x in range(left, right + 1))
    return pixels


def fill_ellipse_pixels(cx, cy, rx, ry):
    """The filled ellipse's pixels: one span per scanline from the
    walk's leftmost to its rightmost dot (the C emit)."""
    pixels = set()
    for line, (left, right) in _SpanSet(cx, cy, rx, ry).lines.items():
        pixels.update((x, line) for x in range(left, right + 1))
    return pixels


def fill_boundary(fill):
    """The 4-neighbour boundary of a filled pixel set: every pixel
    with at least one of its four neighbours outside the set."""
    return {(x, y) for x, y in fill
            if (x - 1, y) not in fill or (x + 1, y) not in fill or
               (x, y - 1) not in fill or (x, y + 1) not in fill}


def outline_ellipse_pixels(cx, cy, rx, ry):
    """The outline ellipse's pixels: the filled set's boundary, every
    fill pixel with at least one of its four neighbours outside the
    fill (derived from the fill alone - it never mirrors the C
    emit)."""
    if rx == 0 or ry == 0:
        # The library routes radius 0 to the degenerate point and
        # line paths (gfx_point/gfx_line), never the walk; their
        # whole fill is boundary.
        if rx == 0 and ry == 0:
            return {(cx, cy)}
        if rx == 0:
            return {(cx, y) for y in range(cy - ry, cy + ry + 1)}
        return {(x, cy) for x in range(cx - rx, cx + rx + 1)}
    return fill_boundary(fill_ellipse_pixels(cx, cy, rx, ry))


def rect_outline_pixels(x, y, width, height):
    pixels = set(vdp_line(x, y, x + width - 1, y, 0))
    pixels.update(vdp_line(x + width - 1, y, x + width - 1,
                           y + height - 1, 0))
    pixels.update(vdp_line(x + width - 1, y + height - 1, x,
                           y + height - 1, 0))
    pixels.update(vdp_line(x, y + height - 1, x, y, 0))
    return pixels


class TestGfxShapes(GfxReplay):
    """`gfxtest shapes`: the shapes' commands and pixels against the
    reference drawings, in GRAPHIC 7 and GRAPHIC 6."""

    def test_outline_reference_is_the_fill_boundary(self):
        """The outline reference (derived from the fill by the
        4-neighbour rule) is one closed ring at every radius: every
        pixel 8-connected to two others, a subset of the fill, and
        the degenerate point and lines are fully boundary (their
        whole fill has a neighbour outside)."""
        cases = [(24, 24), (40, 12), (6, 40), (30, 10), (10, 30),
                 (16, 4), (4, 16), (1, 1), (0, 0), (0, 6), (6, 0)]
        for rx, ry in cases:
            if rx == 0 and ry == 0:
                fill = {(128, 128)}
            elif rx == 0:
                fill = {(128, y) for y in range(128 - ry, 128 + ry + 1)}
            elif ry == 0:
                fill = {(x, 128) for x in range(128 - rx, 128 + rx + 1)}
            else:
                fill = fill_ellipse_pixels(128, 128, rx, ry)
            outline = outline_ellipse_pixels(128, 128, rx, ry)
            self.assertTrue(outline <= fill,
                            "the %ux%u outline is a subset of the fill"
                            % (rx, ry))
            if rx == 0 or ry == 0:
                self.assertEqual(outline, fill,
                                 "the degenerate %ux%u outline is "
                                 "the whole fill" % (rx, ry))
                continue
            self.assertTrue(outline, "the %ux%u outline is not empty"
                            % (rx, ry))
            for x, y in outline:
                neighbours = sum(
                    1 for dx in (-1, 0, 1) for dy in (-1, 0, 1)
                    if (dx or dy) and (x + dx, y + dy) in outline)
                self.assertEqual(
                    neighbours, 2,
                    "pixel %u,%u of the %ux%u ring has %d ring "
                    "neighbours" % (x, y, rx, ry, neighbours))

    def test_lines_rects_ellipses_and_refusals(self):
        z = self.start()
        before = self.snapshot(z)
        ops = []
        since = len(z.glyphs)
        z.hid.hid_queue.extend(b"/fs/gfxtest.exe shapes\r")

        self.run_recording(z, ops, lambda z: OPS_ACQUIRE in ops)
        c0 = len(z.vdp.commands)

        def draws():
            return [c for c in z.vdp.commands[c0:]
                    if c["code"] != 0x00]

        # The G7 drawings: run until the program blocks at key 1
        # (the draws count goes quiet), with the display held.
        self.run_recording(z, ops, lambda z: len(draws()) >= 2)
        steady = 0
        count = 0
        for _step in range(EXEC_BUDGET * 3):
            self.skip_delay(z)
            self.feed(z)
            z.step()
            now = len(draws())
            if now >= 24 and now == count:
                steady += 1
            else:
                steady = 0
            count = now
            if steady >= 200000:
                break
        else:
            self.fail("the shapes never reached key 1")
        self.assertNotIn(OPS_RELEASE, ops,
                         "the shapes still hold the display at key 1")
        self.assertEqual(z.vdp.regs[0], 0x0E, "still GRAPHIC 7 at key 1")
        self.assertEqual(z.vdp.regs[1] & 0x40, 0x40,
                         "the display is on at key 1")

        # Key 1: every G7 drawing's result is on the peer's VRAM.
        # The reference draws the same shapes, in the program's order:
        # the lines, points and rectangles from the reference walkers,
        # the fills from the reference midpoint sets and the outlines
        # from the reference boundary.
        g7_end = len(z.vdp.commands)
        peer = z.vdp
        width = 256
        screen = {}
        for pixel in (0, 1, 2, 3, 4, 5, 6, 7):
            for x, y in vdp_line(64, 48, 64 + 40 * [0, 0, 0, 1, 2, 2,
                                                    2, 1][pixel],
                                 48 + 32 * [0, 1, 2, 2, 2, 1, 0, 0][
                                     pixel], pixel + 1):
                screen[x, y] = pixel + 1
        for x, y in vdp_line(128, 150, 215, 150, 0):
            screen[x, y] = 0x0A
        for x, y in vdp_line(128, 152, 128, 195, 0):
            screen[x, y] = 0x0B
        screen[220, 160] = 0x0C
        screen[0, 0] = 0x0D
        screen[255, 211] = 0x0E
        for x, y in rect_outline_pixels(10, 100, 30, 16):
            screen[x, y] = 0x1A
        for x in range(50, 80):
            for y in range(100, 116):
                screen[x, y] = 0x2B
        # The lines, points and rectangles from the reference walkers,
        # then the ellipses from the reference sets.
        screen[220, 40] = 0x5F
        for x, y in vdp_line(204, 40, 216, 40, 0):
            screen[x, y] = 0x6F
        for x, y in fill_ellipse_pixels(128, 50, 20, 20):
            screen[x, y] = 0x1F
        for x, y in outline_ellipse_pixels(128, 62, 24, 24):
            screen[x, y] = 0x2F
        for x, y in fill_ellipse_pixels(190, 110, 24, 24):
            screen[x, y] = 0x1E
        for x, y in outline_ellipse_pixels(190, 180, 40, 12):
            screen[x, y] = 0x3F
        for x, y in outline_ellipse_pixels(60, 180, 6, 40):
            screen[x, y] = 0x4F
        # A COPY span is an HMMV in GRAPHIC 7: no ellipse colour runs
        # through the per-dot LMMV.
        for entry in [c for c in z.vdp.commands[:g7_end]
                      if c.get("code") == 0x80]:
            self.assertNotIn(entry["logical_fill"][4],
                             (0x1F, 0x2F, 0x1E, 0x3F, 0x4F),
                             "a G7 COPY ellipse span ran as LMMV")
        for y in range(212):
            for x in range(width):
                self.assertEqual(peer.vram[y * 256 + x],
                                 screen.get((x, y), 0x00),
                                 "G7 pixel %u,%u" % (x, y))
        # The XOR over the aligned fill, byte for byte: the XOR pair
        # restored the rectangle.  0x2B ^ 0x3C twice.
        self.assertEqual(peer.vram[100 * 256 + 50:100 * 256 + 80],
                         bytes([0x2B]) * 30, "the XOR fill restored")

        # Key 1 releases the G6 drawing: feed it, then run until the
        # program blocks at key 2 (the draws count goes quiet again,
        # this time after the G6 shapes).
        z.hid.hid_queue.extend(b"x")
        steady = 0
        count = 0
        for _step in range(EXEC_BUDGET * 3):
            self.skip_delay(z)
            self.feed(z)
            z.step()
            now = len(draws())
            if now >= 200 and now == count:
                steady += 1
            else:
                steady = 0
            count = now
            if steady >= 200000:
                break
        else:
            self.fail("the shapes never reached key 2")
        peer = z.vdp
        self.assertEqual(z.vdp.regs[0], 0x0A,
                         "still GRAPHIC 6 at key 2")
        self.assertEqual(z.vdp.regs[1] & 0x40, 0x40,
                         "the display is on at key 2")
        # The odd-x and even-x pixels: the nibble written is the
        # colour's low nibble, the neighbour untouched.
        self.assertEqual(peer.vram[10 * 256 + 15], 0x05,
                         "the line's first byte at odd x")
        self.assertEqual(peer.vram[10 * 256 + 16], 0x55,
                         "the line's packed middle bytes")
        self.assertEqual(peer.vram[12 * 256 + 15], 0x06,
                         "the odd point")
        self.assertEqual(peer.vram[12 * 256 + 16], 0x70,
                         "the even point")
        self.assertEqual(peer.vram[14 * 256 + 15], 0x08,
                         "the odd-x fill's first byte")
        self.assertEqual(peer.vram[14 * 256 + 16], 0x88,
                         "the odd-x fill's second byte")
        self.assertEqual(peer.vram[14 * 256 + 22:14 * 256 + 24],
                         bytes([0x99, 0x99]),
                         "the aligned fill")
        # The rect outline's corners, plotted once.
        for x, y in ((10, 40), (29, 40), (29, 51), (10, 51)):
            self.assertEqual(peer.vram[y * 256 + x // 2]
                             >> (4 if x % 2 == 0 else 0) & 0x0F,
                             0x0A, "corner %u,%u" % (x, y))
        # The G6 ellipses from the reference sets.
        g6_end = len(z.vdp.commands)
        screen6 = {}
        for x, y in vdp_line(31, 10, 40, 10, 0):
            screen6[x, y] = 0x05
        screen6[31, 12] = 0x06
        screen6[32, 12] = 0x07
        for x in range(31, 34):
            for y in range(14, 16):
                screen6[x, y] = 0x08
        for x in range(44, 48):
            for y in range(14, 16):
                screen6[x, y] = 0x09
        for x, y in rect_outline_pixels(10, 40, 20, 12):
            screen6[x, y] = 0x0A
        for x, y in outline_ellipse_pixels(128, 90, 18, 12):
            screen6[x, y] = 0x0C
        for x, y in fill_ellipse_pixels(60, 90, 18, 12):
            screen6[x, y] = 0x0E
        for x, y in fill_ellipse_pixels(200, 90, 14, 20):
            screen6[x, y] = 0x0D
        # A G6 COPY span is an HMMV over its whole bytes (the colour in
        # both nibbles) with an LMMV for an odd end dot: the even-centred
        # 14x20 fill ends every wide span on an even dot.
        g6_cmds = z.vdp.commands[g7_end:g6_end]
        self.assertTrue(any(c.get("code") == 0xC0 and
                            c["block"][12] == 0xDD for c in g6_cmds),
                        "the G6 fill's whole bytes run as HMMV")
        self.assertTrue(any(c.get("code") == 0x80 and
                            c["logical_fill"][4] == 0x0D
                            for c in g6_cmds),
                        "the G6 fill's odd end dots run as LMMV")
        for y in range(0, 112):
            for x in range(0, 216):
                value = peer.vram[y * 256 + x // 2]
                nibble = (value >> 4) if x % 2 == 0 else (value & 0x0F)
                self.assertEqual(nibble, screen6.get((x, y), 0x00),
                                 "G6 nibble %u,%u" % (x, y))

        # Key 2 releases the refusals and key 3 the release: feed both
        # (the refusals queue nothing, so the draws stay quiet), then
        # the console back.
        z.hid.hid_queue.extend(b"x")
        z.hid.hid_queue.extend(b"x")
        self.run_recording(z, ops, lambda z: "gfxtest: shapes PASS"
                           in "".join(z.glyphs[since:]))
        fresh = self.trace(z, since)
        self.assertIn("gfxtest: shapes -2 -2 -2 -2 -2 -2 -2 -2 -2",
                      fresh)
        self.assertIn("gfxtest: shapes PASS", fresh)
        self.assertNotIn("FAIL", fresh)
        self.run_to(z, SHELL_PROMPT, since=since)
        self.settle(z, lambda z: z.vdp.regs[8] == 0x08)
        self.assert_hold_sequence(ops)
        self.assert_restored(z, before)
        self.assertEqual(self.exclusive(z), 0,
                         "the exclusive flag survived the run")


class TestGfxTiming(GfxReplay):
    """`gfxtest timing`: the CPU T-state budgets between the markers,
    counted on the peer's register-write timestamps, with the frame
    clock and the duration model on throughout.  Each figure is the
    window's CPU time with the engine and retrace waits taken out (the
    status-register polls' run, from the peer's status log), the
    figure the targets are set on."""

    def test_timing_budgets(self):
        z = self.start()
        ops = []
        since = len(z.glyphs)
        z.hid.hid_queue.extend(b"/fs/gfxtest.exe timing\r")
        self.run_recording(z, ops, lambda z: OPS_ACQUIRE in ops)
        peer = z.vdp
        peer.frame_period = FRAME_TSTATES
        peer.duration_model = True

        def markers():
            return [now for value, reg, now in peer.reg_writes
                    if reg == 16 and value == 15]
        # The boot console and the program's own mode setup each write
        # the G6 palette through R#16 (values 0..15), so their entry-15
        # writes lead the list.
        z.hid.hid_queue.extend(b"x")
        self.run_recording(
            z, ops, lambda z: "gfxtest: timing" in "".join(z.glyphs[since:]))
        marks = markers()[2:]
        self.assertGreaterEqual(len(marks), 24, marks)
        pairs = list(zip(marks[::2], marks[1::2]))
        reads = [now for now, _ce, _vr in peer.status_reads]

        def figure(index):
            """(total, waits) of the index'th window: a wait is a run
            of back-to-back status polls (the CE and VR loops)."""
            start, end = pairs[index]
            waits = 0
            inside = [t for t in reads if start <= t < end]
            for before, after in zip(inside, inside[1:]):
                if after - before <= 48:
                    waits += after - before
            return end - start, waits
        names = ("glyph", "fill", "line", "filled", "outline",
                 "polyline", "row-new", "row-cached", "stripe", "page",
                 "vblank", "present")
        cpu = {}
        for index, name in enumerate(names):
            total, waits = figure(index)
            cpu[name] = total - waits
            print("gfx timing: %-10s cpu %8d wait %8d total %8d"
                  % (name, total - waits, waits, total))
        # The ceilings are the measured CPU figures + 10%, regression
        # guards; each message names its target.
        for name, ceiling, target in (
                ("fill", PIN_FILL, "1,200"),
                ("line", PIN_LINE, "1,500"),
                ("filled", PIN_FILLED, "150,000"),
                ("outline", PIN_OUTLINE, "250,000"),
                ("polyline", PIN_POLYLINE, "200,000"),
                ("row-cached", PIN_ROW_CACHED, "90,000"),
                ("row-new", PIN_ROW_NEW, "100,000"),
                ("stripe", PIN_FILL, "1,200"),
                ("present", PIN_PRESENT, "report")):
            self.assertLessEqual(cpu[name], ceiling,
                                 "%s CPU %d (target %s)"
                                 % (name, cpu[name], target))
        present = pairs[11][1] - pairs[11][0]
        # The page switch: its R#2 write landed inside the retrace,
        # and the switch back put page 0 on display.
        _write_at, value, clock = peer.page2_writes[-1]
        self.assertEqual(value, 0x1F, "R#2 back to page zero")
        self.assertEqual(peer.shown_page, 0)
        self.assertGreaterEqual(clock, FRAME_TSTATES - RETRACE_TSTATES,
                                "the switch back landed in the retrace")
        # The partial present's VDP time (the duration model) and the
        # CPU's overlap: the copy keeps the engine busy long after
        # the call returns.
        copy = [c for c in peer.commands if c["code"] == 0xD0][-1]
        self.assertEqual((copy["block"][0] | (copy["block"][1] << 8),
                          copy["block"][2] | (copy["block"][3] << 8),
                          copy["block"][4] | (copy["block"][5] << 8),
                          copy["block"][6] | (copy["block"][7] << 8)),
                         (0, 456, 0, 102), "the copy's rectangle")
        self.assertEqual(copy["block"][8] | (copy["block"][9] << 8), 128)
        self.assertEqual(copy["block"][10] | (copy["block"][11] << 8),
                         40)
        vdp_time = copy["busy_until"] - copy["start"]
        print("gfx timing: present VDP %d T-states, CPU overlap %d"
              % (vdp_time, max(vdp_time - present, 0)))
        self.assertGreater(vdp_time, present,
                           "the copy outlasts its CPU call")
        self.run_to(z, SHELL_PROMPT, since=since)
        self.settle(z, lambda z: z.vdp.regs[8] == 0x08)
        fresh = self.trace(z, since)
        self.assertIn("gfxtest: timing PASS", fresh)
        self.assert_hold_sequence(ops)


# The timing ceilings: the measured CPU figures (engine waits out) +
# 10%.
PIN_FILL = 2050
PIN_LINE = 1860
PIN_FILLED = 301300
PIN_OUTLINE = 346100
PIN_POLYLINE = 288700
PIN_ROW_CACHED = 83300
PIN_ROW_NEW = 85400
PIN_PRESENT = 4800
PIN_ATLAS_ROW = 83200
PIN_ATLAS_G6S = 13500
PIN_ATLAS_BUILD = 80800
PIN_ATLAS_TRANS = 9100
PIN_ATLAS_G7 = 14400


# 60 Hz at the emulated 10 MHz CPU; the retrace is the frame's last
# 50 of its 262 lines.
FRAME_TSTATES = 166667
RETRACE_TSTATES = (FRAME_TSTATES * 50 + 131) // 262


class TestGfxPages(GfxReplay):
    """`gfxtest pages`: the asynchronous batches, the page switch at
    vertical blank, the frame pacing and the partial present, with the
    peer's frame clock and duration model on."""

    def test_async_pages_and_partial_present(self):
        z = self.start()
        before = self.snapshot(z)
        ops = []
        since = len(z.glyphs)
        z.hid.hid_queue.extend(b"/fs/gfxtest.exe pages\r")
        self.run_recording(z, ops, lambda z: OPS_ACQUIRE in ops)
        self.run_recording(z, ops, lambda z: OPS_DIRECT_INFO in ops)
        # The hold owns the display (IE0 off): turn the frame clock and
        # the per-command duration model on from here.
        z.vdp.frame_period = FRAME_TSTATES
        z.vdp.duration_model = True
        t0 = z.tstates
        self.assertEqual(z.vdp.frames, 0)

        def draws():
            return [c for c in z.vdp.commands[c0:]
                    if c["code"] != 0x00]

        c0 = len(z.vdp.commands)

        def to_key(count, label):
            """Run until the program blocks at a getchar: the draws
            count reaches `count` and then goes quiet."""
            steady = 0
            seen = 0
            for _step in range(EXEC_BUDGET * 3):
                self.skip_delay(z)
                self.feed(z)
                z.step()
                now = len(draws())
                if now >= count and now == seen:
                    steady += 1
                else:
                    steady = 0
                seen = now
                if steady >= 200000:
                    return
            self.fail("the pages run never reached %s" % label)

        # Key 1: the async order - the fill and the 32-fill batch
        # return with the engine still drawing, and the upload over
        # the fill's rectangle waited for it (its bytes win).
        to_key(34, "key 1")
        self.assertNotIn(OPS_RELEASE, ops,
                         "the pages run still holds the display")
        self.assertEqual(z.vdp.regs[0], 0x0A, "GRAPHIC 6 at key 1")
        self.assertEqual(z.vdp.regs[1] & 0x40, 0x40,
                         "the display is on at key 1")
        peer = z.vdp
        cmds = draws()
        self.assertEqual(len(cmds), 34, [c["fill"] for c in cmds])
        self.assertEqual(cmds[0]["fill"], (0, 0, 512, 212, 0x00),
                         "the black clear")
        self.assertEqual(cmds[1]["fill"], (8, 10, 64, 8, 0xAA),
                         "the fill the upload overwrites")
        for i in range(31):
            color = (i & 0x07) + 1
            self.assertEqual(cmds[2 + i]["fill"],
                             (8 + i * 6, 40, 2, 2,
                              color | (color << 4)),
                             "batch fill %u" % i)
        self.assertEqual(cmds[33]["fill"], (194, 40, 32, 8, 0xBB),
                         "the batch's last, large fill")
        # The marks bracket the batch: the second, right after gfx_end,
        # was reached while the last fill's CE was still set (the batch
        # returned asynchronously), and the pair prices the CPU's
        # queue-and-return time.
        marks = [now for value, reg, now in peer.reg_writes
                 if reg == 16 and value == 15]
        mark1, mark2 = marks[-2:]
        self.assertGreater(cmds[33]["busy_until"], mark2,
                           "the batch returned with CE still set")
        print("gfx pages: 32-fill batch CPU time %d T-states"
              % (mark2 - mark1))
        # The upload's first VRAM access waited for the fill: a CE-busy
        # status read between the fill's start and the address set.
        fill_start = cmds[1]["start"]
        first_addr = [now for now, addr in peer.addr_writes
                      if now >= fill_start][0]
        self.assertTrue(
            any(fill_start <= now < first_addr and ce
                for now, ce, _vr in peer.status_reads),
            "the upload waited for the fill")
        # The upload's bytes won, row by row.
        pattern = bytes(upload_byte(i) for i in range(256))
        for row in range(10, 18):
            self.assertEqual(peer.vram[row * 256 + 4:row * 256 + 36],
                             pattern[(row - 10) * 32:
                                     (row - 9) * 32],
                             "upload row %u" % row)
        for i in range(31):
            color = (i & 0x07) + 1
            byte = color | (color << 4)
            for row in (40, 41):
                self.assertEqual(peer.vram[row * 256 + 4 + 3 * i],
                                 byte, "batch fill %u row %u" % (i, row))
        for row in range(40, 48):
            self.assertEqual(peer.vram[row * 256 + 97:row * 256 + 113],
                             bytes([0xBB]) * 16,
                             "the large fill row %u" % row)

        # Key 2: page 1's picture and the page switch.  The R#2 write
        # lands inside a retrace window and the shown page reads 1.
        z.hid.hid_queue.extend(b"x")
        to_key(37, "key 2")
        self.assertEqual(z.vdp.regs[1] & 0x40, 0x40,
                         "the display is on at key 2")
        cmds = draws()
        self.assertEqual(cmds[34]["fill"], (0, 0, 512, 212, 0xCC),
                         "page 0's clear")
        self.assertEqual(cmds[35]["fill"], (0, 256, 512, 212, 0xDD),
                         "page 1's clear")
        self.assertEqual(cmds[36]["fill"], (0, 256, 512, 70, 0xEE),
                         "page 1's band")
        self.assertEqual(peer.shown_page, 1)
        _write_at, value, clock = peer.page2_writes[-1]
        self.assertEqual(value, 0x3F, "R#2 with the page-1 bit")
        self.assertGreaterEqual(clock, FRAME_TSTATES - RETRACE_TSTATES,
                                "the page switch landed in the retrace")
        for row in range(70):
            self.assertEqual(peer.vram[256 * 256 + row * 256:
                                       256 * 256 + (row + 1) * 256],
                             bytes([0xEE]) * 256, "band row %u" % row)
        for row in range(70, 212):
            self.assertEqual(peer.vram[256 * 256 + row * 256:
                                       256 * 256 + (row + 1) * 256],
                             bytes([0xDD]) * 256, "page-1 row %u" % row)

        # Key 3: the three wait_vblank calls returned in three frames
        # (one retrace per frame).
        z.hid.hid_queue.extend(b"x")
        to_key(37, "key 3")
        self.assertEqual(z.vdp.regs[1] & 0x40, 0x40,
                         "the display is on at key 3")
        marks = [now for value, reg, now in peer.reg_writes
                 if reg == 16 and value == 15][-2:]
        reads = [(now, vr) for now, _ce, vr in peer.status_reads
                 if marks[0] <= now <= marks[1]]
        edges = 0
        prev = reads[0][1]
        for _now, vr in reads[1:]:
            if vr and not prev:
                edges += 1
            prev = vr
        self.assertEqual(edges, 3, "one retrace edge per wait_vblank")
        self.assertGreaterEqual(peer.frames, 3, "three frames elapsed")

        # Key 4: the partial present - page 0 shown again, the band
        # copied from page 1 across the blank; page 0's band equals
        # page 1's and the rest of page 0 is untouched.
        z.hid.hid_queue.extend(b"x")
        to_key(38, "key 4")
        self.assertEqual(z.vdp.regs[1] & 0x40, 0x40,
                         "the display is on at key 4")
        cmds = draws()
        block = cmds[37]["block"]
        self.assertEqual((block[0] | (block[1] << 8),
                          block[2] | (block[3] << 8),
                          block[4] | (block[5] << 8),
                          block[6] | (block[7] << 8),
                          block[8] | (block[9] << 8),
                          block[10] | (block[11] << 8),
                          block[13]),
                         (0, 256, 0, 0, 512, 70, 0x00),
                         "the band copy")
        self.assertEqual(peer.shown_page, 0)
        _write_at, value, clock = peer.page2_writes[-1]
        self.assertEqual(value, 0x1F, "R#2 back to page zero")
        self.assertGreaterEqual(clock, FRAME_TSTATES - RETRACE_TSTATES,
                                "the switch back landed in the retrace")
        for row in range(70):
            self.assertEqual(peer.vram[row * 256:(row + 1) * 256],
                             bytes([0xEE]) * 256,
                             "presented band row %u" % row)
        for row in range(70, 212):
            self.assertEqual(peer.vram[row * 256:(row + 1) * 256],
                             bytes([0xCC]) * 256,
                             "page-0 row %u" % row)

        # Key 5: the flip and the tear sweep, model on.  The sweep's
        # own key ends it early: inject it once the h=64 group is
        # under way (750 copies), then the console returns.
        p2_at = len(peer.page2_writes)
        cmd_at = len(peer.commands)
        sweep_copies = [0]
        scan_at = [cmd_at + 3]  # past the flip's three fills

        def copies_so_far():
            while scan_at[0] < len(peer.commands):
                if peer.commands[scan_at[0]]["code"] == 0xD0:
                    sweep_copies[0] += 1
                scan_at[0] += 1
            return sweep_copies[0]

        z.hid.hid_queue.extend(b"x")
        self.run_recording(z, ops, lambda z: copies_so_far() >= 750,
                           limit=EXEC_BUDGET * 3)
        z.hid.hid_queue.extend(b"x")  # the sweep's early key
        self.run_recording(
            z, ops,
            lambda z: SHELL_PROMPT in "".join(z.glyphs[since:]))
        self.settle(z, lambda z: z.vdp.regs[8] == 0x08)
        # The flip: 12 show_page writes alternating the pages, then
        # page 0 back, then the release's console restore (also page
        # 0); the program's writes all land inside a retrace window.
        flips = peer.page2_writes[p2_at:]
        self.assertEqual(len(flips), 14,
                         [value for _now, value, _clock in flips])
        for k, (_now, value, clock) in enumerate(flips[:12]):
            self.assertEqual(value, 0x3F if k % 2 == 0 else 0x1F,
                             "flip %u" % k)
            self.assertGreaterEqual(clock, FRAME_TSTATES - RETRACE_TSTATES,
                                    "flip %u landed in the retrace" % k)
        self.assertEqual([f[1] for f in flips[12:]],
                         [0x1F, 0x1F],
                         "the flip ends on page 0, the release restores")
        # The flip's full-screen colours and page 1's sweep bands.
        fills = peer.commands[cmd_at:cmd_at + 3]
        self.assertEqual([c["fill"] for c in fills],
                         [(0, 0, 512, 212, 0xFF),
                          (0, 256, 512, 212, 0x33),
                          (0, 320, 512, 64, 0xCC)],
                         "the flip's colours")
        # The sweep's copies: six full heights, then the early key
        # lands a few frames into h=64 (the HID poll sees it one
        # frame late); each copy alternates the two source bands.
        # Where the engine keeps up (h <= 16, CE idle each frame),
        # every copy dispatches inside a retrace window.  The model's
        # first tear: the first height whose copy outlasts the
        # retrace (41,975 T at h=4 against the 31,806-T window).
        sweep = [c for c in peer.commands[cmd_at + 3:]
                 if c["code"] == 0xD0]
        self.assertGreaterEqual(len(sweep), 750,
                                "six heights, then the early key")
        self.assertLessEqual(len(sweep), 755,
                             "the key ends the sweep within a frame")
        first_over = None
        for k, c in enumerate(sweep):
            block = c["block"]
            h = block[10] | (block[11] << 8)
            self.assertEqual((block[0] | (block[1] << 8),
                              block[2] | (block[3] << 8),
                              block[4] | (block[5] << 8),
                              block[6] | (block[7] << 8),
                              block[8] | (block[9] << 8),
                              block[10] | (block[11] << 8)),
                             (0, 256 if k % 2 == 0 else 320,
                              0, 0, 512, h),
                             "sweep copy %u" % k)
            # Where the engine keeps up (h <= 8: the copy's 83,913 T
            # at h=8 fits a frame, so each copy starts in the retrace
            # its wait returned from; h=16's copy just outlasts the
            # frame and the chain drifts out of the window).
            if h <= 8:
                phase = (c["start"] - t0) % FRAME_TSTATES
                self.assertGreaterEqual(
                    phase, FRAME_TSTATES - RETRACE_TSTATES - 2000,
                    "copy %u starts inside the retrace" % k)
            if c["busy_until"] - c["start"] > RETRACE_TSTATES \
                    and first_over is None:
                first_over = h
        for h in (1, 2, 4, 8, 16, 32):
            self.assertEqual(sum(1 for c in sweep
                                 if (c["block"][10] |
                                     (c["block"][11] << 8)) == h), 120,
                             "h=%u ran its full two seconds" % h)
        self.assertEqual(sum(1 for c in sweep
                             if (c["block"][10] |
                                 (c["block"][11] << 8)) == 64),
                         len(sweep) - 720, "the h=64 tail")
        self.assertEqual(first_over, 4,
                         "the model's first tear is at h=4")
        print("gfx pages: %d sweep copies, the model's first split "
              "at h=%d" % (len(sweep), first_over))
        fresh = self.trace(z, since)
        self.assertIn("gfxtest: pages 0 -2 1", fresh)
        self.assertIn("gfxtest: pages PASS", fresh)
        self.assertNotIn("FAIL", fresh)
        self.assert_hold_sequence(ops)
        self.assert_restored(z, before)
        self.assertEqual(self.exclusive(z), 0,
                         "the exclusive flag survived the run")


class TestGfxText(GfxReplay):
    """`gfxtest text`: the expansion and the opaque sink in both modes,
    two colour pairs, the right-edge clip, the redefined glyph and its
    reset, the odd-x and YJK refusals."""

    def test_pairs_clip_glyph_and_refusals(self):
        font = load_font()
        z = self.start()
        before = self.snapshot(z)
        ops = []
        marks = []
        since = len(z.glyphs)
        z.hid.hid_queue.extend(b"/fs/gfxtest.exe text\r")

        def held(z):
            if OPS_ACQUIRE in ops and not marks:
                marks.append(len(z.vdp.commands))
            if OPS_RELEASE in ops and len(marks) == 1:
                marks.append(len(z.vdp.commands))
            return OPS_RELEASE in ops
        # The release: every draw is done, the console not redrawn yet.
        self.run_recording(z, ops, held)
        # Text is data-port traffic only: inside the hold the peer
        # sees no command with a drawing code (the mode changes write
        # the stop command, code 0; the console's own commands start
        # after the release).
        self.assertEqual([c["code"] for c in z.vdp.commands[marks[0]:
                                                            marks[1]]],
                         [0x00] * (marks[1] - marks[0]),
                         z.vdp.commands[marks[0]:marks[1]])
        # G6, two pairs: every glyph byte against the pinned font.
        self.assert_text(z, font, "GFX", 8, 20, 0x01, 0x02, True,
                         "g6 pair1")
        self.assert_text(z, font, "text", 8, 30, 0x03, 0x04, True,
                         "g6 pair2")
        # The right-edge clip: x 506 fits exactly one glyph, so "ABCD"
        # shows only the A.
        self.assert_text(z, font, "A", 506, 40, 0x01, 0x02, True,
                         "the clip")
        # The redefined glyph: a full 6x8 block (every pair byte the
        # fg-fg value 0x11), then the built-in's glyph for 0x80.
        for row in range(8):
            base = (50 + row) * 256 + 4
            self.assertEqual(z.vdp.vram[base:base + 3],
                             bytes([0x11]) * 3, "block row %u" % row)
        self.assert_text(z, font, "\x80", 8, 60, 0x01, 0x02, True,
                         "the reset")
        # G7, one byte per pixel.
        self.assert_text(z, font, "g7", 100, 80, 0x1C, 0x00, False,
                         "g7")

        self.run_recording(z, ops, lambda z:
                           SHELL_PROMPT in "".join(z.glyphs[since:]))
        self.settle(z, lambda z: z.vdp.regs[8] == 0x08)
        fresh = self.trace(z, since)
        self.assertIn("gfxtest: text -2 0 0 -7", fresh)
        self.assertIn("gfxtest: text PASS", fresh)
        self.assertNotIn("FAIL", fresh)
        self.assert_hold_sequence(ops)
        self.assert_restored(z, before)
        self.assertEqual(self.exclusive(z), 0,
                         "the exclusive flag survived the run")


class TestGfxAtlas(GfxReplay):
    """`gfxtest atlas`: the opaque 4x6 font, the transparent text over
    stripes with the atlas builds, the LRU slot reuse, the stale glyph
    redraw, the refusals and the out-of-range count."""

    # The G6 band (y 212, 42 lines, 6-line slots) after the nine
    # colours: colours 8 and 9 rebuilt the two least recently used
    # slots (1 and 2's).  The G7 band (36 lines, 12-line slots) after
    # four colours and the two redraws: 4 in slot 0, 1 in slot 1
    # (rebuilt over 2), 2 in slot 2 (rebuilt over 3).
    G6_SLOT_COLOUR = {0: 8, 1: 9, 2: 3, 3: 4, 4: 5, 5: 6, 6: 7}
    G7_SLOT_COLOUR = {0: 4, 1: 1, 2: 2}

    def stripe_colour(self, x):
        """The stripe behind dot x: 8-dot bars alternating 0x0A and
        0x0B from x 0."""
        return 0x0A if (x // 8) % 2 == 0 else 0x0B

    def glyph_over(self, font4, code, row, fg, g6, x0):
        """One 4x6 glyph row drawn transparently over the stripes at
        x0: foreground pixels take `fg`, the rest keep the stripe."""
        byte = font4[code * 6 + row]
        if g6:
            out = []
            for j in range(2):
                hi = fg if byte & (0x80 >> 2 * j) \
                    else self.stripe_colour(x0 + 2 * j)
                lo = fg if byte & (0x80 >> (2 * j + 1)) \
                    else self.stripe_colour(x0 + 2 * j + 1)
                out.append((hi << 4) | lo)
            return bytes(out)
        return bytes(fg if byte & (0x80 >> j)
                     else self.stripe_colour(x0 + j) for j in range(4))

    def cell4(self, font4, code, row, fg, g6):
        """One 4x6 cell row on colour 0 (the atlas): two GRAPHIC 6
        bytes or four GRAPHIC 7 bytes."""
        byte = font4[code * 6 + row]
        if g6:
            pair = g6_pair(fg, 0)
            return bytes((pair[(byte >> 6) & 3], pair[(byte >> 4) & 3]))
        return bytes(fg if byte & (0x80 >> j) else 0 for j in range(4))

    def assert_text4(self, z, font4, text, x, y, fg, bg, g6, where):
        """Opaque 4x6 text decoded from the peer's VRAM against the
        pinned asset."""
        peer = z.vdp
        for index, code in enumerate(text.encode("latin-1")):
            for row in range(6):
                got = g6_text_row4(font4, code, row, fg, bg) if g6 \
                    else g7_text_row4(font4, code, row, fg, bg)
                col = (x + index * 4) // 2 if g6 else x + index * 4
                self.assertEqual(
                    peer.vram[(y + row) * 256 + col:
                              (y + row) * 256 + col + len(got)], got,
                    "%s: '%s' glyph %u row %u"
                    % (where, text, index, row))

    def assert_text_over(self, z, font4, text, x, y, fg, g6, where):
        """Transparent 4x6 text: the glyph's pixels over the stripes,
        decoded dot by dot."""
        peer = z.vdp
        for index, code in enumerate(text.encode("latin-1")):
            x0 = x + index * 4
            for row in range(6):
                got = self.glyph_over(font4, code, row, fg, g6, x0)
                col = x0 // 2 if g6 else x0
                self.assertEqual(
                    peer.vram[(y + row) * 256 + col:
                              (y + row) * 256 + col + len(got)], got,
                    "%s: '%s' glyph %u row %u"
                    % (where, text, index, row))

    def assert_band(self, z, font4, g6, slot_colour):
        """Every glyph cell of every occupied atlas slot, decoded
        against the RAM font (colour 0 background).  Only the cell
        area is compared: the band's remaining bytes keep the boot
        console's remnants."""
        peer = z.vdp
        slots = 7 if g6 else 3
        for slot in range(slots):
            colour = slot_colour[slot]
            base = 212 + (6 if g6 else 12) * slot
            rows = 6 if g6 else 12
            for row in range(rows):
                expected = bytearray()
                if g6:
                    for code in range(32, 127):
                        expected += self.cell4(font4, code, row,
                                               colour, True)
                else:
                    first = 32 if row < 6 else 96
                    for code in range(first, min(first + 64, 127)):
                        expected += self.cell4(font4, code, row % 6,
                                               colour, False)
                line = peer.vram[(base + row) * 256:
                                 (base + row) * 256 + len(expected)]
                self.assertEqual(bytes(line), bytes(expected),
                                 "%s slot %u row %u" %
                                 ("G6" if g6 else "G7", slot, row))

    def test_atlas_mode(self):
        font4 = load_font4()
        z = self.start()
        before = self.snapshot(z)
        ops = []
        since = len(z.glyphs)
        z.hid.hid_queue.extend(b"/fs/gfxtest.exe atlas\r")
        self.run_recording(z, ops, lambda z: OPS_ACQUIRE in ops)
        self.run_recording(z, ops, lambda z: OPS_DIRECT_INFO in ops)
        peer = z.vdp
        c0 = len(peer.commands)

        def draws():
            return [c for c in peer.commands[c0:] if c["code"] != 0x00]

        def to_count(count, label):
            steady = 0
            seen = 0
            for _step in range(EXEC_BUDGET * 3):
                self.skip_delay(z)
                self.feed(z)
                z.step()
                now = len(draws())
                if now >= count and now == seen:
                    steady += 1
                else:
                    steady = 0
                seen = now
                if steady >= 200000:
                    return
            self.fail("the atlas run never reached %s" % label)

        def to_draw(count, label):
            """Step until exactly `count` draws dispatched (the states
            between the keys are transient: the program never parks
            there, so no quiet window)."""
            for _step in range(EXEC_BUDGET):
                self.skip_delay(z)
                self.feed(z)
                z.step()
                if len(draws()) >= count:
                    self.assertEqual(len(draws()), count, label)
                    return
            self.fail("the atlas run never reached %s" % label)

        # -- key 1: the whole G6 stage --------------------------------
        to_count(143, "key 1")
        self.assertNotIn(OPS_RELEASE, ops,
                         "the atlas run still holds the display")
        self.assertEqual(peer.regs[0], 0x0A, "GRAPHIC 6 at key 1")
        self.assertEqual(peer.regs[1] & 0x40, 0x40,
                         "the display is on at key 1")
        cmds = draws()
        self.assertEqual(len(cmds), 143, [c["code"] for c in cmds])
        self.assertEqual(cmds[0]["fill"], (0, 0, 512, 212, 0x00),
                         "the clear")
        for i in range(64):
            self.assertEqual(cmds[1 + i]["fill"],
                             (i * 8, 96, 8, 96,
                              (0x0A if i % 2 == 0 else 0x0B) * 0x11),
                             "stripe %u" % i)
        # The 78 glyph copies: colours 1..9 into the seven slots (8
        # and 9 rebuilt the LRU slots), then the two in-range glyphs
        # of the out-of-range string and the four-glyph cost run.
        expected = []
        slot_of = {1: 0, 2: 1, 3: 2, 4: 3, 5: 4, 6: 5, 7: 6, 8: 0, 9: 1}
        for colour in range(1, 10):
            x = 8 + 48 * (colour - 1)
            sy = 212 + 6 * slot_of[colour]
            for k in range(8):
                sx = (33 + k) * 4
                if sx < x + 4 * k:
                    expected.append((sx + 3, sy, x + 4 * k + 3, 104,
                                     4, 6, 8, 4))
                else:
                    expected.append((sx, sy, x + 4 * k, 104, 4, 6, 8,
                                     0))
        for k in range(2):
            x = 304 + 4 * k
            sx = (33 + k) * 4
            expected.append((sx + 3, 218, x + 3, 116, 4, 6, 8, 4))
        for k in range(4):
            x = 300 + 4 * k
            sx = (33 + k) * 4
            expected.append((sx + 3, 218, x + 3, 128, 4, 6, 8, 4))
        self.assertEqual([c["logical_copy"] for c in cmds[65:]],
                         expected, "the G6 glyph copies")
        # The opaque 4x6 text: two pairs and the right-edge clip
        # (x 504 holds exactly two glyphs).
        self.assert_text4(z, font4, "4x6", 0, 10, 0x01, 0x02, True,
                          "g6 pair1")
        self.assert_text4(z, font4, "pair two", 0, 20, 0x03, 0x04,
                          True, "g6 pair2")
        self.assert_text4(z, font4, "AB", 504, 30, 0x01, 0x02, True,
                          "the clip")
        for row in range(30, 36):
            self.assertEqual(peer.vram[row * 256:(row + 1) * 256 - 4],
                             bytes(256 - 4), "clip row %u left" % row)
        # The full 128-glyph row: 512 dots, every column, in the
        # cached pair.
        for row in range(70, 76):
            got = peer.vram[row * 256:(row + 1) * 256]
            exp = b"".join(
                g6_text_row4(font4, ord("M" if i % 2 == 0 else "W"),
                             row - 70, 0x01, 0x02)
                for i in range(128))
            self.assertEqual(got, exp, "128-row %u" % row)
        # The stripes behind the text rows.
        stripe_row = b"".join(bytes([0x0A * 0x11 if i % 2 == 0
                                     else 0x0B * 0x11]) * 4
                              for i in range(64))
        for row in range(96, 192):
            if row in range(104, 110) or row in range(116, 122) or \
                    row in range(128, 134):
                continue
            self.assertEqual(peer.vram[row * 256:(row + 1) * 256],
                             stripe_row, "stripe row %u" % row)
        # The transparent strings in nine colours; the out-of-range
        # string's two glyphs; the cost run.
        for colour in range(1, 10):
            self.assert_text_over(z, font4, "ABCDEFGH",
                                  8 + 48 * (colour - 1), 104, colour,
                                  True, "g6 fg%d" % colour)
        self.assert_text_over(z, font4, "AB", 304, 116, 9, True,
                              "the in-range pair")
        for row in range(116, 122):
            line = peer.vram[row * 256:(row + 1) * 256]
            self.assertEqual(line[150:152], bytes([0xBB]) * 2,
                             "the 0x01 slot row %u" % row)
            self.assertEqual(line[156:158], bytes([0xBB]) * 2,
                             "the 0x7F slot row %u" % row)
        self.assert_text_over(z, font4, "ABCD", 300, 128, 9, True,
                              "the cost run")
        # The atlas band: every cell of every occupied slot, nothing
        # beyond byte 190.
        self.assert_band(z, font4, True, self.G6_SLOT_COLOUR)

        # -- the stale glyph ------------------------------------------
        z.hid.hid_queue.extend(b"x")
        to_draw(144, "the block draw")
        cmds = draws()
        self.assertEqual(cmds[143]["logical_copy"],
                         (135, 236, 303, 140, 4, 6, 8, 4),
                         "the block copy (DIX)")
        for row in range(140, 146):
            line = peer.vram[row * 256:(row + 1) * 256]
            self.assertEqual(line[150:152], bytes([0x55]) * 2,
                             "the block row %u" % row)
            self.assertEqual(line[148:150], bytes([0xBB]) * 2,
                             "the stripe left of the block row %u" % row)
            self.assertEqual(line[152:154], bytes([0xAA]) * 2,
                             "the stripe right of the block row %u"
                             % row)
        # Exactly the A cells of the seven slots were redrawn.
        for slot in range(7):
            colour = self.G6_SLOT_COLOUR[slot]
            for row in range(6):
                expected = bytearray()
                for code in range(32, 127):
                    byt = self.cell4(font4, code, row, colour, True)
                    if code == 0x41:
                        byt = bytes([colour * 0x11]) * 2
                    expected += byt
                line = peer.vram[(212 + 6 * slot + row) * 256:
                                 (212 + 6 * slot + row) * 256 +
                                 len(expected)]
                self.assertEqual(bytes(line), bytes(expected),
                                 "the stale band slot %u row %u"
                                 % (slot, row))
        to_draw(145, "the reset draw")
        cmds = draws()
        self.assertEqual(cmds[144]["logical_copy"],
                         (135, 236, 303, 150, 4, 6, 8, 4),
                         "the reset copy (DIX)")
        self.assert_text_over(z, font4, "A", 300, 150, 5, True,
                              "the reset A")
        self.assert_band(z, font4, True, self.G6_SLOT_COLOUR)
        to_draw(146, "the DIY draw")
        cmds = draws()
        self.assertEqual(cmds[145]["logical_copy"],
                         (196, 241, 8, 505, 4, 6, 8, 8),
                         "the DIY copy")
        # The Q lands on page 1's spare lines, rows reversed by DIY.
        # Row 504 carries the console's cursor pattern (0xE0 pairs):
        # the transparency leaves that background, so the copy merges
        # over it; the other spare rows were untouched.
        for row in range(500, 504):
            got = peer.vram[row * 256:(row + 1) * 256]
            self.assertEqual(got[:4], b"\x00" * 4,
                             "Q row %u left" % row)
            self.assertEqual(got[6:], bytes(250),
                             "Q row %u right" % row)
        for step, row in enumerate(range(500, 506)):
            cell = self.cell4(font4, ord("Q"), step, 5, True)
            prev = bytes([0xE0]) * 2 if row == 504 else bytes(2)
            merged = bytearray(2)
            for j in range(2):
                hi = cell[j] >> 4
                lo = cell[j] & 0x0F
                merged[j] = (((hi if hi else prev[j] >> 4) << 4) |
                              (lo if lo else prev[j] & 0x0F))
            self.assertEqual(
                peer.vram[row * 256 + 4:row * 256 + 6], bytes(merged),
                "Q copy row %u" % row)

        # -- key 2: the G7 stage, then the YJK refusals ----------------
        to_count(231, "key 2")
        self.assertEqual(peer.regs[0], 0x0E, "GRAPHIC 7 at key 2")
        self.assertEqual(peer.regs[1] & 0x40, 0x40,
                         "the display is on at key 2")
        cmds = draws()
        self.assertEqual(len(cmds), 231)
        self.assertEqual(cmds[146]["fill"], (0, 0, 256, 212, 0x00),
                         "the G7 clear")
        for i in range(32):
            self.assertEqual(cmds[147 + i]["fill"],
                             (i * 8, 96, 8, 96, 0x0A if i % 2 == 0
                              else 0x0B),
                             "G7 stripe %u" % i)
        # The G7 glyph copies: four colours into three slots (colour 4
        # rebuilt slot 0), then the redraws rebuilding slots 1 and 2,
        # then the cost run.
        sequence = ((1, 212, 8, 104), (2, 224, 56, 104),
                    (3, 236, 104, 104), (4, 212, 152, 104),
                    (1, 224, 8, 116), (2, 236, 56, 116))
        expected = []
        for _colour, sy, x, y in sequence:
            for k in range(8):
                sx = (33 + k) * 4
                if sx < x + 4 * k:
                    expected.append((sx + 3, sy, x + 4 * k + 3, y, 4,
                                     6, 8, 4))
                else:
                    expected.append((sx, sy, x + 4 * k, y, 4, 6, 8, 0))
        for k in range(4):
            expected.append(((33 + k) * 4, 236, 8 + 4 * k, 128, 4, 6,
                             8, 0))
        self.assertEqual([c["logical_copy"] for c in cmds[179:]],
                         expected, "the G7 glyph copies")
        self.assert_text4(z, font4, "g7 4x6", 100, 10, 0x1C, 0x00,
                          False, "g7 opaque")
        stripe_row = b"".join(bytes([0x0A]) * 8 if i % 2 == 0
                              else bytes([0x0B]) * 8 for i in range(32))
        for row in range(96, 192):
            if row in range(104, 110) or row in range(116, 122) or \
                    row in range(128, 134):
                continue
            self.assertEqual(peer.vram[row * 256:(row + 1) * 256],
                             stripe_row, "G7 stripe row %u" % row)
        for colour in range(1, 5):
            self.assert_text_over(z, font4, "ABCDEFGH",
                                  8 + 48 * (colour - 1), 104, colour,
                                  False, "g7 fg%d" % colour)
        self.assert_text_over(z, font4, "ABCDEFGH", 8, 116, 1, False,
                              "g7 fg1 redraw")
        self.assert_text_over(z, font4, "ABCDEFGH", 56, 116, 2, False,
                              "g7 fg2 redraw")
        self.assert_text_over(z, font4, "ABCD", 8, 128, 2, False,
                              "the G7 cost run")
        self.assert_band(z, font4, False, self.G7_SLOT_COLOUR)

        # -- the marks, the budgets and the close ---------------------
        marks = [now for _value, reg, now in peer.reg_writes
                 if reg == 16 and _value == 15][2:]
        self.assertGreaterEqual(len(marks), 14, marks)
        pairs = list(zip(marks[::2], marks[1::2]))
        reads = [now for now, _ce, _vr in peer.status_reads]

        def cpu(start, end):
            """The window's CPU T-states with the engine waits (runs of
            back-to-back status polls) taken out."""
            inside = [t for t in reads if start <= t < end]
            return (end - start) - sum(
                after - before for before, after in zip(inside, inside[1:])
                if after - before <= 48)
        row128, g6s, build, trans4, stale, g7opaque, g7trans = \
            (cpu(a, b) for a, b in pairs)
        print("gfx atlas cpu: 128-row %d g6s %d build %d transparent4 %d "
              "stale %d g7opaque %d g7transparent4 %d"
              % (row128, g6s, build, trans4, stale, g7opaque,
                 g7trans))
        # The ceilings are the measured CPU figures + 10%, regression
        # guards; each message names its target.
        for name, figure, ceiling, target in (
                ("the full 128-glyph 4x6 row", row128, PIN_ATLAS_ROW,
                 "90,000"),
                ("opaque 6x8 G6, 8 glyphs", g6s, PIN_ATLAS_G6S, "8,000"),
                ("the 95-glyph atlas build", build, PIN_ATLAS_BUILD,
                 "150,000"),
                ("transparent G6, 4 glyphs", trans4, PIN_ATLAS_TRANS,
                 "4,000"),
                ("opaque 4x6 G7, 6 glyphs", g7opaque, PIN_ATLAS_G7,
                 "6,000"),
                ("transparent G7, 4 glyphs", g7trans, PIN_ATLAS_TRANS,
                 "4,000")):
            self.assertLessEqual(figure, ceiling, "%s CPU %d (target %s)"
                                 % (name, figure, target))

        z.hid.hid_queue.extend(b"x")
        self.run_recording(z, ops, lambda z:
                           SHELL_PROMPT in "".join(z.glyphs[since:]))
        self.settle(z, lambda z: z.vdp.regs[8] == 0x08)
        fresh = self.trace(z, since)
        self.assertIn("gfxtest: atlas -2 -2 -2 -2 2 0 -2 -7 -7", fresh)
        self.assertIn("gfxtest: atlas PASS", fresh)
        self.assertNotIn("FAIL", fresh)
        self.assert_hold_sequence(ops)
        self.assert_restored(z, before)
        self.assertEqual(self.exclusive(z), 0,
                         "the exclusive flag survived the run")


class TestGfxDemo(GfxReplay):
    """`gfxdemo`: the GFX v1 completion demo on the peer - fills,
    copies, two text pairs, the application glyph - and the console
    restored after the key."""

    def setUp(self):
        super().setUp()
        if not os.path.exists(GFXDEMO_EXE):
            self.skipTest("%s missing; run make -C UserSoftware/GfxDemo"
                          % GFXDEMO_EXE)

    def test_demo_draws_and_restores(self):
        font = load_font()
        with open(GFXDEMO_EXE, "rb") as handle:
            files = {"/".join(KEEP_NAME): KEEP_CONTENT,
                     GFXDEMO_DEV: handle.read()}
        z = self.start(files)
        before = self.snapshot(z)
        ops = []
        since = len(z.glyphs)
        marks = []
        seen_off = []
        z.hid.hid_queue.extend(b"/fs/gfxdemo.exe\r")

        def shown(z):
            if OPS_ACQUIRE not in ops:
                return False
            if not marks:
                marks.append(len(z.vdp.commands))
            if z.vdp.regs[1] == 0x00:
                seen_off.append(True)
            return bool(seen_off) and z.vdp.regs[1] == 0x40
        self.run_recording(z, ops, shown)
        peer = z.vdp
        draws = [c for c in peer.commands[marks[0]:]
                 if c["code"] != 0x00]
        # The demo's whole picture is six commands: the two text pairs
        # and the two glyph runs are data-port traffic only.
        self.assertEqual(len(draws), 6,
                         [c.get("fill", c["code"]) for c in draws])
        # The clear, the two bars, the filled rectangle...
        self.assertEqual([c["fill"] for c in draws[:4]],
                         [(0, 0, 512, 212, 0x44),
                          (0, 0, 512, 20, 0x55),
                          (0, 192, 512, 20, 0x55),
                          (20, 40, 40, 8, 0x99)],
                         [c.get("fill") for c in draws])
        # ...and its two copies (HMMM, DIX and DIY from the overlap,
        # every far edge in the blocks).
        self.assertEqual([c["code"] for c in draws[4:6]], [0xD0, 0xD0])
        for c, dx, dy in zip(draws[4:6], (80, 140), (60, 80)):
            block = c["block"]
            self.assertEqual((block[0] | (block[1] << 8),
                              block[2] | (block[3] << 8),
                              block[4] | (block[5] << 8),
                              block[6] | (block[7] << 8)),
                             (59, 47, dx + 39, dy + 7))
            self.assertEqual(block[8] | (block[9] << 8), 40)
            self.assertEqual(block[10] | (block[11] << 8), 8)
            self.assertEqual(block[13], 0x0C)
        # The bars and the clear's colour.
        for row in range(20):
            self.assertEqual(peer.vram[row * 256:(row + 1) * 256],
                             bytes([0x55]) * 256, "top bar row %u" % row)
        for row in range(192, 212):
            self.assertEqual(peer.vram[row * 256:(row + 1) * 256],
                             bytes([0x55]) * 256,
                             "bottom bar row %u" % row)
        for row in range(20, 40):
            self.assertEqual(peer.vram[row * 256:(row + 1) * 256],
                             bytes([0x44]) * 256,
                             "clear row %u" % row)
        # The fill and its copies.
        for row in range(40, 48):
            line = peer.vram[row * 256:(row + 1) * 256]
            self.assertEqual(line[10:30], bytes([0x99]) * 20,
                             "fill row %u" % row)
        for row in range(60, 68):
            self.assertEqual(peer.vram[row * 256 + 40:row * 256 + 60],
                             bytes([0x99]) * 20, "copy 1 row %u" % row)
        for row in range(80, 88):
            self.assertEqual(peer.vram[row * 256 + 70:row * 256 + 90],
                             bytes([0x99]) * 20, "copy 2 row %u" % row)
        # The text in two colour pairs, and the application glyph.
        self.assert_text(z, font, "KonaOS GFX v1", 20, 100, 0x0F, 0x04,
                         True, "demo white")
        self.assert_text(z, font, "text colours", 20, 116, 0x0A, 0x00,
                         True, "demo green")
        face = (0x1E, 0x21, 0x21, 0x21, 0x21, 0x12, 0x0C, 0x00)
        pair = g6_pair(0x0F, 0x04)
        for row in range(8):
            byte = (face[row] << 2) & 0xFC
            expected = bytes(pair[(byte >> shift) & 3]
                             for shift in (6, 4, 2))
            base = (140 + row) * 256 + 10
            self.assertEqual(peer.vram[base:base + 3], expected,
                             "face row %u" % row)
        self.assert_text(z, font, "\x80", 36, 140, 0x0F, 0x04, True,
                         "demo restored glyph")

        # The key: the console is restored.
        z.hid.hid_queue.extend(b"x")
        self.run_recording(z, ops, lambda z:
                           SHELL_PROMPT in "".join(z.glyphs[since:]))
        self.settle(z, lambda z: z.vdp.regs[8] == 0x08)
        fresh = self.trace(z, since)
        self.assertIn("gfxdemo: done", fresh)
        self.assertNotIn("FAIL", fresh)
        self.assert_hold_sequence(ops)
        self.assert_restored(z, before)
        self.assertEqual(self.exclusive(z), 0,
                         "the exclusive flag survived the run")


if __name__ == "__main__":
    unittest.main()
