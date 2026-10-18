#!/usr/bin/env python3
"""TUI SDK screen-engine tests.

`libtui` is a separately linked user-space library: the public header is the
complete v1 core surface with fixed numeric values, the archive keeps one
coherent function group per member, and a program opts in with `LIBS=tui`.
These tests pin the screen engine:

* `user/sdk/include/tui.h` declares the whole core surface, uses only public
  SDK headers, and carries the architecture's key, style and frame values;
* `libtui.lib` contains exactly the storage, screen, lifecycle, flush and
  formatter members and is packed without build dates;
* one static cell map and one static dirty bitmap back the screen engine,
  and the formatter stays in its own member;
* `tui_test.exe` links the TUI archive after its own objects and before
  `libkos`, and its link map carries the TUI members;
* `hello.exe` links no TUI member or symbol.

Skipped when the artifacts have not been built.
"""

import os
import re
import sys
import unittest
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

TUI_HEADER = os.path.join("user", "sdk", "include", "tui.h")
LIBTUI = os.path.join("build", "user", "sdk", "libtui.lib")
TUI_TEST_DIR = os.path.join("build", "user", "tui_test")
TUI_TEST_LK = os.path.join(TUI_TEST_DIR, "tui_test.lk")
TUI_TEST_MAP = os.path.join(TUI_TEST_DIR, "tui_test.map")
HELLO_DIR = os.path.join("build", "user", "hello")
HELLO_LK = os.path.join(HELLO_DIR, "hello.lk")
HELLO_MAP = os.path.join(HELLO_DIR, "hello.map")

# The complete core surface from the architecture's tui.h.
TUI_FUNCTIONS = {
    "tui_init", "tui_shutdown", "tui_flush", "tui_begin", "tui_end",
    "tui_get_size",
    "tui_gotoxy", "tui_wherex", "tui_wherey",
    "tui_cursor_show", "tui_cursor_hide",
    "tui_putc", "tui_puts", "tui_write", "tui_putc_xy", "tui_puts_xy",
    "tui_printf", "tui_printf_xy",
    "tui_clear", "tui_clreol", "tui_clrline",
    "tui_insert_line", "tui_delete_line",
    "tui_window", "tui_reset_window", "tui_push_window", "tui_pop_window",
    "tui_set_style", "tui_get_style", "tui_set_attr", "tui_get_attr",
    "tui_fill", "tui_hline", "tui_vline", "tui_frame", "tui_frame_title",
    "tui_key_pending", "tui_getkey", "tui_pollkey",
    "tui_saved_size", "tui_save", "tui_restore",
}

TUI_STYLES = {
    "TUI_STYLE_NORMAL": 0,
    "TUI_STYLE_TITLE": 1,
    "TUI_STYLE_SELECTED": 2,
    "TUI_STYLE_DISABLED": 3,
    "TUI_STYLE_ERROR": 4,
    "TUI_STYLE_INPUT": 5,
    "TUI_STYLE_STATUS": 6,
    "TUI_STYLE_ACCENT": 7,
    "TUI_STYLE_COUNT": 8,
}

TUI_FRAMES = {
    "TUI_FRAME_NONE": 0,
    "TUI_FRAME_SINGLE": 1,
    "TUI_FRAME_DOUBLE": 2,
}

TUI_KEYS = {
    "TUI_KEY_NONE": 0x0000,
    "TUI_KEY_ESC": 0x001B,
    "TUI_KEY_UP": 0x0101,
    "TUI_KEY_DOWN": 0x0102,
    "TUI_KEY_LEFT": 0x0103,
    "TUI_KEY_RIGHT": 0x0104,
    "TUI_KEY_HOME": 0x0110,
    "TUI_KEY_END": 0x0111,
    "TUI_KEY_PGUP": 0x0112,
    "TUI_KEY_PGDN": 0x0113,
    "TUI_KEY_INSERT": 0x0114,
    "TUI_KEY_DELETE": 0x0115,
    "TUI_KEY_F1": 0x0121,
    "TUI_KEY_F2": 0x0122,
    "TUI_KEY_F3": 0x0123,
    "TUI_KEY_F4": 0x0124,
    "TUI_KEY_F5": 0x0125,
    "TUI_KEY_F6": 0x0126,
    "TUI_KEY_F7": 0x0127,
    "TUI_KEY_F8": 0x0128,
    "TUI_KEY_F9": 0x0129,
    "TUI_KEY_F10": 0x012A,
    "TUI_KEY_F11": 0x012B,
    "TUI_KEY_F12": 0x012C,
}

# One archive member per coherent function group; later parts of the API
# add their own members instead of growing these.
TUI_MEMBERS = {"tui_state.rel", "tui_screen.rel", "tui_lifecycle.rel",
               "tui_flush.rel", "tui_printf.rel"}

# The cell map and dirty bitmap live in the storage member; these are the
# only two full-screen arrays.
TUI_STORAGE_SYMBOLS = ("_tui_cells", "_tui_dirty")

FORBIDDEN_INCLUDE_HEADS = ("kernel", "drivers", "formats", "platform",
                           "boot", "config", "linker")


def _read(rel_or_abs):
    path = rel_or_abs
    if not os.path.isabs(path):
        path = os.path.join(ROOT, path)
    return Path(path).read_text()


def _symbols(rel_map):
    """Parse an sdldz80 map into {symbol: (address, module)}."""
    table = {}
    pattern = re.compile(r"^\s*([0-9A-Fa-f]{8})\s+(\S+)\s*(\S*)\s*$")
    for line in _read(rel_map).splitlines():
        match = pattern.match(line)
        if match is None:
            continue
        value, name, module = match.groups()
        if name.startswith("."):
            continue
        table[name] = (int(value, 16), module)
    return table


def _linked_modules(symbols):
    return {module for _address, module in symbols.values() if module}


def _ar_members(path):
    """(name, mtime, uid, gid) for each member of a `sdar`/ar archive.

    `sdar` stores names past the 16-byte header field in a `//` string
    table and refers to them as `/offset`."""
    data = Path(path).read_bytes()
    assert data[:8] == b"!<arch>\n", "%s is not an ar archive" % path
    members = []
    strings = ""
    at = 8
    while at + 60 <= len(data):
        head = data[at:at + 60]
        name = head[0:16].decode("ascii").strip()
        size = int(head[48:58].decode("ascii").strip())
        if name == "//":
            strings = data[at + 60:at + 60 + size].decode("ascii")
        else:
            if name.startswith("/") and name[1:].isdigit():
                offset = int(name[1:])
                name = strings[offset:strings.index("\n", offset)]
                name = name.rstrip("/")
            elif name.endswith(".rel/"):
                name = name[:-1]
            if name != "/":
                members.append((name, head[16:28].decode("ascii").strip(),
                                head[28:34].decode("ascii").strip(),
                                head[34:40].decode("ascii").strip()))
        at += 60 + size + (size & 1)
    return members


def _enum_values(text):
    """The explicit and implied numeric values of every enum block."""
    values = {}
    for block in re.findall(r"enum\s*\{(.*?)\};", text, re.S):
        following = 0
        for entry in block.split(","):
            entry = entry.strip()
            if not entry:
                continue
            if "=" in entry:
                name, value = entry.split("=", 1)
                name = name.strip()
                value = value.strip()
                following = int(value, 0)
            else:
                name = entry
            values[name] = following
            following += 1
    return values


def _quoted_includes(path):
    return re.findall(r'#\s*include\s+"([^"]+)"', _read(path))


class TestTuiHeader(unittest.TestCase):
    """The public core header: complete surface, public includes, values."""

    @classmethod
    def setUpClass(cls):
        path = os.path.join(ROOT, TUI_HEADER)
        if not os.path.isfile(path):
            raise unittest.SkipTest("%s is missing" % TUI_HEADER)
        cls.text = _read(TUI_HEADER)

    def test_declares_the_complete_v1_core_surface(self):
        declared = set(re.findall(r"\b(tui_[a-z0-9_]+)\s*\(", self.text))
        self.assertEqual(declared, TUI_FUNCTIONS)

    def test_includes_only_public_sdk_headers(self):
        includes = _quoted_includes(TUI_HEADER)
        self.assertTrue(includes, "tui.h includes no SDK header")
        for include in includes:
            self.assertNotIn("<", include, include)
            self.assertFalse(include.startswith("/"), include)
            self.assertNotIn("..", include, include)
            self.assertNotIn(include.split("/")[0], FORBIDDEN_INCLUDE_HEADS,
                             "tui.h includes %s" % include)

    def test_style_frame_and_key_values_are_the_architecture_values(self):
        values = _enum_values(self.text)
        for expected in (TUI_STYLES, TUI_FRAMES, TUI_KEYS):
            for name, value in expected.items():
                self.assertIn(name, values, "tui.h defines no %s" % name)
                self.assertEqual(values[name], value, name)

    def test_cell_and_geometry_types_are_public(self):
        for declaration in ("typedef uint16_t tui_key_t",
                            "typedef uint8_t  tui_style_t",
                            "typedef uint8_t  tui_frame_style_t",
                            "struct tui_size",
                            "struct tui_rect",
                            "struct tui_cell"):
            self.assertIn(declaration, self.text)


class TestTuiLibrary(unittest.TestCase):
    """libtui: coherent members, deterministic archive, public boundary."""

    @classmethod
    def setUpClass(cls):
        path = os.path.join(ROOT, LIBTUI)
        if not os.path.isfile(path):
            raise unittest.SkipTest("%s has not been built" % LIBTUI)
        cls.members = _ar_members(path)
        cls.path = path

    def test_archive_holds_the_foundation_function_groups(self):
        names = [name for name, _mtime, _uid, _gid in self.members]
        self.assertEqual(sorted(names), sorted(TUI_MEMBERS))
        self.assertEqual(len(names), len(set(names)))

    def test_archive_carries_no_build_dates(self):
        for name, mtime, uid, gid in self.members:
            for field in (mtime, uid, gid):
                self.assertIn(field, ("", "0"), "%s: %r" % (name, field))

    def test_every_source_is_its_own_member(self):
        source_dir = os.path.join(ROOT, "user", "sdk", "tui", "src")
        sources = sorted(name[:-2] + ".rel"
                         for name in os.listdir(source_dir)
                         if name.endswith(".c"))
        self.assertEqual(sorted(name for name, *_ in self.members), sources)

    def test_sources_include_only_the_public_sdk_and_their_private_header(self):
        source_dir = os.path.join(ROOT, "user", "sdk", "tui", "src")
        for name in sorted(os.listdir(source_dir)):
            if not name.endswith((".c", ".h")):
                continue
            rel = os.path.join(source_dir, name)
            for include in _quoted_includes(rel):
                if name.startswith("tui_") and include == "tui_internal.h":
                    continue
                self.assertIn(include, ("tui.h", "tui_internal.h",
                                        "kos/console.h", "kos/types.h",
                                        "kos/errno.h", "klibc/fmt.h",
                                        "klibc/stdarg.h", "klibc/string.h"),
                              "%s includes %s" % (name, include))

    def test_ansi_sequences_live_only_in_the_output_backend(self):
        source_dir = os.path.join(ROOT, "user", "sdk", "tui", "src")
        permitted = {"tui_lifecycle.c", "tui_flush.c"}
        for name in sorted(os.listdir(source_dir)):
            if not name.endswith((".c", ".h")):
                continue
            body = _read(os.path.join(source_dir, name))
            if name in permitted:
                continue
            self.assertNotIn("\\033[", body,
                             "%s carries a terminal sequence" % name)
        self.assertNotIn("\\033[", _read(os.path.join(
            ROOT, "tests", "tui", "tui_test.c")))
        self.assertNotIn("console_write", _read(os.path.join(
            source_dir, "tui_screen.c")))

    def test_flush_groups_runs_by_attribute_and_emits_both_sgr_forms(self):
        body = _read(os.path.join(ROOT, "user", "sdk", "tui", "src",
                                  "tui_flush.c"))
        self.assertIn("tui_cells[index].attr == attr", body)
        self.assertIn('TUI_SGR_NORMAL "\\033[0m"', body)
        self.assertIn('TUI_SGR_REVERSE "\\033[7m"', body)
        self.assertIn("TUI_CURSOR_SHOW", body)
        self.assertIn("TUI_CURSOR_HIDE", body)


class TestTuiLink(unittest.TestCase):
    """Opting in with LIBS=tui: archive order and link-map consequences."""

    @classmethod
    def setUpClass(cls):
        for path in (TUI_TEST_LK, TUI_TEST_MAP):
            if not os.path.isfile(os.path.join(ROOT, path)):
                raise unittest.SkipTest(
                    "%s has not been built; run "
                    "make build/user/tui_test.exe" % path)
        cls.symbols = _symbols(TUI_TEST_MAP)
        cls.lk = _read(TUI_TEST_LK).splitlines()

    def test_program_objects_precede_the_named_archive_and_libkos(self):
        object_line = "build/user/tui_test/tui_test.rel"
        tui_line = "-l build/user/sdk/libtui.lib"
        kos_line = "-l build/user/sdk/libkos.lib"
        for line in (object_line, tui_line, kos_line):
            self.assertIn(line, self.lk)
        self.assertLess(self.lk.index(object_line), self.lk.index(tui_line))
        self.assertLess(self.lk.index(tui_line), self.lk.index(kos_line))

    def test_tui_test_links_the_screen_members(self):
        modules = _linked_modules(self.symbols)
        for member in ("tui_state", "tui_screen", "tui_lifecycle",
                       "tui_flush", "tui_printf"):
            self.assertIn(member, modules)
        self.assertIn("_tui_init", self.symbols)
        self.assertIn("_tui_get_size", self.symbols)
        self.assertIn("_tui_flush", self.symbols)
        self.assertIn("_tui_putc", self.symbols)
        self.assertIn("_tui_printf", self.symbols)

    def test_one_cell_map_and_one_dirty_bitmap(self):
        for name in TUI_STORAGE_SYMBOLS:
            self.assertIn(name, self.symbols)
        self.assertEqual(
            len([name for name in self.symbols
                 if name.startswith("_tui_cells")]), 1)
        self.assertEqual(
            len([name for name in self.symbols
                 if name.startswith("_tui_dirty")]), 1)
        cells = self.symbols["_tui_cells"][0]
        dirty = self.symbols["_tui_dirty"][0]
        run = self.symbols["_tui_run"][0]
        self.assertEqual(dirty - cells, 85 * 26 * 2)
        self.assertEqual(run - dirty, (85 * 26 + 7) // 8)

    def test_the_formatter_is_its_own_member(self):
        for name in ("_tui_printf", "_tui_printf_xy"):
            self.assertEqual(self.symbols[name][1], "tui_printf")

    def test_hello_has_no_tui_member_or_symbol(self):
        for path in (HELLO_LK, HELLO_MAP):
            if not os.path.isfile(os.path.join(ROOT, path)):
                self.skipTest("%s has not been built; run "
                              "make build/user/hello.exe" % path)
        self.assertNotIn("-l build/user/sdk/libtui.lib",
                         _read(HELLO_LK).splitlines())
        symbols = _symbols(HELLO_MAP)
        modules = _linked_modules(symbols)
        self.assertFalse([name for name in modules
                          if name.startswith("tui_")],
                         "hello.exe links a TUI member")
        self.assertFalse([name for name in symbols
                          if name.startswith("_tui_")],
                         "hello.exe links a TUI symbol")


if __name__ == "__main__":
    unittest.main()
