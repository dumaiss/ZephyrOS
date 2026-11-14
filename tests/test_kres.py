#!/usr/bin/env python3
"""The KRES core: host tests, golden fixtures and the host tool.

The host C tests (tests/kres/test_*.c) drive the portable core through
the instrumented in-memory adapter (tests/kres/kres_mem_io.c): the
design's §11 table, the §5.6 commit order with every flush write and
sync failed in turn (with and without catalog relocation) and torn
headers, short-read/write and seek/sync fault injection, a 10,000-op
random mutation oracle checked against verify, the compaction and
sealed policies, and catalogs from 0 to 1,024 resources with bounded
lookup reads.  test_scale also runs on the KonaOS XMEM catalog store
(user/sdk/kres/konaos/kres_cache_xmem.c) over a host XMEM shim.

The fixture and tool tests pin committed containers under
tests/kres/fixtures/ (one mutable, one sealed): the tool's list, verify
and extract outputs, the header and first catalog record bytes, a
pack/unpack round trip with the unsafe-name and collision refusals, and
a 300-resource pack.

Run with: make test
"""

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

KRES_SRC = os.path.join(ROOT, "user", "sdk", "kres", "src")
KRES_INC = os.path.join(ROOT, "user", "sdk", "include")
KRES_HOST_INC = os.path.join(ROOT, "user", "sdk", "kres", "host")
ABI_INC = os.path.join(ROOT, "abi")
TOOL = os.path.join(ROOT, "build", "host", "kres")
TOOL_SRC = os.path.join(ROOT, "tools", "kres", "main.c")
FIXTURES = os.path.join(ROOT, "tests", "kres", "fixtures")
TESTS_KRES = os.path.join(ROOT, "tests", "kres")

CORE_SOURCES = sorted(
    os.path.join(KRES_SRC, name)
    for name in os.listdir(KRES_SRC)
    if name.endswith(".c")
)
MEM_IO = os.path.join(TESTS_KRES, "kres_mem_io.c")

HOST_TESTS = [
    "test_basics",
    "test_commit",
    "test_faults",
    "test_verify",
    "test_compact_seal",
    "test_random",
    "test_scale",
]

# test_scale again, on the KonaOS XMEM catalog store over the shim.
XMEM_SHIM = os.path.join(TESTS_KRES, "xmem_shim")
XMEM_STORE = os.path.join(ROOT, "user", "sdk", "kres", "konaos",
                          "kres_cache_xmem.c")

# Pinned fixture facts (regenerate with the tool when the format or
# the writer changes; the pins below are the contract).
FIXTURE_SHA = {
    "mutable.kpr": "c4479b528e15590f979625e8a243d7c530d9493d07a5a5a337b60d703b9299df",
    "sealed.kpr": "4c75eb5409eda65b4340fd6e994fa1f2e4bf886fccfe3c17dd1976734961a9e5",
}
FIXTURE_HEADER = {
    # record 0, bytes 0..47
    "mutable.kpr": bytes.fromhex(
        "4b52455301000000"          # KRES 1.0
        "00020000"                  # 512-byte records, reserved
        "03000000"                  # generation 3 (three imports)
        "01"                        # active area B
        "00"                        # reserved
        "0300"                      # three resources
        "06000000"                  # next_record 6
        "00000000"                  # flags
        "0100000001000000"          # catalog A: record 1, 1 record
        "0200000001000000"          # catalog B: record 2, 1 record
        "2437"                      # header check
        "0000"),
    "sealed.kpr": bytes.fromhex(
        "4b52455301000000"
        "00020000"
        "00000000"
        "00"
        "00"
        "0300"
        "05000000"                  # next_record 5
        "01000000"                  # SEALED
        "0400000001000000"          # the catalog after the resources
        "0000000000000000"          # no area B
        "2845"
        "0000"),
}
FIXTURE_LIST = {
    "mutable.kpr": 'café                             6      1\nhello                             6      1\nres/a.txt                         6      1\n',
    "sealed.kpr": 'café                             6      1\nhello                             6      1\nres/a.txt                         6      1\n',
}
FIXTURE_EXTRACT_SHA = {
    ("mutable.kpr", "res/a.txt"):
        "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03",
    ("sealed.kpr", "café"):
        "2fb434dc9951bd4cb8e716b7491da41e0c12776d49150534cb1591694a6274bd",
}


def build_host_test(name, xmem=False):
    output = os.path.join(ROOT, "build", "host",
                          name + ("_xmem" if xmem else ""))
    os.makedirs(os.path.dirname(output), exist_ok=True)
    extra_inc = ["-DKRES_TEST_XMEM", "-I" + XMEM_SHIM] if xmem else []
    extra_src = ([os.path.join(XMEM_SHIM, "xmem_shim.c"), XMEM_STORE]
                 if xmem else [])
    command = (
        ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror"] + extra_inc
        + ["-I" + KRES_HOST_INC, "-I" + ABI_INC, "-I" + KRES_INC,
           "-I" + KRES_SRC, "-I" + TESTS_KRES]
        + [os.path.join(TESTS_KRES, name + ".c"), MEM_IO] + extra_src
        + CORE_SOURCES + ["-o", output]
    )
    subprocess.run(command, check=True, cwd=ROOT)
    return output


def build_tool():
    subprocess.run(["make", "kres-host"], check=True, cwd=ROOT,
                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def run(cwd, *args, check=True):
    return subprocess.run(
        list(args), cwd=cwd, check=check, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        digest.update(handle.read())
    return digest.hexdigest()


def regenerate_fixtures():
    """Regenerate tests/kres/fixtures with the current tool."""
    os.makedirs(FIXTURES, exist_ok=True)
    mutable = os.path.join(FIXTURES, "mutable.kpr")
    sealed = os.path.join(FIXTURES, "sealed.kpr")
    with tempfile.TemporaryDirectory() as tmp:
        run(tmp, TOOL, "create", mutable)
        hello = os.path.join(tmp, "hello.txt")
        cafe = os.path.join(tmp, "cafe.txt")
        with open(hello, "wb") as handle:
            handle.write(b"hello\n")
        with open(cafe, "wb") as handle:
            handle.write("olé!\n".encode("utf-8"))
        run(tmp, TOOL, "import", mutable, hello, "hello")
        run(tmp, TOOL, "import", mutable, hello, "res/a.txt")
        run(tmp, TOOL, "import", mutable, cafe, "café")
        run(tmp, TOOL, "seal", mutable, sealed)
    print("regenerated", mutable, sealed)


class KresHostTests(unittest.TestCase):
    """The §11 table and the failure-model rows, on the instrumented
    in-memory adapter."""

    @classmethod
    def setUpClass(cls):
        cls._binaries = {name: build_host_test(name)
                         for name in HOST_TESTS}
        cls._binaries["test_scale_xmem"] = build_host_test("test_scale",
                                                           xmem=True)

    def run_test_binary(self, name):
        result = subprocess.run(
            [self._binaries[name]], cwd=ROOT, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("OK", result.stdout)

    def test_basics(self):
        self.run_test_binary("test_basics")

    def test_commit(self):
        self.run_test_binary("test_commit")

    def test_faults(self):
        self.run_test_binary("test_faults")

    def test_verify(self):
        self.run_test_binary("test_verify")

    def test_compact_seal(self):
        self.run_test_binary("test_compact_seal")

    def test_random(self):
        self.run_test_binary("test_random")

    def test_scale(self):
        self.run_test_binary("test_scale")

    def test_scale_on_the_xmem_store(self):
        self.run_test_binary("test_scale_xmem")


class KresFixtures(unittest.TestCase):
    """The committed golden containers and the tool's outputs on
    them."""

    @classmethod
    def setUpClass(cls):
        build_tool()
        cls._missing = [name for name in ("mutable.kpr", "sealed.kpr")
                        if not os.path.exists(
                            os.path.join(FIXTURES, name))]

    def test_fixtures_present(self):
        self.assertEqual(self._missing, [])

    def test_fixture_bytes(self):
        for name, digest in FIXTURE_SHA.items():
            with self.subTest(name=name):
                self.assertEqual(sha256(os.path.join(FIXTURES, name)),
                                 digest)

    def test_fixture_headers(self):
        for name, header in FIXTURE_HEADER.items():
            with self.subTest(name=name):
                with open(os.path.join(FIXTURES, name), "rb") as handle:
                    first = handle.read(48)
                self.assertEqual(first, header)

    def test_fixture_catalog(self):
        # The first catalog entry of the active catalog, 64 bytes:
        # "café" (CP850 63 61 66 82) sorts first.
        pinned = {
            "mutable.kpr": (2 * 512, bytes.fromhex(
                "63616682"            # "caf\x82"
                + "00" * 44
                + "05000000"          # start 5
                + "01000000"          # allocated 1
                + "06000000"          # used 6
                + "04"                # name length 4
                + "00" + "0000")),
            "sealed.kpr": (4 * 512, bytes.fromhex(
                "63616682"
                + "00" * 44
                + "01000000"          # the first resource record
                + "01000000"
                + "06000000"
                + "04"
                + "00" + "0000")),
        }
        for name, (offset, entry) in pinned.items():
            with self.subTest(name=name):
                with open(os.path.join(FIXTURES, name), "rb") as handle:
                    handle.seek(offset)
                    self.assertEqual(handle.read(64), entry)

    def _tool(self, cwd, *args):
        result = run(cwd, TOOL, *args)
        self.assertEqual(result.returncode, 0, result.stdout)
        return result.stdout

    def test_tool_list_pinned(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name, expected in FIXTURE_LIST.items():
                with self.subTest(name=name):
                    self.assertEqual(
                        self._tool(tmp, "list",
                                   os.path.join(FIXTURES, name)),
                        expected)

    def test_tool_verify_pinned(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name in FIXTURE_LIST:
                with self.subTest(name=name):
                    out = self._tool(tmp, "verify",
                                     os.path.join(FIXTURES, name))
                    self.assertIn(": ok", out)

    def test_tool_extract_pinned(self):
        with tempfile.TemporaryDirectory() as tmp:
            for (name, resource), digest in FIXTURE_EXTRACT_SHA.items():
                with self.subTest(name=name, resource=resource):
                    out_path = os.path.join(tmp, "out.bin")
                    self._tool(tmp, "extract",
                               os.path.join(FIXTURES, name),
                               resource, out_path)
                    self.assertEqual(sha256(out_path), digest)


class KresToolRoundTrip(unittest.TestCase):
    """pack/unpack byte round trips and the export refusals."""

    @classmethod
    def setUpClass(cls):
        build_tool()

    def test_pack_unpack_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            project = os.path.join(tmp, "project")
            nested = os.path.join(project, "src", "deep")
            os.makedirs(nested)
            files = {
                "MAIN.MOD": b"module main;\n",
                "src/READ.ME": b"read me\n",
                "src/deep/DATA.BIN": bytes(range(256)) * 3,
            }
            for rel, payload in files.items():
                with open(os.path.join(project, rel), "wb") as handle:
                    handle.write(payload)
            packed = os.path.join(tmp, "out.kpr")
            run(tmp, TOOL, "pack", project, packed)
            run(tmp, TOOL, "verify", packed)
            export = os.path.join(tmp, "export")
            run(tmp, TOOL, "unpack", packed, export)
            for rel, payload in files.items():
                with open(os.path.join(export, rel), "rb") as handle:
                    self.assertEqual(handle.read(), payload)

    def test_pack_many_resources(self):
        """300 resources (past 256): packed sorted, the catalog after
        the payloads, every one extracted and unpacked byte for byte."""
        with tempfile.TemporaryDirectory() as tmp:
            project = os.path.join(tmp, "project")
            os.makedirs(os.path.join(project, "scenes"))
            os.makedirs(os.path.join(project, "music"))
            files = {}
            for i in range(300):
                rel = ("scenes/S%03d.ZVG" % i if i % 2 == 0
                       else "music/M%03d" % i)
                files[rel] = bytes((i * 7 + j) & 0xFF
                                   for j in range((i * 13) % 900))
            for rel, payload in files.items():
                with open(os.path.join(project, rel), "wb") as handle:
                    handle.write(payload)
            packed = os.path.join(tmp, "many.kpr")
            run(tmp, TOOL, "pack", project, packed)
            out = run(tmp, TOOL, "verify", packed).stdout
            self.assertIn("resources 300 (catalog capacity 304)", out)
            self.assertIn("0 abandoned", out)
            listed = run(tmp, TOOL, "list", packed).stdout.splitlines()
            names = [line.split()[0] for line in listed]
            self.assertEqual(names, sorted(files))
            with open(packed, "rb") as handle:
                header = handle.read(48)
            next_record = int.from_bytes(header[0x14:0x18], "little")
            catalog = int.from_bytes(header[0x1C:0x20], "little")
            self.assertEqual(int.from_bytes(header[0x20:0x24], "little"),
                             38)
            self.assertEqual(catalog + 38, next_record,
                             "the catalog ends the sealed container")
            for rel in ("music/M001", "scenes/S150.ZVG", "scenes/S298.ZVG"):
                target = os.path.join(tmp, "one.bin")
                run(tmp, TOOL, "extract", packed, rel, target)
                with open(target, "rb") as handle:
                    self.assertEqual(handle.read(), files[rel])
            export = os.path.join(tmp, "export")
            run(tmp, TOOL, "unpack", packed, export)
            for rel, payload in files.items():
                with open(os.path.join(export, rel), "rb") as handle:
                    self.assertEqual(handle.read(), payload)

    def test_import_past_one_catalog_record_and_compact(self):
        """A mutable container grows its catalog transparently; the
        abandoned catalog areas go at compaction."""
        with tempfile.TemporaryDirectory() as tmp:
            container = os.path.join(tmp, "grow.kpr")
            src = os.path.join(tmp, "src.bin")
            with open(src, "wb") as handle:
                handle.write(b"payload")
            run(tmp, TOOL, "create", container)
            for i in range(40):
                run(tmp, TOOL, "import", container, src, "r%02d" % (39 - i))
            out = run(tmp, TOOL, "verify", container).stdout
            self.assertIn("resources 40 (catalog capacity 64)", out)
            self.assertNotIn(" 0 abandoned", out)
            run(tmp, TOOL, "compact", container)
            out = run(tmp, TOOL, "verify", container).stdout
            self.assertIn("resources 40 (catalog capacity 40)", out)
            self.assertIn(" 0 abandoned", out)
            names = [line.split()[0] for line in
                     run(tmp, TOOL, "list", container).stdout.splitlines()]
            self.assertEqual(names, ["r%02d" % i for i in range(40)])

    def test_unpack_refuses_unsafe_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            container = os.path.join(tmp, "evil.kpr")
            run(tmp, TOOL, "create", container)
            src = os.path.join(tmp, "src.bin")
            with open(src, "wb") as handle:
                handle.write(b"x")
            run(tmp, TOOL, "import", container, src, "fine")
            run(tmp, TOOL, "import", container, src, "../evil")
            run(tmp, TOOL, "import", container, src, "ok/../up")
            result = run(tmp, TOOL, "unpack", container,
                         os.path.join(tmp, "out"), check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("refus", result.stdout)

    def test_unpack_refuses_case_collisions(self):
        with tempfile.TemporaryDirectory() as tmp:
            container = os.path.join(tmp, "case.kpr")
            run(tmp, TOOL, "create", container)
            src = os.path.join(tmp, "src.bin")
            with open(src, "wb") as handle:
                handle.write(b"y")
            run(tmp, TOOL, "import", container, src, "A.TXT")
            run(tmp, TOOL, "import", container, src, "a.txt")
            result = run(tmp, TOOL, "unpack", container,
                         os.path.join(tmp, "out"), check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("collision", result.stdout)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "regen-fixtures":
        build_tool()
        regenerate_fixtures()
    else:
        unittest.main()
