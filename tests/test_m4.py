#!/usr/bin/env python3
"""Host-side M4 tests: constant contract, arena placement rules, build
artifact checks, and the frozen loader/IRQ interface surface.

The executable loader semantics are replayed from the real linked image by
tests/test_m4_sim.py; the ZMOD tooling is covered by tests/test_zmod.py.
Run with: make test
"""

import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)

import mapcheck  # noqa: E402
from z80sim import read_symbols  # noqa: E402
from test_m1 import evaluate, make_map, parse_c_header  # noqa: E402

MODULE_H = os.path.join(ROOT, "include", "zephyros", "module.h")
M4_SELFTEST_H = os.path.join(ROOT, "tests", "target", "include",
                             "m4_selftest.h")
DRIVER_H = os.path.join(ROOT, "include", "zephyros", "driver.h")
IRQ_H = os.path.join(ROOT, "include", "zephyros", "irq.h")
ERRNO_H = os.path.join(ROOT, "include", "zephyros", "errno.h")
MEMORY_INC = os.path.join(ROOT, "platform", "zephyr80", "memory.inc")


class TestM4Constants(unittest.TestCase):
    def setUp(self):
        self.env = parse_c_header(ERRNO_H)
        self.env.update(parse_c_header(MODULE_H))
        self.env.update(parse_c_header(DRIVER_H))

    def test_driver_abi_is_frozen_at_one(self):
        self.assertEqual(evaluate("ZDRIVER_ABI_VERSION", self.env), 1)

    def test_new_errno_codes_stable_and_distinct(self):
        self.assertEqual(evaluate("ZOS_EIO", self.env), 0x0A)
        self.assertEqual(evaluate("ZOS_EINIT", self.env), 0x0B)

    def test_loader_limits(self):
        self.assertEqual(evaluate("ZMOD_GRANULE_SIZE", self.env), 512)
        self.assertEqual(evaluate("ZMOD_GRANULE_COUNT", self.env), 16)
        self.assertEqual(evaluate("ZMOD_MAX_MODULES", self.env), 16)
        self.assertEqual(evaluate("ZMOD_HEADER_SIZE", self.env), 28)
        self.assertEqual(evaluate("ZREL_ABS16", self.env), 1)

    def test_module_state_machine_distinct(self):
        text = open(MODULE_H).read()
        env = dict(self.env)
        import re
        match = re.search(r"enum\s*\{([^}]*)\}", text)
        self.assertIsNotNone(match)
        names = []
        for item in match.group(1).split(","):
            item = item.split("/*")[0].strip()
            if item.startswith("ZMOD_STATE_"):
                names.append(item.split("=")[0].strip())
        self.assertEqual(names, ["ZMOD_STATE_EMPTY", "ZMOD_STATE_LOADING",
                                 "ZMOD_STATE_RESIDENT"])

    def test_selftest_result_codes_distinct(self):
        env = parse_c_header(M4_SELFTEST_H)
        import re
        numeric = {name: value for name, value in env.items()
                   if name.startswith("ZOS_M4ST_FAIL_") and
                   re.match(r"^0[xX][0-9A-Fa-f]+u?$", value.strip())}
        self.assertGreaterEqual(len(numeric), 8)
        self.assertEqual(evaluate("ZOS_M4ST_OK", env), 0)
        values = [evaluate(name, env) for name in numeric]
        self.assertEqual(len(set(values)), len(values))
        self.assertNotIn(0, values)

    def test_catalog_is_null_terminated_surface(self):
        text = open(MODULE_H).read()
        for member in ("name", "blob", "size"):
            self.assertIn(member, text)
        self.assertIn("zmod_catalog", text)


class TestM4MapRules(unittest.TestCase):
    """mapcheck's new negative cases, using the shared synthetic map."""

    def test_synthetic_map_with_m4_symbols_passes(self):
        mapcheck.check_map(make_map())

    def test_module_loader_in_common_rejected(self):
        text = make_map(symbols={"_module_load": 0xE200})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_relocator_in_isr_stack_rejected(self):
        text = make_map(symbols={"_zmod_apply_relocations": 0xDF10})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_missing_m4_symbol_rejected(self):
        text = make_map(symbols={"_module_code_allowed": None})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_arena_overlap_rejected(self):
        text = make_map(areas={"_CODE": (0x9F00, 0x200)},
                        symbols={"s__CODE": 0x9F00, "l__CODE": 0x200})
        with self.assertRaisesRegex(mapcheck.MapError, "driver arena"):
            mapcheck.check_map(text)

    def test_arena_bounds_must_match_platform(self):
        text = make_map(symbols={"__driver_arena_end": 0xBF00})
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "__driver_arena_end"):
            mapcheck.check_map(text)
        text = make_map(symbols={"__driver_arena_start": 0xA100})
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "__driver_arena_start"):
            mapcheck.check_map(text)

    def test_m4_prefix_sweep_covers_required_symbols(self):
        for name in mapcheck.M4_BANK7_ROUTINES:
            self.assertTrue(
                name.startswith(mapcheck.M4_BANK7_PREFIXES),
                "%s escapes the M4 prefix sweep" % name,
            )

    def test_irq_module_validation_hook_is_declared(self):
        text = open(MODULE_H).read()
        self.assertIn("module_code_allowed", text)
        irq = open(IRQ_H).read()
        for member in ("irq_registry_checkpoint", "irq_registry_rollback",
                       "irq_registry_set_owner"):
            self.assertIn(member, irq)


class TestM4Artifacts(unittest.TestCase):
    def test_arena_reserved_in_memory_inc(self):
        layout = mapcheck.parse_memory_inc(MEMORY_INC)
        self.assertEqual(layout["ZOS_DRIVER_ARENA_BASE"], 0xA000)
        self.assertEqual(layout["ZOS_DRIVER_ARENA_END"], 0xC000)
        self.assertEqual(layout["ZOS_DRIVER_GRANULE_SIZE"], 512)
        self.assertEqual(layout["ZOS_DRIVER_GRANULES"], 16)

    def test_normal_manifest_reports_arena(self):
        path = os.path.join(ROOT, "build", "zephyros.manifest.json")
        if not os.path.exists(path):
            self.skipTest("build manifest missing; run `make` first")
        manifest = json.load(open(path))
        arena = manifest["arena"]
        self.assertEqual(arena["start"], 0xA000)
        self.assertEqual(arena["end"], 0xC000)
        self.assertEqual(arena["granules"], 16)
        self.assertEqual(arena["free_granules"], 16)
        self.assertEqual(arena["modules"], [])
        self.assertEqual(manifest["entries"]["__driver_arena_start"], 0xA000)
        self.assertEqual(manifest["entries"]["__driver_arena_end"], 0xC000)

    def test_normal_image_has_empty_catalog_and_no_selftest_strings(self):
        bin_path = os.path.join(ROOT, "build", "zephyros.bin")
        if not os.path.exists(bin_path):
            self.skipTest("normal image missing; run make")
        image = open(bin_path, "rb").read()
        self.assertNotIn(b"m4 loader selftest", image)
        self.assertNotIn(b"m4 selftest", image)
        self.assertNotIn(b"m4.probe", image)
        # The M4 loader code is part of the production kernel image and
        # the dynamic SIO module bound the console at boot.
        self.assertIn(b"sio0.b", image)  # packed module name
        self.assertIn(b"console: dynamic sio0.b OK\r\n", image)

    # --- dynamic-driver acceptance image matrix --------------------------
    # The packed module bytes live once under build/modules/ and are
    # embedded verbatim into each image's generated catalog blob.

    M4_IMAGE_MATRIX = (
        ("build-m4-selftest", "zephyros-m4-selftest.bin",
         "sio-irq.zmod"),
        ("build-m4-loader-selftest", "zephyros-m4-loader-selftest.bin",
         "sio.zmod"),
        ("build-m4-placement-selftest",
         "zephyros-m4-placement-selftest.bin", "sio.zmod"),
        ("build-m4-failure", "zephyros-m4-failure.bin", "sio-fail.zmod"),
    )

    def read_artifact(self, subdir, filename):
        bin_path = os.path.join(ROOT, subdir, filename)
        if not os.path.exists(bin_path):
            self.skipTest("%s missing; build the image first" % bin_path)
        image = open(bin_path, "rb").read()
        syms = read_symbols(os.path.join(ROOT, subdir, "zephyros.map"))
        return image, syms

    def embedded_blob(self, image, syms):
        """Extract the boot-module container embedded in ROM page 1."""
        addr = syms["_z4_module_boot"]
        size_addr = syms["_z4_module_boot_size"]
        size = image[0x10000 + size_addr] | (image[0x10000 + size_addr + 1] << 8)
        return bytes(image[0x10000 + addr:0x10000 + addr + size])

    def test_embedded_boot_module_bytes_match_packed_variants(self):
        for subdir, filename, zmod in self.M4_IMAGE_MATRIX:
            image, syms = self.read_artifact(subdir, filename)
            packed_path = os.path.join(ROOT, "build", "modules", zmod)
            if not os.path.exists(packed_path):
                self.skipTest("build/modules/%s missing; run make" % zmod)
            self.assertEqual(
                self.embedded_blob(image, syms),
                open(packed_path, "rb").read(),
                "embedded boot module in %s differs from %s"
                % (subdir, zmod))

    def test_placement_image_shifts_the_arena(self):
        image, syms = self.read_artifact(
            "build-m4-placement-selftest",
            "zephyros-m4-placement-selftest.bin")
        self.assertEqual(syms["__driver_arena_start"], 0x9E00)
        self.assertEqual(syms["__driver_arena_end"], 0xBE00)
        manifest_path = os.path.join(
            ROOT, "build-m4-placement-selftest",
            "zephyros-m4-placement-selftest.manifest.json")
        manifest = json.load(open(manifest_path))
        self.assertEqual(manifest["arena"]["start"], 0x9E00)
        self.assertEqual(manifest["arena"]["end"], 0xBE00)
        self.assertEqual(manifest["arena"]["granules"], 16)

    def test_no_static_sio_symbols_in_any_image_map(self):
        # M5: production and the M5 images boot from ROMFS and carry no
        # embedded catalog; the pre-M5 acceptance images still do.
        catalog_images = ("build-m3-selftest", "build-m4-selftest",
                          "build-m4-loader-selftest",
                          "build-m4-placement-selftest", "build-m4-failure")
        checked = 0
        for subdir in ("build",) + catalog_images:
            map_path = os.path.join(ROOT, subdir, "zephyros.map")
            if not os.path.exists(map_path):
                continue
            syms = read_symbols(map_path)
            for name, addr in syms.items():
                if addr == 0:
                    continue
                self.assertFalse(
                    name.startswith(("_sio_", "_sio0b_", "_sio_driver")),
                    "static SIO symbol %s linked in %s" % (name, subdir))
                self.assertFalse(
                    name.startswith("_driver_") or name == "_boot_drivers",
                    "retired static-driver symbol %s linked in %s"
                    % (name, subdir))
            if subdir in catalog_images:
                self.assertIn("_z4_module_boot", syms,
                              "boot catalog missing in %s" % subdir)
            else:
                self.assertNotIn("_z4_module_boot", syms,
                                 "embedded boot blob leaked into %s"
                                 % subdir)
                self.assertNotIn("_zmod_catalog", syms,
                                 "embedded catalog leaked into %s" % subdir)
            checked += 1
        if checked == 0:
            self.skipTest("no image maps present; build first")

    def test_production_image_links_no_acceptance_symbols(self):
        map_path = os.path.join(ROOT, "build", "zephyros.map")
        if not os.path.exists(map_path):
            self.skipTest("build map missing; run make first")
        syms = read_symbols(map_path)
        for name, addr in syms.items():
            if addr == 0:
                continue
            for prefix in mapcheck.PRODUCTION_FORBIDDEN:
                self.assertFalse(
                    name.startswith(prefix),
                    "acceptance symbol %s leaked into the production "
                    "image" % name)

    def test_catalog_fixture_symbols_by_image(self):
        _, loader_syms = self.read_artifact(
            "build-m4-loader-selftest",
            "zephyros-m4-loader-selftest.bin")
        for fixture in ("_z4_fixture_probe", "_z4_fixture_fail",
                        "_z4_fixture_filler"):
            self.assertIn(fixture, loader_syms)
        _, prod_syms = self.read_artifact("build", "zephyros.bin")
        for fixture in ("_z4_fixture_probe", "_z4_fixture_fail",
                        "_z4_fixture_filler"):
            self.assertNotIn(fixture, prod_syms)

    def test_m4_loader_selftest_image_markers(self):
        image, _ = self.read_artifact("build-m4-loader-selftest",
                                      "zephyros-m4-loader-selftest.bin")
        self.assertEqual(len(image), 0x80000)
        self.assertIn(b"m4 loader selftest: OK\r\n", image)
        self.assertIn(b"console: dynamic sio0.b OK\r\n", image)
        # Fixture device names are packed into the embedded catalogs.
        for marker in (b"m4.probe.a", b"m4.probe.b", b"m4.fail"):
            self.assertIn(marker, image)

    def test_m4_acceptance_image_markers(self):
        image, _ = self.read_artifact("build-m4-selftest",
                                      "zephyros-m4-selftest.bin")
        self.assertEqual(len(image), 0x80000)
        for marker in (b"console: dynamic sio0.b OK\r\n",
                       b"m4 loader selftest: OK\r\n",
                       b"m4 selftest: sio at ",
                       b"m4 selftest: OK\r\n"):
            self.assertIn(marker, image)

    def test_m4_failure_image_markers(self):
        image, _ = self.read_artifact("build-m4-failure",
                                      "zephyros-m4-failure.bin")
        self.assertEqual(len(image), 0x80000)
        # The success literal still exists in kernel_main's code (both
        # branches are compiled); only the runtime transcript differs,
        # which the smoke tool and the failure replay verify.
        self.assertIn(b"console: dynamic driver failed\r\n", image)
        self.assertIn(b"m4 failure selftest: OK\r\n", image)

    def test_smoke_mode_strings_match_images(self):
        """The serial_smoke expectations must exist verbatim in the
        acceptance images (and the failure transcript must not contain
        the success line)."""
        import serial_smoke
        accept, _ = self.read_artifact("build-m4-selftest",
                                       "zephyros-m4-selftest.bin")
        failure, _ = self.read_artifact("build-m4-failure",
                                        "zephyros-m4-failure.bin")
        for line in serial_smoke.BANNER.split(b"\r\n"):
            if line:
                self.assertIn(line, accept, "banner line %r missing" % line)
        for const in (serial_smoke.CONSOLE_DYNAMIC_OK,
                      serial_smoke.M4_SELFTEST_OK,
                      serial_smoke.M4_SIO_PREFIX,
                      serial_smoke.M4_ACCEPT_OK):
            self.assertIn(const, accept)
        for const in (serial_smoke.CONSOLE_DYNAMIC_FAIL,
                      serial_smoke.M4_FAILURE_OK):
            self.assertIn(const, failure)

    def test_static_linker_file_lists_module_objects(self):
        lk_path = os.path.join(ROOT, "linker", "zephyr80.lk")
        listed = set()
        for line in open(lk_path):
            line = line.split(";")[0].strip()
            if line.startswith("build/") and line.endswith(".rel"):
                listed.add(line)
        for expected in ("build/arena.rel", "build/module_loader.rel",
                         "build/module_relocate.rel",
                         "build/romfs.rel", "build/rom_access.rel"):
            self.assertIn(expected, listed)
        build_dir = os.path.join(ROOT, "build")
        if not os.path.isdir(build_dir):
            self.skipTest("build/ missing; run make first")
        built = {"build/" + name for name in os.listdir(build_dir)
                 if name.endswith(".rel")}
        self.assertEqual(
            listed, built,
            "linker/zephyr80.lk and the Makefile object list drifted: "
            "only in lk=%s, only in build=%s"
            % (sorted(listed - built), sorted(built - listed)),
        )


if __name__ == "__main__":
    unittest.main()
