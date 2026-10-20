#!/usr/bin/env python3
"""Native shell host tests.

The shell (`user/shell/`) is an ordinary EXE, so its parts are exercised
the way any program's are: the linked image runs in the host Z80 with
staged buffers, and the packed artifact is read back from its own report.
Covered here:

* the splitter round trip - `splitargs` reads back exactly the words
  `shell_serialize` wrote, empty words and all three quoting forms
  included;
* path translation and display, every case of the shell's syntax;
* the native error table;
* the artifact: one packed EXE of the SDK rule, no far segments, not
  staged into any system file.

The whole-session replay is tests/test_shell_sim.py.
"""

import json
import os
import re
import sys
import unittest
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import exepack  # noqa: E402
import g6pack  # noqa: E402
import ihx  # noqa: E402
from z80sim import Z80, read_symbols  # noqa: E402

RET = 0x1234
STACK = 0xFFFE


def signed(value):
    """A 16-bit DE result read back as the `int` the callee returned."""
    return value - 0x10000 if value >= 0x8000 else value

SHELL_OBJDIR = os.path.join("build", "user", "shell")
SHELL_JSON = os.path.join(SHELL_OBJDIR, "shell.json")
SHELL_MAP = os.path.join(SHELL_OBJDIR, "shell.map")
SHELL_REPORT = os.path.join(SHELL_OBJDIR, "shell.report.json")
SHELL_STAGED = os.path.join("build", "modules", "shell.exe")

# The shell's own members (one file per part) and the SDK members it has
# no business in: the segment and extended-memory calls, and the
# diagnostic gate belong to later packages.  The file built-ins pull in
# the SDK's FILE wrapper and the namespace wrapper's `list`.
SHELL_MODULES = {"main", "parse", "path", "lookup", "builtin", "fscmd",
                 "banner", "logo", "fs"}
UNUSED_MODULES = ("fsgate", "diaggate", "seg", "xmem", "xload",
                  "xmemgate")

# Staging buffers inside the program's bank: above the image and BSS of
# the linked shell, below its stack (the artifact test proves the gap).
LINE = 0x6800
ARGV = 0x6900
TAIL = 0x6A00
OUT = 0x6C00
WORDS = 0x6E00
RECORDS = 0x7800
# The buffered console's capture: `putchar` appends its byte here (the
# console flushes a line at a time, so a host test cannot read the
# messages out of a call).
STREAM = 0x7000
STREAM_LEN = 0x7200


def _symbols(rel_map):
    """Parse an sdldz80 map into {symbol: (address, module)}."""
    table = {}
    pattern = re.compile(r"^\s*([0-9A-Fa-f]{8})\s+(\S+)\s+(\S*)\s*$")
    with open(os.path.join(ROOT, rel_map)) as handle:
        for line in handle:
            match = pattern.match(line)
            if match is None:
                continue
            value, name, module = match.groups()
            if name.startswith("."):
                continue
            table[name] = (int(value, 16), module)
    return table


class ShellImage(unittest.TestCase):
    """The linked shell image in the application bank, callable as a
    program: arguments in HL/DE, the rest on the stack, the result in DE
    (the SDK's sdcccall(1) convention, exactly what crt0 and the shell's
    own calls use)."""

    @classmethod
    def setUpClass(cls):
        if not os.path.exists(os.path.join(ROOT, SHELL_JSON)):
            raise unittest.SkipTest("shell.exe has not been built")
        manifest = exepack.load_manifest(os.path.join(ROOT, SHELL_JSON))
        ihx_path = os.path.join(ROOT, SHELL_OBJDIR, manifest["input"])
        cls.image = ihx.parse_ihx(Path(ihx_path).read_text())
        cls.manifest = manifest
        cls.symbols = read_symbols(os.path.join(ROOT, SHELL_MAP))

    def load(self):
        """A machine with the linked shell image in application bank 1."""
        z = Z80(bytearray(b"\xff" * 0x10000), bytearray(b"\xff" * 0x10000))
        z.latch = 0x11  # mode 10: the program runs over application bank 1
        bank = z.banks[1]
        for address, byte in self.image.items():
            bank[address] = byte
        z.sp = STACK
        return z, bank

    def call(self, z, name, hl=0, de=0, stack=()):
        """Call the linked function `name` and return DE.

        sdcccall(1): the first two arguments ride in HL and DE, the rest
        are pushed last-first under the return address, and the callee
        discards them; a 16-bit result comes back in DE."""
        z.hl = hl
        z.de = de
        for value in reversed(stack):
            z.push16(value)
        z.push16(RET)
        z.pc = self.symbols[name]
        for _ in range(400000):
            z.step()
            if z.pc == RET:
                return z.de
        raise AssertionError("%s did not return" % name)

    # The console's buffered path, staged in the program's bank.  A flush
    # succeeds without the IPC gate, and `putchar` appends its byte to the
    # capture buffer and returns it (HL in, DE out: the SDK's convention).
    FLUSH_STUB = bytes((0x11, 0x00, 0x00, 0xC9))   # ld de,#0 ; ret
    PUTCHAR_STUB = bytes((
        0xE5,                    # push hl
        0x2A, STREAM_LEN & 0xFF, STREAM_LEN >> 8,     # ld hl,(STREAM_LEN)
        0x11, STREAM & 0xFF, STREAM >> 8,             # ld de,#STREAM
        0x19,                    # add hl,de          ; the slot
        0xE3,                    # ex (sp),hl         ; hl = the byte
        0x5D, 0x54,              # ld e,l ; ld d,h    ; the result
        0x7D,                    # ld a,l
        0xE1,                    # pop hl             ; the slot
        0x77,                    # ld (hl),a
        0xD5,                    # push de
        0x2A, STREAM_LEN & 0xFF, STREAM_LEN >> 8,     # ld hl,(STREAM_LEN)
        0x23,                    # inc hl
        0x22, STREAM_LEN & 0xFF, STREAM_LEN >> 8,     # ld (STREAM_LEN),hl
        0xD1,                    # pop de
        0xC9,                    # ret
    ))

    def capture_console(self, z, bank):
        """Stub the console's buffered path: `putchar` appends to the
        capture buffer, the flush returns success, and nothing reaches a
        gate.  `_console_write` is stubbed by the caller where its
        "one call" shape matters."""
        for name, code in (("_console_flush", self.FLUSH_STUB),
                           ("_putchar", self.PUTCHAR_STUB)):
            address = self.symbols[name]
            bank[address:address + len(code)] = code
        bank[STREAM_LEN:STREAM_LEN + 2] = b"\x00" * 2

    def streamed(self, bank):
        """The bytes the buffered console collected so far."""
        length = bank[STREAM_LEN] | (bank[STREAM_LEN + 1] << 8)
        return bytes(bank[STREAM:STREAM + length]).decode("ascii")

    def stage(self, bank, address, text):
        raw = text.encode("ascii")
        bank[address:address + len(raw)] = raw
        bank[address + len(raw)] = 0
        return address

    def cstring(self, bank, address):
        chars = []
        while bank[address] != 0:
            chars.append(chr(bank[address]))
            address += 1
        return "".join(chars)

    def split_args(self, z, bank, text, capacity=16):
        """`splitargs` over a staged tail: (argc, argv address)."""
        line = self.stage(bank, LINE, text)
        bank[ARGV:ARGV + 2 * capacity] = b"\x00" * (2 * capacity)
        argc = self.call(z, "_splitargs", hl=line, de=ARGV,
                         stack=(capacity,))
        return argc, ARGV

    def words(self, bank, argv, argc, capacity=16):
        result = []
        for index in range(min(argc, capacity)):
            pointer = (bank[argv + 2 * index]
                       | (bank[argv + 2 * index + 1] << 8))
            result.append(self.cstring(bank, pointer))
        return result

    def stage_argv(self, bank, words):
        """An argv array with `words` staged as separate strings."""
        data = WORDS
        for index, word in enumerate(words):
            raw = word.encode("ascii")
            bank[data:data + len(raw)] = raw
            bank[data + len(raw)] = 0
            bank[ARGV + 2 * index] = data & 0xFF
            bank[ARGV + 2 * index + 1] = data >> 8
            data += len(raw) + 1
        return ARGV

    def serialize(self, z, bank, argv, argc, cap=126):
        """`shell_serialize`: the length (or a negative error)."""
        bank[OUT:OUT + cap] = b"\x00" * cap
        return signed(self.call(z, "_shell_serialize", hl=argv, de=argc,
                                stack=(OUT, cap)))

    def translate(self, z, bank, path):
        bank[TAIL:TAIL + 128] = b"\x00" * 128
        length = signed(self.call(z, "_shell_path_translate",
                                  hl=self.stage(bank, LINE, path),
                                  de=TAIL, stack=(128,)))
        return length, self.cstring(bank, TAIL)

    def display(self, z, bank, path):
        bank[TAIL:TAIL + 128] = b"\x00" * 128
        length = signed(self.call(z, "_shell_path_display",
                                  hl=self.stage(bank, LINE, path),
                                  de=TAIL, stack=(128,)))
        return length, self.cstring(bank, TAIL)


class TestShellSplitterRoundTrip(ShellImage):
    """`splitargs(serialize(argv)) == argv[1..]` (requirement 4)."""

    # One line per quoting form: plain words, both quote forms, the
    # escapes, adjacent pieces and empty words.
    LINES = (
        "one two three",
        "echo 'a b' \"c\\\"d\" e\\ f",
        "echo '' \"\" x",
        "a'b'c \"d\\ne\" f\\\\g",
        "lead\\ trail 'q\\r' \"s\\t\"",
        "empty '' between \"\" words",
        "joined ab\"c d\"e 'f g'h",
    )

    # The tail a word list must serialize to: only a word that needs it
    # is quoted, and `"` and `\` are escaped inside the quotes.
    SERIALIZED = (
        ([], ""),
        (["plain"], "plain"),
        (["two", "words"], "two words"),
        (["a b"], '"a b"'),
        (["a'b"], '"a\'b"'),
        (['a"b'], '"a\\"b"'),
        (["a\\b"], '"a\\\\b"'),
        (["tab\there"], '"tab\there"'),
        ([""], '""'),
        (["a", "", "b"], 'a "" b'),
    )

    def round_trip(self, z, bank, line):
        """Split a line, serialize its tail, split that again."""
        argc, argv = self.split_args(z, bank, line)
        self.assertGreater(argc, 1, line)
        first = self.words(bank, argv, argc)
        length = self.serialize(z, bank, argv, argc)
        tail = self.cstring(bank, OUT)
        self.assertEqual(length, len(tail), line)
        second, argv2 = self.split_args(z, bank, tail)
        return first, second, self.words(bank, argv2, second)

    def test_splitargs_reads_back_what_shell_serialize_wrote(self):
        z, bank = self.load()
        for line in self.LINES:
            first, second, words = self.round_trip(z, bank, line)
            self.assertEqual(second, len(first) - 1,
                             "%r lost a word" % line)
            self.assertEqual(words, first[1:], "%r" % line)

    def test_the_quoting_forms(self):
        z, bank = self.load()
        for tail_words, expected in self.SERIALIZED:
            argv = self.stage_argv(bank, ["cmd"] + tail_words)
            length = self.serialize(z, bank, argv, 1 + len(tail_words))
            self.assertEqual(self.cstring(bank, OUT), expected,
                             repr(tail_words))
            self.assertEqual(length, len(expected), repr(tail_words))

    def test_the_serialized_tail_keeps_empty_words(self):
        z, bank = self.load()
        _first, _second, words = self.round_trip(
            z, bank, "echo a '' b \"\" c")
        self.assertEqual(words, ["a", "", "b", "", "c"])

    def test_a_tail_over_the_limit_is_refused(self):
        z, bank = self.load()
        # 62 + 1 + 62 = 125 bytes fit; one more byte does not.
        argv = self.stage_argv(bank, ["cmd", "a" * 62, "a" * 62])
        self.assertEqual(self.serialize(z, bank, argv, 3), 125)
        self.assertEqual(self.cstring(bank, OUT), "a" * 62 + " " + "a" * 62)
        argv = self.stage_argv(bank, ["cmd", "a" * 63, "a" * 62])
        self.assertEqual(self.serialize(z, bank, argv, 3), -0x02)


class TestShellPathSyntax(ShellImage):
    """Path translation and display (requirement 5)."""

    TRANSLATE = (
        ("/", "/fs"),
        ("/GAMES", "/fs/GAMES"),
        ("/games/AB", "/fs/games/AB"),
        ("/fs", "/fs"),
        ("/FS", "/FS"),
        ("/Fs/bin", "/Fs/bin"),
        ("/system", "/system"),
        ("/SYSTEM/monitor.exe", "/SYSTEM/monitor.exe"),
        ("/systemx", "/fs/systemx"),
        ("/dev", "/dev"),
        ("/dev/console", "/dev/console"),
        ("/Dev/Console", "/Dev/Console"),
        ("//", "/"),
        ("//x", "/x"),
        ("//system", "/system"),
        (".", "."),
        ("..", ".."),
        ("bin/tool.exe", "bin/tool.exe"),
        ("no/lead/slash", "no/lead/slash"),
    )

    DISPLAY = (
        ("/", "//"),
        ("/fs", "/"),
        ("/FS", "/"),
        ("/fs/X", "/X"),
        ("/fs/BIN", "/BIN"),
        ("/FS/Games", "/Games"),
        ("/system", "/system"),
        ("/system/monitor.exe", "/system/monitor.exe"),
        ("/systemx", "/systemx"),
        ("/dev/console", "/dev/console"),
        (".", "."),
        ("..", ".."),
        ("relative", "relative"),
    )

    def test_shell_paths_translate_to_vfs_paths(self):
        z, bank = self.load()
        for path, expected in self.TRANSLATE:
            length, result = self.translate(z, bank, path)
            self.assertEqual(result, expected, path)
            self.assertEqual(length, len(expected), path)

    def test_vfs_paths_display_in_shell_syntax(self):
        z, bank = self.load()
        for path, expected in self.DISPLAY:
            length, result = self.display(z, bank, path)
            self.assertEqual(result, expected, path)
            self.assertEqual(length, len(expected), path)

    def test_what_is_displayed_translates_back(self):
        z, bank = self.load()
        for shown in ("/", "//", "/GAMES", "/system", "/dev"):
            _length, translated = self.translate(z, bank, shown)
            _length, displayed = self.display(z, bank, translated)
            self.assertEqual(displayed, shown, shown)

    def test_a_translation_that_cannot_fit_is_refused(self):
        z, bank = self.load()
        # /fs is three bytes: a 127-byte absolute path is the last that
        # fits.
        length, result = self.translate(z, bank, "/" + "a" * 123)
        self.assertEqual(length, 127)
        self.assertEqual(result, "/fs/" + "a" * 123)
        length, _result = self.translate(z, bank, "/" + "a" * 124)
        self.assertEqual(length, -0x02)


class TestShellErrorTable(ShellImage):
    """The native error table (requirement 8)."""

    TABLE = (
        (0x01, "not implemented"),
        (0x02, "invalid argument"),
        (0x03, "no device"),
        (0x04, "busy"),
        (0x05, "not found"),
        (0x06, "no space"),
        (0x07, "not supported"),
        (0x08, "timed out"),
        (0x09, "try again"),
        (0x0A, "I/O error"),
        (0x0B, "init refused"),
        (0x0C, "bad program"),
        (0x10, "already exists"),
        (0x11, "not a directory"),
        (0x12, "is a directory"),
        (0x13, "not empty"),
        (0x14, "read-only"),
        (0x15, "stale"),
        (0x16, "cross-device"),
        (0x17, "too many open"),
    )

    def test_every_native_code_has_its_message(self):
        z, bank = self.load()
        for code, text in self.TABLE:
            pointer = self.call(z, "_shell_strerror", hl=code)
            self.assertEqual(self.cstring(bank, pointer), text,
                             "code %02X" % code)

    def test_an_unknown_code_reports_its_hex_value(self):
        z, bank = self.load()
        for code in (0x00, 0x18, 0x42, 0xFF):
            pointer = self.call(z, "_shell_strerror", hl=code)
            self.assertEqual(self.cstring(bank, pointer),
                             "error %02X" % code)


class TestShellStartupCwd(ShellImage):
    """The startup CWD (requirement 6).

    The card-less branch is replayed here: the linked image's namespace
    and console entry points are replaced by stubs staged in the
    program's bank, exactly the way the process would see them, and
    `main` runs its startup.  The stubs fail `chdir("/fs")` with
    `KOS_ENODEV` (no card), record every `chdir` path and the long
    console write, refuse the banner's `/dev/video` bind, then fail
    the console so the loop leaves.
    """

    # Staged state at the shared record base: the last chdir path (2),
    # the first (2), the long write (2).
    # ld (RECORDS+2),hl : the previous record moves to slot 2.
    CHDIR_STUB = bytes((
        0xE5,                    # push hl       ; the incoming path
        0x2A, 0x02, 0x78,        # ld hl,(7802h) ; the previous record
        0x22, 0x04, 0x78,        # ld (7804h),hl ; -> slot 2
        0xE1,                    # pop hl
        0x22, 0x02, 0x78,        # ld (7802h),hl ; this call's path
        0x11, 0xFD, 0xFF,        # ld de,#FFF Dh ; -KOS_ENODEV: no card
        0xC9,                    # ret
    ))
    # ld de,#FFF Eh ; -KOS_EINVAL
    FAIL_STUB = bytes((
        0x11, 0xFE, 0xFF,
        0xC9,
    ))
    # A write of at least 40 bytes is the startup message; the short ones
    # (the "?> " of the failed prompt) are ignored.
    WRITE_STUB = bytes((
        0x7B,                    # ld a,e        ; the write length
        0xFE, 0x28,              # cp #40
        0x38, 0x03,              # jr c,+3       ; too short: skip
        0x22, 0x06, 0x78,        # ld (7806h),hl ; record the buffer
        0x11, 0x00, 0x00,        # ld de,#0      ; the write succeeded
        0xC9,                    # ret
    ))

    def test_no_filesystem_falls_back_to_the_store(self):
        z, bank = self.load()
        for name, code in (("_chdir", self.CHDIR_STUB),
                           ("_getcwd", self.FAIL_STUB),
                           ("_console_read", self.FAIL_STUB),
                           ("_console_write", self.WRITE_STUB),
                           ("_bind", self.FAIL_STUB),
                           ("_diag_system_info", self.FAIL_STUB)):
            address = self.symbols[name]
            bank[address:address + len(code)] = code
        self.capture_console(z, bank)
        bank[RECORDS:RECORDS + 8] = b"\x00" * 8

        status = signed(self.call(z, "_main"))

        def record(index):
            address = RECORDS + index
            pointer = bank[address] | (bank[address + 1] << 8)
            return self.cstring(bank, pointer)

        self.assertEqual(record(4), "/fs", "the shell asked for /fs first")
        self.assertEqual(record(2), "/system", "then fell back to /system")
        self.assertEqual(record(6),
                         "shell: no filesystem, starting in /system\r\n")
        # The console failed, so the loop left with its code.
        self.assertEqual(status, 0x02)


class TestShellFileTargets(ShellImage):
    """The `cp`/`mv` target rule (requirements 5 and 6).

    A directory destination takes the source's last component; anything
    else is the target itself, and a trailing slash says "directory" even
    when no `stat` answers for it.
    """

    # The longest directory prefix and source name there are: the target
    # fits exactly.
    LONG_DIR = "/fs/" + "D" * 59                 # KFS/name limits: 63 + 63
    LONG_NAME = "N" * 63

    # src, dst, directory (the `stat` answer), expected target
    CASES = (
        ("/fs/SUB/ORIG.TXT", "/fs/SHTEST", 1, "/fs/SHTEST/ORIG.TXT"),
        ("/fs/SUB/ORIG.TXT", "/fs/SHTEST/", 0, "/fs/SHTEST/ORIG.TXT"),
        ("/fs/SUB/ORIG.TXT", "/fs/SHTEST/COPY.TXT", 0,
         "/fs/SHTEST/COPY.TXT"),
        ("/fs/SUB/ORIG.TXT", "/fs/SHTEST", 0, "/fs/SHTEST"),
        ("/fs/known.bin", "/fs", 1, "/fs/known.bin"),
        ("/file.txt", "SUB", 1, "SUB/file.txt"),
        ("/x/" + LONG_NAME, LONG_DIR, 1, LONG_DIR + "/" + LONG_NAME),
    )

    # src, dst, directory, -KOS_EINVAL when the target cannot fit.
    REFUSED = (
        ("/ab.txt", LONG_DIR + "/" + "E" * 60, 1),
    )

    def target(self, z, bank, src, dst, directory):
        bank[OUT:OUT + 128] = b"\x00" * 128
        status = signed(self.call(z, "_shell_fscmd_target",
                                  hl=self.stage(bank, LINE, src),
                                  de=self.stage(bank, TAIL, dst),
                                  stack=(directory, OUT, 128)))
        return status, self.cstring(bank, OUT)

    def test_the_target_is_a_name_or_a_directory_entry(self):
        z, bank = self.load()
        for src, dst, directory, expected in self.CASES:
            status, target = self.target(z, bank, src, dst, directory)
            self.assertEqual(status, 0, "%s -> %s" % (src, dst))
            self.assertEqual(target, expected, "%s -> %s" % (src, dst))

    def test_a_target_that_cannot_fit_is_refused(self):
        z, bank = self.load()
        for src, dst, directory in self.REFUSED:
            status, target = self.target(z, bank, src, dst, directory)
            self.assertEqual(status, -0x02, "KOS_EINVAL: %s -> %s"
                             % (src, dst))
            self.assertEqual(target, "")


class TestShellFileErrors(ShellImage):
    """The file built-ins' error path with no card (requirement 8).

    The branch is replayed here: the linked image's namespace entry
    points answer `KOS_ENODEV` and the console write is captured instead
    of performed, exactly the way the process would see them.
    """

    # ld de,#FFF Dh ; ret : -KOS_ENODEV from `stat`, `list` and `mkdir`.
    ENODEV_STUB = bytes((0x11, 0xFD, 0xFF, 0xC9))

    # HL = the message, DE = its length: record both, then report a
    # successful write (a formatted message shorter than printf's 32-byte
    # flush buffer arrives as one call).
    CAPTURE_STUB = bytes((
        0x22, 0x00, 0x70,         # ld (7000h),hl ; the buffer
        0xED, 0x53, 0x02, 0x70,   # ld (7002h),de ; its length
        0x11, 0x00, 0x00,         # ld de,#0      ; the write succeeded
        0xC9,                     # ret
    ))

    def setUp(self):
        super().setUp()
        self.stream_len = 0

    def service(self, z, bank):
        for name in ("_stat", "_list", "_mkdir"):
            address = self.symbols[name]
            bank[address:address + len(self.ENODEV_STUB)] = \
                self.ENODEV_STUB
        address = self.symbols["_console_write"]
        bank[address:address + len(self.CAPTURE_STUB)] = self.CAPTURE_STUB
        self.capture_console(z, bank)

    def command(self, z, bank, words):
        """`shell_file_command` over staged words; the positive status.

        The messages are formatted per byte through the buffered console
        now, so the capture is read from where it stood before the call.
        """
        bank[RECORDS:RECORDS + 4] = b"\x00" * 4
        argv = self.stage_argv(bank, list(words))
        self.stream_len = bank[STREAM_LEN] | (bank[STREAM_LEN + 1] << 8)
        return signed(self.call(z, "_shell_file_command", hl=len(words),
                                de=argv))

    def message(self, bank):
        """The console bytes this command wrote, through the memory map
        (printf formats on the process stack, in the common bank)."""
        return self.streamed(bank)[self.stream_len:]

    def test_no_card_reports_no_device(self):
        z, bank = self.load()
        self.service(z, bank)

        self.assertEqual(self.command(z, bank, ("ls", "/")), 0x03)
        self.assertEqual(self.message(bank), "ls: /: no device\r\n")

        self.assertEqual(self.command(z, bank, ("mkdir", "/X")), 0x03)
        self.assertEqual(self.message(bank), "mkdir: /X: no device\r\n")


class TestShellArtifact(unittest.TestCase):
    """The packed program: an ordinary EXE built by the shared rule."""

    def setUp(self):
        if not os.path.exists(os.path.join(ROOT, SHELL_REPORT)):
            raise unittest.SkipTest("shell.exe has not been built")

    def report(self):
        with open(os.path.join(ROOT, SHELL_REPORT)) as handle:
            return json.load(handle)

    def test_it_is_one_packed_exe_with_no_far_segments(self):
        info = self.report()
        self.assertEqual(info["segments"], [])
        self.assertEqual(info["header"]["segment_count"], 0)
        self.assertEqual(info["format"], "exe-v2")
        self.assertGreater(info["header"]["image_size"], 0)

    def test_it_is_staged_as_the_boot_program(self):
        # STAGE=1 copies a program into build/modules, which the recovery
        # manifests consume; the shell is what the production store boots
        # (replayed end to end by tests/test_boot_policy.py).
        self.assertTrue(os.path.exists(os.path.join(ROOT, SHELL_STAGED)))
        with open(os.path.join(ROOT, "systems", "production.json")) as handle:
            production = json.load(handle)
        self.assertEqual(production["boot"], "/system/shell.exe")

    def test_it_links_its_parts_and_no_unused_sdk_member(self):
        modules = {module for _address, module
                   in _symbols(SHELL_MAP).values() if module}
        self.assertTrue(SHELL_MODULES <= modules,
                        "missing %s" % (SHELL_MODULES - modules))
        for name in UNUSED_MODULES:
            self.assertNotIn(name, modules,
                             "the shell links the unused member %s" % name)

    def test_it_leaves_the_host_tests_their_staging_area(self):
        # The host tests stage arguments and records inside the program's
        # bank; a linked image that grows into them would break them in
        # ways that look like test bugs.
        info = self.report()
        end = 0x100 + info["header"]["image_size"] + info["bss"]["size"]
        self.assertLess(end, LINE,
                        "the linked image reached the staging area")
        self.assertLess(
            max(LINE, ARGV, TAIL, OUT, WORDS, STREAM, STREAM_LEN, RECORDS),
            info["stack"]["base"],
            "the staging area reached the program's stack")


class TestShellLogoAsset(unittest.TestCase):
    """The banner logo: the reviewable .lmmc pixel stream and the C
    pair the shell links, generated from it by tools/g6pack.py."""

    LMMC = os.path.join(ROOT, "user", "shell", "konaos-logo-80x46.lmmc")
    LOGO_H = os.path.join(ROOT, "user", "shell", "logo.h")
    LOGO_C = os.path.join(ROOT, "user", "shell", "logo.c")

    def test_the_dump_is_the_pinned_lmmc_pixel_stream(self):
        data = g6pack.parse_g6(self.LMMC)
        self.assertEqual(len(data), g6pack.BYTES)
        self.assertEqual(g6pack.verify(data, self.LMMC),
                         g6pack.PINNED_SHA256)
        # The logo is the G6 LMMC stream: 80 x 46 pixels, one per byte
        # in the low nibble.
        self.assertEqual(g6pack.BYTES, 80 * 46)
        self.assertTrue(all(byte <= 0x0F for byte in data))

    def test_the_committed_c_pair_regenerates_identically(self):
        data = g6pack.parse_g6(self.LMMC)
        g6pack.verify(data, self.LMMC)
        with open(self.LOGO_H, encoding="ascii") as handle:
            self.assertEqual(handle.read(), g6pack.HEADER)
        with open(self.LOGO_C, encoding="ascii") as handle:
            self.assertEqual(handle.read(), g6pack.render_source(data))


if __name__ == "__main__":
    unittest.main()
