#!/usr/bin/env python3
"""M9 IOC-FAT provider structural acceptance (work package 30).

The behavioural acceptance is the target test plus the simulator replay
(`tests/test_iocfat_sim.py`); this module pins the build, manifest and
source-layout facts those runs depend on:

- `fs/iocfat/` reaches the MCU FatFs service only through the IOC class
  interface: no SIO port access, no raw I/O and no `drivers/ioc` include;
- the module manifest declares a resident driver with the budgeted size and
  the module really packs inside the 13-granule ceiling;
- the module keeps exactly one cache slot and carries no test-only counter;
- production loads it last and optionally, and `config/mounts.json` attaches
  it at the optional `/` sourced from `ioc.0`;
- the acceptance image, its manifest and the native program are wired.

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

MODULE_DIR = os.path.join(ROOT, "fs", "iocfat")
MODULE_SRC = os.path.join(MODULE_DIR, "iocfat.c")
MANIFEST = os.path.join(ROOT, "modules", "manifests", "iocfat.json")
BUILT_ZMOD = os.path.join(ROOT, "build", "modules", "iocfat.zmod")
MOUNTS = os.path.join(ROOT, "config", "mounts.json")
RECOVERY = os.path.join(ROOT, "recovery", "manifest.json")
IMAGE_MANIFEST = os.path.join(ROOT, "tests", "m9", "manifest-iocfat.json")
MAKEFILE = os.path.join(ROOT, "Makefile")
MAIN_C = os.path.join(ROOT, "kernel", "main.c")
FSTEST = os.path.join(ROOT, "tests", "target", "m9", "fstest", "fstest.c")
TARGET_TEST = os.path.join(ROOT, "tests", "target", "m9", "m9_iocfat_test.c")
SELFTEST_H = os.path.join(ROOT, "tests", "target", "include",
                          "m9_iocfat_selftest.h")

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

    def test_only_the_class_interface_is_used(self):
        text = read(MODULE_SRC)
        self.assertIn("zioc_ops", text)
        self.assertIn("ZCAP_IOC_COMMAND", text)
        self.assertIn("ZDEV_IOC", text)

    def test_single_cache_slot_is_pinned(self):
        text = read(MODULE_SRC)
        self.assertIn("IOCFAT_CACHE_SLOTS 1u", text)
        # A second slot is a budget error, not a runtime choice.
        self.assertIn("#error", text)
        self.assertRegex(text, r"#if \(IOCFAT_CACHE_SLOTS != 1u\)")

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
        self.assertEqual(manifest["type"], "driver")
        self.assertEqual(manifest["abi"], 3)
        self.assertEqual(manifest["init"], "iocfat_init")
        self.assertIsNone(manifest["fini"])
        self.assertIn("_CODE", manifest["image_areas"])
        self.assertIn("_DATA", manifest["bss_areas"])

    def test_packed_module_fits_the_budget(self):
        if not os.path.exists(BUILT_ZMOD):
            self.skipTest("build/modules/iocfat.zmod missing; run make")
        with open(BUILT_ZMOD, "rb") as handle:
            blob = handle.read()
        header = json.loads(read(os.path.join(ROOT, "build", "modules",
                                              "iocfat.report.json")))
        image = int(header["image_size"])
        bss = int(header["bss_size"])
        self.assertEqual(header["name"], "iocfat")
        self.assertEqual(header["driver_abi"], 3)
        total = image + bss
        granules = (total + GRANULE - 1) // GRANULE
        self.assertLessEqual(total, MAX_TOTAL_BYTES,
                             "iocfat.zmod is %d bytes (max %d)"
                             % (total, MAX_TOTAL_BYTES))
        self.assertLessEqual(granules, MAX_GRANULES,
                             "iocfat.zmod needs %d granules (max %d)"
                             % (granules, MAX_GRANULES))
        self.assertGreater(len(blob), image,
                           "the container is smaller than its image")


class TestIocFatIntegration(IocFatStaticBase):
    def test_production_mounts_the_optional_root(self):
        mounts = json.loads(read(MOUNTS))["mounts"]
        root = [m for m in mounts if m["path"] == "/"]
        self.assertEqual(len(root), 1, "`/` is not attached exactly once")
        self.assertEqual(root[0]["filesystem"], "iocfat")
        self.assertEqual(root[0]["source"], "ioc.0")
        self.assertIs(root[0]["required"], False,
                      "an absent card must not stop the boot")
        system = [m for m in mounts if m["path"] == "/SYSTEM"]
        self.assertEqual(system[0]["filesystem"], "romfs")

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

    def test_kernel_load_is_gated_and_last(self):
        text = read(MAIN_C)
        self.assertIn("ZOS_IOCFAT_BOOT", text)
        self.assertIn("iocfat", text)
        # The gate keeps images whose store carries no provider quiet.
        self.assertRegex(text, r"#if ZOS_IOCFAT_BOOT")
        # The services load from the boot manifest first; the provider is
        # the last arena module loaded, and the mount pass runs after it.
        order = re.findall(r'kernel_load_vfs_module\("([a-z0-9]+)"\)', text)
        self.assertIn("iocfat", order)
        self.assertEqual(order[-1], "iocfat",
                         "iocfat must be the last boot module: %r" % order)
        self.assertLess(text.index("service_load_all(ZOS_SERVICE_MANIFEST_NAME)"),
                        text.index('kernel_load_vfs_module("iocfat")'),
                        "the resident services must be placed before the "
                        "arena module that asks them for the transport")
        # A healthy card prints nothing new, so the load must tolerate a
        # failure instead of stopping the boot chain.
        tail = text.split('kernel_load_vfs_module("iocfat")', 1)[1]
        self.assertIn("kernel_module_unavailable", tail.split("}", 1)[0])

    def test_makefile_wiring(self):
        text = read(MAKEFILE)
        self.assertIn("ZOS_IOCFAT_BOOT", text)
        self.assertIn("ZOS_ARENA_MODULES", text)
        self.assertIn("modules/manifests/iocfat.json", text)
        self.assertIn("$(BOOT_ZMODS)", text)
        self.assertIn("m9-iocfat-selftest", text)
        self.assertIn("tests/m9/manifest-iocfat.json", text)
        self.assertIn("test_iocfat_sim.py", text)
        self.assertIn("fs/iocfat/iocfat.c", text)
        # The acceptance image must not carry modules it does not boot.
        index = text.index("ROMFS_MANIFEST := tests/m9/manifest-iocfat.json")
        self.assertIn("ZOS_M9_IOCFAT_SELFTEST", text[index - 400:index])

    def test_acceptance_image_manifest(self):
        entries = [f["name"] for f in
                   json.loads(read(IMAGE_MANIFEST))["files"]]
        self.assertEqual(entries, ["sio.zmod", "ioc.zmod", "console.zmod",
                                   "services.man", "iocfat.zmod",
                                   "monitor.zex", "fstest.zex"])

    def test_the_acceptance_seam_is_not_extended(self):
        text = read(MAIN_C)
        # The module load is a production path: it carries no acceptance
        # switch, and the acceptance image only changes the boot set.
        self.assertNotIn("ZOS_M9_IOCFAT_SELFTEST", text)
        self.assertIn("ZOS_TARGET_TEST_BUILD", text)


class TestIocFatFstestSection(unittest.TestCase):
    def test_section_is_selected_by_the_mount(self):
        text = read(FSTEST)
        # Selection is a runtime probe of the mounted `/`, not a build flag,
        # so the same fstest.zex runs against a RAM root and a real card.
        self.assertIn("ft_is_iocfat", text)
        self.assertIn("/STATS", text)
        self.assertIn('ft_report("iocfat", ft_iocfat())', text)
        for case in ('ft_report("ramfs"', 'ft_report("exec"'):
            self.assertIn(case, text)

    def test_cache_phases_are_fenced_for_the_replay(self):
        text = read(FSTEST)
        self.assertIn("fstest: MARK ", text)
        for mark in ("ioc0", "io1", "io2", "io3", "io4", "io9"):
            self.assertIn('ft_mark("%s")' % mark, text)

    def test_target_test_covers_the_contract_surface(self):
        text = read(TARGET_TEST)
        for name in ("m9i_case_mount", "m9i_case_readonly", "m9i_case_names",
                     "m9i_case_cache", "m9i_case_full"):
            self.assertIn(name, text)
        for what in ("ZOS_EROFS", "ZOS_EXDEV", "ZOS_ENOTEMPTY", "ZOS_EMFILE",
                     "ZOS_EISDIR", "ZOS_ESTALE", "ZOS_EINVAL"):
            self.assertIn(what, text, "%s is not exercised" % what)
        # The full writable surface runs the native program too.
        self.assertIn('process_exec("/SYSTEM/fstest.zex"', text)

    def test_serial_smoke_has_the_mode(self):
        # Hardware row 3 runs `make smoke-hardware-m9-iocfat`, so the mode
        # and its completion contract must exist.
        text = read(os.path.join(ROOT, "tools", "serial_smoke.py"))
        self.assertIn('"m9-iocfat"', text)
        self.assertIn("m9 iocfat selftest: OK", text)
        self.assertIn("m9 iocfat: fstest iocfat section passed", text)
        self.assertIn("m9 iocfat selftest: FAIL", text)
        # The image's console is SIO/SIO, so the monitor prompt on the same
        # link proves the ordinary launcher still ran.
        self.assertIn('M9IOCFAT_MONITOR = b"Z80> "', text)
        # The `/` mount state is only visible in the serial capture through
        # this note, so the operator can tell the two card states apart.
        self.assertIn("VFS_UNAVAILABLE", text)
        self.assertIn("_root_observation", text)
        makefile = read(MAKEFILE)
        self.assertIn("smoke-hardware-m9-iocfat", makefile)

    def test_failure_codes_are_pinned(self):
        header = read(SELFTEST_H)
        codes = dict(re.findall(r"#define (\w+)\s+(0x[0-9A-Fa-f]{2})u",
                                header))
        self.assertEqual(codes["ZOS_M9IOFAT_OK"], "0x00")
        # Every code is distinct and small enough for one byte.
        values = list(codes.values())
        self.assertEqual(len(values), len(set(values)))
        self.assertLessEqual(max(int(v, 16) for v in values), 0xFF)


class TestIocFatSmokeObservation(unittest.TestCase):
    """The serial smoke tool must report which `/` state the boot reached.

    The production console is bound to the V9958, so the only card-state
    evidence in a serial capture is the mount pass's early-console
    diagnostic; without the note the two card states look identical (which
    is exactly what happened on the bench).
    """

    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        import serial_smoke
        cls.tool = serial_smoke

    def capture(self, func, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            func(*args)
        return out.getvalue()

    def observe(self, data):
        return self.capture(self.tool._root_observation, "normal", data)

    def test_mounted_root_is_reported(self):
        text = self.observe(self.tool.BANNER + b"console: service OK\r\n")
        self.assertIn("mounted without a diagnostic", text)
        self.assertIn("media present", text)

    def test_absent_root_reports_the_error_code(self):
        text = self.observe(self.tool.BANNER + b"vfs: / unavailable 0x0B\r\n")
        self.assertIn("NOT mounted", text)
        self.assertIn("0x0B", text)

    def test_module_diagnostic_is_not_a_root_diagnostic(self):
        # `iocfat: unavailable 0x..` is a module-load failure: a different
        # fault, and the tool must not report it as the root state.
        text = self.observe(self.tool.BANNER
                            + b"iocfat: unavailable 0x07\r\n")
        self.assertNotIn("NOT mounted", text)
        self.assertIn("mounted without a diagnostic", text)

    def test_absent_card_selftest_line_is_reported(self):
        text = self.observe(self.tool.BANNER
                            + b"vfs: / unavailable 0x0B\r\n"
                            + b"m9 iocfat selftest: OK (card absent)\r\n")
        self.assertIn("NOT mounted", text)

    def test_other_modes_stay_silent(self):
        self.assertEqual(
            self.capture(self.tool._root_observation, "m9-fs",
                         self.tool.BANNER), "")

    def test_phantom_root_failure_gets_a_hint(self):
        # `/` mounted, but the first real I/O failed: the card state never
        # reached the provider, and the operator needs to know that is a
        # bench/firmware observation rather than a provider bug.
        data = (self.tool.BANNER + b"console: service OK\r\n"
                + b"m9 iocfat selftest: FAIL 0x03\r\n")
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.tool._explain_wrong_image("m9-iocfat", data)
        self.assertIn("phantom volume", err.getvalue())

    def test_absent_card_mode_hints_at_the_missing_card(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.tool._explain_wrong_image(
                "m9-iocfat", self.tool.BANNER + b"vfs: / unavailable 0x0B\r\n")
        self.assertIn("needs a writable FAT card", err.getvalue())

    def test_writable_card_result_requires_the_monitor(self):
        good = (self.tool.BANNER
                + b"m9 iocfat: fstest iocfat section passed\r\n"
                + b"fstest: PASS 4/4\r\n"
                + b"m9 iocfat selftest: OK\r\n"
                + b"ZephyrOS native monitor\r\nbank=00 pid=0 "
                + b"scratch=11A8\r\nZ80> ")
        self.assertTrue(self.tool._m9iocfat_result(good))
        cut = good[:good.index(b"ZephyrOS native")]
        self.assertFalse(self.tool._m9iocfat_result(cut))


if __name__ == "__main__":
    unittest.main()
