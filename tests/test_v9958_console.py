#!/usr/bin/env python3
"""Host parser/model tests for the V9958 terminal renderer (work package
19).

Approach: boot the console image in the Z80 simulator to kernel context
before the monitor launcher runs, drive the module's output-only
`zchar_ops` entry points (and the `ZVIDEO_RESET` class operation) with
scripted byte streams, and decode the resulting VDP command trace into a
26x85 screen model: 6x8 glyph copies write their cell, HMMV rectangles
blank their cell range with the fill rendition, full-row HMMM copies
move whole rows (insert/delete lines), and every R#23 write moves the
circular scroll origin.  `struct v9958_state` fields are read from the
module's live state.

The expectations are fixed golden cases derived from the ported
reference behavior: exact cell contents, cursor/state fields, print-run
batching, chunking equivalence, scrolled origins, and command-block
shapes (glyph geometry, full-width fills, the circular fill split).

Skipped when the console image has not been built.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import Z80, IocCommandPeer, V9958Peer, read_symbols  # noqa: E402

RET = 0x1234
STEP_LIMIT = 80000000
STACK = 0xFFFE
HOST_SYNC = b"\xaa\x55"

ZDEV_OK = 0
ZDEV_VIDEO = 2
ZVIDEO_PRESENT = 6
ZVIDEO_RESET = 7

COLS = 85
ROWS = 26
CELL_W = 6
CELL_H = 8
TEXT_TOP = 208  # visible lines in the circular bitmap

CLEAR_FILL = 0x44      # normal background fill value
REVERSE_FILL = 0xFF    # reverse-video fill value

# struct v9958_state byte offsets (driver header order).
STATE_OFF = {
    "scroll_origin": 3,
    "term_state": 4,
    "esc_count": 5,
    "csi_param_count": 7,
    "text_col": 11,
    "text_row": 12,
    "cursor_visible": 13,
    "current_attr": 14,
    "saved_col": 15,
    "saved_row": 16,
    "saved_attr": 17,
    "auto_wrap": 18,
    "out_run_count": 19,
    "out_run_col": 20,
    "out_run_row": 21,
}

TERM_NORMAL = 0
TERM_ESC = 1
TERM_CSI = 2

# low RAM of the selected bank while mode 11 is active (kernel scratch)
SCRATCH = 0x1F00


class Screen:
    """Physical circular-bitmap model: `lines[y]` maps column ->
    (character, reverse-flag) for physical scanline `y` (0-255).

    Glyphs, fills, and row copies land on physical lines exactly like
    the VDP commands; the logical 26x85 view is derived by sampling the
    lines starting at the current R#23 scroll origin, so a scroll only
    moves the origin (as on the hardware)."""

    def __init__(self):
        self.lines = {}
        self.origin = 0

    def clear(self):
        self.lines = {}

    def line(self, y):
        return self.lines.setdefault(y & 0xFF, {})

    def put(self, y, col, ch, rev=0):
        if 0 <= col < COLS:
            self.line(y)[col] = (ch, rev)

    def fill(self, y, col_from, col_to, rev=0):
        line = self.line(y)
        for col in range(col_from, min(col_to, COLS)):
            line[col] = (" ", rev)

    def copy_scanline(self, src, dst):
        self.lines[dst & 0xFF] = dict(self.lines.get(src & 0xFF, {}))

    def text(self, row):
        y = (row * CELL_H + self.origin) & 0xFF
        line = self.lines.get(y, {})
        return "".join(line.get(col, (" ", 0))[0]
                       for col in range(COLS)).rstrip()

    def cell(self, row, col):
        y = (row * CELL_H + self.origin) & 0xFF
        return self.lines.get(y, {}).get(col, (" ", 0))


class RendererBase(unittest.TestCase):
    """Boot to kernel context, drive the renderer, decode commands."""

    IMAGE_SUBDIR = "build-m8-console-selftest"
    IMAGE_NAME = "zephyros-m8-console-selftest.bin"

    def setUp(self):
        self.bin_path = os.path.join(ROOT, self.IMAGE_SUBDIR, self.IMAGE_NAME)
        if not os.path.exists(self.bin_path):
            self.skipTest("%s missing; build the image first" % self.bin_path)
        with open(self.bin_path, "rb") as fh:
            self.image = fh.read()
        self.syms = read_symbols(
            os.path.join(ROOT, self.IMAGE_SUBDIR, "zephyros.map"))
        self.z = None

    # -- machine ---------------------------------------------------------
    def make_cpu(self):
        pages = {n: self.image[n * 0x10000:(n + 1) * 0x10000]
                 for n in range(8)}
        z = Z80(pages[0], pages[1],
                rom_pages={n: pages[n] for n in range(2, 8)})
        z.latch = 0x18
        z.sp = STACK
        z.ioc_peer = IocCommandPeer()
        z.vdp_peer = V9958Peer()
        return z

    def skip_delay(self, z):
        if z.pc == self.syms["_kernel_serial_connect_delay"]:
            z.pc = z.pop16()

    def boot(self):
        """crt0 + kernel_main up to the console selftest line (still in
        kernel context; the monitor launcher has not run)."""
        z = self.make_cpu()
        z.push16(RET)
        z.pc = self.syms["__zos_crt0_init"]
        for _ in range(2000000):
            z.step()
            if z.pc == RET:
                break
        else:
            raise AssertionError("C runtime init did not return")
        z.push16(RET)
        z.sio_rx.extend(HOST_SYNC)
        z.pc = self.syms["_kernel_main"]
        for i in range(STEP_LIMIT):
            self.skip_delay(z)
            z.step()
            if b"m8 console selftest: OK" in z.sio_tx:
                break
        else:
            raise AssertionError("console selftest never completed")

        # Locate the registered renderer and its entry points.
        base = self.syms["_device_table"]
        for index in range(16):
            off = base + index * 16
            if z.bank7[off] and z.bank7[off + 1] == ZDEV_VIDEO:
                break
        else:
            raise AssertionError("v9958.0 is not registered")
        self.char_ops = z.bank7[off + 8] | (z.bank7[off + 9] << 8)
        self.state = z.bank7[off + 12] | (z.bank7[off + 13] << 8)
        self.putc_fn = z.rd16(self.char_ops)
        self.write_fn = z.rd16(self.char_ops + 2)

        # Decoded screen model, live from here on.
        self.screen = Screen()
        self.cpos = len(z.vdp_peer.commands)
        self.rpos = len(z.vdp_peer.reg_writes)
        self.origin = 0
        for value, reg, _ in z.vdp_peer.reg_writes:
            if reg == 23:
                self.origin = (value + 4) & 0xFF
        self.screen.origin = self.origin
        self.z = z
        self.write(z, "\x1b[2J")  # known baseline: empty screen, home
        self.assertEqual(self.state_field("text_col"), 0)
        self.assertEqual(self.state_field("text_row"), 0)
        self.assertEqual(self.screen_text(), [""] * ROWS)
        return z

    # -- command decoding ------------------------------------------------
    def apply_command(self, cmd):
        block = cmd["block"]
        code = cmd["code"]
        nx = block[8] | (block[9] << 8)
        ny = block[10] | (block[11] << 8)
        if (code & 0xF0) == 0xD0 and nx == CELL_W and ny == CELL_H:
            sx = block[0] | (block[1] << 8)
            sy = block[2] | (block[3] << 8)
            dx = block[4] | (block[5] << 8)
            dy = block[6] | (block[7] << 8)
            ch = ((sy & 0x3F) >> 3) * 32 + sx // CELL_W
            rev = 1 if (sy & 0x40) else 0
            if ch >= 32:
                self.screen.put(dy, dx // CELL_W, chr(ch), rev)
        elif (code & 0xF0) == 0xD0 and nx == COLS * CELL_W and ny == CELL_H:
            sy = block[2] | (block[3] << 8)
            dy = block[6] | (block[7] << 8)
            for k in range(CELL_H):
                self.screen.copy_scanline(sy + k, dy + k)
        elif (code & 0xF0) == 0xC0:
            dx, dy, fill_nx, fill_ny, color = cmd["fill"]
            rev = 0 if color == CLEAR_FILL else 1
            for y in range(dy, dy + fill_ny):
                col_to = (dx + fill_nx + CELL_W - 1) // CELL_W
                self.screen.fill(y, dx // CELL_W, col_to, rev)

    def pump(self, z):
        peer = z.vdp_peer
        while True:
            cmd = (peer.commands[self.cpos]
                   if self.cpos < len(peer.commands) else None)
            reg = (peer.reg_writes[self.rpos]
                   if self.rpos < len(peer.reg_writes) else None)
            if cmd is None and reg is None:
                break
            if reg is not None and (cmd is None or reg[2] <= cmd["start"]):
                self.rpos += 1
                value, number, _ = reg
                if number == 23:
                    # The bitmap is circular: only the view moves.
                    self.origin = (value + 4) & 0xFF
                    self.screen.origin = self.origin
                continue
            self.cpos += 1
            self.apply_command(cmd)

    # -- driving the module ---------------------------------------------
    def call(self, z, target, hl=None, de=None, extra=(), limit=STEP_LIMIT):
        """sdcccall(1) call.  `extra` pushes the remaining arguments in
        order; a plain integer is pushed as a 16-bit slot, a `(value,)`
        tuple pushes a single byte slot (value << 8 with the low byte
        dropped), matching generated call sites for this toolchain."""
        sp_before = z.sp
        for value in reversed(extra):
            if isinstance(value, tuple):
                z.push16((value[0] & 0xFF) << 8)
                z.sp = (z.sp + 1) & 0xFFFF
            else:
                z.push16(value)
        z.push16(RET)
        if hl is not None:
            z.h = (hl >> 8) & 0xFF
            z.l = hl & 0xFF
        if de is not None:
            z.d = (de >> 8) & 0xFF
            z.e = de & 0xFF
        z.pc = target
        for _ in range(limit):
            self.skip_delay(z)
            z.step()
            if z.pc == RET:
                z.sp = sp_before
                value = (z.d << 8) | z.e
                return value - 0x10000 if value & 0x8000 else value
        raise AssertionError("call at %04X did not return (PC=%04X)"
                             % (target, z.pc))

    def write(self, z, data):
        if isinstance(data, str):
            data = data.encode("latin-1")
        z.bank0[SCRATCH:SCRATCH + len(data)] = data
        rc = self.call(z, self.write_fn, hl=self.state, de=SCRATCH,
                       extra=[len(data)])
        self.pump(z)
        return rc

    def putc(self, z, ch):
        rc = self.call(z, self.putc_fn, hl=self.state, extra=[(ch,)])
        self.pump(z)
        return rc

    def send_op(self, z, op, payload=0, length=0):
        """Call the device's zvideo_ops.send (located through the
        registry class_ops pointer)."""
        base = self.syms["_device_table"]
        for index in range(16):
            off = base + index * 16
            if z.bank7[off] and z.bank7[off + 1] == ZDEV_VIDEO:
                class_ops = z.bank7[off + 10] | (z.bank7[off + 11] << 8)
        send = z.rd16(class_ops)
        sp_before = z.sp
        z.push16(length)
        z.push16(payload)
        z.push16((op & 0xFF) << 8)
        z.sp = (z.sp + 1) & 0xFFFF
        z.push16(RET)
        z.h = (self.state >> 8) & 0xFF
        z.l = self.state & 0xFF
        z.pc = send
        for _ in range(STEP_LIMIT):
            self.skip_delay(z)
            z.step()
            if z.pc == RET:
                z.sp = sp_before
                value = (z.d << 8) | z.e
                return value - 0x10000 if value & 0x8000 else value
        raise AssertionError("send did not return")

    def reset(self, z):
        rc = self.send_op(z, ZVIDEO_RESET, 0, 0)
        self.assertEqual(rc, ZDEV_OK)
        self.pump(z)

    # -- observation -----------------------------------------------------
    def state_field(self, name):
        return self.z.rd(self.state + STATE_OFF[name])

    def cursor(self):
        return self.state_field("text_col"), self.state_field("text_row")

    def screen_text(self, upto=ROWS):
        return [self.screen.text(row) for row in range(upto)]

    def glyph_commands(self, z, since):
        return [cmd for cmd in z.vdp_peer.commands[since:]
                if (cmd["code"] & 0xF0) == 0xD0 and
                (cmd["block"][8] | (cmd["block"][9] << 8)) == CELL_W and
                (cmd["block"][10] | (cmd["block"][11] << 8)) == CELL_H]

    def fill_commands(self, z, since):
        return [cmd for cmd in z.vdp_peer.commands[since:]
                if (cmd["code"] & 0xF0) == 0xC0]

    def clear(self, z):
        self.write(z, "\x1b[2J\x1b[1;1H\x1b[0m")


class TestPrintableAndControls(RendererBase):
    """Printable placement, CR/LF/VT/BS/TAB/FF, consumed controls."""

    def test_printable_placement_and_cursor_state(self):
        z = self.boot()
        self.assertEqual(self.write(z, "ABC"), 3)
        self.assertEqual(self.screen.text(0), "ABC")
        self.assertEqual(self.cursor(), (3, 0))
        self.assertEqual(self.state_field("term_state"), TERM_NORMAL)
        self.assertEqual(self.state_field("out_run_count"), 0)

    def test_putc_prints_one_byte_visibly(self):
        z = self.boot()
        self.assertEqual(self.putc(z, ord("A")), ZDEV_OK)
        self.assertEqual(self.putc(z, ord("B")), ZDEV_OK)
        self.assertEqual(self.screen.text(0), "AB")
        self.assertEqual(self.cursor(), (2, 0))

    def test_nul_bel_and_controls_are_consumed(self):
        z = self.boot()
        self.write(z, "A\x00\x07\x0e\x0fB\x1bZ")  # NUL/BEL/SO/SI/ESC-Z
        self.assertEqual(self.screen.text(0), "AB")
        self.assertEqual(self.cursor(), (2, 0))

    def test_cr_lf_vt_bs_tab(self):
        z = self.boot()
        self.write(z, "AB\r\nCD")
        self.assertEqual(self.screen.text(0), "AB")
        self.assertEqual(self.screen.text(1), "CD")
        self.assertEqual(self.cursor(), (2, 1))

        self.clear(z)
        self.write(z, "AB\x08C")  # BS moves left, C overwrites B
        self.assertEqual(self.screen.text(0), "AC")
        self.assertEqual(self.cursor(), (2, 0))

        self.clear(z)
        self.write(z, "A\tB")  # tab to the next 8-column stop
        self.assertEqual(self.screen.text(0), "A       B")
        self.assertEqual(self.cursor(), (9, 0))

        self.clear(z)
        self.write(z, "AB\x0bC")  # VT = LF with the column kept
        self.assertEqual(self.screen.text(0), "AB")
        self.assertEqual(self.screen.text(1), "  C")
        self.assertEqual(self.cursor(), (3, 1))

    def test_lf_keeps_column_and_scrolls_at_bottom(self):
        z = self.boot()
        self.write(z, "AB\n")
        self.assertEqual(self.cursor(), (2, 1))
        self.write(z, "CD")
        self.assertEqual(self.screen.text(1), "  CD")

    def test_ff_clears_the_screen_and_homes(self):
        z = self.boot()
        self.write(z, "JUNK\r\nMORE")
        self.write(z, "\x0c")  # FF
        self.assertEqual(self.screen_text(), [""] * ROWS)
        self.assertEqual(self.cursor(), (0, 0))
        self.write(z, "N")
        self.assertEqual(self.screen.text(0), "N")


class TestGeometryAndWrapping(RendererBase):
    """Right-margin policy, DECAWM, batching, boundaries."""

    def test_print_run_batches_and_never_loses_bytes(self):
        z = self.boot()
        for chunk in (63, 64, 65, 128):
            self.clear(z)
            since = len(z.vdp_peer.commands)
            payload = "".join(chr(ord("a") + i % 26) for i in range(chunk))
            self.assertEqual(self.write(z, payload), chunk)
            glyphs = self.glyph_commands(z, since)
            self.assertEqual(len(glyphs), chunk,
                             "one glyph copy per printable byte (%d)" % chunk)
            self.assertEqual(self.cursor(),
                             ((chunk % COLS), chunk // COLS))

    def test_right_margin_wraps_first_and_advances(self):
        z = self.boot()
        since = len(z.vdp_peer.commands)
        self.write(z, "Z" * COLS)
        glyphs = self.glyph_commands(z, since)
        self.assertEqual(len(glyphs), COLS)
        # Sequential destination cells 0..84 on the same line.
        xs = [cmd["block"][4] | (cmd["block"][5] << 8) for cmd in glyphs]
        self.assertEqual(xs, [col * CELL_W for col in range(COLS)])
        ys = {cmd["block"][6] | (cmd["block"][7] << 8) for cmd in glyphs}
        self.assertEqual(len(ys), 1, "the run spans one text row")
        self.assertEqual(self.screen.text(0), "Z" * COLS)
        self.assertEqual(self.cursor(), (0, 1), "wrapped after column 84")
        self.write(z, "W")
        self.assertEqual(self.screen.text(1), "W")

    def test_decawn_off_clamps_and_overwrites(self):
        z = self.boot()
        self.write(z, "\x1b[?7l")
        self.assertEqual(self.state_field("auto_wrap"), 0)
        self.write(z, "A" * (COLS + 5))
        self.assertEqual(self.screen.text(0), "A" * COLS)
        self.assertEqual(self.screen.text(1), "")
        self.assertEqual(self.cursor(), (COLS - 1, 0))
        self.write(z, "\x1b[?7h")
        self.assertEqual(self.state_field("auto_wrap"), 1)
        self.assertIn("A", self.screen.text(0))

    def test_cursor_boundaries_clamp(self):
        z = self.boot()
        self.write(z, "\x1b[9A")   # up at the top: stays
        self.write(z, "\x1b[9D")   # left at column 0: stays
        self.assertEqual(self.cursor(), (0, 0))
        self.write(z, "\x1b[99;99H")
        self.assertEqual(self.cursor(), (COLS - 1, ROWS - 1))
        self.write(z, "\x1b[9B")   # down at the bottom: stays
        self.write(z, "\x1b[9C")   # right at the last column: stays
        self.assertEqual(self.cursor(), (COLS - 1, ROWS - 1))


class TestCsiFamilies(RendererBase):
    """Cursor movement/absolute, erase, SGR, save/restore, IL/DL."""

    def test_cursor_movement_and_absolute_position(self):
        z = self.boot()
        self.write(z, "ABCDEF")
        self.write(z, "\x1b[3D")
        self.assertEqual(self.cursor(), (3, 0))
        self.write(z, "\x1b[2C")
        self.assertEqual(self.cursor(), (5, 0))
        self.write(z, "\x1b[10G")
        self.assertEqual(self.cursor(), (9, 0))
        self.write(z, "\x1b[1;1H")
        self.assertEqual(self.cursor(), (0, 0))
        self.write(z, "\x1b[4;20H")
        self.assertEqual(self.cursor(), (19, 3))
        self.write(z, "\x1b[3d")   # VPA keeps the column
        self.assertEqual(self.cursor(), (19, 2))
        self.write(z, "\x1b[2f")   # HVP is CUP
        self.assertEqual(self.cursor(), (0, 1))
        self.write(z, "\x1b[20;90H")
        self.assertEqual(self.cursor(), (COLS - 1, 19))
        self.write(z, "M")
        self.assertEqual(self.screen.cell(19, COLS - 1)[0], "M")

    def test_erase_in_display(self):
        z = self.boot()
        self.write(z, "11111\r\n22222\r\n33333")
        self.write(z, "\x1b[2;3H\x1b[0J")     # cursor through end of screen
        self.assertEqual(self.screen.text(0), "11111")
        self.assertEqual(self.screen.text(1), "22")
        self.assertEqual(self.screen.text(2), "")
        self.assertEqual(self.cursor(), (2, 1))

        self.clear(z)
        self.write(z, "11111\r\n22222")
        self.write(z, "\x1b[2;3H\x1b[1J")     # start through cursor
        self.assertEqual(self.screen.text(0), "")
        self.assertEqual(self.screen.text(1), "   22")
        self.assertEqual(self.cursor(), (2, 1))

        self.write(z, "\x1b[2J")              # whole screen + home
        self.assertEqual(self.screen_text(), [""] * ROWS)
        self.assertEqual(self.cursor(), (0, 0))

    def test_erase_in_line_and_characters(self):
        z = self.boot()
        self.write(z, "ABCDEF")
        self.write(z, "\x1b[3G\x1b[K")        # EL0: cursor to end
        self.assertEqual(self.screen.text(0), "AB")
        self.assertEqual(self.cursor(), (2, 0))

        self.clear(z)
        self.write(z, "ABCDEF")
        self.write(z, "\x1b[3G\x1b[1K")      # EL1: start through cursor
        self.assertEqual(self.screen.text(0), "   DEF")
        self.assertEqual(self.cursor(), (2, 0))

        self.clear(z)
        self.write(z, "ABCDEF")
        self.write(z, "\x1b[3G\x1b[2K")      # EL2: whole line
        self.assertEqual(self.screen.text(0), "")
        self.assertEqual(self.cursor(), (2, 0))

        self.clear(z)
        self.write(z, "ABCDEF")
        since = len(z.vdp_peer.commands)
        self.write(z, "\x1b[3G\x1b[2X")      # ECH: two cells blanked
        self.assertEqual(self.screen.text(0), "AB  EF")
        self.assertEqual(self.cursor(), (2, 0))
        fills = self.fill_commands(z, since)
        self.assertEqual(len(fills), 1)
        self.assertEqual(fills[0]["fill"][2], 2 * CELL_W)

    def test_sgr_reverse_rendition(self):
        z = self.boot()
        self.write(z, "\x1b[7mRV\x1b[27mN")
        self.assertEqual(self.screen.text(0), "RVN")
        self.assertEqual(self.screen.cell(0, 0)[1], 1)
        self.assertEqual(self.screen.cell(0, 1)[1], 1)
        self.assertEqual(self.screen.cell(0, 2)[1], 0)
        self.assertEqual(self.state_field("current_attr"), 0)

        # Unknown SGR parameters (bold/underline/color) are consumed.
        self.write(z, "\x1b[1;4;31mX")
        self.assertEqual(self.screen.cell(0, 3)[1], 0)
        self.write(z, "\x1b[0m\x1b[7mY\x1b[mZ")
        self.assertEqual(self.screen.cell(0, 4)[1], 1)
        self.assertEqual(self.screen.cell(0, 5)[1], 0)

    def test_reverse_fill_uses_the_reverse_background(self):
        z = self.boot()
        self.write(z, "ABCDEF")
        since = len(z.vdp_peer.commands)
        self.write(z, "\x1b[7m\x1b[3G\x1b[K")  # EL under reverse video
        fills = self.fill_commands(z, since)
        self.assertEqual(len(fills), 1)
        self.assertEqual(fills[0]["fill"][4], REVERSE_FILL)
        self.assertEqual(self.screen.text(0), "AB")
        self.assertEqual(self.screen.cell(0, 2)[1], 1)
        self.write(z, "\x1b[0m\x1b[3G\x1b[K")  # normal background again
        self.assertEqual(self.screen.cell(0, 2)[1], 0)

    def test_save_and_restore_cursor_and_attr(self):
        z = self.boot()
        self.write(z, "AB\x1b7\r\nCD\x1b8E")
        self.assertEqual(self.screen.text(0), "ABE")
        self.assertEqual(self.screen.text(1), "CD")
        self.assertEqual(self.cursor(), (3, 0))

        # CSI s / CSI u with rendition saved.
        self.clear(z)
        self.write(z, "\x1b[7m\x1b[4;9H\x1b[s\x1b[12;40H\x1b[uX")
        self.assertEqual(self.cursor(), (9, 3))  # restored, then printed
        self.assertEqual(self.screen.cell(3, 8)[0], "X")
        self.assertEqual(self.screen.cell(3, 8)[1], 1)
        self.assertEqual(self.state_field("saved_col"), 8)
        self.assertEqual(self.state_field("saved_row"), 3)
        self.assertEqual(self.state_field("saved_attr"), 1)

    def test_insert_and_delete_lines(self):
        z = self.boot()
        self.write(z, "11\r\n22\r\n33")
        self.write(z, "\x1b[1;1H\x1b[L")   # insert one line at row 0
        self.assertEqual(self.screen_text(6)[:5],
                         ["", "11", "22", "33", ""])
        self.write(z, "\x1b[1;1H\x1b[M")   # delete it again
        self.assertEqual(self.screen_text(6)[:5],
                         ["11", "22", "33", "", ""])

        # Two lines at once; the bottom of the region is blanked.
        self.write(z, "\x1b[1;1H\x1b[2L")
        self.assertEqual(self.screen_text(6)[:5],
                         ["", "", "11", "22", "33"])
        # Clamped to the rows below the cursor: nothing moves at the
        # bottom edge and the cursor keeps its row.
        self.write(z, "\x1b[25;1H\x1b[9L")
        self.assertEqual(self.screen_text(6)[:5],
                         ["", "", "11", "22", "33"])
        self.assertEqual(self.cursor(), (0, ROWS - 2))


class TestScrollingAndReset(RendererBase):
    """Circular scroll origin, FF/ESC c/triple-ESC/RESET, cursor SAT."""

    def fill_rows(self, z):
        lines = "\r\n".join("R%02d" % n for n in range(ROWS))
        self.write(z, lines)

    def test_scroll_advances_the_origin_and_keeps_content(self):
        z = self.boot()
        self.fill_rows(z)
        self.assertEqual(self.screen_text(2), ["R00", "R01"])
        origin_before = self.state_field("scroll_origin")
        self.write(z, "\r\nR26")
        self.assertEqual(self.state_field("scroll_origin"),
                         (origin_before + CELL_H) & 0xFF)
        self.assertEqual(self.screen.text(0), "R01")
        self.assertEqual(self.screen.text(ROWS - 1), "R26")
        self.assertEqual(self.cursor(), (3, ROWS - 1))

    def test_full_screen_scroll_in_circular_geometry(self):
        z = self.boot()
        since = len(z.vdp_peer.commands)
        self.write(z, "\r\n" * (ROWS + 4))
        fills = self.fill_commands(z, since)
        # The scroll repaint covers the four margin lines and the new
        # bottom row; the circular split appears when the rectangle
        # crosses physical line 255.
        widths = {(cmd["fill"][2]) for cmd in fills}
        self.assertIn(COLS * CELL_W, widths)

    def test_esc_c_triple_esc_and_reset_restore_defaults(self):
        z = self.boot()
        for script in ("ABC\x1b[7m", "ABC\x1b[?7l", "ABC\x1b[?25l"):
            self.write(z, script)
            self.write(z, "\x1b[2J\x1b[1;1H\x1b[0m" + "\x1b[?7h\x1b[?25h")
            self.write(z, "XY")
            self.assertEqual(self.screen.text(0), "XY")

        # ESC c reinitializes the terminal (RIS).
        self.write(z, "JUNK")
        self.write(z, "\x1b\x1b\x1b")
        self.assertEqual(self.screen_text(), [""] * ROWS)
        self.assertEqual(self.cursor(), (0, 0))
        self.assertEqual(self.state_field("auto_wrap"), 1)
        self.assertEqual(self.state_field("current_attr"), 0)
        self.assertEqual(self.state_field("cursor_visible"), 1)

        # ZVIDEO_RESET re-establishes all driver-owned state.
        self.write(z, "\x1b[7m\x1b[?7l\x1b[?25l\r\n\r\nJUNK")
        self.reset(z)
        self.assertEqual(self.screen_text(), [""] * ROWS)
        self.assertEqual(self.cursor(), (0, 0))
        self.assertEqual(self.state_field("auto_wrap"), 1)
        self.assertEqual(self.state_field("current_attr"), 0)
        self.assertEqual(self.state_field("cursor_visible"), 1)
        self.assertEqual(self.state_field("scroll_origin"), 0)
        since = len(z.vdp_peer.commands)
        self.write(z, "OK")
        self.assertEqual(self.screen.text(0), "OK")
        glyphs = self.glyph_commands(z, since)
        self.assertEqual(len(glyphs), 2)

    def test_cursor_visibility_state(self):
        z = self.boot()
        self.write(z, "\x1b[?25l")
        self.assertEqual(self.state_field("cursor_visible"), 0)
        self.write(z, "\x1b[?25h")
        self.assertEqual(self.state_field("cursor_visible"), 1)
        # ESC[?25h without a parameter, and other DEC private finals,
        # are consumed without disturbing the cursor.
        self.write(z, "\x1b[?12;34h\x1b[?99l")
        self.assertEqual(self.cursor(), (0, 0))


class TestParserRecoveryAndChunking(RendererBase):
    """Unknown/partial sequences, chunking equivalence, command shape."""

    def test_unknown_and_partial_sequences_are_consumed(self):
        z = self.boot()
        self.write(z, "A\x1b[99zB")       # unknown CSI final
        self.assertEqual(self.screen.text(0), "AB")

        self.clear(z)
        self.write(z, "A\x1b[;XBC")       # empty params, unknown final
        self.assertEqual(self.screen.text(0), "ABC")

        self.clear(z)
        self.write(z, "A\x1b#8B")         # ESC # <final>: consumed
        self.assertEqual(self.screen.text(0), "AB")

        self.clear(z)
        self.write(z, "A\x1b(0B\x1b)0C")  # charset designators: consumed
        self.assertEqual(self.screen.text(0), "ABC")

        self.clear(z)
        self.write(z, "A\x1b")            # lone ESC at the end
        self.assertEqual(self.state_field("term_state"), TERM_ESC)
        self.write(z, "B")                # ESC B: consumed final
        self.assertEqual(self.screen.text(0), "A")
        self.assertEqual(self.state_field("term_state"), TERM_NORMAL)

    def test_arbitrary_chunking_matches_one_write(self):
        script = ("HELLO\x1b[7mREV\x1b[27m\r\n"
                  "TAB\tEND\x08!\x1b[3D<>")
        z = self.boot()
        self.write(z, script)
        reference = {y: dict(line)
                     for y, line in self.screen.lines.items()}
        reference_origin = self.screen.origin

        for size in (1, 2, 3, 7, 16, 64):
            self.clear(z)
            data = script.encode("latin-1")
            for start in range(0, len(data), size):
                self.write(z, data[start:start + size])
            self.assertEqual(self.screen.lines, reference,
                             "chunk size %d diverged" % size)
            self.assertEqual(self.screen.origin, reference_origin)

    def test_glyph_command_geometry_is_exact(self):
        z = self.boot()
        since = len(z.vdp_peer.commands)
        self.write(z, "A")
        glyphs = self.glyph_commands(z, since)
        self.assertEqual(len(glyphs), 1)
        block = glyphs[0]["block"]
        sx = block[0] | (block[1] << 8)
        self.assertEqual(block[8] | (block[9] << 8), CELL_W)
        self.assertEqual(block[10] | (block[11] << 8), CELL_H)
        self.assertEqual(sx, (ord("A") & 0x1F) * CELL_W)
        self.assertEqual(block[2], (ord("A") >> 5) * CELL_H)
        self.assertEqual(block[3], 0x01, "normal atlas texture page")
        self.write(z, "\x1b[7mB")
        glyphs = self.glyph_commands(z, since)
        self.assertTrue(glyphs[1]["block"][2] & 0x40,
                        "reverse glyph comes from the 40h page")

    def test_full_width_fill_geometry(self):
        z = self.boot()
        self.write(z, "TEXT")
        since = len(z.vdp_peer.commands)
        self.write(z, "\x1b[2K")
        fills = self.fill_commands(z, since)
        self.assertEqual(len(fills), 1)
        dx, dy, nx, ny, color = fills[0]["fill"]
        self.assertEqual((dx, nx, ny), (0, COLS * CELL_W, CELL_H))
        self.assertEqual(color, CLEAR_FILL)
        self.assertEqual(self.screen.text(0), "")


if __name__ == "__main__":
    unittest.main()
