#!/usr/bin/env python3
"""Far-data outcome acceptance.

Two halves:

**Host.**  The build rules and the packer are exercised through the real
`exe_program` macro: a throwaway program tree is compiled and linked with
`make -f Makefile -f <extra>.mk`, so the checks are the rule a user
gets, not a copy of it.

- the generated rows are the far-code segments first, then one mappable
  `DATA` row per `DATAn=` option (ascending), then one plain `DATA` row
  per `BLOBS=` file (in order);
- the row flag is accepted on `DATA` rows only, and only up to the 16 KiB
  aperture; a mappable `DATAn` source over that is refused;
- an initialized non-`const` object in a `DATAn` source is refused by the
  packer, naming the symbol;
- a `BLOBS` file over 32 KiB is refused;
- the spike's findings, asserted on a real link: every `DATAn` pair and
  every `FARn` area is based at `8000h`, each `_DATA<n>B` starts exactly
  at the end of its `_DATA<n>` image, the primary chain keeps the
  program's own memory below `8000h`, and the packed header's
  `bss_size` is the primary BSS the map reports (the loader zeroes
  exactly that, so a header of 0 would leave every static uninitialized).

**Replay.**  The production image ships `/system/far_data.exe` (the
outcome program in `tests/fardata/`) plus the `banks.exe` container that
cannot fit in the machine's auxiliary banks, and `known.bin`, the store
file the program reads.  Booted with a writable card, the monitor's `X`
runs the program and the transcript must report every outcome and the
verdict:

- the DATA1 pair mapped with `farmap`/`farunmap`: the table reads through
  the plain C symbols, the zeroed half reads as zeroes and takes writes,
  the caller's latch comes back exactly;
- `seginfo` finds the segment as mappable DATA of the linked size;
- `read` into a mappable object straight through the open window, from
  `/fs` and from `/system`, leaves the program's own window memory (the
  blob row and the DATA1 segment) untouched;
- `blob(n)` plus `xread` returns the payload's bytes;
- `xload` of a store file, plain and mappable, matches the file; a 48 KiB
  file is `-KOS_ENOSPC` and nothing leaks;
- `run("/system/banks.exe")` is refused with `-KOS_ENOSPC` because the
  container's six rows need more auxiliary banks than the machine has,
  and nothing leaks;
- a `__banked` call takes table values by copy and restores the caller's
  latch (one window serves far code or far data, never both).

Run with: `python3 -m unittest tests.test_far_data`.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import exepack  # noqa: E402
import ihx  # noqa: E402
from replay import PROMPT, STEP_LIMIT, ConsoleReplay  # noqa: E402
from z80sim import IocCommandPeer  # noqa: E402

IMAGE_SUBDIR = "build"
IMAGE_NAME = "konaos.bin"

RUN_LINE = "X /system/far_data.exe"
BEGIN = "far-data: begin\n"
PASS = "far-data: PASS"
FAIL = "far-data: FAIL"
EXIT_OK = "exec: exit status 0x0000"
CHECKS = "far-data: 39 checks, 0 failures"

# Every outcome line the program must report, in order.
OUTCOMES = (
    "farmap-window", "farmap-prev", "table-read", "bss-zeroed",
    "bss-write-read", "seginfo-flags", "seginfo-size", "farunmap-latch",
    "xalloc-mappable", "xmap-window", "card-write", "read-into-window",
    "read-window-bytes", "read-system", "read-system-bytes", "blob-handle",
    "blob-read", "blob-bytes", "segment-untouched", "xload-plain",
    "xload-plain-read", "xload-plain-bytes", "xload-plain-free",
    "xload-mappable", "xload-mappable-bytes", "xload-mappable-free",
    "big-write", "xload-over-mappable", "xload-over-plain",
    "xload-no-leak", "xload-no-leak-free", "exec-banks-refused",
    "exec-no-leak", "exec-no-leak-free", "far-segmap", "farmap-again",
    "farunmap-again", "banked-copy", "banked-latch",
)

FAR_LEVELS = """const unsigned char tab[8] = {1, 2, 3, 4, 5, 6, 7, 8};
unsigned char zeroes[16];
"""

MAIN = """int main(void)
{
	return 0;
}
"""

BIG_TABLE = """const unsigned char big[20480] = { 1 };
"""

INIT_VALUE = """unsigned char init_value = 5;
const unsigned char tab[8] = {1};
"""

FAR_STUB = """int far_entry(int value) __banked
{
	return value;
}
"""


def _read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


class FarDataBuild(unittest.TestCase):
    """One throwaway program tree, built by the real rules."""

    tmp = None
    build_dir = None

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="kosfardata-")
        cls.build_dir = os.path.join(cls.tmp, "build")
        cls.extra = os.path.join(cls.tmp, "extra.mk")
        cls.extra_entries = []

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def source(self, name, text):
        path = os.path.join(self.tmp, name)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(text)
        return path

    def declare(self, name, directory, sources, options=()):
        """Add one exe_program line to the tree's extra rule file and
        return the command that builds it."""
        args = ", ".join([name, directory] + list(sources) + list(options))
        self.extra_entries.append(
            "$(eval $(call exe_program,%s))" % args)
        with open(self.extra, "w", encoding="utf-8") as handle:
            handle.write("\n".join(self.extra_entries) + "\n")
        return ["make", "-C", ROOT, "BUILD=" + self.build_dir,
                "-f", "Makefile", "-f", self.extra, name]

    def run_make(self, command):
        result = subprocess.run(command, cwd=ROOT, capture_output=True,
                                text=True)
        return result.returncode, result.stdout + result.stderr

    def build_ok(self, command):
        code, output = self.run_make(command)
        self.assertEqual(code, 0, output)
        return output

    def build_fails(self, command, needle):
        code, output = self.run_make(command)
        self.assertNotEqual(code, 0, output)
        self.assertIn(needle, output)
        return output

    def artifacts(self, name):
        base = os.path.join(self.build_dir, "user", name)
        return (os.path.join(base, name + ".json"),
                os.path.join(base, name + ".map"),
                os.path.join(self.build_dir, "user", name + ".exe"))

    def rows(self, name):
        """The packed container's segment rows and primary geometry."""
        _manifest, _map, exe = self.artifacts(name)
        data = open(exe, "rb").read()
        fields = exepack.parse_header(data)
        rows = []
        for index in range(fields["segment_count"]):
            offset = (fields["segment_table_offset"] +
                      index * exepack.SEGMENT_ENTRY_SIZE)
            rows.append(exepack.parse_segment_row(data[offset:]))
        return fields, rows


class TestTheGeneratedLayout(FarDataBuild):

    def test_far_then_data_then_blobs(self):
        """The rule generates the rows the loader and the SDK's ordinals
        expect: FAR1 first, then DATA1/DATA2 (mappable), then the two
        payload files in the order given."""
        self.source("main.c", MAIN)
        self.source("levels1.c", FAR_LEVELS)
        self.source("levels2.c", FAR_LEVELS.replace("tab", "tab2")
                    .replace("zeroes", "zeroes2"))
        self.source("one.bin", "one\n")
        self.source("two.bin", "two\n")
        self.source("far.c", FAR_STUB)
        command = self.declare(
            "layout", self.tmp, ["main.c"],
            ["STACK=256", "FAR1=far.c", "DATA1=levels1.c",
             "DATA2=levels2.c", "BLOBS=one.bin:two.bin"])
        self.build_ok(command)
        fields, rows = self.rows("layout")
        self.assertEqual(fields["segment_count"], 5)
        self.assertEqual([(row["id"], row["kind"], row["flags"])
                          for row in rows],
                         [(1, exepack.KIND_FAR_CODE, 0),
                          (2, exepack.KIND_DATA, exepack.SEG_FLAG_MAPPABLE),
                          (3, exepack.KIND_DATA, exepack.SEG_FLAG_MAPPABLE),
                          (4, exepack.KIND_DATA, 0),
                          (5, exepack.KIND_DATA, 0)])
        self.assertEqual([row["image_size"] for row in rows[3:]],
                         [4, 4])

    def test_the_flag_is_only_for_data_rows_and_the_aperture(self):
        for row in (
                {"id": 1, "kind": "FAR_CODE", "area": "_FAR1",
                 "bss_size": 0, "flags": 1},
                {"id": 1, "kind": "DATA", "input": "one.bin",
                 "bss_size": 0, "flags": 2}):
            with self.assertRaises(exepack.ExeError) as caught:
                exepack._validate_segment_manifest([row])
            self.assertEqual(caught.exception.code,
                             "EXE_EXEC_ERR_SEGMENT")
        exepack._validate_segment_manifest(
            [{"id": 1, "kind": "DATA", "area": "_DATA1",
              "bss_area": "_DATA1B", "flags": 1}])

    def test_a_mappable_source_over_the_aperture_is_refused(self):
        """`DATAn=` sources are limited to the 16 KiB the window holds:
        the row's image plus BSS must fit, exactly as for a far-code
        segment."""
        self.source("main.c", MAIN)
        self.source("bigtable.c", BIG_TABLE)
        command = self.declare("toobig", self.tmp, ["main.c"],
                               ["STACK=256", "DATA1=bigtable.c"])
        self.build_fails(command, "over the 16384-byte aperture")

    def test_an_initialized_writable_object_is_named(self):
        """Far data is either `const` or zeroed: an initialized writable
        object has no startup support, so the packer refuses it and says
        which symbol did it."""
        self.source("main.c", MAIN)
        self.source("init.c", INIT_VALUE)
        command = self.declare("init", self.tmp, ["main.c"],
                               ["STACK=256", "DATA1=init.c"])
        output = self.build_fails(command, "initialized writable data")
        self.assertIn("_init_value", output)

    def test_a_blob_over_the_object_limit_is_refused(self):
        self.source("main.c", MAIN)
        with open(os.path.join(self.tmp, "big.bin"), "w") as handle:
            handle.write("x" * 33792)
        command = self.declare("bigblob", self.tmp, ["main.c"],
                               ["STACK=256", "BLOBS=big.bin"])
        self.build_fails(command, "over the 32768 limit")


class TestTheSpikeLayout(FarDataBuild):

    def test_every_pair_and_the_far_area_sit_at_8000h(self):
        """The spike's findings, on the link the rule makes: the primary
        chain stays below the aperture, every `_DATA<n>`/`_FAR<n>` area is
        based at 8000h, and each `_DATA<n>B` follows its image; the packed
        header carries the primary BSS the map reports."""
        self.source("main.c", MAIN)
        self.source("levels1.c", FAR_LEVELS)
        self.source("levels2.c", FAR_LEVELS.replace("tab", "tab2")
                    .replace("zeroes", "zeroes2"))
        self.source("far.c", FAR_STUB)
        command = self.declare("spike", self.tmp, ["main.c"],
                               ["STACK=256", "FAR1=far.c",
                                "DATA1=levels1.c", "DATA2=levels2.c"])
        self.build_ok(command)
        manifest_path, map_path, _exe = self.artifacts("spike")
        symbols = ihx.parse_map_symbols(_read(map_path))
        manifest = json.loads(_read(manifest_path))

        self.assertEqual(symbols["s__FAR1"], 0x8000)
        self.assertEqual(symbols["s__DATA1"], 0x8000)
        self.assertEqual(symbols["s__DATA1B"],
                         0x8000 + symbols["l__DATA1"])
        self.assertEqual(symbols["s__DATA2"], 0x8000)
        self.assertEqual(symbols["s__DATA2B"],
                         0x8000 + symbols["l__DATA2"])
        # The primary chain: the program's own image and BSS, below the
        # aperture the packer requires for a far-data program.
        self.assertEqual(symbols["s__DATA"],
                         exepack.LOAD_BASE + manifest["image_size"])
        self.assertEqual(symbols["l__DATA"], manifest["bss_size"])
        self.assertLess(symbols["s__DATA"] + symbols["l__DATA"], 0x8000)

        fields, rows = self.rows("spike")
        self.assertEqual(fields["bss_size"], manifest["bss_size"])
        self.assertEqual(fields["image_size"], manifest["image_size"])
        self.assertEqual([row["bss_size"] for row in rows],
                         [0, symbols["l__DATA1B"], symbols["l__DATA2B"]])


class FarDataSim(ConsoleReplay, unittest.TestCase):
    """The production image, run through the monitor."""

    IMAGE_SUBDIR = IMAGE_SUBDIR
    IMAGE_NAME = IMAGE_NAME

    def setUp(self):
        image = os.path.join(ROOT, IMAGE_SUBDIR, IMAGE_NAME)
        if not os.path.isfile(image):
            self.skipTest("%s is missing; run make" % image)
        self.load_image()

    def make_cpu(self, **peer_kwargs):
        z = super().make_cpu(**peer_kwargs)
        peer = IocCommandPeer(fs2_media=True)
        z.hid = peer
        z.ioc_peer = peer
        return z

    def test_the_far_data_outcomes_in_the_production_image(self):
        """`X /system/far_data.exe` reports every far-data outcome, the
        pinned check count and the verdict, and exits 0."""
        z = self.make_cpu()
        self.boot_to_prompt(z)
        start = len(z.glyphs)
        self.send_line(z, RUN_LINE, PROMPT, limit=STEP_LIMIT)
        text = "".join(z.glyphs[start:])

        self.assertIn(BEGIN, text)
        for label in OUTCOMES:
            self.assertIn("far-data: %s OK\n" % label, text)
        self.assertNotIn(FAIL, text)
        self.assertIn(CHECKS, text)
        self.assertIn(PASS, text)
        self.assertIn(EXIT_OK, text)


if __name__ == "__main__":
    unittest.main()
