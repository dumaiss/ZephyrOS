#!/usr/bin/env python3
"""TUI SDK screen-engine tests.

`libtui` is a separately linked user-space library: the public header is the
complete v1 core surface with fixed numeric values, the archive keeps one
coherent function group per member, and a program opts in with `LIBS=tui`.
These tests pin the screen engine:

* `user/sdk/include/tui.h` declares the whole core surface, uses only public
  SDK headers, and carries the architecture's key, style and frame values;
* `libtui.lib` contains the storage, screen, lifecycle, flush, formatter,
  window, style, key and region members and is packed without build dates;
* one static cell map, dirty bitmap and invert bitmap back the screen engine,
  and the optional groups stay in their own members;
* `tui_test.exe` links the widget archive before `libtui` and both before
  `libkos`, and its link map carries the TUI members;
* a core-only probe omits the key, region, style, window and widget members;
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
WIDGET_HEADER = os.path.join("user", "sdk", "include", "tui_widgets.h")
LIBTUI_WIDGETS = os.path.join("build", "user", "sdk", "libtui_widgets.lib")
TUI_TEST_DIR = os.path.join("build", "user", "tui_test")
TUI_TEST_LK = os.path.join(TUI_TEST_DIR, "tui_test.lk")
TUI_TEST_MAP = os.path.join(TUI_TEST_DIR, "tui_test.map")
HELLO_DIR = os.path.join("build", "user", "hello")
HELLO_LK = os.path.join(HELLO_DIR, "hello.lk")
HELLO_MAP = os.path.join(HELLO_DIR, "hello.map")
CORE_PROBE_DIR = os.path.join("build", "user", "tui_core_probe")
CORE_PROBE_LK = os.path.join(CORE_PROBE_DIR, "tui_core_probe.lk")
CORE_PROBE_MAP = os.path.join(CORE_PROBE_DIR, "tui_core_probe.map")
WIDGET_PROBE_DIR = os.path.join("build", "user", "tui_widget_probe")
WIDGET_PROBE_LK = os.path.join(WIDGET_PROBE_DIR, "tui_widget_probe.lk")
WIDGET_PROBE_MAP = os.path.join(WIDGET_PROBE_DIR, "tui_widget_probe.map")

# The complete core surface from the architecture's tui.h.
TUI_FUNCTIONS = {
    "tui_init", "tui_shutdown", "tui_flush", "tui_begin", "tui_end",
    "tui_get_size",
    "tui_gotoxy", "tui_wherex", "tui_wherey",
    "tui_cursor_show", "tui_cursor_hide",
    "tui_putc", "tui_puts", "tui_write", "tui_putc_xy", "tui_puts_xy",
    "tui_put_text",
    "tui_printf", "tui_printf_xy",
    "tui_clear", "tui_clreol", "tui_clrline",
    "tui_insert_line", "tui_delete_line", "tui_scroll_rows",
    "tui_window", "tui_reset_window", "tui_get_window",
    "tui_push_window", "tui_pop_window",
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
               "tui_flush.rel", "tui_printf.rel", "tui_window.rel",
               "tui_style.rel", "tui_key.rel", "tui_region.rel",
               "tui_scroll.rel", "tui_text.rel"}

WIDGET_MEMBERS = {"widget_common.rel", "widget_render.rel",
                  "widget_menu.rel", "widget_input.rel",
                  "widget_modal.rel"}

WIDGET_FUNCTIONS = {
    "tui_statusbar", "tui_menubar", "tui_message_box", "tui_confirm",
    "tui_button", "tui_label", "tui_input", "tui_checkbox",
    "tui_listbox", "tui_menu_run", "tui_input_run",
}

# The cell map and two classification bitmaps live in the storage member.
TUI_STORAGE_SYMBOLS = ("_tui_chars", "_tui_attrs", "_tui_dirty", "_tui_invert")

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

    def test_render_abi_is_pinned(self):
        protocol = _read(os.path.join("abi", "kos", "protocol.h"))
        render = _read(os.path.join("abi", "kos", "console_render.h"))

        def value(name):
            match = re.search(r"(?m)^#define\s+%s\s+(\S+)" % name, render)
            self.assertIsNotNone(match, name)
            return int(match.group(1).rstrip("u"), 0)

        self.assertEqual(
            int(re.search(r"(?m)^#define\s+KCONSOLE_OP_RENDER\s+(\S+)",
                          protocol).group(1).rstrip("u"), 0), 6)
        self.assertEqual(
            int(re.search(r"(?m)^#define\s+KVIDEO_OP_RENDER\s+(\S+)",
                          protocol).group(1).rstrip("u"), 0), 4)
        self.assertEqual(value("KCONSOLE_RENDER_VERSION"), 1)
        self.assertEqual(value("KCONSOLE_RENDER_TEXT"), 1)
        self.assertEqual(value("KCONSOLE_RENDER_INVERT"), 2)
        self.assertEqual(value("KCONSOLE_RENDER_STATE"), 3)
        self.assertEqual(value("KCONSOLE_RENDER_SCROLL"), 4)
        self.assertEqual(value("KCONSOLE_RENDER_HEADER_SIZE"), 4)
        self.assertEqual(value("KCONSOLE_RENDER_SPAN_BASE"), 6)
        self.assertEqual(value("KCONSOLE_RENDER_STATE_SIZE"), 6)
        self.assertEqual(value("KCONSOLE_RENDER_SCROLL_SIZE"), 6)
        for field in ("uint8_t version;", "uint8_t flags;",
                      "uint16_t total_length;"):
            self.assertIn(field, render)
        console = _read(os.path.join("user", "sdk", "include", "kos",
                                     "console.h"))
        self.assertIn("console_render(const void *stream", console)
        # The grid is reported, not fixed: the info operations and their
        # reply record, and no grid constant in the render ABI.
        self.assertEqual(
            int(re.search(r"(?m)^#define\s+KCONSOLE_OP_INFO\s+(\S+)",
                          protocol).group(1).rstrip("u"), 0), 7)
        self.assertEqual(
            int(re.search(r"(?m)^#define\s+KVIDEO_OP_INFO\s+(\S+)",
                          protocol).group(1).rstrip("u"), 0), 5)
        self.assertEqual(value("KCONSOLE_INFO_SIZE"), 8)
        self.assertEqual(value("KCONSOLE_ATTR_REVERSE"), 1)
        self.assertEqual(value("KCONSOLE_INFO_RENDER"), 1)
        for field in ("uint8_t cols;", "uint8_t rows;", "uint8_t attr_bits;",
                      "uint8_t reserved[4];"):
            self.assertIn(field, render)
        self.assertNotIn("KCONSOLE_RENDER_COLS", render)
        self.assertIn("int console_info(struct kconsole_info *out);",
                      console)


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
        sources = sorted(name.rsplit(".", 1)[0] + ".rel"
                         for name in os.listdir(source_dir)
                         if name.endswith((".c", ".asm")))
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
                           "kos.h", "kos/console.h",
                           "kos/console_render.h",
                                        "kos/types.h", "kos/errno.h",
                                        "klibc/fmt.h", "klibc/stdarg.h",
                                        "klibc/string.h"),
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

    def test_cp850_box_bytes_live_only_in_the_style_member(self):
        source_dir = os.path.join(ROOT, "user", "sdk", "tui", "src")
        for name in sorted(os.listdir(source_dir)):
            if not name.endswith((".c", ".h")) or name == "tui_style.c":
                continue
            body = _read(os.path.join(source_dir, name))
            for glyph in ("0xC4u", "0xB3u", "0xDAu", "0xC9u", "0xCDu"):
                self.assertNotIn(glyph, body,
                                 "%s embeds CP850 byte %s" % (name, glyph))
        test_body = _read(os.path.join(ROOT, "tests", "tui", "tui_test.c"))
        for glyph in ("0xC4", "0xC9", "\\xC4", "\\xC9"):
            self.assertNotIn(glyph, test_body)

    def test_flush_groups_runs_by_attribute_and_emits_both_sgr_forms(self):
        body = _read(os.path.join(ROOT, "user", "sdk", "tui", "src",
                                  "tui_flush.c"))
        self.assertIn("tui_priv_attr(index) == attr", body)
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

    def test_program_objects_precede_the_named_archives_and_libkos(self):
        object_line = "build/user/tui_test/tui_test.rel"
        widget_line = "-l build/user/sdk/libtui_widgets.lib"
        tui_line = "-l build/user/sdk/libtui.lib"
        kos_line = "-l build/user/sdk/libkos.lib"
        for line in (object_line, widget_line, tui_line, kos_line):
            self.assertIn(line, self.lk)
        self.assertLess(self.lk.index(object_line),
                        self.lk.index(widget_line))
        self.assertLess(self.lk.index(widget_line), self.lk.index(tui_line))
        self.assertLess(self.lk.index(tui_line), self.lk.index(kos_line))

    def test_tui_test_links_the_screen_members(self):
        modules = _linked_modules(self.symbols)
        for member in ("tui_state", "tui_screen", "tui_lifecycle",
                       "tui_flush", "tui_printf", "tui_window",
                       "tui_style", "tui_key", "tui_region"):
            self.assertIn(member, modules)
        for symbol in ("_tui_init", "_tui_get_size", "_tui_flush",
                       "_tui_putc", "_tui_printf", "_tui_clear",
                       "_tui_set_style", "_tui_getkey", "_tui_save",
                       "_tui_restore", "_tui_statusbar", "_tui_menu_run",
                       "_tui_input_run", "_tui_message_box",
                       "_tui_confirm"):
            self.assertIn(symbol, self.symbols)

    def test_every_public_declaration_has_a_linked_definition(self):
        for name in TUI_FUNCTIONS | WIDGET_FUNCTIONS:
            self.assertIn("_" + name, self.symbols)
        public = _read(os.path.join(ROOT, TUI_HEADER))
        public += _read(os.path.join(ROOT, WIDGET_HEADER))
        self.assertNotIn("tui_priv_", public)
        self.assertNotIn("widget_", public)

    def test_one_cell_map_and_two_classification_bitmaps(self):
        """The 85 x 26 cell map: one byte per character and four bits per
        attribute, then the two classification bitmaps."""
        for name in TUI_STORAGE_SYMBOLS:
            self.assertIn(name, self.symbols)
        for prefix in ("_tui_chars", "_tui_attrs"):
            self.assertEqual(
                len([name for name in self.symbols
                     if name.startswith(prefix)]), 1)
        self.assertEqual(
            len([name for name in self.symbols
                 if name.startswith("_tui_dirty")]), 1)
        self.assertEqual(
            len([name for name in self.symbols
                 if name.startswith("_tui_invert")]), 1)
        chars = self.symbols["_tui_chars"][0]
        attrs = self.symbols["_tui_attrs"][0]
        dirty = self.symbols["_tui_dirty"][0]
        invert = self.symbols["_tui_invert"][0]
        run = self.symbols["_tui_run"][0]
        self.assertEqual(attrs - chars, 86 * 26)
        self.assertEqual(dirty - attrs, (86 * 26 + 1) // 2)
        self.assertEqual(invert - dirty, (86 * 26 + 7) // 8)
        self.assertEqual(run - invert, (86 * 26 + 7) // 8)
        self.assertNotIn("_tui_render", self.symbols)

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


class TestWidgetHeader(unittest.TestCase):
    """The optional widget header: exact v1 surface and pinned rules."""

    @classmethod
    def setUpClass(cls):
        path = os.path.join(ROOT, WIDGET_HEADER)
        if not os.path.isfile(path):
            raise unittest.SkipTest("%s is missing" % WIDGET_HEADER)
        cls.text = _read(path)

    def test_declares_the_v1_widget_surface(self):
        declared = set(re.findall(r"\b(tui_[a-z0-9_]+)\s*\(", self.text))
        self.assertEqual(declared, WIDGET_FUNCTIONS)

    def test_includes_only_the_public_core_header(self):
        self.assertEqual(_quoted_includes(os.path.join(ROOT, WIDGET_HEADER)),
                         ["tui.h"])

    def test_button_mask_and_ids_are_pinned(self):
        for name in ("TUI_ID_NONE", "TUI_ID_OK", "TUI_ID_CANCEL",
                     "TUI_ID_YES", "TUI_ID_NO"):
            self.assertIn("#define %s" % name, self.text)
        self.assertIn("#define TUI_BUTTON(id) ((uint16_t)(1u << (id)))",
                      self.text)


class TestWidgetLibrary(unittest.TestCase):
    """libtui_widgets: deterministic members and public-only boundary."""

    @classmethod
    def setUpClass(cls):
        path = os.path.join(ROOT, LIBTUI_WIDGETS)
        if not os.path.isfile(path):
            raise unittest.SkipTest("%s has not been built" % LIBTUI_WIDGETS)
        cls.members = _ar_members(path)
        cls.path = path

    def test_archive_holds_the_widget_members(self):
        names = [name for name, _mtime, _uid, _gid in self.members]
        self.assertEqual(sorted(names), sorted(WIDGET_MEMBERS))
        self.assertEqual(len(names), len(set(names)))

    def test_archive_carries_no_build_dates(self):
        for name, mtime, uid, gid in self.members:
            for field in (mtime, uid, gid):
                self.assertIn(field, ("", "0"), "%s: %r" % (name, field))

    def test_every_source_is_its_own_member(self):
        source_dir = os.path.join(ROOT, "user", "sdk", "widgets", "src")
        sources = sorted(name[:-2] + ".rel"
                         for name in os.listdir(source_dir)
                         if name.endswith(".c"))
        self.assertEqual(sorted(name for name, *_ in self.members), sources)

    def test_widget_sources_use_only_public_headers_and_own_helpers(self):
        source_dir = os.path.join(ROOT, "user", "sdk", "widgets", "src")
        for name in sorted(os.listdir(source_dir)):
            if not name.endswith((".c", ".h")):
                continue
            rel = os.path.join(source_dir, name)
            for include in _quoted_includes(rel):
                if name.endswith(".c") and include == "widget_internal.h":
                    continue
                if name == "widget_internal.h" and include == "tui_widgets.h":
                    continue
                self.assertIn(include, ("tui.h", "tui_widgets.h"),
                              "%s includes %s" % (name, include))

    def test_widgets_embed_no_terminal_cp850_or_os_knowledge(self):
        source_dir = os.path.join(ROOT, "user", "sdk", "widgets", "src")
        for name in sorted(os.listdir(source_dir)):
            if not name.endswith((".c", ".h")):
                continue
            body = _read(os.path.join(source_dir, name))
            self.assertNotIn("\\033[", body, name)
            for glyph in ("0xC4", "0xB3", "0xDA", "0xC9", "0xCD",
                          "0xBF", "0xD9", "0xBB", "0xBC"):
                self.assertNotIn(glyph, body, "%s embeds %s" % (name, glyph))
            for call in ("console_read", "console_write", "console_poll(",
                         "idle(", "bind(", "call(", "malloc(", "free("):
                self.assertNotIn(call, body, "%s calls %s" % (name, call))


class TestTuiProbeLinks(unittest.TestCase):
    """Core-only and render-only programs: no unused member or data cost."""

    @classmethod
    def setUpClass(cls):
        for path in (CORE_PROBE_LK, CORE_PROBE_MAP,
                     WIDGET_PROBE_LK, WIDGET_PROBE_MAP):
            if not os.path.isfile(os.path.join(ROOT, path)):
                raise unittest.SkipTest("%s has not been built" % path)
        cls.core_symbols = _symbols(CORE_PROBE_MAP)
        cls.widget_symbols = _symbols(WIDGET_PROBE_MAP)

    def test_core_probe_omits_key_region_style_and_widget_members(self):
        modules = _linked_modules(self.core_symbols)
        for absent in ("tui_key", "tui_region", "tui_style", "tui_window",
                       "tui_printf"):
            self.assertNotIn(absent, modules)
        for symbol in ("_tui_getkey", "_tui_pollkey", "_tui_key_pending",
                       "_tui_save", "_tui_restore", "_tui_saved_size",
                       "_tui_fill", "_tui_frame", "_tui_set_style",
                       "_tui_window", "_tui_clear"):
            self.assertNotIn(symbol, self.core_symbols)
        for module in WIDGET_MEMBERS:
            self.assertNotIn(module[:-4], modules)
        self.assertNotIn("-l build/user/sdk/libtui_widgets.lib",
                         _read(os.path.join(ROOT, CORE_PROBE_LK)).splitlines())

    def test_widget_probe_links_only_render_members(self):
        modules = _linked_modules(self.widget_symbols)
        for present in ("widget_render", "widget_common"):
            self.assertIn(present, modules)
        for absent in ("widget_menu", "widget_input", "widget_modal"):
            self.assertNotIn(absent, modules)
        for symbol in ("_tui_statusbar", "_tui_label", "_tui_button"):
            self.assertIn(symbol, self.widget_symbols)
        for symbol in ("_tui_menu_run", "_tui_input_run", "_tui_message_box",
                       "_tui_confirm"):
            self.assertNotIn(symbol, self.widget_symbols)
        lk = _read(os.path.join(ROOT, WIDGET_PROBE_LK)).splitlines()
        self.assertLess(lk.index("-l build/user/sdk/libtui_widgets.lib"),
                        lk.index("-l build/user/sdk/libtui.lib"))

    def test_modal_member_has_a_measurable_data_cost(self):
        def data_size(path):
            for line in _read(os.path.join(ROOT, path)).splitlines():
                match = re.match(r"^_DATA\s+([0-9A-Fa-f]+)\s+"
                                 r"([0-9A-Fa-f]+)", line)
                if match:
                    return int(match.group(2), 16)
            self.fail("no _DATA area in %s" % path)

        core = data_size(CORE_PROBE_MAP)
        render = data_size(WIDGET_PROBE_MAP)
        with_modal = data_size(TUI_TEST_MAP)
        self.assertEqual(core, render,
                         "render-only widgets add static data")
        self.assertGreaterEqual(with_modal - render, 60 * 16 * 2,
                                "the modal save buffer is not in the map")


if __name__ == "__main__":
    unittest.main()
