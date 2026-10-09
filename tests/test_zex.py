#!/usr/bin/env python3
"""Host-side ZEX tooling tests for the subsystem acceptance image.

Coverage:

* constant parity: tools/zexpack.py result codes and format constants are
  value-identical with include/zephyros/zex.h;
* one malformed fixture per validation boundary rejects with the exact
  boundary code, and the fixture set matches the subsystem ROMFS
  acceptance manifest (names, media types, control files);
* packing a synthetic linked image honors the manifest, rejects content
  outside the declared image, entry/BSS/scratch/geometry drift, and
  produces byte- and report-identical output on repeat runs;
* the real zextest artifacts: the packed .zex validates, its report
  matches the bytes, the subsystem acceptance ROMFS stores identical
  payloads (including the truncated corrupt-entry fixture beside the
  valid copy), and the generated layout header matches the manifest;
* mapcheck's process/ZEX placement rules (common gates/state, bank-7
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

import zexpack  # noqa: E402
import gen_zex_fixtures as gen_fixtures  # noqa: E402
import mapcheck  # noqa: E402
import romfspack  # noqa: E402
from maptools import evaluate, make_map, parse_c_header  # noqa: E402

ZEX_H = os.path.join(ROOT, "include", "zephyros", "zex.h")
ZEXTEST_JSON = os.path.join(ROOT, "build", "user", "zextest",
                             "zextest.json")
SUBSYS_ZEXTEST_JSON = os.path.join(ROOT, "build-subsys-selftest", "user",
                                    "zextest", "zextest.json")
SUBSYS_MANIFEST = os.path.join(ROOT, "tests", "subsys", "manifest.json")
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
        manifest = json.loads(open(SUBSYS_MANIFEST).read())
        entries = {entry["name"]: entry for entry in manifest["files"]}
        for fixture in gen_fixtures.build_fixtures():
            self.assertIn(fixture["name"], entries,
                          "%s missing from tests/subsys/manifest.json"
                          % fixture["name"])
        # Media types: the malformed/return ZEX files are ZEX, the
        # wrong-type control is packed as DATA.
        self.assertEqual(entries["zex-wrong-type.dat"]["type"], "DATA")
        for name in ("zextest.zex", "zex-return.zex", "zex-bad-magic.zex"):
            self.assertEqual(entries[name]["type"], "ZEX")
        self.assertIn("sio.zmod", entries)
        # The corrupt-entry rollback is no longer a separate failure
        # image: the subsystem manifest carries the truncated fixture
        # next to the valid zextest.zex, so the process selftest rolls
        # the bad load back and still runs the valid copy in one boot.
        self.assertEqual(entries["zex-truncated.zex"]["type"], "ZEX")
        self.assertIn("zextest.zex", entries)


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

    def test_corrupt_image_flashed_to_the_subsystem_mode(self):
        import serial_smoke
        # The corrupt-store image panics before any module runs; a run
        # with --mode subsystem must name the image to flash instead of
        # timing out silently.
        transcript = serial_smoke.BANNER + serial_smoke.ROMFS_CORRUPT_PANIC
        hint = self._hint("subsystem", transcript)
        self.assertIn("romfs-corrupt", hint,
                      "corrupt-image transcript not recognized: %r" % hint)

    def test_subsystem_image_flashed_to_the_corrupt_mode(self):
        import serial_smoke
        # The subsystem image boots the module groups; a run with
        # --mode romfs-corrupt must point back at the subsystem image.
        transcript = (serial_smoke.BANNER +
                      serial_smoke.CONSOLE_SERVICE_OK +
                      serial_smoke.SUBSYS_ROMFS_OK)
        hint = self._hint("romfs-corrupt", transcript)
        self.assertIn("subsystem", hint,
                      "subsystem transcript not recognized: %r" % hint)


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
        if not os.path.exists(ZEXTEST_JSON):
            self.skipTest("the generated zextest manifest has not been built")
        result = self._run("get", ZEXTEST_JSON, "image_size")
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads(open(ZEXTEST_JSON).read())
        self.assertEqual(int(result.stdout.strip()), manifest["image_size"])


class TestProcessMapcheckRules(unittest.TestCase):
    def test_synthetic_map_with_process_symbols_passes(self):
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

    def test_missing_process_symbol_rejected(self):
        text = make_map(symbols={"_zos_bank_copy": None})
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "map has no symbol _zos_bank_copy"):
            mapcheck.check_map(text)


class TestSubsystemArtifacts(unittest.TestCase):
    def _read(self, rel):
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            self.skipTest("%s missing; build it first" % rel)
        with open(path, "rb") as fh:
            return path, fh.read()

    def test_packed_zextest_matches_report_and_manifest(self):
        _p, data = self._read("build/modules/zextest.zex")
        _p, report_text = self._read(
            "build/user/zextest/zextest.report.json")
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
        _p, stream = self._read("build-subsys-selftest/recovery.romfs")
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

    def test_recovery_store_carries_the_corrupt_entry_fixture(self):
        # The retired m6-failure image swapped in a truncated ZEX as the
        # only zextest.zex.  The subsystem image instead packs the
        # truncated fixture beside the valid copy, so one boot proves
        # the corrupt-entry rollback and the valid re-run.
        _p, stream = self._read("build-subsys-selftest/recovery.romfs")
        _h, entries = romfspack.parse_image(stream)
        names = {entry["name"]: entry for entry in entries}
        _p, valid = self._read("build/modules/zextest.zex")
        truncated = open(os.path.join(ROOT, "build", "modules",
                                      "zex-truncated.zex"), "rb").read()
        self.assertEqual(names["zextest.zex"]["payload"], valid)
        self.assertEqual(names["zex-truncated.zex"]["payload"], truncated)

    def test_images_contain_the_acceptance_markers(self):
        _p, image = self._read("build-subsys-selftest/"
                               "zephyros-subsys-selftest.bin")
        for marker in (b"syscall selftest: OK\r\n",
                       b"irq selftest: OK\r\n",
                       b"romfs selftest: OK\r\n",
                       b"process selftest: OK\r\n",
                       b"loader selftest: OK\r\n",
                       b"zextest: OK\r\n",
                       b"process selftest: two-bank OK\r\n"):
            self.assertIn(marker, image)
        # The corrupt-entry rollback is now in the same image (the
        # current equivalent of the retired m6-failure image's marker).
        self.assertIn(b"process selftest: corrupt entry rolled back\r\n",
                      image)

    def test_serial_smoke_expectations_exist_verbatim(self):
        import serial_smoke
        _p, image = self._read("build-subsys-selftest/"
                               "zephyros-subsys-selftest.bin")
        for const in (serial_smoke.SUBSYS_SYSCALL_OK,
                      serial_smoke.SUBSYS_IRQ_OK,
                      serial_smoke.SUBSYS_ROMFS_OK,
                      serial_smoke.SUBSYS_LOADER_OK,
                      serial_smoke.SUBSYS_PROCESS_DONE,
                      serial_smoke.SUBSYS_ZEXTEST_OK,
                      serial_smoke.SUBSYS_REPEAT_OK,
                      serial_smoke.SUBSYS_TWO_BANK_OK,
                      serial_smoke.SUBSYS_RAW_RETURN_OK,
                      serial_smoke.SUBSYS_REJECTED_OK,
                      serial_smoke.SUBSYS_CORRUPT_ROLLBACK_OK,
                      serial_smoke.SUBSYS_STATUS_PREFIX):
            self.assertIn(const, image,
                          "serial_smoke expectation %r is not in the "
                          "subsystem image" % const)
        # The composed status/bank transcript lines are not linked
        # literals (the target prints the prefix and the digits through
        # separate writes); only their embedded prefix is in the image.
        self.assertIn(b"zextest: bank=", image)
        for const in (serial_smoke.SUBSYS_STATUS_PASS,
                      serial_smoke.SUBSYS_STATUS_PASS_BANK1):
            self.assertTrue(const.startswith(
                serial_smoke.SUBSYS_STATUS_PREFIX))
        for const in (serial_smoke.SUBSYS_ZEXTEST_BANK0,
                      serial_smoke.SUBSYS_ZEXTEST_BANK1):
            self.assertTrue(const.startswith(b"zextest: bank="))

    def test_generated_layout_matches_the_manifest(self):
        _p, generated = self._read("build-subsys-selftest/"
                                   "process_expect.inc")
        manifest = json.loads(open(SUBSYS_ZEXTEST_JSON).read())
        env = parse_c_header(os.path.join(ROOT, "build-subsys-selftest",
                                          "process_expect.inc"))
        self.assertEqual(evaluate(env["PROCESS_ZEX_IMAGE_SIZE"], env),
                         manifest["image_size"])
        self.assertEqual(evaluate(env["PROCESS_ZEX_BSS_SIZE"], env),
                         manifest["bss_size"])
        self.assertEqual(evaluate(env["PROCESS_ZEX_STACK_SIZE"], env),
                         manifest["stack_size"])
        self.assertEqual(evaluate(env["PROCESS_ZEX_STACK_BASE"], env),
                         0xE000 - manifest["stack_size"])
        expected_fixtures = len(
            [f for f in gen_fixtures.build_fixtures()
             if f["kernel_code"] is not None])
        self.assertEqual(evaluate(env["PROCESS_FIXTURE_COUNT"], env),
                         expected_fixtures)
        self.assertIn(b"process_fixtures", generated)


if __name__ == "__main__":
    unittest.main()
