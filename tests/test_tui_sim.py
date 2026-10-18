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
            attr = 1 if sy & 0x8000 else 0
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
        manifest["files"] = [entry for entry in manifest["files"]
                             if entry["name"].lower() != "tui_test.exe"]
        manifest["files"].append({"name": "tui_test.exe", "type": "EXE",
                                  "path": TUI_TEST_EXE})
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

    def _run(self, mode):
        replay, z = self._boot()
        glyph_start = len(z.glyphs)
        command_start = len(z.vdp.commands)
        serial_start = len(z.sio_tx)
        if self.SERIAL:
            z.sio_rx.extend(
                ("/system/tui_test.exe %s\r" % mode).encode("ascii"))
        else:
            z.hid.hid_queue.extend(
                ("/system/tui_test.exe %s\r" % mode).encode("ascii"))
        writes = []
        armed = False
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
                    if expected.decode("ascii") in text:
                        if mode != "batch" or len(TUI_SHUTDOWN) in writes:
                            break
                    if "FAIL" in text:
                        raise AssertionError(
                            "tui_test %s reported FAIL: %r"
                            % (mode, text[-200:]))
        else:
            raise AssertionError("tui_test %s did not finish" % mode)
        replay.pump(z)
        return {
            "replay": replay,
            "z": z,
            "writes": writes,
            "lane": bytes(z.sio_tx[serial_start:]),
            "command_start": command_start,
            "glyphs": "".join(z.glyphs[glyph_start:]),
        }


class TestTuiSerialScreen(TuiSimBase):
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


class TestTuiV9958Screen(TuiSimBase):
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
