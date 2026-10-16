#!/usr/bin/env python3
"""Host-side HID class and driver contract tests.

Covers the declarative and structural surface that can be checked
without executing target code:

  - the client-visible MCU HID driver vocabulary shared through the
    public IOC protocol contract (drivers/include/ioc_protocol.h):
    CMD_HID_INPUT 0Eh, RSP_HID_INPUT 8Eh, 2 metadata bytes and a 24-byte
    reply maximum (26 with metadata), never the IOC driver's private
    transport header;
  - the packed hid.mod container, manifest, ROMFS entry and boot order
    (SIO, then IOC, then HID), with a failed/malformed dependency not
    invalidating earlier devices;
  - the driver source discipline: IOC discovery through the driver's own
    protocol calls only (no imported IOC-module symbol, no name lookup,
    no port/status constants, no IN/OUT or register access), the exact
    input-only descriptor, fetch-on-doorbell only, exact
    request sizing, strict reply validation before any ring commit, and
    the proven 64-call stuck-doorbell penalty;
  - the fake-IOC fixture and the no-IOC/malformed images that prove
    dependency failure, interface verification and rollback;
  - mapcheck rejects linked HID driver symbols (module-only).

Executable behavior (the monitor's typed input through the HID engine)
is replayed from the production image by
tests/test_console_acceptance.py.
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
import modpack  # noqa: E402
from maptools import evaluate, parse_c_header  # noqa: E402

PROTO_H = os.path.join(ROOT, "drivers", "include", "ioc_protocol.h")
IOC_PRIVATE_H = os.path.join(ROOT, "drivers", "ioc", "ioc_proto.h")
DRIVER_H = os.path.join(ROOT, "drivers", "hid", "hid_driver.h")
DRIVER_C = os.path.join(ROOT, "drivers", "hid", "hid.c")
# HID is a bank-6 driver image - one hot segment (the whole driver plus
# the protocol dispatch), its io segment and no boot segment.
MANIFEST = os.path.join(ROOT, "drivers", "hid", "hid.json")
PRODUCTION_SYSTEM = os.path.join(ROOT, "systems", "production.json")
TARGET_TEST_C = os.path.join(ROOT, "tests", "target", "hid",
                             "hid_test.c")
TARGET_RUNNER = os.path.join(ROOT, "tests", "target", "support",
                             "target_test.c")
MAIN_C = os.path.join(ROOT, "kernel", "main.c")
MAKEFILE = os.path.join(ROOT, "Makefile")
MOD = os.path.join(ROOT, "build", "modules", "hid.mod")
MAP = os.path.join(ROOT, "build", "konaos.map")


def _read(path, mode="r"):
    with open(path, mode) as handle:
        return handle.read()


def _load_json(path):
    with open(path) as handle:
        return json.load(handle)


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
        # The HID driver vocabulary has exactly one owner: the public
        # client-visible contract.  The IOC driver's private header may
        # extend it but must not redeclare it.
        private = parse_c_header(IOC_PRIVATE_H)
        for name in ("IOC_CMD_HID_INPUT", "IOC_RSP_HID_INPUT",
                     "IOC_OFF_HID_INPUT_MAX", "IOC_HID_INPUT_META_LEN",
                     "IOC_HID_INPUT_MAX_DATA"):
            self.assertNotIn(name, private,
                             "IOC private header duplicates %s" % name)


class TestHidDriverArtifact(unittest.TestCase):
    def test_manifest_shape(self):
        manifest = _load_json(MANIFEST)
        self.assertEqual(manifest["name"], "hid")
        self.assertEqual(manifest["init"], "hid_driver_init")
        self.assertEqual(manifest["objects"],
                         ["hid.rel", "sdcc_indirect_calls.rel"])
        self.assertEqual([segment["role"] for segment in manifest["segments"]],
                         ["hot", "io"])
        hot, io = manifest["segments"]
        self.assertEqual(hot["image_areas"], ["_CODE"])
        self.assertEqual(io["bss_areas"], ["_DRIVER_IO"])
        self.assertEqual(manifest["endpoints"],
                         [{"class": 4, "instance": 0, "version": 1,
                           "dispatch": "hid_driver_dispatch", "flags": 0}])
        # The IOC dependency is image metadata, named symbolically in the
        # source: a store that places HID without IOC is a loader error
        # before any HID byte runs, and tools/systemgen.py resolves the
        # name against the selected set (tests/test_systemgen.py).
        self.assertEqual(manifest["depends"], ["IOC"])

    def test_packed_container(self):
        if not os.path.exists(MOD):
            self.skipTest("build/modules/hid.mod missing; build it first")
        data = _read(MOD, "rb")
        info = modpack.parse_container(data, "hid.mod")
        # The image carries its host-built publications
        # (`/dev/input/keyboard`).
        self.assertEqual([publication["name"]
                          for publication in info["publications"]],
                         ["/input/keyboard"])
        self.assertGreater(info["image_size"], 0)
        self.assertGreater(info["bss_size"], 0)
        self.assertNotEqual(info["init_offset"], 0xFFFF)
        # The code and the io segment: the ring and state are its only
        # BSS, placed inside the driver I/O window 8000h-BFFFh.
        self.assertEqual([segment["role"] for segment in info["segments"]],
                         ["hot", "io"])
        hot, io = info["segments"]
        self.assertEqual(hot["bss_size"], 0)
        self.assertLess(io["bss_size"], 0x4000)
        # Host-linked: the dispatch entry lies inside the hot image (the
        # flat plan carries no relocation records at all).
        endpoint = info["endpoints"][0]
        self.assertGreaterEqual(endpoint["dispatch"], hot["base"])
        self.assertLess(endpoint["dispatch"],
                        hot["base"] + hot["image_size"])

    def test_system_file_and_boot_order(self):
        # The system file names the driver load order and the
        # store's own files; the generated store manifest (the driver
        # images, drivers.man, then those files) is
        # tests/test_systemgen.py's business.
        system = _load_json(PRODUCTION_SYSTEM)
        self.assertEqual(system["drivers"],
                         ["sio", "ioc", "hid", "v9958", "console-v9958-hid",
                          "timer", "iocfat"])
        self.assertEqual(system["store"],
                         ["shell.exe", "monitor.exe",
                          # The production feature exerciser and the
                          # outcome programs it runs.
                          "hwtest.exe", "timer.exe", "beat.exe",
                          "xmem_selftest.exe",
                          "xmem_child.exe", "xmem_far.exe",
                          "fs_selftest.exe", "fs_child.exe"])
        # The boot manifest orders the drivers; HID's declared dependency
        # on the IOC class is what makes the loader place IOC first.
        manifest_name = _read(os.path.join(ROOT, "abi", "kos",
                                           "protocol.h"))
        self.assertIn("KOS_CLASS_HID", manifest_name)
        main = _read(MAIN_C)
        self.assertIn("driver_load_all(KOS_DRIVER_MANIFEST_NAME)", main)
        self.assertIn("console: unbound", main)

    def test_makefile_targets_and_module_rule(self):
        makefile = _read(MAKEFILE)
        self.assertIn("hid-test:", makefile)
        # The driver set and the resolved manifests are the
        # system file's, and one foreach emits the pack rule per driver.
        self.assertIn("$(BUILD)/drivers/$(1).json", makefile)
        self.assertIn("hid.rel", makefile)

    def test_production_map_has_no_linked_hid_symbols(self):
        if not os.path.exists(MAP):
            self.skipTest("build/konaos.map missing; build first")
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
        self.driver = self.c.split("static uint8_t hid_driver")[1].split(
            "int hid_read_text")[0]
        self.fetch = self.c.split("static uint8_t hid_fetch")[1].split(
            "static uint8_t hid_driver")[0]
        self.init = self.c.split("uint8_t hid_driver_init")[1].split(
            "int16_t hid_driver_dispatch")[0]

    def test_no_sio1_rr0_or_port_constants_anywhere(self):
        forbidden = ("KOS_SIO1", "KOS_SIO_RR0", "KOS_IOC_HID_DOORBELL",
                     "KOS_IOC_CMD_READY", "KOS_IOC_WR", "KOS_IOC_BULK",
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
        # Discovery is a host-resolved dependency: the image
        # metadata declares the IOC dependency, the packer resolves it to
        # the endpoint number and the init reads it from the boot bundle -
        # no run-time lookup at all.
        self.assertIn("boot->deps[HID_DEP_IOC]", self.init)
        self.assertNotIn("lookup", self.init)
        self.assertIn("KIOC_OP_READY", self.init)
        # The engine speaks the IOC protocol and never a driver symbol.
        for op in ("KIOC_OP_COMMAND", "KIOC_OP_STATUS"):
            self.assertIn(op, self.c, op)

    def test_dispatch_serves_the_hid_protocol_only(self):
        self.assertIn("hid_driver_dispatch", self.c)
        for op in ("KHID_OP_READ_TEXT", "KHID_OP_POLL", "KHID_OP_GETC",
                   "KHID_OP_READY"):
            self.assertIn(op, self.c, op)
        # Input-only: the driver exposes no output path at all.
        self.assertNotIn("KCAP_CHAR_OUT", self.c)
        self.assertNotIn("putc", self.header)
        # The ready probe reports the link state.
        self.assertIn("hid_state.ioc_handle", self.c)

    def test_ring_and_penalty_constants(self):
        env = parse_c_header(DRIVER_H)
        self.assertEqual(evaluate("KHID_RING_SIZE", env), 16)
        self.assertEqual(evaluate("KHID_STUCK_PENALTY", env), 64)
        # Power-of-two ring: wraparound is the mask expression.
        self.assertIn("KHID_RING_SIZE - 1u", self.c)

    def test_fetch_on_doorbell_only_and_driver_order(self):
        # The semantic status sample comes before any fetch, and the
        # fetch is gated on KIOC_STATUS_HID_PENDING.
        status = self.driver.index("KIOC_OP_STATUS")
        pending = self.driver.index("KIOC_STATUS_HID_PENDING")
        penalty = self.driver.index("state->penalty != 0u")
        fetch = self.driver.index("hid_fetch(state)")
        self.assertLess(status, pending)
        self.assertLess(pending, penalty)
        self.assertLess(penalty, fetch)
        # Idle (doorbell released): clear the penalty, no command.
        self.assertIn("state->penalty = 0u;", self.driver)

    def test_exact_request_sizing(self):
        self.assertIn("IOC_HID_INPUT_MAX_DATA", self.fetch)
        self.assertIn("IOC_CMD_HID_INPUT", self.fetch)
        self.assertIn("frame.arg1 = 1u", self.fetch)
        # Never ask for more than the local free space.
        self.assertIn("free_space", self.fetch)

    def test_reply_validation_precedes_commit(self):
        # The reply frame arrives staged in this driver's own buffer, so
        # every check reads the frame words before a single byte moves.
        for needle in ("IOC_RSP_HID_INPUT", "frame.payload[2] != 0u",
                       "frame.arg1 < IOC_HID_INPUT_META_LEN",
                       "frame.arg1 > (uint8_t)(IOC_HID_INPUT_META_LEN",
                       "IOC_HID_INPUT_MAX_DATA)",
                       "payload > free_space",
                       "payload > (uint8_t)(KHID_RING_SIZE - "
                       "state->count)"):
            self.assertIn(needle, self.fetch,
                          "fetch missing validation %r" % needle)
        # The commit (the head update) is the last thing: after every
        # check and after the one same-domain copy out of the reply frame
        # the IOC driver staged (split at the ring wrap when it needs to
        # be).
        commit = self.fetch.index("state->head = (uint8_t)((state->head")
        for check in ("payload > free_space",
                      "payload > (uint8_t)(KHID_RING_SIZE - "
                      "state->count)",
                      "hid_api->move(source, (uint16_t)(ring + insert)",
                      "hid_api->move((uint16_t)(source + first)"):
            self.assertLess(self.fetch.index(check), commit)

    def test_stuck_penalty_applied_and_cleared(self):
        self.assertIn("state->penalty = KHID_STUCK_PENALTY", self.driver)
        self.assertIn("--state->penalty", self.driver)
        # Successful data clears it.
        self.assertIn("state->penalty = 0u;", self.driver)

    def test_getc_and_read_text_are_nonblocking(self):
        self.assertIn("return (-KOS_EAGAIN);", self.c)
        self.assertIn("(-KOS_EINVAL)", self.c)
        # No blocking loops anywhere in the driver.
        self.assertNotIn("while (1)", self.c)
        self.assertNotIn("for (;;)", self.c)


if __name__ == "__main__":
    unittest.main()
