#!/usr/bin/env python3
"""Host-side M8 tests for work package 14-m8-driver-abi-arena.

Covers the declarative ABI v2 surface and the derived-arena build
contract:

  - driver ABI v2 version, structure member order, and callback order,
  - capability high-bit ranges (VIDEO/HID/IOC) and class values,
  - the two-pass arena geometry: alignment, minimum size, and the
    negative cases (unaligned relation, bytes in the derived arena,
    below-16-KiB arena, kernel end past the arena end),
  - the boot-set arena plan (per-module report and module-sum overflow),
  - ABI v1 rejection and the architectural granule bound,
  - that the module-facing ABI exposes no romfs_file_t or mutable state.

The executable behavior (discovery calls, console policy, resource
imports) is replayed from the real linked images by test_m3_sim.py,
test_m4_sim.py, test_m6_sim.py, and the target acceptance code under
tests/target/ and tests/modules/.
"""

import contextlib
import io
import json
import os
import re
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)

import mapcheck  # noqa: E402
import pack_image  # noqa: E402
import zmodpack  # noqa: E402
from test_m1 import (  # noqa: E402
    evaluate, make_map, parse_c_header, MEMORY_INC)
from test_zmod import (  # noqa: E402
    build_valid_container, pack_in_dir, rel_text, simple_manifest)
from test_m3 import parse_c_enums  # noqa: E402

DEVICE_H = os.path.join(ROOT, "include", "zephyros", "device.h")
DRIVER_H = os.path.join(ROOT, "include", "zephyros", "driver.h")
MODULE_H = os.path.join(ROOT, "include", "zephyros", "module.h")
CONSOLE_H = os.path.join(ROOT, "include", "zephyros", "console.h")

_PACK_IMAGE_GLOBALS = (
    "ARENA_BASE", "ARENA_END", "ARENA_DERIVED", "ARENA_MIN_SIZE",
    "ARENA_MIN_GRANULES", "GRANULE_SIZE", "GRANULE_COUNT", "REGIONS",
    "ROMFS_FIRST_PAGE", "ROMFS_PAGE_COUNT", "ROMFS_WINDOW_SIZE",
    "ROMFS_CAPACITY", "ROM_ACCESS_BASE", "ROM_ACCESS_SIZE",
)


def struct_member_order(text, struct_name, function_pointers=False):
    stripped = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    body = stripped.split("struct %s {" % struct_name, 1)[1].split("};", 1)[0]
    names = []
    if function_pointers:
        names = re.findall(r"\(\s*\*\s*(\w+)\s*\)", body)
    else:
        for line in body.splitlines():
            line = line.strip().rstrip(";")
            match = re.search(r"(\w+)\s*$", line)
            if line and match:
                names.append(match.group(1))
    return names


@contextlib.contextmanager
def derived_arena_context(start, granules):
    """Apply a synthetic derived arena for one mapcheck call."""
    saved = {name: getattr(mapcheck, name) for name in
             ("DRIVER_ARENA_DERIVED", "DRIVER_ARENA_BASE", "DRIVER_ARENA_END",
              "DRIVER_GRANULE_SIZE", "DRIVER_GRANULES", "DRIVER_ARENA_MIN_SIZE",
              "DRIVER_ARENA_MIN_GRANULES", "OWNED_AREAS")}
    try:
        mapcheck.DRIVER_ARENA_DERIVED = 1
        mapcheck.DRIVER_ARENA_BASE = start
        mapcheck.DRIVER_ARENA_END = 0xC000
        mapcheck.DRIVER_GRANULE_SIZE = 0x200
        mapcheck.DRIVER_GRANULES = granules
        mapcheck.DRIVER_ARENA_MIN_SIZE = 0x4000
        mapcheck.DRIVER_ARENA_MIN_GRANULES = 32
        owned = dict(mapcheck.OWNED_AREAS)
        for name in mapcheck.LOADABLE_AREAS:
            owned[name] = (mapcheck.BANK7_BASE, start)
        mapcheck.OWNED_AREAS = owned
        yield
    finally:
        for name, value in saved.items():
            setattr(mapcheck, name, value)


class TestAbiV2Contract(unittest.TestCase):
    def setUp(self):
        self.driver_env = parse_c_header(DRIVER_H)
        self.driver_env.update(parse_c_header(DEVICE_H))
        self.device_env = parse_c_header(DEVICE_H)

    def test_abi_version_is_two(self):
        self.assertEqual(evaluate("ZDRIVER_ABI_VERSION", self.driver_env), 2)

    def test_zdriver_api_callback_order(self):
        text = open(DRIVER_H).read()
        self.assertEqual(
            struct_member_order(text, "zdriver_api", function_pointers=True),
            ["device_register", "irq_claim", "irq_release", "device_find",
             "device_interface", "resource_size", "resource_read"])

    def test_zchar_ops_member_order(self):
        text = open(DEVICE_H).read()
        self.assertEqual(
            struct_member_order(text, "zchar_ops", function_pointers=True),
            ["putc", "write", "poll", "getc"])

    def test_device_descriptor_and_entry_member_order(self):
        text = open(DEVICE_H).read()
        self.assertEqual(
            struct_member_order(text, "zdevice_desc"),
            ["class_id", "capabilities", "name", "char_ops", "class_ops",
             "state"])
        self.assertEqual(
            struct_member_order(text, "zdevice"),
            ["handle", "class_id", "capabilities", "name", "char_ops",
             "class_ops", "state", "driver"])

    def test_compile_time_layout_pins_cover_abi_fields(self):
        device = open(DEVICE_H).read()
        driver = open(DRIVER_H).read()
        for offset in ("char_ops) == 7", "class_ops) == 9", "state) == 11"):
            self.assertIn(offset, device)
        for offset in ("device_find) == 6", "device_interface) == 8",
                       "resource_size) == 10", "resource_read) == 12"):
            self.assertIn(offset, driver)

    def test_capability_high_bits_are_pinned(self):
        expected = {
            "ZCAP_VIDEO_CTRL_WRITE": 0x0100,
            "ZCAP_VIDEO_DATA_WRITE": 0x0200,
            "ZCAP_VIDEO_DATA_BLOCK": 0x0400,
            "ZCAP_VIDEO_PALETTE": 0x0800,
            "ZCAP_VIDEO_INDIRECT": 0x1000,
            "ZCAP_VIDEO_PRESENT": 0x2000,
            "ZCAP_VIDEO_RESET": 0x4000,
            "ZCAP_HID_TEXT": 0x10000,
            "ZCAP_HID_KEY_EVENTS": 0x20000,
            "ZCAP_HID_GAMEPAD": 0x40000,
            "ZCAP_IOC_COMMAND": 0x1000000,
            "ZCAP_IOC_BULK_READ": 0x2000000,
            "ZCAP_IOC_BULK_WRITE": 0x4000000,
            "ZCAP_IOC_STATUS": 0x8000000,
        }
        for name, value in expected.items():
            self.assertEqual(evaluate(name, self.device_env), value, name)

    def test_class_any_is_zero_and_not_a_registrable_class(self):
        env = dict(self.device_env)
        env.update(parse_c_enums(DEVICE_H, env))
        self.assertEqual(evaluate("ZDEV_CLASS_ANY", env), 0)
        self.assertEqual(evaluate("ZDEV_SERIAL", env), 1)
        self.assertEqual(evaluate("ZDEV_HID", env), 4)
        self.assertEqual(evaluate("ZDEV_IOC", env), 5)

    def test_module_api_exposes_no_romfs_types_or_registry_mutation(self):
        for path in (DRIVER_H, DEVICE_H, CONSOLE_H):
            text = re.sub(r"/\*.*?\*/", "", open(path).read(), flags=re.S)
            self.assertNotIn("romfs_file_t", text, path)
            self.assertNotIn("romfs_lookup", text, path)
            self.assertNotIn("romfs_read", text, path)
        driver = open(DRIVER_H).read()
        self.assertIn("device_find", driver)
        self.assertIn("device_interface", driver)
        self.assertIn("resource_size", driver)
        self.assertIn("resource_read", driver)
        # No registry mutation is offered through the module API block.
        api_body = driver.split("struct zdriver_api {", 1)[1].split("};", 1)[0]
        for forbidden in ("rollback", "unregister", "set_owner"):
            self.assertNotIn(forbidden, api_body)


class TestDerivedGeometry(unittest.TestCase):
    def setUp(self):
        self.values = mapcheck.parse_inc(MEMORY_INC)

    def probe_map(self, end):
        return make_map(
            areas={"_CODE": (0x2000, end - 0x2000)},
            symbols={
                "s__CODE": 0x2000, "l__CODE": end - 0x2000,
                "s__HOME": end, "l__HOME": 0,
                "s__INITIALIZER": end, "l__INITIALIZER": 0,
                "s__GSINIT": end, "l__GSINIT": 0,
                "s__GSFINAL": end, "l__GSFINAL": 0,
            })

    def test_derives_aligned_start_and_end(self):
        geometry = mapcheck.derive_geometry(self.probe_map(0x5CA2),
                                            self.values)
        self.assertEqual(geometry["kernel_code_end"], 0x5CA2)
        self.assertEqual(geometry["arena_start"], 0x5E00)
        self.assertEqual(geometry["arena_end"], 0xC000)
        self.assertEqual(geometry["granules"], (0xC000 - 0x5E00) // 512)

    def test_aligned_end_needs_no_padding(self):
        geometry = mapcheck.derive_geometry(self.probe_map(0x5E00),
                                            self.values)
        self.assertEqual(geometry["arena_start"], 0x5E00)

    def test_below_minimum_arena_is_rejected(self):
        with self.assertRaisesRegex(mapcheck.MapError, "minimum"):
            mapcheck.derive_geometry(self.probe_map(0x8001), self.values)

    def test_kernel_end_past_arena_end_is_rejected(self):
        with self.assertRaisesRegex(mapcheck.MapError, "past the arena end"):
            mapcheck.derive_geometry(self.probe_map(0xC201), self.values)

    def test_geometry_inc_marks_probe_and_derived_modes(self):
        geometry = mapcheck.derive_geometry(self.probe_map(0x5CA2),
                                            self.values)
        derived = mapcheck.format_geometry_inc(geometry, derived=True)
        self.assertIn("ZOS_ARENA_DERIVED = 1", derived)
        self.assertIn("ZOS_DRIVER_ARENA_BASE = 0x5E00", derived)
        legacy = mapcheck.format_geometry_inc(geometry, derived=False)
        self.assertIn("ZOS_ARENA_DERIVED = 0", legacy)

    def test_memory_variant_replaces_arena_declarations(self):
        geometry = mapcheck.derive_geometry(self.probe_map(0x5CA2),
                                            self.values)
        variant = mapcheck.format_memory_variant(open(MEMORY_INC).read(),
                                                 geometry)
        self.assertIn("ZOS_DRIVER_ARENA_DERIVED = 1", variant)
        self.assertIn("ZOS_DRIVER_ARENA_BASE = 0x5E00", variant)
        self.assertIn("ZOS_DRIVER_ARENA_END = 0xC000", variant)
        self.assertIn("ZOS_DRIVER_GRANULES = "
                      "%d" % geometry["granules"], variant)
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "memory.inc")
            with open(path, "w") as handle:
                handle.write(variant)
            parsed = mapcheck.parse_inc(path)
        self.assertEqual(parsed["ZOS_DRIVER_ARENA_DERIVED"], 1)
        self.assertEqual(parsed["ZOS_DRIVER_ARENA_BASE"], 0x5E00)
        self.assertEqual(parsed["ZOS_DRIVER_GRANULES"],
                         geometry["granules"])

    def test_mapcheck_rejects_start_not_align_up_of_kernel_end(self):
        # Linked end 0x206F, arena start 0x2400: aligned up would be
        # 0x2200, so the derived relation must fail.
        text = make_map(
            areas={"_CODE": (0x2000, 0x6F)},
            symbols={
                "s__CODE": 0x2000, "l__CODE": 0x6F,
                "__kernel_code_end": 0x206F,
                "__driver_arena_start": 0x2400,
                "__driver_arena_end": 0xC000,
                "_irq_kernel_code_end": 0x206F,
            })
        with derived_arena_context(0x2400, (0xC000 - 0x2400) // 512):
            with self.assertRaisesRegex(mapcheck.MapError, "align_up"):
                mapcheck.check_map(text)

    def test_mapcheck_accepts_correctly_derived_relation(self):
        text = make_map(
            areas={"_CODE": (0x2000, 0x6F)},
            symbols={
                "s__CODE": 0x2000, "l__CODE": 0x6F,
                "__kernel_code_end": 0x206F,
                "__driver_arena_start": 0x2200,
                "__driver_arena_end": 0xC000,
                "_irq_kernel_code_end": 0x206F,
            })
        with derived_arena_context(0x2200, (0xC000 - 0x2200) // 512):
            mapcheck.check_map(text)

    def test_mapcheck_rejects_linked_bytes_in_derived_arena(self):
        text = make_map(
            areas={"_VECTOR": (0x2500, 0x100)},
            symbols={"__kernel_code_end": 0x206F,
                     "__driver_arena_start": 0x2400,
                     "__driver_arena_end": 0xC000})
        with derived_arena_context(0x2400, (0xC000 - 0x2400) // 512):
            with self.assertRaisesRegex(mapcheck.MapError, "driver arena"):
                mapcheck.check_map(text)

    def test_pack_image_rejects_bytes_in_derived_arena(self):
        geometry = {"kernel_code_end": 0x226F, "arena_start": 0x2400,
                    "arena_end": 0xC000,
                    "granules": (0xC000 - 0x2400) // 512}
        variant = mapcheck.format_memory_variant(open(MEMORY_INC).read(),
                                                 geometry)
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "memory.inc")
            with open(path, "w") as handle:
                handle.write(variant)
            saved = {name: getattr(pack_image, name)
                     for name in _PACK_IMAGE_GLOBALS}
            try:
                pack_image.apply_memory_inc(path)
                self.assertEqual(pack_image.ARENA_BASE, 0x2400)
                with self.assertRaisesRegex(pack_image.LayoutError,
                                            "driver arena"):
                    pack_image.validate_layout({0x2400: 0x00})
                pack_image.validate_layout({0x2200: 0x11})
            finally:
                for name, value in saved.items():
                    setattr(pack_image, name, value)


class TestBootSetPlan(unittest.TestCase):
    def write_module(self, tmp, name, area_size):
        rel = rel_text(
            name, [("_CODE", area_size)],
            [("__.ABS.", "def", 0, None), ("_entry", "def", 0, 0)],
            [(0, 0, bytes([0xC9]), [])])
        container, _ = pack_in_dir(
            tmp, {name + ".rel": rel},
            simple_manifest([name + ".rel"], "entry", name=name))
        path = os.path.join(tmp, name + ".zmod")
        with open(path, "wb") as handle:
            handle.write(container)
        return path

    def write_geometry(self, tmp, granules, derived=0):
        path = os.path.join(tmp, "geometry.inc")
        start = 0xC000 - granules * 512
        with open(path, "w") as handle:
            handle.write("ZOS_DRIVER_ARENA_DERIVED = %d\n"
                         "ZOS_DRIVER_ARENA_BASE = 0x%04X\n"
                         "ZOS_DRIVER_ARENA_END = 0xC000\n"
                         "ZOS_DRIVER_GRANULE_SIZE = 0x0200\n"
                         "ZOS_DRIVER_GRANULES = %d\n"
                         % (derived, start, granules))
        return path

    def test_plan_reports_modules_and_totals(self):
        with tempfile.TemporaryDirectory() as tmp:
            module = self.write_module(tmp, "boot", 1024)  # 2 granules
            geometry = self.write_geometry(tmp, 32)
            report = os.path.join(tmp, "plan.json")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                result = zmodpack.cmd_plan(
                    ["--geometry", geometry, "--report", report, module])
            self.assertEqual(result, 0)
            plan = json.load(open(report))
            self.assertEqual(plan["arena"]["granules"], 32)
            self.assertEqual(plan["arena"]["used_granules"], 2)
            self.assertEqual(plan["arena"]["free_granules"], 30)
            self.assertEqual(plan["modules"][0]["name"], "boot")
            self.assertEqual(plan["modules"][0]["granules"], 2)
            self.assertEqual(plan["modules"][0]["image_size"], 1024)
            self.assertIn("boot set uses 2 of 32 granules", out.getvalue())

    def test_plan_rejects_module_sum_overflow(self):
        with tempfile.TemporaryDirectory() as tmp:
            module = self.write_module(tmp, "big", 0x2800)  # 20 granules
            geometry = self.write_geometry(tmp, 16)
            with self.assertRaisesRegex(zmodpack.ZmodError,
                                        "cannot coexist|needs 20 granules"):
                with contextlib.redirect_stdout(io.StringIO()):
                    zmodpack.cmd_plan(["--geometry", geometry, module])

    def test_plan_rejects_inconsistent_geometry(self):
        with tempfile.TemporaryDirectory() as tmp:
            module = self.write_module(tmp, "boot", 512)
            # Base/end span 32 granules but the declaration says 16.
            geometry = os.path.join(tmp, "geometry.inc")
            with open(geometry, "w") as handle:
                handle.write("ZOS_DRIVER_ARENA_DERIVED = 1\n"
                             "ZOS_DRIVER_ARENA_BASE = 0x8000\n"
                             "ZOS_DRIVER_ARENA_END = 0xC000\n"
                             "ZOS_DRIVER_GRANULE_SIZE = 0x0200\n"
                             "ZOS_DRIVER_GRANULES = 16\n")
            with self.assertRaisesRegex(zmodpack.ZmodError, "inconsistent"):
                with contextlib.redirect_stdout(io.StringIO()):
                    zmodpack.cmd_plan(["--geometry", geometry, module])

    def test_packer_rejects_architectural_granule_overflow(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(zmodpack.ZmodError,
                                        "architectural maximum"):
                self.write_module(tmp, "huge", 0x10200)  # 129 granules

    def test_abi_v1_container_rejected(self):
        container, _ = build_valid_container()
        data = bytearray(container)
        data[6] = 1
        with self.assertRaisesRegex(zmodpack.ZmodError,
                                    "unsupported driver ABI 1"):
            zmodpack.parse_container(bytes(data), "abi1.zmod")

    def test_memory_inc_declares_legacy_defaults_and_minimums(self):
        values = mapcheck.parse_inc(MEMORY_INC)
        self.assertEqual(values["ZOS_DRIVER_ARENA_DERIVED"], 0)
        self.assertEqual(values["ZOS_DRIVER_ARENA_MIN_SIZE"], 0x4000)
        self.assertEqual(values["ZOS_DRIVER_ARENA_MIN_GRANULES"], 32)
        self.assertEqual(values["ZOS_DRIVER_ARENA_END"], 0xC000)
        self.assertEqual(values["ZOS_DRIVER_GRANULE_SIZE"], 512)

    def test_generated_production_geometry_matches_manifest(self):
        geometry_inc = os.path.join(ROOT, "build", "geometry",
                                    "arena_geometry.inc")
        manifest_path = os.path.join(ROOT, "build",
                                     "zephyros.manifest.json")
        if not (os.path.exists(geometry_inc)
                and os.path.exists(manifest_path)):
            self.skipTest("derived production build missing; run make")
        values = mapcheck.parse_inc(geometry_inc)
        self.assertEqual(values["ZOS_ARENA_DERIVED"], 1)
        manifest = json.load(open(manifest_path))
        arena = manifest["arena"]
        self.assertEqual(values["ZOS_KERNEL_CODE_END"],
                         manifest["entries"]["__kernel_code_end"])
        self.assertEqual(values["ZOS_DRIVER_ARENA_BASE"], arena["start"])
        self.assertEqual(values["ZOS_DRIVER_ARENA_END"], arena["end"])
        self.assertGreaterEqual(arena["granules"], 32)


if __name__ == "__main__":
    unittest.main()
