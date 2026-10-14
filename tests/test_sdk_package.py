#!/usr/bin/env python3
"""Standalone SDK package tests.

`make sdk` assembles build/kos-sdk-<version>/ (and a tarball of it) from
the SDK sources, the public ABI headers and the built library.  These
tests unpack the tarball outside the repository and pin the package:

* the tree matches the pinned layout exactly (a golden file list), the
  header closure is complete (every `#include` resolves inside the tree)
  and VERSION carries the ABI number read from formats/include/exe.h;
* no file in the tree names the OS tree (`abi/`, `user/sdk` or the
  repository path): the SDK stands alone;
* its template and examples build with the SDK alone, with the tree's
  own include and library paths absent from the environment;
* the packed program runs in the simulator on the production image (the
  shell is its boot program and runs `/system/hello.exe`), and a copy of
  it whose ABI byte is changed is refused by the loader (the shell
  reports `invalid argument`, the refusal SYS_EXEC maps for a malformed
  container) while the unchanged copy runs.

Skipped when the packed programs or the production image have not been
built.
"""

import ast
import json
import os
import posixpath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import exepack  # noqa: E402
import pack_image  # noqa: E402
import romfspack  # noqa: E402
import sdkpack  # noqa: E402
from replay import ConsoleReplay  # noqa: E402
from z80sim import read_symbols  # noqa: E402

BUILD = os.path.join(ROOT, "build")
CONFIG = os.path.join(ROOT, "config", "sdk.mk")
ABI_HEADER = os.path.join(ROOT, "formats", "include", "exe.h")
IMAGE = os.path.join(BUILD, "konaos.bin")
IMAGE_MAP = os.path.join(BUILD, "konaos.map")
STORE_JSON = os.path.join(BUILD, "store.json")
MODULES = os.path.join(BUILD, "modules")

# The line the SDK's hello example prints, the refusal the shell
# reports for a malformed container, and the doctor'd copy's store name.
# SYS_EXEC maps every loader refusal (EXE_EXEC_ERR_* <= -20h, the ABI
# check among them) to KOS_EINVAL for user space (kernel/syscall.c): the
# monitor renders that as ERR: 02, the shell - the production image's
# boot program - as `invalid argument`, the same refusal an absent or
# malformed program has always produced.
GREETING = "Hello, KonaOS!"
ABI_REFUSAL = "invalid argument"
BAD_ABI_NAME = "abi-probe.exe"

# The pinned layout: the SDK tree is exactly these files.  The
# include/ entries are the SDK headers at the paths their includes name
# ("kos/…", "klibc/…") plus the public ABI headers they include, copied
# under include/abi/ where "kos/…" resolves to them (the SDK's first
# include root, exactly like the OS tree's -I abi -I user/sdk/include).
SDK_FILES = {
    "VERSION",
    "include/kos.h",
    "include/kos/console.h",
    "include/kos/diag.h",
    "include/kos/fs.h",
    "include/kos/ipc.h",
    "include/kos/process.h",
    "include/kos/seg.h",
    "include/kos/timer.h",
    "include/kos/xmem.h",
    "include/klibc/fmt.h",
    "include/klibc/stdarg.h",
    "include/klibc/string.h",
    "include/abi/kos/diag.h",
    "include/abi/kos/errno.h",
    "include/abi/kos/fs.h",
    "include/abi/kos/ipc.h",
    "include/abi/kos/object.h",
    "include/abi/kos/process.h",
    "include/abi/kos/seg.h",
    "include/abi/kos/syscall.h",
    "include/abi/kos/types.h",
    "include/abi/kos/xmem.h",
    "lib/libkos.lib",
    "lib/crt0.rel",
    "tools/exepack.py",
    "tools/exemanifest.py",
    "tools/ihx.py",
    "mk/kos.mk",
    "template/Makefile",
    "template/hello.c",
    "examples/Makefile",
    "examples/hello.c",
    "examples/fardemo.c",
    "examples/fardemo_far.c",
    "examples/files.c",
    "examples/fardata.c",
    "examples/fardata_far.c",
    "examples/levels.c",
    "docs/sdk.md",
    "docs/exe-format.md",
}

# Paths a copied file may not name; the docs describe the OS tree too and
# are the tree's documentation, so they are exempt.
FORBIDDEN_TOKENS = ("abi/", "user/sdk")


def _read(path):
    return Path(path).read_text()


def _sdk_version():
    match = re.search(r"^KOS_SDK_VERSION\s*:=\s*(\S+)", _read(CONFIG), re.M)
    assert match is not None, "config/sdk.mk defines no KOS_SDK_VERSION"
    return match.group(1)


def _ar_members(path):
    """(name, mtime, uid, gid) for each member of a `sdar`/ar archive."""
    data = Path(path).read_bytes()
    assert data[:8] == b"!<arch>\n"
    at = 8
    while at + 60 <= len(data):
        head = data[at:at + 60]
        yield (head[0:16].decode().strip(), head[16:28].decode().strip(),
               head[28:34].decode().strip(), head[34:40].decode().strip())
        size = int(head[48:58].decode())
        at += 60 + size + (size & 1)


def _tree_files(tree):
    files = set()
    for dirpath, _dirnames, filenames in os.walk(tree):
        for name in filenames:
            files.add(os.path.relpath(os.path.join(dirpath, name), tree)
                      .replace(os.sep, "/"))
    return files


def _quoted_includes(path):
    return re.findall(r'#\s*include\s+"([^"]+)"', _read(path))


def _resolve_include(tree, dest, include):
    """The tree path a quoted include resolves to, or None.

    Quoted includes resolve the way SDCC resolves them for an SDK build:
    the including file's own directory first, then the ABI root, then the
    SDK include root (mk/kos.mk passes -I<abi> -I<include>)."""
    for candidate in (posixpath.normpath(posixpath.join(
                          posixpath.dirname(dest), include)),
                      posixpath.join("include/abi", include),
                      posixpath.join("include", include)):
        if os.path.isfile(os.path.join(tree, candidate)):
            return candidate
    return None


def _run_make(cwd):
    """Run make in `cwd` with a minimal, path-free environment.

    The tree's own include/library paths must not be reachable: the
    environment carries no compiler variable at all (CPATH,
    C_INCLUDE_PATH, LIBRARY_PATH, ...), only PATH and the shell basics.
    """
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"),
           "HOME": os.environ.get("HOME", "/tmp"),
           "TMPDIR": os.environ.get("TMPDIR", "/tmp"),
           "LANG": os.environ.get("LANG", "C")}
    result = subprocess.run(["make", "--no-print-directory"], cwd=cwd,
                            env=env, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    if result.returncode != 0:
        raise AssertionError("make in %s failed (%d):\n%s"
                             % (cwd, result.returncode, result.stdout))
    return result.stdout


def _splice_store(image, stream):
    """Place a ROMFS stream into a packed image (pack_image's placement).

    Logical byte L goes to offset L % window of recovery page first + L /
    window; the C000h-FFFFh half of every recovery page (the ROM-access
    stub mirror) and the boot pages stay untouched.
    """
    data = bytearray(image)
    window = pack_image.ROMFS_WINDOW_SIZE
    for page in range(pack_image.ROMFS_FIRST_PAGE,
                      pack_image.ROMFS_FIRST_PAGE + pack_image.ROMFS_PAGE_COUNT):
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


class _PatchedReplay(ConsoleReplay):
    """The production image with a test store in its recovery pages."""

    def __init__(self, image):
        self.root = ROOT
        self.subdir = "build"
        self.image_name = "konaos.bin"
        self.image = image
        self.syms = read_symbols(IMAGE_MAP)


class TestSdkPackage(unittest.TestCase):
    """The tarball: layout, closure, VERSION and an out-of-tree build."""

    @classmethod
    def setUpClass(cls):
        cls.version = _sdk_version()
        cls.tarball = os.path.join(BUILD, "kos-sdk-%s.tar.gz" % cls.version)
        result = subprocess.run(
            ["make", "--no-print-directory", "sdk"], cwd=ROOT,
            env={"PATH": os.environ.get("PATH", "/usr/bin:/bin"),
                 "HOME": os.environ.get("HOME", "/tmp")},
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode != 0:
            raise AssertionError("make sdk failed (%d):\n%s"
                                 % (result.returncode, result.stdout[-2000:]))
        cls.tmp = tempfile.mkdtemp(prefix="kos-sdk-")
        with tarfile.open(cls.tarball) as archive:
            cls.entries = archive.getmembers()
            cls.members = {member.name for member in cls.entries
                           if member.isfile()}
            archive.extractall(cls.tmp)
        cls.tree = os.path.join(cls.tmp, "kos-sdk-%s" % cls.version)
        if not os.path.isdir(cls.tree):
            raise AssertionError("%s carries no kos-sdk-%s tree"
                                 % (cls.tarball, cls.version))
        # The unpacked copy is the only input: it must not sit inside the
        # repository, so no include or library path can reach the tree.
        if os.path.commonpath([os.path.realpath(cls.tree),
                               os.path.realpath(ROOT)]) in (
                os.path.realpath(ROOT), os.path.realpath(cls.tree)):
            raise AssertionError("the unpacked SDK sits inside the tree")
        cls.built = None
        cls.build_error = None

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    @classmethod
    def _build(cls):
        """Build template/ and examples/ outside the tree, once.

        The build runs in a second copy, so the unpacked tree the layout
        tests read stays exactly what the tarball carried."""
        if cls.build_error is not None:
            raise cls.build_error
        if cls.built is not None:
            return cls.built
        try:
            outputs = {}
            source = os.path.join(cls.tmp, "build-copy")
            shutil.copytree(cls.tree, source)
            for directory, programs in (("template", ["hello"]),
                                        ("examples", ["hello", "fardemo",
                                                      "files", "fardata"])):
                workdir = os.path.join(source, directory)
                output = _run_make(workdir)
                if ROOT in output:
                    raise AssertionError(
                        "the SDK build reached the OS tree:\n%s" % output)
                for name in programs:
                    exe = os.path.join(workdir, "build", "user",
                                       "%s.exe" % name)
                    if not os.path.isfile(exe):
                        raise AssertionError("%s was not packed" % exe)
                    fields, _geo = exepack.validate_image(
                        Path(exe).read_bytes())
                    if fields["format_version"] != 2:
                        raise AssertionError("%s is not a EXE v2" % exe)
                    outputs[name] = exe
        except Exception as exc:
            cls.build_error = exc
            raise
        cls.built = outputs
        return outputs

    def _require_image(self):
        for path in (IMAGE, IMAGE_MAP):
            if not os.path.isfile(path):
                self.skipTest("%s is missing; run make" % path)

    def _pack_test_store(self, extra):
        """The production store plus the out-of-tree programs, packed to
        a test ROMFS stream by the real packer."""
        manifest = json.loads(_read(STORE_JSON))
        for name, path in extra:
            manifest["files"] = [entry for entry in manifest["files"]
                                 if entry["name"].lower() != name.lower()]
            manifest["files"].append({"name": name, "type": "EXE",
                                      "path": path})
        manifest_path = os.path.join(self.tmp, "test-store.json")
        Path(manifest_path).write_text(json.dumps(manifest))
        out_path = os.path.join(self.tmp, "test-store.romfs")
        stream, _report = romfspack.pack(manifest_path, out_path,
                                         objdir=MODULES)
        return stream

    def _boot(self, stream):
        replay = _PatchedReplay(_splice_store(Path(IMAGE).read_bytes(),
                                              stream))
        z = replay.make_cpu()
        replay.boot_to_shell(z)
        return replay, z

    # -- the pinned layout -------------------------------------------------

    def test_the_tarball_holds_the_pinned_layout(self):
        expected = {"kos-sdk-%s/%s" % (self.version, dest)
                    for dest in SDK_FILES}
        self.assertEqual(self.members, expected)
        self.assertEqual(_tree_files(self.tree), SDK_FILES)

    def test_the_shipped_library_carries_no_build_dates(self):
        """lib/libkos.lib is packed with `sdar -rcD`: every member's date
        and ids are zero, so the shipped library does not record when it
        was built."""
        members = list(_ar_members(os.path.join(self.tree, "lib",
                                                "libkos.lib")))
        self.assertGreater(len(members), 1)
        for name, mtime, uid, gid in members:
            # The long-name table's fields are blank, the rest zero.
            for field in (mtime, uid, gid):
                self.assertIn(field, ("", "0"), "%s: %r" % (name, field))

    def test_the_tarball_is_reproducible(self):
        """sdkpack pins member order, mtimes and ownership, so the shipped
        archive is exactly what packing its own tree again produces."""
        for entry in self.entries:
            self.assertEqual(entry.mtime, 0, entry.name)
            self.assertEqual((entry.uid, entry.gid), (0, 0), entry.name)
        repack = os.path.join(self.tmp, "repack")
        tree = os.path.join(repack, "kos-sdk-%s" % self.version)
        again = os.path.join(repack, "kos-sdk-%s.tar.gz" % self.version)
        sdkpack.build_tree(
            ROOT, tree, self.version,
            os.path.join(self.tree, "lib", "libkos.lib"),
            os.path.join(self.tree, "lib", "crt0.rel"))
        sdkpack.pack_tar(tree, again)
        # Same tree, same bytes: neither the member order nor the gzip
        # header carries anything of the build that produced them.
        self.assertEqual(Path(again).read_bytes(),
                         Path(self.tarball).read_bytes())

    def test_the_header_closure_is_complete(self):
        """Every include inside the tree resolves inside the tree, and
        every ABI header the SDK headers name is shipped."""
        headers = [os.path.join(dirpath, name).replace(self.tree + os.sep, "")
                   .replace(os.sep, "/")
                   for dirpath, _dirnames, filenames in os.walk(self.tree)
                   for name in filenames if name.endswith(".h")]
        resolved = set()
        for dest in headers:
            for include in _quoted_includes(os.path.join(self.tree, dest)):
                target = _resolve_include(self.tree, dest, include)
                self.assertIsNotNone(
                    target, "%s: %s does not resolve inside the SDK"
                    % (dest, include))
                resolved.add(target)
        # The closure reaches exactly the ABI headers the golden list
        # carries; the walk above proved each of them resolves its own
        # includes inside the tree too.
        abi = {dest for dest in resolved if dest.startswith("include/abi/")}
        self.assertEqual(abi, {dest for dest in SDK_FILES
                               if dest.startswith("include/abi/")})

    def test_the_standalone_tools_use_the_standard_library_only(self):
        """exepack, exemanifest and ihx ship inside the SDK: they must
        import nothing but the Python 3 standard library (and each
        other)."""
        local = {"exepack", "exemanifest", "ihx"}
        violations = []
        for name in sorted(local):
            source = _read(os.path.join(self.tree, "tools", "%s.py" % name))
            for node in ast.walk(ast.parse(source)):
                if isinstance(node, ast.Import):
                    modules = [alias.name for alias in node.names]
                elif isinstance(node, ast.ImportFrom):
                    modules = [node.module or ""]
                else:
                    continue
                for module in modules:
                    top = module.split(".")[0]
                    if top and top not in sys.stdlib_module_names \
                            and top not in local:
                        violations.append("%s.py imports %s" % (name, module))
        self.assertEqual(violations, [], "\n".join(violations))

    def test_version_matches_the_config_and_the_abi_header(self):
        abi = sdkpack.abi_version(_read(ABI_HEADER))
        self.assertEqual(_read(os.path.join(self.tree, "VERSION")),
                         "%s abi %d\n" % (self.version, abi))

    def test_nothing_in_the_sdk_names_the_os_tree(self):
        violations = []
        for dest in sorted(_tree_files(self.tree)):
            if dest.startswith("docs/"):
                continue
            data = Path(os.path.join(self.tree, dest)).read_bytes()
            for token in FORBIDDEN_TOKENS + (ROOT,):
                if token.encode("ascii") in data:
                    violations.append("%s: %s" % (dest, token))
        self.assertEqual(violations, [],
                         "the SDK names the OS tree:\n%s"
                         % "\n".join(violations))

    # -- the out-of-tree build and its programs ----------------------------

    def test_template_and_examples_build_outside_the_tree(self):
        """`make` in template/ and examples/ packs every program with the
        SDK alone; no compile or link names the OS tree."""
        self.assertEqual(sorted(self._build()),
                         ["fardata", "fardemo", "files", "hello"])

    def test_the_packed_program_runs_in_the_simulator(self):
        """The store carries the SDK-built hello.exe as /system/hello.exe;
        the shell runs it and the console shows its line."""
        self._require_image()
        hello = self._build()["hello"]
        stream = self._pack_test_store([("hello.exe", hello)])
        replay, z = self._boot(stream)
        replay.send_line(z, "/system/hello.exe", GREETING)

    def test_a_changed_abi_byte_is_refused_with_the_existing_error(self):
        """The template program with its ABI byte changed (offset 05h)
        must be refused by the loader: the shell reports the refusal
        and the program's line never appears, while the same store's
        unchanged copy (the other test) runs."""
        self._require_image()
        hello = self._build()["hello"]
        data = bytearray(Path(hello).read_bytes())
        self.assertEqual(data[0:4], b"KEX2")
        self.assertNotEqual(data[5], 0x02)
        data[5] = 0x02
        path = os.path.join(self.tmp, BAD_ABI_NAME)
        Path(path).write_bytes(bytes(data))
        stream = self._pack_test_store([("hello.exe", hello),
                                        (BAD_ABI_NAME, path)])
        replay, z = self._boot(stream)
        start = len(z.glyphs)
        replay.send_line(z, "/system/%s" % BAD_ABI_NAME, ABI_REFUSAL)
        refused = "".join(z.glyphs[start:])
        self.assertNotIn(GREETING, refused, "the refused EXE still ran")


if __name__ == "__main__":
    unittest.main()
