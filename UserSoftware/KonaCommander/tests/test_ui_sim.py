"""Focused KonaCommander TUI/navigation smoke on the accepted KonaOS image."""

import importlib.util
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
from replay import ConsoleReplay, v9958_screen  # noqa: E402
from z80sim import read_symbols  # noqa: E402


BUILD = OS_ROOT / "build"
IMAGE = BUILD / "konaos.bin"
MAP = BUILD / "konaos.map"
STORE = BUILD / "store.json"
MODULES = BUILD / "modules"
PROGRAM = ROOT / "build/user/kc.exe"
PROGRAM_MAP = ROOT / "build/user/kc/kc.map"


_SPEC = importlib.util.spec_from_file_location(
    "kcommander_host", ROOT / "host" / "kcommander_host.py")
host = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = host
_SPEC.loader.exec_module(host)

# The pointer's sprite attribute entry (slot 1) in the V9958 VRAM and its
# hidden value (the pattern field 2).
POINTER_SLOT = 0x1F204
POINTER_HIDDEN = (0x00, 0x00, 2, 0)


class Pointer:
    """Scripted IOC mouse reports (flags, buttons, pressed, dx, dy,
    wheel).  The pointer starts at pixel (255, 104), where the tracker
    puts it; a cell is 6 x 8 pixels and the pointer rests in its middle."""

    def __init__(self):
        self.x = 255
        self.y = 104

    def move(self, col, row):
        x, y = col * 6 + 3, row * 8 + 4
        report = (3, 0, 0, x - self.x, y - self.y, 0)
        self.x, self.y = x, y
        return [report]

    def press(self):
        return [(3, 1, 1, 0, 0, 0)]

    def release(self):
        return [(3, 0, 0, 0, 0, 0)]

    def click(self, col, row):
        return self.move(col, row) + self.press() + self.release()

    def right_click(self, col, row):
        return self.move(col, row) + [(3, 2, 2, 0, 0, 0)] + self.release()

    def wheel(self, col, row, notches):
        return self.move(col, row) + [(3, 0, 0, 0, 0, notches)]

    @staticmethod
    def idle(reads):
        return [(3, 0, 0, 0, 0, 0)] * reads


class HostBridge:
    """The PC end of the SIO0/B link: the program's KCP1 records go to the
    host tool's protocol engine, its answers to the receive queue."""

    def __init__(self, root, z):
        self.service = host.FileService(Path(root), "TESTPC",
                                        allow_write=True)
        self.seen = len(z.sio_tx)
        self.record = bytearray()

    def pump(self, z):
        data = bytes(z.sio_tx[self.seen:])
        self.seen += len(data)
        for byte in data:
            if byte != 0:
                self.record.append(byte)
                continue
            record, self.record = bytes(self.record), bytearray()
            try:
                answer = self.service.process(host.cobs_decode(record))
            except ValueError:
                continue
            z.sio_rx.extend(host.cobs_encode(answer) + b"\0")


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
        self.tmp = Path(tempfile.mkdtemp(prefix="kc-tui-"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def make_replay(self):
        manifest = json.loads(STORE.read_text())
        manifest["files"] = [
            item for item in manifest["files"]
            if item["name"].lower() != "kc.exe"
        ]
        manifest["files"].append({
            "name": "kc.exe", "type": "EXE", "path": str(PROGRAM)
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
        z.hid.hid_queue.extend(key)
        # Back in the input wait: after a step, PC is tui_getkey again
        # having left it (see KonaEdit's harness for the count).
        hits = {"n": 0}

        def hook(step):
            length = self.write_length(z, dispatch)
            if length is not None:
                writes.append(length)
            length = self.render_length(z, dispatch)
            if length is not None:
                renders.append(length)
            if z.pc == getkey and step >= 1:
                if step - 1 > hits["n"]:
                    return True
                hits["n"] += 1
            return None

        if replay.drive(z, 8_000_000, {dispatch, getkey}, hook) \
                is not None:
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

    def wait_for_input_loop(self, replay, z, bridge=None):
        symbols = read_symbols(str(PROGRAM_MAP))
        getkey = symbols["_tui_getkey"]
        ui_run = symbols["_kc_ui_run"]
        entered = {"yes": False}

        def hook(_step):
            if z.pc == ui_run:
                entered["yes"] = True
            return (entered["yes"] and z.pc == getkey) or None

        tick = None
        if bridge is not None:
            tick = lambda _step: bridge.pump(z)  # noqa: E731
        if replay.drive(z, 8_000_000, {ui_run, getkey}, hook, tick=tick,
                        every=0x3FF) is not None:
            return
        self.fail("Commander did not enter its input loop")

    def test_menu_focus_tab_and_quit_return_to_the_shell(self):
        replay, z = self.make_replay()
        z.hid.hid_queue.extend(b"/system/kc.exe\r")
        self.wait_for_input_loop(replay, z)

        # Selection and focus are compact markers. Neither operation may
        # re-style complete 42-column rows or retransmit the screen.
        move = self.press_and_measure(replay, z, b"\x1b[B")
        focus = self.press_and_measure(replay, z, b"\t")
        if os.environ.get("KC_REPORT_TRAFFIC"):
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
        if os.environ.get("KC_REPORT_TRAFFIC"):
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

    # -- the mouse -------------------------------------------------------

    def start_with_host(self, pc_files=None):
        """The shell, the PC end attached to the link, kc running with
        its panes loaded: the local card holds ALPHA.TXT, BETA.TXT,
        GAMMA.TXT, PAGE.TXT (40 lines) and the directory ZDIR, the PC
        root NOTES.TXT and the directory SUB."""
        replay, z = self.make_replay()
        for name, content in (("ALPHA.TXT", b"alpha\n"),
                              ("BETA.TXT", b"beta\n"),
                              ("GAMMA.TXT", b"gamma\n"),
                              ("PAGE.TXT", b"".join(
                                  ("line%02d\n" % line).encode()
                                  for line in range(40))),
                              ("ZDIR/INNER.TXT", b"inner\n")):
            z.hid.fs2_add(name, content)
        root = Path(self.tmp) / "pc"
        (root / "SUB").mkdir(parents=True)
        (root / "NOTES.TXT").write_bytes(b"notes\n")
        (root / "SUB" / "DEEP.TXT").write_bytes(b"deep\n")
        self.root = root
        bridge = HostBridge(root, z)
        z.hid.hid_queue.extend(b"/system/kc.exe\r")
        self.wait_for_input_loop(replay, z, bridge)
        self.bridge = bridge
        self.pointer = Pointer()
        return replay, z

    @staticmethod
    def screen_text(z):
        screen = v9958_screen(z.vdp.vram, z.vdp.regs[23])
        return ["".join(cell[0] for cell in row) for row in screen]

    def mouse(self, replay, z, reports, extra=3):
        """Queue the reports and run until the program has read them all
        and three more, so it has answered the last one and is idle."""
        peer = z.ioc_peer
        target = peer.mouse_commands + len(reports) + extra
        peer.mouse_reports.extend(reports)

        def tick(_step):
            self.bridge.pump(z)
            return peer.mouse_commands >= target

        if replay.drive(z, 60_000_000, tick=tick, every=0x3FF) is None:
            self.fail("the mouse script was not consumed: %r"
                      % self.screen_text(z)[22:])

    def row_of(self, z, name, column):
        for row, text in enumerate(self.screen_text(z)[2:22], 2):
            if text[column:column + 40].lstrip(" />").startswith(name):
                return row
        self.fail("%s is not listed: %r" % (name, self.screen_text(z)))

    def focused(self, z, column):
        """The pane whose path prefix ('L' or 'R') is reversed."""
        return v9958_screen(z.vdp.vram, z.vdp.regs[23])[1][column][1] != 0

    def pointer_entry(self, z):
        return tuple(z.vdp.vram[POINTER_SLOT:POINTER_SLOT + 4])

    def test_mouse_selects_opens_and_scrolls_the_panes(self):
        replay, z = self.start_with_host()
        ptr = self.pointer
        text = self.screen_text(z)
        self.assertIn("R:/", text[1][43:])
        alpha = self.row_of(z, "ALPHA.TXT", 0)
        beta = self.row_of(z, "BETA.TXT", 0)
        notes = self.row_of(z, "NOTES.TXT", 43)
        sub = self.row_of(z, "SUB", 43)
        self.assertEqual(text[alpha][0], ">")

        # A click selects the entry and focuses its pane; the pointer is
        # on the clicked cell.
        self.mouse(replay, z, ptr.click(5, beta))
        text = self.screen_text(z)
        self.assertEqual((text[beta][0], text[alpha][0]), (">", " "))
        self.assertTrue(self.focused(z, 0))
        entry = self.pointer_entry(z)
        self.assertEqual((entry[1], entry[2]), ((5 * 6 + 3) // 2, 1))
        self.mouse(replay, z, ptr.click(50, notes))
        text = self.screen_text(z)
        self.assertEqual(text[notes][43], ">")
        self.assertTrue(self.focused(z, 43))
        self.assertFalse(self.focused(z, 0))

        # Two presses on one entry more than 500 ms apart only select;
        # two within 500 ms open it, as Enter does.
        self.mouse(replay, z, ptr.click(50, sub) + ptr.idle(40) +
                   ptr.click(50, sub))
        self.assertIn("R:/ ", self.screen_text(z)[1][43:])
        self.mouse(replay, z, ptr.click(50, sub) + ptr.click(50, sub))
        text = self.screen_text(z)
        self.assertIn("R:/SUB", text[1][43:])
        self.assertIn("DEEP.TXT", "\n".join(row[43:] for row in text[2:22]))

        # The wheel moves the selection of the pane under the pointer a
        # row a notch, toward the user down; the pane takes the focus.
        self.mouse(replay, z, ptr.wheel(10, 5, -2))
        text = self.screen_text(z)
        self.assertTrue(self.focused(z, 0))
        self.assertEqual(text[beta + 2][0], ">")
        self.assertEqual(text[beta][0], " ")
        self.mouse(replay, z, ptr.wheel(10, 5, 1))
        text = self.screen_text(z)
        self.assertEqual((text[beta + 1][0], text[beta + 2][0]), (">", " "))

    def test_mouse_in_the_viewer_and_the_help_screen(self):
        replay, z = self.start_with_host()
        ptr = self.pointer
        page = self.row_of(z, "PAGE.TXT", 0)
        alpha = self.row_of(z, "ALPHA.TXT", 0)

        # A double click opens a file in the viewer.  The wheel pages
        # (one page per event) and a right click returns.
        self.mouse(replay, z, ptr.click(5, page) + ptr.click(5, page))
        text = self.screen_text(z)
        self.assertIn("TEXT viewer", text[24])
        self.assertIn("offset 00000000", text[24])
        self.assertEqual(text[1].rstrip(), "line00")
        self.mouse(replay, z, ptr.wheel(40, 12, -1))
        text = self.screen_text(z)
        self.assertNotIn("offset 00000000", text[24])
        self.assertEqual(text[1].rstrip(), "line22")
        self.mouse(replay, z, ptr.wheel(40, 12, 1))
        text = self.screen_text(z)
        self.assertIn("offset 00000000", text[24])
        self.assertEqual(text[1].rstrip(), "line00")
        self.mouse(replay, z, ptr.right_click(40, 12))
        text = self.screen_text(z)
        self.assertNotIn("viewer", text[24])
        self.assertIn("ALPHA.TXT", text[alpha])

        # A click on the Help button shows the help; the button's release
        # does not dismiss it, a later press does.
        self.mouse(replay, z, ptr.click(2, 24))
        self.assertIn("KonaCommander help", self.screen_text(z)[0])
        self.mouse(replay, z, ptr.click(20, 10))
        text = self.screen_text(z)
        self.assertNotIn("KonaCommander help", text[0])
        self.assertIn("ALPHA.TXT", text[alpha])

    def test_mouse_in_the_menus_and_the_buttons(self):
        replay, z = self.start_with_host()
        ptr = self.pointer
        alpha = self.row_of(z, "ALPHA.TXT", 0)

        # A click on a title opens its pop-up, a click on an item runs it
        # (View > View hex), a click outside the pop-up closes it.
        self.mouse(replay, z, ptr.click(11, 0))
        text = self.screen_text(z)
        self.assertIn("View text", text[2])
        self.assertIn("View hex", text[3])
        self.mouse(replay, z, ptr.click(13, 3))
        text = self.screen_text(z)
        self.assertIn("HEX viewer", text[24])
        self.assertIn("616C7068610A", text[1].replace(" ", ""))
        self.mouse(replay, z, ptr.right_click(40, 12))
        self.assertNotIn("viewer", self.screen_text(z)[24])
        self.mouse(replay, z, ptr.click(2, 0))
        self.assertIn("Open", self.screen_text(z)[2])
        self.mouse(replay, z, ptr.click(60, 12))
        text = self.screen_text(z)
        self.assertNotIn("Open", text[2][:25])
        self.assertIn("ALPHA.TXT", text[alpha])

        # A click on a button runs its action: View opens the selected
        # file, Quit leaves, and the pointer is gone.
        self.mouse(replay, z, ptr.click(12, 24))
        self.assertIn("TEXT viewer", self.screen_text(z)[24])
        self.mouse(replay, z, ptr.right_click(40, 12))
        start = len(z.glyphs)
        self.mouse(replay, z, ptr.move(55, 24) + ptr.press(), extra=0)
        replay.run_to_pattern(z, r"/\n?> ", since=start)
        self.assertEqual(self.pointer_entry(z), POINTER_HIDDEN)

    def test_mouse_answers_the_copy_confirmation(self):
        replay, z = self.start_with_host()
        ptr = self.pointer
        alpha = self.row_of(z, "ALPHA.TXT", 0)

        # Copy asks first; a click on NO declines, a click on YES copies
        # ALPHA.TXT to the PC root.
        self.mouse(replay, z, ptr.click(5, alpha) + ptr.click(31, 24))
        text = self.screen_text(z)
        self.assertIn("Copy file", text[5])
        self.mouse(replay, z, ptr.click(44, 19))
        text = self.screen_text(z)
        self.assertIn("Copy cancelled", text[23])
        self.assertFalse((self.root / "ALPHA.TXT").exists())
        self.mouse(replay, z, ptr.click(31, 24))
        self.assertIn("Copy file", self.screen_text(z)[5])
        self.mouse(replay, z, ptr.click(36, 19))
        text = self.screen_text(z)
        self.assertIn("Copy to PC complete", text[23])
        self.assertEqual((self.root / "ALPHA.TXT").read_bytes(), b"alpha\n")
        self.assertIn("ALPHA.TXT", "\n".join(row[43:] for row in text[2:22]))


if __name__ == "__main__":
    unittest.main()
