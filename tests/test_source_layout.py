#!/usr/bin/env python3
"""Source-tree ownership tests (work packages 09-10).

Enforces docs/architecture.md section 3.1 and the M4-era layout:

- implemented sources live under their owning boot/kernel/driver/test
  directories, and driver directories contain device implementation only;
- platform/zephyr80/ contains declarative constants/configuration only,
  with no executable content or instruction-emitting macros;
- production module metadata and shared module runtime support have
  explicit non-driver owners (modules/manifests, modules/runtime);
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
    "kernel/irq/init.asm",
    "kernel/irq/entry.asm",
    "kernel/irq/vectors.asm",
    "kernel/runtime/sdcc_helpers.asm",
    "kernel/module/arena.asm",
    "kernel/module/loader.c",
    "kernel/module/relocate.c",
    "kernel/romfs/romfs.c",
    "kernel/romfs/rom_access.asm",
    "include/zephyros/romfs.h",
    "recovery/manifest.json",
    "tests/m5/manifest.json",
    "tests/m5/gen_asset.py",
    "tests/m5/gen_expect.py",
    "tests/m5/corrupt_romfs.py",
    "tests/target/m5/m5_test.c",
    "tests/target/include/m5_selftest.h",
    "drivers/ctc/ctc.c",
    "drivers/sio/sio.c",
    "drivers/sio/sio.h",
    "drivers/sio/sio_io.asm",
    "drivers/sio/sio_module.c",
    "modules/manifests/sio.json",
    "modules/runtime/sdcc_indirect_calls.asm",
    "tests/modules/sio-irq.json",
    "tests/modules/sio-fail.json",
    "tests/modules/sio_irq_module.c",
    "tests/modules/sio_fail_module.c",
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
    "tests/target/support/target_test.c",
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
    "kernel/module", "kernel/romfs",
    "drivers/sio", "drivers/ctc",
    "modules/manifests", "modules/runtime",
    "recovery",
    "tests/m5", "tests/modules",
    "tests/target/m1", "tests/target/m2", "tests/target/m3",
    "tests/target/m4", "tests/target/m5", "tests/target/include",
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

    def test_production_headers_declare_no_test_runners(self):
        for name in ("irq.h", "module.h", "driver.h", "device.h",
                     "console.h", "platform.h", "kernel.h"):
            body = _code_only(os.path.join(ROOT, "include", "zephyros", name))
            self.assertNotIn("zos_run_", body,
                             "%s declares a target-test runner" % name)

    def test_makefile_has_single_test_seam(self):
        text = _read(os.path.join(ROOT, "Makefile"))
        self.assertIn("ZOS_TARGET_TEST_BUILD=1", text)
        self.assertIn("tests/target/include", text)
        self.assertIn("target_test.rel", text)
        self.assertNotIn("SELFTEST_OBJS", text)


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
