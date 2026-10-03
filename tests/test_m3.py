#!/usr/bin/env python3
"""Host-side M3 tests: device/driver constant contract, mapcheck bank-7
placement rules (synthetic fixtures), fallback-build artifacts, and the
static linker-file/object consistency check.

The executable M3 semantics (registry, rollback, console, IRQ claim) are
replayed from the real linked image by tests/test_m3_sim.py; these tests
cover the declarative and build-artifact surface around it.
"""

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)

import mapcheck  # noqa: E402
from test_m1 import evaluate, make_map, parse_c_header  # noqa: E402

DEVICE_H = os.path.join(ROOT, "include", "zephyros", "device.h")
DRIVER_H = os.path.join(ROOT, "include", "zephyros", "driver.h")
CONSOLE_H = os.path.join(ROOT, "include", "zephyros", "console.h")
M3_SELFTEST_H = os.path.join(ROOT, "include", "zephyros", "m3_selftest.h")
ERRNO_H = os.path.join(ROOT, "include", "zephyros", "errno.h")
HARDWARE_H = os.path.join(ROOT, "platform", "zephyr80", "hardware.h")
MEMORY_INC = os.path.join(ROOT, "platform", "zephyr80", "memory.inc")

INTERNAL_ERRORS = (
    "EINVAL", "ENODEV", "EBUSY", "ENOENT", "ENOSPC", "ENOTSUP",
    "ETIMEDOUT", "EAGAIN",
)


def parse_c_enums(path, env):
    """Parse plain C enum bodies (name[ = expr], comma separated)."""
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


class TestDeviceConstants(unittest.TestCase):
    def setUp(self):
        self.device = parse_c_header(DEVICE_H)
        self.errno = parse_c_header(ERRNO_H)
        self.env = dict(self.errno)
        self.env.update(self.device)
        self.env.update(parse_c_enums(DEVICE_H, self.env))

    def test_handle_limits(self):
        self.assertEqual(evaluate("ZDEV_HANDLE_INVALID", self.env), 0)
        self.assertEqual(evaluate("ZDEV_MAX_DEVICES", self.env), 16)

    def test_class_ids_are_distinct_and_ordered(self):
        names = ["ZDEV_SERIAL", "ZDEV_VIDEO", "ZDEV_STORAGE",
                 "ZDEV_INPUT", "ZDEV_TRANSPORT", "ZDEV_OTHER"]
        values = [evaluate(name, self.env) for name in names]
        self.assertEqual(values, [1, 2, 3, 4, 5, 6])
        self.assertEqual(len(set(values)), len(values))

    def test_capability_bits_are_single_distinct_bits(self):
        names = ["ZCAP_CHAR_IN", "ZCAP_CHAR_OUT",
                 "ZCAP_CONSOLE_IN", "ZCAP_CONSOLE_OUT"]
        values = [evaluate(name, self.env) for name in names]
        self.assertEqual(values, [0x0001, 0x0002, 0x0004, 0x0008])
        for value in values:
            self.assertEqual(value & (value - 1), 0, "not a single bit")
        self.assertEqual(len(set(values)), len(values))

    def test_internal_errors_are_negated_errno_values(self):
        self.assertEqual(evaluate("ZDEV_OK", self.env), 0)
        for suffix in INTERNAL_ERRORS:
            internal = evaluate("ZDEV_" + suffix, self.env)
            public = evaluate("ZOS_" + suffix, self.env)
            self.assertEqual(
                internal, -public,
                "ZDEV_%s (%d) must be -ZOS_%s (%d)"
                % (suffix, internal, suffix, public),
            )

    def test_errno_values_are_stable_and_distinct(self):
        self.assertEqual(evaluate("ZOS_ENOSYS", self.errno), 0x01)
        values = [evaluate(name, self.errno)
                  for name in ["ZOS_" + s for s in INTERNAL_ERRORS]]
        self.assertEqual(len(set(values)), len(values))
        self.assertNotIn(evaluate("ZOS_ENOSYS", self.errno), values)


class TestDriverConstants(unittest.TestCase):
    def setUp(self):
        self.driver = parse_c_header(DRIVER_H)
        self.env = dict(parse_c_header(DEVICE_H))
        self.env.update(self.driver)

    def test_abi_version(self):
        self.assertEqual(evaluate("ZDRIVER_ABI_VERSION", self.env), 1)

    def test_init_result_convention(self):
        self.assertEqual(evaluate("ZDRIVER_OK", self.env), 0)
        self.assertEqual(evaluate("ZDRIVER_FAIL", self.env), -1)

    def test_api_exposes_only_the_three_m3_services(self):
        text = open(DRIVER_H).read()
        for member in ("device_register", "irq_claim", "irq_release"):
            self.assertIn(member, text)
        # The explicit table must be declared, not linker-discovered.
        self.assertIn("boot_drivers", text)

    def test_console_service_surface(self):
        text = open(CONSOLE_H).read()
        for member in ("console_putc", "console_write",
                       "console_poll", "console_getc",
                       "console_bind_defaults"):
            self.assertIn(member, text)


class TestM3SelftestConstants(unittest.TestCase):
    def test_result_codes_distinct_and_ok_zero(self):
        env = parse_c_header(M3_SELFTEST_H)
        self.assertEqual(evaluate("ZOS_M3ST_OK", env), 0)
        values = [evaluate(name, env) for name in env
                  if name.startswith("ZOS_M3ST_FAIL_")]
        self.assertEqual(len(set(values)), len(values))
        self.assertNotIn(0, values)

    def test_rx_protocol(self):
        env = parse_c_header(M3_SELFTEST_H)
        self.assertEqual(evaluate("ZOS_M3ST_RX_BYTE", env), ord("R"))
        self.assertGreater(evaluate("ZOS_M3ST_RX_ATTEMPTS", env), 0)

    def test_isr_stack_constants_match_memory_inc(self):
        env = parse_c_header(HARDWARE_H)
        layout = mapcheck.parse_memory_inc(MEMORY_INC)
        self.assertEqual(evaluate("ZOS_IRQ_ISR_STACK_BASE", env),
                         layout["ZOS_IRQ_ISR_STACK_BASE"])
        self.assertEqual(evaluate("ZOS_IRQ_ISR_STACK_TOP", env),
                         layout["ZOS_IRQ_ISR_STACK_TOP"])


class TestMapcheckM3Rules(unittest.TestCase):
    def test_synthetic_map_with_m3_symbols_passes(self):
        mapcheck.check_map(make_map())

    def test_console_routine_in_common_rejected(self):
        text = make_map(symbols={"_console_putc": 0xE100})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_registry_state_in_common_rejected(self):
        text = make_map(symbols={"_device_table": 0xE200})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_driver_state_in_isr_stack_rejected(self):
        text = make_map(symbols={"_sio0b_state": 0xDF10})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_missing_m3_symbol_rejected(self):
        text = make_map(symbols={"_device_register": None})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_test_driver_prefix_in_common_rejected(self):
        text = make_map(symbols={"_m3_irq_probe_hits": 0xE180})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_test_driver_prefix_in_bank7_accepted(self):
        text = make_map(symbols={"_m3_irq_probe_hits": 0xC380})
        mapcheck.check_map(text)

    def test_m3_prefix_sweep_covers_real_symbol_lists(self):
        # Every required M3 symbol must be subject to (or explicitly
        # listed for) bank-7 checking.
        for name in mapcheck.M3_BANK7_ROUTINES + mapcheck.M3_BANK7_OBJECTS:
            self.assertTrue(
                name.startswith(mapcheck.M3_BANK7_PREFIXES),
                "%s escapes the M3 prefix sweep" % name,
            )


class TestM3Artifacts(unittest.TestCase):
    def setUp(self):
        self.bin_path = os.path.join(ROOT, "build", "zephyros.bin")

    def read_image(self, *parts):
        path = os.path.join(ROOT, *parts)
        if not os.path.exists(path):
            self.skipTest("%s missing; build the image first" % path)
        return open(path, "rb").read()

    def test_normal_image_contains_console_lines(self):
        image = self.read_image("build", "zephyros.bin")
        self.assertIn(b"console: sio0.b OK\r\n", image)
        self.assertIn(b"console: unbound\r\n", image)
        self.assertIn(b"sio0.b", image)
        # The self-test harness must not leak into the production image.
        self.assertNotIn(b"m3 selftest", image)

    def test_m3_selftest_image_contains_harness(self):
        image = self.read_image("build-m3-selftest",
                                "zephyros-m3-selftest.bin")
        self.assertEqual(len(image), 131072)
        for marker in (b"m3 selftest", b"console: sio0.b OK\r\n",
                       b"m3 selftest: ready for RX\r\n"):
            self.assertIn(marker, image)
        normal = self.read_image("build", "zephyros.bin")
        self.assertNotIn(b"m3 selftest", normal)

    def test_fallback_nodrv_image(self):
        image = self.read_image("build-m3-nodrv", "zephyros.bin")
        self.assertIn(b"console: static drivers disabled\r\n", image)
        self.assertNotIn(b"console: sio0.b OK\r\n", image)
        self.assertIn(b"ZephyrOS 0.0\r\n", image)

    def test_fallback_nobind_image(self):
        image = self.read_image("build-m3-nobind", "zephyros.bin")
        self.assertIn(b"console: binding disabled\r\n", image)
        self.assertNotIn(b"console: sio0.b OK\r\n", image)
        self.assertIn(b"ZephyrOS 0.0\r\n", image)

    def test_static_linker_file_matches_build_objects(self):
        lk_path = os.path.join(ROOT, "linker", "zephyr80.lk")
        self.assertTrue(os.path.exists(lk_path))
        listed = set()
        for line in open(lk_path):
            line = line.split(";")[0].strip()
            if line.startswith("build/") and line.endswith(".rel"):
                listed.add(line)
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
