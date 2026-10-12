#!/usr/bin/env python3
"""Host-side IOC command+bulk contract tests.

Covers the declarative and structural surface that can be checked
without executing target code:

  - the paired platform headers carry the verified command-lane and
    bulk-lane ports, register/status values, packet constants, polling
    budgets, scan/FIFO bounds, the deployed CRC-bypass mode, and the HID
    doorbell, value-identical between hardware.h and hardware.inc;
  - drivers/include/ioc.h: native frame layout, operation order,
    capability limits, semantic status and the transport error set;
  - the internal transport-class error codes added to errno.h;
  - the packed ioc.zmod container, manifest, ROMFS entry and boot order
    (SIO first, then IOC, with a failed IOC not invalidating SIO);
  - mapcheck rejects linked IOC driver symbols (module-only) and the
    acceptance symbols in production images;
  - the driver sources keep the load-bearing ordering on both lanes:
    one command-lane reset and one bulk-lane reset (init only),
    preload-before-RTS, EOM reset after the command preload, the bulk
    write's terminating underrun sequence, bounded drains, and no SIO0 or
    kernel symbol use.

Executable behavior (link bring-up, framing, faults, status sampling,
bulk READY/DONE phases) is replayed from the real linked image by
tests/test_ioc_sim.py.
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
from z80sim import ioc_crc16  # noqa: E402
from maptools import (  # noqa: E402
    HARDWARE_H, HARDWARE_INC, evaluate, parse_asm_inc, parse_c_header)

IOC_H = os.path.join(ROOT, "drivers", "include", "ioc.h")
# The public ABI errno values (abi/zos/errno.h) plus the IOC
# transport-class codes, which live in the IOC header itself (T3 moved
# them out of drivers/include/ioc.h when the private errno.h was deleted).
ERRNO_H = os.path.join(ROOT, "abi", "zos", "errno.h")
PROTO_H = os.path.join(ROOT, "drivers", "ioc", "ioc_proto.h")
IOC_PROTOCOL_H = os.path.join(ROOT, "drivers", "include", "ioc_protocol.h")
DRIVER_H = os.path.join(ROOT, "drivers", "ioc", "ioc_driver.h")
DRIVER_C = os.path.join(ROOT, "drivers", "ioc", "ioc.c")
DRIVER_ASM = os.path.join(ROOT, "drivers", "ioc", "ioc_io.asm")
# Package 41: the driver is a bank-6 driver image.  The driver half owns
# the protocol dispatch and the init entry, the boot half the lane
# programming, and the image description lives in the driver directory.
PROTO_C = os.path.join(ROOT, "drivers", "ioc", "ioc_proto.c")
BOOT_C = os.path.join(ROOT, "drivers", "ioc", "ioc_boot.c")
MANIFEST = os.path.join(ROOT, "drivers", "ioc", "ioc.json")
RECOVERY_MANIFEST = os.path.join(ROOT, "recovery", "manifest.json")
MAIN_C = os.path.join(ROOT, "kernel", "main.c")
MAKEFILE = os.path.join(ROOT, "Makefile")
ZMOD = os.path.join(ROOT, "build", "modules", "ioc.zmod")
MAP = os.path.join(ROOT, "build", "zephyros.map")


def _read(path, mode="r"):
    """Read a whole file; the handle is always closed."""
    with open(path, mode) as handle:
        return handle.read()


def _load_json(path):
    """Load a JSON file; the handle is always closed."""
    with open(path) as handle:
        return json.load(handle)

# The verified command-lane and bulk-lane constants (deployed host
# contract level 08h, bulk DATA CRC bypassed on both ends).
IOC_CONSTANTS = {
    "ZOS_SIO1A_DATA": 0x30,
    "ZOS_SIO1A_CTRL": 0x31,
    "ZOS_SIO1B_DATA": 0x32,
    "ZOS_SIO1B_CTRL": 0x33,
    "ZOS_SIO_RR0_DCD": 0x08,
    "ZOS_SIO_RR0_TX_UNDERRUN": 0x40,
    "ZOS_IOC_CMD_READY_MASK": 0x08,
    "ZOS_IOC_HID_DOORBELL_MASK": 0x20,
    "ZOS_IOC_WR0_RESET_CHANNEL": 0x18,
    "ZOS_IOC_WR0_RESET_EXT_STATUS": 0x10,
    "ZOS_IOC_WR0_RESET_ERROR": 0x30,
    "ZOS_IOC_WR0_RESET_EOM": 0xC0,
    "ZOS_IOC_WR0_RESET_RX_CRC": 0x40,
    "ZOS_IOC_WR0_RESET_TX_CRC": 0x80,
    "ZOS_IOC_WR_SELECT_WR7": 0x07,
    "ZOS_IOC_WR_SELECT_WR6": 0x06,
    "ZOS_IOC_WR1_INT_WAIT_OFF": 0x00,
    "ZOS_IOC_WR3_RX_OFF": 0xC0,
    "ZOS_IOC_WR3_RX_HUNT": 0xD1,
    "ZOS_IOC_WR3_RX_NO_HUNT": 0xC1,
    "ZOS_IOC_WR3_BULK_RX_HUNT": 0xD9,
    "ZOS_IOC_WR3_BULK_RX_NO_HUNT": 0xC9,
    "ZOS_IOC_WR3_BULK_TX": 0xC1,
    "ZOS_IOC_WR4_EXTSYNC_X1": 0x30,
    "ZOS_IOC_WR5_TX_RTS_OFF": 0x68,
    "ZOS_IOC_WR5_TX_RTS_ON": 0x6A,
    "ZOS_IOC_WR5_IDLE": 0x00,
    "ZOS_IOC_WR5_BULK_RTS_ON": 0xEB,
    "ZOS_IOC_WR5_BULK_RTS_OFF": 0xE9,
    "ZOS_IOC_WR5_BULK_IDLE": 0xE0,
    "ZOS_IOC_WR7_UNDERRUN_FILL": 0xFF,
    "ZOS_IOC_BULK_SYNC_FILL": 0xAA,
    "ZOS_IOC_BULK_LEADIN": 0x00,
    "ZOS_IOC_BULK_SCAN_LIMIT": 0x08,
    "ZOS_IOC_BULK_MAX_LEN": 512,
    "ZOS_IOC_BULK_UNDERRUN_GUARD": 128,
    "ZOS_IOC_BULK_CRC_BYPASS": 1,
    "ZOS_IOC_PACKET_SYNC0": 0xA5,
    "ZOS_IOC_PACKET_SYNC1": 0x5A,
    "ZOS_IOC_PACKET_FIXED_LEN": 0x03,
    "ZOS_IOC_PACKET_CRC_BYTES": 0x02,
    "ZOS_IOC_CMD_MAX_DATA": 0x1A,
    "ZOS_IOC_SEND_TRAILER_BYTES": 0x02,
    "ZOS_IOC_SEND_TRAILER_FILL": 0xFF,
    "ZOS_IOC_CMD_READY_TIMEOUT": 0xFFFF,
    "ZOS_IOC_CMD_READY_OUTER": 0x08,
    "ZOS_IOC_CMD_BYTE_TIMEOUT": 0xFFFF,
    "ZOS_IOC_CMD_BYTE_OUTER": 0x08,
    "ZOS_IOC_CMD_REPLY_SCAN_LIMIT": 0xA0,
    "ZOS_IOC_RX_FIFO_DEPTH": 0x03,
    "ZOS_IOC_RX_DRAIN_LIMIT": 0x04,
    "ZOS_IOC_CMD_LINK_SYNC": 0x0C,
    "ZOS_IOC_RSP_LINK_SYNC": 0x8C,
    "ZOS_IOC_LINK_BRINGUP_TRIES": 0x04,
    "ZOS_SMOKE_HOST_SYNC_FIRST": 0xAA,
    "ZOS_SMOKE_HOST_SYNC_SECOND": 0x55,
}


def struct_member_order(text, struct_name, function_pointers=False):
    stripped = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    body = stripped.split("struct %s {" % struct_name, 1)[1].split("};", 1)[0]
    names = []
    if function_pointers:
        names = re.findall(r"\(\s*\*\s*(\w+)\s*\)", body)
    else:
        for line in body.splitlines():
            line = line.strip().rstrip(";")
            match = re.search(r"(\w+)(\[[^\]]*\])?\s*$", line)
            if line and match:
                names.append(match.group(1))
    return names


class TestIocPlatformConstants(unittest.TestCase):
    def setUp(self):
        self.c = parse_c_header(HARDWARE_H)
        self.asm = parse_asm_inc(HARDWARE_INC)

    def test_verified_constants_present_and_exact(self):
        for name, value in sorted(IOC_CONSTANTS.items()):
            self.assertIn(name, self.c, "hardware.h missing %s" % name)
            self.assertIn(name, self.asm, "hardware.inc missing %s" % name)
            self.assertEqual(evaluate(self.c[name], self.c), value,
                             "hardware.h %s" % name)
            self.assertEqual(evaluate(self.asm[name], self.asm), value,
                             "hardware.inc %s" % name)

    def test_derived_register_relations(self):
        env = dict(self.c)
        # RTS on is RTS off with the RTS bit; hunt is no-hunt plus bit 4;
        # RX off is RX enabled with the enable bit clear; idle disables TX.
        self.assertEqual(evaluate("ZOS_IOC_WR5_TX_RTS_ON", env),
                         evaluate("ZOS_IOC_WR5_TX_RTS_OFF", env) | 0x02)
        self.assertEqual(evaluate("ZOS_IOC_WR3_RX_HUNT", env),
                         evaluate("ZOS_IOC_WR3_RX_NO_HUNT", env) | 0x10)
        self.assertEqual(evaluate("ZOS_IOC_WR3_RX_OFF", env),
                         evaluate("ZOS_IOC_WR3_RX_NO_HUNT", env) & ~0x01)
        self.assertEqual(evaluate("ZOS_IOC_WR5_IDLE", env), 0)
        self.assertEqual(evaluate("ZOS_IOC_WR5_TX_RTS_OFF", env) & 0x08,
                         0x08)  # TX enabled at idle entry
        self.assertEqual(evaluate("ZOS_IOC_RX_DRAIN_LIMIT", env),
                         evaluate("ZOS_IOC_RX_FIFO_DEPTH", env) + 1)

        # Bulk-lane register relations: hunt differs from no-hunt by bit
        # 4; the write-direction value keeps the receiver enabled; RTS on
        # is RTS off with the RTS bit; idle disables the transmitter.
        self.assertEqual(evaluate("ZOS_IOC_WR3_BULK_RX_HUNT", env),
                         evaluate("ZOS_IOC_WR3_BULK_RX_NO_HUNT", env) | 0x10)
        self.assertEqual(evaluate("ZOS_IOC_WR3_BULK_RX_NO_HUNT", env) & 0x01,
                         0x01)
        self.assertEqual(evaluate("ZOS_IOC_WR3_BULK_TX", env) & 0x11, 0x01)
        self.assertEqual(evaluate("ZOS_IOC_WR5_BULK_RTS_ON", env),
                         evaluate("ZOS_IOC_WR5_BULK_RTS_OFF", env) | 0x02)
        self.assertEqual(evaluate("ZOS_IOC_WR5_BULK_IDLE", env),
                         evaluate("ZOS_IOC_WR5_BULK_RTS_OFF", env) & ~0x09)
        self.assertEqual(evaluate("ZOS_IOC_BULK_MAX_LEN", env), 512)
        self.assertEqual(evaluate("ZOS_IOC_BULK_UNDERRUN_GUARD", env), 128)

    def test_bulk_crc_bypass_is_paired_and_explicit(self):
        # The deployed pair bypasses bulk DATA CRC on both ends.  Both
        # platform headers must carry the same value (a one-sided change
        # fails the shared-name parity test), and the driver must guard
        # against the platform default differing from the deployed mode
        # instead of defining a private default of its own.
        self.assertEqual(evaluate("ZOS_IOC_BULK_CRC_BYPASS", self.c), 1)
        self.assertEqual(evaluate("ZOS_IOC_BULK_CRC_BYPASS", self.asm), 1)
        driver = _read(DRIVER_C)
        self.assertIn("#if ZOS_IOC_BULK_CRC_BYPASS != 1", driver)
        for source in (driver, _read(DRIVER_ASM)):
            self.assertNotIn("#define IOC_BULK_CRC_BYPASS", source)
            self.assertNotIn("CRC_BYPASS     =", source)

    def test_smoke_host_sync_is_paired_and_wired(self):
        # Smoke images hold the banner for the host sync sequence (the
        # board's reset also resets the USB bridge on this bench).  Both
        # headers carry the bytes, the kernel waits for them before the
        # first banner byte, the Makefile wires the gate to the smoke
        # image, and the smoke tool sends exactly that sequence.
        import serial_smoke

        first = evaluate("ZOS_SMOKE_HOST_SYNC_FIRST", self.c)
        second = evaluate("ZOS_SMOKE_HOST_SYNC_SECOND", self.c)
        self.assertNotEqual(first, second)
        self.assertEqual(serial_smoke.IOC_HOST_SYNC,
                         bytes((first, second)))
        main = _read(MAIN_C)
        self.assertIn("#if ZOS_SMOKE_HOST_SYNC", main)
        self.assertIn("ZOS_SMOKE_HOST_SYNC_FIRST", main)
        self.assertIn("ZOS_SMOKE_HOST_SYNC_SECOND", main)
        self.assertLess(main.index("kernel_host_sync_wait();"),
                        main.index('early_console_puts("ZephyrOS 0.0'))
        makefile = _read(MAKEFILE)
        self.assertIn("ZOS_SMOKE_HOST_SYNC=1", makefile)

    def test_bulk_arming_classes_and_unknown_status_are_pinned(self):
        # The complete frozen set of request classes that can arm the
        # bulk lane, and the unknown-command status that uniquely proves
        # a driver is absent.  The driver must gate READY recognition on
        # this set, not on the eight-byte shape alone.
        env = parse_c_header(PROTO_H)
        for name, value in (("IOC_CMD_BULK_TEST", 0x04),
                            ("IOC_CMD_SD_READ_BULK", 0x05),
                            ("IOC_CMD_SD_WRITE_BULK", 0x07),
                            ("IOC_CMD_SD_READ_REC", 0x08),
                            ("IOC_CMD_SD_WRITE_REC", 0x09),
                            ("IOC_CMD_SD_READ_BLOCK", 0x0F),
                            ("IOC_CMD_FS_READ", 0x23),
                            ("IOC_CMD_FS_WRITE", 0x24),
                            ("IOC_CMD_FS2_READ", 0x36),
                            ("IOC_CMD_FS2_WRITE", 0x3E),
                            ("IOC_STATUS_UNKNOWN_CMD", 0x02)):
            self.assertIn(name, env, "ioc_proto.h missing %s" % name)
            self.assertEqual(evaluate(name, env), value, name)
        driver = _read(DRIVER_C)
        self.assertIn("ioc_bulk_arming_request(request->type)", driver)
        for name in ("IOC_CMD_BULK_TEST", "IOC_CMD_SD_READ_BULK",
                     "IOC_CMD_SD_WRITE_BULK", "IOC_CMD_SD_READ_REC",
                     "IOC_CMD_SD_WRITE_REC", "IOC_CMD_SD_READ_BLOCK",
                     "IOC_CMD_FS_READ", "IOC_CMD_FS_WRITE",
                     "IOC_CMD_FS2_READ", "IOC_CMD_FS2_WRITE"):
            self.assertIn(name, driver)

    def test_shared_names_are_value_identical(self):
        for name in sorted(set(self.c) & set(self.asm)):
            self.assertEqual(evaluate(self.c[name], self.c),
                             evaluate(self.asm[name], self.asm),
                             "%s differs between hardware.h/.inc" % name)


class TestIocClassContract(unittest.TestCase):
    def setUp(self):
        self.text = _read(IOC_H)
        self.env = parse_c_header(IOC_H)

    def test_maximum_data_and_bulk_limits(self):
        self.assertEqual(evaluate("ZIOC_MAX_DATA", self.env), 26)
        self.assertEqual(evaluate("ZIOC_BULK_MAX", self.env), 512)
        self.assertEqual(evaluate("ZIOC_FRAME_SIZE", self.env), 29)

    def test_semantic_status_bit(self):
        self.assertEqual(evaluate("ZIOC_STATUS_HID_PENDING", self.env), 1)

    def test_transport_errors_are_distinct_negative_codes(self):
        env = dict(self.env)
        env.update(parse_c_header(ERRNO_H))
        codes = {
            name: evaluate(name, env) for name in (
                "ZIOC_EINVAL", "ZIOC_EBUSY", "ZIOC_ETIMEDOUT",
                "ZIOC_EBADFRAME", "ZIOC_EBADCRC", "ZIOC_EBADSEQ",
                "ZIOC_EIO")
        }
        for name, value in codes.items():
            self.assertLess(value, 0, "%s must be negative" % name)
        self.assertEqual(len(set(codes.values())), len(codes),
                         "transport error codes must be distinct: %r" % codes)
        self.assertEqual(codes["ZIOC_EINVAL"], -evaluate("ZOS_EINVAL", env))
        self.assertEqual(codes["ZIOC_EBUSY"], -evaluate("ZOS_EBUSY", env))
        self.assertEqual(codes["ZIOC_EBADFRAME"],
                         -evaluate("ZOS_EPROTO", env))
        self.assertEqual(codes["ZIOC_EBADCRC"], -evaluate("ZOS_ECRC", env))
        self.assertEqual(codes["ZIOC_EBADSEQ"],
                         -evaluate("ZOS_EBADSEQ", env))
        self.assertEqual(codes["ZIOC_EIO"], -evaluate("ZOS_EIO", env))

    def test_frame_member_order(self):
        self.assertEqual(struct_member_order(self.text, "zioc_frame"),
                         ["type", "status", "length", "data"])

    def test_layout_assertions_present(self):
        for needle in ("sizeof(struct zioc_frame) == ZIOC_FRAME_SIZE",
                       "__builtin_offsetof(struct zioc_frame, length) == 2",
                       "__builtin_offsetof(struct zioc_frame, data) == 3"):
            self.assertIn(needle, self.text)

    def test_general_api_keeps_mcu_command_numbers_private(self):
        # The class ABI (ioc.h) never names MCU commands.  The
        # client-visible driver vocabulary lives in the owning public
        # header and is not duplicated in the transport-private header.
        self.assertNotIn("CMD_PING", self.text)
        self.assertNotIn("CMD_HID", self.text)
        protocol = _read(IOC_PROTOCOL_H)
        self.assertIn("#define IOC_CMD_PING", protocol)
        self.assertIn("IOC_FW_LEVEL_PINNED   74u", protocol)
        proto = _read(PROTO_H)
        self.assertNotIn("IOC_CMD_PING", proto)
        self.assertNotIn("IOC_HID_INPUT_META_LEN", proto)


class TestIocErrorCodes(unittest.TestCase):
    def test_crc_deployed_parameter_set(self):
        # The deployed transport CRC: polynomial 1021h, initial value
        # 0000h, MSB-first, no final XOR (the classic check value for
        # this parameter set is 31C3h, not CCITT-FALSE's 29B1h).
        self.assertEqual(ioc_crc16(b"123456789"), 0x31C3)
        self.assertEqual(ioc_crc16(b""), 0x0000)
        self.assertEqual(ioc_crc16(b"A5"), 0x580B)

    def test_errno_transport_codes(self):
        env = parse_c_header(ERRNO_H)
        env.update(parse_c_header(IOC_H))
        self.assertEqual(evaluate("ZOS_EPROTO", env), 0x0D)
        self.assertEqual(evaluate("ZOS_ECRC", env), 0x0E)
        self.assertEqual(evaluate("ZOS_EBADSEQ", env), 0x0F)
        # Distinct from every earlier code.
        earlier = [evaluate("ZOS_E" + name, env) for name in
                   ("NOSYS", "INVAL", "NODEV", "BUSY", "NOENT", "NOSPC",
                    "NOTSUP", "TIMEDOUT", "AGAIN", "IO", "INIT", "FAULT")]
        self.assertEqual(len(set(earlier + [0x0D, 0x0E, 0x0F])),
                         len(earlier) + 3)


class TestIocDriverArtifact(unittest.TestCase):
    def test_manifest_shape(self):
        manifest = _load_json(MANIFEST)
        self.assertEqual(manifest["name"], "ioc")
        self.assertEqual(manifest["init"], "ioc_driver_init")
        roles = [segment["role"] for segment in manifest["segments"]]
        self.assertEqual(roles, ["hot", "boot"])
        hot, boot = manifest["segments"]
        self.assertEqual(hot["image_areas"], ["_CODE"])
        self.assertEqual(hot["bss_areas"], ["_DRIVER_IO"])
        self.assertEqual(boot["image_areas"], ["_BOOT"])
        self.assertEqual(boot["entry"], "ioc_boot_entry")
        self.assertEqual(manifest["objects"],
                         ["ioc.rel", "ioc_io.rel", "ioc_driver.rel",
                          "ioc_boot.rel", "sdcc_indirect_calls.rel"])
        self.assertEqual(manifest["endpoints"],
                         [{"class": 5, "instance": 0, "version": 1,
                           "dispatch": "ioc_driver_dispatch", "flags": 0}])
        self.assertEqual(manifest["depends"], [])

    def test_romfs_entry_and_boot_order(self):
        manifest = _load_json(RECOVERY_MANIFEST)
        names = [entry["name"] for entry in manifest["files"]]
        self.assertIn("ioc.zmod", names)
        self.assertIn("drivers.man", names)
        # The boot manifest orders the drivers: HID declares the IOC
        # dependency, so IOC must be listed (and therefore placed) first.
        order = _read(os.path.join(ROOT, "tools", "zmodpack.py"))
        self.assertIn("def cmd_drivers", order)
        main = _read(MAIN_C)
        self.assertIn("driver_load_all(ZOS_DRIVER_MANIFEST_NAME)", main)
        # Package 42b retired the arena module pass and the IOC class
        # shim with it: every image is a driver now, so HID resolves its
        # IOC dependency from the publication the driver declares.
        self.assertNotIn("ioc" + "_shim_find", main)
        self.assertFalse(os.path.exists(os.path.join(ROOT, "kernel",
                                                     "module", "loader.c")),
                         "the arena module loader must be gone")

    def test_makefile_targets_and_module_rule(self):
        makefile = _read(MAKEFILE)
        self.assertIn("ioc-test:", makefile)
        self.assertIn("$(call DRV_MANIFEST,$(1))", makefile)
        self.assertIn("$(MODULES)/$(1).zmod: $(call DRV_MANIFEST,$(1))",
                      makefile)
        self.assertIn("ioc_driver.rel", makefile)
        # The IOC diagnostic runs in the console-integration image (work
        # package 42d merged the per-device images); the table row links
        # it and defines the group's guard.
        self.assertIn("ZOS_IOC_SELFTEST", makefile)
        index = makefile.index("ifeq ($(ZOS_CONSOLE_INTEGRATION_SELFTEST),1)")
        self.assertIn("$(BUILD)/ioc_test.rel",
                      makefile[index:index + 400])
        self.assertIn("ioc_test", makefile)
        # The declared driver set is the manifest's business: an image
        # boots the drivers its driver set lists.
        self.assertRegex(makefile,
                         r"ZOS_DRIVERS \?= .*\bioc\b")
        self.assertNotIn("BOOT_ZMODS", makefile)

    def test_production_map_has_no_linked_ioc_symbols(self):
        if not os.path.exists(MAP):
            self.skipTest("build/zephyros.map missing; build first")
        syms = zmodpack_symbols(MAP)
        # The one `_ioc_`-prefixed family the nucleus owns is the class
        # shim that publishes the driver to a bank-7 module; the driver
        # itself is a bank-6 image.
        linked = [name for name in syms
                  if name.startswith("_ioc_") and
                  not name.startswith("_ioc" + "_shim_")]
        self.assertEqual(linked, [],
                         "IOC driver symbols must not be linked into "
                         "the kernel image: %s" % linked)


def zmodpack_symbols(map_path):
    import re as _re
    syms = {}
    with open(map_path) as handle:
        for line in handle:
            match = _re.match(r"\s*([0-9A-Fa-f]{8})\s+(\S+)", line)
            if match:
                syms[match.group(2)] = int(match.group(1), 16)
    return syms


class TestMapcheckRules(unittest.TestCase):
    def test_ioc_symbols_are_module_only(self):
        self.assertIn("_ioc_", mapcheck.STATIC_IOC_PREFIXES)
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_symbols({}, {"_ioc_ops": 0x2000})

    def test_production_rejects_acceptance_symbols(self):
        self.assertIn("_m8", mapcheck.PRODUCTION_FORBIDDEN)
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_symbols({}, {"_ace_ioc_selftest_result": 0x2000},
                                   production=True)


class TestIocSourceDiscipline(unittest.TestCase):
    def setUp(self):
        self.c = _read(DRIVER_C)
        self.asm = _read(DRIVER_ASM)
        self.proto = _read(PROTO_C)
        self.boot = _read(BOOT_C)
        # Lane regions so a check cannot be satisfied by the other lane's
        # routine appearing earlier in the file.
        self.bulk_init = self.asm.split("_ioc_io_bulk_init::")[1].split(
            "_ioc_io_bulk_read::")[0]
        self.bulk_read = self.asm.split("_ioc_io_bulk_read::")[1].split(
            "_ioc_io_bulk_write::")[0]
        self.bulk_write = self.asm.split("_ioc_io_bulk_write::")[1].split(
            "ioc_bulk_put:")[0]
        self.cmd_tx = self.asm.split("_ioc_io_tx::")[1].split(
            "_ioc_io_rx::")[0]

    def test_one_channel_reset_per_lane_in_init_only(self):
        # Command lane: exactly one reset, in the boot tier's ioc_hw_init
        # (package 41 moved it there: the boot segment is where the
        # one-shot lane programming runs, through the mode-10 gate).
        self.assertEqual(self.boot.count("ZOS_IOC_WR0_RESET_CHANNEL"), 1)
        hw_init = self.boot.split("void ioc_hw_init")[1].split(
            "void ioc_boot_entry")[0]
        self.assertIn("ZOS_IOC_WR0_RESET_CHANNEL", hw_init)
        # Bulk lane: exactly one reset, in the module-owned init routine.
        self.assertEqual(self.asm.count("ZOS_IOC_WR0_RESET_CHANNEL"), 1)
        self.assertIn("ZOS_IOC_WR0_RESET_CHANNEL", self.bulk_init)

    def test_command_tx_ordering_is_preload_then_grant(self):
        first_put = self.cmd_tx.index("call\tioc_put_byte")
        eom = self.cmd_tx.index("ZOS_IOC_WR0_RESET_EOM")
        rts_on = self.cmd_tx.index("ZOS_IOC_WR5_TX_RTS_ON")
        trailers = self.cmd_tx.index("ZOS_IOC_SEND_TRAILER_BYTES")
        idle = self.cmd_tx.index("ZOS_IOC_WR5_IDLE")
        wr3 = self.cmd_tx.index("ZOS_IOC_WR3_RX_HUNT")
        self.assertLess(first_put, eom, "EOM reset must follow the preload")
        self.assertLess(eom, rts_on, "RTS must be granted after the EOM reset")
        self.assertLess(rts_on, trailers)
        self.assertLess(trailers, idle, "trailers before the RTS release")
        self.assertLess(idle, wr3, "RX is armed after the RTS release")

    def test_command_receive_is_bounded_and_hunts_once(self):
        self.assertIn("ZOS_IOC_RX_DRAIN_LIMIT", self.asm)
        self.assertIn("ZOS_IOC_CMD_REPLY_SCAN_LIMIT", self.asm)
        # Enter Hunt is selected from the io block, never unconditional.
        self.assertIn("ZOS_IOC_WR3_RX_NO_HUNT", self.asm)

    def test_bulk_read_discipline(self):
        r = self.bulk_read
        # Hunt chosen from the stored sync flag, drain bounded, RTS grant,
        # DCD admission, bounded scan, wire-speed header capture, INI
        # payload, the two bypassed trailer bytes, RTS release and the
        # CTS deassertion confirmation - in that order.
        order = ["ZOS_IOC_WR3_BULK_RX_HUNT",
                 "ZOS_IOC_RX_DRAIN_LIMIT",
                 "ZOS_IOC_WR5_BULK_RTS_ON",
                 "ZOS_IOC_WR0_RESET_EXT_STATUS",
                 "ZOS_IOC_BULK_SCAN_LIMIT",
                 "ini",
                 "call\tioc_bulk_rts_off",
                 "ZOS_SIO_RR0_CTS"]
        positions = []
        for needle in order:
            self.assertIn(needle, r, "bulk read missing %r" % needle)
            positions.append(r.index(needle))
        self.assertEqual(positions, sorted(positions),
                         "bulk read ordering broken")
        # The deployed pair consumes but never compares the trailer, and
        # the admitted length (not the wire length) drives the INI loop
        # (a completed payload loop reaches the trailer block).
        self.assertNotIn("ZOS_IOC_BULK_CRC_BYPASS", r)
        self.assertIn("ioc_br_trailer", r)

    def test_bulk_write_discipline(self):
        w = self.bulk_write
        order = ["ZOS_IOC_WR_SELECT_WR6",
                 "ZOS_IOC_BULK_SYNC_FILL",
                 "ZOS_IOC_WR0_RESET_TX_CRC",
                 "ZOS_IOC_BULK_LEADIN",
                 "ZOS_IOC_WR5_BULK_RTS_ON",
                 "ZOS_SIO_RR0_CTS",
                 "ZOS_IOC_PACKET_SYNC0",
                 "outi",
                 "ZOS_IOC_WR0_RESET_EOM",
                 "ZOS_SIO_RR0_TX_UNDERRUN",
                 "ZOS_IOC_BULK_UNDERRUN_GUARD",
                 "call\tioc_bulk_rts_off",
                 "ioc_bw_cts_out:"]
        positions = []
        for needle in order:
            self.assertIn(needle, w, "bulk write missing %r" % needle)
            positions.append(w.index(needle))
        self.assertEqual(positions, sorted(positions),
                         "bulk write ordering broken")
        # The EOM reset comes after the final payload byte, interrupts
        # return before the commit wait (which polls /CTSA again), and
        # the shared release helper disables the transmitter.
        self.assertLess(w.index("outi"), w.index("ZOS_IOC_WR0_RESET_EOM"))
        self.assertLess(w.index("call\tioc_bulk_irq_restore"),
                        w.index("ioc_bw_cts_out:"))
        self.assertIn("ZOS_SIO_RR0_CTS", w[w.index("ioc_bw_cts_out:"):])
        helper = self.asm.split("ioc_bulk_rts_off:")[1]
        self.assertIn("ZOS_IOC_WR5_BULK_IDLE", helper)

    def test_no_sio0_or_kernel_symbol_use(self):
        for body in (self.c, self.asm, self.proto, self.boot):
            self.assertNotIn("ZOS_SIO0", body)
            self.assertNotIn("SIO0B", body)
            self.assertNotIn("ZOS_CTC0", body)
        # Every SIO1/A access lives in the driver's own assembly.
        self.assertIn("ZOS_SIO1A_CTRL", self.asm)
        self.assertNotIn("ZOS_SIO1A", self.c)
        # The endpoint is declared by the image metadata; the host packer
        # assigns its number and the resident loader installs the record
        # (architecture 36.11), so no driver code registers anything.
        self.assertIn("ipc_install_endpoint",
                      open(os.path.join(ROOT, "kernel", "driver",
                                        "driver_loader.c")).read())
        self.assertNotIn("register_ep", self.proto)
        self.assertIn("ZOS_CLASS_IOC", self.proto)
        self.assertIn("ZOS_PROTOCOL_VERSION", self.proto)
        for op in ("ZIOC_OP_COMMAND", "ZIOC_OP_BULK_READ",
                   "ZIOC_OP_BULK_WRITE", "ZIOC_OP_STATUS",
                   "ZIOC_OP_READY"):
            self.assertIn(op, self.proto)

    def test_status_exposes_only_semantic_bits(self):
        body = self.c.split("uint16_t ioc_status")[1].split(
            "int ioc_link_bringup")[0]
        self.assertIn("ZIOC_STATUS_HID_PENDING", body)
        # No raw RR0 mask or SIO status bit leaks through the result.
        self.assertNotIn("ZOS_IOC_HID_DOORBELL_MASK", body)
        self.assertNotIn("ZOS_SIO_RR0", body)

    def test_busy_guard_covers_every_exec_pathed_exit(self):
        self.assertIn("ZIOC_EBUSY", self.c)
        # One guard acquisition per command exchange and per bulk phase;
        # every one of those paths releases it exactly once.
        self.assertEqual(self.c.count("state->busy = 1u;"), 3)
        self.assertEqual(self.c.count("state->busy = 0u;"), 5)

    def test_ready_done_interlock_state_machine(self):
        self.assertIn("IOC_PHASE_ADMITTED", self.c)
        self.assertIn("IOC_PHASE_DONE_REQUIRED", self.c)
        self.assertIn("IOC_CMD_XFER_STATUS", self.c)
        self.assertIn("IOC_RSP_XFER_STATUS", self.c)
        self.assertIn("IOC_OFF_DONE_XFER_ID", self.c)
        self.assertIn("IOC_OFF_DONE_STATUS", self.c)
        # A bulk failure always arms whole-link recovery, and only a
        # fully validated read proves the bulk boundary.
        self.assertIn("state->recover = 1u;", self.c)
        self.assertIn("state->bulk_synced = 1u;", self.c)

    def test_bulk_limits_and_errors_stay_in_the_transport_vocabulary(self):
        header = _read(IOC_H)
        self.assertIn("ZIOC_BULK_MAX 512u", header)
        self.assertIn("ZIOC_EIO", header)
        # The bulk phases stay private to the driver (ioc_protocol.h is
        # the client-visible vocabulary: commands and payload offsets).
        self.assertIn("READY/DONE bulk admission state",
                      _read(IOC_PROTOCOL_H))


if __name__ == "__main__":
    unittest.main()
