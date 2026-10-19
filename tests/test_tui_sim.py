#!/usr/bin/env python3
"""TUI screen-engine replay tests.

`tui_test.exe` runs on both console backends: the production V9958/HID image
and `build-console-serial-selftest`.  The tests decode the visible screen from
each backend and compare it with the same logical model:

* the batch mode draws one 85-column row through 85 `tui_putc` calls.  The
  serial byte trace must carry one cursor position and one 85-byte run, and
  the console driver's WRITE dispatches between the program's init and its
  PASS marker must be exactly the row run, the marker cursor sync, the PASS
  line and shutdown.  The unchanged redraw between the row and the marker
  emits nothing.
* the screen mode covers cursor placement, full and clipped rows, control
  bytes, everything outside the right and bottom edges, nested transactions,
  an explicit flush inside a transaction, cursor visibility and formatting.

Skipped when the program or an image has not been built.
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
from replay import ConsoleReplay, SHELL_PROMPT, STEP_LIMIT  # noqa: E402
from z80sim import IocCommandPeer, read_symbols  # noqa: E402

TUI_TEST_EXE = os.path.join(ROOT, "build", "user", "tui_test.exe")
TUI_DEMO_EXE = os.path.join(ROOT, "build", "user", "tui_demo.exe")
TUI_KEYS_EXE = os.path.join(ROOT, "build", "user", "tui_keys.exe")
TUI_DIALOG_EXE = os.path.join(ROOT, "build", "user", "tui_dialog.exe")
PRODUCTION = ("build", "konaos.bin")
SERIAL = ("build-console-serial-selftest",
          "konaos-console-serial-selftest.bin")

# The lifecycle byte lengths and the exact batch-mode program output.  The
# init write is 22 bytes (SGR 0, clear, home, wrap off, cursor show).
TUI_INIT = b"\x1b[0m\x1b[2J\x1b[H\x1b[?7l\x1b[?25h"
TUI_SHUTDOWN = b"\x1b[0m\x1b[?7h\x1b[?25h"
TUI_INIT_LENGTH = len(TUI_INIT)
BATCH_COMMAND = b"/system/tui_test.exe batch\r\n"
BATCH_PASS = b"tui: batch pass\r\n"
SCREEN_PASS = "tui: screen pass"

ROWS = 26
COLS = 85
SCREEN_ROWS = 21

FORMAT_ROW = "str Z -1234 54321 ABCD -123456 123456 BEEF 00042   ab %"
FORMAT_ROW_XY = "0012|ok|!|-7"


def _blank_screen():
    return [[(" ", 0) for _ in range(COLS)] for _ in range(ROWS)]


def _put(screen, row, col, text):
    for offset, ch in enumerate(text):
        if col + offset < COLS:
            screen[row][col + offset] = (ch, 0)


def expected_screen():
    """The model the screen mode must produce."""
    screen = _blank_screen()
    row = ""
    ch = "A"
    for _ in range(COLS):
        row += ch
        ch = chr(ord(ch) + 1)
        if ch > "Z":
            ch = "A"
    _put(screen, 0, 0, row)
    _put(screen, 1, 0, "#" * COLS)
    _put(screen, 2, 0, ("0123456789" * 9)[:COLS])
    _put(screen, 3, 83, "XY")
    screen[4][0] = ("D", 0)
    screen[4][10] = ("A", 0)
    screen[4][11] = ("B", 0)
    screen[4][16] = ("C", 0)
    screen[5][1] = ("E", 0)
    _put(screen, 6, 0, "ABCDE")
    screen[7][0] = ("X", 0)
    screen[7][1] = ("Y", 0)
    screen[7][10] = ("P", 0)
    _put(screen, 8, 0, FORMAT_ROW)
    _put(screen, 9, 0, FORMAT_ROW_XY)
    screen[10][0] = ("Q", 0)
    _put(screen, 11, 0, "inner1")
    _put(screen, 12, 0, "inner2")
    _put(screen, 13, 0, "explicit")
    screen[14][0] = ("Z", 0)
    screen[15][0] = ("B", 0)
    screen[16][84] = ("T", 0)
    screen[17][2] = ("I", 0)
    screen[25][0] = ("W", 0)
    screen[25][83] = ("X", 0)
    screen[25][84] = ("Y", 0)
    return screen


def batch_row():
    text = ""
    ch = "A"
    for _ in range(COLS):
        text += ch
        ch = chr(ord(ch) + 1)
        if ch > "Z":
            ch = "A"
    return text


def shutdown_screen():
    """The cells the shutdown mode draws without an explicit flush."""
    screen = _blank_screen()
    row = ""
    ch = "A"
    for _ in range(COLS):
        row += ch
        ch = chr(ord(ch) + 1)
        if ch > "Z":
            ch = "A"
    _put(screen, 0, 0, row)
    _put(screen, 1, 0, "dirty two")
    _put(screen, 2, 5, "xy")
    return screen



def windows_screen():
    screen = _blank_screen()
    _put(screen, 0, 0, "END")
    _put(screen, 1, 0, "S1")
    _put(screen, 2, 2, "ABCDEFGHIJ")
    _put(screen, 2, 12, "R2")
    _put(screen, 3, 2, "KLMNOPQRXY")
    _put(screen, 4, 2, "UVINYZ0123")
    _put(screen, 7, 0, "S7")
    return screen


def _put_attr(screen, row, col, text, attr):
    for offset, ch in enumerate(text):
        if col + offset < COLS:
            screen[row][col + offset] = (ch, attr)


def _frame(screen, x, y, w, h, style, attr):
    glyphs = {
        "single": ("\u00c4", "\u00b3", "\u00da", "\u00bf",
                   "\u00c0", "\u00d9"),
        "double": ("\u00cd", "\u00ba", "\u00c9", "\u00bb",
                   "\u00c8", "\u00bc"),
    }[style]
    hz, vt, tl, tr, bl, br = glyphs
    for column in range(x + 1, x + w - 1):
        _put_attr(screen, y, column, hz, attr)
        _put_attr(screen, y + h - 1, column, hz, attr)
    for row in range(y + 1, y + h - 1):
        _put_attr(screen, row, x, vt, attr)
        _put_attr(screen, row, x + w - 1, vt, attr)
    _put_attr(screen, y, x, tl, attr)
    if w > 1:
        _put_attr(screen, y, x + w - 1, tr, attr)
        _put_attr(screen, y + h - 1, x, bl, attr)
        _put_attr(screen, y + h - 1, x + w - 1, br, attr)


def styles_screen():
    screen = _blank_screen()
    _frame(screen, 2, 1, 20, 7, "single", 1)
    _put_attr(screen, 1, 3, " ", 1)
    _put_attr(screen, 1, 4, "Single", 1)
    _put_attr(screen, 1, 20, " ", 1)
    _frame(screen, 30, 1, 20, 7, "double", 1)
    _put_attr(screen, 1, 31, " ", 1)
    _put_attr(screen, 1, 32, "Double", 1)
    _put_attr(screen, 1, 48, " ", 1)
    _put_attr(screen, 1, 61, " ", 1)
    _put_attr(screen, 1, 62, "LongTi", 1)
    _put_attr(screen, 1, 68, " ", 1)

    _put_attr(screen, 10, 1, "\u00da", 0)
    _put_attr(screen, 10, 3, "\u00da\u00bf", 0)
    _put_attr(screen, 10, 6, "\u00c9", 0)
    _put_attr(screen, 10, 8, "\u00da\u00c4\u00bf", 0)
    _put_attr(screen, 10, 70, "\u00da", 0)
    _put_attr(screen, 11, 3, "\u00c0\u00d9", 0)
    _put_attr(screen, 11, 6, "\u00ba", 0)

    for row in (12, 13):
        _put_attr(screen, row, 2, "#" * 10, 0)
    _put_attr(screen, 15, 2, "\u00c4" * 10, 0)
    _put_attr(screen, 15, 80, "\u00da", 0)
    _put_attr(screen, 15, 82, "TIT", 0)
    for row in range(16, 19):
        _put_attr(screen, row, 2, "\u00ba", 0)
        _put_attr(screen, row, 80, "\u00b3", 0)
    _put_attr(screen, 19, 2, "\u00ba", 0)
    _put_attr(screen, 19, 20, "SENT", 0)
    _put_attr(screen, 19, 80, "\u00c0\u00c4\u00c4\u00c4\u00c4", 0)
    _put_attr(screen, 20, 2, "ATTR", 1)

    for col in range(COLS):
        screen[21][col] = (" ", 1)
    _put_attr(screen, 21, 0, "STATUS", 1)
    _put_attr(screen, 21, 7, "normal", 0)
    _put_attr(screen, 21, 14, "SELECTED", 1)
    return screen


def region_screen():
    screen = _blank_screen()
    for row in range(5, 13):
        _put_attr(screen, row, 10, "." * 20, 0)
    _put_attr(screen, 6, 12, "KEEP", 1)
    _put_attr(screen, 9, 20, "TEXT", 0)
    for row in range(12, 16):
        _put_attr(screen, row, 22, "N" * 10, 0)
    return screen


def widgets_screen():
    screen = _blank_screen()
    _put_attr(screen, 0, 0, "File", 1)
    _put_attr(screen, 0, 5, "Edit Help", 0)
    _put_attr(screen, 2, 2, "Name:", 0)
    _put_attr(screen, 2, 10, "dam", 0)
    _put_attr(screen, 4, 2, "[X] Enabled", 0)
    _frame(screen, 2, 6, 20, 8, "single", 0)
    for col in range(3, 21):
        screen[7][col] = (" ", 1)
    _put_attr(screen, 7, 3, "Red", 1)
    _put_attr(screen, 8, 3, "Green", 0)
    _put_attr(screen, 9, 3, "Blue", 0)
    _put_attr(screen, 15, 2, "[ OK ]", 1)
    _put_attr(screen, 15, 12, "[ Cancel ]", 0)
    _put_attr(screen, 18, 10, "ab", 0)
    _put_attr(screen, 24, 60, "L" * 25, 0)
    for col in range(COLS):
        screen[25][col] = (" ", 1)
    _put_attr(screen, 25, 0, "KonaOS Config", 1)
    return screen


KEYS_INPUT = (
    b"A"
    b"\x1b[A\x1b[B\x1b[C\x1b[D\x1b[H\x1b[F"
    b"\x1b[2~\x1b[3~\x1b[5~\x1b[6~"
    b"\x1bOP\x1bOQ\x1bOR\x1bOS"
    b"\x1b[15~\x1b[17~\x1b[18~\x1b[19~"
    b"\x1b[20~\x1b[21~\x1b[23~\x1b[24~"
    b"\x00"
    b"\x1b[Z"
    b"\x1b[1A"
    b"\x1b[12345678~Y"
    b"\x1bOX"
    b"\x1bX"
)

KEYS_EXPECTED = [
    0x0041, 0x0101, 0x0102, 0x0104, 0x0103, 0x0110, 0x0111, 0x0114,
    0x0115, 0x0112, 0x0113, 0x0121, 0x0122, 0x0123, 0x0124, 0x0125,
    0x0126, 0x0127, 0x0128, 0x0129, 0x012A, 0x012B, 0x012C, 0x001B,
    0x005B, 0x005A, 0x001B, 0x005B, 0x0031, 0x0041,
    0x001B, 0x005B, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036,
    0x0037, 0x0038, 0x007E, 0x0059,
    0x001B, 0x004F, 0x0058,
    0x001B, 0x0058,
]

WIDGET_KEYS = (
    b"\x1b[C\x1b[C\r"
    b"f"
    b"\x1b"
    b"\x1b[Fm\r"
    b"\x1b[H\x1b[3~\r"
    b"\x1b"
    b"\x1b[F\x1b[H\x1b[B\x1b[A\x1b[6~\x1b[5~\r"
    b"\x1b[C\r"
    b"\x1b"
    b"cd\r"
)

def parse_serial_screen(data):
    """Decode the serial lane's ANSI/VT100 bytes into an 85x26 text map."""
    screen = _blank_screen()
    row = 0
    col = 0
    attr = 0
    wrap = True
    index = 0
    while index < len(data):
        byte = data[index]
        if byte == 0x1B and index + 1 < len(data) and data[index + 1] == 0x5B:
            at = index + 2
            private = at < len(data) and data[at] == 0x3F
            if private:
                at += 1
            start = at
            while at < len(data) and not 0x40 <= data[at] <= 0x7E:
                at += 1
            if at >= len(data):
                break
            text = data[start:at].decode("ascii", "ignore")
            final = chr(data[at])
            numbers = [int(part) if part else 0
                       for part in text.split(";")] if text else []
            if final in "Hf" and not private:
                row = (numbers[0] if len(numbers) > 0 and numbers[0] else 1)
                col = (numbers[1] if len(numbers) > 1 and numbers[1] else 1)
                row = max(0, min(ROWS - 1, row - 1))
                col = max(0, min(COLS - 1, col - 1))
            elif final == "J" and not private:
                if numbers and numbers[0] == 2:
                    screen = _blank_screen()
            elif final == "m" and not private:
                for number in numbers or [0]:
                    if number in (0, 27):
                        attr = 0
                    elif number == 7:
                        attr = 1
            elif final == "l" and private and numbers and numbers[0] == 7:
                wrap = False
            elif final == "h" and private and numbers and numbers[0] == 7:
                wrap = True
            index = at + 1
            continue
        if byte == 0x1B:
            index += 2
            continue
        if byte == 0x0D:
            col = 0
        elif byte == 0x0A:
            row = min(row + 1, ROWS - 1)
        elif byte == 0x08:
            col = max(col - 1, 0)
        elif byte >= 0x20:
            if row < ROWS and col < COLS:
                screen[row][col] = (chr(byte), attr)
            if col < COLS - 1:
                col += 1
            elif wrap:
                col = 0
                row = min(row + 1, ROWS - 1)
        index += 1
    return screen


def decode_v9958_screen(replay, z, command_start):
    """Decode the glyph and fill commands into the same 85x26 text map."""
    screen = _blank_screen()
    commands = z.vdp.commands[command_start:]
    if not commands:
        return screen
    registers = sorted((now, value) for value, register, now
                       in z.vdp.reg_writes if register == 23)
    origin = 0
    register_index = 0
    for command in commands:
        while (register_index < len(registers) and
               registers[register_index][0] <= command["start"]):
            origin = (registers[register_index][1] + 4) & 0xFF
            register_index += 1
        block = command["block"]
        if command["code"] == 0xC0:  # HMMV fill
            dx = block[4] | (block[5] << 8)
            dy = block[6] | (block[7] << 8)
            nx = block[8] | (block[9] << 8)
            ny = block[10] | (block[11] << 8)
            attr = 1 if block[12] == 0xFF else 0
            for scan in range(dy, dy + ny):
                offset = (scan - origin) & 0xFF
                if offset >= ROWS * 8 or offset % 8 != 0:
                    continue
                at_row = offset // 8
                for x in range(dx, dx + nx, 6):
                    at_col = x // 6
                    if at_col < COLS:
                        screen[at_row][at_col] = (" ", attr)
        elif command["code"] == 0xD0:  # HMMM copy
            sx = block[0] | (block[1] << 8)
            sy = block[2] | (block[3] << 8)
            dx = block[4] | (block[5] << 8)
            dy = block[6] | (block[7] << 8)
            nx = block[8] | (block[9] << 8)
            ny = block[10] | (block[11] << 8)
            if nx != 6 or ny != 8:
                continue
            offset = (dy - origin) & 0xFF
            if offset >= ROWS * 8 or offset % 8 != 0:
                continue
            at_row = offset // 8
            at_col = dx // 6
            glyph = ((sy & 0x3F) >> 3) * 32 + sx // 6
            attr = 1 if sy & 0x0040 else 0
            if at_col < COLS:
                screen[at_row][at_col] = (chr(glyph), attr)
    return screen


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
    def __init__(self, image, subdir):
        self.root = ROOT
        self.subdir = subdir
        self.image_name = "tui-replay.bin"
        self.image = image
        self.syms = read_symbols(os.path.join(ROOT, subdir, "konaos.map"))


class TuiSimBase(unittest.TestCase):
    """Shared store packing, boot and WRITE-dispatch accounting."""

    SUBDIR = None
    IMAGE_NAME = None
    SERIAL = False

    @classmethod
    def setUpClass(cls):
        if not os.path.isfile(TUI_TEST_EXE):
            raise unittest.SkipTest(
                "build/user/tui_test.exe has not been built")
        cls.image_path = os.path.join(ROOT, cls.SUBDIR, cls.IMAGE_NAME)
        for path in (cls.image_path,
                     os.path.join(ROOT, cls.SUBDIR, "konaos.map"),
                     os.path.join(ROOT, cls.SUBDIR, "store.json"),
                     os.path.join(ROOT, cls.SUBDIR, "modules",
                                  "drivers.plan.json")):
            if not os.path.isfile(path):
                raise unittest.SkipTest("%s has not been built" % path)
        cls.tmp = tempfile.mkdtemp(prefix="tui-replay-")
        cls.stream = cls._pack_store()
        cls.dispatch = cls._console_dispatch()
        cls.image = _splice_store(Path(cls.image_path).read_bytes(),
                                  cls.stream)

    @classmethod
    def tearDownClass(cls):
        if getattr(cls, "tmp", None):
            import shutil
            shutil.rmtree(cls.tmp, ignore_errors=True)

    @classmethod
    def _pack_store(cls):
        manifest_path = os.path.join(ROOT, cls.SUBDIR, "store.json")
        manifest = json.loads(Path(manifest_path).read_text())
        extras = (("tui_test.exe", TUI_TEST_EXE),
                  ("tui_demo.exe", TUI_DEMO_EXE),
                  ("tui_keys.exe", TUI_KEYS_EXE),
                  ("tui_dialog.exe", TUI_DIALOG_EXE))
        for name, path in extras:
            manifest["files"] = [entry for entry in manifest["files"]
                                 if entry["name"].lower() != name.lower()]
            if os.path.isfile(path):
                manifest["files"].append({"name": name, "type": "EXE",
                                          "path": path})
        out_json = os.path.join(cls.tmp, "store.json")
        Path(out_json).write_text(json.dumps(manifest))
        stream, _report = romfspack.pack(
            out_json, os.path.join(cls.tmp, "store.romfs"),
            objdir=os.path.join(ROOT, cls.SUBDIR, "modules"))
        return stream

    @classmethod
    def _console_dispatch(cls):
        path = os.path.join(ROOT, cls.SUBDIR, "modules",
                            "drivers.plan.json")
        plan = json.loads(Path(path).read_text())
        for driver in plan["drivers"]:
            if driver["name"].startswith("console"):
                return driver["endpoints"][0]["dispatch"]
        raise AssertionError("the plan carries no console driver")

    def _write_length(self, z):
        """The length of a console WRITE dispatch at the current PC."""
        if z.pc != self.dispatch:
            return None
        try:
            if z.bank_of(z.pc) != 6:
                return None
        except SystemExit:
            return None
        if z.rd(z.hl) != 0:  # KCONSOLE_OP_WRITE
            return None
        return z.rd16((z.hl + 4) & 0xFFFF)

    def _boot(self):
        replay = _Replay(self.image, self.SUBDIR)
        z = replay.make_cpu()
        if self.SERIAL:
            z.hid = z.ioc_peer = IocCommandPeer()
            replay.start_kernel(z, host_sync=b"\xaa\x55")
            for step in range(STEP_LIMIT):
                replay.skip_delay(z)
                z.step()
                if step % 0x1FFF == 0:
                    replay.pump(z)
                    if SHELL_PROMPT.encode("ascii") in bytes(z.sio_tx):
                        return replay, z
            raise AssertionError("the serial shell prompt did not appear")
        replay.boot_to_shell(z)
        return replay, z

    def _run(self, mode, keys=b"", late_keys=b"", late_marker=None,
             program="tui_test", expected=None):
        replay, z = self._boot()
        glyph_start = len(z.glyphs)
        command_start = len(z.vdp.commands)
        serial_start = len(z.sio_tx)
        if program == "tui_test":
            command = ("/system/tui_test.exe %s\r" % mode).encode("ascii")
        else:
            command = ("/system/%s.exe\r" % program).encode("ascii")
        command += keys
        if self.SERIAL:
            z.sio_rx.extend(command)
        else:
            z.hid.hid_queue.extend(command)
        writes = []
        armed = False
        late_sent = False
        if expected is None:
            if mode == "foundation":
                expected = b"tui: PASS"
            else:
                expected = ("tui: %s pass" % mode).encode("ascii")
        for step in range(STEP_LIMIT):
            length = self._write_length(z)
            if length is not None:
                if armed:
                    writes.append(length)
                elif length == TUI_INIT_LENGTH:
                    armed = True
            replay.skip_delay(z)
            z.step()
            if step % 0x1FFF == 0:
                replay.pump(z)
                if self.SERIAL:
                    lane = bytes(z.sio_tx[serial_start:])
                    if (late_keys and not late_sent and
                            late_marker is not None and
                            late_marker.encode("ascii") in lane):
                        z.sio_rx.extend(late_keys)
                        late_sent = True
                    if expected in lane:
                        after = lane[lane.index(expected) + len(expected):]
                        if mode != "batch" or TUI_SHUTDOWN in after:
                            break
                    if b"FAIL" in lane:
                        raise AssertionError(
                            "tui_test %s reported FAIL: %r"
                            % (mode, lane[-200:]))
                else:
                    text = "".join(z.glyphs[glyph_start:])
                    if (late_keys and not late_sent and
                            late_marker is not None and
                            late_marker in text):
                        z.hid.hid_queue.extend(late_keys)
                        late_sent = True
                    if expected.decode("ascii") in text:
                        if mode != "batch" or len(TUI_SHUTDOWN) in writes:
                            break
                    if "FAIL" in text:
                        raise AssertionError(
                            "tui_test %s reported FAIL: %r"
                            % (mode, text[-200:]))
        else:
            tail = (bytes(z.sio_tx[serial_start:]) if self.SERIAL
                    else "".join(z.glyphs[glyph_start:]))
            raise AssertionError("tui_test %s did not finish: %r"
                                 % (mode, tail[-200:]))
        replay.pump(z)
        return {
            "replay": replay,
            "z": z,
            "writes": writes,
            "lane": bytes(z.sio_tx[serial_start:]),
            "command_start": command_start,
            "glyphs": "".join(z.glyphs[glyph_start:]),
        }



class TuiModeChecks:
    """Shared package-68..72 screen and transcript checks for both lanes."""

    def _screen(self, run):
        if self.SERIAL:
            return parse_serial_screen(run["lane"])
        return decode_v9958_screen(run["replay"], run["z"],
                                   run["command_start"])

    def _assert_rows(self, mode, expected, keys=b"", rows=21):
        run = self._run(mode, keys)
        screen = self._screen(run)
        for row in range(rows):
            self.assertEqual(screen[row], expected[row],
                             "%s row %d" % (mode, row))
        return run

    def _text(self, run):
        return (run["lane"].decode("latin1") if self.SERIAL
                else run["glyphs"])

    def test_lone_escape_returns_without_a_following_byte(self):
        run = self._run("loneesc", b"\x1b")
        self.assertIn("tui: loneesc pass", self._text(run))

    def test_reinitialization_resets_an_incomplete_parser(self):
        run = self._run("keyreset", b"\x1b[",
                        late_keys=b"A",
                        late_marker="tui: keyreset primed")
        self.assertIn("tui: keyreset primed", self._text(run))
        self.assertIn("tui: keyreset pass", self._text(run))

    def _require_example(self, path):
        if not os.path.isfile(path):
            self.skipTest("%s has not been built" % path)

    def test_demo_example_runs(self):
        self._require_example(TUI_DEMO_EXE)
        run = self._run("demo", b"q", program="tui_demo",
                        expected=b"TUI DEMO PASS")
        self.assertIn("TUI DEMO PASS", self._text(run))

    def test_keys_example_runs(self):
        self._require_example(TUI_KEYS_EXE)
        run = self._run("keys", b"A\x1b", program="tui_keys",
                        expected=b"TUI KEYS PASS")
        self.assertIn("TUI KEYS PASS", self._text(run))
        self.assertIn("0041 BYTE", self._text(run))

    def test_dialog_example_runs(self):
        self._require_example(TUI_DIALOG_EXE)
        keys = (b"f"
                b"\x1b[Fm\r"
                b"\x1b[B\r"
                b"\x1b[C\r")
        expected = (b"TUI DIALOG PASS name=Adam sel=2 checked=1 "
                    b"menu=10 input=1 confirm=4")
        run = self._run("dialog", keys, program="tui_dialog",
                        expected=expected)
        text = self._text(run)
        self.assertIn("TUI DIALOG PASS", text)
        self.assertIn("name=Adam sel=2 checked=1 menu=10 input=1 confirm=4",
                      text)

    def test_widget_form_flush_counts(self):
        full = self._run("flushfull")["writes"][:-1]
        same = self._run("flushsame")["writes"][:-1]
        row = self._run("flushrow")["writes"][:-1]
        self.assertEqual(full, [126, 7, 112, 27, 53, 96, 6])
        self.assertEqual(same, full)
        self.assertEqual(row, full + [17])

    def test_windows_clips_clears_and_shifts(self):
        run = self._assert_rows("windows", windows_screen())
        self.assertTrue(all(4 <= length <= 128
                            for length in run["writes"]))

    def test_keys_report_the_exact_vocabulary(self):
        run = self._run("keys", KEYS_INPUT)
        if self.SERIAL:
            lane = run["lane"].decode("ascii", "ignore")
            start = lane.index(TUI_INIT.decode("ascii")) + len(TUI_INIT)
            body = lane[start:lane.index("tui: keys pass", start)]
        else:
            body = run["glyphs"][:run["glyphs"].index("tui: keys pass")]
        codes = re.findall(r"\b[0-9A-F]{4}\b", body)
        self.assertEqual([int(code, 16) for code in codes], KEYS_EXPECTED)

    def test_styles_frames_titles_and_attribute_runs(self):
        run = self._assert_rows("styles", styles_screen())
        self.assertTrue(all(4 <= length <= 128
                            for length in run["writes"]))

    def test_region_restores_the_exact_background(self):
        run = self._assert_rows("region", region_screen())
        self.assertTrue(all(4 <= length <= 128
                            for length in run["writes"]))

    def test_widgets_drive_a_configuration_form(self):
        run = self._assert_rows("widgets", widgets_screen(),
                                WIDGET_KEYS, rows=21)
        text = (run["lane"].decode("latin1") if self.SERIAL
                else run["glyphs"])
        self.assertIn("tui: widgets m=12 f=10 e=0 i=1 c=0 t=1", text)
        self.assertIn("tui: widgets n=dam s=0 cf=4 ms=2", text)
        self.assertIn("tui: widgets pass", text)


class TestTuiSerialScreen(TuiModeChecks, TuiSimBase):
    SUBDIR, IMAGE_NAME = SERIAL
    SERIAL = True

    def test_batch_is_one_run_and_the_unchanged_redraw_is_silent(self):
        run = self._run("batch")
        row = batch_row().encode("ascii")
        lane = run["lane"]
        self.assertTrue(lane.startswith(BATCH_COMMAND), lane[:80])
        program = lane[len(BATCH_COMMAND):]
        expected_program = (TUI_INIT + b"\x1b[1;1H" + row +
                            b"\x1b[23;1H" + BATCH_PASS + TUI_SHUTDOWN)
        self.assertTrue(program.startswith(expected_program), program)
        # Only the shell prompt may follow the program's own bytes.
        self.assertNotIn(b"\x1b[", program[len(expected_program):])
        self.assertEqual(program.count(row), 1)
        # One 6-byte cursor position precedes the row; the unchanged
        # redraw contributes no sequence and no cell, and the prompt the
        # shell writes after the program is not part of its dispatch list.
        writes = run["writes"]
        if len(TUI_SHUTDOWN) in writes:
            writes = writes[:writes.index(len(TUI_SHUTDOWN)) + 1]
        self.assertEqual(writes, [91, 7, 17, 15])
        screen = parse_serial_screen(lane)
        self.assertEqual("".join(ch for ch, _attr in screen[0]), batch_row())
        for row_number in range(1, 21):
            self.assertEqual("".join(ch for ch, _attr in screen[row_number]),
                             " " * COLS)

    def test_foundation_still_passes(self):
        run = self._run("foundation")
        self.assertIn(b"tui: PASS", run["lane"])

    def test_screen_matches_the_logical_model(self):
        run = self._run("screen")
        screen = parse_serial_screen(run["lane"])
        expected = expected_screen()
        for row in range(SCREEN_ROWS):
            self.assertEqual(screen[row], expected[row], "row %d" % row)
        hide = run["lane"].rfind(b"\x1b[?25l")
        self.assertGreaterEqual(hide, 0)
        self.assertIn(b"\x1b[?25h", run["lane"][hide + 1:])

    def test_shutdown_flushes_dirty_cells(self):
        run = self._run("shutdown")
        writes = run["writes"]
        self.assertGreaterEqual(len(writes), 3)
        self.assertEqual(writes[:3], [126, 15, 20])
        lane = run["lane"]
        init = lane.index(TUI_INIT)
        body = lane[init + len(TUI_INIT):]
        shutdown = body.rindex(TUI_SHUTDOWN)
        before = body[:shutdown]
        self.assertIn(b"dirty", before)
        self.assertIn(b"two", before)
        self.assertIn(b"tui: shutdown pass", body[shutdown:])
        screen = parse_serial_screen(lane[:init + len(TUI_INIT) + shutdown])
        expected = shutdown_screen()
        for row in range(SCREEN_ROWS):
            self.assertEqual(screen[row], expected[row], "row %d" % row)


class TestTuiV9958Screen(TuiModeChecks, TuiSimBase):
    SUBDIR, IMAGE_NAME = PRODUCTION

    def test_batch_is_one_run_and_the_unchanged_redraw_is_silent(self):
        run = self._run("batch")
        writes = run["writes"]
        if len(TUI_SHUTDOWN) in writes:
            writes = writes[:writes.index(len(TUI_SHUTDOWN)) + 1]
        self.assertEqual(writes, [91, 7, 17, 15])
        screen = decode_v9958_screen(run["replay"], run["z"],
                                     run["command_start"])
        self.assertEqual("".join(ch for ch, _attr in screen[0]), batch_row())
        for row in range(1, 21):
            self.assertEqual("".join(ch for ch, _attr in screen[row]),
                             " " * COLS)

    def test_foundation_still_passes(self):
        run = self._run("foundation")
        self.assertIn("tui: PASS", run["glyphs"])

    def test_screen_matches_the_logical_model(self):
        run = self._run("screen")
        screen = decode_v9958_screen(run["replay"], run["z"],
                                     run["command_start"])
        expected = expected_screen()
        for row in range(SCREEN_ROWS):
            self.assertEqual(screen[row], expected[row], "row %d" % row)
        # Drawing a few hundred cells stays far below one write per cell.
        self.assertLess(len(run["writes"]), 30, run["writes"])

    def test_shutdown_flushes_dirty_cells(self):
        run = self._run("shutdown")
        writes = run["writes"]
        self.assertGreaterEqual(len(writes), 3)
        self.assertEqual(writes[:3], [126, 15, 20])
        screen = decode_v9958_screen(run["replay"], run["z"],
                                     run["command_start"])
        expected = shutdown_screen()
        for row in range(SCREEN_ROWS):
            self.assertEqual(screen[row], expected[row], "row %d" % row)


if __name__ == "__main__":
    unittest.main()
