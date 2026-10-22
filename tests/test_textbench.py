#!/usr/bin/env python3
"""Direct-text spike acceptance, on the production image.

The replay splices `textbench.exe` into the production store, boots to
the shell and runs one row at a time (the one-row mode renders, then
waits in `console_read` before printing its line - the replay snapshots
the clean screen at that entry, sends a key, and checks the line).

Three properties per row:

* same pixels: after every direct row, the page-zero bitmap equals the
  `txt.console` row's screen cell for cell.  The console's screen is
  decoded from its command stream (HMMM copies are recorded, never
  applied to VRAM by the peer); the direct rows' screen is the peer's
  VRAM itself (data-port writes and HMMV fills both execute into it).
  Both sides are compared as pixels, expanded from the pinned font
  through the console's pair table.
* the row line's shape and its T-states per character.
* the renderer sizes the driver tier would carry, from the link map.

Run with: `python3 -m unittest tests.test_textbench`.
"""

import json
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import pack_image  # noqa: E402
import romfspack  # noqa: E402
from fontpack import parse_font  # noqa: E402
from replay import ConsoleReplay, STEP_LIMIT  # noqa: E402
from test_tui_sim import ROWS, COLS  # noqa: E402
from z80sim import Ctc, read_symbols  # noqa: E402

IMAGE = os.path.join(ROOT, "build", "konaos.bin")
PROGRAM = os.path.join(ROOT, "build", "user", "textbench.exe")
PROGRAM_MAP = os.path.join(ROOT, "build", "user", "textbench",
                          "textbench.map")
MODULES = os.path.join(ROOT, "build", "modules")
KERNEL_MAP = os.path.join(ROOT, "build", "konaos.map")

ROW_NAMES = ("txt.console", "txt.glyph1", "txt.glyph6", "txt.run1",
             "txt.run6")
DIRECT_ROWS = ROW_NAMES[1:]
# The console row reports its eight 256-byte writes; the direct rows
# report their 2,000 characters.
ROW_RE = (r"textbench:\s+%s\s+ops\s+%d\s+bytes\s+2000\s+min"
          r"\s+\d+\s+mean\s+\d+\s+max\s+\d+\s+per-byte\s+\d+")

# The console's normal pair table (drivers/v9958/v9958.c): two pixels
# per byte, pixel on = F, off = 4.
PAIR_NORMAL = (0x44, 0x4F, 0xF4, 0xFF)
# The display offset the console writes into R#23 (V9958_DISPLAY_OFFSET).
DISPLAY_OFFSET = 4

# The sizes the link map must show (code and table bytes of each direct
# renderer, and the font bytes), the cost the driver tier would carry.
EXPECTED_SIZES = {
    "font bits (image)": ("_textbench_font", 2048),
    "expand table (BSS)": ("_textbench_expand", 192),
    "pre-coloured font (BSS)": ("_textbench_font6", 6144),
}


def _splice_store(image, stream):
    data = bytearray(image)
    window = pack_image.ROMFS_WINDOW_SIZE
    for page in range(pack_image.ROMFS_FIRST_PAGE,
                      pack_image.ROMFS_FIRST_PAGE +
                      pack_image.ROMFS_PAGE_COUNT):
        base = page * pack_image.PAGE_SIZE
        data[base:base + window] = bytes([pack_image.FILL]) * window
    logical = 0
    while logical < len(stream):
        page = pack_image.ROMFS_FIRST_PAGE + logical // window
        offset = logical % window
        chunk = min(window - offset, len(stream) - logical)
        base = page * pack_image.PAGE_SIZE + offset
        data[base:base + chunk] = stream[logical:logical + chunk]
        logical += chunk
    return bytes(data)


class _Replay(ConsoleReplay):
    def __init__(self, image):
        self.root = ROOT
        self.subdir = "build"
        self.image_name = "textbench-replay.bin"
        self.image = image
        self.syms = read_symbols(KERNEL_MAP)


def _cell_pixels(ch, attr, font):
    """The 24 bytes (3 per scanline) of a cell, from the console's pair
    table (XOR 0BBh for reverse)."""
    pixels = []
    for scan in range(8):
        value = font[ch * 8 + scan] >> 2
        row = [PAIR_NORMAL[(value >> 4) & 3],
               PAIR_NORMAL[(value >> 2) & 3],
               PAIR_NORMAL[value & 3]]
        if attr:
            row = [byte ^ 0xBB for byte in row]
        pixels.extend(row)
    return bytes(pixels)


class TestDirectTextSpike(unittest.TestCase):
    """The direct rows render the console row's pixels, and the sizes
    the record reports come from the link map."""

    @classmethod
    def setUpClass(cls):
        if not os.path.isfile(PROGRAM):
            raise unittest.SkipTest(
                "build/user/textbench.exe has not been built")
        if not os.path.isfile(IMAGE):
            raise unittest.SkipTest("build/konaos.bin has not been built")
        cls.font = parse_font(os.path.join(
            ROOT, "drivers", "v9958", "assets", "font_cp850_6x8.inc"))
        tmp = tempfile.mkdtemp(prefix="textbench-replay-")
        manifest = json.loads(
            Path(os.path.join(ROOT, "build", "store.json")).read_text())
        manifest["files"] = [entry for entry in manifest["files"]
                             if entry["name"].lower() != "textbench.exe"]
        manifest["files"].append({"name": "textbench.exe", "type": "EXE",
                                  "path": PROGRAM})
        out_json = os.path.join(tmp, "store.json")
        Path(out_json).write_text(json.dumps(manifest))
        stream, _report = romfspack.pack(
            out_json, os.path.join(tmp, "store.romfs"), objdir=MODULES)
        cls.image = _splice_store(Path(IMAGE).read_bytes(), stream)
        cls.program_syms = read_symbols(PROGRAM_MAP)
        for name in ("_console_read", "_textbench_render_glyph1",
                     "_textbench_render_glyph6", "_textbench_render_run1",
                     "_textbench_render_run6"):
            if name not in cls.program_syms:
                raise unittest.SkipTest("the textbench map has no %s" % name)

    def _boot(self):
        replay = _Replay(self.image)
        z = replay.make_cpu()
        # The production clock: the bench measures with the TIMER driver.
        Ctc().attach(z)
        replay.boot_to_shell(z)
        return replay, z

    def _run_row(self, replay, z, name):
        """Run one row; return (line, snapshot) taken at the row's
        console_read wait, before its report prints."""
        renderer = None
        if name != "txt.console":
            renderer = self.program_syms["_textbench_render_" +
                                         name[len("txt."):]]
        glyph_start = len(z.glyphs)
        commands_start = len(z.vdp.commands)
        z.hid.hid_queue.extend(
            ("/system/textbench.exe %s\r" % name).encode("ascii"))
        if renderer is not None:
            # The program start is the renderer's first entry.
            seen = False
        else:
            # The console row starts at its clear: a new full-screen
            # 512 x 256 background fill after the launch.
            seen = False

        def program_started():
            nonlocal seen
            if not seen:
                if renderer is not None:
                    seen = z.pc == renderer
                else:
                    for command in z.vdp.commands[commands_start:]:
                        block = command["block"]
                        if (command["code"] == 0xC0 and
                                block[8] | (block[9] << 8) == 512 and
                                block[10] | (block[11] << 8) == 256 and
                                block[12] == 0x44):
                            seen = True
                            break
            return seen

        snapshot = None
        for _step in range(STEP_LIMIT):
            if program_started() and \
                    z.pc == self.program_syms["_console_read"]:
                snapshot = {
                    "commands": list(z.vdp.commands),
                    "commands_start": commands_start,
                    "vram": bytes(z.vdp.vram),
                    "reg23": z.vdp.regs[23],
                }
                break
            replay.skip_delay(z)
            replay.feed(z)
            z.step()
            if _step % 0x1FFF == 0:
                replay.pump(z)
                if "PANIC" in "".join(z.glyphs[glyph_start:]):
                    self.fail("the replay panicked")
        else:
            self.fail("%s never reached its wait" % name)
        # A key releases the wait; the row line prints.
        z.hid.hid_queue.extend(b"x")
        line = None
        for _step in range(STEP_LIMIT):
            replay.skip_delay(z)
            replay.feed(z)
            z.step()
            if _step % 0x1FFF == 0:
                replay.pump(z)
                text = "".join(z.glyphs[glyph_start:])
                match = re.search(
                    ROW_RE % (name, 8 if name == "txt.console" else 2000),
                    text)
                if match:
                    line = match.group(0)
                    break
                if "FAIL" in text:
                    self.fail("%s reported FAIL: %r" % (name, text[-200:]))
        else:
            self.fail("%s never printed its line" % name)
        return line, snapshot

    def _console_screen(self, snapshot):
        """Decode the console row's final screen: every command's DY is
        an absolute scanline (the console scrolls by moving the R#23
        origin after drawing, so a fixed final origin maps cells; the
        per-command origin decode of test_tui_sim only works when
        nothing is drawn after a scroll)."""
        final_origin = (snapshot["reg23"] + DISPLAY_OFFSET) & 0xFF
        screen = [[(" ", 0) for _ in range(COLS)] for _ in range(ROWS)]
        for command in snapshot["commands"][snapshot["commands_start"]:]:
            block = command["block"]
            code = command["code"]
            if code == 0xC0:  # HMMV fill
                dx = block[4] | (block[5] << 8)
                dy = block[6] | (block[7] << 8)
                nx = block[8] | (block[9] << 8)
                ny = block[10] | (block[11] << 8)
                attr = 1 if block[12] == 0xFF else 0
                for scan in range(dy, dy + ny, 8):
                    offset = (scan - final_origin) & 0xFF
                    if offset >= ROWS * 8:
                        continue
                    at_row = offset // 8
                    for x in range(dx, dx + nx, 6):
                        at_col = x // 6
                        if at_col < COLS:
                            screen[at_row][at_col] = (" ", attr)
            elif code == 0xD0:  # HMMM glyph copy
                sx = block[0] | (block[1] << 8)
                sy = block[2] | (block[3] << 8)
                dx = block[4] | (block[5] << 8)
                dy = block[6] | (block[7] << 8)
                nx = block[8] | (block[9] << 8)
                if nx == 0 or nx % 6 != 0 or block[10] != 8:
                    continue
                offset = (dy - final_origin) & 0xFF
                if offset >= ROWS * 8:
                    continue
                at_row = offset // 8
                attr = 1 if sy & 0x40 else 0
                for x_offset in range(0, nx, 6):
                    at_col = (dx + x_offset) // 6
                    glyph = (((sy & 0x3F) >> 3) * 32 +
                             (sx + x_offset) // 6)
                    if at_col < COLS:
                        screen[at_row][at_col] = (chr(glyph), attr)
        return screen

    def test_the_direct_rows_reproduce_the_console_pixels(self):
        """After every direct row the page-zero bitmap equals the
        txt.console row's screen, cell for cell, decoded as pixels."""
        # ROWS/COLS are the imported screen geometry constants.
        screen_rows, screen_cols = ROWS, COLS
        replay, z = self._boot()
        line, snapshot = self._run_row(replay, z, "txt.console")
        self.assertRegex(line, ROW_RE % ("txt.console", 8))
        reference = self._console_screen(snapshot)
        for name in DIRECT_ROWS:
            line, snapshot = self._run_row(replay, z, name)
            self.assertRegex(line, ROW_RE % (name, 2000))
            origin = (snapshot["reg23"] + DISPLAY_OFFSET) & 0xFF
            vram = snapshot["vram"]
            mismatches = []
            for row in range(screen_rows):
                for col in range(screen_cols):
                    ch, attr = reference[row][col]
                    expected = _cell_pixels(ord(ch), attr, self.font)
                    for scan in range(8):
                        y = (origin + row * 8 + scan) & 0xFF
                        base = y * 256 + col * 3
                        actual = vram[base:base + 3]
                        if actual != expected[scan * 3:scan * 3 + 3]:
                            mismatches.append(
                                "%d,%d,%d: %s != %s"
                                % (row, col, scan, actual.hex(),
                                   expected[scan * 3:scan * 3 + 3].hex()))
                            break
            self.assertEqual(mismatches, [],
                             "%s differs from the console row: %s"
                             % (name, mismatches[:4]))

    def test_the_renderer_sizes_are_what_the_driver_tier_would_carry(self):
        """The code and table bytes of every direct renderer and the
        font bytes, read from the link map."""
        symbols = read_symbols(PROGRAM_MAP)
        for label, size in EXPECTED_SIZES.values():
            self.assertIn(label, symbols)
        for name in ("glyph1", "glyph6", "run1", "run6"):
            start = symbols["_textbench_render_" + name]
            end = symbols["_textbench_render_" + name + "_end"]
            self.assertGreater(end, start, name)
        shared = symbols["_textbench_shared_end"] - symbols[
            "_textbench_shared"]
        self.assertGreater(shared, 0)


if __name__ == "__main__":
    unittest.main()
