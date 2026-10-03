#!/usr/bin/env python3
"""Host-side M6 ZEX tooling tests.

Coverage:

* constant parity: tools/zexpack.py result codes and format constants are
  value-identical with include/zephyros/zex.h;
* one malformed fixture per validation boundary rejects with the exact
  boundary code, and the fixture set matches the ROMFS acceptance
  manifest (names, media types, control files);
* packing a synthetic linked image honors the manifest, rejects content
  outside the declared image, entry/BSS/scratch/geometry drift, and
  produces byte- and report-identical output on repeat runs;
* the real zextest artifacts: the packed .zex validates, its report
  matches the bytes, the acceptance ROMFS stores identical payloads, and
  the failure image swaps in the truncated ZEX exactly;
* mapcheck's M6 placement rules (common gates/state, bank-7 process
  code, staging/stack sizes).

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
sys.path.insert(0, os.path.join(ROOT, "tests", "m6"))

import zexpack  # noqa: E402
import gen_fixtures  # noqa: E402
import mapcheck  # noqa: E402
import romfspack  # noqa: E402
from test_m1 import evaluate, make_map, parse_c_header  # noqa: E402

ZEX_H = os.path.join(ROOT, "include", "zephyros", "zex.h")
ZEXTEST_JSON = os.path.join(ROOT, "tests", "m6", "zextest.json")
M6_MANIFEST = os.path.join(ROOT, "tests", "m6", "manifest.json")
M6_FAILURE_MANIFEST = os.path.join(ROOT, "tests", "m6", "manifest-failure.json")
ZEXPACK = os.path.join(ROOT, "tools", "zexpack.py")


class TestZexConstantParity(unittest.TestCase):
    def setUp(self):
        self.env = parse_c_header(ZEX_H)

    def test_result_codes_match(self):
        for name, value in zexpack.RESULT_CODES:
            self.assertIn(name, self.env,
                          "%s missing from zex.h" % name)
            self.assertEqual(evaluate(self.env[name], self.env), value,
                             "%s mismatch between zex.h and zexpack.py"
                             % name)

    def test_format_constants_match(self):
        expected = {
            "ZEX_FORMAT_VERSION": zexpack.FORMAT_VERSION,
            "ZEX_ABI_VERSION": zexpack.ABI_VERSION,
            "ZEX_FLAG_NONE": zexpack.FLAGS_NONE,
            "ZEX_HEADER_SIZE": zexpack.HEADER_SIZE,
            "ZEX_LOAD_BASE": zexpack.LOAD_BASE,
            "ZEX_STACK_TOP": zexpack.STACK_TOP,
            "ZEX_MIN_STACK_SIZE": zexpack.MIN_STACK_SIZE,
        }
        for name, value in expected.items():
            self.assertEqual(evaluate(self.env[name], self.env), value,
                             "%s mismatch between zex.h and zexpack.py"
                             % name)
        # Magic characters spelled out as C char literals.
        text = open(ZEX_H).read()
        for index, char in enumerate("ZEX1"):
            self.assertIn("#define ZEX_MAGIC_%d '%s'" % (index, char), text)


class TestFixtureBoundaries(unittest.TestCase):
    def test_every_fixture_matches_its_declared_boundary(self):
        for fixture in gen_fixtures.build_fixtures():
            data = fixture["data"]
            if fixture["tool_code"] is None:
                zexpack.validate_image(data)  # must be well-formed
            else:
                with self.assertRaises(zexpack.ZexError) as caught:
                    zexpack.validate_image(data)
                self.assertEqual(caught.exception.code, fixture["tool_code"],
                                 "%s rejected as %s, expected %s"
                                 % (fixture["name"], caught.exception.code,
                                    fixture["tool_code"]))

    def test_fixture_generation_is_deterministic(self):
        first = gen_fixtures.build_fixtures()
        second = gen_fixtures.build_fixtures()
        self.assertEqual([f["data"] for f in first],
                         [f["data"] for f in second])

    def test_fixture_set_matches_acceptance_manifest(self):
        manifest = json.loads(open(M6_MANIFEST).read())
        entries = {entry["name"]: entry for entry in manifest["files"]}
        for fixture in gen_fixtures.build_fixtures():
            self.assertIn(fixture["name"], entries,
                          "%s missing from tests/m6/manifest.json"
                          % fixture["name"])
        # Media types: the malformed/return ZEX files are ZEX, the
        # wrong-type control is packed as DATA.
        self.assertEqual(entries["zex-wrong-type.dat"]["type"], "DATA")
        for name in ("zextest.zex", "zex-return.zex", "zex-bad-magic.zex"):
            self.assertEqual(entries[name]["type"], "ZEX")
        self.assertIn("sio.zmod", entries)
        # The failure manifest swaps zextest.zex for the truncated
        # fixture and keeps a valid copy under zexok.zex.
        failure = json.loads(open(M6_FAILURE_MANIFEST).read())
        fentries = {entry["name"]: entry for entry in failure["files"]}
        self.assertEqual(fentries["zextest.zex"]["path"],
                         "zex-truncated.zex")
        self.assertEqual(fentries["zexok.zex"]["path"], "zextest.zex")


class TestSmokeMismatchHints(unittest.TestCase):
    """A board that still carries a previously flashed image must produce
    an actionable hint instead of a bare timeout."""

    def _hint(self, mode, transcript):
        import contextlib
        import io
        import serial_smoke
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            serial_smoke._explain_wrong_image(mode, transcript)
        return err.getvalue()

    def test_m5_corrupt_image_still_flashed(self):
        import serial_smoke
        # The exact diagnostic line captured on hardware for the
        # m5-corrupt acceptance image (docs/validation/
        # 2026-10-03-m5-romfs.md): the header reserved field rejection.
        transcript = (
            serial_smoke.BANNER +
            b"romfs: init failed, code 0x0A, latch 0x18, "
            b"header 5A 52 46 53 01 24 02 00\r\n" +
            serial_smoke.M5_CORRUPT_PANIC)
        for mode, image in (
                ("m6", "build-m6-selftest/zephyros-m6-selftest.bin"),
                ("m6-failure",
                 "build-m6-failure-selftest/zephyros-m6-failure.bin")):
            hint = self._hint(mode, transcript)
            self.assertIn("m5-corrupt", hint,
                          "%s: corrupt-image transcript not recognized: %r"
                          % (mode, hint))
            self.assertIn(image, hint,
                          "%s: hint does not name the image to flash"
                          % mode)


class TestSyntheticPacking(unittest.TestCase):
    """Packing behavior on controlled linked inputs."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="zoszex-")
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
            "zos_runtime_scratch": 0x0120,
        }

    def _write(self, mem=None, symbols=None, **manifest_overrides):
        mem = dict(self.mem if mem is None else mem)
        symbols = dict(self.symbols if symbols is None else symbols)
        ihx_lines = [ihx_record(addr, [byte])
                     for addr, byte in sorted(mem.items())]
        ihx_lines.append(":00000001FF")
        ihx_path = os.path.join(self.tmp, "prog.ihx")
        map_path = os.path.join(self.tmp, "prog.map")
        with open(ihx_path, "w") as fh:
            fh.write("\n".join(ihx_lines) + "\n")
        with open(map_path, "w") as fh:
            for name, addr in sorted(symbols.items()):
                fh.write("     %08X  %-32s fixture\n" % (addr, name))
        manifest = {
            "format": "zex-v1",
            "name": "prog.zex",
            "input": "prog.ihx",
            "map": "prog.map",
            "entry": "zt_entry",
            "entry_offset": 0,
            "image_size": self.image_size,
            "bss_size": self.bss_size,
            "stack_size": self.stack_size,
            "scratch": {"symbol": "zos_runtime_scratch", "size": 8},
        }
        manifest.update(manifest_overrides)
        manifest_path = os.path.join(self.tmp, "prog.json")
        with open(manifest_path, "w") as fh:
            json.dump(manifest, fh)
        return manifest_path

    def _pack(self, **kw):
        manifest_path = self._write(**kw)
        out_path = os.path.join(self.tmp, "prog.zex")
        result = zexpack.pack(manifest_path, out_path)
        return out_path, result

    def test_valid_pack_produces_the_header_and_image(self):
        out_path, result = self._pack()
        data = open(out_path, "rb").read()
        fields, geo = zexpack.validate_image(data)
        self.assertEqual(fields["image_size"], self.image_size)
        self.assertEqual(fields["bss_size"], self.bss_size)
        self.assertEqual(fields["stack_size"], self.stack_size)
        self.assertEqual(geo["entry"], 0x0100)
        self.assertEqual(data[20:23], bytes((0xC9, 0x00, 0xAA)))
        self.assertEqual(len(data), 20 + self.image_size)
        self.assertEqual(result["sha256"], hashlib.sha256(data).hexdigest())

    def test_pack_is_byte_and_report_deterministic(self):
        out_a, _ = self._pack()
        out_b, _ = self._pack()
        self.assertEqual(open(out_a, "rb").read(), open(out_b, "rb").read())
        report_a = os.path.join(self.tmp, "a.report.json")
        report_b = os.path.join(self.tmp, "b.report.json")
        manifest = self._write()
        zexpack.pack(manifest, out_a, report_path=report_a)
        zexpack.pack(manifest, out_b, report_path=report_b)
        self.assertEqual(open(report_a, "rb").read(),
                         open(report_b, "rb").read())

    def test_content_outside_declared_image_rejected(self):
        mem = dict(self.mem)
        mem[0x100 + self.image_size] = 0x55
        with self.assertRaises(zexpack.ZexError) as caught:
            self._pack(mem=mem)
        self.assertIn("outside the declared image", str(caught.exception))

    def test_entry_mismatch_rejected(self):
        with self.assertRaises(zexpack.ZexError) as caught:
            self._pack(entry_offset=1)
        self.assertIn("declared offset", str(caught.exception))

    def test_bss_base_mismatch_rejected(self):
        symbols = dict(self.symbols)
        symbols["s__DATA"] = 0x0130
        with self.assertRaises(zexpack.ZexError) as caught:
            self._pack(symbols=symbols)
        self.assertIn("BSS base", str(caught.exception))

    def test_bss_overflow_rejected(self):
        symbols = dict(self.symbols)
        symbols["l__DATA"] = self.bss_size + 1
        with self.assertRaises(zexpack.ZexError) as caught:
            self._pack(symbols=symbols)
        self.assertIn("bss_size", str(caught.exception))

    def test_initialized_data_rejected(self):
        symbols = dict(self.symbols)
        symbols["l__INITIALIZER"] = 4
        with self.assertRaises(zexpack.ZexError) as caught:
            self._pack(symbols=symbols)
        self.assertIn("initialized data", str(caught.exception))

    def test_scratch_outside_window_rejected(self):
        symbols = dict(self.symbols)
        symbols["zos_runtime_scratch"] = 0x2000
        with self.assertRaises(zexpack.ZexError) as caught:
            self._pack(symbols=symbols)
        self.assertIn("argument window", str(caught.exception))

    def test_declared_geometry_rejected(self):
        with self.assertRaises(zexpack.ZexError) as caught:
            self._pack(stack_size=1)
        self.assertEqual(caught.exception.code, "ZEX_EXEC_ERR_STACK")

    def test_unknown_and_missing_manifest_keys_rejected(self):
        with self.assertRaises(zexpack.ZexError) as caught:
            self._pack(mystery=1)
        self.assertIn("unknown manifest keys", str(caught.exception))
        manifest_path = self._write()
        manifest = json.loads(open(manifest_path).read())
        del manifest["bss_size"]
        with open(manifest_path, "w") as fh:
            json.dump(manifest, fh)
        with self.assertRaises(zexpack.ZexError) as caught:
            zexpack.pack(manifest_path,
                         os.path.join(self.tmp, "out.zex"))
        self.assertIn("missing 'bss_size'", str(caught.exception))


def ihx_record(addr, data):
    length = len(data)
    body = bytes([length, (addr >> 8) & 0xFF, addr & 0xFF, 0x00]) + bytes(data)
    checksum = (-sum(body)) & 0xFF
    return ":%s%02X" % (body.hex().upper(), checksum)


class TestZexpackCLI(unittest.TestCase):
    def _run(self, *args):
        return subprocess.run([sys.executable, ZEXPACK] + list(args),
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True)

    def test_validate_rejects_each_boundary_with_its_name(self):
        with tempfile.TemporaryDirectory(prefix="zoszex-") as tmp:
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
        with tempfile.TemporaryDirectory(prefix="zoszex-") as tmp:
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
        result = self._run("get", ZEXTEST_JSON, "image_size")
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads(open(ZEXTEST_JSON).read())
        self.assertEqual(int(result.stdout.strip()), manifest["image_size"])


class TestM6MapcheckRules(unittest.TestCase):
    def test_synthetic_map_with_m6_symbols_passes(self):
        mapcheck.check_map(make_map())

    def test_process_launch_must_be_common(self):
        text = make_map(symbols={"_zos_process_launch": 0x3000})
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
        text = make_map(symbols={"_zos_user_copy_staging": 0xC100})
        with self.assertRaisesRegex(mapcheck.MapError, "common region"):
            mapcheck.check_map(text)

    def test_copy_staging_size_mismatch(self):
        text = make_map(symbols={"zos_copy_op": 0xE3C0})
        with self.assertRaisesRegex(mapcheck.MapError, "staging"):
            mapcheck.check_map(text)

    def test_copy_stack_size_mismatch(self):
        text = make_map(symbols={"zos_copy_stack_top": 0xE3D0})
        with self.assertRaisesRegex(mapcheck.MapError, "copy stack"):
            mapcheck.check_map(text)

    def test_missing_m6_symbol_rejected(self):
        text = make_map(symbols={"_zos_bank_copy": None})
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "map has no symbol _zos_bank_copy"):
            mapcheck.check_map(text)


class TestM6Artifacts(unittest.TestCase):
    def _read(self, rel):
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            self.skipTest("%s missing; build it first" % rel)
        with open(path, "rb") as fh:
            return path, fh.read()

    def test_packed_zextest_matches_report_and_manifest(self):
        _p, data = self._read("build/modules/zextest.zex")
        _p, report_text = self._read("build/modules/zextest.report.json")
        report = json.loads(report_text)
        fields, geo = zexpack.validate_image(data)
        manifest = json.loads(open(ZEXTEST_JSON).read())
        self.assertEqual(fields["image_size"], manifest["image_size"])
        self.assertEqual(fields["bss_size"], manifest["bss_size"])
        self.assertEqual(fields["stack_size"], manifest["stack_size"])
        self.assertEqual(geo["entry"],
                         zexpack.LOAD_BASE + manifest["entry_offset"])
        self.assertEqual(report["sha256"],
                         hashlib.sha256(data).hexdigest())

    def test_acceptance_romfs_carries_the_same_zextest_bytes(self):
        _p, stream = self._read("build-m6-selftest/recovery.romfs")
        _h, entries = romfspack.parse_image(stream)
        names = {entry["name"]: entry for entry in entries}
        _p, data = self._read("build/modules/zextest.zex")
        self.assertEqual(names["zextest.zex"]["payload"], data)
        self.assertEqual(names["zextest.zex"]["flags"], romfspack.FLAG_ZEX)
        # Every malformed fixture is present with its generated bytes.
        outdir = os.path.join(ROOT, "build", "modules")
        for fixture in gen_fixtures.build_fixtures():
            path = os.path.join(outdir, fixture["name"])
            if not os.path.exists(path):
                continue
            self.assertEqual(names[fixture["name"]]["payload"],
                             open(path, "rb").read())

    def test_failure_romfs_swaps_in_the_truncated_zex(self):
        _p, stream = self._read("build-m6-failure-selftest/recovery.romfs")
        _h, entries = romfspack.parse_image(stream)
        names = {entry["name"]: entry for entry in entries}
        _p, valid = self._read("build/modules/zextest.zex")
        truncated = open(os.path.join(ROOT, "build", "modules",
                                      "zex-truncated.zex"), "rb").read()
        self.assertEqual(names["zextest.zex"]["payload"], truncated)
        self.assertEqual(names["zexok.zex"]["payload"], valid)

    def test_images_contain_the_acceptance_markers(self):
        _p, success = self._read("build-m6-selftest/"
                                 "zephyros-m6-selftest.bin")
        _p, failure = self._read("build-m6-failure-selftest/"
                                 "zephyros-m6-failure.bin")
        for marker in (b"m6 selftest: OK\r\n", b"zextest: OK\r\n",
                       b"m6 selftest: two-bank OK\r\n"):
            self.assertIn(marker, success)
        for marker in (b"m6 failure selftest: OK\r\n",):
            self.assertIn(marker, failure)
        self.assertNotIn(b"m6 failure selftest: OK", success)
        self.assertNotIn(b"m6 selftest: OK", failure)

    def test_serial_smoke_expectations_exist_verbatim(self):
        import serial_smoke
        _p, success = self._read("build-m6-selftest/"
                                 "zephyros-m6-selftest.bin")
        for const in (b"m6 selftest: zextest status 0x",
                      serial_smoke.M6_REPEAT_OK,
                      serial_smoke.M6_TWO_BANK_OK,
                      serial_smoke.M6_RAW_RETURN_OK,
                      serial_smoke.M6_REJECTED_OK,
                      serial_smoke.M6_SELFTEST_OK,
                      serial_smoke.M6_ZEXTEST_OK):
            self.assertIn(const, success,
                          "serial_smoke expectation %r is not in the "
                          "success image" % const)
        _p, failure = self._read("build-m6-failure-selftest/"
                                 "zephyros-m6-failure.bin")
        self.assertIn(serial_smoke.M6_FAILURE_OK, failure)
        self.assertNotIn(serial_smoke.M6_SELFTEST_OK, failure)

    def test_generated_layout_matches_the_manifest(self):
        _p, generated = self._read("build-m6-selftest/m6_expect.inc")
        manifest = json.loads(open(ZEXTEST_JSON).read())
        env = parse_c_header(os.path.join(ROOT, "build-m6-selftest",
                                          "m6_expect.inc"))
        self.assertEqual(evaluate(env["M6_ZEX_IMAGE_SIZE"], env),
                         manifest["image_size"])
        self.assertEqual(evaluate(env["M6_ZEX_BSS_SIZE"], env),
                         manifest["bss_size"])
        self.assertEqual(evaluate(env["M6_ZEX_STACK_SIZE"], env),
                         manifest["stack_size"])
        self.assertEqual(evaluate(env["M6_ZEX_STACK_BASE"], env),
                         0xE000 - manifest["stack_size"])
        self.assertIn(b"m6_fixtures", generated)


if __name__ == "__main__":
    unittest.main()
