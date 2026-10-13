#!/usr/bin/env python3
"""Host-side EXE v2 tooling tests for the subsystem acceptance image.

Coverage:

* constant parity: tools/exepack.py format constants, segment constants
  and result codes are value-identical with formats/include/exe.h;
* one malformed fixture per validation boundary rejects with the exact
  boundary code, and the fixture set matches the subsystem ROMFS
  acceptance manifest (names, media types, control files);
* packing a synthetic linked program honors the manifest, rejects
  content outside the declared image, entry/BSS/scratch/geometry drift,
  and produces byte- and report-identical output on repeat runs;
* DATA-segment packing: the table rows, payload offsets, slot total and
  a container over 64 KiB; the manifest refusals (unknown kinds, id order,
  empty/oversized payloads, too many slots);
* FAR_CODE packing: the payload comes from the program's linked `_FAR_CODE`
  area (one or several segment declarations), the area must be linked at
  8000h and fit the 16 KiB aperture, and the 36.17 rule keeps the primary
  image, BSS and stack outside 8000h-BFFFh;
* the real exetest/outcome artifacts: the packed containers validate,
  their reports match the bytes, the subsystem acceptance ROMFS stores
  identical payloads, and the generated layout header matches the
  containers;
* mapcheck's process/EXE placement rules (common gates/state, bank-7
  process code, staging/stack sizes).

Skipped when the packed modules / images have not been built.
"""

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests", "subsys"))

import exepack  # noqa: E402
import gen_exe_fixtures as gen_fixtures  # noqa: E402
import mapcheck  # noqa: E402
import romfspack  # noqa: E402
import systemgen  # noqa: E402
from maptools import evaluate, make_map, parse_c_header  # noqa: E402

EXE_H = os.path.join(ROOT, "formats", "include", "exe.h")
EXETEST_JSON = os.path.join(ROOT, "build", "user", "exetest",
                             "exetest.json")
SUBSYS_EXETEST_JSON = os.path.join(ROOT, "build-subsys-selftest", "user",
                                    "exetest", "exetest.json")
SUBSYS_SYSTEM = os.path.join(ROOT, "systems", "subsystem.json")
EXEPACK = os.path.join(ROOT, "tools", "exepack.py")


class TestExeConstantParity(unittest.TestCase):
    def setUp(self):
        self.env = parse_c_header(EXE_H)

    def test_result_codes_match(self):
        for name, value in exepack.RESULT_CODES:
            self.assertIn(name, self.env,
                          "%s missing from exe.h" % name)
            self.assertEqual(evaluate(self.env[name], self.env), value,
                             "%s mismatch between exe.h and exepack.py"
                             % name)

    def test_format_constants_match(self):
        expected = {
            "EXE_FORMAT_VERSION": exepack.FORMAT_VERSION,
            "EXE_ABI_VERSION": exepack.ABI_VERSION,
            "EXE_FLAG_NONE": exepack.FLAGS_NONE,
            "EXE_HEADER_SIZE": exepack.HEADER_SIZE,
            "EXE_SEGMENT_ENTRY_SIZE": exepack.SEGMENT_ENTRY_SIZE,
            "EXE_LOAD_BASE": exepack.LOAD_BASE,
            "EXE_STACK_TOP": exepack.STACK_TOP,
            "EXE_MIN_STACK_SIZE": exepack.MIN_STACK_SIZE,
            "EXE_MAX_SEGMENTS": exepack.MAX_SEGMENTS,
            "EXE_MAX_TOTAL_SLOTS": exepack.MAX_TOTAL_SLOTS,
            "EXE_SEG_SLOT_SIZE": exepack.SEG_SLOT_SIZE,
            "EXE_SEG_MAX_SIZE": exepack.SEG_MAX_SIZE,
            "EXE_SEG_MAX_FAR_SIZE": exepack.SEG_MAX_FAR_SIZE,
            "EXE_SEG_KIND_FAR_CODE": exepack.KIND_FAR_CODE,
            "EXE_SEG_KIND_DATA": exepack.KIND_DATA,
        }
        for name, value in expected.items():
            self.assertEqual(evaluate(self.env[name], self.env), value,
                             "%s mismatch between exe.h and exepack.py"
                             % name)
        # Magic characters spelled out as C char literals.
        text = open(EXE_H).read()
        for index, char in enumerate("KEX2"):
            self.assertIn("#define EXE_MAGIC_%d '%s'" % (index, char), text)


class TestFixtureBoundaries(unittest.TestCase):
    def test_every_fixture_matches_its_declared_boundary(self):
        for fixture in gen_fixtures.build_fixtures():
            data = fixture["data"]
            if fixture["tool_code"] is None:
                exepack.validate_image(data)  # must be well-formed
            else:
                with self.assertRaises(exepack.ExeError) as caught:
                    exepack.validate_image(data)
                self.assertEqual(caught.exception.code, fixture["tool_code"],
                                 "%s rejected as %s, expected %s"
                                 % (fixture["name"], caught.exception.code,
                                    fixture["tool_code"]))

    def test_fixture_generation_is_deterministic(self):
        first = gen_fixtures.build_fixtures()
        second = gen_fixtures.build_fixtures()
        self.assertEqual([f["data"] for f in first],
                         [f["data"] for f in second])

    def test_old_magic_is_rejected(self):
        """The pre-rename magic cannot reappear (the bytes are spelled in
        hex): a container carrying it hits the ordinary bad-magic
        boundary."""
        with self.assertRaises(exepack.ExeError) as caught:
            exepack.validate_image(gen_fixtures.old_magic_container())
        self.assertEqual(caught.exception.code, "EXE_EXEC_ERR_MAGIC")

    def test_fixture_set_matches_the_system_file(self):
        system = json.loads(open(SUBSYS_SYSTEM).read())
        store = system["store"]
        for fixture in gen_fixtures.build_fixtures():
            self.assertIn(fixture["name"], store,
                          "%s missing from systems/subsystem.json"
                          % fixture["name"])
        # Media types follow from the names (tools/systemgen.py's
        # store_type): the malformed/return EXE files are EXE, the
        # wrong-type control is packed as DATA.
        self.assertEqual(systemgen.store_type("exe-wrong-type.dat"), "DATA")
        for name in ("exetest.exe", "exe-return.exe", "exe-bad-magic.exe",
                     "exe-seg-kind.exe", "exe2probe.exe", "exe2seg.exe",
                     "exe2big.exe"):
            self.assertEqual(systemgen.store_type(name), "EXE", name)
            self.assertIn(name, store, name)
        self.assertIn("sio", system["drivers"])
        # The corrupt-entry rollback is no longer a separate failure
        # image: the subsystem store carries the truncated fixture
        # next to the valid exetest.exe, so the process selftest rolls
        # the bad load back and still runs the valid copy in one boot.
        self.assertEqual(systemgen.store_type("exe-truncated.exe"), "EXE")
        self.assertIn("exe-truncated.exe", store)


def ihx_record(addr, data):
    length = len(data)
    body = bytes([length, (addr >> 8) & 0xFF, addr & 0xFF, 0x00]) + bytes(data)
    checksum = (-sum(body)) & 0xFF
    return ":%s%02X" % (body.hex().upper(), checksum)


class SyntheticProgram:
    """A controlled linked program plus manifest, packed on demand."""

    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix="kosexe-")
        self.image_size = 0x20
        self.bss_size = 0x20
        self.stack_size = 0x10
        self.mem = {0x100: 0xC9, 0x101: 0x00, 0x102: 0xAA}
        self.symbols = {
            "zt_entry": 0x0100,
            "s__DATA": 0x0120,
            "l__DATA": 0x08,
            "l__INITIALIZER": 0,
            "l__INITIALIZED": 0,
            "kos_runtime_scratch": 0x0120,
        }

    def write_manifest(self, mem=None, symbols=None, segments=None,
                       **manifest_overrides):
        mem = dict(self.mem if mem is None else mem)
        symbols = dict(self.symbols if symbols is None else symbols)
        ihx_lines = []
        for addr, value in sorted(mem.items()):
            values = value if isinstance(value, (list, tuple)) else [value]
            for byte in values:
                ihx_lines.append(ihx_record(addr, [byte]))
        ihx_lines.append(":00000001FF")
        with open(os.path.join(self.tmp, "prog.ihx"), "w") as fh:
            fh.write("\n".join(ihx_lines) + "\n")
        with open(os.path.join(self.tmp, "prog.map"), "w") as fh:
            for name, addr in sorted(symbols.items()):
                fh.write("     %08X  %-32s fixture\n" % (addr, name))
        manifest = {
            "format": "exe-v2",
            "name": "prog.exe",
            "input": "prog.ihx",
            "map": "prog.map",
            "entry": "zt_entry",
            "entry_offset": 0,
            "image_size": self.image_size,
            "bss_size": self.bss_size,
            "stack_size": self.stack_size,
            "scratch": {"symbol": "kos_runtime_scratch", "size": 8},
        }
        if segments is not None:
            manifest["segments"] = segments
        manifest.update(manifest_overrides)
        manifest_path = os.path.join(self.tmp, "prog.json")
        with open(manifest_path, "w") as fh:
            json.dump(manifest, fh)
        return manifest_path

    def write_payload(self, name, data):
        path = os.path.join(self.tmp, name)
        with open(path, "wb") as fh:
            fh.write(data)
        return path

    def pack(self, **kw):
        manifest_path = self.write_manifest(**kw)
        out_path = os.path.join(self.tmp, "prog.exe")
        result = exepack.pack(manifest_path, out_path)
        return out_path, result


class TestSyntheticPacking(unittest.TestCase):
    """Packing behavior on controlled linked inputs."""

    def setUp(self):
        self.program = SyntheticProgram()

    def test_valid_pack_produces_the_header_and_image(self):
        out_path, result = self.program.pack()
        data = open(out_path, "rb").read()
        fields, geo = exepack.validate_image(data)
        self.assertEqual(fields["image_size"], self.program.image_size)
        self.assertEqual(fields["bss_size"], self.program.bss_size)
        self.assertEqual(fields["stack_size"], self.program.stack_size)
        self.assertEqual(fields["segment_count"], 0)
        self.assertEqual(fields["total_segment_slots"], 0)
        self.assertEqual(fields["primary_payload_offset"],
                         exepack.HEADER_SIZE)
        self.assertEqual(fields["segment_table_offset"],
                         exepack.HEADER_SIZE + self.program.image_size)
        self.assertEqual(geo["entry"], 0x0100)
        self.assertEqual(data[36:39], bytes((0xC9, 0x00, 0xAA)))
        self.assertEqual(len(data), 36 + self.program.image_size)
        self.assertEqual(result["sha256"], hashlib.sha256(data).hexdigest())

    def test_pack_is_byte_and_report_deterministic(self):
        out_a, _ = self.program.pack()
        out_b, _ = self.program.pack()
        self.assertEqual(open(out_a, "rb").read(), open(out_b, "rb").read())
        report_a = os.path.join(self.program.tmp, "a.report.json")
        report_b = os.path.join(self.program.tmp, "b.report.json")
        manifest = self.program.write_manifest()
        exepack.pack(manifest, out_a, report_path=report_a)
        exepack.pack(manifest, out_b, report_path=report_b)
        self.assertEqual(open(report_a, "rb").read(),
                         open(report_b, "rb").read())

    def test_content_outside_declared_image_rejected(self):
        mem = dict(self.program.mem)
        mem[0x100 + self.program.image_size] = 0x55
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(mem=mem)
        self.assertIn("outside the declared image", str(caught.exception))

    def test_entry_mismatch_rejected(self):
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(entry_offset=1)
        self.assertIn("declared offset", str(caught.exception))

    def test_bss_base_mismatch_rejected(self):
        symbols = dict(self.program.symbols)
        symbols["s__DATA"] = 0x0130
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(symbols=symbols)
        self.assertIn("BSS base", str(caught.exception))

    def test_bss_overflow_rejected(self):
        symbols = dict(self.program.symbols)
        symbols["l__DATA"] = self.program.bss_size + 1
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(symbols=symbols)
        self.assertIn("bss_size", str(caught.exception))

    def test_initialized_data_rejected(self):
        symbols = dict(self.program.symbols)
        symbols["l__INITIALIZER"] = 4
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(symbols=symbols)
        self.assertIn("initialized writable data", str(caught.exception))

    def test_scratch_outside_window_rejected(self):
        symbols = dict(self.program.symbols)
        symbols["kos_runtime_scratch"] = 0x2000
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(symbols=symbols)
        self.assertIn("argument window", str(caught.exception))

    def test_declared_geometry_rejected(self):
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(stack_size=1)
        self.assertEqual(caught.exception.code, "EXE_EXEC_ERR_STACK")

    def test_unknown_and_missing_manifest_keys_rejected(self):
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(mystery=1)
        self.assertIn("unknown manifest keys", str(caught.exception))
        manifest_path = self.program.write_manifest()
        manifest = json.loads(open(manifest_path).read())
        del manifest["bss_size"]
        with open(manifest_path, "w") as fh:
            json.dump(manifest, fh)
        with self.assertRaises(exepack.ExeError) as caught:
            exepack.pack(manifest_path,
                         os.path.join(self.program.tmp, "out.exe"))
        self.assertIn("missing 'bss_size'", str(caught.exception))


class TestSegmentPacking(unittest.TestCase):
    """The EXE v2 DATA-segment path (package 44a)."""

    def setUp(self):
        self.program = SyntheticProgram()
        self.payload1 = bytes(range(64))
        self.payload2 = bytes([0xA5] * 96)

    def _segments(self, **overrides):
        first = {"id": 1, "kind": "DATA", "input": "seg1.bin",
                 "bss_size": 4}
        second = {"id": 2, "kind": "DATA", "input": "seg2.bin",
                  "bss_size": 96}
        first.update(overrides)
        return [first, second], second

    def _pack_two(self, **overrides):
        segments, _second = self._segments(**overrides)
        self.program.write_payload("seg1.bin", self.payload1)
        self.program.write_payload("seg2.bin", self.payload2)
        return self.program.pack(segments=segments)

    def test_table_rows_offsets_and_slot_total(self):
        out_path, result = self._pack_two()
        data = open(out_path, "rb").read()
        fields, _geo = exepack.validate_image(data)
        self.assertEqual(fields["segment_count"], 2)
        self.assertEqual(fields["total_segment_slots"], 2)
        rows = [
            exepack.parse_segment_row(
                data, fields["segment_table_offset"] +
                i * exepack.SEGMENT_ENTRY_SIZE)
            for i in range(2)
        ]
        self.assertEqual([row["id"] for row in rows], [1, 2])
        self.assertEqual([row["kind"] for row in rows],
                         [exepack.KIND_DATA, exepack.KIND_DATA])
        self.assertEqual([row["image_size"] for row in rows],
                         [len(self.payload1), len(self.payload2)])
        self.assertEqual([row["bss_size"] for row in rows], [4, 96])
        # Payloads lie inside the file, 4-byte aligned, and match the
        # input files byte for byte.
        self.assertEqual(
            data[rows[0]["payload_offset"]:
                 rows[0]["payload_offset"] + len(self.payload1)],
            self.payload1)
        self.assertEqual(
            data[rows[1]["payload_offset"]:
                 rows[1]["payload_offset"] + len(self.payload2)],
            self.payload2)
        self.assertEqual(rows[0]["payload_offset"] % 4, 0)
        self.assertEqual(len(data), fields["file_size"])
        self.assertEqual(result["segments"][0]["sha256"],
                         hashlib.sha256(self.payload1).hexdigest())

    def test_container_over_64_kib(self):
        big = bytes(((i * 7 + 0x13) & 0xFF) for i in range(32768 - 8))
        segments = [
            {"id": 1, "kind": "DATA", "input": "big1.bin", "bss_size": 8},
            {"id": 2, "kind": "DATA", "input": "big2.bin", "bss_size": 8},
        ]
        self.program.write_payload("big1.bin", big)
        self.program.write_payload("big2.bin", big)
        out_path, result = self.program.pack(segments=segments)
        data = open(out_path, "rb").read()
        self.assertGreater(len(data), 0x10000)
        fields, _geo = exepack.validate_image(data)
        self.assertEqual(fields["file_size"], len(data))
        self.assertEqual(fields["total_segment_slots"], 8)
        # The second payload starts before 64 KiB and ends after it.
        row2 = exepack.parse_segment_row(
            data, fields["segment_table_offset"] +
            exepack.SEGMENT_ENTRY_SIZE)
        self.assertLess(row2["payload_offset"], 0x10000)
        self.assertGreater(row2["payload_offset"] + row2["image_size"],
                           0x10000)
        self.assertEqual(result["size"], len(data))

    def test_far_code_without_an_area_refused(self):
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(segments=[
                {"id": 1, "kind": "FAR_CODE", "input": "seg1.bin",
                 "bss_size": 0}])
        self.assertIn("'area'", str(caught.exception))
        self.assertEqual(caught.exception.code, "EXE_EXEC_ERR_ARG")

    def test_data_with_an_area_refused(self):
        self.program.write_payload("seg1.bin", self.payload1)
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(segments=[
                {"id": 1, "kind": "DATA", "input": "seg1.bin",
                 "area": "_FAR_CODE", "bss_size": 0}])
        self.assertIn("needs the 'bss_area'", str(caught.exception))
        self.assertEqual(caught.exception.code, "EXE_EXEC_ERR_ARG")

    def test_unknown_kind_refused(self):
        self.program.write_payload("seg1.bin", self.payload1)
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(segments=[
                {"id": 1, "kind": "VECTOR", "input": "seg1.bin",
                 "bss_size": 0}])
        self.assertEqual(caught.exception.code, "EXE_EXEC_ERR_SEG_KIND")

    def test_ids_must_be_one_through_n(self):
        self.program.write_payload("seg1.bin", self.payload1)
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(segments=[
                {"id": 2, "kind": "DATA", "input": "seg1.bin",
                 "bss_size": 0}])
        self.assertEqual(caught.exception.code, "EXE_EXEC_ERR_SEGMENT")

    def test_empty_payload_refused(self):
        self.program.write_payload("seg1.bin", b"")
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(segments=[
                {"id": 1, "kind": "DATA", "input": "seg1.bin",
                 "bss_size": 0}])
        self.assertEqual(caught.exception.code, "EXE_EXEC_ERR_SEGMENT")

    def test_missing_payload_refused(self):
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(segments=[
                {"id": 1, "kind": "DATA", "input": "absent.bin",
                 "bss_size": 0}])
        self.assertEqual(caught.exception.code, "EXE_EXEC_ERR_ARG")

    def test_image_plus_bss_limit_enforced(self):
        self.program.write_payload("seg1.bin", b"\x00" * 64)
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(segments=[
                {"id": 1, "kind": "DATA", "input": "seg1.bin",
                 "bss_size": exepack.SEG_MAX_SIZE - 63}])
        self.assertEqual(caught.exception.code, "EXE_EXEC_ERR_SEGMENT")

    def test_slot_total_limit_enforced(self):
        # Six 32-KiB segments need 24 slots, over the 20-slot ceiling.
        segments = []
        for index in range(6):
            name = "fill%d.bin" % index
            self.program.write_payload(name, b"\x00" * (32768 - 8))
            segments.append({"id": index + 1, "kind": "DATA",
                             "input": name, "bss_size": 8})
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(segments=segments)
        self.assertEqual(caught.exception.code, "EXE_EXEC_ERR_SEG_SLOTS")

    def test_segment_count_limit_enforced(self):
        segments = []
        for index in range(exepack.MAX_SEGMENTS + 1):
            name = "tiny%d.bin" % index
            self.program.write_payload(name, b"\x01")
            segments.append({"id": index + 1, "kind": "DATA",
                             "input": name, "bss_size": 0})
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(segments=segments)
        self.assertEqual(caught.exception.code, "EXE_EXEC_ERR_SEG_SLOTS")

    def test_round_trip_through_validate_and_dump(self):
        out_path, _result = self._pack_two()
        data = open(out_path, "rb").read()
        exepack.validate_image(data)
        text = exepack.dump(out_path)
        self.assertIn("segments 2/2 slots", text)
        self.assertIn("segment 1  kind DATA  flags 0  image 64  bss 4",
                      text)
        self.assertIn("segment 2  kind DATA  flags 0  image 96  bss 96",
                      text)


class TestFarCodePacking(unittest.TestCase):
    """The EXE v2 FAR_CODE path (packages 44b/49b): the SDK links one
    `_FAR<n>` area per `FAR<n>=` option, all at 8000h, the packer takes
    each segment's bytes out of its own IHX record stream and enforces the
    36.17 aperture link rule."""

    def setUp(self):
        self.program = SyntheticProgram()
        self.far = bytes([0x3E, 0x5A, 0xC9])  # LD A,5Ah / RET
        self.far2 = bytes([0x3E, 0x3C])       # LD A,3Ch

    def _declaration(self, **overrides):
        far = {"id": 1, "kind": "FAR_CODE", "area": "_FAR1",
               "bss_size": 0}
        far.update(overrides)
        return [far]

    def _inputs(self, far_bytes=None, far_base=exepack.FAR_BASE,
                area="_FAR1"):
        far_bytes = self.far if far_bytes is None else far_bytes
        mem = dict(self.program.mem)
        for index, byte in enumerate(far_bytes):
            mem[far_base + index] = byte
        symbols = dict(self.program.symbols)
        symbols["s_" + area] = far_base
        symbols["l_" + area] = len(far_bytes)
        return mem, symbols

    def test_far_segment_packs_from_the_linked_area(self):
        mem, symbols = self._inputs()
        out_path, result = self.program.pack(mem=mem, symbols=symbols,
                                             segments=self._declaration())
        data = open(out_path, "rb").read()
        fields, _geo = exepack.validate_image(data)
        self.assertEqual(fields["segment_count"], 1)
        row = exepack.parse_segment_row(data,
                                        fields["segment_table_offset"])
        self.assertEqual(row["kind"], exepack.KIND_FAR_CODE)
        self.assertEqual(row["image_size"], len(self.far))
        self.assertEqual(row["bss_size"], 0)
        self.assertEqual(
            data[row["payload_offset"]:
                 row["payload_offset"] + len(self.far)], self.far)
        self.assertEqual(result["segments"][0]["kind"], "FAR_CODE")
        self.assertEqual(result["segments"][0]["sha256"],
                         hashlib.sha256(self.far).hexdigest())

    def _two_areas(self):
        """Two far areas at 8000h, one record stream each: the n-th value
        of an address belongs to the n-th area in link order."""
        mem, symbols = self._inputs()
        symbols["s__FAR2"] = exepack.FAR_BASE
        symbols["l__FAR2"] = len(self.far2)
        for index, byte in enumerate(self.far2):
            addr = exepack.FAR_BASE + index
            mem[addr] = [mem[addr], byte]
        segments = self._declaration() + self._declaration(
            id=2, area="_FAR2")
        return mem, symbols, segments

    def test_several_far_areas_are_split_by_stream(self):
        """Each FAR_CODE row's payload is its own area's bytes: 8000h holds
        one byte per area at that address, in ascending id."""
        mem, symbols, segments = self._two_areas()
        out_path, _result = self.program.pack(mem=mem, symbols=symbols,
                                              segments=segments)
        data = open(out_path, "rb").read()
        fields, _geo = exepack.validate_image(data)
        self.assertEqual(fields["segment_count"], 2)
        self.assertEqual(fields["total_segment_slots"], 2)
        rows = [exepack.parse_segment_row(
            data, fields["segment_table_offset"] + i * 16) for i in range(2)]
        self.assertEqual([row["kind"] for row in rows],
                         [exepack.KIND_FAR_CODE, exepack.KIND_FAR_CODE])
        for row, payload in zip(rows, (self.far, self.far2)):
            self.assertEqual(
                data[row["payload_offset"]:
                     row["payload_offset"] + len(payload)], payload)

    def test_a_far_area_must_match_its_id(self):
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(segments=self._declaration(area="_FAR2"))
        self.assertEqual(caught.exception.code, "EXE_EXEC_ERR_ARG")
        self.assertIn("_FAR1", str(caught.exception))

    def test_far_ids_are_the_first_segments_in_order(self):
        """The far code segments are ids 1..k in declaration order (a
        FAR2= without FAR1= is segment 2 of a container that has no
        segment 1)."""
        mem, symbols = self._inputs()
        symbols["s__FAR2"] = exepack.FAR_BASE
        symbols["l__FAR2"] = len(self.far2)
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(mem=mem, symbols=symbols,
                              segments=self._declaration(id=2, area="_FAR2"))
        self.assertEqual(caught.exception.code, "EXE_EXEC_ERR_ARG")
        self.assertIn("declared first", str(caught.exception))

    def test_a_far_byte_stream_must_match_the_area_count(self):
        """A hole in one area's stream would make the n-th byte of an
        address ambiguous, so it is refused."""
        mem, symbols, segments = self._two_areas()
        # Drop one record of the second stream: the address now carries one
        # value where two areas cover it.
        mem[exepack.FAR_BASE + 1] = self.far[1]
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(mem=mem, symbols=symbols, segments=segments)
        self.assertEqual(caught.exception.code, "EXE_EXEC_ERR_ARG")
        self.assertIn("record stream", str(caught.exception))

    def test_far_area_must_be_linked_at_8000h(self):
        mem, symbols = self._inputs(far_base=0x8100)
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(mem=mem, symbols=symbols,
                              segments=self._declaration())
        self.assertIn("linked at", str(caught.exception))
        self.assertEqual(caught.exception.code, "EXE_EXEC_ERR_ARG")

    def test_far_area_over_the_aperture_refused(self):
        size = exepack.SEG_MAX_FAR_SIZE + 1
        mem, symbols = self._inputs(far_bytes=bytes(size))
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(mem=mem, symbols=symbols,
                              segments=self._declaration())
        self.assertEqual(caught.exception.code, "EXE_EXEC_ERR_SEGMENT")

    def test_far_image_plus_bss_limit_enforced(self):
        mem, symbols = self._inputs(
            far_bytes=bytes(exepack.SEG_MAX_FAR_SIZE))
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(mem=mem, symbols=symbols,
                              segments=self._declaration(bss_size=1))
        self.assertEqual(caught.exception.code, "EXE_EXEC_ERR_SEGMENT")

    def test_far_bytes_outside_the_declared_area_refused(self):
        mem, symbols = self._inputs()
        mem[exepack.FAR_BASE + 0x40] = 0x77
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(mem=mem, symbols=symbols,
                              segments=self._declaration())
        self.assertIn("outside the declared image", str(caught.exception))

    def test_linked_bytes_above_the_aperture_need_a_far_segment(self):
        mem = dict(self.program.mem)
        mem[exepack.FAR_BASE] = 0x77
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(mem=mem, symbols=self.program.symbols)
        self.assertIn("outside the declared image", str(caught.exception))

    def test_aperture_rule_needs_the_image_under_8000h(self):
        mem, symbols = self._inputs()
        symbols["s__DATA"] = exepack.LOAD_BASE + exepack.FAR_IMAGE_END
        symbols["l__DATA"] = 0
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(mem=mem, symbols=symbols,
                              segments=self._declaration(),
                              image_size=exepack.FAR_IMAGE_END, bss_size=0)
        self.assertIn("outside", str(caught.exception))
        self.assertEqual(caught.exception.code, "EXE_EXEC_ERR_ARG")

    def test_aperture_rule_needs_the_stack_above_bfffh(self):
        mem, symbols = self._inputs()
        with self.assertRaises(exepack.ExeError) as caught:
            self.program.pack(mem=mem, symbols=symbols,
                              segments=self._declaration(),
                              stack_size=exepack.STACK_TOP -
                              exepack.FAR_STACK_MIN + 1)
        self.assertIn("outside", str(caught.exception))
        self.assertEqual(caught.exception.code, "EXE_EXEC_ERR_ARG")


class TestExepackCLI(unittest.TestCase):
    def _run(self, *args):
        return subprocess.run([sys.executable, EXEPACK] + list(args),
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True)

    def test_validate_rejects_each_boundary_with_its_name(self):
        with tempfile.TemporaryDirectory(prefix="kosexe-") as tmp:
            for fixture in gen_fixtures.build_fixtures():
                if fixture["tool_code"] is None:
                    continue
                path = os.path.join(tmp, fixture["name"])
                with open(path, "wb") as fh:
                    fh.write(fixture["data"])
                result = self._run("validate", path)
                self.assertEqual(result.returncode, 1,
                                 "%s accepted by the CLI" % fixture["name"])
                self.assertIn(fixture["tool_code"], result.stderr,
                              "%s: boundary name missing from %r"
                              % (fixture["name"], result.stderr))

    def test_validate_accepts_the_control_files(self):
        with tempfile.TemporaryDirectory(prefix="kosexe-") as tmp:
            for fixture in gen_fixtures.build_fixtures():
                if fixture["tool_code"] is not None:
                    continue
                path = os.path.join(tmp, fixture["name"])
                with open(path, "wb") as fh:
                    fh.write(fixture["data"])
                result = self._run("validate", path)
                self.assertEqual(result.returncode, 0, result.stderr)
                dump = self._run("dump", path)
                self.assertEqual(dump.returncode, 0, dump.stderr)
                self.assertIn("entry", dump.stdout)

    def test_get_reads_layout_fields(self):
        if not os.path.exists(EXETEST_JSON):
            self.skipTest("the generated exetest manifest has not been built")
        result = self._run("get", EXETEST_JSON, "image_size")
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads(open(EXETEST_JSON).read())
        self.assertEqual(int(result.stdout.strip()), manifest["image_size"])


class TestProcessMapcheckRules(unittest.TestCase):
    def test_synthetic_map_with_process_symbols_passes(self):
        mapcheck.check_map(make_map())

    def test_process_launch_must_be_common(self):
        text = make_map(symbols={"_kos_process_launch": 0x3000})
        with self.assertRaisesRegex(mapcheck.MapError, "common region"):
            mapcheck.check_map(text)

    def test_process_code_must_be_bank7(self):
        text = make_map(symbols={"_process_exec": 0xE100})
        with self.assertRaisesRegex(mapcheck.MapError, "bank-7"):
            mapcheck.check_map(text)

    def test_process_table_must_be_bank7(self):
        text = make_map(symbols={"_process_table": 0xE100})
        with self.assertRaisesRegex(mapcheck.MapError, "bank-7"):
            mapcheck.check_map(text)

    def test_copy_staging_must_be_common(self):
        text = make_map(symbols={"_kos_user_copy_staging": 0xC100})
        with self.assertRaisesRegex(mapcheck.MapError, "common region"):
            mapcheck.check_map(text)

    def test_copy_staging_size_mismatch(self):
        text = make_map(symbols={"kos_copy_op": 0xE3C0})
        with self.assertRaisesRegex(mapcheck.MapError, "staging"):
            mapcheck.check_map(text)

    def test_copy_stack_size_mismatch(self):
        text = make_map(symbols={"kos_copy_stack_top": 0xE3D0})
        with self.assertRaisesRegex(mapcheck.MapError, "copy stack"):
            mapcheck.check_map(text)

    def test_missing_process_symbol_rejected(self):
        text = make_map(symbols={"_kos_bank_copy": None})
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "map has no symbol _kos_bank_copy"):
            mapcheck.check_map(text)


class TestSubsystemArtifacts(unittest.TestCase):
    def _read(self, rel):
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            self.skipTest("%s missing; build it first" % rel)
        with open(path, "rb") as fh:
            return path, fh.read()

    def test_packed_exetest_matches_report_and_manifest(self):
        _p, data = self._read("build/modules/exetest.exe")
        _p, report_text = self._read(
            "build/user/exetest/exetest.report.json")
        report = json.loads(report_text)
        fields, geo = exepack.validate_image(data)
        manifest = json.loads(open(EXETEST_JSON).read())
        self.assertEqual(fields["image_size"], manifest["image_size"])
        self.assertEqual(fields["bss_size"], manifest["bss_size"])
        self.assertEqual(fields["stack_size"], manifest["stack_size"])
        self.assertEqual(geo["entry"],
                         exepack.LOAD_BASE + manifest["entry_offset"])
        self.assertEqual(report["sha256"],
                         hashlib.sha256(data).hexdigest())
        self.assertEqual(report["format"], "exe-v2")

    def test_outcome_programs_pack_and_carry_their_segments(self):
        _p, probe = self._read("build/user/exe2probe.exe")
        fields, _geo = exepack.validate_image(probe)
        self.assertEqual(fields["segment_count"], 0)

        name, seg = self._read("build/user/exe2seg.exe")
        self.assertIn("exe2seg.exe", name)
        fields, _geo = exepack.validate_image(seg)
        self.assertEqual(fields["segment_count"], 1)
        self.assertEqual(fields["total_segment_slots"], 1)
        row = exepack.parse_segment_row(seg, fields["segment_table_offset"])
        self.assertEqual((row["kind"], row["image_size"], row["bss_size"]),
                         (exepack.KIND_DATA, 1024, 64))

        _p, big = self._read("build/user/exe2big.exe")
        fields, _geo = exepack.validate_image(big)
        self.assertEqual(fields["segment_count"], 2)
        self.assertEqual(fields["total_segment_slots"], 8)
        self.assertGreater(len(big), 0x10000)

    def test_generated_expect_header_matches_the_containers(self):
        _p, text = self._read("build-subsys-selftest/process_expect.inc")
        lines = text.decode("ascii")
        _p, probe = self._read("build-subsys-selftest/user/exe2probe.exe")
        fields, _geo = exepack.validate_image(probe)
        self.assertIn("#define EXE2_PROBE_IMAGE_SIZE 0x%04Xu"
                      % fields["image_size"], lines)
        self.assertIn("#define EXE2_PROBE_BSS_START  0x%04Xu"
                      % (exepack.LOAD_BASE + fields["image_size"]), lines)
        _p, seg = self._read("build-subsys-selftest/user/exe2seg.exe")
        fields, _geo = exepack.validate_image(seg)
        self.assertIn("#define EXE2_SEG_SEG1_IMAGE 0x%04Xu" % 1024, lines)
        self.assertIn("#define EXE2_SEG_SEG1_BSS   0x%04Xu" % 64, lines)

    def test_acceptance_romfs_carries_the_same_exetest_bytes(self):
        _p, stream = self._read("build-subsys-selftest/recovery.romfs")
        _h, entries = romfspack.parse_image(stream)
        names = {entry["name"]: entry for entry in entries}
        _p, data = self._read("build/modules/exetest.exe")
        self.assertEqual(names["exetest.exe"]["payload"], data)
        self.assertEqual(names["exetest.exe"]["flags"], romfspack.FLAG_EXE)
        # Every malformed fixture and outcome program is present with its
        # generated bytes.
        outdir = os.path.join(ROOT, "build-subsys-selftest", "modules")
        for fixture in gen_fixtures.build_fixtures():
            path = os.path.join(outdir, fixture["name"])
            if not os.path.exists(path):
                continue
            self.assertEqual(names[fixture["name"]]["payload"],
                             open(path, "rb").read())
        for name in ("exe2probe.exe", "exe2seg.exe", "exe2big.exe"):
            path = os.path.join(ROOT, "build-subsys-selftest", "user", name)
            self.assertEqual(names[name]["payload"],
                             open(path, "rb").read())

    def test_recovery_store_carries_the_corrupt_entry_fixture(self):
        # The retired m6-failure image swapped in a truncated EXE as the
        # only exetest.exe.  The subsystem image instead packs the
        # truncated fixture beside the valid copy, so one boot proves
        # the corrupt-entry rollback and the valid re-run.
        _p, stream = self._read("build-subsys-selftest/recovery.romfs")
        _h, entries = romfspack.parse_image(stream)
        names = {entry["name"]: entry for entry in entries}
        _p, valid = self._read("build/modules/exetest.exe")
        _q, truncated = self._read(
            "build-subsys-selftest/modules/exe-truncated.exe")
        self.assertEqual(names["exetest.exe"]["payload"], valid)
        self.assertEqual(names["exe-truncated.exe"]["payload"], truncated)

    def test_images_contain_the_acceptance_markers(self):
        _p, image = self._read("build-subsys-selftest/"
                               "konaos-subsys-selftest.bin")
        for marker in (b"syscall selftest: OK\r\n",
                       b"irq selftest: OK\r\n",
                       b"romfs selftest: OK\r\n",
                       b"process selftest: OK\r\n",
                       b"exetest: OK\r\n",
                       b"process selftest: two-bank OK\r\n",
                       b"process selftest: exe2 BSS/bank OK\r\n",
                       b"process selftest: exe2 segment OK\r\n",
                       b"process selftest: exe2 big container OK\r\n",
                       b"process selftest: exe2 rollback OK\r\n"):
            self.assertIn(marker, image)
        # The corrupt-entry rollback is now in the same image (the
        # current equivalent of the retired m6-failure image's marker).
        self.assertIn(b"process selftest: corrupt entry rolled back\r\n",
                      image)

    def test_serial_smoke_expectations_exist_verbatim(self):
        import serial_smoke
        _p, image = self._read("build-subsys-selftest/"
                               "konaos-subsys-selftest.bin")
        for const in (b"KonaOS 0.0\r\n", b"platform: Zephyr-80\r\n",
                      b"kernel entry: OK\r\n", b"serial: OK\r\n",
                      serial_smoke.CONSOLE_DRIVER_OK,
                      serial_smoke.SUBSYS_SYSCALL_OK,
                      serial_smoke.SUBSYS_IRQ_OK,
                      serial_smoke.SUBSYS_ROMFS_OK,
                      serial_smoke.SUBSYS_PROCESS_OK,
                      serial_smoke.SUBSYS_FAIL,
                      serial_smoke.SUBSYS_PROMPT,
                      serial_smoke.SUBSYS_EXIT):
            self.assertIn(const, image,
                          "serial_smoke expectation %r is not in the "
                          "subsystem image" % const)


if __name__ == "__main__":
    unittest.main()
