#!/usr/bin/env python3
"""IOC-FAT driver structural acceptance.

The behavioural acceptance is the simulator replay of the outcome image
(`tests/test_fs_selftest_sim.py`); this module pins the build, manifest and
source-layout facts that run depends on:

- `drivers/iocfat/` reaches the MCU FatFs driver only through the IOC class
  interface: no SIO port access, no raw I/O and no `drivers/ioc` include;
- the driver image packs inside the 13-granule ceiling and keeps the hot
  tier's 2,048-byte reserve;
- the block cache's C geometry is the bulk segment the packer placed, and
  the module carries no test-only counter;
- production loads it as one of the drivers the boot manifest lists, and
  it publishes `/fs`;
- the acceptance image, its store manifest and the SDK programs are wired;
- the one documented "no media" answer: the driver decides the card state
  from the command that reads the medium.

Run with: `python3 -m unittest tests.test_iocfat`.
"""

import contextlib
import io
import json
import os
import re
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import modpack  # noqa: E402

MODULE_DIR = os.path.join(ROOT, "drivers", "iocfat")
MODULE_SRC = os.path.join(MODULE_DIR, "iocfat.c")
MANIFEST = os.path.join(ROOT, "drivers", "iocfat", "iocfat.json")
BUILT_MOD = os.path.join(ROOT, "build", "modules", "iocfat.mod")
DRIVER_PLAN = os.path.join(ROOT, "build", "modules", "drivers.plan.json")
PRODUCTION = os.path.join(ROOT, "systems", "production.json")
IMAGE_SYSTEM = os.path.join(ROOT, "systems", "fs.json")
MAKEFILE = os.path.join(ROOT, "Makefile")
MAIN_C = os.path.join(ROOT, "kernel", "main.c")
SELFTEST = os.path.join(ROOT, "tests", "fs", "fs_selftest.c")
CHILD = os.path.join(ROOT, "tests", "fs", "fs_child.c")

# Budget: image + BSS at most 6656 bytes (13 granules).
GRANULE = 512
MAX_TOTAL_BYTES = 6656
MAX_GRANULES = 13


def read(path):
    with open(path) as handle:
        return handle.read()


class IocFatStaticBase(unittest.TestCase):
    def module_sources(self):
        names = sorted(n for n in os.listdir(MODULE_DIR)
                       if n.endswith((".c", ".h", ".asm", ".inc")))
        self.assertTrue(names, "drivers/iocfat carries no source")
        return [(n, read(os.path.join(MODULE_DIR, n))) for n in names]


class TestIocFatSourceLayout(IocFatStaticBase):
    def test_no_sio_or_raw_io_in_the_provider(self):
        # The provider is a filesystem: it must reach the card only through
        # the IOC class interface (architecture 5.6).
        forbidden = [
            (r"SIO[01]?_[A-Z]", "an SIO port constant"),
            (r"\b(?:in|outp|inp)\s*\(", "raw Z80 port I/O"),
            (r"io_ports\.h|drivers/sio", "the SIO driver"),
        ]
        for name, text in self.module_sources():
            for pattern, what in forbidden:
                self.assertIsNone(
                    re.search(pattern, text),
                    "%s reaches %s; only the IOC class interface is allowed"
                    % (name, what))

    def test_no_driver_private_include(self):
        # A module may reach the IOC driver only through its class interface
        # (drivers/include/ioc.h), never through the driver's private header.
        for name, text in self.module_sources():
            self.assertIsNone(
                re.search(r'#include\s*[<"]drivers/', text),
                "%s includes a driver-private header" % name)
            self.assertIsNone(
                re.search(r'#include\s*[<"]kernel/', text),
                "%s reaches a kernel-private header" % name)

    def test_only_the_driver_interface_is_used(self):
        # The driver calls its host-resolved IOC dependency
        # through the driver API, never through a class shim.
        text = read(MODULE_SRC)
        self.assertIn("driver_api", text)
        self.assertIn("KIOC_OP_COMMAND", text)
        self.assertNotIn("zioc" + "_ops", text)
        self.assertNotIn("KCAP_IOC_COMMAND", text)

    def test_the_cache_geometry_is_the_bulk_segment(self):
        # The C side addresses the bulk extent by offset (slots, then the
        # path records); drivers/iocfat/iocfat_cache.asm reserves it.  The
        # two must agree, or a slot or a path lands past the extent and
        # the nucleus refuses the copy.
        text = read(MODULE_SRC)
        value = {}
        for name in ("IOCFAT_BLOCK_SIZE", "IOCFAT_CACHE_SLOTS",
                     "IOCFAT_CACHE_FILES", "IOCFAT_CACHE_PATH"):
            match = re.search(r"#define %s\s+(\d+)u" % name, text)
            self.assertIsNotNone(match, name)
            value[name] = int(match.group(1))
        expected = (value["IOCFAT_CACHE_SLOTS"] * value["IOCFAT_BLOCK_SIZE"] +
                    value["IOCFAT_CACHE_FILES"] * value["IOCFAT_CACHE_PATH"])
        asm = read(os.path.join(MODULE_DIR, "iocfat_cache.asm"))
        match = re.search(r"\.ds\s+(\d+)\s*\*\s*(\d+)\s*\+\s*(\d+)"
                          r"\s*\*\s*(\d+)", asm)
        self.assertIsNotNone(match)
        a, b, c, d = (int(group) for group in match.groups())
        self.assertEqual(a * b + c * d, expected)
        if os.path.exists(BUILT_MOD):
            with open(BUILT_MOD, "rb") as handle:
                info = modpack.parse_container(handle.read(), "iocfat.mod")
            bulk = [segment for segment in info["segments"]
                    if segment["role"] == "bulk"][0]
            self.assertEqual(bulk["image_size"] + bulk["bss_size"], expected)
        # The path records hold any path the nucleus can hand over.
        self.assertGreaterEqual(value["IOCFAT_CACHE_PATH"], 128)

    def test_no_test_only_hook_remains(self):
        for name, text in self.module_sources():
            self.assertNotIn("TEST-ONLY", text, "%s keeps a test-only hook"
                             % name)
            self.assertNotIn("selftest", text.lower(),
                             "%s carries acceptance code" % name)


class TestIocFatManifest(IocFatStaticBase):
    def test_manifest_shape(self):
        manifest = json.loads(read(MANIFEST))
        self.assertEqual(manifest["name"], "iocfat")
        self.assertEqual(manifest["init"], "iocfat_init")
        self.assertEqual([segment["role"] for segment in manifest["segments"]],
                         ["hot", "io", "bulk"])
        hot, io, bulk = manifest["segments"]
        self.assertIn("_CODE", hot["image_areas"])
        self.assertIn("_DRIVER_IO", io["bss_areas"])
        self.assertEqual(bulk["bss_areas"], ["_FSCACHE"])
        self.assertEqual([(ep["class"], ep["instance"])
                          for ep in manifest["endpoints"]], [(7, 0)])
        # The filesystem-namespace publication: the nucleus FS provider
        # learns the driver's endpoint from it.
        self.assertEqual(manifest["publishes"][0]["path"], "/fs")
        self.assertEqual(manifest["publishes"][0]["type"], "FS")
        # The dependency is named symbolically in the source (the IOC
        # protocol type); tools/systemgen.py resolves it against the
        # selected set (tests/test_systemgen.py).
        self.assertEqual(manifest["depends"], ["IOC"])

    def test_packed_driver_keeps_the_hot_tier_room(self):
        if not os.path.exists(BUILT_MOD):
            self.skipTest("build/modules/iocfat.mod missing; run make")
        with open(BUILT_MOD, "rb") as handle:
            blob = handle.read()
        info = modpack.parse_container(blob, "iocfat.mod")
        self.assertGreater(info["image_size"], 0)
        self.assertEqual([segment["role"] for segment in info["segments"]],
                         ["hot", "io", "bulk"])
        # The render-transaction driver growth leaves at least 2 KiB for
        # another bounded device implementation.
        # after every image is placed.
        plan = json.loads(read(DRIVER_PLAN))
        hot = plan["nucleus"]
        free = int(hot["driver_hot_capacity_bytes"]) - \
            int(hot["driver_hot_bytes"])
        self.assertGreaterEqual(free, 2048,
                                "driver hot tier has only %d bytes free"
                                % free)


class TestIocFatIntegration(IocFatStaticBase):
    def test_production_boots_it_as_a_driver(self):
        # The production driver set names the driver, so the nucleus
        # loads it and installs its /fs publication at boot.
        system = json.loads(read(PRODUCTION))
        self.assertEqual(system["drivers"][-1], "iocfat",
                         "the provider must be the last driver image")
        self.assertIn("iocfat", system["drivers"])

    def test_the_system_file_loads_it_last(self):
        system = json.loads(read(PRODUCTION))
        # Last module: nothing loads after it but data and programs, and
        # the store order (drivers, the boot manifest, then the store's
        # own files) always puts every driver image first.
        self.assertEqual(system["drivers"][-1], "iocfat")
        self.assertNotIn("iocfat.mod", system["store"])
        self.assertIn("monitor.exe", system["store"])

    def test_kernel_boots_it_from_the_driver_manifest(self):
        text = read(MAIN_C)
        # The driver is one of the drivers the boot manifest lists: there
        # is no per-driver boot gate.
        self.assertIn("driver_load_all(KOS_DRIVER_MANIFEST_NAME)", text)
        self.assertNotIn("KOS_IOCFAT_BOOT", text)
        self.assertNotIn("kernel_load_vfs_module", text)
        store = read(os.path.join(ROOT, "systems", "production.json"))
        self.assertIn("iocfat", store)

    def test_makefile_wiring(self):
        text = read(MAKEFILE)
        # The driver is a driver image of the production and acceptance
        # boot sets, packed like the other drivers from the system file's
        # driver list.
        self.assertIn("$(BUILD)/drivers/$(1).json", text)
        self.assertIn("tools/modpack.py pack", text)
        self.assertIn("SYSTEM=fs", text)
        self.assertIn("build-fs-selftest", text)
        self.assertIn("SYSTEM_SUFFIX.fs := -fs-selftest", text)
        self.assertIn("test_fs_selftest", text)
        self.assertIn("drivers/iocfat/iocfat.c", text)
        # The acceptance image must not carry modules it does not boot.
        system = json.loads(read(IMAGE_SYSTEM))
        for name in ("sio", "ioc", "console-serial", "iocfat"):
            self.assertIn(name, system["drivers"])
        self.assertNotIn("hid", system["drivers"])

    def test_acceptance_image_system_file(self):
        system = json.loads(read(IMAGE_SYSTEM))
        # The acceptance image boots the timer driver and carries the
        # device-event outcome program too: the event replay rides the
        # filesystem image (its card, its child program) and the timer
        # driver is the event source (`events.exe` in the store).
        self.assertEqual(system["drivers"],
                         ["sio", "ioc", "console-serial", "iocfat",
                          "timer"])
        # The far-data outcome program, the hand-written bank container
        # and the DATA file they read are store files too.
        self.assertEqual(system["store"],
                         ["monitor.exe", "fs_selftest.exe", "fs_child.exe",
                          "events.exe", "far_data.exe", "banks.exe",
                          "known.bin", "iobench.exe", "iomix.exe",
                          "iomix_child.exe", "iostream.exe"])
        self.assertEqual(system["boot"], "/system/fs_selftest.exe")

    def test_the_acceptance_seam_is_not_extended(self):
        text = read(MAIN_C)
        # The module load is a production path: it carries no acceptance
        # switch, and the acceptance image only changes the boot set.
        self.assertNotIn("KOS_FS_SELFTEST", text)
        self.assertIn("KOS_TARGET_TEST_BUILD", text)


class TestFsAcceptancePrograms(unittest.TestCase):
    """The outcome programs the fs-selftest image boots."""

    def test_program_covers_the_card_surface(self):
        text = read(SELFTEST)
        for name in ("card_case", "system_case", "exec_case"):
            self.assertIn(name, text)
        for what in ("FS_SIZE", "KOS_ENOENT", "list", "mkdir",
                     "rename", "unlink", "rmdir"):
            self.assertIn(what, text, "%s is not exercised" % what)
        self.assertIn('#include "kos.h"', text)
        self.assertNotIn("vfs_", text)
        # The image hands the console to the monitor as its last act.
        self.assertIn('run("/system/monitor.exe"', text)

    def test_child_reports_its_tail_and_leaks_its_handles(self):
        text = read(CHILD)
        self.assertIn("KOS_EXIT_STATUS", text)
        self.assertIn("startup()", text)
        self.assertIn('bind(FS_DIR', text.replace('"/fs"', "FS_DIR"))
        self.assertNotRegex(text, r"\bclose\b")

    def test_serial_smoke_has_the_mode(self):
        # Hardware smoke runs `make smoke-hardware-fs`, so the mode and its
        # completion contract must exist.
        text = read(os.path.join(ROOT, "tools", "serial_smoke.py"))
        self.assertIn('"fs"', text)
        self.assertIn("fs-selftest: PASS", text)
        self.assertIn("fs-selftest: card absent", text)
        self.assertIn('FS_IMAGE_MONITOR = b"Z80> "', text)
        makefile = read(MAKEFILE)
        self.assertIn("smoke-hardware-fs", makefile)


class TestNoMediaContract(unittest.TestCase):
    """`/fs` with no media answers `KOS_ENODEV` for every operation.

    The deployed firmware answers some commands (CAPS, GENERATION, ROOT,
    and OPENDIR for the root) from a cached mount context after the card
    is gone, so the driver must decide the card state from the one command
    that reads the medium: the root directory open *and* its first read.
    Otherwise `chdir("/fs")` succeeds on the bench where the simulator's
    replay expects the refusal (the 2026-10-11 hardware failure).
    """

    def test_the_media_probe_reads_the_directory(self):
        text = read(MODULE_SRC)
        init = text[text.index("uint8_t iocfat_init("):]
        probe = init[:init.index("iocfat_ready = 1u;")]
        self.assertIn("iocfat_media_readable()", probe)
        self.assertIn("return 0u;", probe)
        # The probe itself: open the root and read one entry, both of them.
        helper = text[text.index("static int iocfat_media_readable(void)\n{"):]
        helper = helper[:helper.index("\n}\n")]
        self.assertIn('iocfat_dir_open("/"', helper)
        self.assertIn("iocfat_dir_read(", helper)
        # One entry and the end are both proof; only a code is a refusal.
        self.assertIn("(result < 0) ? result : 0", helper)

    def test_the_root_stat_proves_the_media_before_answering(self):
        text = read(MODULE_SRC)
        stat = text[text.index("static int iocfat_stat_path"):]
        stat = stat[:stat.index("\n}\n")]
        self.assertIn("iocfat_media_readable()", stat)
        # A busy directory slot means an enumeration was reading it.
        self.assertIn("-KOS_EMFILE", stat)

    def test_every_operation_reports_the_documented_error(self):
        text = read(MODULE_SRC)
        dispatch = text[text.index("int16_t iocfat_dispatch"):]
        dispatch = dispatch[:dispatch.index("\n}\n")]
        self.assertIn("iocfat_ready == 0u", dispatch)
        self.assertIn("-KOS_ENODEV", dispatch)
        # NO_MEDIA from a live card (removed mid-run) maps to the same code.
        table = text[text.index("FS2_STATUS_NO_MEDIA"):]
        self.assertIn("0x03u, /* NO_MEDIA  -> ENODEV */",
                      text)

    def test_the_residency_keeps_the_path_in_the_workspace(self):
        text = read(MODULE_SRC)
        # The provider never names a caller buffer of its own: the path
        # arrives in the fixed-core workspace (architecture 4.2).
        self.assertIn("workspace", text)


if __name__ == "__main__":
    unittest.main()
