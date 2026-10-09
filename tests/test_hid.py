#!/usr/bin/env python3
"""Host-side HID class and driver contract tests.

Covers the declarative and structural surface that can be checked
without executing target code:

  - include/zephyros/hid.h: operation order, the reserved key-event and
    gamepad shapes, layout assertions, and the capability-authoritative
    ZDEV_ENOTSUP convention (absent methods are null and are never
    called);
  - the client-visible MCU HID service vocabulary shared through the
    public IOC protocol contract (include/zephyros/ioc_protocol.h):
    CMD_HID_INPUT 0Eh, RSP_HID_INPUT 8Eh, 2 metadata bytes and a 24-byte
    reply maximum (26 with metadata), never the IOC driver's private
    transport header;
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
  - the smoke tool's hid markers and result classification.

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
from maptools import evaluate, parse_c_header  # noqa: E402

HID_H = os.path.join(ROOT, "include", "zephyros", "hid.h")
DEVICE_H = os.path.join(ROOT, "include", "zephyros", "device.h")
PROTO_H = os.path.join(ROOT, "include", "zephyros", "ioc_protocol.h")
IOC_PRIVATE_H = os.path.join(ROOT, "drivers", "ioc", "ioc_proto.h")
DRIVER_H = os.path.join(ROOT, "drivers", "hid", "hid_driver.h")
DRIVER_C = os.path.join(ROOT, "drivers", "hid", "hid.c")
# Package 41: HID is a bank-6 service image - one hot segment (the whole
# driver, as before, plus the protocol dispatch) and no boot segment.
MANIFEST = os.path.join(ROOT, "services", "manifests", "hid.json")
RECOVERY_MANIFEST = os.path.join(ROOT, "recovery", "manifest.json")
TARGET_TEST_C = os.path.join(ROOT, "tests", "target", "hid",
                             "hid_test.c")
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
            self.assertIn(name, self.env,
                          "ioc_protocol.h missing %s" % name)
            self.assertEqual(evaluate(name, self.env), value, name)

    def test_reply_maximum_matches_the_wire_bound(self):
        # metadata + translated bytes = 26 = the command-frame DATA cap.
        self.assertEqual(evaluate("IOC_HID_INPUT_META_LEN", self.env) +
                         evaluate("IOC_HID_INPUT_MAX_DATA", self.env), 26)

    def test_hid_vocabulary_is_not_duplicated_in_the_private_header(self):
        # The HID service vocabulary has exactly one owner: the public
        # client-visible contract.  The IOC driver's private header may
        # extend it but must not redeclare it.
        private = parse_c_header(IOC_PRIVATE_H)
        for name in ("IOC_CMD_HID_INPUT", "IOC_RSP_HID_INPUT",
                     "IOC_OFF_HID_INPUT_MAX", "IOC_HID_INPUT_META_LEN",
                     "IOC_HID_INPUT_MAX_DATA"):
            self.assertNotIn(name, private,
                             "IOC private header duplicates %s" % name)


class TestHidServiceArtifact(unittest.TestCase):
    def test_manifest_shape(self):
        manifest = _load_json(MANIFEST)
        self.assertEqual(manifest["name"], "hid")
        self.assertEqual(manifest["type"], "service")
        self.assertEqual(manifest["abi"], 3)
        self.assertEqual(manifest["init"], "hid_service_init")
        self.assertEqual(manifest["objects"],
                         ["hid.rel", "sdcc_indirect_calls.rel"])
        self.assertEqual([segment["role"] for segment in manifest["segments"]],
                         ["hot"])
        hot = manifest["segments"][0]
        self.assertEqual(hot["image_areas"], ["_CODE"])
        self.assertEqual(hot["bss_areas"], ["_SERVICE_IO"])
        self.assertEqual(manifest["endpoints"],
                         [{"class": 4, "instance": 0, "version": 1,
                           "dispatch": "hid_service_dispatch", "flags": 0}])
        # The IOC dependency is image metadata: a store that places HID
        # without IOC is a loader error before any HID byte runs.
        self.assertEqual(manifest["depends"],
                         [{"class": 5, "instance": 0}])

    def test_packed_container(self):
        if not os.path.exists(ZMOD):
            self.skipTest("build/modules/hid.zmod missing; build it first")
        data = _read(ZMOD, "rb")
        info = zmodpack.parse_container(data, "hid.zmod")
        self.assertEqual(info["module_type"], "service")
        self.assertEqual(info["service_abi"], 3)
        self.assertGreater(info["image_size"], 0)
        self.assertGreater(info["bss_size"], 0)
        self.assertNotEqual(info["init_offset"], 0xFFFF)
        # One hot segment: the ring and state are its only BSS, and the
        # image stays inside the service I/O window 8000h-BFFFh.
        self.assertEqual([segment["role"] for segment in info["segments"]],
                         ["hot"])
        hot = info["segments"][0]
        # The loader places the segment inside the service I/O window
        # 8000h-BFFFh, so the resident footprint must fit in it.
        self.assertLess(hot["image_size"] + hot["bss_size"], 0x4000)
        # Host-linked: the dispatch entry lies inside the hot image and
        # the container carries no relocation records.
        self.assertEqual(info["reloc_count"], 0)
        endpoint = info["endpoints"][0]
        self.assertGreaterEqual(endpoint["dispatch"], hot["base"])
        self.assertLess(endpoint["dispatch"],
                        hot["base"] + hot["image_size"])

    def test_romfs_entry_and_boot_order(self):
        manifest = _load_json(RECOVERY_MANIFEST)
        names = [entry["name"] for entry in manifest["files"]]
        self.assertEqual(names, ["sio.zmod", "ioc.zmod", "hid.zmod",
                                 "v9958.zmod", "console.zmod",
                                 "services.man", "iocfat.zmod",
                                 "monitor.zex"])
        # The boot manifest orders the services; HID's declared dependency
        # on the IOC class is what makes the loader place IOC first.
        manifest_name = _read(os.path.join(ROOT, "abi", "zos",
                                           "protocol.h"))
        self.assertIn("ZOS_CLASS_HID", manifest_name)
        main = _read(MAIN_C)
        self.assertIn("service_load_all(ZOS_SERVICE_MANIFEST_NAME)", main)
        self.assertIn("console: unbound", main)

    def test_makefile_targets_and_module_rule(self):
        makefile = _read(MAKEFILE)
        for target in ("hid-test:", "hid-selftest:"):
            self.assertIn(target, makefile)
        self.assertIn("services/manifests/%.json", makefile)
        self.assertIn("hid.rel", makefile)
        self.assertIn("ZOS_HID_SELFTEST", makefile)
        # Package 41: which services an image boots is its manifest's
        # service set, and the arena plan lists the arena modules only.
        self.assertIn("ZOS_SERVICES := sio ioc hid console", makefile)
        self.assertIn("ZOS_ARENA_MODULES", makefile)
        image_manifest = os.path.join(ROOT, "tests", "hid", "manifest.json")
        self.assertIn('"name": "console.zmod"', _read(image_manifest))

    def test_production_map_has_no_linked_hid_symbols(self):
        if not os.path.exists(MAP):
            self.skipTest("build/zephyros.map missing; build first")
        syms = _map_symbols(MAP)
        linked = [name for name in syms
                  if name.startswith("_hid_") or name.startswith("_fake")]
        self.assertEqual(linked, [],
                         "HID driver/fixture symbols must not be linked "
                         "into the kernel image: %s" % linked)


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

    def test_production_rejects_hid_acceptance_symbols(self):
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_symbols({}, {"_ace_hid_selftest_result": 0x2000},
                                   production=True)


class TestHidSourceDiscipline(unittest.TestCase):
    def setUp(self):
        self.c = _read(DRIVER_C)
        self.header = _read(DRIVER_H)
        self.sources = (self.c, self.header)
        self.service = self.c.split("static uint8_t hid_service")[1].split(
            "int hid_read_text")[0]
        self.fetch = self.c.split("static uint8_t hid_fetch")[1].split(
            "static uint8_t hid_service")[0]
        self.init = self.c.split("void hid_service_init")[1].split(
            "int16_t hid_service_dispatch")[0]

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

    def test_discovery_is_host_resolved_only(self):
        # The M8 device query is a host-resolved dependency now: the image
        # metadata declares the IOC dependency, the packer resolves it to
        # the endpoint number and the init reads it from the boot bundle -
        # no run-time lookup at all.
        self.assertIn("boot->deps[HID_DEP_IOC]", self.init)
        self.assertNotIn("lookup", self.init)
        self.assertIn("ZIOC_OP_READY", self.init)
        # The engine speaks the IOC protocol and never a driver symbol.
        for op in ("ZIOC_OP_COMMAND", "ZIOC_OP_STATUS"):
            self.assertIn(op, self.c, op)

    def test_dispatch_serves_the_hid_protocol_only(self):
        self.assertIn("hid_service_dispatch", self.c)
        for op in ("ZHID_OP_READ_TEXT", "ZHID_OP_POLL", "ZHID_OP_GETC",
                   "ZHID_OP_READY"):
            self.assertIn(op, self.c, op)
        # Input-only: the driver exposes no output path at all.
        self.assertNotIn("ZCAP_CHAR_OUT", self.c)
        self.assertNotIn("putc", self.header)
        # The ready probe is the console composition hook.
        self.assertIn("hid_state.ioc_handle", self.c)

    def test_ring_and_penalty_constants(self):
        env = parse_c_header(DRIVER_H)
        self.assertEqual(evaluate("ZHID_RING_SIZE", env), 16)
        self.assertEqual(evaluate("ZHID_STUCK_PENALTY", env), 64)
        # Power-of-two ring: wraparound is the mask expression.
        self.assertIn("ZHID_RING_SIZE - 1u", self.c)

    def test_fetch_on_doorbell_only_and_service_order(self):
        # The semantic status sample comes before any fetch, and the
        # fetch is gated on ZIOC_STATUS_HID_PENDING.
        status = self.service.index("ZIOC_OP_STATUS")
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
        self.assertIn("frame.arg1 = 1u", self.fetch)
        # Never ask for more than the local free space.
        self.assertIn("free_space", self.fetch)

    def test_reply_validation_precedes_commit(self):
        # The reply frame arrives staged in this service's own buffer, so
        # every check reads the frame words before a single byte moves.
        for needle in ("IOC_RSP_HID_INPUT", "frame.payload[2] != 0u",
                       "frame.arg1 < IOC_HID_INPUT_META_LEN",
                       "frame.arg1 > (uint8_t)(IOC_HID_INPUT_META_LEN",
                       "IOC_HID_INPUT_MAX_DATA)",
                       "payload > free_space",
                       "payload > (uint8_t)(ZHID_RING_SIZE - "
                       "state->count)"):
            self.assertIn(needle, self.fetch,
                          "fetch missing validation %r" % needle)
        # The commit (the head update) is the last thing: after every
        # check and after the one same-domain copy out of the reply frame
        # the IOC service staged (split at the ring wrap when it needs to
        # be).
        commit = self.fetch.index("state->head = (uint8_t)((state->head")
        for check in ("payload > free_space",
                      "payload > (uint8_t)(ZHID_RING_SIZE - "
                      "state->count)",
                      "hid_api->move(source, (uint16_t)(ring + insert)",
                      "hid_api->move((uint16_t)(source + first)"):
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


if __name__ == "__main__":
    unittest.main()
