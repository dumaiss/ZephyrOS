#!/usr/bin/env python3
"""Host-side driver-ABI and derived-arena contract tests.

Covers the declarative driver-ABI surface and the derived-arena build
contract:

  - driver ABI version, structure member order, and callback order,
  - capability high-bit ranges (VIDEO/HID/IOC) and class values,
  - the two-pass arena geometry: alignment, minimum size, and the
    negative cases (unaligned relation, bytes in the derived arena,
    below-16-KiB arena, kernel end past the arena end),
  - the boot-set arena plan (per-module report and module-sum overflow),
  - ABI v1 rejection and the architectural granule bound,
  - that the module-facing ABI exposes no romfs_file_t or mutable state.

The executable behavior (discovery calls, console policy, resource
imports) is replayed from the real linked images by the subsystem
acceptance replay (tests/test_subsys_sim.py) and the target acceptance
code under tests/target/ and tests/modules/.
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
from maptools import (  # noqa: E402
    evaluate, layout_context, make_map, parse_c_header, IRQ_INC, MEMORY_INC)
from test_zmod import (  # noqa: E402
    build_valid_container, pack_in_dir, rel_text, simple_manifest)

RESIDENCY = os.path.join(ROOT, "config", "residency.json")
DEVICE_H = os.path.join(ROOT, "include", "zephyros", "device.h")
DRIVER_H = os.path.join(ROOT, "include", "zephyros", "driver.h")
MODULE_H = os.path.join(ROOT, "include", "zephyros", "module.h")

_PACK_IMAGE_GLOBALS = (
    "ARENA_BASE", "ARENA_END", "DATA_BASE", "WINDOW_BASE",
    "WINDOW_DATA_BASE", "FIXED_CODE_END", "FIXED_REQUIRED_BYTES",
    "FIXED_OVERFLOW_BYTES", "FIXED_STACKS_BYTES", "FIXED_ROOM_BYTES",
    "SERVICE_STACK_BASE", "SERVICE_STACK_TOP", "SERVICE_STACK_SIZE",
    "GRANULE_SIZE", "GRANULE_COUNT", "REGIONS",
    "BANK7_RUNTIME_END", "IRQ_ISR_STACK_SIZE",
    "ROMFS_FIRST_PAGE", "ROMFS_PAGE_COUNT", "ROMFS_WINDOW_SIZE",
    "ROMFS_CAPACITY", "ROM_ACCESS_BASE", "ROM_ACCESS_SIZE",
)


def parse_c_enums(path, env):
    """Parse plain C enum bodies (name[ = expr], comma separated).

    Inlined from the retired tests/test_m3.py: the device-class
    contract reads the enum values device.h defines alongside its
    #define constants.
    """
    result = {}
    text = open(path).read()
    for match in re.finditer(r"enum\s*\{([^}]*)\}", text):
        value = 0
        for item in match.group(1).split(","):
            item = item.split("/*")[0].split("//")[0].strip()
            if not item:
                continue
            if "=" in item:
                name, expr = [part.strip() for part in item.split("=", 1)]
                value = evaluate(expr, dict(env, **result))
            else:
                name = item
            result[name] = value
            value += 1
    return result


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


# Synthetic two-region geometry matching the maptools fixture and the
# host defaults both layout tools carry: fixed core 2000h-5FFFh (code end
# 206Fh, runtime data at 5A00h), cold half 6000h-DFFFh (code end 6200h,
# runtime data at D800h) and the four-granule arena between them.
SYNTHETIC_GEOMETRY = {
    "kernel_code_end": 0x6200,
    "fixed_code_end": 0x206F,
    "arena_start": 0x6200,
    "arena_end": 0x6A00,
    "granules": 4,
    "data_base": 0x5A00,
    "data_size": 0,
    "window_split": 0x6000,
    "window_code_end": 0x6200,
    "window_data_base": 0xD800,
    "window_data_size": 0,
    "fixed_required_bytes": 0x6F,
    "fixed_overflow_bytes": 0,
    "fixed_stacks_bytes": 0x300,
    "fixed_room_bytes": 0x4000 - 0x6F - 0x300,
    "common_code_size": 0x100,
    "common_data_size": 0x43,
}


def geometry_values(**overrides):
    geometry = dict(SYNTHETIC_GEOMETRY)
    geometry.update(overrides)
    return geometry


def write_geometry_inc(tmp, *keys, **overrides):
    """A generated memory.inc variant for one layout check.

    The named keys are removed afterwards, so a caller can prove that a
    missing derived declaration is a hard error."""
    variant = mapcheck.format_memory_variant(open(MEMORY_INC).read(),
                                             geometry_values(**overrides))
    if keys:
        variant = "\n".join(
            line for line in variant.splitlines()
            if not any(line.startswith(key) for key in keys))
    path = os.path.join(tmp, "geometry.inc")
    with open(path, "w") as handle:
        handle.write(variant)
    return path


class TestAbiContract(unittest.TestCase):
    def setUp(self):
        self.driver_env = parse_c_header(DRIVER_H)
        self.driver_env.update(parse_c_header(DEVICE_H))
        self.device_env = parse_c_header(DEVICE_H)

    def test_abi_version_is_three(self):
        self.assertEqual(evaluate("ZDRIVER_ABI_VERSION", self.driver_env), 3)

    def test_zdriver_api_callback_order(self):
        text = open(DRIVER_H).read()
        self.assertEqual(
            struct_member_order(text, "zdriver_api", function_pointers=True),
            ["device_register", "irq_claim", "irq_release", "device_find",
             "device_interface", "resource_size", "resource_read",
             "fs_register"])

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
                       "resource_size) == 10", "resource_read) == 12",
                       "fs_register) == 14"):
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
        for path in (DRIVER_H, DEVICE_H):
            text = re.sub(r"/\*.*?\*/", "", open(path).read(), flags=re.S)
            self.assertNotIn("romfs_file_t", text, path)
            self.assertNotIn("romfs_lookup", text, path)
            self.assertNotIn("romfs_lookup_ci", text, path)
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
    """The two-region derivation: fixed core, cold half, arena between."""

    def setUp(self):
        self.values = mapcheck.parse_inc(MEMORY_INC)

    def probe_map(self, cold_end=0x6200, fixed_data=0, cold_data=0,
                  fixed_end=0x206F):
        return make_map(
            areas={"_CODE": (0x2000, fixed_end - 0x2000),
                   "_DATA": (0x5A00, fixed_data),
                   "_WINDOW": (0x6000, cold_end - 0x6000),
                   "_WINDOW_DATA": (0xD800, cold_data)},
            symbols={
                "s__CODE": 0x2000, "l__CODE": fixed_end - 0x2000,
                "s__HOME": fixed_end, "l__HOME": 0,
                "s__INITIALIZER": fixed_end, "l__INITIALIZER": 0,
                "s__GSINIT": fixed_end, "l__GSINIT": 0,
                "s__GSFINAL": fixed_end, "l__GSFINAL": 0,
                "s__DATA": 0x5A00, "l__DATA": fixed_data,
                "s__INITIALIZED": 0x5A00 + fixed_data,
                "l__INITIALIZED": 0,
                "s__WINDOW": 0x6000, "l__WINDOW": cold_end - 0x6000,
                "s__WINDOW_DATA": 0xD800, "l__WINDOW_DATA": cold_data,
            })

    def test_derives_the_arena_from_the_cold_code_end(self):
        geometry = mapcheck.derive_geometry(self.probe_map(0x62A2),
                                            self.values)
        self.assertEqual(geometry["kernel_code_end"], 0x62A2)
        self.assertEqual(geometry["window_code_end"], 0x62A2)
        self.assertEqual(geometry["arena_start"], 0x6400)
        # With no cold runtime data the cold data base saturates the half
        # at align_down(E000h, 512) = E000h.
        self.assertEqual(geometry["window_data_base"], 0xE000)
        self.assertEqual(geometry["arena_end"], geometry["window_data_base"])
        self.assertEqual(geometry["granules"], (0xE000 - 0x6400) // 512)

    def test_both_data_bases_derive_from_their_runtime_data_size(self):
        # 1294 bytes of runtime data on each side: the fixed base is
        # align_down(5D00h - 1294, 512) = 5600h and the cold base is
        # align_down(E000h - 1294, 512) = DA00h.
        geometry = mapcheck.derive_geometry(
            self.probe_map(0x62A2, fixed_data=1294, cold_data=1294),
            self.values)
        self.assertEqual(geometry["data_size"], 1294)
        self.assertEqual(geometry["data_base"], 0x5600)
        self.assertEqual(geometry["window_data_size"], 1294)
        self.assertEqual(geometry["window_data_base"], 0xDA00)
        self.assertEqual(geometry["arena_end"], 0xDA00)
        # The fixed-core ledger: required bytes are the linked fixed code,
        # plus the two stack reservations.
        self.assertEqual(geometry["fixed_required_bytes"], 0x6F)
        self.assertEqual(geometry["fixed_stacks_bytes"], 0x300)
        self.assertEqual(geometry["fixed_room_bytes"],
                         0x4000 - 0x6F - 0x300)

    def test_aligned_cold_end_needs_no_padding(self):
        geometry = mapcheck.derive_geometry(self.probe_map(0x6200),
                                            self.values)
        self.assertEqual(geometry["arena_start"], 0x6200)

    def test_fixed_code_past_the_derived_data_base_is_rejected(self):
        # 0x3A00 bytes of fixed runtime data put the derived base at 2200h,
        # below the linked fixed code end 2300h.
        with self.assertRaisesRegex(mapcheck.MapError, "past the derived"):
            mapcheck.derive_geometry(
                self.probe_map(fixed_end=0x2300, fixed_data=0x3A00),
                self.values)

    def test_cold_code_past_the_cold_data_base_is_rejected(self):
        # 0x1000 bytes of cold runtime data put the cold base at D000h,
        # below the linked cold code end D401h.
        with self.assertRaisesRegex(mapcheck.MapError, "past the derived"):
            mapcheck.derive_geometry(self.probe_map(cold_end=0xD401,
                                                    cold_data=0x1000),
                                     self.values)

    def test_geometry_inc_carries_derived_values(self):
        geometry = mapcheck.derive_geometry(self.probe_map(0x62A2),
                                            self.values)
        derived = mapcheck.format_geometry_inc(geometry)
        self.assertIn("ZOS_ARENA_PROBE = 0", derived)
        self.assertIn("ZOS_DRIVER_ARENA_BASE = 0x6400", derived)
        self.assertIn("ZOS_DRIVER_ARENA_END = 0xE000", derived)
        self.assertIn("ZOS_KERNEL_CODE_END = 0x62A2", derived)
        self.assertIn("ZOS_DATA_BASE = 0x5C00", derived)
        probe = mapcheck.format_probe_geometry_inc()
        self.assertIn("ZOS_ARENA_PROBE = 1", probe)
        self.assertNotIn("ZOS_DRIVER_ARENA_BASE", probe)

    def test_memory_variant_replaces_geometry_declarations(self):
        geometry = mapcheck.derive_geometry(
            self.probe_map(0x62A2, fixed_data=1294, cold_data=600),
            self.values)
        variant = mapcheck.format_memory_variant(open(MEMORY_INC).read(),
                                                 geometry)
        self.assertNotIn("ZOS_DRIVER_ARENA_DERIVED", variant)
        self.assertIn("ZOS_DRIVER_ARENA_BASE = 0x6400", variant)
        self.assertIn("ZOS_DRIVER_ARENA_END = 0xDC00", variant)
        self.assertIn("ZOS_DATA_BASE = 0x5600", variant)
        self.assertIn("ZOS_DATA_SIZE = 1294", variant)
        self.assertIn("ZOS_WINDOW_DATA_BASE = 0xDC00", variant)
        self.assertIn("ZOS_FIXED_CODE_END = 0x206F", variant)
        self.assertIn("ZOS_DRIVER_GRANULES = %d" % geometry["granules"],
                      variant)
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "memory.inc")
            with open(path, "w") as handle:
                handle.write(variant)
            parsed = mapcheck.parse_inc(path)
        self.assertEqual(parsed["ZOS_DRIVER_ARENA_BASE"], 0x6400)
        self.assertEqual(parsed["ZOS_DRIVER_ARENA_END"], 0xDC00)
        self.assertEqual(parsed["ZOS_DATA_BASE"], 0x5600)
        self.assertEqual(parsed["ZOS_WINDOW_DATA_BASE"], 0xDC00)
        self.assertEqual(parsed["ZOS_DRIVER_GRANULES"],
                         geometry["granules"])

    def test_mapcheck_rejects_start_not_align_up_of_kernel_end(self):
        # Linked cold end 6100h, arena start 6400h: aligned up is 6200h, so
        # the derived relation must fail.
        text = make_map(
            areas={"_WINDOW": (0x6000, 0x100)},
            symbols={"s__WINDOW": 0x6000, "l__WINDOW": 0x100,
                     "__kernel_code_end": 0x6100,
                     "__driver_arena_start": 0x6400,
                     "__driver_arena_end": 0x6A00})
        with tempfile.TemporaryDirectory() as tmp:
            path = write_geometry_inc(tmp, arena_start=0x6400,
                                      arena_end=0x6A00, granules=3,
                                      kernel_code_end=0x6100,
                                      window_code_end=0x6100)
            with layout_context(path, IRQ_INC):
                with self.assertRaisesRegex(mapcheck.MapError, "align_up"):
                    mapcheck.check_map(text)

    def test_mapcheck_accepts_correctly_derived_relation(self):
        text = make_map(
            areas={"_WINDOW": (0x6000, 0x100)},
            symbols={"s__WINDOW": 0x6000, "l__WINDOW": 0x100,
                     "__kernel_code_end": 0x6100,
                     "__driver_arena_start": 0x6200,
                     "__driver_arena_end": 0x6A00})
        with tempfile.TemporaryDirectory() as tmp:
            path = write_geometry_inc(tmp, arena_start=0x6200,
                                      arena_end=0x6A00, granules=4,
                                      kernel_code_end=0x6100,
                                      window_code_end=0x6100)
            with layout_context(path, IRQ_INC):
                mapcheck.check_map(text)

    def test_mapcheck_rejects_linked_bytes_in_derived_arena(self):
        text = make_map(
            areas={"_VECTOR": (0x6400, 0x100)},
            symbols={"__kernel_code_end": 0x6200,
                     "__driver_arena_start": 0x6200,
                     "__driver_arena_end": 0x6A00})
        with tempfile.TemporaryDirectory() as tmp:
            path = write_geometry_inc(tmp)
            with layout_context(path, IRQ_INC):
                with self.assertRaisesRegex(mapcheck.MapError, "driver arena"):
                    mapcheck.check_map(text)

    def test_mapcheck_common_ceiling_matches_the_platform_description(self):
        values = mapcheck.parse_inc(MEMORY_INC)
        self.assertEqual(mapcheck.COMMON_CEILING, values["ZOS_COMMON_CEILING"])
        self.assertEqual(mapcheck.COMMON_M10_RESERVATION,
                         values["ZOS_COMMON_M10_RESERVATION"])
        self.assertEqual(mapcheck.COMMON_CEILING, 3072)
        self.assertEqual(mapcheck.COMMON_M10_RESERVATION, 512)

    def test_mapcheck_rejects_common_over_the_m9_limit(self):
        # Packed common (0x600 + 0x500 = 2816) exceeds the 2560-byte M9
        # limit (ceiling 3072 minus the 512-byte M10 reservation).
        text = make_map(
            areas={"_COMMON": (0xE000, 0x600),
                   "_COMMON_DATA": (0xE600, 0x500)},
            symbols={"s__COMMON": 0xE000, "l__COMMON": 0x600,
                     "s__COMMON_DATA": 0xE600, "l__COMMON_DATA": 0x500})
        with self.assertRaisesRegex(mapcheck.MapError, "M10 reservation"):
            mapcheck.check_map(text)

    def test_mapcheck_accepts_common_at_the_m9_limit(self):
        # Exactly the limit (0x600 + 0x400 = 2560) passes.
        text = make_map(
            areas={"_COMMON": (0xE000, 0x600),
                   "_COMMON_DATA": (0xE600, 0x400)},
            symbols={"s__COMMON": 0xE000, "l__COMMON": 0x600,
                     "s__COMMON_DATA": 0xE600, "l__COMMON_DATA": 0x400})
        mapcheck.check_map(text)

    def test_missing_arena_end_in_geometry_fails_loudly(self):
        # A geometry include without ZOS_DRIVER_ARENA_END must be rejected
        # by name, never validated against a retired boundary.
        with tempfile.TemporaryDirectory() as tmp:
            path = write_geometry_inc(tmp, "ZOS_DRIVER_ARENA_END")
            with self.assertRaisesRegex(mapcheck.MapError,
                                        "ZOS_DRIVER_ARENA_END"):
                with layout_context(path, IRQ_INC):
                    pass

    def test_pack_image_requires_arena_end_in_geometry(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = write_geometry_inc(tmp, "ZOS_DRIVER_ARENA_END")
            saved = {name: getattr(pack_image, name)
                     for name in _PACK_IMAGE_GLOBALS}
            try:
                with self.assertRaisesRegex(pack_image.LayoutError,
                                            "ZOS_DRIVER_ARENA_END"):
                    pack_image.apply_memory_inc(path)
            finally:
                for name, value in saved.items():
                    setattr(pack_image, name, value)

    def test_pack_image_rejects_bytes_in_derived_arena(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = write_geometry_inc(tmp)
            saved = {name: getattr(pack_image, name)
                     for name in _PACK_IMAGE_GLOBALS}
            try:
                pack_image.apply_memory_inc(path)
                self.assertEqual(pack_image.ARENA_BASE, 0x6200)
                self.assertEqual(pack_image.DATA_BASE, 0x5A00)
                with self.assertRaisesRegex(pack_image.LayoutError,
                                            "driver arena"):
                    pack_image.validate_layout({0x6200: 0x00})
                pack_image.validate_layout({0x2000: 0x11})
            finally:
                for name, value in saved.items():
                    setattr(pack_image, name, value)


class TestResidency(unittest.TestCase):
    """Nucleus residency enforcement (architecture 36.5, package 38).

    `tools/mapcheck.py --residency` is the placement gate: the production
    classification must cover the linked object list exactly, a
    fixed-required object may not compile into the cold areas, and the
    cold-half set must match the objects the build compiles cold."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="zosres-")
        self.objdir = os.path.join(self.tmp, "build")
        os.makedirs(self.objdir)
        self.linker = os.path.join(self.tmp, "zephyr80.lk")
        self.residency = os.path.join(self.tmp, "residency.json")

    def add_object(self, name, fixed=0, cold=0):
        areas = []
        if fixed:
            areas.append(("_CODE", fixed))
        if cold:
            areas.append(("_WINDOW", cold))
        if not areas:
            areas.append(("_CODE", 4))
        with open(os.path.join(self.objdir, "%s.rel" % name), "w") as handle:
            handle.write(rel_text(name, areas,
                                  [("__.ABS.", "def", 0, None)],
                                  [(0, 0, b"\x00" * areas[0][1], [])]))

    def write_linker(self, *names):
        with open(self.linker, "w") as handle:
            handle.write("\n".join("build/%s.rel" % name
                                   for name in names) + "\n")

    def write_residency(self, objects, cold_half=(), test_link=()):
        data = {"format": "zos-residency/1", "objects": objects,
                "cold_half": list(cold_half), "test_link": list(test_link)}
        with open(self.residency, "w") as handle:
            json.dump(data, handle)

    def check(self):
        tiers, cold_half, test_link = mapcheck.load_residency(self.residency)
        mapcheck.check_residency(self.objdir, tiers, cold_half, self.linker,
                                 test_link)

    def test_production_classification_covers_the_linker_list(self):
        tiers, cold_half, test_link = mapcheck.load_residency(RESIDENCY)
        self.assertEqual(sorted(cold_half), sorted(set(cold_half)),
                         "cold_half must not repeat an object")
        production = mapcheck.linker_object_list(
            os.path.join(ROOT, "linker", "zephyr80.lk"))
        # `test_link` objects (the M10 substrate in work package 39) are
        # classified exactly as production will link them in package 41,
        # but the current production link does not list them yet.
        self.assertEqual(sorted(list(production) + list(test_link)),
                         sorted(tiers),
                         "every linked or test-linked object needs exactly "
                         "one tier")
        self.assertEqual(tiers["upcall"], "fixed-required")
        for name in cold_half:
            self.assertIn(name, tiers)
        for name in test_link:
            self.assertIn(name, tiers)

    def test_production_build_matches_the_classification(self):
        # The linked object files must agree with the classification (the
        # check the build itself runs); skipped before the first build.
        objdir = os.path.join(ROOT, "build")
        if not os.path.isdir(objdir):
            self.skipTest("build/ missing; run make first")
        tiers, cold_half, test_link = mapcheck.load_residency(RESIDENCY)
        mapcheck.check_residency(objdir, tiers, cold_half,
                                 os.path.join(ROOT, "linker", "zephyr80.lk"),
                                 test_link)

    def test_missing_classification_entry_fails(self):
        self.add_object("alpha")
        self.write_linker("alpha", "beta")
        self.write_residency([{"object": "alpha", "tier": "fixed-required"}])
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "missing from the residency"):
            self.check()

    def test_fixed_required_object_in_the_cold_half_fails(self):
        self.add_object("alpha", cold=0x40)
        self.write_linker("alpha")
        self.write_residency([{"object": "alpha", "tier": "fixed-required"}])
        with self.assertRaisesRegex(mapcheck.MapError, "fixed-required"):
            self.check()

    def test_cold_half_object_in_the_fixed_areas_fails(self):
        self.add_object("alpha", fixed=0x40)
        self.write_linker("alpha")
        self.write_residency([{"object": "alpha", "tier": "cold"}],
                             cold_half=("alpha",))
        with self.assertRaisesRegex(mapcheck.MapError, "compiles into the fixed"):
            self.check()

    def test_cold_object_outside_the_cold_half_list_fails(self):
        self.add_object("alpha", cold=0x40)
        self.write_linker("alpha")
        self.write_residency([{"object": "alpha", "tier": "cold"}])
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "not place it there"):
            self.check()

    def test_test_link_cold_object_may_compile_cold(self):
        # A test-linked object is linked only by acceptance images, so its
        # placement is not named by production's cold_half list: the tier
        # decides, exactly as it will once production links it.
        self.add_object("alpha", cold=0x40)
        self.write_linker()
        self.write_residency([{"object": "alpha", "tier": "cold"}],
                             test_link=("alpha",))
        self.check()

    def test_test_link_fixed_object_in_the_cold_half_fails(self):
        self.add_object("alpha", cold=0x40)
        self.write_linker()
        self.write_residency([{"object": "alpha", "tier": "fixed-required"}],
                             test_link=("alpha",))
        with self.assertRaisesRegex(mapcheck.MapError, "fixed-required"):
            self.check()

    def test_cold_half_may_not_name_a_fixed_required_object(self):
        # cold_half is production's compile-cold set: naming a
        # fixed-required object there would compile it into the window.
        self.write_residency([{"object": "alpha", "tier": "fixed-required"}],
                             cold_half=("alpha",))
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "cold_half names fixed-required"):
            mapcheck.load_residency(self.residency)

    def test_moves_entry_needs_a_stage(self):
        self.write_residency([{"object": "alpha", "tier": "moves"}])
        with self.assertRaisesRegex(mapcheck.MapError, "moves without a stage"):
            mapcheck.load_residency(self.residency)

    def test_overflow_is_allowed_in_the_fixed_core(self):
        # A cold-tier object that the build does not compile cold may use
        # leftover fixed space: that is the ledger's `overflow`, not an
        # error (architecture 36.5, requirement 2).
        self.add_object("alpha", fixed=0x40)
        self.write_linker("alpha")
        self.write_residency([{"object": "alpha", "tier": "moves",
                               "stage": 4}])
        self.check()


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

    def write_geometry(self, tmp, granules):
        """A generated geometry include for the plan tests: the arena runs
        from the aligned cold code end to the derived cold data base."""
        path = os.path.join(tmp, "geometry.inc")
        start = 0xD800 - granules * 512
        code_end = 0x6000 + 0x40
        data_size = 16
        with open(path, "w") as handle:
            handle.write("ZOS_BANK7_BASE = 0x2000\n"
                         "ZOS_REV13_WINDOW_BASE = 0x6000\n"
                         "ZOS_BANK7_RUNTIME_END = 0xE000\n"
                         "ZOS_IRQ_ISR_STACK_SIZE = 0x0100\n"
                         "ZOS_SERVICE_STACK_BASE = 0x5D00\n"
                         "ZOS_SERVICE_STACK_TOP = 0x5F00\n"
                         "ZOS_SERVICE_STACK_SIZE = 0x0200\n"
                         "ZOS_ARENA_PROBE = 0\n"
                         "ZOS_KERNEL_CODE_END = 0x%04X\n"
                         "ZOS_WINDOW_CODE_END = 0x%04X\n"
                         "ZOS_DRIVER_ARENA_BASE = 0x%04X\n"
                         "ZOS_DRIVER_ARENA_END = 0xD800\n"
                         "ZOS_DRIVER_GRANULE_SIZE = 0x0200\n"
                         "ZOS_DRIVER_GRANULES = %d\n"
                         "ZOS_DATA_BASE = 0x5A00\n"
                         "ZOS_DATA_SIZE = 0\n"
                         "ZOS_FIXED_CODE_END = 0x206F\n"
                         "ZOS_FIXED_REQUIRED_BYTES = 0x6F\n"
                         "ZOS_FIXED_OVERFLOW_BYTES = 0\n"
                         "ZOS_FIXED_STACKS_BYTES = 0x300\n"
                         "ZOS_FIXED_ROOM_BYTES = 0x3C91\n"
                         "ZOS_WINDOW_DATA_BASE = 0xD800\n"
                         "ZOS_WINDOW_DATA_SIZE = %d\n"
                         "ZOS_COMMON_BASE = 0xE000\n"
                         "ZOS_COMMON_CODE_END = 0xFD00\n"
                         "ZOS_COMMON_CEILING = 0x0C00\n"
                         "ZOS_COMMON_M10_RESERVATION = 0x0200\n"
                         "ZOS_COMMON_CODE_SIZE = 0x100\n"
                         "ZOS_COMMON_DATA_SIZE = 0x43\n"
                         % (code_end, code_end, start, granules, data_size))
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
            ledger = plan["nucleus"]
            self.assertEqual(ledger["cold_code_end"], 0x6040)
            self.assertEqual(ledger["cold_code_bytes"], 0x40)
            self.assertEqual(ledger["cold_arena_granules"], 32)
            self.assertEqual(ledger["cold_arena_used_granules"], 2)
            self.assertEqual(ledger["cold_data_bytes"], 16)
            self.assertEqual(ledger["cold_data_base"], 0xD800)
            # Cold room excludes the arena: the arena is reported by its
            # granules, never twice.
            self.assertEqual(ledger["cold_room_bytes"],
                             0x8000 - 0x40 - 16 - 32 * 512)
            self.assertEqual(ledger["fixed_code_end"], 0x206F)
            self.assertEqual(ledger["fixed_required_bytes"], 0x6F)
            self.assertEqual(ledger["fixed_overflow_bytes"], 0)
            self.assertEqual(ledger["fixed_stacks_bytes"], 0x300)
            self.assertEqual(ledger["fixed_room_bytes"], 0x3C91)
            self.assertEqual(ledger["service_hot_bytes"], 0)
            self.assertEqual(ledger["service_boot_bytes"], 0)
            self.assertEqual(ledger["common_code_bytes"], 0x100)
            self.assertEqual(ledger["common_data_bytes"], 0x43)
            self.assertEqual(ledger["common_used_bytes"], 0x143)
            self.assertEqual(ledger["common_capacity_bytes"],
                             0xFD00 - 0xE000)
            text = out.getvalue()
            self.assertIn("nucleus fixed: required 111 bytes", text)
            self.assertIn("stacks 768 | room 15505 bytes", text)
            self.assertIn("nucleus cold: code 64 bytes end 6040h", text)
            self.assertIn("arena 32 granules (2 boot, 30 free)", text)
            self.assertIn("data 16 bytes at D800h | room 16304 bytes", text)
            self.assertIn("service hot: 0 bytes of 32768", text)
            self.assertIn("service boot/bulk: 0 bytes of 24576", text)
            self.assertIn("ceiling 3072, m10 512, limit 2560", text)

    def test_plan_requires_the_geometry_ledger_inputs(self):
        # The ledger must never invent a missing derived declaration.
        with tempfile.TemporaryDirectory() as tmp:
            module = self.write_module(tmp, "boot", 512)
            geometry = self.write_geometry(tmp, 32)
            text = open(geometry).read()
            kept = [line for line in text.splitlines()
                    if not line.startswith("ZOS_KERNEL_CODE_END")]
            with open(geometry, "w") as handle:
                handle.write("\n".join(kept) + "\n")
            with self.assertRaisesRegex(zmodpack.ZmodError,
                                        "ZOS_KERNEL_CODE_END"):
                with contextlib.redirect_stdout(io.StringIO()):
                    zmodpack.cmd_plan(["--geometry", geometry, module])

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
                handle.write("ZOS_DRIVER_ARENA_BASE = 0x8000\n"
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

    def test_memory_inc_declares_only_the_build_contract(self):
        values = mapcheck.parse_inc(MEMORY_INC)
        self.assertEqual(values["ZOS_BANK7_BASE"], 0x2000)
        self.assertEqual(values["ZOS_REV13_WINDOW_BASE"], 0x6000)
        self.assertEqual(values["ZOS_SERVICE_STACK_SIZE"], 0x200)
        self.assertEqual(values["ZOS_IRQ_ISR_STACK_SIZE"], 0x100)
        self.assertEqual(values["ZOS_DRIVER_GRANULE_SIZE"], 512)
        # There is no granule floor any more: the arena only has to hold
        # the declared boot set, which the plan and the packer enforce.
        for retired in ("ZOS_BANK7_LOAD_END", "ZOS_DRIVER_ARENA_MIN_SIZE",
                        "ZOS_DRIVER_ARENA_MIN_GRANULES"):
            self.assertNotIn(retired, values, retired)
        # The arena base/end/granules and both data bases exist only in
        # the generated geometry variant; the probe link places no arena.
        for name in ("ZOS_DRIVER_ARENA_DERIVED", "ZOS_DRIVER_ARENA_BASE",
                     "ZOS_DRIVER_ARENA_END", "ZOS_DRIVER_GRANULES",
                     "ZOS_DATA_BASE", "ZOS_WINDOW_DATA_BASE",
                     "ZOS_FIXED_CODE_END", "ZOS_FIXED_REQUIRED_BYTES"):
            self.assertNotIn(name, values, name)


INTEGRATION_FIXTURE_MANIFESTS = {
    "manifest-fallback-arena.json":
        os.path.join(ROOT, "tests", "console", "manifest-fallback-arena.json"),
    "manifest-fallback-nohid.json":
        os.path.join(ROOT, "tests", "console", "manifest-fallback-nohid.json"),
    "manifest-fallback-nohid-nov9958.json":
        os.path.join(ROOT, "tests", "console",
                     "manifest-fallback-nohid-nov9958.json"),
}


def _read_json(path):
    with open(path) as handle:
        return json.load(handle)


# Every subsystem fixture manifest carries the required media types; the
# failure variants substitute fixture bytes only for named slots or drop
# only the module(s) whose absence is the point of the variant.
INTEGRATION_REQUIRED_TYPES = {
    "sio.zmod": "ZMOD",
    "ioc.zmod": "ZMOD",
    "hid.zmod": "ZMOD",
    "v9958.zmod": "ZMOD",
    "console.zmod": "ZMOD",
    "iocfat.zmod": "ZMOD",
    "services.man": "DATA",
    "monitor.zex": "ZEX",
}
# Package 41: the five driver services are the store's boot set (SIO,
# IOC, HID, V9958, console) and the boot manifest lists them in load order.
INTEGRATION_SERVICE_SET = ["sio.zmod", "ioc.zmod", "hid.zmod", "v9958.zmod",
                           "console.zmod", "services.man"]
INTEGRATION_FULL_SET = INTEGRATION_SERVICE_SET + ["monitor.zex"]
# Package 30: the production store also carries the resident IOC-FAT
# provider, loaded last (before the monitor).
INTEGRATION_PRODUCTION_SET = INTEGRATION_SERVICE_SET + ["iocfat.zmod",
                                                        "monitor.zex"]


class TestManifests(unittest.TestCase):
    def check(self, path, names):
        manifest = _read_json(path)
        self.assertEqual(manifest["format"], "romfs-v1")
        self.assertEqual([entry["name"] for entry in manifest["files"]],
                         names)
        for entry in manifest["files"]:
            self.assertEqual(entry["type"], INTEGRATION_REQUIRED_TYPES[entry["name"]],
                             entry["name"])
        index = {entry["name"]: i
                 for i, entry in enumerate(manifest["files"])}
        self.assertEqual(index["sio.zmod"], 0,
                         "SIO is the required first boot module")
        if "hid.zmod" in index:
            self.assertLess(index["ioc.zmod"], index["hid.zmod"],
                            "IOC must be bootable before HID")
        if "v9958.zmod" in index:
            self.assertLess(index["v9958.zmod"], index["monitor.zex"])
        self.assertEqual(index["monitor.zex"], len(names) - 1,
                         "the monitor launcher is last")

    def test_production_and_failure_manifests(self):
        cases = (
            (os.path.join(ROOT, "recovery", "manifest.json"),
             INTEGRATION_PRODUCTION_SET),
            # Each fallback variant is a service-set subset now: the console
            # composes whatever the store booted.
            (INTEGRATION_FIXTURE_MANIFESTS["manifest-fallback-arena.json"],
             ["sio.zmod", "ioc.zmod", "hid.zmod", "console.zmod",
              "services.man", "v9958.zmod", "monitor.zex"]),
            (INTEGRATION_FIXTURE_MANIFESTS["manifest-fallback-nohid.json"],
             ["sio.zmod", "ioc.zmod", "v9958.zmod", "console.zmod",
              "services.man", "monitor.zex"]),
            (INTEGRATION_FIXTURE_MANIFESTS["manifest-fallback-nohid-nov9958.json"],
             ["sio.zmod", "ioc.zmod", "console.zmod", "services.man",
              "monitor.zex"]),
        )
        for path, names in cases:
            with self.subTest(manifest=os.path.basename(path)):
                self.check(path, names)

    def test_production_manifest_names_are_the_artifacts(self):
        manifest = _read_json(os.path.join(ROOT, "recovery",
                                           "manifest.json"))
        for entry in manifest["files"]:
            self.assertEqual(entry["path"], entry["name"],
                             "production manifests must not substitute "
                             "fixture bytes")

    def test_failure_manifests_substitute_only_named_slots(self):
        # Package 41: the font is the video image's bulk segment and the
        # IOC link is the IOC service's business, so the two M8 variants
        # that injected fixture bytes for them are gone.  What remains is
        # the arena variant, whose `v9958.zmod` slot carries the
        # exhaustion fixture (the load fails before any service byte runs).
        substitutions = {
            "manifest-fallback-arena.json": {
                "v9958.zmod": "arena_fill.zmod"},
            "manifest-fallback-nohid.json": {},
            "manifest-fallback-nohid-nov9958.json": {},
        }
        for name, mapping in substitutions.items():
            manifest = _read_json(INTEGRATION_FIXTURE_MANIFESTS[name])
            paths = {entry["name"]: entry["path"]
                     for entry in manifest["files"]}
            for slot, path in mapping.items():
                self.assertEqual(paths[slot], path, name)
            for slot, path in paths.items():
                if slot not in mapping:
                    self.assertEqual(path, slot,
                                     "%s: unexpected substitution" % name)

    def test_makefile_acceptance_targets_and_flags(self):
        with open(os.path.join(ROOT, "Makefile")) as handle:
            makefile = handle.read()
        for target in ("console-integration-selftest:",
                       "console-fallback-selftests:",
                       "smoke-hardware-console-integration:",
                       "smoke-hardware-console-fb-nohid:",
                       "smoke-hardware-console-fb-nohid-nov9958:",
                       "smoke-hardware-console-fb-badioc:",
                       "smoke-hardware-console-fb-arena:"):
            self.assertIn(target, makefile)
        for flag in ("ZOS_CONSOLE_INTEGRATION_SELFTEST", "ZOS_CONSOLE_FALLBACK_SELFTEST",
                     "ZOS_CONSOLE_FALLBACK"):
            self.assertIn(flag, makefile)

    def test_target_runner_wires_package20_tests(self):
        with open(os.path.join(ROOT, "tests", "target", "support",
                               "target_test.c")) as handle:
            runner = handle.read()
        self.assertIn('#include "console_integration_selftest.h"', runner)
        self.assertIn('#include "console_fallback_selftest.h"', runner)
        self.assertIn("zos_run_console_integration_selftest()", runner)
        self.assertIn("zos_run_console_fallback_selftest()", runner)

    def test_serial_smoke_modes_classify_the_matrix(self):
        import serial_smoke as tool
        self.assertEqual(
            sorted(tool.FALLBACK_MODES),
            ["console-fb-arena", "console-fb-badioc", "console-fb-nohid",
             "console-fb-nohid-nov9958"])
        for mode in tool.FALLBACK_MODES:
            self.assertIn(mode, tool.__doc__)
        base = (tool.BANNER +
                b"hid: unavailable 0x05\r\n" +
                b"console fallback: input sio0.b class 01\r\n" +
                b"console fallback: output v9958.0 class 02\r\n" +
                b"console fallback: devices 3\r\n" +
                b"console fallback: send input\r\n" +
                b"console fallback: input OK 0x52\r\n" +
                b"console fallback: console write OK\r\n" +
                b"console fallback selftest: OK\r\n")
        self.assertTrue(tool._fallback_result("console-fb-nohid", base))
        self.assertFalse(tool._fallback_result(
            "console-fb-nohid", base + tool.FALLBACK_CONSOLE_MARKER))
        self.assertFalse(tool._fallback_result(
            "console-fb-nohid", base.replace(b"v9958.0", b"sio0.b")))
        self.assertFalse(tool._fallback_result(
            "console-fb-nohid", base.replace(b"input OK 0x52", b"input none")))
        integration = (tool.BANNER +
                       b"console integration: input hid.keyboard0 class 04\r\n" +
                       b"console integration: output v9958.0 class 02\r\n" +
                       b"console integration: devices 04\r\n" +
                       b"console integration: ioc level 4A\r\n" +
                       b"console integration: rejected calls OK\r\n" +
                       b"console integration: OK\r\n")
        self.assertTrue(tool._integration_result(integration))
        self.assertFalse(tool._integration_result(
            integration + b"hid: unavailable 0x05\r\n"))
        self.assertIn("console-fb-nohid", tool.__doc__)


if __name__ == "__main__":
    unittest.main()
