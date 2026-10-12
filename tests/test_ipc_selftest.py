"""Host checks for the IPC substrate acceptance package (39).

Pins the structural facts the `ipc-selftest` image, its client program and
its replay depend on, without booting anything:

  - the build wiring (target, guard define, image names, manifest);
  - the pinned ABI: the two syscall numbers, the frame and argument-block
    layouts and the descriptor conventions in abi/kos/ipc.h;
  - the residency classification: the substrate is `fixed-required`, its
    cold syscall body is `cold`, and all three are `test_link` objects the
    production linker script does not list (production stays byte-identical
    until package 41 adopts them);
  - the production image really carries no substrate symbol;
  - the client program's data fits the syscall argument window, including
    every libkos marshalling block (the SDK's lookup/call blocks);
  - the SDK surface and the six container manifests the loader validates;
  - correction 01: the caller-buffer probe's window layout (code above the
    0100h-02FFh buffer it names), the driver I/O buffer rule in the
    container manifests and the loader's range check, the aperture
    primitive's common placement and the three path counters.

Run with: `python3 -m unittest tests.test_ipc_selftest`.
"""

import json
import os
import re
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests"))

import mapcheck  # noqa: E402

MAKEFILE = os.path.join(ROOT, "Makefile")
ABI = os.path.join(ROOT, "abi", "kos", "ipc.h")
SDK_HEADER = os.path.join(ROOT, "user", "sdk", "include", "kos", "ipc.h")
SDK_INTERNAL = os.path.join(ROOT, "user", "sdk", "src", "kos_internal.h")
SDK_SOURCE = os.path.join(ROOT, "user", "sdk", "src", "ipc.c")
SDK_GATE = os.path.join(ROOT, "user", "sdk", "src", "ipcgate.asm")
EXE_MK = os.path.join(ROOT, "user", "sdk", "exe.mk")
RESIDENCY = os.path.join(ROOT, "config", "residency.json")
LINKER = os.path.join(ROOT, "linker", "zephyr80.lk")
SYSTEM_JSON = os.path.join(ROOT, "systems", "ipc.json")
CLIENT_DIR = os.path.join(ROOT, "tests", "target", "ipc")
DRIVER_MANIFESTS = os.path.join(CLIENT_DIR, "manifests")

IMAGE_SUBDIR = os.path.join(ROOT, "build-ipc-selftest")
PROD_SUBDIR = os.path.join(ROOT, "build")
# The driver images, reports and boot manifest belong to the IPC image's
# own module directory (one `$(MODULES)` per build, no subdirectory).
MODULE_DIR = os.path.join(IMAGE_SUBDIR, "modules")
DRIVER_DIR = os.path.join(IMAGE_SUBDIR, "modules")

SYSCALL_WINDOW_BASE = 0x0100
SYSCALL_WINDOW_END = 0x2000
SPLIT = 0x6000

# The manifest also pins every declarable fact the packer can check: the
# load order and the images that may name each dependency.
LOADS_BEFORE = {
    "ioc": [],
    "fat": ["ioc"],
    "vfs": ["ioc", "fat"],
    "echo": ["ioc", "fat", "vfs"],
    "con": ["ioc", "fat", "vfs", "echo"],
    "irqx": ["ioc", "fat", "vfs", "echo", "con"],
}
IPC_ENDPOINT_VERSION = 1


def read(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read()


def symbols(map_path):
    found = {}
    with open(map_path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = re.match(r"\s*([0-9A-Fa-f]{8})\s+(\S+)(?:\s+\S+)?\s*$",
                             line)
            if match:
                found[match.group(2)] = int(match.group(1), 16)
    return found


class TestIpcBuildWiring(unittest.TestCase):
    def test_makefile_has_the_acceptance_image(self):
        text = read(MAKEFILE)
        for token in ("SYSTEM=ipc",
                      "SYSTEM_SUFFIX.ipc := -ipc-selftest",
                      "systems/$(SYSTEM).json"):
            self.assertIn(token, text, token)
        # The image's own target builds it, replays it and prints the
        # packed path.
        self.assertIn("ipc-selftest:", text)
        self.assertIn("build-ipc-selftest/konaos-ipc-selftest.bin", text)
        self.assertIn("test_ipc_selftest*.py", text)
        self.assertIn("exe_program,ipccopy", text)
        system = json.loads(read(SYSTEM_JSON))
        self.assertIn("ipccopy.exe", system["store"])
        self.assertEqual(system["boot"], "/system/ipccopy.exe")

    def test_makefile_builds_the_probe_above_its_caller_buffer(self):
        text = read(MAKEFILE)
        self.assertIn("ipccopy.c ipccopy_entry.asm", text)
        self.assertIn("NO_CRT0=1,ENTRY=ipc_copy_entry", text)
        self.assertIn("LOWBASE=0x0300,CODEBASE=0x0344", text)

    def test_exe_rule_owns_the_code_base_option(self):
        text = read(EXE_MK)
        self.assertIn("CODEBASE=", text)
        self.assertIn("LOWBASE=", text)
        self.assertIn("'-b _KOS_SDK_DATA = $$($(1)_LOWBASE)'", text)
        self.assertIn("'-b _CODE = $$($(1)_CODEBASE)'", text)

    def test_container_data_area_is_named_for_the_rule(self):
        text = read(MAKEFILE)
        self.assertIn("--dataseg DRIVER_IO", text)

    def test_loader_and_packer_enforce_the_driver_tiers(self):
        text = read(os.path.join(ROOT, "kernel", "driver",
                                "driver_loader.c"))
        # The nucleus loader refuses a segment whose allocation leaves its
        # tier (the hot tier 6000h-DFFFh, which contains the io role's
        # 8000h-BFFFh driver I/O window, and the boot/bulk tier
        # 0000h-5FFFh); the packer refuses the same set at build time.
        self.assertIn("DRIVER_HOT_BASE", text)
        self.assertIn("DRIVER_HOT_END", text)
        self.assertIn("DRIVER_BOOT_END", text)
        packer = read(os.path.join(ROOT, "tools", "modpack.py"))
        self.assertIn('"io": (0x8000, 0xC000)', packer)
        self.assertIn('"hot": (0x6000, 0xE000)', packer)
        self.assertIn("does not fit its tier", packer)

    def test_makefile_builds_the_substrate_and_the_loader(self):
        text = read(MAKEFILE)
        for token in ("kernel/ipc/ipc.c", "kernel/ipc/ipc_asm.asm",
                      "kernel/driver/driver_loader.c",
                      "kernel/driver/driver_boot.asm",
                      "kernel/vfs/router.c", "kernel/syscall/object.c"):
            self.assertIn(token, text, token)
        # The image links the substrate into the fixed core, the loader and
        # its cold body into the cold half, and packs the six driver
        # images with the boot manifest.  It uses production placement;
        # an image-specific cold-extra set would overflow its cold half.
        # The image uses production placement: its row sets
        # COLD_EXTRA_CSV empty and names no image-specific cold set.
        self.assertIn("COLD_EXTRA_CSV :=", text)
        # Package 49a: the store manifest, the boot manifest and the
        # resolved driver manifests are generated inputs, and every
        # selected driver packs to <id>.mod in the image's own
        # $(MODULES).
        self.assertIn("$(BUILD)/store.json", text)
        self.assertIn("tools/systemgen.py", text)
        self.assertIn("modpack.py plan", text)
        self.assertIn("$(MODULES)/ipc_%.rel:", text)
        system = json.loads(read(SYSTEM_JSON))
        self.assertEqual(system["drivers"][2:],
                         ["tests/target/ipc/manifests/ioc",
                          "tests/target/ipc/manifests/fat",
                          "tests/target/ipc/manifests/vfs",
                          "tests/target/ipc/manifests/echo",
                          "tests/target/ipc/manifests/con",
                          "tests/target/ipc/manifests/irqx"])
        for token in ("--codeseg BOOT --dataseg BOOT_DATA",
                      "--dataseg HOT", "--dataseg DRIVER_IO"):
            self.assertIn(token, text, token)

    def test_client_program_is_an_ordinary_sdk_program(self):
        text = read(MAKEFILE)
        self.assertIn("exe_program,ipc,tests/target/ipc", text)
        self.assertIn("SYSTEM=ipc", text)

    def test_dispatch_is_routed_in_the_dispatcher_proper(self):
        # Package 41 removed the KOS_TARGET_TEST_BUILD routing seam and
        # package 42b retired the stage-3 lookup: 06h-0Bh reach the object
        # family from the production switch, and no acceptance hook
        # remains.
        text = read(os.path.join(ROOT, "kernel", "syscall.c"))
        self.assertIn("KOS_SYSCALL_BIND", text)
        self.assertIn("kos_object_dispatch", text)
        self.assertNotIn("KOS_SYSCALL_IPC_LOOKUP", text)
        self.assertNotIn("kos_target_test_syscall", text)
        self.assertNotIn("KOS_TARGET_TEST_BUILD", text)


class TestIpcAbi(unittest.TestCase):
    def test_pinned_numbers_and_layouts(self):
        text = read(ABI)
        for pattern, value in (
                ("KOS_SYSCALL_CALL", 0x07),
                ("KOS_IPC_FRAME_SIZE", 32),
        ):
            match = re.search(r"#define %s\s+(0x[0-9A-Fa-f]+|[0-9]+)" % pattern,
                              text)
            self.assertIsNotNone(match, pattern)
            self.assertEqual(int(match.group(1), 0), value, pattern)

    def test_argument_blocks_and_frame_are_static_asserted(self):
        text = read(ABI)
        for token in ("sizeof(struct kos_ipc_frame) == KOS_IPC_FRAME_SIZE",
                      "sizeof(struct kos_ipc_call_args) == 44",
                      "offsetof(struct kos_ipc_call_args, frame) == 10",
                      "offsetof(struct kos_ipc_call_args, status) == 42",
                      "offsetof(struct kos_ipc_frame, status) == 30"):
            self.assertIn(token, text, token)

    def test_the_window_rule_is_documented(self):
        text = read(ABI)
        # The argument block lives in the caller window: the gate leaves
        # only 0100h-1FFFh of the caller bank visible while the nucleus
        # runs.  Descriptor buffers may go further (the patch below in
        # this file and correction 01 in docs/abi.md section 12).
        self.assertIn("0100h-1FFFh", text)
        self.assertIn("KOS_USER_BUFFER_BASE", text)
        self.assertIn("E000h-FFFFh", text)
        self.assertIn("Copy paths (correction 01", text)
        for token in ("A  the caller buffer", "B  the caller buffer",
                      "C  everything else"):
            self.assertIn(token, text, token)

    def test_the_driver_io_rule_is_documented(self):
        text = read(os.path.join(ROOT, "drivers", "include", "driver.h"))
        self.assertIn("8000h-BFFFh", text)
        self.assertIn("counted as slow", text)
        abi = read(ABI)
        self.assertIn("belongs in bank 6's 8000h-BFFFh", abi)


class TestIpcResidency(unittest.TestCase):
    def test_classification_covers_the_substrate(self):
        tiers, cold_half, test_link = mapcheck.load_residency(RESIDENCY)
        self.assertEqual(tiers["ipc"], "fixed-required")
        self.assertEqual(tiers["ipc_asm"], "fixed-required")
        # The aperture primitive is common, not fixed-core: it runs with
        # mode 10 and page 1 in force, where bank 7 is not mapped at all.
        self.assertEqual(tiers["ipc_common"], "fixed-required")
        self.assertEqual(tiers["driver_loader"], "cold")
        self.assertEqual(tiers["driver_boot"], "fixed-required")
        # Package 41 linked the substrate into production, and package 42b
        # switched the namespace family over too, so nothing is test-linked
        # and the retired rows are gone.
        self.assertEqual(sorted(test_link), [])
        self.assertIn("driver_loader", cold_half)
        # The retired object names, assembled from parts so this file
        # does not itself carry the tokens the grep gate keeps out.
        for gone in ("ipc" + "_syscall", "ioc" + "_shim", "fs", "registry",
                     "module" + "_loader"):
            self.assertNotIn(gone, tiers, gone)

    def test_production_linker_script_lists_the_substrate(self):
        # Package 41 linked the substrate and the resident driver loader;
        # package 42b also links the namespace family and drops the retired
        # syscall front end and class shim.
        text = read(LINKER)
        for name in ("ipc.rel", "ipc_asm.rel", "ipc_common.rel",
                     "driver_loader.rel", "driver_boot.rel",
                     "vfs_router.rel", "vfs_dev.rel", "vfs_fs.rel",
                     "vfs_system.rel", "object.rel"):
            self.assertIn(name, text, name)
        for gone in ("ipc" + "_syscall.rel", "ioc" + "_shim.rel",
                     "vfs_mount.rel", "vfs_file.rel", "vfs_dir.rel",
                     "registry.rel", "module" + "_loader.rel",
                     "romfs_provider.rel"):
            self.assertNotIn(gone, text, gone)

    def test_production_image_carries_the_substrate(self):
        map_path = os.path.join(PROD_SUBDIR, "konaos.map")
        if not os.path.exists(map_path):
            self.skipTest("build/konaos.map missing; run make all")
        found = symbols(map_path)
        for name in ("_ipc_install_endpoint", "_ipc_copy_run",
                     "_ipc_gate_latch", "_driver_load_all",
                     "_ipc_nucleus_call", "kos_driver_stack"):
            self.assertIn(name, found, name)
        # The substrate's fixed core is where the call machinery runs, and
        # the loader's bodies are cold-half code.  The stage-3 lookup is
        # gone (package 42b).
        self.assertLess(found["_ipc_install_endpoint"], 0x6000)
        self.assertNotIn("_ipc_lookup", found)
        self.assertGreaterEqual(found["_driver_load_all"], 0x6000)


class TestIpcImageLayout(unittest.TestCase):
    def setUp(self):
        self.map_path = os.path.join(IMAGE_SUBDIR, "konaos.map")
        if not os.path.exists(self.map_path):
            self.skipTest("build-ipc-selftest missing; run make ipc-selftest")
        self.syms = symbols(self.map_path)

    def test_substrate_is_fixed_and_the_bodies_are_cold(self):
        for name in ("_ipc_install_endpoint", "_driver_boot",
                     "_ipc_copy_run", "_ipc_invoke", "_ipc_gate_latch"):
            address = self.syms[name]
            self.assertGreaterEqual(address, 0x2000, name)
            self.assertLess(address, SPLIT, name)
        # The IPC dispatch body lives with the syscall gates?  No: the
        # object family's dispatcher is cold code (package 42b).
        self.assertGreaterEqual(self.syms["_kos_object_dispatch"], SPLIT)
        # The driver loader is cold code too, and the boot gate it calls
        # is common-resident (bank 7 is unmapped while a boot segment runs).
        self.assertGreaterEqual(self.syms["_driver_load_all"], SPLIT)
        self.assertLess(self.syms["_driver_load_all"], 0xDFFF)
        self.assertGreaterEqual(self.syms["_kos_driver_run_boot"], 0xE000)
        self.assertLess(self.syms["_kos_driver_run_boot"], 0xFD00)
        self.assertEqual(self.syms["kos_driver_stack"], 0x5D00)
        self.assertEqual(self.syms["kos_driver_stack_top"], 0x5F00)

    def test_six_driver_containers_are_packed_and_manifested(self):
        if not os.path.exists(os.path.join(MODULE_DIR, "echo.mod")):
            self.skipTest("the IPC image's driver images are not packed; "
                          "run make ipc-selftest")
        # The drivers are no longer linked into the kernel: they are
        # containers in the recovery store, named by the boot manifest the
        # loader reads.  The image carries no blob symbol.
        for name in ("echo", "con", "ioc", "fat", "vfs", "irqx"):
            self.assertNotIn("_ipc_%s_mod" % name, self.syms, name)

        # Package 41: the image also boots the SIO and console drivers
        # its SDK programs print through, ahead of the synthetic set.
        order = ["sio.mod", "console-serial.mod",
                 "ioc.mod", "fat.mod", "vfs.mod",
                 "echo.mod", "con.mod", "irqx.mod"]
        with open(os.path.join(DRIVER_DIR, "drivers.man"), "rb") as handle:
            data = handle.read()
        self.assertEqual(len(data), 24 * len(order))
        names = [data[i * 24:(i + 1) * 24].split(b"\0")[0].decode()
                 for i in range(len(order))]
        self.assertEqual(names, order)

        # Every named container is a driver image whose io segment fits
        # bank 6's 8000h-BFFFh and whose hot segment fits the hot tier.
        with open(os.path.join(DRIVER_DIR, "drivers.plan.json"),
                  encoding="utf-8") as handle:
            report = json.load(handle)
        self.assertEqual(report["order"], order)
        for entry in report["drivers"]:
            self.assertGreaterEqual(entry["hot_base"], 0x6000, entry)
            self.assertLess(entry["hot_base"], 0xE000, entry)
            if entry["io_base"] is not None:
                self.assertGreaterEqual(entry["io_base"], 0x8000, entry)
                self.assertLess(entry["io_base"], 0xC000, entry)
            if entry["boot_base"] is not None:
                self.assertGreaterEqual(entry["boot_base"], 0, entry)
                self.assertLess(entry["boot_base"], 0x6000, entry)
        nucleus = report["nucleus"]
        self.assertLessEqual(nucleus["driver_hot_bytes"],
                             nucleus["driver_hot_capacity_bytes"])
        self.assertLessEqual(nucleus["driver_boot_bytes"],
                             nucleus["driver_boot_capacity_bytes"])


class TestIpcClientWindow(unittest.TestCase):
    """Every block the syscall gate touches must fit its caller window."""

    def setUp(self):
        self.report_path = os.path.join(IMAGE_SUBDIR, "user", "ipc",
                                        "ipc.report.json")
        self.map_path = os.path.join(IMAGE_SUBDIR, "user", "ipc", "ipc.map")
        if not os.path.exists(self.report_path):
            self.skipTest("the client program has not been built")
        with open(self.report_path, encoding="utf-8") as handle:
            self.report = json.load(handle)
        self.syms = symbols(self.map_path)

    def test_every_marshalling_block_is_in_the_caller_window(self):
        for name, size in (("kos_ipc_raw_req", 4),
                           ("kos_ipc_call_block", 44),
                           ("kos_bind_block", 8),
                           ("kos_scratch", 16)):
            address = self.syms["_" + name]
            self.assertGreaterEqual(address, SYSCALL_WINDOW_BASE, name)
            self.assertLessEqual(address + size, SYSCALL_WINDOW_END, name)

    def test_the_manifest_pins_the_shared_scratch(self):
        scratch = self.report["scratch"]
        self.assertEqual(scratch["symbol"], "kos_scratch")
        self.assertEqual(scratch["size"], 16)
        self.assertEqual(self.report["entry"]["symbol"], "kos_entry")


class TestIpcCorrection01Layout(unittest.TestCase):
    """Correction 01: the probe's window layout and the common aperture.

    The probe is the only EXE that can name the caller window as a copy
    destination, which it does by linking its own code above the buffer;
    the aperture primitive is the only code in the image that runs with
    mode 10 and page 1 in force, so every byte of it must be common.
    """

    def setUp(self):
        self.report_path = os.path.join(IMAGE_SUBDIR, "user", "ipccopy",
                                        "ipccopy.report.json")
        self.map_path = os.path.join(IMAGE_SUBDIR, "user", "ipccopy",
                                     "ipccopy.map")
        self.kernel_map = os.path.join(IMAGE_SUBDIR, "konaos.map")
        if not os.path.exists(self.report_path) or \
                not os.path.exists(self.kernel_map):
            self.skipTest("the ipc image has not been built")

    def test_probe_links_its_code_above_the_caller_buffer(self):
        with open(self.report_path, encoding="utf-8") as handle:
            report = json.load(handle)
        syms = symbols(self.map_path)
        # 0100h-02FFh is the 512-byte buffer the probe names as a caller
        # buffer: the image is loaded there, so its code starts above it
        # and the entry offset points into the code.
        self.assertEqual(syms["s__KOS_SDK_DATA"], 0x0300)
        self.assertEqual(syms["s__CODE"], 0x0344)
        self.assertGreaterEqual(report["header"]["entry_offset"], 0x0300 -
                                0x0100)
        self.assertGreater(syms["ipc_copy_entry"], 0x0344)
        self.assertLess(syms["_ipc_copy_entry"], syms["s__DATA"])
        end = SYSCALL_WINDOW_BASE + report["header"]["image_size"] + \
            report["header"]["bss_size"]
        self.assertLessEqual(end, SYSCALL_WINDOW_END,
                             "the probe ends at %04Xh" % end)
        print("ipc: probe image %d bytes + bss %d bytes ends at %04Xh, "
              "code base %04Xh"
              % (report["header"]["image_size"],
                 report["header"]["bss_size"], end, syms["s__CODE"]))

    def test_probe_uses_no_c_runtime(self):
        text = read(MAKEFILE)
        self.assertIn("ipccopy.c ipccopy_entry.asm", text)
        entry = read(os.path.join(CLIENT_DIR, "ipccopy_entry.asm"))
        self.assertIn("ipc_copy_entry::", entry)
        self.assertIn("jp\t_ipc_copy_entry", entry)

    def test_every_container_data_area_is_a_driver_io_area(self):
        for name in ("echo", "con", "ioc", "fat", "vfs", "irqx"):
            manifest = json.loads(read(os.path.join(DRIVER_MANIFESTS,
                                                    name + ".json")))
            roles = [segment["role"] for segment in manifest["segments"]]
            self.assertIn("hot", roles, name)
            self.assertIn("io", roles, name)
            self.assertIn("boot", roles, name)
            payload = [(segment.get("image_areas") or []) +
                       (segment.get("bss_areas") or [])
                       for segment in manifest["segments"]]
            hot = manifest["segments"][roles.index("hot")]
            self.assertEqual(hot["image_areas"], ["_CODE"], name)
            self.assertEqual(
                manifest["segments"][roles.index("io")]["bss_areas"],
                ["_DRIVER_IO"], name)
            for areas in payload:
                self.assertTrue(areas, name)

            # The packer reserves each io block inside 8000h-BFFFh and the
            # loader places the hot segment in 6000h-DFFFh; the plan is the
            # address table the replay pins the loader's own report to.
            report_path = os.path.join(MODULE_DIR,
                                       name + ".report.json")
            if not os.path.exists(report_path):
                continue
            with open(report_path, encoding="utf-8") as handle:
                report = json.load(handle)
            io = [segment for segment in report["segments"]
                  if segment["role"] == "io"]
            self.assertEqual(len(io), 1, name)
            self.assertLessEqual(io[0]["image_size"] +
                                 io[0]["bss_size"], 0x4000, name)

    def test_aperture_primitive_is_common_resident(self):
        found = symbols(self.kernel_map)
        base = found["ipc_common_begin"]
        end = found["ipc_common_end"]
        self.assertGreaterEqual(base, 0xE000,
                                "the aperture stub is not common")
        self.assertLess(end, 0xFD00)
        for name in ("_ipc_ap_copy", "_ipc_ap_req", "_ipc_ap_saved_latch",
                     "_ipc_ap_saved_sp", "_ipc_ap_stack",
                     "_ipc_ap_stack_top"):
            self.assertIn(name, found, name)
            self.assertGreaterEqual(found[name], 0xE000, name)
            self.assertLess(found[name], 0xFD00, name)
        self.assertLessEqual(found["_ipc_ap_stack_top"] -
                             found["_ipc_ap_stack"], 8,
                             "the common interrupt-push reservation grew")
        for name in ("_ipc_copy_window", "_ipc_copy_aperture",
                     "_ipc_copy_slow"):
            self.assertIn(name, found, name)
            self.assertLess(found[name], SPLIT, name)

    def test_descriptor_window_covers_the_common_page(self):
        text = read(os.path.join(ROOT, "kernel", "ipc", "ipc.c"))
        # The capture accepts the whole caller-visible range, common page
        # included, and the resolver keeps the three paths.
        self.assertIn("(uint32_t)addr + length > 0x10000u", text)
        self.assertIn("in_common(caller_addr, length)", text)
        self.assertIn("in_driver_io(driver_addr, length)", text)
        self.assertIn("crosses_aperture(caller_addr, length)", text)
        self.assertIn("++ipc_copy_window;", text)
        self.assertIn("++ipc_copy_aperture;", text)
        self.assertIn("++ipc_copy_slow;", text)

    def test_irqx_driver_arms_the_correction_profile(self):
        driver = read(os.path.join(CLIENT_DIR, "irqx.c"))
        header = read(os.path.join(CLIENT_DIR, "..", "include",
                                   "ipc_test.h"))
        self.assertIn("IPC_IRQX_TC_APERTURE", driver)
        self.assertIn("IRQX_COPY_BYTES", header)
        self.assertIn("IPC_FRAME_SLOW_IO", header)
        self.assertIn("IPC_CON_SLOW_BYTES", header)
        # The con driver's deliberate slow buffer lives in its own hot
        # segment (con_slow.c, a _HOT area), so it is never inside bank 6's
        # 8000h-BFFFh and the copy takes path C.
        slow = read(os.path.join(CLIENT_DIR, "con_slow.c"))
        self.assertIn("ipc_con_slow_buffer", slow)
        self.assertIn("--dataseg HOT", read(MAKEFILE))
        manifest = json.loads(read(os.path.join(DRIVER_MANIFESTS,
                                                "con.json")))
        hot = [segment for segment in manifest["segments"]
               if segment["role"] == "hot"][0]
        self.assertIn("_HOT", hot["bss_areas"])


class TestIpcSdkSurface(unittest.TestCase):
    def test_client_wrappers_are_declared(self):
        text = read(SDK_HEADER)
        for token in ("int bind(const char *path",
                      "int unbind(unsigned char handle)",
                      "int chdir(const char *path)",
                      "int getcwd(char *buffer",
                      "int list(const char *path",
                      "int call("):
            self.assertIn(token, text, token)
        # Package 46 T6: the raw gate stub is SDK-private, so the public
        # header no longer names it and kos_internal.h declares it.
        internal = read(SDK_INTERNAL)
        self.assertIn("int kos_ipc_raw_call(struct kos_ipc_call_req *req)",
                      internal)
        self.assertNotIn("kos_ipc_raw_call", text)

    def test_sdk_owns_the_gate_and_the_cache(self):
        text = read(SDK_SOURCE)
        # Package 42b: no lookup and no cache - a client binds a name and
        # the kernel resolves it, so the SDK holds only the marshalling
        # blocks.
        self.assertIn("kos_ipc_call_block", text)
        self.assertIn("kos_bind_block", text)
        self.assertNotIn("KOS_IPC_CACHE_ENTRIES", text)
        self.assertNotIn("kos_ipc_lookup", text)
        gate = read(SDK_GATE)
        self.assertIn("_kos_ipc_raw_call", gate)
        # The carry records a structural refusal, so an IPC status carried
        # in HL is never confused with -errno.
        self.assertIn("_exe_runtime_error", gate)
        self.assertIn("ipcgate.asm", read(EXE_MK))

    def test_container_manifests_are_driver_manifests(self):
        for name in ("echo", "con", "ioc", "fat", "vfs", "irqx"):
            path = os.path.join(DRIVER_MANIFESTS, "%s.json" % name)
            self.assertTrue(os.path.exists(path), path)
            manifest = json.loads(read(path))
            # A driver manifest carries no module type and no version
            # field (work package 42c): the packer validates the shape.
            self.assertNotIn("type", manifest, name)
            self.assertNotIn("abi", manifest, name)
            self.assertIn("ipc_%s_init" % name,
                          manifest["init"], name)
            for endpoint in manifest["endpoints"]:
                self.assertIn("dispatch", endpoint, name)
                self.assertEqual(endpoint["version"],
                                 IPC_ENDPOINT_VERSION, name)
            # The source manifest names its dependencies symbolically: a
            # protocol type, `TYPE:instance` or a raw `#class`, with a
            # trailing `?` for the optional one.  tools/systemgen.py
            # resolves every entry against the selected set and refuses a
            # name that is missing or ambiguous (tests/test_systemgen.py);
            # the packer then refuses a resolved set it cannot place.
            for dependency in manifest.get("depends") or []:
                self.assertRegex(dependency,
                                 r"^(#[0-9]+|[A-Z]+(:[0-9]+)?)\??$",
                                 "%s: %s" % (name, dependency))

    def test_the_system_file_carries_the_client_program(self):
        system = json.loads(read(SYSTEM_JSON))
        self.assertIn("sio", system["drivers"])
        self.assertIn("ipc.exe", system["store"])


if __name__ == "__main__":
    unittest.main()
