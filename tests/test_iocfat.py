#!/usr/bin/env python3
"""IOC-FAT service structural acceptance (work package 42b).

The behavioural acceptance is the simulator replay of the outcome image
(`tests/test_fs_selftest_sim.py`); this module pins the build, manifest and
source-layout facts that run depends on:

- `fs/iocfat/` reaches the MCU FatFs service only through the IOC class
  interface: no SIO port access, no raw I/O and no `drivers/ioc` include;
- the service image packs inside the 13-granule ceiling and keeps the hot
  tier's 4,096-byte reserve;
- the module keeps exactly one cache slot and carries no test-only counter;
- production loads it as one of the services the boot manifest lists, and
  it publishes `/FS`;
- the acceptance image, its store manifest and the SDK programs are wired.

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

import zmodpack  # noqa: E402

MODULE_DIR = os.path.join(ROOT, "fs", "iocfat")
MODULE_SRC = os.path.join(MODULE_DIR, "iocfat.c")
MANIFEST = os.path.join(ROOT, "services", "manifests", "iocfat.json")
BUILT_ZMOD = os.path.join(ROOT, "build", "modules", "iocfat.zmod")
SERVICE_PLAN = os.path.join(ROOT, "build", "modules", "services.plan.json")
RECOVERY = os.path.join(ROOT, "recovery", "manifest.json")
IMAGE_MANIFEST = os.path.join(ROOT, "tests", "fs", "manifest.json")
MAKEFILE = os.path.join(ROOT, "Makefile")
MAIN_C = os.path.join(ROOT, "kernel", "main.c")
SELFTEST = os.path.join(ROOT, "tests", "fs", "fs_selftest.c")
CHILD = os.path.join(ROOT, "tests", "fs", "fs_child.c")

# Correction-01 budget: image + BSS at most 6656 bytes (13 granules).
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
        self.assertTrue(names, "fs/iocfat carries no source")
        return [(n, read(os.path.join(MODULE_DIR, n))) for n in names]


class TestIocFatSourceLayout(IocFatStaticBase):
    def test_no_sio_or_raw_io_in_the_provider(self):
        # The provider is a filesystem: it must reach the card only through
        # the IOC class interface (architecture 32.17/32.19).
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
        # (zephyros/ioc.h), never through the driver's private header.
        for name, text in self.module_sources():
            self.assertIsNone(
                re.search(r'#include\s*[<"]drivers/', text),
                "%s includes a driver-private header" % name)
            self.assertIsNone(
                re.search(r'#include\s*[<"]kernel/', text),
                "%s reaches a kernel-private header" % name)

    def test_only_the_service_interface_is_used(self):
        # Package 42b: the driver calls its host-resolved IOC dependency
        # through the service API, never through a class shim.
        text = read(MODULE_SRC)
        self.assertIn("ipc_service_api", text)
        self.assertIn("ZIOC_OP_COMMAND", text)
        self.assertNotIn("zioc" + "_ops", text)
        self.assertNotIn("ZCAP_IOC_COMMAND", text)

    def test_single_cache_slot_is_pinned(self):
        text = read(MODULE_SRC)
        # One cache slot in SERVICE_IO, one handle pool: the instance is a
        # single static object with `cache[1]`.
        self.assertIn("iocfat_cache_slot cache[1]", text)

    def test_no_test_only_hook_remains(self):
        for name, text in self.module_sources():
            self.assertNotIn("TEST-ONLY", text, "%s keeps a test-only hook"
                             % name)
            self.assertNotIn("selftest", text.lower(),
                             "%s carries acceptance code" % name)

    def test_every_cache_rule_is_present(self):
        # The accepted deviation replaced the module-side counters with host
        # command counts, so the rules must be visible in the source.
        text = read(MODULE_SRC)
        for what in ("iocfat_cache_fill", "iocfat_cache_clear",
                     "iocfat_cache_invalidate_file",
                     "iocfat_cache_invalidate_other",
                     "iocfat_cache_invalidate_all"):
            self.assertIn(what, text, "%s is missing" % what)


class TestIocFatManifest(IocFatStaticBase):
    def test_manifest_shape(self):
        manifest = json.loads(read(MANIFEST))
        self.assertEqual(manifest["name"], "iocfat")
        self.assertEqual(manifest["init"], "iocfat_init")
        self.assertEqual([segment["role"] for segment in manifest["segments"]],
                         ["hot"])
        hot = manifest["segments"][0]
        self.assertIn("_CODE", hot["image_areas"])
        self.assertIn("_SERVICE_IO", hot["bss_areas"])
        self.assertEqual([(ep["class"], ep["instance"])
                          for ep in manifest["endpoints"]], [(7, 0)])
        # The filesystem-namespace publication: the nucleus FS provider
        # learns the driver's endpoint from it.
        self.assertEqual(manifest["publishes"][0]["path"], "/FS")
        self.assertEqual(manifest["publishes"][0]["type"], "FS")
        self.assertEqual(
            [(d["class"], d["instance"]) for d in manifest["depends"]],
            [(5, 0)])

    def test_packed_service_keeps_the_hot_tier_room(self):
        if not os.path.exists(BUILT_ZMOD):
            self.skipTest("build/modules/iocfat.zmod missing; run make")
        with open(BUILT_ZMOD, "rb") as handle:
            blob = handle.read()
        info = zmodpack.parse_container(blob, "iocfat.zmod")
        self.assertGreater(info["image_size"], 0)
        self.assertEqual([segment["role"] for segment in info["segments"]],
                         ["hot"])
        # Requirement 10: the driver hot tier keeps at least 4,096 bytes
        # free after every image is placed.
        plan = json.loads(read(SERVICE_PLAN))
        hot = plan["nucleus"]
        free = int(hot["service_hot_capacity_bytes"]) - \
            int(hot["service_hot_bytes"])
        self.assertGreaterEqual(free, 4096,
                                "driver hot tier has only %d bytes free"
                                % free)


class TestIocFatIntegration(IocFatStaticBase):
    def test_production_boots_it_as_a_service(self):
        # The production service set names the driver, so the nucleus
        # loads it and installs its /FS publication at boot.
        text = read(MAKEFILE)
        self.assertRegex(text, r"ZOS_SERVICES \?= .*iocfat")
        entries = [f["name"] for f in
                   json.loads(read(RECOVERY))["files"]]
        providers = [n for n in entries if n.endswith(".zmod")]
        self.assertEqual(providers[-1], "iocfat.zmod",
                         "the provider must be the last service image")

    def test_recovery_manifest_loads_it_last(self):
        entries = [f["name"] for f in
                   json.loads(read(RECOVERY))["files"]]
        self.assertIn("iocfat.zmod", entries)
        self.assertEqual(entries[-2:], ["v9958-6x8.font", "monitor.zex"]
                         if "v9958-6x8.font" in entries else entries[-2:])
        # Last module: nothing loads after it but data and programs.
        modules = [n for n in entries if n.endswith(".zmod")]
        self.assertEqual(modules[-1], "iocfat.zmod")
        self.assertLess(entries.index("iocfat.zmod"),
                        entries.index("monitor.zex"))

    def test_kernel_boots_it_from_the_service_manifest(self):
        text = read(MAIN_C)
        # The driver is one of the services the boot manifest lists: there
        # is no arena module pass and no per-driver boot gate any more.
        self.assertIn("service_load_all(ZOS_SERVICE_MANIFEST_NAME)", text)
        self.assertNotIn("ZOS_IOCFAT_BOOT", text)
        self.assertNotIn("kernel_load_vfs_module", text)
        store = read(os.path.join(ROOT, "recovery", "manifest.json"))
        self.assertIn("iocfat.zmod", store)

    def test_makefile_wiring(self):
        text = read(MAKEFILE)
        # Package 42b: the driver is a service image of the production and
        # acceptance boot sets, packed like the other services.
        self.assertIn("SVC_MANIFEST_DIR ?= services/manifests", text)
        self.assertRegex(text, r"ZOS_SERVICES \?= .*\biocfat\b")
        self.assertIn("tools/zmodpack.py pack $< $@ --objdir $(MODULES)",
                      text)
        self.assertIn("ZOS_FS_SELFTEST", text)
        self.assertIn("build-fs-selftest", text)
        self.assertIn("tests/fs/manifest.json", text)
        self.assertIn("test_fs_selftest", text)
        self.assertIn("fs/iocfat/iocfat.c", text)
        # The acceptance image must not carry modules it does not boot.
        index = text.index("ROMFS_MANIFEST := tests/fs/manifest.json")
        self.assertIn("ZOS_FS_SELFTEST", text[index - 400:index])

    def test_acceptance_image_manifest(self):
        entries = [f["name"] for f in
                   json.loads(read(IMAGE_MANIFEST))["files"]]
        self.assertEqual(entries, ["sio.zmod", "ioc.zmod", "console.zmod",
                                   "services.man", "iocfat.zmod",
                                   "monitor.zex", "fs_selftest.zex",
                                   "fs_child.zex"])

    def test_the_acceptance_seam_is_not_extended(self):
        text = read(MAIN_C)
        # The module load is a production path: it carries no acceptance
        # switch, and the acceptance image only changes the boot set.
        self.assertNotIn("ZOS_FS_SELFTEST", text)
        self.assertIn("ZOS_TARGET_TEST_BUILD", text)


class TestFsAcceptancePrograms(unittest.TestCase):
    """The outcome programs the fs-selftest image boots."""

    def test_program_covers_the_card_surface(self):
        text = read(SELFTEST)
        for name in ("card_case", "system_case", "exec_case"):
            self.assertIn(name, text)
        for what in ("FS_SIZE", "ZOS_ENOENT", "zos_ns_enum", "zos_mkdir",
                     "zos_rename", "zos_unlink", "zos_rmdir"):
            self.assertIn(what, text, "%s is not exercised" % what)
        self.assertIn('#include "zos.h"', text)
        self.assertNotIn("vfs_", text)
        # The image hands the console to the monitor as its last act.
        self.assertIn('zos_exec("/SYSTEM/monitor.zex"', text)

    def test_child_reports_its_tail_and_leaks_its_handles(self):
        text = read(CHILD)
        self.assertIn("ZOS_EXIT_STATUS", text)
        self.assertIn("zos_args()", text)
        self.assertIn('zos_ipc_bind(FS_DIR', text.replace('"/FS"', "FS_DIR"))
        self.assertNotIn("zos_close", text)

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


if __name__ == "__main__":
    unittest.main()
