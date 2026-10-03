#!/usr/bin/env python3
"""Host-side M8 HID class and driver contract tests (work package 17).

Covers the declarative and structural surface that can be checked
without executing target code:

  - include/zephyros/hid.h: operation order, the reserved key-event and
    gamepad shapes, layout assertions, and the capability-authoritative
    ZDEV_ENOTSUP convention (absent methods are null and are never
    called);
  - the private MCU HID protocol constants shared with the
    protocol-aware IOC clients (drivers/ioc/ioc_proto.h): CMD_HID_INPUT
    0Eh, RSP_HID_INPUT 8Eh, 2 metadata bytes and a 24-byte reply maximum
    (26 with metadata);
  - the packed hid.zmod container, manifest, ROMFS entry and boot order
    (SIO, then IOC, then HID), with a failed/malformed dependency not
    invalidating earlier devices;
  - the driver source discipline: IOC discovery through device_find /
    device_interface only (no imported IOC-module symbol, no name
    lookup, no port/status constants, no IN/OUT or register access),
    the exact input-only descriptor, fetch-on-doorbell only, exact
    request sizing, strict reply validation before any ring commit, and
    the proven 64-call stuck-doorbell penalty;
  - the fake-IOC fixture and the no-IOC/malformed images that prove
    dependency failure, interface verification and rollback;
  - mapcheck rejects linked HID driver symbols (module-only);
  - the smoke tool's m8-hid markers and result classification.

Executable behavior is replayed from the real linked images by
tests/test_hid_sim.py.
"""

import json
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)

import mapcheck  # noqa: E402
import zmodpack  # noqa: E402
from test_ioc import struct_member_order  # noqa: E402
from test_m1 import evaluate, parse_c_header  # noqa: E402

HID_H = os.path.join(ROOT, "include", "zephyros", "hid.h")
DEVICE_H = os.path.join(ROOT, "include", "zephyros", "device.h")
PROTO_H = os.path.join(ROOT, "drivers", "ioc", "ioc_proto.h")
DRIVER_H = os.path.join(ROOT, "drivers", "hid", "hid_driver.h")
DRIVER_C = os.path.join(ROOT, "drivers", "hid", "hid.c")
MODULE_C = os.path.join(ROOT, "drivers", "hid", "hid_module.c")
MANIFEST = os.path.join(ROOT, "modules", "manifests", "hid.json")
RECOVERY_MANIFEST = os.path.join(ROOT, "recovery", "manifest.json")
BADIOC_MANIFEST = os.path.join(ROOT, "tests", "m8",
                               "manifest-hid-badioc.json")
FAKE_IOC_C = os.path.join(ROOT, "tests", "modules", "hid_fake_ioc.c")
TARGET_TEST_C = os.path.join(ROOT, "tests", "target", "m8",
                             "m8_hid_test.c")
TARGET_RUNNER = os.path.join(ROOT, "tests", "target", "support",
                             "target_test.c")
MAIN_C = os.path.join(ROOT, "kernel", "main.c")
MAKEFILE = os.path.join(ROOT, "Makefile")
ZMOD = os.path.join(ROOT, "build", "modules", "hid.zmod")
MAP = os.path.join(ROOT, "build", "zephyros.map")


def _read(path, mode="r"):
    with open(path, mode) as handle:
        return handle.read()


def _load_json(path):
    with open(path) as handle:
        return json.load(handle)


class TestHidClassContract(unittest.TestCase):
    def setUp(self):
        self.text = _read(HID_H)
        self.env = parse_c_header(HID_H)

    def test_ops_order_and_reserved_shapes(self):
        self.assertEqual(
            struct_member_order(self.text, "zhid_ops",
                                function_pointers=True),
            ["read_text", "read_gamepad"])
        self.assertEqual(struct_member_order(self.text, "zhid_key_event"),
                         ["usage", "modifiers", "pressed", "reserved"])
        self.assertEqual(struct_member_order(self.text,
                                             "zhid_gamepad_state"),
                         ["buttons", "reserved", "x", "y"])

    def test_layout_assertions_present(self):
        for needle in ("sizeof(struct zhid_ops) == 4",
                       "sizeof(struct zhid_key_event) == 4",
                       "sizeof(struct zhid_gamepad_state) == 4",
                       "__builtin_offsetof(struct zhid_ops, read_gamepad)"
                       " == 2"):
            self.assertIn(needle, self.text)

    def test_capabilities_are_authoritative(self):
        # Absent methods stay null and unsupported capabilities must be
        # reported as ZDEV_ENOTSUP instead of being inferred from the
        # name; M8 implements only ZCAP_HID_TEXT.
        self.assertIn("ZDEV_ENOTSUP", self.text)
        self.assertIn("never inferred", self.text)
        self.assertIn("Absent methods stay null", self.text)
        self.assertIn("M8 implements only ZCAP_HID_TEXT", self.text)

    def test_general_api_keeps_mcu_command_numbers_private(self):
        self.assertNotIn("CMD_HID", self.text)
        self.assertNotIn("RSP_HID", self.text)
        self.assertNotIn("0x0E", self.text)

    def test_text_read_semantics_documented(self):
        for needle in ("returns 0 when nothing is available",
                       "ZDEV_EAGAIN",
                       "NUL and", "multi-byte VT100"):
            self.assertIn(needle, self.text)


class TestHidProtocolConstants(unittest.TestCase):
    def setUp(self):
        self.env = parse_c_header(PROTO_H)

    def test_hid_command_reply_and_layout(self):
        expected = {
            "IOC_CMD_HID_INPUT": 0x0E,
            "IOC_RSP_HID_INPUT": 0x8E,
            "IOC_OFF_HID_INPUT_MAX": 0,
            "IOC_HID_INPUT_META_LEN": 2,
            "IOC_HID_INPUT_MAX_DATA": 24,
        }
        for name, value in expected.items():
            self.assertIn(name, self.env, "ioc_proto.h missing %s" % name)
            self.assertEqual(evaluate(name, self.env), value, name)

    def test_reply_maximum_matches_the_wire_bound(self):
        # metadata + translated bytes = 26 = the command-frame DATA cap.
        self.assertEqual(evaluate("IOC_HID_INPUT_META_LEN", self.env) +
                         evaluate("IOC_HID_INPUT_MAX_DATA", self.env), 26)


class TestHidModuleArtifact(unittest.TestCase):
    def test_manifest_shape(self):
        manifest = _load_json(MANIFEST)
        self.assertEqual(manifest["name"], "hid")
        self.assertEqual(manifest["type"], "driver")
        self.assertEqual(manifest["abi"], 2)
        self.assertEqual(manifest["init"], "hid_module_init")
        self.assertEqual(manifest["image_areas"], ["_CODE"])
        self.assertEqual(manifest["bss_areas"], ["_DATA"])
        self.assertEqual(manifest["objects"],
                         ["hid.rel", "hid_module.rel",
                          "sdcc_indirect_calls.rel"])

    def test_packed_container(self):
        if not os.path.exists(ZMOD):
            self.skipTest("build/modules/hid.zmod missing; build it first")
        data = _read(ZMOD, "rb")
        info = zmodpack.parse_container(data, "hid.zmod")
        self.assertGreater(info["image_size"], 0)
        self.assertGreater(info["bss_size"], 0)
        self.assertNotEqual(info["init_offset"], 0xFFFF)
        # Small resident module: the ring and state are the only BSS.
        self.assertLessEqual(info["required_granules"], 4)
        for rtype, _patch in info["relocs"]:
            self.assertEqual(rtype, 1)

    def test_romfs_entry_and_boot_order(self):
        manifest = _load_json(RECOVERY_MANIFEST)
        names = [entry["name"] for entry in manifest["files"]]
        self.assertEqual(names, ["sio.zmod", "ioc.zmod", "hid.zmod",
                                 "monitor.zex"])
        main = _read(MAIN_C)
        sio = main.index('romfs_load_zmod("sio"')
        ioc = main.index('romfs_load_zmod("ioc"')
        hid = main.index('romfs_load_zmod("hid"')
        self.assertLess(sio, ioc, "IOC must load after the SIO fallback")
        self.assertLess(ioc, hid, "HID must load after the IOC transport")
        self.assertIn('early_console_puts("hid: unavailable', main)
        self.assertIn("#if ZOS_HID_BOOT", main)

    def test_makefile_targets_and_module_rule(self):
        makefile = _read(MAKEFILE)
        for target in ("hid-test:", "m8-hid-selftest:",
                       "m8-hid-noioc-selftest:",
                       "m8-hid-badioc-selftest:",
                       "smoke-hardware-m8-hid:"):
            self.assertIn(target, makefile)
        self.assertIn("$(MODULES)/hid.zmod", makefile)
        self.assertIn("$(HID_MODULE_OBJS)", makefile)
        self.assertIn("ZOS_M8_HID_SELFTEST", makefile)
        self.assertIn("ZOS_M8_HID_NOIOC", makefile)
        self.assertIn("ZOS_M8_HID_BADIOC", makefile)
        self.assertIn("ZOS_HID_BOOT", makefile)
        # The declared boot-set plan must carry SIO, IOC and HID.
        self.assertIn("BOOT_ZMODS", makefile)
        self.assertIn("$(MODULES)/hid.zmod", makefile)

    def test_production_map_has_no_linked_hid_symbols(self):
        if not os.path.exists(MAP):
            self.skipTest("build/zephyros.map missing; build first")
        syms = _map_symbols(MAP)
        linked = [name for name in syms
                  if name.startswith("_hid_") or name.startswith("_fake")]
        self.assertEqual(linked, [],
                         "HID driver/fixture symbols must not be linked "
                         "into the kernel image: %s" % linked)

    def test_pre_m8_images_compile_hid_boot_out(self):
        # Pre-M8 acceptance images never carry the module; the Makefile
        # must default ZOS_HID_BOOT to 0 for every one of them so their
        # accepted bytes are preserved.
        makefile = _read(MAKEFILE)
        index = makefile.index("ZOS_HID_BOOT := 0")
        gate = makefile[index - 600:index]
        for flag in ("ZOS_SELFTEST", "ZOS_M3_SELFTEST", "ZOS_M4_SELFTEST",
                     "ZOS_M5_SELFTEST", "ZOS_M6_SELFTEST",
                     "ZOS_M7_SELFTEST"):
            self.assertIn(flag, gate, "%s must keep HID compiled out" % flag)


def _map_symbols(map_path):
    syms = {}
    with open(map_path) as handle:
        for line in handle:
            match = re.match(r"\s*([0-9A-Fa-f]{8})\s+(\S+)", line)
            if match:
                syms[match.group(2)] = int(match.group(1), 16)
    return syms


class TestMapcheckRules(unittest.TestCase):
    def test_hid_symbols_are_module_only(self):
        self.assertIn("_hid_", mapcheck.STATIC_HID_PREFIXES)
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_symbols({}, {"_hid_ops": 0x2000})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_symbols({}, {"_hid_module_init": 0x3000})

    def test_production_rejects_m8_hid_acceptance_symbols(self):
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_symbols({}, {"_m8_hid_selftest_result": 0x2000},
                                   production=True)


class TestHidSourceDiscipline(unittest.TestCase):
    def setUp(self):
        self.c = _read(DRIVER_C)
        self.header = _read(DRIVER_H)
        self.module = _read(MODULE_C)
        self.sources = (self.c, self.header, self.module)
        self.service = self.c.split("static uint8_t hid_service")[1].split(
            "int hid_read_text")[0]
        self.fetch = self.c.split("static uint8_t hid_fetch")[1].split(
            "static uint8_t hid_service")[0]
        self.init = self.c.split("int hid_driver_init")[1].split(
            "const struct zhid_ops")[0]

    def test_no_sio1_rr0_or_port_constants_anywhere(self):
        forbidden = ("ZOS_SIO1", "ZOS_SIO_RR0", "ZOS_IOC_HID_DOORBELL",
                     "ZOS_IOC_CMD_READY", "ZOS_IOC_WR", "ZOS_IOC_BULK",
                     "__sfr", "__asm", "CTSB_LAT")
        for body in self.sources:
            for needle in forbidden:
                self.assertNotIn(needle, body,
                                 "HID source must not use %r" % needle)
        # No assembly mnemonics / port literals.
        for body in self.sources:
            self.assertNotRegex(body, r"(?m)^\s*(in|out)\s")
            self.assertNotIn("0x32", body)
            self.assertNotIn("0x33", body)
            self.assertNotIn("0x30", body)
            self.assertNotIn("0x31", body)

    def test_no_imported_ioc_module_symbol_or_name_lookup(self):
        for body in self.sources:
            for needle in ("_ioc_", "ioc_command", "ioc_bulk_read",
                           "ioc_bulk_write", "ioc_hw_init",
                           "ioc_link_bringup", "ioc_module_init",
                           "ioc_state", "ioc.0"):
                self.assertNotIn(needle, body,
                                 "HID must not reference %r" % needle)

    def test_discovery_is_class_capability_interface_only(self):
        self.assertIn("api->device_find(ZDEV_IOC", self.c)
        self.assertIn("ZCAP_IOC_COMMAND | ZCAP_IOC_STATUS", self.c)
        self.assertIn("api->device_interface(handle, ZDEV_IOC", self.c)
        self.assertIn("ZDEV_OK", self.init)
        self.assertIn("ZDRIVER_FAIL", self.init)
        # The interface contents are verified before use.
        self.assertIn("ops->command ==", self.init)
        self.assertIn("ops->status ==", self.init)

    def test_descriptor_is_exact_and_input_only(self):
        self.assertIn("static const struct zdevice_desc "
                      "hid_keyboard0_desc", self.module)
        self.assertIn('"hid.keyboard0"', self.module)
        self.assertIn("ZCAP_HID_TEXT | ZCAP_CHAR_IN | ZCAP_CONSOLE_IN",
                      self.module)
        self.assertIn("&hid_char_ops", self.module)
        self.assertIn("&hid_ops", self.module)
        self.assertIn("&hid_state", self.module)
        # Input-only character table: putc/write are null.
        self.assertIn("(int (*)(void *, char))0", self.c)
        self.assertIn("(int (*)(void *, const char *, uint16_t))0", self.c)
        self.assertIn("hid_poll,\n\thid_getc", self.c)
        # read_gamepad stays null in M8.
        self.assertIn(
            "(int (*)(void *, uint8_t, struct zhid_gamepad_state *))0",
            self.c)

    def test_ring_and_penalty_constants(self):
        env = parse_c_header(DRIVER_H)
        self.assertEqual(evaluate("ZHID_RING_SIZE", env), 16)
        self.assertEqual(evaluate("ZHID_STUCK_PENALTY", env), 64)
        # Power-of-two ring: wraparound is the mask expression.
        self.assertIn("ZHID_RING_SIZE - 1u", self.c)

    def test_fetch_on_doorbell_only_and_service_order(self):
        # The semantic status sample comes before any fetch, and the
        # fetch is gated on ZIOC_STATUS_HID_PENDING.
        status = self.service.index("state->ioc->status(")
        pending = self.service.index("ZIOC_STATUS_HID_PENDING")
        penalty = self.service.index("state->penalty != 0u")
        fetch = self.service.index("hid_fetch(state)")
        self.assertLess(status, pending)
        self.assertLess(pending, penalty)
        self.assertLess(penalty, fetch)
        # Idle (doorbell released): clear the penalty, no command.
        self.assertIn("state->penalty = 0u;", self.service)

    def test_exact_request_sizing(self):
        self.assertIn("IOC_HID_INPUT_MAX_DATA", self.fetch)
        self.assertIn("IOC_CMD_HID_INPUT", self.fetch)
        self.assertIn("IOC_OFF_HID_INPUT_MAX", self.fetch)
        self.assertIn("request.length = 1u", self.fetch)
        # Never ask for more than the local free space.
        self.assertIn("free_space", self.fetch)

    def test_reply_validation_precedes_commit(self):
        for needle in ("IOC_RSP_HID_INPUT", "reply.status != 0u",
                       "reply.length < IOC_HID_INPUT_META_LEN",
                       "reply.length > (uint8_t)(IOC_HID_INPUT_META_LEN",
                       "IOC_HID_INPUT_MAX_DATA)",
                       "payload > free_space",
                       "payload > (uint8_t)(ZHID_RING_SIZE - "
                       "state->count)"):
            self.assertIn(needle, self.fetch,
                          "fetch missing validation %r" % needle)
        # Commit is a single loop after every check.
        commit = self.fetch.index("state->ring[state->head]")
        for check in ("payload > free_space",
                      "payload > (uint8_t)(ZHID_RING_SIZE - "
                      "state->count)"):
            self.assertLess(self.fetch.index(check), commit)

    def test_stuck_penalty_applied_and_cleared(self):
        self.assertIn("state->penalty = ZHID_STUCK_PENALTY", self.service)
        self.assertIn("--state->penalty", self.service)
        # Successful data clears it.
        self.assertIn("state->penalty = 0u;", self.service)

    def test_getc_and_read_text_are_nonblocking(self):
        self.assertIn("return ZDEV_EAGAIN;", self.c)
        self.assertIn("ZDEV_EINVAL", self.c)
        # No blocking loops anywhere in the driver.
        self.assertNotIn("while (1)", self.c)
        self.assertNotIn("for (;;)", self.c)


class TestHidDependencyFailureArtifacts(unittest.TestCase):
    def test_fake_ioc_registers_malformed_interface(self):
        fake = _read(FAKE_IOC_C)
        self.assertIn("ZDEV_IOC", fake)
        self.assertIn("ZCAP_IOC_COMMAND | ZCAP_IOC_STATUS", fake)
        self.assertIn("api->device_register", fake)
        # The command operation is absent while the capability claims it.
        self.assertIn(
            "(int (*)(void *, const struct zioc_frame *,\n"
            "\t\t struct zioc_frame *))0", fake)
        self.assertIn("hid_fake_ioc_status", fake)

    def test_badioc_manifest_maps_ioc_entry_to_fixture(self):
        manifest = _load_json(BADIOC_MANIFEST)
        entries = {entry["name"]: entry["path"]
                   for entry in manifest["files"]}
        self.assertEqual(entries["ioc.zmod"], "fakeioc.zmod")
        self.assertEqual(entries["hid.zmod"], "hid.zmod")
        self.assertIn("sio.zmod", entries)

    def test_target_test_covers_rollback_and_fallback(self):
        test = _read(TARGET_TEST_C)
        for needle in ("ZOS_M8_HID_NOIOC", "ZOS_M8_HID_BADIOC",
                       "ZDEV_HANDLE_INVALID",
                       "m8 hid: hid absent after rollback",
                       "m8 hid: malformed ioc rejected",
                       "m8 hid: sio fallback, send R",
                       "m8 hid: sio fallback OK",
                       "m8 hid selftest: OK"):
            self.assertIn(needle, test)
        # The capability dispatch helper proves ENOTSUP for absent
        # capabilities without calling through a null method.
        self.assertIn("m8hid_gamepad", test)
        self.assertIn("ZDEV_ENOTSUP", test)
        self.assertIn("ZCAP_HID_GAMEPAD", test)
        runner = _read(TARGET_RUNNER)
        self.assertIn("#if ZOS_M8_HID_SELFTEST || ZOS_M8_HID_NOIOC || "
                      "ZOS_M8_HID_BADIOC", runner)
        self.assertIn("zos_run_m8_hid_selftest();", runner)


class TestHidSmokeToolContract(unittest.TestCase):
    def setUp(self):
        import serial_smoke

        self.tool = serial_smoke
        self.test = _read(TARGET_TEST_C)

    def test_markers_match_target_output(self):
        for needle in ("m8 hid: keyboard0 OK\\r\\n",
                       "m8 hid: ready for input\\r\\n",
                       "m8 hid: rx ",
                       "m8 hid: typed pass OK\\r\\n",
                       "m8 hid: sio fallback, send R\\r\\n",
                       "m8 hid: sio fallback OK\\r\\n",
                       "m8 hid selftest: OK\\r\\n",
                       "m8 hid selftest: FAIL 0x"):
            self.assertIn(needle, self.test,
                          "target output changed without the smoke tool: %r"
                          % needle)
        for constant, prefix in (
                (self.tool.M8HID_READY, b"m8 hid: ready for input"),
                (self.tool.M8HID_TYPED_OK, b"m8 hid: typed pass OK"),
                (self.tool.M8HID_SIO_PROMPT,
                 b"m8 hid: sio fallback, send R"),
                (self.tool.M8HID_FALLBACK_OK, b"m8 hid: sio fallback OK"),
                (self.tool.M8HID_SELFTEST_OK, b"m8 hid selftest: OK")):
            self.assertTrue(constant.startswith(prefix),
                            "%r does not start with %r" % (constant, prefix))

    def test_typed_bytes_match_the_mcu_translation(self):
        # a, B, 3, Enter, Backspace, Ctrl-X, Arrow-Up (ESC [ A).
        self.assertEqual(self.tool.M8HID_TYPED_BYTES,
                         (0x61, 0x42, 0x33, 0x0D, 0x08, 0x18,
                          0x1B, 0x5B, 0x41))
        for fragment in ("0x61u", "0x42u", "0x33u", "0x0Du", "0x08u",
                         "0x18u", "0x1Bu, 0x5Bu, 0x41u"):
            self.assertIn(fragment, self.test)

    def test_result_classification(self):
        rx = b"".join(b"m8 hid: rx %02X\r\n" % b
                      for b in self.tool.M8HID_TYPED_BYTES)
        base = (self.tool.BANNER + self.tool.CONSOLE_DYNAMIC_OK +
                b"m8 hid: keyboard0 OK\r\n"
                b"m8 hid: ready for input\r\n" + rx +
                b"m8 hid: typed pass OK\r\n"
                b"m8 hid: sio fallback, send R\r\n"
                b"m8 hid: sio fallback OK\r\n"
                b"m8 hid selftest: OK\r\n")
        self.assertEqual(self.tool._m8hid_result(base), "ok")
        # Short/missing evidence is a failure.
        for wrong in (
                base.replace(b"m8 hid: typed pass OK\r\n", b""),
                base.replace(b"m8 hid: sio fallback OK\r\n", b""),
                base.replace(b"m8 hid selftest: OK\r\n", b""),
                base.replace(b"m8 hid: rx 42\r\n", b""),
                base.replace(b"m8 hid: rx 42\r\nm8 hid: rx 33\r\n",
                             b"m8 hid: rx 33\r\nm8 hid: rx 42\r\n"),
                base + b"m8 hid selftest: FAIL 0x05 detail 0x00\r\n",
                base.replace(b"m8 hid: keyboard0 OK\r\n",
                             b"hid: unavailable\r\n")):
            self.assertEqual(self.tool._m8hid_result(wrong), "fail",
                             "misclassified: %r" % wrong[-80:])

    def test_docstring_and_modes_mention_m8_hid(self):
        self.assertIn("m8-hid", self.tool.__doc__)
        source = _read(os.path.join(ROOT, "tools", "serial_smoke.py"))
        self.assertIn('"m8-hid"', source)


if __name__ == "__main__":
    unittest.main()
