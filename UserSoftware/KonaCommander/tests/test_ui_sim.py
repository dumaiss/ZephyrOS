"""Focused KonaCommander TUI/navigation smoke on the accepted KonaOS image."""

import json
import os
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
from replay import ConsoleReplay  # noqa: E402
from z80sim import read_symbols  # noqa: E402


BUILD = OS_ROOT / "build"
IMAGE = BUILD / "konaos.bin"
MAP = BUILD / "konaos.map"
STORE = BUILD / "store.json"
MODULES = BUILD / "modules"
PROGRAM = ROOT / "build/user/konacmd.exe"
PROGRAM_MAP = ROOT / "build/user/konacmd/konacmd.map"


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


class CommanderReplay(ConsoleReplay):
    def __init__(self, image):
        self.root = str(OS_ROOT)
        self.subdir = "build"
        self.image_name = "konaos.bin"
        self.image = image
        self.syms = read_symbols(str(MAP))


class CommanderTuiSmoke(unittest.TestCase):
    def setUp(self):
        for path in (IMAGE, MAP, STORE, PROGRAM, PROGRAM_MAP):
            if not path.is_file():
                self.skipTest("%s is missing" % path)
        self.tmp = Path(tempfile.mkdtemp(prefix="konacmd-tui-"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def make_replay(self):
        manifest = json.loads(STORE.read_text())
        manifest["files"] = [
            item for item in manifest["files"]
            if item["name"].lower() != "konacmd.exe"
        ]
        manifest["files"].append({
            "name": "konacmd.exe", "type": "EXE", "path": str(PROGRAM)
        })
        manifest_path = self.tmp / "store.json"
        manifest_path.write_text(json.dumps(manifest))
        stream, _report = romfspack.pack(
            str(manifest_path), str(self.tmp / "store.romfs"),
            objdir=str(MODULES))
        replay = CommanderReplay(splice_store(IMAGE.read_bytes(), stream))
        z = replay.make_cpu()
        z.hid.fs2_add("ALPHA.TXT", b"alpha\n")
        z.hid.fs2_add("BETA.TXT", b"beta\n")
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
    def write_length(z, dispatch):
        if z.pc != dispatch or z.bank_of(z.pc) != 6 or z.rd(z.hl) != 0:
            return None
        return z.rd16((z.hl + 4) & 0xFFFF)

    @staticmethod
    def render_length(z, dispatch):
        if z.pc != dispatch or z.bank_of(z.pc) != 6 or z.rd(z.hl) != 6:
            return None
        return z.rd16((z.hl + 4) & 0xFFFF)

    def press_and_measure(self, replay, z, key):
        dispatch = self.console_dispatch()
        getkey = read_symbols(str(PROGRAM_MAP))["_tui_getkey"]
        writes = []
        renders = []
        command_start = len(z.vdp.commands)
        vram_write_start = len(z.vdp.vram_writes)
        cycle_start = z.cycles
        left_getkey = False
        z.hid.hid_queue.extend(key)
        for _step in range(8_000_000):
            length = self.write_length(z, dispatch)
            if length is not None:
                writes.append(length)
            length = self.render_length(z, dispatch)
            if length is not None:
                renders.append(length)
            replay.skip_delay(z)
            replay.feed(z)
            z.step()
            if z.pc != getkey:
                left_getkey = True
            elif left_getkey:
                commands = z.vdp.commands[command_start:]
                glyphs = sum(
                    1 for command in commands
                    if command.get("code") == 0xD0 and command.get("block")
                    and (command["block"][8] |
                         (command["block"][9] << 8)) == 6
                    and (command["block"][10] |
                         (command["block"][11] << 8)) == 8)
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
                    "writes": writes,
                    "renders": renders,
                    "inversions": sum(
                        1 for command in commands
                        if command.get("code") == 0x83),
                    "glyphs": glyphs,
                    "cells": cells,
                    "sat_writes": sum(
                        1 for address, _value
                        in z.vdp.vram_writes[vram_write_start:]
                        if 0xF200 <= address < 0xF208),
                    "cycles": z.cycles - cycle_start,
                }
        self.fail("Commander did not return to its input loop")

    def wait_for_input_loop(self, replay, z):
        symbols = read_symbols(str(PROGRAM_MAP))
        getkey = symbols["_tui_getkey"]
        ui_run = symbols["_kc_ui_run"]
        entered = False
        for _step in range(8_000_000):
            if z.pc == ui_run:
                entered = True
            if entered and z.pc == getkey:
                return
            replay.skip_delay(z)
            replay.feed(z)
            z.step()
        self.fail("Commander did not enter its input loop")

    def test_menu_focus_tab_and_quit_return_to_the_shell(self):
        replay, z = self.make_replay()
        z.hid.hid_queue.extend(b"/system/konacmd.exe\r")
        self.wait_for_input_loop(replay, z)

        # Selection and focus are compact markers. Neither operation may
        # re-style complete 42-column rows or retransmit the screen.
        move = self.press_and_measure(replay, z, b"\x1b[B")
        focus = self.press_and_measure(replay, z, b"\t")
        if os.environ.get("KONACMD_REPORT_TRAFFIC"):
            print("Down traffic:", move)
            print("Tab traffic:", focus)
        self.assertLessEqual(move["glyphs"], 2, move)
        self.assertLessEqual(focus["glyphs"], 2, focus)
        self.assertEqual(move["sat_writes"], 0, move)
        self.assertEqual(focus["sat_writes"], 0, focus)

        # F1, Right twice (View), Down (open), Down (move its selection),
        # Tab (close and focus left), then quit.  Moving inside an open popup
        # changes only the old and new 18-cell item rows.
        menu_focus = self.press_and_measure(replay, z, b"\x1bOP")
        menu_switch = self.press_and_measure(replay, z, b"\x1b[C")
        menu_switch_view = self.press_and_measure(replay, z, b"\x1b[C")
        menu_open = self.press_and_measure(replay, z, b"\x1b[B")
        menu_move = self.press_and_measure(replay, z, b"\x1b[B")
        menu_close = self.press_and_measure(replay, z, b"\t")
        if os.environ.get("KONACMD_REPORT_TRAFFIC"):
            print("Menu focus traffic:", menu_focus)
            print("Menu switch traffic:", menu_switch)
            print("Menu View traffic:", menu_switch_view)
            print("Menu open traffic:", menu_open)
            print("Menu move traffic:", menu_move)
            print("Menu close traffic:", menu_close)
        self.assertLessEqual(menu_move["cells"], 36, menu_move)
        self.assertEqual(len(menu_switch["renders"]), 1, menu_switch)
        self.assertEqual(menu_switch["inversions"], 2, menu_switch)
        self.assertEqual(menu_switch["cells"], 0, menu_switch)
        self.assertEqual(len(menu_switch_view["renders"]), 1,
                         menu_switch_view)
        self.assertEqual(menu_switch_view["inversions"], 2,
                         menu_switch_view)
        self.assertEqual(menu_switch_view["cells"], 0,
                         menu_switch_view)

        keys = b"q"
        start = len(z.glyphs)
        z.hid.hid_queue.extend(keys)
        # Depending on whether the VDP trace saw the adjacent cursor move as
        # a new run, its model may place a newline between slash and prompt.
        replay.run_to_pattern(z, r"/\n?> ", since=start)


if __name__ == "__main__":
    unittest.main()
