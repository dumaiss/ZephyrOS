#!/usr/bin/env python3
"""The KRES core: host tests, golden fixtures and the host tool.

The host C tests (tests/kres/test_*.c) drive the portable core through
the instrumented in-memory adapter (tests/kres/kres_mem_io.c): the
design's §11 table, the §5.6 commit order with every flush write failed
in turn, short-read/write and seek/sync fault injection, a 10,000-op
random mutation oracle checked against verify, and the compaction and
sealed policies.

The fixture and tool tests pin committed containers under
tests/kres/fixtures/ (one mutable, one sealed): the tool's list, verify
and extract outputs, the header and first catalog record bytes, and a
pack/unpack round trip with the unsafe-name and collision refusals.

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
]

# Pinned fixture facts (regenerate with the tool when the format or
# the writer changes; the pins below are the contract).
FIXTURE_SHA = {
    "mutable.kpr": "24d29c46d3de55a05850786c8d90f079d66a62e770beab0bf3fc8d72b1709f44",
    "sealed.kpr": "8bffa5aa72724ed582e9240a6c4e66d0062021af1442751ad514317c78a8a89c",
}
FIXTURE_HEADER = {
    # record 0, bytes 0..31
    "mutable.kpr": bytes.fromhex(
        "4b5245530100000000020400"  # KRES 1.0, 512-byte records, 4
        "03000000"                  # generation 3 (three imports)
        "01"                        # active area B
        "00"                        # reserved
        "0300"                      # three resources
        "0c000000"                  # next_record 12
        "00000000"                  # flags
        "00000000"),
    "sealed.kpr": bytes.fromhex(
        "4b5245530100000000020400"
        "00000000"
        "00"
        "00"
        "0300"
        "08000000"
        "01000000"                  # SEALED
        "00000000"),
}
FIXTURE_LIST = {
    "mutable.kpr": 'hello                             6      1\nres/a.txt                         6      1\ncafé                             6      1\n',
    "sealed.kpr": 'hello                             6      1\nres/a.txt                         6      1\ncafé                             6      1\n',
}
FIXTURE_EXTRACT_SHA = {
    ("mutable.kpr", "res/a.txt"):
        "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03",
    ("sealed.kpr", "café"):
        "2fb434dc9951bd4cb8e716b7491da41e0c12776d49150534cb1591694a6274bd",
}


def build_host_test(name):
    output = os.path.join(ROOT, "build", "host", name)
    os.makedirs(os.path.dirname(output), exist_ok=True)
    command = (
        ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror"]
        + ["-I" + KRES_HOST_INC, "-I" + ABI_INC, "-I" + KRES_INC,
           "-I" + KRES_SRC, "-I" + TESTS_KRES]
        + [os.path.join(TESTS_KRES, name + ".c"), MEM_IO] + CORE_SOURCES
        + ["-o", output]
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
                    first = handle.read(32)
                self.assertEqual(first, header)

    def test_fixture_catalog(self):
        # The first catalog entry of the active area, 64 bytes.
        pinned = {
            "mutable.kpr": (5 * 512, bytes.fromhex(
                "68656c6c6f"          # "hello"
                + "00" * 43
                + "09000000"          # start 9
                + "01000000"          # allocated 1
                + "06000000"          # used 6
                + "05"                # name length 5
                + "00" + "0000")),
            "sealed.kpr": (1 * 512, bytes.fromhex(
                "68656c6c6f"
                + "00" * 43
                + "05000000"
                + "01000000"
                + "06000000"
                + "05"
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
