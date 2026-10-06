#!/usr/bin/env python3
"""Host-side M8 tests for work package 14-m8-driver-abi-arena.

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

DEVICE_H = os.path.join(ROOT, "include", "zephyros", "device.h")
DRIVER_H = os.path.join(ROOT, "include", "zephyros", "driver.h")
MODULE_H = os.path.join(ROOT, "include", "zephyros", "module.h")
CONSOLE_H = os.path.join(ROOT, "include", "zephyros", "console.h")

_PACK_IMAGE_GLOBALS = (
    "ARENA_BASE", "ARENA_END", "DATA_BASE", "ARENA_MIN_SIZE",
    "ARENA_MIN_GRANULES", "GRANULE_SIZE", "GRANULE_COUNT", "REGIONS",
    "BANK7_RUNTIME_END", "IRQ_ISR_STACK_SIZE",
    "ROMFS_FIRST_PAGE", "ROMFS_PAGE_COUNT", "ROMFS_WINDOW_SIZE",
    "ROMFS_CAPACITY", "ROM_ACCESS_BASE", "ROM_ACCESS_SIZE",
)


def parse_c_enums(path, env):
    """Parse plain C enum bodies (name[ = expr], comma separated).

    Inlined from the retired tests/test_m3.py: the M8 device-class
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


@contextlib.contextmanager
def derived_arena_context(start, granules, data_base=0xC000):
    """Apply a synthetic derived geometry for one mapcheck call.

    Synthetic maps keep runtime data at C000h, so the arena end (which is
    the derived data base) is C000h unless a caller says otherwise."""
    saved = {name: getattr(mapcheck, name) for name in
             ("DRIVER_ARENA_BASE", "DRIVER_ARENA_END", "DATA_BASE",
              "DRIVER_GRANULE_SIZE", "DRIVER_GRANULES", "DRIVER_ARENA_MIN_SIZE",
              "DRIVER_ARENA_MIN_GRANULES", "OWNED_AREAS")}
    try:
        mapcheck.DRIVER_ARENA_BASE = start
        mapcheck.DRIVER_ARENA_END = data_base
        mapcheck.DATA_BASE = data_base
        mapcheck.DRIVER_GRANULE_SIZE = 0x200
        mapcheck.DRIVER_GRANULES = granules
        mapcheck.DRIVER_ARENA_MIN_SIZE = 0x4000
        mapcheck.DRIVER_ARENA_MIN_GRANULES = 32
        owned = dict(mapcheck.OWNED_AREAS)
        for name in mapcheck.LOADABLE_AREAS:
            owned[name] = (mapcheck.BANK7_BASE, start)
        for name in ("_DATA", "_INITIALIZED"):
            owned[name] = (data_base, mapcheck.BANK7_RUNTIME_END)
        mapcheck.OWNED_AREAS = owned
        yield
    finally:
        for name, value in saved.items():
            setattr(mapcheck, name, value)


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
        for path in (DRIVER_H, DEVICE_H, CONSOLE_H):
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
    def setUp(self):
        self.values = mapcheck.parse_inc(MEMORY_INC)

    def probe_map(self, end, data_size=0):
        return make_map(
            areas={"_CODE": (0x2000, end - 0x2000)},
            symbols={
                "s__CODE": 0x2000, "l__CODE": end - 0x2000,
                "s__HOME": end, "l__HOME": 0,
                "s__INITIALIZER": end, "l__INITIALIZER": 0,
                "s__GSINIT": end, "l__GSINIT": 0,
                "s__GSFINAL": end, "l__GSFINAL": 0,
                "s__DATA": 0xC000, "l__DATA": data_size,
                "s__INITIALIZED": 0xC000 + data_size,
                "l__INITIALIZED": 0,
            })

    def test_derives_aligned_start_and_end(self):
        geometry = mapcheck.derive_geometry(self.probe_map(0x5CA2),
                                            self.values)
        self.assertEqual(geometry["kernel_code_end"], 0x5CA2)
        self.assertEqual(geometry["arena_start"], 0x5E00)
        # With no runtime data, the data base is the granule-aligned
        # DF00h floor: align_down(DF00h, 512) = DE00h.
        self.assertEqual(geometry["data_base"], 0xDE00)
        self.assertEqual(geometry["arena_end"], geometry["data_base"])
        self.assertEqual(geometry["granules"], (0xDE00 - 0x5E00) // 512)

    def test_data_base_derives_from_runtime_data_size(self):
        # 1294 bytes of runtime data: align_down(DF00h - 1294, 512) = D800h.
        geometry = mapcheck.derive_geometry(
            self.probe_map(0x5CA2, data_size=1294), self.values)
        self.assertEqual(geometry["data_size"], 1294)
        self.assertEqual(geometry["data_base"], 0xD800)
        self.assertEqual(geometry["arena_end"], 0xD800)

    def test_aligned_end_needs_no_padding(self):
        geometry = mapcheck.derive_geometry(self.probe_map(0x5E00),
                                            self.values)
        self.assertEqual(geometry["arena_start"], 0x5E00)

    def test_below_minimum_arena_is_rejected(self):
        # Data base DE00h and kernel end DC01h leave less than the floor.
        with self.assertRaisesRegex(mapcheck.MapError, "minimum"):
            mapcheck.derive_geometry(self.probe_map(0xDC01), self.values)

    def test_kernel_end_past_data_base_is_rejected(self):
        with self.assertRaisesRegex(mapcheck.MapError, "past the derived"):
            mapcheck.derive_geometry(self.probe_map(0xDE01), self.values)

    def test_geometry_inc_carries_derived_values(self):
        geometry = mapcheck.derive_geometry(self.probe_map(0x5CA2),
                                            self.values)
        derived = mapcheck.format_geometry_inc(geometry)
        self.assertIn("ZOS_ARENA_PROBE = 0", derived)
        self.assertIn("ZOS_DRIVER_ARENA_BASE = 0x5E00", derived)
        self.assertIn("ZOS_DRIVER_ARENA_END = 0xDE00", derived)
        self.assertIn("ZOS_DATA_BASE = 0xDE00", derived)
        probe = mapcheck.format_probe_geometry_inc()
        self.assertIn("ZOS_ARENA_PROBE = 1", probe)
        self.assertNotIn("ZOS_DRIVER_ARENA_BASE", probe)

    def test_memory_variant_replaces_geometry_declarations(self):
        geometry = mapcheck.derive_geometry(
            self.probe_map(0x5CA2, data_size=1294), self.values)
        variant = mapcheck.format_memory_variant(open(MEMORY_INC).read(),
                                                 geometry)
        self.assertNotIn("ZOS_DRIVER_ARENA_DERIVED", variant)
        self.assertIn("ZOS_DRIVER_ARENA_BASE = 0x5E00", variant)
        self.assertIn("ZOS_DRIVER_ARENA_END = 0xD800", variant)
        self.assertIn("ZOS_DATA_BASE = 0xD800", variant)
        self.assertIn("ZOS_DATA_SIZE = 1294", variant)
        self.assertIn("ZOS_DRIVER_GRANULES = "
                      "%d" % geometry["granules"], variant)
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "memory.inc")
            with open(path, "w") as handle:
                handle.write(variant)
            parsed = mapcheck.parse_inc(path)
        self.assertEqual(parsed["ZOS_DRIVER_ARENA_BASE"], 0x5E00)
        self.assertEqual(parsed["ZOS_DRIVER_ARENA_END"], 0xD800)
        self.assertEqual(parsed["ZOS_DATA_BASE"], 0xD800)
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

    def _geometry_without(self, key, tmp):
        """Real memory include with one derived declaration removed."""
        geometry = {"kernel_code_end": 0x226F, "arena_start": 0x2400,
                    "arena_end": 0x2E00, "granules": 5,
                    "data_base": 0x2E00, "data_size": 0,
                    "common_code_size": 0x100, "common_data_size": 0x43}
        variant = mapcheck.format_memory_variant(open(MEMORY_INC).read(),
                                                 geometry)
        kept = [line for line in variant.splitlines()
                if not line.startswith(key)]
        path = os.path.join(tmp, "geometry.inc")
        with open(path, "w") as handle:
            handle.write("\n".join(kept) + "\n")
        return path

    def test_missing_arena_end_in_geometry_fails_loudly(self):
        # A geometry include without ZOS_DRIVER_ARENA_END must be rejected
        # by name, never validated against the retired C000h boundary.
        with tempfile.TemporaryDirectory() as tmp:
            path = self._geometry_without("ZOS_DRIVER_ARENA_END", tmp)
            with self.assertRaisesRegex(mapcheck.MapError,
                                        "ZOS_DRIVER_ARENA_END"):
                with layout_context(path, IRQ_INC):
                    pass

    def test_pack_image_requires_arena_end_in_geometry(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = self._geometry_without("ZOS_DRIVER_ARENA_END", tmp)
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
        geometry = {"kernel_code_end": 0x226F, "arena_start": 0x2400,
                    "arena_end": 0x2E00, "granules": 5,
                    "data_base": 0x2E00, "data_size": 0,
                    "common_code_size": 0x100, "common_data_size": 0x43}
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
                self.assertEqual(pack_image.DATA_BASE, 0x2E00)
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

    def write_geometry(self, tmp, granules):
        path = os.path.join(tmp, "geometry.inc")
        start = 0xC000 - granules * 512
        code_end = 0x2000 + 0x40
        data_size = 16
        with open(path, "w") as handle:
            handle.write("ZOS_BANK7_BASE = 0x2000\n"
                         "ZOS_BANK7_RUNTIME_END = 0xDF00\n"
                         "ZOS_IRQ_ISR_STACK_SIZE = 0x0100\n"
                         "ZOS_ARENA_PROBE = 0\n"
                         "ZOS_KERNEL_CODE_END = 0x%04X\n"
                         "ZOS_DRIVER_ARENA_BASE = 0x%04X\n"
                         "ZOS_DRIVER_ARENA_END = 0xC000\n"
                         "ZOS_DRIVER_GRANULE_SIZE = 0x0200\n"
                         "ZOS_DRIVER_GRANULES = %d\n"
                         "ZOS_DATA_BASE = 0xC000\n"
                         "ZOS_DATA_SIZE = %d\n"
                         "ZOS_COMMON_BASE = 0xE000\n"
                         "ZOS_COMMON_CODE_END = 0xFD00\n"
                         "ZOS_COMMON_CODE_SIZE = 0x100\n"
                         "ZOS_COMMON_DATA_SIZE = 0x43\n"
                         % (code_end, start, granules, data_size))
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
            ledger = plan["bank7"]
            self.assertEqual(ledger["code_bytes"], 0x40)
            self.assertEqual(ledger["code_end"], 0x2040)
            self.assertEqual(ledger["arena_granules"], 32)
            self.assertEqual(ledger["arena_used_granules"], 2)
            self.assertEqual(ledger["data_bytes"], 16)
            self.assertEqual(ledger["data_base"], 0xC000)
            self.assertEqual(ledger["isr_bytes"], 256)
            self.assertEqual(ledger["slack_bytes"], 0xDF00 - (0xC000 + 16))
            self.assertEqual(ledger["common_code_bytes"], 0x100)
            self.assertEqual(ledger["common_data_bytes"], 0x43)
            self.assertEqual(ledger["common_used_bytes"], 0x143)
            self.assertEqual(ledger["common_capacity_bytes"],
                             0xFD00 - 0xE000)
            self.assertIn(
                "bank7: code 64 bytes end 2040h | arena 32 granules "
                "(2 boot, 30 free) | data 16 bytes at C000h | isr 256 | "
                "slack 7920 bytes",
                out.getvalue())
            self.assertIn(
                "common: used 323 of 7424 bytes (code 256, data 67)",
                out.getvalue())

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
        self.assertEqual(values["ZOS_DRIVER_ARENA_MIN_SIZE"], 0x4000)
        self.assertEqual(values["ZOS_DRIVER_ARENA_MIN_GRANULES"], 32)
        self.assertEqual(values["ZOS_DRIVER_GRANULE_SIZE"], 512)
        # The arena base/end/granules and the data base exist only in the
        # generated geometry variant; the probe link places no arena.
        for name in ("ZOS_DRIVER_ARENA_DERIVED", "ZOS_DRIVER_ARENA_BASE",
                     "ZOS_DRIVER_ARENA_END", "ZOS_DRIVER_GRANULES",
                     "ZOS_DATA_BASE"):
            self.assertNotIn(name, values, name)

    def test_generated_production_geometry_matches_manifest(self):
        geometry_inc = os.path.join(ROOT, "build", "geometry",
                                    "arena_geometry.inc")
        manifest_path = os.path.join(ROOT, "build",
                                     "zephyros.manifest.json")
        if not (os.path.exists(geometry_inc)
                and os.path.exists(manifest_path)):
            self.skipTest("derived production build missing; run make")
        values = mapcheck.parse_inc(geometry_inc)
        self.assertEqual(values["ZOS_ARENA_PROBE"], 0)
        manifest = _read_json(manifest_path)
        arena = manifest["arena"]
        self.assertEqual(values["ZOS_KERNEL_CODE_END"],
                         manifest["entries"]["__kernel_code_end"])
        self.assertEqual(values["ZOS_DRIVER_ARENA_BASE"], arena["start"])
        self.assertEqual(values["ZOS_DRIVER_ARENA_END"], arena["end"])
        self.assertEqual(values["ZOS_DATA_BASE"], arena["end"])
        self.assertGreaterEqual(arena["granules"], 32)
        # The arena is the aligned-up linked kernel end and ends exactly at
        # the derived runtime data base, keeping the 16-KiB minimum.
        kernel_end = manifest["entries"]["__kernel_code_end"]
        self.assertEqual(arena["start"], (kernel_end + 0x1FF) & ~0x1FF,
                         "arena start is not align_up(kernel end, 512)")
        self.assertEqual(arena["end"], manifest["layout"]["data_base"])
        self.assertEqual(arena["end"], manifest["entries"]["s__DATA"])
        self.assertGreaterEqual(arena["size"], 0x4000)
        self.assertEqual(arena["granules"], arena["size"] // 512)
        self.assertEqual(arena["granule_size"], 512)

    def test_bank7_ledger_matches_map_and_manifest(self):
        map_path = os.path.join(ROOT, "build", "zephyros.map")
        manifest_path = os.path.join(ROOT, "build",
                                     "zephyros.manifest.json")
        plan_path = os.path.join(ROOT, "build", "arena.plan.json")
        if not (os.path.exists(map_path) and os.path.exists(manifest_path)
                and os.path.exists(plan_path)):
            self.skipTest("derived production build missing; run make")
        symbols = pack_image.parse_map_symbols(open(map_path).read())
        manifest = _read_json(manifest_path)
        plan = _read_json(plan_path)
        ledger = manifest["bank7"]
        self.assertEqual(ledger, plan["bank7"])
        self.assertEqual(ledger["code_bytes"],
                         symbols["__kernel_code_end"] - 0x2000)
        self.assertEqual(ledger["code_end"], symbols["__kernel_code_end"])
        self.assertEqual(ledger["arena_start"],
                         symbols["__driver_arena_start"])
        self.assertEqual(ledger["arena_end"], symbols["__driver_arena_end"])
        self.assertEqual(ledger["data_base"], symbols["s__DATA"])
        self.assertEqual(ledger["data_bytes"],
                         symbols["l__DATA"] + symbols["l__INITIALIZED"])
        self.assertEqual(ledger["isr_bytes"], 256)
        self.assertEqual(
            ledger["slack_bytes"],
            0xDF00 - (symbols["s__DATA"] + symbols["l__DATA"]
                      + symbols["l__INITIALIZED"]))

        # Common memory is reported in the same ledger: the packed
        # `_COMMON` + `_COMMON_DATA` run against the permanently mapped
        # window capacity.  Both halves must come from the map.
        self.assertEqual(ledger["common_code_bytes"], symbols["l__COMMON"])
        self.assertEqual(ledger["common_data_bytes"],
                         symbols["l__COMMON_DATA"])
        self.assertEqual(ledger["common_used_bytes"],
                         symbols["l__COMMON"] + symbols["l__COMMON_DATA"])
        self.assertEqual(ledger["common_capacity_bytes"], 0xFD00 - 0xE000)
        self.assertEqual(
            ledger["common_used_bytes"],
            symbols["s__COMMON_DATA"] + symbols["l__COMMON_DATA"]
            - symbols["s__COMMON"],
            "the two common areas must be one contiguous run")

        self.assertEqual(pack_image.format_bank7_ledger(ledger),
                         zmodpack.format_bank7_ledger(plan["bank7"]))
        text = pack_image.format_bank7_ledger(ledger)
        self.assertRegex(
            text,
            r"^bank7: code \d+ bytes end [0-9A-F]{4}h \| arena \d+ granules "
            r"\(\d+ boot, \d+ free\) \| data \d+ bytes at [0-9A-F]{4}h \| "
            r"isr 256 \| slack \d+ bytes\n"
            r"common: used \d+ of \d+ bytes \(code \d+, data \d+\)$")

    def test_production_plan_reconciles_with_packed_modules(self):
        plan_path = os.path.join(ROOT, "build", "arena.plan.json")
        manifest_path = os.path.join(ROOT, "build",
                                     "zephyros.manifest.json")
        if not (os.path.exists(plan_path)
                and os.path.exists(manifest_path)):
            self.skipTest("derived production build missing; run make")
        plan = _read_json(plan_path)
        manifest = _read_json(manifest_path)
        self.assertEqual([entry["name"] for entry in plan["modules"]],
                         ["sio", "ioc", "hid", "v9958", "iocfat"],
                         "the production boot modules must all fit")
        used = 0
        for entry in plan["modules"]:
            with open(entry["path"], "rb") as handle:
                data = handle.read()
            info = zmodpack.parse_container(data, entry["path"])
            self.assertEqual(entry["image_size"], info["image_size"])
            self.assertEqual(entry["bss_size"], info["bss_size"])
            self.assertEqual(entry["total_size"],
                             info["image_size"] + info["bss_size"])
            self.assertEqual(entry["granules"], info["required_granules"])
            used += entry["granules"]
        arena = plan["arena"]
        self.assertEqual(arena["used_granules"], used)
        self.assertEqual(arena["free_granules"],
                         arena["granules"] - used)
        self.assertGreaterEqual(arena["free_granules"], 0)
        # The packed image report carries the same boot-set plan, and the
        # arena never reaches into the runtime data at the derived base.
        self.assertEqual(manifest["arena"]["modules"], plan["modules"])
        self.assertEqual(manifest["arena"]["end"], plan["arena"]["end"])
        self.assertEqual(manifest["arena"]["end"], manifest["layout"]["data_base"])
        self.assertEqual(manifest["arena"]["granule_size"], 512)


M8_MANIFEST_DIR = os.path.join(ROOT, "tests", "m8")


def _read_json(path):
    with open(path) as handle:
        return json.load(handle)


# Package 20: every M8 manifest carries the required media types; the
# failure variants substitute fixture bytes only for named slots or drop
# only the module(s) whose absence is the point of the variant.
M8_REQUIRED_TYPES = {
    "sio.zmod": "ZMOD",
    "ioc.zmod": "ZMOD",
    "hid.zmod": "ZMOD",
    "v9958.zmod": "ZMOD",
    "iocfat.zmod": "ZMOD",
    "v9958-6x8.font": "DATA",
    "monitor.zex": "ZEX",
}
M8_FULL_SET = ["sio.zmod", "ioc.zmod", "hid.zmod", "v9958.zmod",
               "v9958-6x8.font", "monitor.zex"]
# Package 30: the production store also carries the resident IOC-FAT
# provider, loaded last (before the monitor).
M8_PRODUCTION_SET = ["sio.zmod", "ioc.zmod", "hid.zmod", "v9958.zmod",
                     "iocfat.zmod", "v9958-6x8.font", "monitor.zex"]


class TestM8Manifests(unittest.TestCase):
    def check(self, path, names):
        manifest = _read_json(path)
        self.assertEqual(manifest["format"], "romfs-v1")
        self.assertEqual([entry["name"] for entry in manifest["files"]],
                         names)
        for entry in manifest["files"]:
            self.assertEqual(entry["type"], M8_REQUIRED_TYPES[entry["name"]],
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
             M8_PRODUCTION_SET),
            (os.path.join(M8_MANIFEST_DIR, "manifest-hid-badioc.json"),
             M8_FULL_SET),
            (os.path.join(M8_MANIFEST_DIR, "manifest-v9958-fontfail.json"),
             M8_FULL_SET),
            (os.path.join(M8_MANIFEST_DIR,
                          "manifest-fallback-arena.json"), M8_FULL_SET),
            (os.path.join(M8_MANIFEST_DIR, "manifest-fallback-nohid.json"),
             ["sio.zmod", "ioc.zmod", "v9958.zmod", "v9958-6x8.font",
              "monitor.zex"]),
            (os.path.join(M8_MANIFEST_DIR,
                          "manifest-fallback-nohid-nov9958.json"),
             ["sio.zmod", "ioc.zmod", "monitor.zex"]),
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
        substitutions = {
            "manifest-hid-badioc.json": {"ioc.zmod": "fakeioc.zmod"},
            "manifest-v9958-fontfail.json": {
                "v9958-6x8.font": "v9958-font-stub.bin"},
            "manifest-fallback-arena.json": {
                "v9958.zmod": "m8regfill.zmod"},
            "manifest-fallback-nohid.json": {},
            "manifest-fallback-nohid-nov9958.json": {},
        }
        for name, mapping in substitutions.items():
            manifest = _read_json(os.path.join(M8_MANIFEST_DIR, name))
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
        for target in ("m8-selftest:", "m8-fallback-selftests:",
                       "smoke-hardware-m8:", "smoke-hardware-m8-fb-nohid:",
                       "smoke-hardware-m8-fb-nohid-nov9958:",
                       "smoke-hardware-m8-fb-badioc:",
                       "smoke-hardware-m8-fb-arena:"):
            self.assertIn(target, makefile)
        for flag in ("ZOS_M8_SELFTEST", "ZOS_M8_FALLBACK_SELFTEST",
                     "ZOS_M8_FALLBACK"):
            self.assertIn(flag, makefile)

    def test_target_runner_wires_package20_tests(self):
        with open(os.path.join(ROOT, "tests", "target", "support",
                               "target_test.c")) as handle:
            runner = handle.read()
        self.assertIn('#include "m8_selftest.h"', runner)
        self.assertIn('#include "m8_fallback_selftest.h"', runner)
        self.assertIn("zos_run_m8_selftest()", runner)
        self.assertIn("zos_run_m8_fallback_selftest()", runner)

    def test_serial_smoke_modes_classify_the_matrix(self):
        import serial_smoke as tool
        self.assertEqual(
            sorted(tool.M8FB_MODES),
            ["m8-fb-arena", "m8-fb-badioc", "m8-fb-nohid",
             "m8-fb-nohid-nov9958"])
        for mode in tool.M8FB_MODES:
            self.assertIn(mode, tool.__doc__)
        base = (tool.BANNER +
                b"hid: unavailable 0x05\r\n" +
                b"m8 fallback: input sio0.b class 01\r\n" +
                b"m8 fallback: output v9958.0 class 02\r\n" +
                b"m8 fallback: devices 3\r\n" +
                b"m8 fallback: send input\r\n" +
                b"m8 fallback: input OK 0x52\r\n" +
                b"m8 fallback: console write OK\r\n" +
                b"m8 fallback selftest: OK\r\n")
        self.assertTrue(tool._m8fb_result("m8-fb-nohid", base))
        self.assertFalse(tool._m8fb_result(
            "m8-fb-nohid", base + tool.M8FB_CONSOLE_MARKER))
        self.assertFalse(tool._m8fb_result(
            "m8-fb-nohid", base.replace(b"v9958.0", b"sio0.b")))
        self.assertFalse(tool._m8fb_result(
            "m8-fb-nohid", base.replace(b"input OK 0x52", b"input none")))
        integration = (tool.BANNER +
                       b"m8 selftest: input hid.keyboard0 class 04\r\n" +
                       b"m8 selftest: output v9958.0 class 02\r\n" +
                       b"m8 selftest: devices 04\r\n" +
                       b"m8 selftest: ioc level 4A\r\n" +
                       b"m8 selftest: rejected calls OK\r\n" +
                       b"m8 selftest: OK\r\n")
        self.assertTrue(tool._m8_result(integration))
        self.assertFalse(tool._m8_result(
            integration + b"hid: unavailable 0x05\r\n"))
        self.assertIn("m8", tool.__doc__)


if __name__ == "__main__":
    unittest.main()
