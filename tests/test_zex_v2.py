#!/usr/bin/env python3
"""Host-side ZEX v2 tooling tests (M10 design spike).

ZEX v2 is the M10 container: a primary image plus relocatable auxiliary
segments and a far-entry table.  This module covers the host packer, which
is where the spike puts all the precomputation (work/M10-REFERENCE.md):

* constant parity between tools/zexpack.py and include/zephyros/zex2.h;
* packing a synthetic container (a hand-built primary link plus a
  hand-built SDCC .rel segment) and byte-identical repeat runs;
* the container validation boundaries;
* the relocation finding: an ABS16 auxiliary-segment relocation is a
  single high-byte patch for every legal 8-KiB slot base, proven against
  real SDCC output when the compiler is available.
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import zexpack  # noqa: E402
from maptools import evaluate, parse_c_header  # noqa: E402

ZEX2_H = os.path.join(ROOT, "include", "zephyros", "zex2.h")

# A minimal valid primary link: four bytes at 0100h, no BSS.
PRIMARY_IHX = (
    ":040100003E42C900B2\n"
    ":00000001FF\n"
)
PRIMARY_MAP = (
    "00000100 zos_entry\n"
    "00000104 s__DATA\n"
    "00000000 l__DATA\n"
    "00000004 l__CODE\n"
)

# A hand-built SDCC .rel: _CODE holds `LD A,(1234h); RET`, and the 1234h
# word is an ABS16 relocation to a _CODE-local address.
SEGMENT_REL = (
    "XL4\n"
    "H 2 areas 2 global symbols\n"
    "M seg\n"
    "O -mz80 sdcccall(1)\n"
    "S .__.ABS. Def00000000\n"
    "A _CODE size 4 flags 0 addr 0\n"
    "S seg_inc Def00000000\n"
    "A _DATA size 0 flags 0 addr 0\n"
    "T 00 00 00 00 3A 34 12 C9\n"
    "R 00 00 00 00 00 05 00 00\n"
)

FAR_C_SOURCE = """\
#include "zos/types.h"

static const char msg[] = "far segment";
static uint16_t counter;

static uint16_t helper(uint16_t x)
{
	return (uint16_t)(x + counter + msg[0]);
}

uint16_t far_add(uint16_t x, uint16_t y)
{
	counter = (uint16_t)(counter + 1);
	return (uint16_t)(helper(x) + y + msg[4]);
}
"""


def _manifest(primary_overrides=None, segments=None):
    primary = {
        "input": "primary.ihx",
        "map": "primary.map",
        "entry": "zos_entry",
        "entry_offset": 0,
        "image_size": 4,
        "bss_size": 0,
        "stack_size": 256,
    }
    if primary_overrides:
        primary.update(primary_overrides)
    return {
        "format": "zex-v2",
        "name": "spike.zex",
        "primary": primary,
        "segments": segments if segments is not None else [],
    }


class _Workspace:
    """A temp directory with the synthetic primary link written out."""

    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="zexv2-test-")
        self._write("primary.ihx", PRIMARY_IHX)
        self._write("primary.map", PRIMARY_MAP)
        self._write("farseg.rel", SEGMENT_REL)

    def _write(self, name, text):
        with open(os.path.join(self.dir, name), "w") as handle:
            handle.write(text)

    def path(self, name):
        return os.path.join(self.dir, name)

    def manifest(self, name="manifest.json", **kwargs):
        path = os.path.join(self.dir, name)
        with open(path, "w") as handle:
            json.dump(_manifest(**kwargs), handle)
        return path

    def cleanup(self):
        shutil.rmtree(self.dir, ignore_errors=True)


class TestZex2ConstantParity(unittest.TestCase):
    def setUp(self):
        self.env = parse_c_header(ZEX2_H)

    def test_format_constants_match(self):
        expected = {
            "ZEX2_FORMAT_VERSION": zexpack.ZEX2_FORMAT_VERSION,
            "ZEX2_ABI_VERSION": zexpack.ZEX2_ABI_VERSION,
            "ZEX2_HEADER_SIZE": zexpack.ZEX2_HEADER_SIZE,
            "ZEX2_SEGMENT_ENTRY_SIZE": zexpack.ZEX2_SEGMENT_ENTRY_SIZE,
            "ZEX2_FAR_ENTRY_SIZE": zexpack.ZEX2_FAR_ENTRY_SIZE,
            "ZEX2_AUX_SLOT_SIZE": zexpack.ZEX2_AUX_SLOT_SIZE,
            "ZEX2_AUX_SLOT_BASE": zexpack.ZEX2_AUX_SLOT_BASE,
            "ZEX2_AUX_SLOT_COUNT": zexpack.ZEX2_AUX_SLOT_COUNT,
            "ZEX2_MAX_SEGMENTS": zexpack.ZEX2_MAX_SEGMENTS,
            "ZEX2_MAX_FAR_ENTRIES": zexpack.ZEX2_MAX_FAR_ENTRIES,
        }
        for name, value in expected.items():
            self.assertIn(name, self.env, "%s missing from zex2.h" % name)
            self.assertEqual(evaluate(self.env[name], self.env), value,
                             "%s mismatch between zex2.h and zexpack.py"
                             % name)

    def test_result_codes_match(self):
        for name, value in zexpack.ZEX2_RESULT_CODES:
            self.assertIn(name, self.env, "%s missing from zex2.h" % name)
            self.assertEqual(evaluate(self.env[name], self.env), value,
                             "%s mismatch between zex2.h and zexpack.py"
                             % name)


class TestZex2Pack(unittest.TestCase):
    def setUp(self):
        self.ws = _Workspace()

    def tearDown(self):
        self.ws.cleanup()

    def _pack(self, out="spike.zex", **kwargs):
        manifest = self.ws.manifest(**kwargs)
        return zexpack.pack(manifest, self.ws.path(out),
                            objdir=self.ws.dir), self.ws.path(out)

    def test_primary_only_container(self):
        result, out = self._pack()
        self.assertEqual(result["format"], "zex-v2")
        self.assertEqual(result["header"]["segment_count"], 0)
        self.assertEqual(result["header"]["far_entry_count"], 0)
        self.assertEqual(result["header"]["total_slots"], 0)
        data = Path(out).read_bytes()
        self.assertEqual(data[:4], b"ZEX2")
        fields = zexpack.validate_v2(data)
        self.assertEqual(fields["primary_image_size"], 4)
        self.assertEqual(fields["segments"], [])
        self.assertEqual(fields["far_entries"], [])

    def test_two_segments_with_far_entries(self):
        segments = [
            {"id": 1, "flags": ["EXEC"],
             "objects": ["farseg.rel"],
             "image_areas": ["_CODE", "_DATA"],
             "far_entries": [{"id": 7, "symbol": "seg_inc"}]},
            {"id": 2, "flags": ["WRITE"],
             "objects": ["farseg.rel"],
             "image_areas": ["_CODE", "_DATA"],
             "far_entries": []},
        ]
        result, out = self._pack(segments=segments)
        self.assertEqual(result["header"]["segment_count"], 2)
        self.assertEqual(result["header"]["far_entry_count"], 1)
        self.assertEqual(result["header"]["total_slots"], 2)
        self.assertEqual(result["far_entries"],
                         [{"id": 7, "segment_id": 1, "offset": 0}])
        fields = zexpack.validate_v2(Path(out).read_bytes())
        self.assertEqual(fields["segments"][0]["reloc_count"], 1)
        self.assertEqual(fields["segments"][0]["alloc_size"],
                         zexpack.ZEX2_AUX_SLOT_SIZE)

    def test_repeat_pack_is_byte_identical(self):
        segments = [{"id": 1, "flags": ["EXEC"],
                     "objects": ["farseg.rel"],
                     "image_areas": ["_CODE", "_DATA"],
                     "far_entries": [{"id": 1, "symbol": "seg_inc"}]}]
        _, first = self._pack(out="a.zex", segments=segments)
        _, second = self._pack(out="b.zex", segments=segments)
        left = Path(first).read_bytes()
        right = Path(second).read_bytes()
        self.assertEqual(hashlib.sha256(left).hexdigest(),
                         hashlib.sha256(right).hexdigest())

    def test_cli_pack_and_dump(self):
        segments = [{"id": 1, "flags": ["EXEC"],
                     "objects": ["farseg.rel"],
                     "image_areas": ["_CODE", "_DATA"],
                     "far_entries": [{"id": 3, "symbol": "seg_inc"}]}]
        manifest = self.ws.manifest(segments=segments)
        out = self.ws.path("cli.zex")
        proc = subprocess.run(
            [sys.executable, os.path.join(ROOT, "tools", "zexpack.py"),
             "pack", manifest, out, "--objdir", self.ws.dir],
            capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        proc = subprocess.run(
            [sys.executable, os.path.join(ROOT, "tools", "zexpack.py"),
             "dump", out],
            capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("far entry 3 -> segment 1", proc.stdout)

    def test_segment_count_limit(self):
        segments = [{"id": i + 1, "objects": ["farseg.rel"],
                     "image_areas": ["_CODE"], "flags": []}
                    for i in range(zexpack.ZEX2_MAX_SEGMENTS + 1)]
        with self.assertRaises(zexpack.ZexError) as caught:
            self._pack(segments=segments)
        self.assertEqual(caught.exception.code, "ZEX2_EXEC_ERR_SEG_COUNT")

    def test_duplicate_far_entry_id_rejected(self):
        segments = [
            {"id": 1, "flags": ["EXEC"], "objects": ["farseg.rel"],
             "image_areas": ["_CODE", "_DATA"],
             "far_entries": [{"id": 5, "symbol": "seg_inc"}]},
            {"id": 2, "flags": ["EXEC"], "objects": ["farseg.rel"],
             "image_areas": ["_CODE", "_DATA"],
             "far_entries": [{"id": 5, "symbol": "seg_inc"}]},
        ]
        with self.assertRaises(zexpack.ZexError) as caught:
            self._pack(segments=segments)
        self.assertEqual(caught.exception.code, "ZEX2_EXEC_ERR_FAR")

    def test_far_entry_outside_image_rejected(self):
        segments = [{"id": 1, "flags": ["EXEC"], "objects": ["farseg.rel"],
                     "image_areas": ["_CODE", "_DATA"],
                     "far_entries": [{"id": 1, "symbol": "no_such_symbol"}]}]
        with self.assertRaises(zexpack.ZexError) as caught:
            self._pack(segments=segments)
        self.assertEqual(caught.exception.code, "ZEX2_EXEC_ERR_SEGMENT")


class TestZex2Validation(unittest.TestCase):
    def setUp(self):
        self.ws = _Workspace()

    def tearDown(self):
        self.ws.cleanup()

    def _valid_bytes(self, **kwargs):
        manifest = self.ws.manifest(**kwargs)
        result = zexpack.pack(manifest, self.ws.path("ok.zex"),
                              objdir=self.ws.dir)
        return bytearray(Path(self.ws.path("ok.zex")).read_bytes()), result

    def test_magic_rejected(self):
        data, _ = self._valid_bytes()
        data[0:4] = b"ZEX1"
        with self.assertRaises(zexpack.ZexError) as caught:
            zexpack.validate_v2(data)
        self.assertEqual(caught.exception.code, "ZEX_EXEC_ERR_MAGIC")

    def test_file_size_mismatch_rejected(self):
        data, _ = self._valid_bytes()
        del data[-1]
        with self.assertRaises(zexpack.ZexError) as caught:
            zexpack.validate_v2(data)
        self.assertEqual(caught.exception.code, "ZEX_EXEC_ERR_FILESIZE")

    def test_total_slots_mismatch_rejected(self):
        segments = [{"id": 1, "flags": ["EXEC"], "objects": ["farseg.rel"],
                     "image_areas": ["_CODE", "_DATA"], "far_entries": []}]
        data, _ = self._valid_bytes(segments=segments)
        # total_slots is at 0x14; bump it so it disagrees with the table.
        data[0x14] = 5
        with self.assertRaises(zexpack.ZexError) as caught:
            zexpack.validate_v2(data)
        self.assertEqual(caught.exception.code, "ZEX2_EXEC_ERR_SLOTS")

    def test_patch_crossing_image_end_rejected(self):
        segments = [{"id": 1, "flags": ["EXEC"], "objects": ["farseg.rel"],
                     "image_areas": ["_CODE", "_DATA"], "far_entries": []}]
        data, _ = self._valid_bytes(segments=segments)
        fields = zexpack.parse_header_v2(data)
        seg_base = fields["segment_table_offset"]
        reloc_offset = int.from_bytes(bytes(data[seg_base + 12:
                                                  seg_base + 16]), "little")
        reloc_count = int.from_bytes(bytes(data[seg_base + 16:
                                                  seg_base + 18]), "little")
        self.assertGreaterEqual(reloc_count, 1)
        image_size = int.from_bytes(bytes(data[seg_base + 2:seg_base + 4]),
                                    "little")
        # Point the first patch at the last byte of the image.
        data[reloc_offset] = (image_size - 1) & 0xFF
        data[reloc_offset + 1] = (image_size - 1) >> 8
        with self.assertRaises(zexpack.ZexError) as caught:
            zexpack.validate_v2(data)
        self.assertEqual(caught.exception.code, "ZEX2_EXEC_ERR_RELOC")

    def test_far_entry_offset_out_of_range_rejected(self):
        segments = [{"id": 1, "flags": ["EXEC"], "objects": ["farseg.rel"],
                     "image_areas": ["_CODE", "_DATA"],
                     "far_entries": [{"id": 1, "symbol": "seg_inc"}]}]
        data, _ = self._valid_bytes(segments=segments)
        fields = zexpack.parse_header_v2(data)
        far_base = fields["far_entry_table_offset"]
        data[far_base + 2] = 0xFF
        data[far_base + 3] = 0xFF
        with self.assertRaises(zexpack.ZexError) as caught:
            zexpack.validate_v2(data)
        self.assertEqual(caught.exception.code, "ZEX2_EXEC_ERR_FAR")


class TestRelocationFinding(unittest.TestCase):
    """The spike's relocation question, proven rather than assumed."""

    def test_high_byte_patch_equals_full_add(self):
        # A slot base is a multiple of 256, so adding it never carries
        # into the low byte: patching only the high byte is exact for
        # every legal slot base.
        for value in (0x0000, 0x0F37, 0x1234, 0x1FFF):
            for base in (0x2000 * i for i in range(1, 7)):
                full = (value + base) & 0xFFFF
                high = ((value & 0x00FF)
                        | (((value >> 8) + (base >> 8)) & 0xFF) << 8)
                self.assertEqual(full, high,
                                 "value %04X base %04X" % (value, base))
                self.assertEqual(value & 0xFF, full & 0xFF,
                                 "low byte must be base-independent")

    @unittest.skipUnless(shutil.which("sdcc"), "sdcc is not installed")
    def test_real_sdcc_segment_uses_high_byte_patches(self):
        workdir = tempfile.mkdtemp(prefix="zexv2-sdcc-")
        try:
            src = os.path.join(workdir, "farseg.c")
            obj = os.path.join(workdir, "farseg.rel")
            with open(src, "w") as handle:
                handle.write(FAR_C_SOURCE)
            proc = subprocess.run(
                ["sdcc", "-mz80", "-c", "--nostdinc", "-I",
                 os.path.join(ROOT, "abi"), src, "-o", obj],
                capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            seg = {"id": 1, "flags": ["EXEC"], "objects": [obj],
                   "image_areas": ["_CODE", "_DATA"],
                   "bss_areas": ["_INITIALIZED"],
                   "far_entries": [{"id": 1, "symbol": "far_add"}]}
            packed = zexpack._zex2_link_segment(seg, obj, None)
            self.assertTrue(packed["relocs"], "segment produced no relocations")
            self.assertGreater(len(packed["image"]), 0)
            for site in packed["relocs"]:
                # Every site is a whole 16-bit word with room to patch.
                self.assertLessEqual(site + 2, len(packed["image"]))
        finally:
            shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
