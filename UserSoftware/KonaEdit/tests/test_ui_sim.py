"""Focused KonaEdit TUI replay on the accepted KonaOS image."""

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OS_ROOT = ROOT.parents[1]
sys.path.insert(0, str(OS_ROOT / "tests"))
sys.path.insert(0, str(OS_ROOT / "tools"))

import pack_image  # noqa: E402
import romfspack  # noqa: E402
from replay import ConsoleReplay, v9958_screen  # noqa: E402
from z80sim import read_symbols  # noqa: E402


BUILD = OS_ROOT / "build"
IMAGE = BUILD / "konaos.bin"
MAP = BUILD / "konaos.map"
STORE = BUILD / "store.json"
MODULES = BUILD / "modules"
PROGRAM = ROOT / "build/user/konaedit.exe"
PROGRAM_MAP = ROOT / "build/user/konaedit/konaedit.map"


def splice_store(image, stream):
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


class EditorReplay(ConsoleReplay):
    def __init__(self, image):
        self.root = str(OS_ROOT)
        self.subdir = "build"
        self.image_name = "konaos.bin"
        self.image = image
        self.syms = read_symbols(str(MAP))


class KonaEditTuiSmoke(unittest.TestCase):
    def setUp(self):
        for path in (IMAGE, MAP, STORE, PROGRAM, PROGRAM_MAP):
            if not path.is_file():
                self.skipTest("%s is missing" % path)
        self.tmp = Path(tempfile.mkdtemp(prefix="konaedit-tui-"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def make_replay(self):
        manifest = json.loads(STORE.read_text())
        manifest["files"] = [
            item for item in manifest["files"]
            if item["name"].lower() != "konaedit.exe"
        ]
        manifest["files"].append({
            "name": "konaedit.exe", "type": "EXE", "path": str(PROGRAM)
        })
        manifest_path = self.tmp / "store.json"
        manifest_path.write_text(json.dumps(manifest))
        stream, _report = romfspack.pack(
            str(manifest_path), str(self.tmp / "store.romfs"),
            objdir=str(MODULES))
        replay = EditorReplay(splice_store(IMAGE.read_bytes(), stream))
        z = replay.make_cpu()
        z.hid.fs2_add("ALPHA.TXT", b"alpha\n")
        for index in range(10):
            content = b"page\n"
            if index == 0:
                content = b"".join(
                    ("line%02d\n" % line).encode("ascii")
                    for line in range(40))
            z.hid.fs2_add("PAGE%02d.TXT" % index, content)
        z.hid.fs2_add(
            "cpm/b/dev/modula-2/list.mod",
            b"MODULE List;\r\nEND List.\r\n" + b"\x1a" * 15 +
            b"\x00\x00DIRECTORY-DEBRIS")
        replay.boot_to_shell(z)
        return replay, z

    @staticmethod
    def console_dispatch():
        plan = json.loads((MODULES / "drivers.plan.json").read_text())
        for driver in plan["drivers"]:
            if driver["name"].startswith("console"):
                return driver["endpoints"][0]["dispatch"]
        raise AssertionError("production plan carries no console driver")

    @staticmethod
    def render_length(z, dispatch):
        if z.pc != dispatch or z.bank_of(z.pc) != 6 or z.rd(z.hl) != 6:
            return None
        return z.rd16((z.hl + 4) & 0xFFFF)

    def wait_for_input_loop(self, replay, z, shell_bank):
        symbols = read_symbols(str(PROGRAM_MAP))
        getkey = symbols["_tui_getkey"]
        main = symbols["_main"]
        executable = PROGRAM.read_bytes()
        offset = 36 + main - 0x0100
        signature = executable[offset:offset + 24]
        found = {"bank": None}

        def hook(_step):
            if (z.pc == main and z.bank_of(z.pc) not in (0, 6, 7) and
                    z.bank_of(z.pc) != shell_bank and
                    bytes(z.rd(main + i) for i in range(len(signature))) ==
                    signature):
                found["bank"] = z.bank_of(z.pc)
            return (found["bank"] is not None and z.pc == getkey and
                    z.bank_of(z.pc) == found["bank"]) or None

        if replay.drive(z, 10_000_000, {main, getkey}, hook) is not None:
            return found["bank"]
        self.fail("KonaEdit did not enter its input loop")

    def press_and_measure(self, replay, z, app_bank, key):
        dispatch = self.console_dispatch()
        getkey = read_symbols(str(PROGRAM_MAP))["_tui_getkey"]
        renders = []
        command_start = len(z.vdp.commands)
        cycle_start = z.cycles
        tstate_start = z.tstates
        origin_start = z.vdp.regs[23]
        z.hid.hid_queue.extend(key)
        # The program is back in its input wait when, after a step, PC is
        # tui_getkey in the program's bank again having left it: a stop
        # at getkey at iteration n follows n - 1 earlier post-step
        # states, each one a departure unless it was itself such a stop.
        hits = {"n": 0}

        def hook(step):
            length = self.render_length(z, dispatch)
            if length is not None:
                renders.append(length)
            if z.pc == getkey and step >= 1 and \
                    z.bank_of(z.pc) == app_bank:
                if step - 1 > hits["n"]:
                    return True
                hits["n"] += 1
            return None

        if replay.drive(z, 10_000_000, {dispatch, getkey}, hook) \
                is not None:
            commands = z.vdp.commands[command_start:]
            cells = sum(
                (command["block"][8] |
                 (command["block"][9] << 8)) // 6
                for command in commands
                if command.get("code") in (0xC0, 0xD0)
                and command.get("block")
                and (command["block"][8] |
                     (command["block"][9] << 8)) in (6, 12)
                and (command["block"][10] |
                     (command["block"][11] << 8)) == 8)
            return {
                "renders": renders,
                "inversions": sum(
                    command.get("code") == 0x83 for command in commands
                ),
                "invert_blocks": [
                    command.get("block") for command in commands
                    if command.get("code") == 0x83
                ],
                "cells": cells,
                "cycles": z.cycles - cycle_start,
                "tstates": z.tstates - tstate_start,
                "origin_start": origin_start,
                "origin_end": z.vdp.regs[23],
                "origin_delta": ((z.vdp.regs[23] - origin_start) &
                                 0xFF),
                "commands": [
                    (command.get("code"), command.get("block"))
                    for command in commands
                ],
            }
        self.fail("KonaEdit did not return to an input wait")

    @staticmethod
    def screen_text(z):
        screen = v9958_screen(z.vdp.vram, z.vdp.regs[23])
        return ["".join(cell[0] for cell in row) for row in screen]

    def test_edit_menu_invert_save_and_return_to_shell(self):
        replay, z = self.make_replay()
        shell_bank = z.bank_of(0x0100)
        z.hid.hid_queue.extend(b"/system/konaedit.exe /fs/ALPHA.TXT\r")
        app_bank = self.wait_for_input_loop(replay, z, shell_bank)
        self.assertIn(
            "alpha", "\n".join(self.screen_text(z)),
            "pc=%04X sp=%04X ret=%04X glyph-tail=%r fs2=%r req=%r" %
            (z.pc, z.sp, z.rd16(z.sp), "".join(z.glyphs)[-160:],
             z.hid.fs2_counts, [item[0] for item in z.hid.requests[-20:]]))

        typed = self.press_and_measure(replay, z, app_bank, b"X")
        self.assertIn("Xalpha", "\n".join(self.screen_text(z)))
        self.assertLessEqual(typed["cells"], 8, typed)
        self.assertLessEqual(typed["cycles"], 150_000, typed)
        newline = self.press_and_measure(replay, z, app_bank, b"\r")
        newline_screen = self.screen_text(z)
        self.assertEqual(newline_screen[1].rstrip(), "X")
        self.assertEqual(newline_screen[2].rstrip(), "alpha")
        # The rows below move by one hardware SCROLL record (ten
        # bytes); one render then draws the two changed rows and the
        # status.
        self.assertEqual(len(newline["renders"]), 2, newline)
        self.assertEqual(newline["renders"][0], 10, newline)
        self.assertLessEqual(newline["cycles"], 150_000, newline)
        unnewline = self.press_and_measure(replay, z, app_bank, b"\x08")
        self.assertIn("Xalpha", "\n".join(self.screen_text(z)))
        self.assertEqual(len(unnewline["renders"]), 2, unnewline)
        self.assertEqual(unnewline["renders"][0], 10, unnewline)
        self.assertLessEqual(unnewline["cycles"], 150_000, unnewline)

        self.press_and_measure(replay, z, app_bank, b"\x1bOP")  # F1
        self.press_and_measure(replay, z, app_bank, b"\x1b[B")  # open File
        menu_move = self.press_and_measure(replay, z, app_bank, b"\x1b[B")
        self.assertEqual(menu_move["cells"], 0, menu_move)
        self.assertGreaterEqual(menu_move["inversions"], 2, menu_move)
        self.assertLessEqual(menu_move["inversions"], 3, menu_move)
        self.press_and_measure(replay, z, app_bank, b"\x1b")  # popup -> bar
        self.press_and_measure(replay, z, app_bank, b"\x1b")  # bar -> editor

        self.press_and_measure(replay, z, app_bank, b"\x1bOQ")  # F2 save
        self.assertEqual(bytes(z.hid.fs2_files[("ALPHA.TXT",)]),
                         b"Xalpha\n")

        self.press_and_measure(replay, z, app_bank, b"\x1bOR")  # F3 open
        first_page = self.screen_text(z)
        self.assertEqual(first_page[1].rstrip(), "Xalpha")
        picker_move = None
        for _index in range(7):
            picker_move = self.press_and_measure(
                replay, z, app_bank, b"\x1b[B")
        self.assertEqual(self.screen_text(z), first_page)
        self.assertLessEqual(picker_move["cycles"], 150_000, picker_move)
        page_move = self.press_and_measure(replay, z, app_bank, b"\x1b[B")
        self.assertNotEqual(self.screen_text(z), first_page)
        self.assertEqual(self.screen_text(z)[1].rstrip(), "Xalpha")
        self.press_and_measure(replay, z, app_bank, b"\t")
        name_key = self.press_and_measure(replay, z, app_bank, b"Z")
        self.assertLessEqual(name_key["cycles"], 150_000, name_key)
        self.press_and_measure(replay, z, app_bank, b"\x1b")
        self.press_and_measure(replay, z, app_bank, b"\x1bOR")  # F3 open
        self.press_and_measure(replay, z, app_bank, b"\t")
        for byte in b"PAGE00.TXT":
            self.press_and_measure(replay, z, app_bank, bytes([byte]))
        self.press_and_measure(replay, z, app_bank, b"\t")
        opened = self.press_and_measure(replay, z, app_bank, b"\r")
        self.assertEqual(self.screen_text(z)[1].rstrip(), "line00")

        print("KonaEdit traffic: insert=%r newline=%r unnewline=%r "
              "menu-move=%r picker=%r name-key=%r page=%r open=%r" %
              (typed, newline, unnewline, menu_move, picker_move, name_key,
               page_move, opened))

        z.hid.hid_queue.extend(b"\x1b[21~")               # F10
        start = len(z.glyphs)
        replay.run_to_pattern(z, r"/\n?> ", since=start)

        z.hid.hid_queue.extend(
            b"/system/konaedit.exe /fs/cpm/b/dev/modula-2/list.mod\r")
        app_bank = self.wait_for_input_loop(replay, z, shell_bank)
        nested_screen = self.screen_text(z)
        self.assertEqual(nested_screen[1].rstrip(), "MODULE List;")
        self.assertEqual(nested_screen[2].rstrip(), "END List.")
        z.hid.hid_queue.extend(b"\x1b[21~")               # F10
        start = len(z.glyphs)
        replay.run_to_pattern(z, r"/\n?> ", since=start)

    def test_viewport_scroll_uses_vram_origin(self):
        replay, z = self.make_replay()
        shell_bank = z.bank_of(0x0100)
        z.hid.hid_queue.extend(
            b"/system/konaedit.exe /fs/PAGE00.TXT\r")
        app_bank = self.wait_for_input_loop(replay, z, shell_bank)
        for _index in range(23):
            self.press_and_measure(replay, z, app_bank, b"\x1b[B")
        before_scroll = self.screen_text(z)
        self.assertEqual(before_scroll[1].rstrip(), "line00")
        self.assertEqual(before_scroll[24].rstrip(), "line23")

        scroll = self.press_and_measure(replay, z, app_bank, b"\x1b[B")
        after_scroll = self.screen_text(z)
        self.assertEqual(after_scroll[0], before_scroll[0], scroll)
        self.assertEqual(after_scroll[1].rstrip(), "line01", scroll)
        self.assertEqual(after_scroll[24].rstrip(), "line24", scroll)
        self.assertIn(10, scroll["renders"], scroll)
        self.assertEqual(scroll["origin_delta"], 8, scroll)
        self.assertLessEqual(scroll["cycles"], 1_475_000, scroll)

        # Move back to the first line: the final Up scrolls the same region
        # downward and restores the preceding circular-VRAM row.
        for _index in range(23):
            self.press_and_measure(replay, z, app_bank, b"\x1b[A")
        before_reverse = self.screen_text(z)
        reverse = self.press_and_measure(replay, z, app_bank, b"\x1b[A")
        after_reverse = self.screen_text(z)
        self.assertEqual(after_reverse[0], before_reverse[0], reverse)
        self.assertEqual(after_reverse[1].rstrip(), "line00", reverse)
        self.assertEqual(after_reverse[24].rstrip(), "line23", reverse)
        self.assertIn(10, reverse["renders"], reverse)
        self.assertEqual(reverse["origin_delta"], 0xF8, reverse)
        self.assertLessEqual(reverse["cycles"], 1_475_000, reverse)
        z.hid.hid_queue.extend(b"\x1b[21~")               # F10
        start = len(z.glyphs)
        replay.run_to_pattern(z, r"/\n?> ", since=start)


SOURCE_LINES = [
    "\tPROCEDURE Item%03d(VAR x: INTEGER); (* comment *)" % index
    for index in range(110)]
# A 3 KiB source: the lines above cut at 3,072 bytes (the last line
# partial, without a newline).
SOURCE = ("".join(line + "\n" for line in SOURCE_LINES)).encode()[:3072]
LINES = SOURCE.decode().split("\n")
BODY = 24

UP, DOWN, LEFT, RIGHT = b"\x1b[A", b"\x1b[B", b"\x1b[D", b"\x1b[C"
PGUP, PGDN, DELETE = b"\x1b[5~", b"\x1b[6~", b"\x1b[3~"
F1, F7 = b"\x1bOP", b"\x1b[18~"


def shown(line):
    """A line as the editor draws it: TAB to the next multiple of 8."""
    out = ""
    for ch in line:
        out += " " * (8 - len(out) % 8) if ch == "\t" else ch
    return out


class KonaEditViewBudgets(KonaEditTuiSmoke):
    """A 3 KiB source with the cursor near line 60: each key's whole-
    system T-states, and the screen after it."""

    def body(self, z):
        return [row.rstrip() for row in self.screen_text(z)[1:1 + BODY]]

    def expect_window(self, z, lines, top, where):
        want = [shown(line).rstrip() for line in lines[top:top + BODY]]
        want += [""] * (BODY - len(want))
        self.assertEqual(self.body(z), want, where)

    def expect_some_window(self, z, lines, where):
        """After a newline or a join the view keeps either the rows above
        or the rows below the cursor: the screen is some window of the
        lines, and the cursor's line (the status Ln) is in it."""
        body = self.body(z)
        line = int(self.status(z).split("Ln ")[1].split()[0]) - 1
        for top in range(max(0, line - BODY + 1), line + 1):
            want = [shown(text).rstrip() for text in lines[top:top + BODY]]
            want += [""] * (BODY - len(want))
            if body == want:
                return top
        self.fail("no window of the lines around line %d: %r (%r)"
                  % (line + 1, body, where))

    def status(self, z):
        return self.screen_text(z)[25]

    def test_keys_meet_their_budgets_anywhere_in_the_file(self):
        replay, z = self.make_replay()
        z.hid.fs2_add("SRC.MOD", SOURCE)
        shell_bank = z.bank_of(0x0100)
        z.hid.hid_queue.extend(b"/system/konaedit.exe /fs/SRC.MOD\r")
        app = self.wait_for_input_loop(replay, z, shell_bank)
        lines = list(LINES)

        def press(key):
            return self.press_and_measure(replay, z, app, key)

        self.expect_window(z, lines, 0, "opened")
        for _index in range(BODY - 1):
            press(DOWN)
        scroll = press(DOWN)
        self.expect_window(z, lines, 1, scroll)
        self.assertLessEqual(scroll["tstates"], 1_200_000, scroll)
        for _index in range(BODY - 1):
            press(UP)
        back = press(UP)
        self.expect_window(z, lines, 0, back)
        self.assertLessEqual(back["tstates"], 1_200_000, back)

        # Line 60 (index 59), the cursor eight bytes in.
        for _index in range(59):
            press(DOWN)
        self.assertIn("Ln 60 ", self.status(z))
        top = 59 - (BODY - 1)
        self.expect_window(z, lines, top, "line 60")
        press(UP)
        down = press(DOWN)
        self.assertLessEqual(down["tstates"], 500_000, down)
        for _index in range(7):
            press(RIGHT)
        right = press(RIGHT)
        self.assertLessEqual(right["tstates"], 500_000, right)

        typed = press(b"X")
        lines[59] = lines[59][:8] + "X" + lines[59][8:]
        self.expect_window(z, lines, top, typed)
        self.assertLessEqual(typed["tstates"], 800_000, typed)
        erased = press(b"\x08")
        lines[59] = lines[59][:8] + lines[59][9:]
        self.expect_window(z, lines, top, erased)
        self.assertLessEqual(erased["tstates"], 800_000, erased)
        deleted = press(DELETE)
        lines[59] = lines[59][:8] + lines[59][9:]
        self.expect_window(z, lines, top, deleted)
        self.assertLessEqual(deleted["tstates"], 800_000, deleted)

        # Enter in mid-line, then the join that undoes it.
        split = press(b"\r")
        lines[59:60] = [lines[59][:8], lines[59][8:]]
        self.expect_some_window(z, lines, split)
        self.assertIn("Ln 61 Col 1 ", self.status(z))
        self.assertLessEqual(split["tstates"], 1_500_000, split)
        joined = press(b"\x08")
        lines[59:61] = [lines[59] + lines[60]]
        self.expect_some_window(z, lines, joined)
        self.assertIn("Ln 60 Col 16 ", self.status(z))
        self.assertLessEqual(joined["tstates"], 1_500_000, joined)
        # The same in the middle of the screen.
        for _index in range(BODY // 2):
            press(UP)
        status = self.status(z)
        split = press(b"\r")
        row = int(status.split("Ln ")[1].split()[0]) - 1
        lines[row:row + 1] = [lines[row][:8], lines[row][8:]]
        self.expect_some_window(z, lines, split)
        self.assertLessEqual(split["tstates"], 1_500_000, split)
        joined = press(b"\x08")
        lines[row:row + 2] = [lines[row] + lines[row + 1]]
        self.expect_some_window(z, lines, joined)
        self.assertEqual(self.status(z), status)
        self.assertLessEqual(joined["tstates"], 1_500_000, joined)
        middle = self.body(z)

        # Page Down then Page Up: the same screen and line.
        press(PGUP)
        press(PGDN)
        self.assertEqual(self.body(z), middle)
        self.assertEqual(self.status(z), status)

        # A selection over three rows, then Esc clears it.
        press(F7)
        for _index in range(3):
            press(DOWN)
        screen = v9958_screen(z.vdp.vram, z.vdp.regs[23])
        marked = [row for row in range(1, 1 + BODY)
                  if any(attr for _ch, attr in screen[row])]
        self.assertEqual(len(marked), 4, marked)
        self.assertEqual(marked, list(range(marked[0], marked[0] + 4)))
        press(b"\x1b")
        screen = v9958_screen(z.vdp.vram, z.vdp.regs[23])
        self.assertFalse([row for row in range(1, 1 + BODY)
                          if any(attr for _ch, attr in screen[row])])

        # Undo after typing (Edit > Undo).
        before = self.body(z)
        press(b"Q")
        self.assertNotEqual(self.body(z), before)
        for key in (F1, RIGHT, DOWN, b"\r"):
            press(key)
        self.assertEqual(self.body(z), before)

        # Go To Line 40 (Search > Go To Line, below Find, Find Next and
        # Replace).
        for key in (F1, RIGHT, RIGHT, DOWN, DOWN, DOWN, DOWN, b"\r", b"\x08",
                    b"4", b"0", b"\r"):
            press(key)
        self.assertIn("Ln 40 ", self.status(z))
        self.assertIn(shown(lines[39]).rstrip(), self.body(z))
        print("KonaEdit view: scroll %d/%d down %d right %d type %d "
              "backspace %d delete %d enter %d join %d T-states" % (
                  scroll["tstates"], back["tstates"], down["tstates"],
                  right["tstates"], typed["tstates"], erased["tstates"],
                  deleted["tstates"], split["tstates"],
                  joined["tstates"]))
        z.hid.hid_queue.extend(b"\x1b[21~")               # F10
        for key in (RIGHT, b"\r"):                       # discard
            z.hid.hid_queue.extend(key)

    def test_a_long_line_scrolls_horizontally(self):
        """End on a line wider than the screen moves the left column so
        the cursor's column is the last one shown; Home moves it back."""
        long_line = "\tx" + "".join(chr(97 + i % 26) for i in range(130))
        replay, z = self.make_replay()
        z.hid.fs2_add("LONG.TXT", (long_line + "\nshort\n").encode())
        shell_bank = z.bank_of(0x0100)
        z.hid.hid_queue.extend(b"/system/konaedit.exe /fs/LONG.TXT\r")
        app = self.wait_for_input_loop(replay, z, shell_bank)
        text = shown(long_line)
        self.assertEqual(self.body(z)[0], text[:85].rstrip())
        self.press_and_measure(replay, z, app, b"\x1b[F")      # End
        left = len(text) - 85 + 1
        self.assertEqual(self.body(z)[0], text[left:left + 85].rstrip())
        self.assertIn("Col %d " % (len(text) + 1), self.status(z))
        self.press_and_measure(replay, z, app, b"\x1b[H")      # Home
        self.assertEqual(self.body(z)[0], text[:85].rstrip())
        # Position k (k >= 1) is visual column 8 + k - 1; at column 85
        # the left column becomes 1, inside the leading TAB, whose rest
        # shows as blanks.
        for _index in range(78):
            self.press_and_measure(replay, z, app, RIGHT)
        self.assertIn("Col 86 ", self.status(z))
        self.assertEqual(self.body(z)[0], text[1:86].rstrip())


if __name__ == "__main__":
    unittest.main()
