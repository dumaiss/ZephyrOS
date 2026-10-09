#!/usr/bin/env python3
"""User SDK acceptance tests (work package 28c-m9-user-sdk).

The SDK is the only way a native ZEX is built: `user/sdk/` provides crt0,
libzos and zlibc, `user/sdk/zex.mk` provides the one-line `zex_program`
rule, and the public native ABI lives once in `abi/zos/`.  These tests pin
that contract:

* `libzos` is one object per source, exports only the SDK surface and
  contains no kernel code;
* `hello.zex` is packed from a manifest derived entirely from its link map
  (no hand-entered size), validates as a ZEX v1 container, and pulls in
  neither the filesystem nor the diagnostic library members;
* every ZEX program source compiles against the ABI and the SDK include
  root alone -- never a kernel header;
* the Z80 simulator executes the SDK startup path: `_zos_split` turns the
  kernel's raw command tail into argv with the documented quoting, and the
  `helloworld` image boots `hello.zex` as `/SYSTEM/hello.zex` through the
  unmodified production launcher, prints the greeting and is re-executed
  after a clean exit 0.

Skipped when the SDK artifacts have not been built.
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

import pack_image  # noqa: E402
import zexpack  # noqa: E402
import zexmanifest  # noqa: E402
from z80sim import (IocCommandPeer, V9958Peer, Z80,  # noqa: E402
                    read_symbols, skip_serial_connect_delay)

RET = 0x1234
STACK = 0xFFFE
STEP_LIMIT = 20000000
LOAD_BASE = 0x0100
STACK_TOP = 0xE000
WINDOW_END = 0x2000

SDK_DIR = "user/sdk"
SDK_SRC = os.path.join(SDK_DIR, "src")
SDK_INCLUDE = os.path.join(SDK_DIR, "include")
ZEX_MK = os.path.join(SDK_DIR, "zex.mk")

LIBZOS = os.path.join("build", "user", "sdk", "libzos.lib")
HELLO_OBJDIR = os.path.join("build", "user", "hello")
HELLO_JSON = os.path.join(HELLO_OBJDIR, "hello.json")
HELLO_MAP = os.path.join(HELLO_OBJDIR, "hello.map")
HELLO_ZEX = os.path.join("build", "user", "hello.zex")

IMAGE_SUBDIR = "build-helloworld"
IMAGE_NAME = "zephyros-helloworld.bin"
BOOT_PROGRAM = "/SYSTEM/hello.zex"

# The include roots a ZEX program may resolve a quoted include against: its
# own directory, the public ABI, the SDK, and the SDK rule's INCLUDES=.
PROGRAM_INCLUDE_ROOTS = (SDK_INCLUDE, "abi")

# Kernel/production include roots a program header must never resolve into.
KERNEL_INCLUDE_ROOTS = ("include", "kernel", "platform", "drivers", "fs",
                        "services", "boot", "modules", "config", "linker")


# What `hello` links: crt0, the shared runtime, the program, the console
# protocol client with its IPC gate, the argv members, and the
# printf/string helpers it actually calls.
HELLO_LIBRARY_MODULES = {
    "zos_crt0", "zos_runtime", "hello", "args", "console_service", "ipc",
    "zos_gate", "zos_ipcgate", "printf",
    "strlen", "utoa", "ultoa", "itoa", "ltoa", "sdcc_arith",
    "zos_sdcc_arith_entries",
}

# Kernel services a program must not link: the filesystem, the diagnostic
# and the exec wrappers are archive members that stay out unless called.
FORBIDDEN_HELLO_MODULES = ("fs", "fsgate", "diaggate", "exec", "console_io")


def _read(rel_or_abs):
    path = rel_or_abs
    if not os.path.isabs(path):
        path = os.path.join(ROOT, path)
    return Path(path).read_text()


def _symbols(rel_map):
    """Parse an sdldz80 map into {symbol: (address, module)}.

    The area symbols (`s__DATA`, `l__DATA`, `l__CODE`) carry no module
    name: they are the linker's own bounds, and the manifest is derived
    from them."""
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
    """Return the archive member names that carry an object (skip indexes).

    `sdar` stores names past the 16-byte header field in a `//` string
    table and refers to them as `/offset`, so those are resolved here."""
    data = Path(path).read_bytes()
    assert data[:8] == b"!<arch>\n", "%s is not an ar archive" % path
    names = []
    strings = ""
    pos = 8
    while pos + 60 <= len(data):
        header = data[pos:pos + 60]
        name = header[0:16].decode("ascii").strip()
        size = int(header[48:58].decode("ascii").strip())
        if name == "//":
            strings = data[pos + 60:pos + 60 + size].decode("ascii")
        elif name.startswith("/") and name[1:].isdigit():
            offset = int(name[1:])
            end = strings.index("\n", offset)
            names.append(strings[offset:end].rstrip("/"))
        elif name.endswith(".rel/"):
            names.append(name[:-1])
        pos += 60 + size + (size & 1)
    return names


def _rule_sources(variable):
    """The source list of one ``:=`` assignment in user/sdk/zex.mk."""
    match = re.search(r"^%s\s*:=\s*((?:.*\\\n)*.*)" % variable,
                      _read(ZEX_MK), re.M)
    assert match is not None, "zex.mk defines no %s" % variable
    return match.group(1).replace("\\\n", " ").split()


def _split_call_arguments(text, start):
    """Split the top-level arguments of the call whose body starts at
    `start` (just past the opening parenthesis)."""
    depth = 1
    args = [""]
    index = start
    while index < len(text):
        char = text[index]
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                break
        if char == "," and depth == 1:
            args.append("")
        elif char not in "\\\n":
            args[-1] += char
        index += 1
    return [arg.strip() for arg in args]


def _declared_programs():
    """Every declared ZEX: (name, dir, sources, options).

    The declarations live in the top-level Makefile and in the
    sub-project fragments it includes (user/helloworld/Makefile)."""
    programs = []
    for name in ("Makefile", os.path.join("user", "helloworld", "Makefile")):
        text = _read(name)
        for match in re.finditer(r"\$\(eval \$\(call\s+zex_program,",
                                 text):
            args = _split_call_arguments(text, match.end())
            programs.append((args[0], args[1], args[2].split(), args[3:]))
    return programs


def _program_options(options, key):
    return [option[len(key) + 1:] for option in options
            if option.startswith(key + "=")]


def _program_include_roots(options):
    roots = []
    for value in _program_options(options, "INCLUDES"):
        roots += value.split(":")
    return roots


def _program_files(directory, sources, extra):
    """The declared sources plus the program's own headers."""
    paths = [os.path.join(directory, name) for name in sources]
    for name in sorted(os.listdir(os.path.join(ROOT, directory))):
        if name.endswith(".h"):
            paths.append(os.path.join(directory, name))
    return paths


def _sdk_sources():
    paths = []
    for dirpath, _dirnames, filenames in os.walk(os.path.join(ROOT, SDK_SRC)):
        for filename in filenames:
            if filename.endswith((".c", ".asm")):
                paths.append(os.path.join(dirpath, filename))
    return sorted(paths)


def _quoted_includes(path):
    return re.findall(r'#\s*include\s+"([^"]+)"', _read(path))


class TestSdkLibrary(unittest.TestCase):
    """libzos is the whole user-space library, one member per source."""

    @classmethod
    def setUpClass(cls):
        path = os.path.join(ROOT, LIBZOS)
        if not os.path.exists(path):
            raise unittest.SkipTest("libzos has not been built")
        cls.members = _ar_members(path)

    def test_every_sdk_source_is_its_own_archive_member(self):
        sources = (_rule_sources("ZOS_SDK_C_SRC")
                   + _rule_sources("ZOS_SDK_ASM_SRC"))
        self.assertTrue(sources, "zex.mk lists no library sources")
        expected = sorted(os.path.splitext(os.path.basename(source))[0]
                          + ".rel" for source in sources)
        self.assertEqual(sorted(self.members), expected,
                         "libzos members do not match the SDK sources")
        # One member per source file, so an unused helper is never linked.
        self.assertEqual(len(self.members), len(set(self.members)))
        # The archive is rebuilt from those sources alone; crt0 and the
        # shared runtime object are linked explicitly, not archived.
        standalone = {"crt0.asm", "runtime.asm"}
        self.assertEqual(
            sorted(set(os.path.basename(path) for path in _sdk_sources())
                   - standalone),
            sorted(os.path.basename(source) for source in sources))

    def test_library_is_named_libzos_and_lives_under_the_sdk_build(self):
        self.assertTrue(LIBZOS.startswith(os.path.join("build", "user")))
        self.assertTrue(all(name.endswith(".rel") for name in self.members))

    def test_no_archive_member_is_kernel_code(self):
        for member in self.members:
            self.assertNotIn("kernel", member)
            self.assertFalse(member.startswith("vfs"),
                             "kernel object %s is in libzos" % member)

    def test_sdk_sources_include_only_the_abi_and_the_sdk(self):
        for path in _sdk_sources():
            rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
            for include in _quoted_includes(path):
                self.assertNotIn("<", include, "%s: %s" % (rel, include))
                self.assertFalse(include.startswith("/"), rel)
                self.assertNotIn("..", include, rel)
                head = include.split("/")[0]
                self.assertNotIn(
                    head, KERNEL_INCLUDE_ROOTS,
                    "%s includes kernel header %s" % (rel, include))


class TestHelloZex(unittest.TestCase):
    """hello.zex is packed from the map, not from hand-entered numbers."""

    @classmethod
    def setUpClass(cls):
        for path in (HELLO_JSON, HELLO_MAP, HELLO_ZEX):
            if not os.path.exists(os.path.join(ROOT, path)):
                raise unittest.SkipTest(
                    "%s has not been built; run make build/user/hello.zex"
                    % path)
        cls.manifest = zexpack.load_manifest(os.path.join(ROOT, HELLO_JSON))
        cls.symbols = _symbols(HELLO_MAP)
        cls.image = Path(os.path.join(ROOT, HELLO_ZEX)).read_bytes()

    def test_manifest_is_exactly_what_the_link_map_implies(self):
        derived = zexmanifest.build_manifest(
            os.path.join(ROOT, HELLO_MAP), "hello.zex", "zos_entry", 256,
            "zos_scratch:16")
        self.assertEqual(derived, self.manifest)

    def test_sizes_come_from_the_map(self):
        self.assertEqual(self.manifest["image_size"],
                         self.symbols["s__DATA"][0] - LOAD_BASE)
        self.assertEqual(self.manifest["bss_size"],
                         self.symbols["l__DATA"][0])
        self.assertEqual(self.manifest["image_size"],
                         self.symbols["l__ZOS_SDK_DATA"][0] +
                         self.symbols["l__CODE"][0])

    def test_container_validates_and_enters_after_low_data(self):
        fields, geometry = zexpack.validate_image(self.image)
        self.assertEqual(fields["entry_offset"], 0x44)
        self.assertEqual(geometry["entry"], LOAD_BASE + 0x44)
        self.assertEqual(geometry["image_size"], self.manifest["image_size"])
        self.assertEqual(geometry["stack_base"], STACK_TOP - 256)
        self.assertEqual(self.manifest["stack_size"], 256)

    def test_entry_symbol_is_crt0_not_entry_assembly(self):
        self.assertEqual(self.manifest["entry"], "zos_entry")
        self.assertEqual(self.symbols["zos_entry"],
                         (LOAD_BASE + 0x44, "zos_crt0"))
        # A program never writes entry assembly: no .asm beside the demo.
        for name in os.listdir(os.path.join(ROOT, "user", "helloworld")):
            self.assertFalse(name.endswith(".asm"),
                             "the demo carries entry assembly: %s" % name)

    def test_scratch_stays_inside_the_syscall_window(self):
        scratch = self.manifest["scratch"]
        address = self.symbols[scratch["symbol"]][0]
        self.assertGreaterEqual(address, LOAD_BASE)
        self.assertLessEqual(address + scratch["size"], WINDOW_END)

    def test_hello_links_no_filesystem_or_diagnostic_member(self):
        modules = _linked_modules(self.symbols)
        self.assertEqual(modules, HELLO_LIBRARY_MODULES)
        for name in FORBIDDEN_HELLO_MODULES:
            self.assertNotIn(name, modules)
        for symbol in self.symbols:
            for prefix in ("_zos_fs_", "_zos_diag_", "_vfs_", "_kernel_"):
                self.assertFalse(
                    symbol.startswith(prefix),
                    "hello.zex links kernel/FS/diag symbol %s" % symbol)

    def test_no_zex_manifest_is_checked_in(self):
        """Every ZEX manifest is generated; only the helloworld ROMFS
        manifest (not a zex-v1 manifest) is a source file."""
        checked_in = []
        for base in ("tests/target", "user/monitor", "user/helloworld"):
            for dirpath, _dirnames, filenames in os.walk(
                    os.path.join(ROOT, base)):
                for filename in filenames:
                    if filename.endswith(".json"):
                        checked_in.append(
                            os.path.join(dirpath, filename)
                            .replace(ROOT + os.sep, "").replace(os.sep, "/"))
        # tests/target/ipc/manifests/*.json are the work-package-39 ZMOD
        # container pack inputs (acceptance images only), not ZEX
        # manifests.
        checked_in = [path for path in checked_in
                      if not path.startswith(
                          "tests/target/ipc/manifests/")]
        self.assertEqual(checked_in, ["user/helloworld/manifest.json"])
        manifest = json.loads(_read("user/helloworld/manifest.json"))
        self.assertEqual(manifest["format"], "romfs-v1")


class TestSdkBoundary(unittest.TestCase):
    """A program sees the ABI and the SDK, and nothing else."""

    def test_every_program_is_declared_once_by_the_generic_rule(self):
        programs = _declared_programs()
        # `ipc` and `ipccopy` are the resident-service acceptance client
        # and its caller-buffer probe: declared by the same generic rule,
        # but only the ZOS_IPC_SELFTEST configuration builds them.
        self.assertEqual(sorted(name for name, _d, _s, _o in programs),
                         ["fschild", "fsdeep", "fsfat", "fsraw", "fstest",
                          "fstest_l", "hello", "ipc", "ipccopy", "monitor",
                          "zextest"])
        for name, directory, _sources, _options in programs:
            count = sum(_read(path).count("zex_program,%s," % name)
                        for path in ("Makefile",
                                     os.path.join("user", "helloworld",
                                                  "Makefile")))
            self.assertEqual(count, 1, "%s is declared %d times"
                             % (name, count))
            self.assertTrue(
                os.path.isdir(os.path.join(ROOT, directory)))

    def test_the_program_file_includes_the_umbrella_header(self):
        for name, directory, sources, options in _declared_programs():
            if _program_options(options, "NO_CRT0"):
                continue  # the raw-return child has no C startup at all
            path = os.path.join(ROOT, directory, sources[0])
            self.assertIn("zos.h", _quoted_includes(path),
                          "%s (%s) does not include zos.h"
                          % (name, sources[0]))

    def test_every_quoted_include_resolves_inside_the_sdk_boundary(self):
        for name, directory, sources, options in _declared_programs():
            extra = _program_include_roots(options)
            roots = [os.path.join(ROOT, directory)]
            roots += [os.path.join(ROOT, r) for r in PROGRAM_INCLUDE_ROOTS]
            roots += [os.path.join(ROOT, r) for r in extra]
            for path in _program_files(directory, sources, extra):
                for include in _quoted_includes(os.path.join(ROOT, path)):
                    found = [r for r in roots
                             if os.path.exists(os.path.join(r, include))]
                    self.assertTrue(
                        found, "%s: %s is not on a program include path"
                        % (path, include))
                    resolved = os.path.relpath(found[0], ROOT)
                    self.assertNotIn(
                        resolved.split(os.sep)[0], KERNEL_INCLUDE_ROOTS,
                        "%s reaches %s" % (path, resolved))

    def test_rule_compiles_with_the_abi_and_sdk_include_roots_only(self):
        rule = _read(ZEX_MK)
        match = re.search(r"^ZEX_CFLAGS\s*:=\s*(.+)$", rule, re.M)
        self.assertIsNotNone(match, "zex.mk defines no ZEX_CFLAGS")
        flags = match.group(1)
        self.assertIn("--nostdinc", flags)
        self.assertIn("-I abi", flags)
        self.assertIn("-I $(ZOS_SDK_INCLUDE)", flags)
        for root in KERNEL_INCLUDE_ROOTS:
            self.assertNotIn("-I %s" % root, flags,
                             "the program rule adds -I %s" % root)
        self.assertNotIn("include/zephyros", rule)
        # ASM sources see the ABI only (the SDK's asm uses no C header).
        self.assertIn("ZEX_ASFLAGS := -Iabi", rule)

    def test_no_crt0_programs_are_the_documented_pair(self):
        no_crt0 = [name for name, _d, _s, options in _declared_programs()
                   if _program_options(options, "NO_CRT0")]
        # `fsraw` is the raw-return acceptance child; `ipccopy` is the
        # caller-buffer probe, which links without the C startup so that
        # CODEBASE= can lift its code above the 0100h-02FFh buffer it
        # names as a caller buffer (work package 39 correction 01).
        self.assertEqual(sorted(no_crt0), ["fsraw", "ipccopy"])
        for name, _d, _s, options in _declared_programs():
            if _program_options(options, "NO_CRT0"):
                self.assertTrue(_program_options(options, "ENTRY"),
                                "%s owns an entry" % name)

    def test_the_moved_headers_have_one_owner(self):
        for name in ("types.h", "errno.h", "syscall.h", "fs.h", "process.h",
                     "diag.h"):
            self.assertTrue(os.path.isfile(
                os.path.join(ROOT, "abi", "zos", name)),
                "abi/zos/%s is missing" % name)
        for name in ("fs.h", "syscall.h"):
            self.assertFalse(
                os.path.isfile(os.path.join(ROOT, "include", "zephyros",
                                            name)),
                "include/zephyros/%s mirrors a public ABI header" % name)

    def test_sdk_headers_are_the_only_user_headers(self):
        self.assertTrue(os.path.isfile(
            os.path.join(ROOT, SDK_INCLUDE, "zos.h")))
        for name in ("console.h", "fs.h", "process.h", "diag.h"):
            self.assertTrue(os.path.isfile(
                os.path.join(ROOT, SDK_INCLUDE, "zos", name)))


class TestSdkSplitterInSimulator(unittest.TestCase):
    """crt0's argv comes from `zos_split`; run the linked bytes.

    The program's own image is loaded into the application bank exactly as
    the loader places it, and the splitter is called the way crt0 calls it
    (HL = tail, DE = argv, capacity on the stack).  This is the argv half
    of the startup contract; the end-to-end transport from the kernel is
    asserted by the exec acceptance replay (tests/test_m9_exec_sim.py).
    """

    TAIL = 0x1200
    ARGV = 0x1280
    BLOCK = 0x1300  # a startup block staged inside the program's bank
    CAPACITY = 16

    @classmethod
    def setUpClass(cls):
        if not os.path.exists(os.path.join(ROOT, HELLO_JSON)):
            raise unittest.SkipTest("hello.zex has not been built")
        manifest = zexpack.load_manifest(os.path.join(ROOT, HELLO_JSON))
        ihx = os.path.join(ROOT, HELLO_OBJDIR, manifest["input"])
        cls.image = pack_image.parse_ihx(Path(ihx).read_text())
        cls.manifest = manifest
        cls.symbols = read_symbols(os.path.join(ROOT, HELLO_MAP))

    def split(self, tail):
        z = Z80(bytearray(b"\xff" * 0x10000), bytearray(b"\xff" * 0x10000))
        z.latch = 0x11  # mode 10: the program runs over application bank 1
        bank = z.banks[1]
        for address, byte in self.image.items():
            bank[address] = byte
        raw = tail.encode("ascii")
        bank[self.TAIL:self.TAIL + len(raw)] = raw
        bank[self.TAIL + len(raw)] = 0
        bank[self.ARGV:self.ARGV + 2 * self.CAPACITY] = b"\x00" * 32
        z.sp = STACK
        z.hl = self.TAIL
        z.de = self.ARGV
        z.push16(self.CAPACITY)
        z.push16(RET)
        z.pc = self.symbols["_zos_split"]
        for _ in range(200000):
            z.step()
            if z.pc == RET:
                break
        else:
            raise AssertionError("_zos_split did not return")
        argc = z.de
        words = []
        for index in range(min(argc, self.CAPACITY)):
            pointer = (bank[self.ARGV + 2 * index]
                       | (bank[self.ARGV + 2 * index + 1] << 8))
            chars = []
            while bank[pointer] != 0:
                chars.append(chr(bank[pointer]))
                pointer += 1
            words.append("".join(chars))
        return argc, words

    def test_whitespace_separates_words(self):
        self.assertEqual(self.split(""), (0, []))
        self.assertEqual(self.split("one"), (1, ["one"]))
        self.assertEqual(self.split("  spaced   out  "), (2, ["spaced", "out"]))

    def test_double_quotes_group_words_and_are_removed(self):
        self.assertEqual(self.split('two words "a b" tail'),
                         (4, ["two", "words", "a b", "tail"]))
        self.assertEqual(self.split('a b "c d" e '),
                         (4, ["a", "b", "c d", "e"]))

    def machine(self):
        """A CPU with the linked program image in application bank 1."""
        z = Z80(bytearray(b"\xff" * 0x10000), bytearray(b"\xff" * 0x10000))
        z.latch = 0x11  # mode 10: the program runs over application bank 1
        bank = z.banks[1]
        for address, byte in self.image.items():
            bank[address] = byte
        z.sp = STACK
        return z, bank

    def startup_block(self, bank, tail):
        """Write a startup block v1 into the program's bank."""
        raw = tail.encode("ascii")
        bank[self.BLOCK] = 1
        bank[self.BLOCK + 1] = len(raw)
        bank[self.BLOCK + 2:self.BLOCK + 2 + len(raw)] = raw
        bank[self.BLOCK + 2 + len(raw)] = 0

    def test_crt0_splits_the_tail_into_main_argv(self):
        """Run crt0 itself: it must hand main() the copied words.

        Stopping at `_main` reads exactly what crt0 passes (HL = argc,
        DE = argv) before the program's own syscalls can invalidate the
        kernel's page-zero block."""
        z, bank = self.machine()
        self.startup_block(bank, 'one "two words" three')
        z.hl = self.BLOCK
        z.push16(RET)
        z.pc = self.symbols["zos_entry"]
        for _ in range(200000):
            z.step()
            if z.pc == self.symbols["_main"]:
                break
        else:
            raise AssertionError("crt0 did not reach main")
        argc, argv = z.hl, z.de
        self.assertEqual(argc, 3)
        # The words live in the program's own data, not in the startup
        # block: the kernel resets page zero on the first syscall.
        self.assertNotEqual(argv, self.BLOCK + 2)
        image_end = LOAD_BASE + self.manifest["image_size"]
        words = []
        for index in range(argc):
            pointer = argv + 2 * index
            self.assertGreaterEqual(pointer, image_end)
            while bank[pointer] != 0:
                pointer += 1
        for index in range(argc):
            address = bank[argv + 2 * index] | (bank[argv + 2 * index + 1] << 8)
            word = []
            while bank[address] != 0:
                word.append(chr(bank[address]))
                address += 1
            words.append("".join(word))
        self.assertEqual(words, ["one", "two words", "three"])

    def test_crt0_gives_an_empty_command_line_for_a_bad_block(self):
        for version, length in ((0x02, 3), (0x01, 126)):
            z, bank = self.machine()
            self.startup_block(bank, "one")
            bank[self.BLOCK] = version
            bank[self.BLOCK + 1] = length
            z.hl = self.BLOCK
            z.push16(RET)
            z.pc = self.symbols["zos_entry"]
            for _ in range(200000):
                z.step()
                if z.pc == self.symbols["_main"]:
                    break
            else:
                raise AssertionError("crt0 did not reach main")
            self.assertEqual(z.hl, 0, "version/length %02X/%d must be empty"
                             % (version, length))

    def test_argc_is_counted_beyond_the_stored_capacity(self):
        tail = " ".join("w%d" % i for i in range(20))
        argc, words = self.split(tail)
        self.assertEqual(argc, 20)
        self.assertEqual(words, ["w%d" % i for i in range(self.CAPACITY)])


class TestHelloWorldImage(unittest.TestCase):
    """Boot the helloworld image: a user program needs no kernel change."""

    GREETING = "Hello, ZephyrOS!"
    REPORT = "monitor: exit status 0x0000 active 0 free-banks 6"
    CONSOLE_BOUND = "console: service OK"

    @classmethod
    def setUpClass(cls):
        cls.bin_path = os.path.join(ROOT, IMAGE_SUBDIR, IMAGE_NAME)
        if not os.path.exists(cls.bin_path):
            raise unittest.SkipTest(
                "%s missing; run make helloworld-image" % cls.bin_path)
        cls.image = Path(cls.bin_path).read_bytes()
        cls.symbols = read_symbols(os.path.join(ROOT, IMAGE_SUBDIR,
                                                "zephyros.map"))
        cls.transcript = cls.boot()

    @classmethod
    def pump(cls, z):
        """Decode newly issued glyph copies into text.

        The console binds to the loaded V9958 module, so the demo's output
        is visible on the video console: a 6x8 HMMM copy is a printable
        glyph, and a copy that does not continue the previous one starts a
        new model line."""
        commands = z.vdp.commands
        while z.glyph_pos < len(commands):
            cmd = commands[z.glyph_pos]
            z.glyph_pos += 1
            block = cmd.get("block")
            if (cmd.get("code") != 0xD0 or not block
                    or (block[8] | (block[9] << 8)) != 6
                    or (block[10] | (block[11] << 8)) != 8):
                continue
            sx = block[0] | (block[1] << 8)
            sy = block[2]
            dx = block[4] | (block[5] << 8)
            dy = block[6] | (block[7] << 8)
            glyph = ((sy & 0x3F) >> 3) * 32 + sx // 6
            if 32 <= glyph < 127:
                if z.glyph_last == [dy, dx - 6]:
                    z.glyphs.append(chr(glyph))
                else:
                    z.glyphs.append("\n" + chr(glyph))
                z.glyph_last = [dy, dx]

    @classmethod
    def boot(cls):
        pages = {n: cls.image[n * 0x10000:(n + 1) * 0x10000]
                 for n in range(8)}
        z = Z80(pages[0], pages[1],
                rom_pages={n: pages[n] for n in range(2, 8)})
        z.latch = 0x18
        z.sp = STACK
        z.hid = IocCommandPeer()
        z.ioc_peer = z.hid
        z.vdp = V9958Peer()
        z.vdp_peer = z.vdp
        z.glyphs = []
        z.glyph_pos = 0
        z.glyph_last = None
        z.push16(RET)
        z.pc = cls.symbols["__zos_crt0_init"]
        for _ in range(2000000):
            z.step()
            if z.pc == RET:
                break
        else:
            raise AssertionError("C runtime init did not return")
        z.push16(RET)
        z.pc = cls.symbols["_kernel_main"]
        delay = cls.symbols["_kernel_serial_connect_delay"]
        for step in range(STEP_LIMIT):
            skip_serial_connect_delay(z, delay)
            z.step()
            if step % 0x1FFF == 0:
                cls.pump(z)
                if "".join(z.glyphs).count(cls.REPORT) >= 2:
                    break
        cls.serial = bytes(z.sio_tx)
        return "".join(z.glyphs)

    def test_kernel_carries_the_demo_as_its_boot_program(self):
        self.assertIn(BOOT_PROGRAM.encode("ascii"), self.image)
        self.assertNotIn(b"/SYSTEM/monitor.zex", self.image)

    def test_demo_prints_the_greeting_on_the_bound_console(self):
        self.assertIn(self.CONSOLE_BOUND, self.transcript)
        self.assertIn(self.GREETING, self.transcript)

    def test_demo_exits_zero_and_the_launcher_reexecs_it(self):
        out = self.transcript.replace("\n", "")
        self.assertIn(self.GREETING + self.REPORT, out)
        self.assertGreaterEqual(out.count(self.GREETING), 2,
                                "the demo did not run twice")
        self.assertGreaterEqual(out.count(self.REPORT), 2)
        self.assertNotIn("fail", self.transcript.lower())

    def test_early_console_still_reports_the_machine(self):
        self.assertIn(b"kernel entry: OK", self.serial)
        self.assertIn(b"ZephyrOS 0.0", self.serial)


if __name__ == "__main__":
    unittest.main()
