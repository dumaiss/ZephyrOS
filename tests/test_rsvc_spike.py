#!/usr/bin/env python3
"""Resident-service spike host checks (work package 35).

Verifies the spike's static evidence without booting anything:

* the page geometry, read from the target configuration and cross-checked
  against the decoder model the replay boots;
* the client package geometry and the fixed-region core the spike must keep
  visible while the window shows the service bank;
* the spike's recovery-store manifest, so the image that boots is the one
  the documentation names.

The build-dependent checks skip when the image has not been built.
"""

import os
import re
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests"))

import zexpack  # noqa: E402
import zmodpack  # noqa: E402

CONFIG = os.path.join(ROOT, "tests", "target", "rsvc", "rsvc_config.h")
MANIFEST = os.path.join(ROOT, "tests", "m10", "manifest-rsvc.json")
MODULES = ("ioc", "fat", "vfs", "echo", "con", "irqx")
# Transport A on decoder revision 13: the service bank in the 6000h-DFFFh
# window, the fixed core in 2000h-5FFFh.
IMAGE = {"dir": "build-rsvc-spike-6000", "image": "zephyros-rsvc-spike.bin"}
SPLIT = 0x6000


def read_config():
    """The #define values of the target spike configuration."""
    values = {}
    with open(CONFIG) as handle:
        for line in handle:
            match = re.match(r"#define\s+(\w+)\s+([0-9xA-Fa-fu]+)", line)
            if match:
                raw = match.group(2).rstrip("uU")
                values[match.group(1)] = int(raw, 0)
    return values


def container_sizes(build_dir):
    sizes = {}
    for name in MODULES:
        path = os.path.join(ROOT, build_dir, "rsvc-modules",
                            "%s.zmod" % name)
        if not os.path.exists(path):
            return None
        with open(path, "rb") as handle:
            container = zmodpack.parse_container(handle.read(), path)
        sizes[name] = container["image_size"] + container["bss_size"]
    return sizes


class TestTransportGeometry(unittest.TestCase):
    """Target configuration against the decoder model the replay boots."""

    def setUp(self):
        self.config = read_config()
        if not os.path.exists(CONFIG):
            self.skipTest("target configuration missing")
        from z80sim import MemoryDecoder

        self.decoder = MemoryDecoder(13)

    def test_page_bits_and_domain_are_the_window(self):
        self.assertEqual(self.config["ZOS_RSVC_PAGE_BITS"], 0x20)
        self.assertEqual(self.config["ZOS_RSVC_DOMAIN_BASE"], 0x8000)
        self.assertEqual(self.config["ZOS_RSVC_DOMAIN_SIZE"], 0x6000)
        self.assertEqual(self.config["ZOS_RSVC_MAX_NESTING"], 4)

    def test_window_geometry_matches_the_decoder_model(self):
        """The revision-13 window is 6000h-DFFFh; with page 1 the whole
        window shows bank 6 while 2000h-5FFFh stays the fixed core.  The
        spike relocates its services at 8000h inside that window."""
        latch = 0x18 | 0x20 | 0x01          # OS mode, page 1, bank 1
        for address in (0x6000, 0x7FFF, 0x8000, 0xDFFF):
            self.assertEqual(self.decoder.bank_of(address, latch), 6,
                             address)
        for address in (0x2000, 0x3FFF, 0x4000, 0x5FFF):
            self.assertEqual(self.decoder.bank_of(address, latch), 7,
                             address)
        self.assertEqual(self.decoder.window_span(), (0x6000, 0xE000))
        self.assertEqual(self.config["ZOS_RSVC_DOMAIN_BASE"], 0x8000)

    def test_service_stack_fits_the_common_ceiling(self):
        # The stack lives in common memory (mapped in every mode); the
        # common limit is 2560 bytes and the build reports the usage.
        build = os.path.join(ROOT, IMAGE["dir"], "zephyros.map")
        if not os.path.exists(build):
            self.skipTest("run make rsvc-spike-selftest first")
        with open(build) as handle:
            text = handle.read()
        match = re.search(r"_COMMON\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})",
                          text)
        data = re.search(r"_COMMON_DATA\s+([0-9A-Fa-f]{8})\s+"
                         r"([0-9A-Fa-f]{8})", text)
        self.assertIsNotNone(match)
        self.assertIsNotNone(data)
        used = int(match.group(2), 16) + int(data.group(2), 16)
        self.assertLessEqual(used, 2560,
                             "common %d exceeds the M10 limit" % used)
        self.assertGreaterEqual(used, 2488 - 64,
                                "spike common usage unexpectedly small")


class TestClientAndFixedCore(unittest.TestCase):
    """The client package and the code that must stay page-independent."""

    DIR = IMAGE["dir"]

    def setUp(self):
        self.build = os.path.join(ROOT, self.DIR)
        self.zex = os.path.join(self.build, "user", "rsvc.zex")
        if not os.path.exists(self.zex):
            self.skipTest("run make rsvc-spike-selftest first")

    def test_client_package_geometry(self):
        with open(self.zex, "rb") as handle:
            header = zexpack.parse_header(handle.read())
        self.assertEqual(header["magic"], zexpack.MAGIC)
        # The client runs in the low window of its bank: image, BSS and
        # stack must all stay inside the 8 KiB the process window shows.
        self.assertLessEqual(header["image_size"] + header["bss_size"],
                             0x2000)
        self.assertLessEqual(header["stack_size"], 0x2000)
        self.assertLess(header["entry_offset"], header["image_size"])

    def test_client_has_no_initialised_data_areas(self):
        """The ZEX loader copies the image and zeroes the BSS; a
        compiler-emitted initialiser area would sit outside both, so every
        initialiser area must link empty at the end of the BSS."""
        map_path = os.path.join(self.build, "user", "rsvc", "rsvc.map")
        with open(map_path) as handle:
            text = handle.read()
        values = {}
        for name in ("s__DATA", "l__DATA", "s__GSINIT", "s__INITIALIZER",
                     "s__INITIALIZED"):
            match = re.search(r"^\s*([0-9A-Fa-f]{8})\s+%s\s" % name, text,
                              re.M)
            self.assertIsNotNone(match, name)
            values[name] = int(match.group(1), 16)
        bss_end = values["s__DATA"] + values["l__DATA"]
        self.assertEqual(values["s__GSINIT"], bss_end)
        self.assertEqual(values["s__INITIALIZER"], bss_end)
        self.assertEqual(values["s__INITIALIZED"], bss_end)
        self.assertLessEqual(bss_end, 0x2000)

    def test_fixed_core_stays_below_the_window(self):
        map_path = os.path.join(self.build, "zephyros.map")
        with open(map_path) as handle:
            text = handle.read()
        fixed = {}
        for name in ("_rsvc_latch", "_rsvc_domain_enter", "_rsvc_domain_set",
                     "_rsvc_domain_exit", "_rsvc_invoke",
                     "_rsvc_invoke_here", "_zos_rsvc_dispatch",
                     "_rsvc_irq_trampoline"):
            match = re.search(r"^\s*([0-9A-Fa-f]{8})\s+%s\s" % name, text,
                              re.M)
            if match:
                fixed[name] = int(match.group(1), 16)
        self.assertIn("_rsvc_latch", fixed)
        self.assertIn("_rsvc_invoke", fixed)
        self.assertIn("_zos_rsvc_dispatch", fixed)
        for name, address in sorted(fixed.items()):
            self.assertGreaterEqual(address, 0x2000, name)
            self.assertLess(address, SPLIT, name)

    def test_relocated_isr_stack_is_at_the_top_of_the_fixed_core(self):
        map_path = os.path.join(self.build, "zephyros.map")
        with open(map_path) as handle:
            text = handle.read()
        values = {}
        for name in ("irq_isr_stack", "irq_isr_stack_top",
                     "__kernel_code_end"):
            match = re.search(r"^\s*([0-9A-Fa-f]{8})\s+%s\s" % name, text,
                              re.M)
            self.assertIsNotNone(match, name)
            values[name] = int(match.group(1), 16)
        self.assertEqual(values["irq_isr_stack"], 0x5F00)
        self.assertEqual(values["irq_isr_stack_top"], 0x6000)
        self.assertLessEqual(values["__kernel_code_end"], 0x5F00)

    def test_syscall_side_objects_are_window_resident(self):
        """The two-bank classification puts the syscall dispatcher in the
        cold tier: the gate clears the page field, so it runs with P = 0
        and links in the window half (it would not fit the 16 KiB fixed
        region otherwise)."""
        map_path = os.path.join(self.build, "zephyros.map")
        with open(map_path) as handle:
            text = handle.read()
        for name in ("_kernel_syscall_dispatch", "_kernel_main"):
            match = re.search(r"^\s*([0-9A-Fa-f]{8})\s+%s\s" % name, text,
                              re.M)
            self.assertIsNotNone(match, name)
            self.assertGreaterEqual(int(match.group(1), 16), SPLIT, name)
            self.assertLess(int(match.group(1), 16), 0xE000, name)

    def test_service_containers_are_in_the_fixed_image(self):
        # The containers are linked into bank 7's loadable region and the
        # loader copies them into bank 6 at run time: their blobs must be
        # inside the always-visible fixed core, not in the window.
        map_path = os.path.join(self.build, "zephyros.map")
        with open(map_path) as handle:
            text = handle.read()
        for name in MODULES:
            match = re.search(r"^\s*([0-9A-Fa-f]{8})\s+_rsvc_%s_zmod\s"
                              % name, text, re.M)
            self.assertIsNotNone(match, name)
            address = int(match.group(1), 16)
            self.assertGreaterEqual(address, 0x2000, name)
            self.assertLess(address, SPLIT, name)


class TestRecoveryManifest(unittest.TestCase):
    def test_spike_manifest_names_the_boot_set(self):
        with open(MANIFEST) as handle:
            text = handle.read()
        self.assertIn("sio.zmod", text)
        self.assertIn("rsvc.zex", text)

    def test_boot_program_is_the_client(self):
        makefile = os.path.join(ROOT, "Makefile")
        with open(makefile) as handle:
            text = handle.read()
        self.assertIn(
            "ZOS_BOOT_PROGRAM=/SYSTEM/rsvc.zex", text)


if __name__ == "__main__":
    unittest.main()
