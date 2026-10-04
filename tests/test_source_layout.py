#!/usr/bin/env python3
"""Source-tree ownership tests (work packages 09-10, extended for M6).

Enforces docs/architecture.md section 3.1 and the current layout:

- implemented sources live under their owning boot/kernel/driver/test
  directories, and driver directories contain device implementation only;
- platform/zephyr80/ contains declarative constants/configuration only,
  with no executable content or instruction-emitting macros;
- production module metadata and shared module runtime support have
  explicit non-driver owners (modules/manifests, modules/runtime);
- the M6 process lifecycle (kernel/process), the common transition gates
  (kernel/memory), and the user runtime (user/runtime) have explicit
  owners, with the ZEX program sources under tests/target/m6 and the
  ZEX tooling/fixtures under tools/ and tests/m6;
- all milestone acceptance code lives under tests/target/ (or
  tests/modules/ for test ZMODs), behind the single ZOS_TARGET_TEST_BUILD
  seam; production sources contain no self-test implementations;
- the retired M3 static-driver mechanism, driver-private SDCC glue, and
  test-only SIO build variants cannot return;
- live build/source documentation contains no references to the removed
  implementation paths (dated acceptance records under docs/validation/
  and the work/ contracts are intentionally excluded);
- root build* directories are generated output only: .gitignore covers
  every build variant and make clean removes them all.

Run with: make test
"""

import os
import re
import shutil
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

PLATFORM_DIR = os.path.join(ROOT, "platform", "zephyr80")

# The complete, exact platform file set: constants (.h/.inc) and build
# configuration (.mk) only.
PLATFORM_FILES = {
    "hardware.h", "hardware.inc",
    "memory.h", "memory.inc",
    "irq.h", "irq.inc",
    "config.mk",
}

# Implementation files that must exist under their owning directories.
OWNER_FILES = (
    "boot/start.asm",
    "boot/shadow_copy.asm",
    "kernel/memory/banking.asm",
    "kernel/syscall/entry.asm",
    "kernel/syscall/page_zero.asm",
    "kernel/syscall/diag.c",
    "kernel/syscall/diag.h",
    "kernel/syscall/diag_io.asm",
    "kernel/irq/init.asm",
    "kernel/irq/entry.asm",
    "kernel/irq/vectors.asm",
    "kernel/irq/irq_private.h",
    "kernel/device/registry.h",
    "kernel/memory/banking.h",
    "kernel/runtime/sdcc_helpers.asm",
    "kernel/module/arena.asm",
    "kernel/module/loader.c",
    "kernel/module/relocate.c",
    "kernel/romfs/romfs.c",
    "kernel/romfs/rom_access.asm",
    "kernel/process/process.c",
    "kernel/process/exec.c",
    "kernel/process/launch.asm",
    "kernel/memory/user_copy.asm",
    "include/zephyros/romfs.h",
    "include/zephyros/process.h",
    "include/zephyros/zex.h",
    "user/runtime/zex_runtime.asm",
    "user/runtime/zex_runtime.h",
    "user/runtime/zex_diag_runtime.asm",
    "user/runtime/console_io.c",
    "user/runtime/console_io.h",
    "user/runtime/hexfmt.c",
    "user/runtime/hexfmt.h",
    "user/runtime/hexdump.c",
    "user/runtime/hexdump.h",
    "user/runtime/status.c",
    "user/runtime/status.h",
    "user/monitor/monitor.c",
    "user/monitor/monitor.h",
    "user/monitor/monitor_mem.c",
    "user/monitor/monitor_io.c",
    "user/monitor/monitor_entry.asm",
    "user/monitor/monitor.json",
    "tools/zexpack.py",
    "recovery/manifest.json",
    "tests/m5/manifest.json",
    "tests/m5/gen_asset.py",
    "tests/m5/gen_expect.py",
    "tests/m5/corrupt_romfs.py",
    "tests/m6/manifest.json",
    "tests/m6/manifest-failure.json",
    "tests/m6/zextest.json",
    "tests/m6/gen_fixtures.py",
    "tests/m6/gen_expect.py",
    "tests/m7/manifest.json",
    "tests/target/m5/m5_test.c",
    "tests/target/m6/zextest.c",
    "tests/target/m6/zextest_entry.asm",
    "tests/target/m6/m6_test.c",
    "tests/target/m7/m7_test.c",
    "tests/target/include/m5_selftest.h",
    "tests/target/include/m6_selftest.h",
    "tests/target/include/m7_selftest.h",
    "drivers/ctc/ctc.c",
    "drivers/sio/sio.c",
    "drivers/sio/sio.h",
    "drivers/sio/sio_io.asm",
    "drivers/sio/sio_module.c",
    "drivers/ioc/ioc.c",
    "drivers/ioc/ioc_driver.h",
    "drivers/ioc/ioc_io.asm",
    "drivers/ioc/ioc_module.c",
    "drivers/ioc/ioc_proto.h",
    "include/zephyros/ioc.h",
    "include/zephyros/ioc_protocol.h",
    "modules/manifests/ioc.json",
    "drivers/hid/hid.c",
    "drivers/hid/hid_driver.h",
    "drivers/hid/hid_module.c",
    "include/zephyros/hid.h",
    "modules/manifests/hid.json",
    "drivers/v9958/v9958.c",
    "drivers/v9958/v9958_driver.h",
    "drivers/v9958/v9958_io.asm",
    "drivers/v9958/v9958_module.c",
    "drivers/v9958/assets/font_cp850_6x8.inc",
    "include/zephyros/video.h",
    "modules/manifests/v9958.json",
    "tests/m8/manifest-v9958-fontfail.json",
    "tests/m8/v9958-font-stub.bin",
    "tests/target/m8/m8_v9958_test.c",
    "tests/target/include/m8_v9958_selftest.h",
    "tests/target/m8/m8_ioc_test.c",
    "tests/target/include/m8_ioc_selftest.h",
    "modules/manifests/sio.json",
    "modules/runtime/sdcc_indirect_calls.asm",
    "tests/modules/sio-irq.json",
    "tests/modules/sio-fail.json",
    "tests/modules/sio_irq_module.c",
    "tests/modules/sio_fail_module.c",
    "tests/modules/m8_api.c",
    "tests/modules/m8_api.json",
    "tests/target/m1/selftest.asm",
    "tests/target/m1/syscall_test.c",
    "tests/target/m2/irq_selftest.asm",
    "tests/target/m2/irq_test.c",
    "tests/target/m3/m3_test.c",
    "tests/target/m3/m3_irq_probe.c",
    "tests/target/m3/m3_probe_io.asm",
    "tests/target/m3/m3_fixtures.c",
    "tests/target/m4/m4_test.c",
    "tests/target/m4/m4_fixture_catalog.c",
    "tests/target/include/selftest.h",
    "tests/target/include/irq_selftest.inc",
    "tests/target/include/m3_selftest.h",
    "tests/target/include/m4_selftest.h",
    "tests/target/include/ctc_test.h",
    "tests/target/support/target_test.c",
    "tests/target/support/ctc_test.c",
)

# Implementation paths removed by the ownership refactors.  None of these
# may be referenced by live build/source files.
REMOVED_PATHS = (
    "platform/zephyr80/reset.asm",
    "platform/zephyr80/banking.asm",
    "platform/zephyr80/page_zero.asm",
    "platform/zephyr80/syscall_entry.asm",
    "platform/zephyr80/irq.asm",
    "platform/zephyr80/irq_sources.c",
    "platform/zephyr80/sdcc_helpers.asm",
    "platform/zephyr80/selftest.asm",
    "platform/zephyr80/selftest.inc",
    "platform/zephyr80/irq_selftest.asm",
    "platform/zephyr80/irq_selftest.inc",
    "drivers/sio/driver.c",
    "drivers/sio/module.json",
    "drivers/sio/module-irq.json",
    "drivers/sio/module-fail.json",
    "drivers/sio/sdcc_glue.asm",
    "tests/modules/m4_sdcc_glue.asm",
    "kernel/device/driver.c",
    "kernel/irq/irq_selftest.c",
    "kernel/irq/selftest.c",
    "kernel/device/m3_selftest.c",
    "kernel/module/m4_selftest.c",
    "drivers/test/irq_probe.c",
    "drivers/test/fixtures.c",
    "drivers/test/irq_probe_io.asm",
    "include/zephyros/selftest.h",
    "include/zephyros/irq_selftest.h",
    "include/zephyros/m3_selftest.h",
    "include/zephyros/m4_selftest.h",
    "tests/target/zephyr80/selftest.asm",
    "tests/target/zephyr80/irq_selftest.asm",
)

# Directories that must exist after the production/test separation.  Every
# acceptance implementation has a milestone owner under tests/target/;
# test ZMODs live under tests/modules/; production module metadata and
# runtime support have their own non-driver owners.
REQUIRED_DIRS = (
    "boot",
    "kernel/memory", "kernel/syscall", "kernel/irq", "kernel/runtime",
    "kernel/module", "kernel/romfs", "kernel/process",
    "drivers/sio", "drivers/ctc", "drivers/ioc", "drivers/hid",
    "drivers/v9958", "modules/manifests", "modules/runtime",
    "user/runtime", "user/monitor",
    "recovery",
    "tests/m5", "tests/m6", "tests/m7", "tests/m8", "tests/modules",
    "tests/target/m1", "tests/target/m2", "tests/target/m3",
    "tests/target/m4", "tests/target/m5", "tests/target/m6",
    "tests/target/m7", "tests/target/m8",
    "tests/target/include",
    "tests/target/support",
)

# The retired M3 static-driver mechanism must not reappear in live sources.
RETIRED_SOURCE_PATTERNS = (
    re.compile(r"\bboot_drivers\b"),
    re.compile(r"\bdriver_init_static\b"),
    re.compile(r"\bdriver_static_\w+\b"),
    re.compile(r"\bstruct zdriver\b"),
)

# Test-only SIO build variants must be confined to tests/ (the production
# module entry has no conditional test branches).
TEST_ONLY_SIO_MACROS = (
    "ZOS_SIO_MODULE_IRQ_SELFTEST",
    "ZOS_SIO_MODULE_INIT_FAIL",
)

# Milestone self-test implementations live only under tests/.  Production
# files may reference ZOS_M4_SIO_IRQ solely through the labeled boot-phase
# TEST-ONLY HOOK in kernel/main.c and the shared ZOS_TARGET_TEST_BUILD seam.
PRODUCTION_SELFTEST_MACROS = (
    "ZOS_SYSCALL_SELFTEST",
    "ZOS_IRQ_SELFTEST",
    "ZOS_M3_SELFTEST",
    "ZOS_M4_SELFTEST",
    "ZOS_M4_FAILURE_SELFTEST",
    "ZOS_M5_SELFTEST",
    "ZOS_M5_CORRUPT_SELFTEST",
    "ZOS_M6_SELFTEST",
    "ZOS_M6_FAILURE_SELFTEST",
    "ZOS_M7_SELFTEST",
    "ZOS_M8_IOC_SELFTEST",
    "ZOS_M8_HID_SELFTEST",
    "ZOS_M8_HID_NOIOC",
    "ZOS_M8_HID_BADIOC",
    "ZOS_M8_V9958_SELFTEST",
    "ZOS_M8_V9958_FONTFAIL",
)

# Generated-output names that must be ignored and cleaned.  build-modules is
# the shared module directory used by the ZMOD targets.
BUILD_VARIANTS = (
    "build",
    "build-selftest",
    "build-irq-selftest",
    "build-m3-selftest",
    "build-m4-loader-selftest",
    "build-m4-selftest",
    "build-m4-placement-selftest",
    "build-m4-failure",
    "build-m5-selftest",
    "build-m5-corrupt-selftest",
    "build-m6-selftest",
    "build-m6-failure-selftest",
    "build-m7-selftest",
    "build-m8-ioc-selftest",
    "build-m8-hid-selftest",
    "build-m8-hid-noioc-selftest",
    "build-m8-hid-badioc-selftest",
    "build-m8-v9958-selftest",
    "build-m8-v9958-fontfail-selftest",
    "build-modules",
)

# Assembler directives/keywords that emit target bytes or define routines.
EMITTER_PATTERNS = (
    re.compile(r"^\s*\.(area|org|globl|global|db|dw|ds|byte|word|module"
               r"|macro|endm|rept|endr|irp|irpc)\b"),
    re.compile(r"^\s*(ld|out|in|jp|jr|call|reti?|di|ei|im|djnz|push|pop"
               r"|halt|exx?|cpl|neg|nop|scf|ccf|rlca|rrca|rla|rra|daa"
               r"|add|adc|sub|sbc|and|or|xor|cp|inc|dec|bit|set|res"
               r"|rlc|rrc|rl|rr|sla|sra|srl|sli)\s"),
)

# Live documentation/build files that must never name the removed paths.
SCAN_ROOTS = (
    "boot", "docs", "drivers", "fs", "include", "kernel", "libk",
    "linker", "platform", "services", "tests", "tools",
)
SCAN_TOP_FILES = ("Makefile", "README.md")

# Historical/worker records and this test's own data are exempt.
SCAN_EXCLUDED_DIRS = {
    os.path.join(ROOT, "docs", "validation"),
    os.path.join(ROOT, "work"),
    os.path.join(ROOT, "tests", "__pycache__"),
}
SCAN_EXCLUDED_FILES = {os.path.join(HERE, "test_source_layout.py")}


def _read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def _strip_comments(path, text):
    if path.endswith((".inc", ".mk")) or os.path.basename(path) == "Makefile":
        return "\n".join(line.split(";")[0] for line in text.splitlines())
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _code_only(path):
    return _strip_comments(path, _read(path))


def _walk(root_name, suffixes=None):
    root = os.path.join(ROOT, root_name)
    for dirpath, _, filenames in os.walk(root):
        for name in sorted(filenames):
            if suffixes and not name.endswith(suffixes):
                continue
            yield os.path.join(dirpath, name)


# Production source roots scanned for cross-driver private includes.  Test
# trees are excluded: a test-only IOC diagnostic may use the private
# include path when it exercises transport-private behavior.
PRODUCTION_SOURCE_ROOTS = ("boot", "drivers", "include", "kernel",
                           "modules", "user")
SOURCE_SUFFIXES = (".c", ".h", ".asm", ".inc")

DRIVERS_DIR = os.path.join(ROOT, "drivers")


def _driver_private_headers():
    """Map every driver-private header basename to its owning driver dir."""
    owners = {}
    for name in sorted(os.listdir(DRIVERS_DIR)):
        directory = os.path.join(DRIVERS_DIR, name)
        if not os.path.isdir(directory):
            continue
        for entry in sorted(os.listdir(directory)):
            if entry.endswith((".h", ".inc")):
                owners[entry] = "drivers/" + name
    return owners


def _owning_driver(rel_path):
    """Return the drivers/<name> directory owning a repo-relative path."""
    parts = rel_path.replace(os.sep, "/").split("/")
    if len(parts) >= 3 and parts[0] == "drivers":
        return "drivers/" + parts[1]
    return None


def _sibling_private_includes(rel_path, text, owners):
    """Return quoted includes that pull a sibling driver's private header.

    `owners` maps a header basename to its owning `drivers/<name>`.  A
    file owned by `drivers/<A>` may include its own directory's headers,
    but not a header basename owned by another driver, nor a relative or
    explicit path that lands inside another driver's directory.
    """
    own = _owning_driver(rel_path)
    own_name = own.split("/")[1] if own is not None else None
    own_dir = os.path.dirname(rel_path.replace(os.sep, "/"))
    violations = []
    body = _strip_comments(rel_path, text)
    for match in re.finditer(r'#\s*include\s+"([^"]+)"', body):
        include = match.group(1).replace("\\", "/")
        owner = None
        if "/" not in include and include in owners:
            owner = owners[include]
        else:
            resolved = os.path.normpath(
                os.path.join(own_dir, include)).replace(os.sep, "/")
            if resolved.startswith("drivers/"):
                candidate = resolved.split("/")[1]
                if candidate != own_name:
                    owner = "drivers/" + candidate
        if owner is not None and owner != own:
            violations.append((rel_path, include, owner))
    return violations


def _tab_recipe_lines(text):
    """Yield (owning_rule_header, recipe_line) for tab-indented recipe lines.

    Only lines whose preceding non-indented statement looks like a rule
    (a `target: prereq` line, not a `VAR := value` assignment) are bound
    to that rule; continuation prerequisite lines are ignored for
    ownership because they do not name an include path.
    """
    header = ""
    for line in text.splitlines():
        if line.startswith("\t"):
            yield header, line
        elif line.strip() and not line.lstrip().startswith("#"):
            header = line if (":" in line and ":=" not in line) else ""
        else:
            header = ""


# The headers a ZMOD module author may read: the ABI, the class headers,
# and the narrow board-constant header a hardware-owning driver includes.
# (kernel/... is deliberately not reachable from the module build.)
MODULE_FACING_HEADERS = (
    "include/zephyros/driver.h",
    "include/zephyros/device.h",
    "include/zephyros/irq.h",
    "include/zephyros/ioc.h",
    "include/zephyros/hid.h",
    "include/zephyros/video.h",
    "platform/zephyr80/hardware.h",
)

# Declarations that must not appear anywhere in the module-facing include
# closure.  ABI field names (`device_register`, `irq_claim`,
# `irq_release`) are intentionally absent: they are the module service
# surface.  These are the kernel registry transaction/owner/storage,
# executive lifecycle, diagnostics, platform hooks, and raw memory
# primitives (work package 22).
KERNEL_ONLY_IDENTIFIERS = (
    "device_registry_init", "device_get", "device_registry_checkpoint",
    "device_registry_rollback", "device_registry_set_owner",
    "device_registry_owner", "device_table",
    "irq_init", "irq_registry_checkpoint", "irq_registry_rollback",
    "irq_registry_set_owner", "irq_registry_owner",
    "irq_unexpected_count", "irq_unclaimed_count", "irq_last_source",
    "irq_arch_lock", "irq_arch_unlock", "irq_arch_init",
    "platform_irq_vector", "platform_irq_quiesce",
    "platform_ctc0_test_start", "platform_ctc0_stop",
    "platform_memctl_read", "platform_memctl_write",
    "platform_bank_get", "platform_bank_set",
)

# The include paths a representative module is compiled with: only the
# shared include/ and platform/ surfaces plus its own driver directory.
MODULE_PROBE_FLAGS = ("-mz80", "-c", "--nostdinc", "--Werror", "-I", "include",
                      "-I", "platform", "-I", "drivers/sio")

_MODULE_PROBE_SOURCE = """\
#include "zephyros/driver.h"
#include "zephyros/device.h"
#include "zephyros/irq.h"
#include "zephyros/ioc.h"
#include "zephyros/hid.h"
#include "zephyros/video.h"
#include "zephyr80/hardware.h"

static const struct zdevice_desc probe_desc = {
\tZDEV_HID, ZCAP_HID_TEXT, "probe", (const struct zchar_ops *)0,
\t(const void *)0, (void *)0
};

int probe_init(const struct zdriver_api *api)
{
\tzdev_handle_t handle;
\tif (api->device_register(&probe_desc, &handle) != ZDRIVER_OK) {
\t\treturn ZDRIVER_FAIL;
\t}
\treturn api->device_find(ZDEV_HID, ZCAP_HID_TEXT, 0u) == handle ?
\t\tZDRIVER_OK : ZDRIVER_FAIL;
}
"""


def _resolve_quoted_include(name, origin_dir):
    """Resolve a quoted include the way the module build does."""
    candidates = [os.path.join(origin_dir, name)]
    if name.startswith("zephyr80/"):
        candidates.append(os.path.join(ROOT, "platform", name))
    else:
        candidates.append(os.path.join(ROOT, "include", name))
        candidates.append(os.path.join(ROOT, "platform", name))
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    return None


def _header_closure(rel_paths):
    """Return the reachable repo-relative quote-include closure."""
    seen = []
    pending = list(rel_paths)
    while pending:
        rel = pending.pop(0)
        if rel in seen:
            continue
        seen.append(rel)
        text = _read(os.path.join(ROOT, rel))
        origin = os.path.dirname(os.path.join(ROOT, rel))
        for match in re.finditer(r'#\s*include\s+"([^"]+)"', text):
            resolved = _resolve_quoted_include(match.group(1), origin)
            if resolved is not None:
                candidate = os.path.relpath(resolved, ROOT).replace(
                    os.sep, "/")
                if candidate not in seen:
                    pending.append(candidate)
    return seen


class TestOwnerLayout(unittest.TestCase):
    def test_owner_files_exist(self):
        for rel in OWNER_FILES:
            self.assertTrue(os.path.isfile(os.path.join(ROOT, rel)),
                            "missing owner file %s" % rel)

    def test_owner_directories_exist(self):
        for rel in REQUIRED_DIRS:
            self.assertTrue(os.path.isdir(os.path.join(ROOT, rel)),
                            "missing owner directory %s" % rel)

    def test_makefile_includes_platform_config(self):
        text = _read(os.path.join(ROOT, "Makefile"))
        self.assertIn("platform/zephyr80/config.mk", text)
        self.assertIn("include platform/zephyr80/config.mk", text)


class TestProductionTestSeparation(unittest.TestCase):
    """Production trees contain implementation only; acceptance code,
    test ZMOD entries, manifests and glue live under their explicit
    non-production owners, behind the single test-build seam."""

    def test_driver_directories_contain_device_implementation_only(self):
        self.assertEqual(sorted(os.listdir(os.path.join(ROOT, "drivers", "sio"))),
                         ["sio.c", "sio.h", "sio_io.asm", "sio_module.c"])
        self.assertEqual(sorted(os.listdir(os.path.join(ROOT, "drivers", "ctc"))),
                         ["ctc.c"])
        self.assertEqual(sorted(os.listdir(os.path.join(ROOT, "drivers", "ioc"))),
                         ["ioc.c", "ioc_driver.h", "ioc_io.asm",
                          "ioc_module.c", "ioc_proto.h"])
        self.assertEqual(sorted(os.listdir(os.path.join(ROOT, "drivers", "hid"))),
                         ["hid.c", "hid_driver.h", "hid_module.c"])
        self.assertEqual(sorted(os.listdir(os.path.join(ROOT, "drivers", "v9958"))),
                         ["assets", "v9958.c", "v9958_driver.h",
                          "v9958_io.asm", "v9958_module.c"])

    def test_no_json_manifests_under_drivers(self):
        for path in _walk("drivers"):
            self.assertFalse(
                path.endswith(".json"),
                "ZMOD manifest belongs under modules/ or tests/modules/: %s"
                % os.path.relpath(path, ROOT))

    def test_no_driver_private_sdcc_glue(self):
        for path in _walk("drivers", (".c", ".h", ".asm", ".inc")):
            body = _code_only(path)
            for symbol in ("___sdcc_call_iy", "___sdcc_call_hl"):
                self.assertNotIn(
                    symbol, body,
                    "%s defines module-local SDCC glue; the shared source is "
                    "modules/runtime/sdcc_indirect_calls.asm"
                    % os.path.relpath(path, ROOT))

    def test_retired_static_driver_symbols_absent(self):
        roots = ("boot", "drivers", "include", "kernel", "modules")
        for root_name in roots:
            for path in _walk(root_name):
                if not path.endswith((".c", ".h", ".asm", ".inc")):
                    continue
                body = _code_only(path)
                for pattern in RETIRED_SOURCE_PATTERNS:
                    self.assertIsNone(
                        pattern.search(body),
                        "%s still references the retired M3 static-driver "
                        "mechanism: %r"
                        % (os.path.relpath(path, ROOT), pattern.pattern))
        makefile = _read(os.path.join(ROOT, "Makefile"))
        self.assertNotIn("ZOS_M3_NO_STATIC_DRIVERS", makefile)
        self.assertNotIn("ZOS_M3_NO_CONSOLE_BIND", makefile)
        self.assertNotIn("m3-fallback", makefile)

    def test_test_only_sio_macros_confined_to_tests(self):
        paths = list(_walk("drivers"))
        paths += list(_walk("kernel", (".c", ".h")))
        for path in paths:
            body = _code_only(path)
            for macro in TEST_ONLY_SIO_MACROS:
                self.assertNotIn(
                    macro, body,
                    "%s compiles a test SIO variant into production code"
                    % os.path.relpath(path, ROOT))

    def test_production_sources_have_no_milestone_selftests(self):
        for root_name in ("boot", "drivers", "kernel", "modules"):
            for path in _walk(root_name):
                if not path.endswith((".c", ".h", ".asm", ".inc")):
                    continue
                body = _code_only(path)
                for macro in PRODUCTION_SELFTEST_MACROS:
                    self.assertNotIn(
                        macro, body,
                        "%s contains a milestone self-test implementation; "
                        "acceptance code belongs under tests/target/"
                        % os.path.relpath(path, ROOT))

    def test_main_keeps_only_labeled_test_hooks(self):
        main = _read(os.path.join(ROOT, "kernel", "main.c"))
        self.assertIn("ZOS_TARGET_TEST_BUILD", main)
        self.assertIn("tests/target/support/target_test.c", main)
        self.assertIn("TEST-ONLY HOOK", main)
        self.assertIn("tests/modules/sio_irq_module.c", main)

    def test_production_sources_have_no_ctc_acceptance_helpers(self):
        # The CTC0 programming is test-owned; no production source may
        # define, declare, or call it, and the obsolete vector lookup is
        # gone (work package 23).
        for root_name in ("boot", "drivers", "kernel", "modules"):
            for path in _walk(root_name, SOURCE_SUFFIXES):
                body = _code_only(path)
                for token in ("platform_ctc0_test_start",
                              "platform_ctc0_stop", "platform_irq_vector",
                              "zos_ctc0_test_start", "zos_ctc0_test_stop"):
                    self.assertNotIn(
                        token, body,
                        "%s contains acceptance-only CTC code %r"
                        % (os.path.relpath(path, ROOT), token))

    def test_production_headers_declare_no_test_runners(self):
        for name in ("irq.h", "module.h", "driver.h", "device.h",
                     "console.h", "platform.h", "kernel.h", "syscall.h"):
            body = _code_only(os.path.join(ROOT, "include", "zephyros", name))
            self.assertNotIn("zos_run_", body,
                             "%s declares a target-test runner" % name)

    def test_makefile_has_single_test_seam(self):
        text = _read(os.path.join(ROOT, "Makefile"))
        self.assertIn("ZOS_TARGET_TEST_BUILD=1", text)
        self.assertIn("tests/target/include", text)
        self.assertIn("target_test.rel", text)
        self.assertNotIn("SELFTEST_OBJS", text)


class TestCrossDriverIncludeBoundary(unittest.TestCase):
    """A production driver reaches a sibling only through a public header
    (include/, platform/) or its own directory's private headers.  A
    sibling driver's private include path and private headers are never a
    production dependency edge (work package 21)."""

    def test_global_module_flags_do_not_expose_ioc_private_dir(self):
        text = _read(os.path.join(ROOT, "Makefile"))
        match = re.search(r"^MOD_CFLAGS\s*:?=\s*(.*?)$", text, re.M)
        self.assertIsNotNone(match, "Makefile has no MOD_CFLAGS assignment")
        self.assertNotIn(
            "drivers/ioc", match.group(1),
            "the shared module flags still expose drivers/ioc/")

    def test_only_test_rules_use_the_ioc_private_include_path(self):
        text = _read(os.path.join(ROOT, "Makefile"))
        offenders = []
        for header, recipe in _tab_recipe_lines(text):
            if "-Idrivers/ioc" in recipe or "-I drivers/ioc" in recipe:
                if "tests/" not in header:
                    offenders.append("%s -> %s"
                                     % (header.strip(), recipe.strip()))
        self.assertEqual(
            offenders, [],
            "a production rule uses the IOC private include path:\n%s"
            % "\n".join(offenders))

    def test_production_sources_have_no_sibling_private_includes(self):
        owners = _driver_private_headers()
        violations = []
        for root_name in PRODUCTION_SOURCE_ROOTS:
            for path in _walk(root_name, SOURCE_SUFFIXES):
                rel = os.path.relpath(path, ROOT)
                violations += _sibling_private_includes(
                    rel, _read(path), owners)
        self.assertEqual(
            violations, [],
            "production sources include sibling-driver private headers:\n%s"
            % "\n".join("%s includes %r (owned by %s)" % item
                        for item in violations))

    def test_sibling_private_include_checker_detects_a_leak(self):
        # Negative fixture: the checker must flag a deliberate
        # sibling-driver private-header edge (the original HID -> IOC
        # bug) and accept the public contract and same-directory headers.
        owners = {"ioc_proto.h": "drivers/ioc",
                  "hid_driver.h": "drivers/hid"}
        leaked = ('#include "zephyros/device.h"\n'
                  '#include "ioc_proto.h"\n')
        self.assertTrue(_sibling_private_includes(
            "drivers/hid/hid.c", leaked, owners))
        relative = '#include "../ioc/ioc_proto.h"\n'
        self.assertTrue(_sibling_private_includes(
            "drivers/hid/hid.c", relative, owners))
        allowed = ('#include "zephyros/ioc_protocol.h"\n'
                   '#include "hid_driver.h"\n')
        self.assertEqual(_sibling_private_includes(
            "drivers/hid/hid.c", allowed, owners), [])


class TestModuleFacingHeaderBoundary(unittest.TestCase):
    """A module author reads the ABI types and callbacks in driver.h and
    its class headers, but not kernel registry transactions, IRQ-executive
    lifecycle controls, platform hooks, or raw memory primitives (work
    package 22)."""

    def test_module_facing_closure_has_no_kernel_only_declarations(self):
        closure = _header_closure(MODULE_FACING_HEADERS)
        offenders = []
        for rel in closure:
            text = _strip_comments(rel, _read(os.path.join(ROOT, rel)))
            for ident in KERNEL_ONLY_IDENTIFIERS:
                if re.search(r"\b%s\b" % re.escape(ident), text):
                    offenders.append("%s declares %s" % (rel, ident))
        self.assertEqual(
            offenders, [],
            "kernel-only declarations leak into the module-facing header "
            "closure:\n%s" % "\n".join(offenders))

    def test_module_facing_closure_keeps_the_abi_surface(self):
        headers = _header_closure(MODULE_FACING_HEADERS)
        text = "".join(_read(os.path.join(ROOT, rel)) for rel in headers)
        for needle in ("struct zdriver_api", "struct zdevice_desc",
                       "struct zchar_ops", "irq_source_t", "irq_handler_t",
                       "ZCAP_IOC_COMMAND", "ZDEV_HID"):
            self.assertIn(needle, text)

    @unittest.skipUnless(shutil.which("sdcc"), "sdcc not available")
    def test_representative_module_compiles_against_module_headers_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = os.path.join(tmp, "probe.c")
            with open(source, "w") as handle:
                handle.write(_MODULE_PROBE_SOURCE)
            result = subprocess.run(
                ["sdcc"] + list(MODULE_PROBE_FLAGS) +
                ["-o", os.path.join(tmp, "probe.rel"), source],
                cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True)
            self.assertEqual(
                result.returncode, 0,
                "a representative module failed to compile against the "
                "module-facing headers alone:\n%s" % result.stdout)
            # A kernel-only reference must not resolve against these
            # headers alone; this is the negative half of the boundary.
            leak = os.path.join(tmp, "leak.c")
            with open(leak, "w") as handle:
                handle.write('#include "zephyros/device.h"\n'
                             "int probe_leak(void) {\n"
                             "\tdevice_registry_rollback(0u);\n"
                             "\treturn 0;\n}\n")
            result = subprocess.run(
                ["sdcc"] + list(MODULE_PROBE_FLAGS) +
                ["-o", os.path.join(tmp, "leak.rel"), leak],
                cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True)
            self.assertNotEqual(
                result.returncode, 0,
                "a kernel-only symbol resolved from the module-facing "
                "headers:\n%s" % result.stdout)


class TestPlatformBoundary(unittest.TestCase):
    def test_platform_contains_only_constant_configuration_files(self):
        actual = set(os.listdir(PLATFORM_DIR))
        self.assertEqual(actual, PLATFORM_FILES,
                         "platform/zephyr80/ must contain constants and "
                         "configuration only (unexpected: %s, missing: %s)"
                         % (sorted(actual - PLATFORM_FILES),
                            sorted(PLATFORM_FILES - actual)))

    def test_no_target_code_suffixes(self):
        for name in sorted(os.listdir(PLATFORM_DIR)):
            self.assertIn(os.path.splitext(name)[1], (".h", ".inc", ".mk"),
                          "platform file %s has a target-code suffix" % name)

    def test_no_executable_content(self):
        for name in sorted(os.listdir(PLATFORM_DIR)):
            path = os.path.join(PLATFORM_DIR, name)
            body = _strip_comments(path, _read(path))
            for lineno, line in enumerate(body.splitlines(), start=1):
                for pattern in EMITTER_PATTERNS:
                    self.assertIsNone(
                        pattern.match(line),
                        "%s:%d looks like target-emitting content: %r"
                        % (name, lineno, line.strip()))

    def test_no_function_like_macros(self):
        for name in sorted(os.listdir(PLATFORM_DIR)):
            if not name.endswith(".h"):
                continue
            path = os.path.join(PLATFORM_DIR, name)
            for lineno, line in enumerate(_read(path).splitlines(), start=1):
                self.assertIsNone(
                    re.match(r"\s*#\s*define\s+[A-Za-z_]\w*\(", line),
                    "%s:%d defines a function-like macro: %r"
                    % (name, lineno, line.strip()))

    def test_config_mk_lists_no_implementation_sources(self):
        body = _strip_comments(
            "config.mk", _read(os.path.join(PLATFORM_DIR, "config.mk")))
        for line in body.splitlines():
            self.assertIsNone(
                re.search(r"\b\w+\.(c|asm|rel|o)\b", line),
                "config.mk names an implementation source: %r" % line.strip())

    def test_platform_h_includes_constant_headers(self):
        text = _read(os.path.join(ROOT, "include", "zephyros", "platform.h"))
        for header in ("zephyr80/hardware.h", "zephyr80/memory.h",
                       "zephyr80/irq.h"):
            self.assertIn('#include "%s"' % header, text)


class TestNoStaleRemovedPaths(unittest.TestCase):
    def _live_files(self):
        for name in SCAN_TOP_FILES:
            path = os.path.join(ROOT, name)
            if os.path.isfile(path):
                yield path
        for root_name in SCAN_ROOTS:
            for dirpath, dirnames, filenames in os.walk(
                    os.path.join(ROOT, root_name)):
                dirnames[:] = [d for d in dirnames
                               if os.path.join(dirpath, d)
                               not in SCAN_EXCLUDED_DIRS]
                if dirpath in SCAN_EXCLUDED_DIRS:
                    continue
                for filename in filenames:
                    path = os.path.join(dirpath, filename)
                    if path in SCAN_EXCLUDED_FILES:
                        continue
                    if filename.endswith((".pyc",)):
                        continue
                    yield path

    def test_live_files_do_not_name_removed_paths(self):
        stale = []
        for path in self._live_files():
            try:
                text = _read(path)
            except (IOError, OSError):
                continue
            for removed in REMOVED_PATHS:
                if removed in text:
                    stale.append("%s: %s"
                                 % (os.path.relpath(path, ROOT), removed))
        self.assertEqual(stale, [],
                         "live files still reference removed paths:\n%s"
                         % "\n".join(stale))

    def test_removed_platform_files_are_gone(self):
        for removed in REMOVED_PATHS:
            self.assertFalse(os.path.exists(os.path.join(ROOT, removed)),
                             "removed path still exists: %s" % removed)


class TestBuildOutputHygiene(unittest.TestCase):
    def _gitignore_patterns(self):
        patterns = []
        for line in _read(os.path.join(ROOT, ".gitignore")).splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                patterns.append(line)
        return patterns

    def test_gitignore_covers_every_build_variant(self):
        patterns = self._gitignore_patterns()

        def ignored(name):
            for pattern in patterns:
                if pattern == "/%s/" % name or pattern == "/%s" % name:
                    return True
                if pattern == "/build-*/" and name.startswith("build-"):
                    return True
                if pattern == "/build*/" and name.startswith("build"):
                    return True
            return False

        for name in BUILD_VARIANTS:
            self.assertTrue(ignored(name),
                            ".gitignore does not cover generated %s/" % name)

    def test_make_clean_removes_every_build_variant(self):
        text = _read(os.path.join(ROOT, "Makefile"))
        match = re.search(r"^clean:\n((?:\t.*\n)+)", text, re.M)
        self.assertIsNotNone(match, "Makefile has no clean: recipe")
        recipe = match.group(1)
        self.assertIn("rm -rf", recipe)
        self.assertIn("$(BUILD)", recipe)
        self.assertIn("build-*", recipe)

    def test_build_ignore_patterns_are_root_anchored(self):
        # Anchored patterns keep source directories (e.g. kernel/build/)
        # out of the generated-output ignore rules.
        for pattern in self._gitignore_patterns():
            if "build" in pattern:
                self.assertTrue(pattern.startswith("/"),
                                "build ignore pattern is not root-anchored: %r"
                                % pattern)

    def test_no_tracked_files_under_build_directories(self):
        import subprocess
        try:
            result = subprocess.run(
                ["git", "ls-files", "--"] + list(BUILD_VARIANTS),
                cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                text=True)
        except OSError:
            self.skipTest("git not available")
        if result.returncode != 0:
            self.skipTest("not a git work tree")
        tracked = [line for line in result.stdout.splitlines() if line.strip()]
        self.assertEqual(tracked, [],
                         "generated build directories contain tracked "
                         "files: %s" % tracked)


if __name__ == "__main__":
    unittest.main()
