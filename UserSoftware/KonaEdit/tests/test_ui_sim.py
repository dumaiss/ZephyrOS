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
        app_bank = None
        for _step in range(10_000_000):
            if (z.pc == main and z.bank_of(z.pc) not in (0, 6, 7) and
                    z.bank_of(z.pc) != shell_bank and
                    bytes(z.rd(main + i) for i in range(len(signature))) ==
                    signature):
                app_bank = z.bank_of(z.pc)
            if app_bank is not None and z.pc == getkey and \
                    z.bank_of(z.pc) == app_bank:
                return app_bank
            replay.skip_delay(z)
            replay.feed(z)
            z.step()
        self.fail("KonaEdit did not enter its input loop")

    def press_and_measure(self, replay, z, app_bank, key):
        dispatch = self.console_dispatch()
        getkey = read_symbols(str(PROGRAM_MAP))["_tui_getkey"]
        renders = []
        command_start = len(z.vdp.commands)
        cycle_start = z.cycles
        origin_start = z.vdp.regs[23]
        left_getkey = False
        z.hid.hid_queue.extend(key)
        for _step in range(10_000_000):
            length = self.render_length(z, dispatch)
            if length is not None:
                renders.append(length)
            replay.skip_delay(z)
            replay.feed(z)
            z.step()
            if z.pc != getkey or z.bank_of(z.pc) != app_bank:
                left_getkey = True
            elif left_getkey and z.bank_of(z.pc) == app_bank:
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
        self.assertEqual(len(newline["renders"]), 1, newline)
        self.assertLessEqual(newline["cycles"], 150_000, newline)
        unnewline = self.press_and_measure(replay, z, app_bank, b"\x08")
        self.assertIn("Xalpha", "\n".join(self.screen_text(z)))
        self.assertEqual(len(unnewline["renders"]), 1, unnewline)
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


if __name__ == "__main__":
    unittest.main()
