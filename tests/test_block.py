#!/usr/bin/env python3
"""Host tests for the M9 block-device class (work package 29).

Pins the `ZDEV_STORAGE` -> `ZDEV_BLOCK` rename, the block capability bits
in the previously unused range 20-23, and the architecture 32.15
`zblock_info` / `zblock_ops` layouts.  No block driver exists in M9, so
this file validates the definition-only contract; the resident-driver
prohibition is enforced by tests/test_source_layout.py.

Run with: make test
"""

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from maptools import evaluate, parse_c_header  # noqa: E402

DEVICE_H = os.path.join(ROOT, "include", "zephyros", "device.h")
BLOCK_H = os.path.join(ROOT, "include", "zephyros", "block.h")
DOCS_DEVICES = os.path.join(ROOT, "docs", "devices.md")
ARCH = os.path.join(ROOT, "docs", "architecture.md")

# The block class contract of architecture section 32.15, in order.
OPERATIONS = (
    ("get_info", "void *state, struct zblock_info *out"),
    ("read", "void *state, uint32_t lba, uint16_t count, void *buffer"),
    ("write", "void *state, uint32_t lba, uint16_t count, "
              "const void *buffer"),
    ("flush", "void *state"),
    ("media_status", "void *state"),
)

BLOCK_CAPS = {
    "ZCAP_BLOCK_READ": 0x00100000,
    "ZCAP_BLOCK_WRITE": 0x00200000,
    "ZCAP_BLOCK_FLUSH": 0x00400000,
    "ZCAP_BLOCK_REMOVABLE": 0x00800000,
}

# Production/source trees that must not still name the retired class.
STORAGE_SCAN_ROOTS = ("boot", "drivers", "fs", "include", "kernel",
                      "modules", "services", "tools", "user", "tests")
STORAGE_SCAN_EXCLUDED = {os.path.relpath(__file__, ROOT)}


def _read(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read()


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _struct_body(text, name):
    stripped = _strip_comments(text)
    return stripped.split("struct %s {" % name, 1)[1].split("};", 1)[0]


def _operation_signatures(body):
    def squash(text):
        return " ".join(text.split())

    return {m.group(1): squash(m.group(2))
            for m in re.finditer(r"int \(\*(\w+)\)\(([^)]*)\)", body, re.S)}


class TestBlockClassRename(unittest.TestCase):
    def setUp(self):
        self.text = _read(DEVICE_H)

    def _class_values(self):
        env = parse_c_header(DEVICE_H)
        values = {}
        value = 0
        for item in re.search(r"enum\s*\{(.*?)\}", self.text,
                              re.S).group(1).split(","):
            item = _strip_comments(item).strip()
            if not item:
                continue
            if "=" in item:
                name, expr = [part.strip() for part in item.split("=", 1)]
                value = evaluate(expr, env)
            else:
                name = item
            values[name] = value
            value += 1
        return values

    def test_block_class_keeps_numeric_slot_three(self):
        values = self._class_values()
        self.assertEqual(values["ZDEV_BLOCK"], 3,
                         "ZDEV_BLOCK must keep the numeric slot 3")
        self.assertEqual(values["ZDEV_SERIAL"], 1)
        self.assertEqual(values["ZDEV_VIDEO"], 2)
        self.assertEqual(values["ZDEV_HID"], 4)
        self.assertEqual(values["ZDEV_IOC"], 5)

    def test_storage_class_has_no_name_or_alias(self):
        self.assertNotIn("ZDEV_STORAGE", self.text,
                         "ZDEV_STORAGE must not survive as a name or alias")

    def test_retired_name_absent_from_live_source(self):
        offenders = []
        for root_name in STORAGE_SCAN_ROOTS:
            for dirpath, dirnames, filenames in os.walk(
                    os.path.join(ROOT, root_name)):
                dirnames[:] = [d for d in dirnames if d != "__pycache__"]
                for name in sorted(filenames):
                    path = os.path.join(dirpath, name)
                    rel = os.path.relpath(path, ROOT)
                    if rel in STORAGE_SCAN_EXCLUDED:
                        continue
                    if not name.endswith((".c", ".h", ".asm", ".inc",
                                          ".py", ".json", ".mk")):
                        continue
                    if "ZDEV_STORAGE" in _read(path):
                        offenders.append(rel)
        self.assertEqual(offenders, [],
                         "the retired class name survives in source: %s"
                         % ", ".join(offenders))

    def test_docs_name_the_block_class(self):
        devices = _read(DOCS_DEVICES)
        self.assertIn("BLOCK=3", devices)
        self.assertNotIn("STORAGE=3", devices)
        self.assertIn("BLOCK 20-23", " ".join(devices.split()))
        # The first resident driver is deferred to its first consumer.
        self.assertIn("first block driver arrives with its first consumer",
                      devices)
        self.assertIn("first block driver arrives with its first consumer",
                      _read(ARCH))


class TestBlockCapabilities(unittest.TestCase):
    def test_capability_bits_are_pinned(self):
        env = parse_c_header(DEVICE_H)
        for name, value in BLOCK_CAPS.items():
            self.assertEqual(evaluate(name, env), value, name)

    def test_range_is_documented_and_does_not_collide(self):
        text = _read(DEVICE_H)
        self.assertRegex(text, r"BLOCK class capabilities: bits 20-23")
        ranges = {
            "generic": (0, 3),
            "VIDEO": (8, 14),
            "HID": (16, 18),
            "BLOCK": (20, 23),
            "IOC": (24, 27),
        }
        seen = []
        for low, high in ranges.values():
            seen.extend(range(low, high + 1))
        self.assertEqual(len(seen), len(set(seen)),
                         "capability ranges overlap")
        for name, value in BLOCK_CAPS.items():
            bit = value.bit_length() - 1
            self.assertTrue(20 <= bit <= 23, "%s is outside 20-23" % name)


class TestBlockAbiShape(unittest.TestCase):
    def setUp(self):
        self.text = _read(BLOCK_H)

    def test_layout_structures_exist(self):
        for name in ("zblock_info", "zblock_ops"):
            self.assertIn("struct %s {" % name, self.text)
        self.assertIn("ZDEV_BLOCK", self.text)
        includes = [line for line in self.text.splitlines()
                    if line.startswith("#include")]
        self.assertTrue(any("zephyros/device.h" in line for line in includes),
                        "block.h must reach the class/capability contract")

    def test_info_field_order(self):
        body = _struct_body(self.text, "zblock_info")
        fields = re.findall(r"\b(\w+)\s*;", body)
        self.assertEqual(fields, ["block_size", "block_count",
                                  "max_transfer_blocks", "flags"])

    def test_operation_order_and_signatures(self):
        body = _struct_body(self.text, "zblock_ops")
        found = _operation_signatures(body)
        self.assertEqual(list(found), [name for name, _ in OPERATIONS])
        for name, params in OPERATIONS:
            self.assertEqual(found[name], " ".join(params.split()),
                             "%s must keep the section 32.15 signature"
                             % name)

    def test_compile_time_layout_pins(self):
        for assert_re in (
                r"sizeof\(struct zblock_info\)\s*==\s*10",
                r"__builtin_offsetof\(struct zblock_info, block_count\)"
                r"\s*==\s*2",
                r"__builtin_offsetof\(struct zblock_info, max_transfer_blocks\)"
                r"\s*==\s*6",
                r"__builtin_offsetof\(struct zblock_info, flags\)\s*==\s*8",
                r"sizeof\(struct zblock_ops\)\s*==\s*10",
                r"__builtin_offsetof\(struct zblock_ops, read\)\s*==\s*2",
                r"__builtin_offsetof\(struct zblock_ops, write\)\s*==\s*4",
                r"__builtin_offsetof\(struct zblock_ops, flush\)\s*==\s*6",
                r"__builtin_offsetof\(struct zblock_ops, media_status\)"
                r"\s*==\s*8"):
            self.assertRegex(self.text, assert_re)

    def test_media_status_semantics_are_documented(self):
        # Normalize the comment framing so a phrase split across `*`
        # continuation lines still matches.
        text = " ".join(re.sub(r"\s*\*\s*", " ", self.text).split())
        self.assertIn("0 when the media is present and usable", text)
        self.assertIn("-ZOS_ENODEV", text)
        self.assertIn("-ZOS_EIO", text)

    def test_results_follow_the_device_error_convention(self):
        text = " ".join(self.text.split())
        self.assertIn("0 on success or a negative `ZOS_E*`", text)


if __name__ == "__main__":
    unittest.main()
