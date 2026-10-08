"""Host checks for the IPC substrate acceptance package (39).

Pins the structural facts the `ipc-selftest` image, its client program and
its replay depend on, without booting anything:

  - the build wiring (target, guard define, image names, manifest);
  - the pinned ABI: the two syscall numbers, the frame and argument-block
    layouts and the descriptor conventions in abi/zos/ipc.h;
  - the residency classification: the substrate is `fixed-required`, its
    cold syscall body is `cold`, and all three are `test_link` objects the
    production linker script does not list (production stays byte-identical
    until package 41 adopts them);
  - the production image really carries no substrate symbol;
  - the client program's data fits the syscall argument window, including
    every libzos marshalling block (the SDK's lookup/call blocks);
  - the SDK surface and the six container manifests the loader validates;
  - correction 01: the caller-buffer probe's window layout (code above the
    0100h-02FFh buffer it names), the service I/O buffer rule in the
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
ABI = os.path.join(ROOT, "abi", "zos", "ipc.h")
SDK_HEADER = os.path.join(ROOT, "user", "sdk", "include", "zos", "ipc.h")
SDK_SOURCE = os.path.join(ROOT, "user", "sdk", "src", "ipc.c")
SDK_GATE = os.path.join(ROOT, "user", "sdk", "src", "ipcgate.asm")
ZEX_MK = os.path.join(ROOT, "user", "sdk", "zex.mk")
RESIDENCY = os.path.join(ROOT, "config", "residency.json")
LINKER = os.path.join(ROOT, "linker", "zephyr80.lk")
MANIFEST = os.path.join(ROOT, "tests", "ipc", "manifest.json")
CLIENT_DIR = os.path.join(ROOT, "tests", "target", "ipc")
SERVICE_MANIFESTS = os.path.join(CLIENT_DIR, "manifests")

IMAGE_SUBDIR = os.path.join(ROOT, "build-ipc-selftest")
PROD_SUBDIR = os.path.join(ROOT, "build")
# The service images, their reports and the boot manifest are packed into
# the shared module directory (the recovery manifest names them there).
MODULE_DIR = os.path.join(ROOT, "build", "modules", "ipc-modules")

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
        self.assertIn("ipc-selftest:", text)
        self.assertIn("ZOS_IPC_SELFTEST=1", text)
        self.assertIn("PK_SUFFIX := -ipc-selftest", text)
        self.assertIn("build-ipc-selftest/zephyros-ipc-selftest.bin", text)
        self.assertIn("tests/ipc/manifest.json", text)
        self.assertIn("ZOS_BOOT_PROGRAM=/SYSTEM/ipccopy.zex", text)
        self.assertIn("$(MODULES)/ipccopy.zex", text)
        self.assertIn('"name": "ipccopy.zex"', read(MANIFEST))

    def test_makefile_builds_the_probe_above_its_caller_buffer(self):
        text = read(MAKEFILE)
        self.assertIn("ipccopy.c ipccopy_entry.asm", text)
        self.assertIn("NO_CRT0=1,ENTRY=ipc_copy_entry", text)
        self.assertIn("CODEBASE=0x0300", text)

    def test_zex_rule_owns_the_code_base_option(self):
        text = read(ZEX_MK)
        self.assertIn("CODEBASE=", text)
        self.assertIn("'-b _CODE = $$($(1)_CODEBASE)'", text)

    def test_container_data_area_is_named_for_the_rule(self):
        text = read(MAKEFILE)
        self.assertIn("--dataseg SERVICE_IO", text)

    def test_loader_enforces_the_service_io_range(self):
        text = read(os.path.join(ROOT, "kernel", "service",
                                "service_loader.c"))
        # The nucleus loader refuses an image whose io segment leaves bank
        # 6's 8000h-BFFFh (correction 01's service I/O buffer rule).
        self.assertIn("SERVICE_IO_BASE", text)
        self.assertIn("SERVICE_IO_END", text)
        self.assertIn("io_cursor < SERVICE_IO_BASE", text)
        self.assertIn("io range full", text)

    def test_makefile_builds_the_substrate_and_the_loader(self):
        text = read(MAKEFILE)
        for token in ("kernel/ipc/ipc.c", "kernel/ipc/ipc_asm.asm",
                      "kernel/syscall/ipc.c", "kernel/service/service_loader.c",
                      "kernel/service/service_boot.asm"):
            self.assertIn(token, text, token)
        # The image links the substrate into the fixed core, the loader and
        # its cold body into the cold half, and packs the six service
        # images with the boot manifest (residency --cold-extra names the
        # moves objects that still fit beside the loader).
        self.assertIn("IPC_COLD_EXTRA_CSV", text)
        self.assertIn("--cold-extra $(IPC_COLD_EXTRA_CSV)", text)
        self.assertIn("$(IPC_MODDIR)/services.man", text)
        self.assertIn("zmodpack.py services", text)
        self.assertIn("IPC_SERVICE_NAMES := ioc fat vfs echo con irqx", text)
        self.assertIn("$(IPC_MODDIR)/ipc-%.zmod:", text)
        for token in ("--codeseg BOOT --dataseg BOOT_DATA",
                      "--dataseg HOT", "--dataseg SERVICE_IO"):
            self.assertIn(token, text, token)

    def test_client_program_is_an_ordinary_sdk_program(self):
        text = read(MAKEFILE)
        self.assertIn("zex_program,ipc,tests/target/ipc", text)
        self.assertIn("tests/ipc/manifest.json", text)

    def test_acceptance_hook_is_behind_the_documented_guard(self):
        text = read(os.path.join(ROOT, "kernel", "syscall.c"))
        self.assertIn("#if ZOS_TARGET_TEST_BUILD", text)
        self.assertIn("zos_target_test_syscall", text)
        # The hook is the only production-side reference: the dispatcher
        # itself lists no IPC number, so production returns ENOSYS for the
        # pair until package 41 links the substrate.
        self.assertNotIn("ZOS_SYSCALL_IPC_LOOKUP", text)
        self.assertNotIn("zos_ipc_dispatch", text)


class TestIpcAbi(unittest.TestCase):
    def test_pinned_numbers_and_layouts(self):
        text = read(ABI)
        for pattern, value in (
                ("ZOS_SYSCALL_IPC_LOOKUP", 0x06),
                ("ZOS_SYSCALL_IPC_CALL", 0x07),
                ("ZOS_IPC_FRAME_SIZE", 32),
        ):
            match = re.search(r"#define %s\s+(0x[0-9A-Fa-f]+|[0-9]+)" % pattern,
                              text)
            self.assertIsNotNone(match, pattern)
            self.assertEqual(int(match.group(1), 0), value, pattern)

    def test_argument_blocks_and_frame_are_static_asserted(self):
        text = read(ABI)
        for token in ("sizeof(struct zos_ipc_frame) == ZOS_IPC_FRAME_SIZE",
                      "sizeof(struct zos_ipc_lookup_args) == 4",
                      "sizeof(struct zos_ipc_call_args) == 44",
                      "offsetof(struct zos_ipc_call_args, frame) == 10",
                      "offsetof(struct zos_ipc_call_args, status) == 42",
                      "offsetof(struct zos_ipc_frame, status) == 30"):
            self.assertIn(token, text, token)

    def test_the_window_rule_is_documented(self):
        text = read(ABI)
        # The argument block lives in the caller window: the gate leaves
        # only 0100h-1FFFh of the caller bank visible while the nucleus
        # runs.  Descriptor buffers may go further (the patch below in
        # this file and correction 01 in docs/abi.md section 12).
        self.assertIn("0100h-1FFFh", text)
        self.assertIn("ZOS_USER_BUFFER_BASE", text)
        self.assertIn("E000h-FFFFh", text)
        self.assertIn("Copy paths (correction 01", text)
        for token in ("A  the caller buffer", "B  the caller buffer",
                      "C  everything else"):
            self.assertIn(token, text, token)

    def test_the_service_io_rule_is_documented(self):
        text = read(os.path.join(ROOT, "include", "zephyros", "ipc.h"))
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
        self.assertEqual(tiers["ipc_syscall"], "cold")
        self.assertEqual(tiers["service_loader"], "cold")
        self.assertEqual(tiers["service_boot"], "fixed-required")
        self.assertEqual(sorted(test_link),
                         ["ipc", "ipc_asm", "ipc_common", "ipc_syscall",
                          "service_boot", "service_loader"])
        # Production's cold set is untouched: the substrate and the loader
        # are not in it (the loader arrives in package 41).
        for name in test_link:
            self.assertNotIn(name, cold_half)

    def test_production_linker_script_does_not_list_the_substrate(self):
        text = read(LINKER)
        for name in ("ipc.rel", "ipc_asm.rel", "ipc_common.rel",
                     "ipc_syscall.rel", "service_loader.rel",
                     "service_boot.rel"):
            self.assertNotIn(name, text, name)

    def test_production_image_carries_no_substrate_symbol(self):
        map_path = os.path.join(PROD_SUBDIR, "zephyros.map")
        if not os.path.exists(map_path):
            self.skipTest("build/zephyros.map missing; run make all")
        found = symbols(map_path)
        for name in ("_ipc_lookup", "_ipc_syscall_call", "_ipc_copy_run",
                     "ipc_asm_begin", "ipc_asm_end", "_zos_ipc_dispatch",
                     "zos_service_stack", "_ipc_ap_copy", "_ipc_ap_req",
                     "_ipc_copy_window", "ipc_common_begin"):
            self.assertNotIn(name, found, name)


class TestIpcImageLayout(unittest.TestCase):
    def setUp(self):
        self.map_path = os.path.join(IMAGE_SUBDIR, "zephyros.map")
        if not os.path.exists(self.map_path):
            self.skipTest("build-ipc-selftest missing; run make ipc-selftest")
        self.syms = symbols(self.map_path)

    def test_substrate_is_fixed_and_the_bodies_are_cold(self):
        for name in ("_ipc_lookup", "_ipc_syscall_call", "_ipc_service_api",
                     "_ipc_copy_run", "_ipc_invoke", "_ipc_gate_latch"):
            address = self.syms[name]
            self.assertGreaterEqual(address, 0x2000, name)
            self.assertLess(address, SPLIT, name)
        self.assertGreaterEqual(self.syms["_zos_ipc_dispatch"], SPLIT)
        # The service loader is cold code too, and the boot gate it calls
        # is common-resident (bank 7 is unmapped while a boot segment runs).
        self.assertGreaterEqual(self.syms["_service_load_all"], SPLIT)
        self.assertLess(self.syms["_service_load_all"], 0xDFFF)
        self.assertGreaterEqual(self.syms["_zos_service_run_boot"], 0xE000)
        self.assertLess(self.syms["_zos_service_run_boot"], 0xFD00)
        self.assertEqual(self.syms["zos_service_stack"], 0x5D00)
        self.assertEqual(self.syms["zos_service_stack_top"], 0x5F00)

    def test_six_service_containers_are_packed_and_manifested(self):
        # The services are no longer linked into the kernel: they are
        # containers in the recovery store, named by the boot manifest the
        # loader reads.  The image carries no blob symbol.
        for name in ("echo", "con", "ioc", "fat", "vfs", "irqx"):
            self.assertNotIn("_ipc_%s_zmod" % name, self.syms, name)

        order = ["ipc-ioc.zmod", "ipc-fat.zmod", "ipc-vfs.zmod",
                 "ipc-echo.zmod", "ipc-con.zmod", "ipc-irqx.zmod"]
        with open(os.path.join(MODULE_DIR, "services.man"), "rb") as handle:
            data = handle.read()
        self.assertEqual(len(data), 24 * len(order))
        names = [data[i * 24:(i + 1) * 24].split(b"\0")[0].decode()
                 for i in range(len(order))]
        self.assertEqual(names, order)

        # Every named container is a service image whose io segment fits
        # bank 6's 8000h-BFFFh and whose hot segment fits the hot tier.
        with open(os.path.join(MODULE_DIR, "services.plan.json"),
                  encoding="utf-8") as handle:
            report = json.load(handle)
        self.assertEqual(report["order"], order)
        for entry in report["services"]:
            self.assertGreaterEqual(entry["hot_base"], 0x6000, entry)
            self.assertLess(entry["hot_base"], 0xE000, entry)
            if entry["io_base"] is not None:
                self.assertGreaterEqual(entry["io_base"], 0x8000, entry)
                self.assertLess(entry["io_base"], 0xC000, entry)
            self.assertGreaterEqual(entry["boot_base"], 0, entry)
            self.assertLess(entry["boot_base"], 0x6000, entry)
        nucleus = report["nucleus"]
        self.assertLessEqual(nucleus["service_hot_bytes"],
                             nucleus["service_hot_capacity_bytes"])
        self.assertLessEqual(nucleus["service_boot_bytes"],
                             nucleus["service_boot_capacity_bytes"])


class TestIpcClientWindow(unittest.TestCase):
    """The client program and libzos must fit the argument window."""

    def setUp(self):
        self.report_path = os.path.join(IMAGE_SUBDIR, "user", "ipc",
                                        "ipc.report.json")
        self.map_path = os.path.join(IMAGE_SUBDIR, "user", "ipc", "ipc.map")
        if not os.path.exists(self.report_path):
            self.skipTest("the client program has not been built")
        with open(self.report_path, encoding="utf-8") as handle:
            self.report = json.load(handle)
        self.syms = symbols(self.map_path)

    def test_program_and_bss_fit_the_caller_window(self):
        header = self.report["header"]
        end = SYSCALL_WINDOW_BASE + header["image_size"] + \
            header["bss_size"]
        self.assertLessEqual(end, SYSCALL_WINDOW_END,
                             "the program ends at %04Xh" % end)
        print("ipc: client image %d bytes + bss %d bytes ends at %04Xh"
              % (header["image_size"], header["bss_size"], end))

    def test_every_marshalling_block_is_in_the_caller_window(self):
        for name, size in (("zos_ipc_raw_req", 4),
                           ("zos_ipc_lookup_block", 4),
                           ("zos_ipc_call_block", 44),
                           ("zos_scratch", 16)):
            address = self.syms["_" + name]
            self.assertGreaterEqual(address, SYSCALL_WINDOW_BASE, name)
            self.assertLessEqual(address + size, SYSCALL_WINDOW_END, name)

    def test_the_manifest_pins_the_shared_scratch(self):
        scratch = self.report["scratch"]
        self.assertEqual(scratch["symbol"], "zos_scratch")
        self.assertEqual(scratch["size"], 16)
        self.assertEqual(self.report["entry"]["symbol"], "zos_entry")


class TestIpcCorrection01Layout(unittest.TestCase):
    """Correction 01: the probe's window layout and the common aperture.

    The probe is the only ZEX that can name the caller window as a copy
    destination, which it does by linking its own code above the buffer;
    the aperture primitive is the only code in the image that runs with
    mode 10 and page 1 in force, so every byte of it must be common.
    """

    def setUp(self):
        self.report_path = os.path.join(IMAGE_SUBDIR, "user", "ipccopy",
                                        "ipccopy.report.json")
        self.map_path = os.path.join(IMAGE_SUBDIR, "user", "ipccopy",
                                     "ipccopy.map")
        self.kernel_map = os.path.join(IMAGE_SUBDIR, "zephyros.map")
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
        self.assertEqual(syms["s__CODE"], 0x0300)
        self.assertGreaterEqual(report["header"]["entry_offset"], 0x0300 -
                                0x0100)
        self.assertGreater(syms["ipc_copy_entry"], 0x0300)
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

    def test_every_container_data_area_is_a_service_io_area(self):
        for name in ("echo", "con", "ioc", "fat", "vfs", "irqx"):
            manifest = json.loads(read(os.path.join(SERVICE_MANIFESTS,
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
                ["_SERVICE_IO"], name)
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
        self.assertIn("in_service_io(service_addr, length)", text)
        self.assertIn("crosses_aperture(caller_addr, length)", text)
        self.assertIn("++ipc_copy_window;", text)
        self.assertIn("++ipc_copy_aperture;", text)
        self.assertIn("++ipc_copy_slow;", text)

    def test_irqx_service_arms_the_correction_profile(self):
        service = read(os.path.join(CLIENT_DIR, "irqx.c"))
        header = read(os.path.join(CLIENT_DIR, "..", "include",
                                   "ipc_test.h"))
        self.assertIn("IPC_IRQX_TC_APERTURE", service)
        self.assertIn("IRQX_COPY_BYTES", header)
        self.assertIn("IPC_FRAME_SLOW_IO", header)
        self.assertIn("IPC_CON_SLOW_BYTES", header)
        # The con service's deliberate slow buffer lives in its own hot
        # segment (con_slow.c, a _HOT area), so it is never inside bank 6's
        # 8000h-BFFFh and the copy takes path C.
        slow = read(os.path.join(CLIENT_DIR, "con_slow.c"))
        self.assertIn("ipc_con_slow_buffer", slow)
        self.assertIn("--dataseg HOT", read(MAKEFILE))
        manifest = json.loads(read(os.path.join(SERVICE_MANIFESTS,
                                                "con.json")))
        hot = [segment for segment in manifest["segments"]
               if segment["role"] == "hot"][0]
        self.assertIn("_HOT", hot["bss_areas"])


class TestIpcSdkSurface(unittest.TestCase):
    def test_client_wrappers_are_declared(self):
        text = read(SDK_HEADER)
        for token in ("int zos_ipc_lookup(", "int zos_ipc_lookup_version(",
                      "int zos_ipc_call(",
                      "int zos_ipc_raw_call(struct zos_ipc_call_req *req)"):
            self.assertIn(token, text, token)

    def test_sdk_owns_the_gate_and_the_cache(self):
        text = read(SDK_SOURCE)
        self.assertIn("zos_ipc_call_block", text)
        self.assertIn("ZOS_IPC_CACHE_ENTRIES", text)
        gate = read(SDK_GATE)
        self.assertIn("_zos_ipc_raw_call", gate)
        # The carry records a structural refusal, so an IPC status carried
        # in HL is never confused with -errno.
        self.assertIn("_zex_runtime_error", gate)
        self.assertIn("ipcgate.asm", read(ZEX_MK))

    def test_container_manifests_are_service_manifests(self):
        for name in ("echo", "con", "ioc", "fat", "vfs", "irqx"):
            path = os.path.join(SERVICE_MANIFESTS, "%s.json" % name)
            self.assertTrue(os.path.exists(path), path)
            manifest = json.loads(read(path))
            self.assertEqual(manifest["type"], "service", name)
            self.assertEqual(manifest["abi"], 1, name)
            self.assertIn("ipc_%s_init" % name,
                          manifest["init"], name)
            for endpoint in manifest["endpoints"]:
                self.assertIn("dispatch", endpoint, name)
                self.assertEqual(endpoint["version"],
                                 IPC_ENDPOINT_VERSION, name)
            # The manifest order is the load order, so a dependency must
            # name an endpoint an earlier image exports: the loader refuses
            # the image otherwise.
            exports = set()
            for earlier in LOADS_BEFORE[name]:
                other = json.loads(read(os.path.join(SERVICE_MANIFESTS,
                                                     earlier + ".json")))
                exports.update((endpoint["class"], endpoint["instance"])
                               for endpoint in other["endpoints"])
            for dependency in manifest.get("depends") or []:
                self.assertIn((dependency["class"], dependency["instance"]),
                              exports, name)

    def test_recovery_manifest_carries_the_client_program(self):
        manifest = json.loads(read(MANIFEST))
        names = json.dumps(manifest)
        self.assertIn("sio.zmod", names)
        self.assertIn("ipc.zex", names)


if __name__ == "__main__":
    unittest.main()
