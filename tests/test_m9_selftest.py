"""Host checks for the M9 integration acceptance package (31).

Pins the structural facts the `m9-selftest` image and its transcript depend
on:

  - the build wiring (target, guard define, ROMFS manifest, map check);
  - the manifest carrying the whole native acceptance set;
  - `fstest.zex` exec'ing the deep-matrix program `fsfat.zex` -- and why a
    second program exists at all (the `0100h-1FFFh` syscall argument window
    caps one ZEX image, since the SDK links its argument scratch at the
    image end);
  - `fsfat.zex` covering the section-33 items that do not fit in fstest;
  - `m9_selftest.c` running fstest, the depth probe and the two fault
    probes, then checking the pools are back to baseline;
  - the serial smoke mode and its completion contract.

Run with: `python3 -m unittest tests.test_m9_selftest`.
"""

import json
import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MAKEFILE = os.path.join(ROOT, "Makefile")
MAIN_C = os.path.join(ROOT, "kernel", "main.c")
TARGET_TEST = os.path.join(ROOT, "tests", "target", "support",
                           "target_test.c")
SELFTEST_C = os.path.join(ROOT, "tests", "target", "m9", "m9_selftest.c")
HEADER = os.path.join(ROOT, "tests", "target", "include",
                      "m9_integration_selftest.h")
FSTEST_C = os.path.join(ROOT, "tests", "target", "m9", "fstest", "fstest.c")
FSFAT_C = os.path.join(ROOT, "tests", "target", "m9", "fstest", "fsfat.c")
MANIFEST = os.path.join(ROOT, "tests", "m9", "manifest-selftest.json")
SERIAL_SMOKE = os.path.join(ROOT, "tools", "serial_smoke.py")


def read(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read()


class TestIntegrationBuildWiring(unittest.TestCase):
    def test_makefile_has_the_acceptance_image(self):
        text = read(MAKEFILE)
        self.assertIn("m9-selftest:", text)
        self.assertIn("ZOS_M9_SELFTEST=1", text)
        self.assertIn("PK_SUFFIX := -m9-selftest", text)
        self.assertIn("build-m9-selftest/zephyros-m9-selftest.bin", text)
        self.assertIn("tests/m9/manifest-selftest.json", text)

    def test_makefile_builds_the_deep_program_through_the_sdk(self):
        text = read(MAKEFILE)
        self.assertIn("include user/sdk/zex.mk", text)
        self.assertIn("zex_program,fsfat,tests/target/m9/fstest,fsfat.c",
                      text)

    def test_image_is_part_of_the_acceptance_matrix(self):
        text = read(MAKEFILE)
        self.assertIn("smoke-hardware-m9:", text)
        # The image joins the "build every named image" target.
        index = text.index("hardware-images: all")
        self.assertIn("m9-selftest", text[index:index + 400])

    def test_guard_define_selects_the_image(self):
        text = read(MAKEFILE)
        # The shared acceptance guard must include the new define, or the
        # target runner and the include path are compiled out.
        index = text.index("ifneq ($(filter 1,")
        self.assertIn("$(ZOS_M9_SELFTEST)", text[index:index + 700])

    def test_acceptance_manifest_carries_the_programs(self):
        data = json.loads(read(MANIFEST))
        names = [entry["name"] for entry in data["files"]]
        for name in ("sio.zmod", "ioc.zmod", "iocfat.zmod", "monitor.zex",
                     "fstest.zex", "fsfat.zex", "fsdeep.zex"):
            self.assertIn(name, names)
        # The card stores no RAM provider: `/` is the real volume.
        self.assertNotIn("ramfs.zmod", names)

    def test_target_test_seam_selects_the_image(self):
        text = read(TARGET_TEST)
        self.assertIn("ZOS_M9_SELFTEST", text)
        self.assertIn("zos_run_m9_selftest", text)

    def test_serial_smoke_has_the_mode(self):
        text = read(SERIAL_SMOKE)
        self.assertIn('"m9"', text)
        self.assertIn("M9INTEG_SELFTEST_OK", text)
        self.assertIn("m9 selftest: OK", text)
        self.assertIn("M9INTEG_FSFAT_PASS", text)


class TestFstestDelegation(unittest.TestCase):
    def test_fstest_execs_the_deep_program(self):
        text = read(FSTEST_C)
        self.assertIn('"/SYSTEM/fsfat.zex"', text)
        self.assertIn("ft_report(\"fat\", fat)", text)
        # The probe keeps the deep matrix out of images that do not pack it.
        self.assertIn("zos_open(\"/SYSTEM/fsfat.zex\", ZFS_O_READ)", text)
        self.assertIn("return -1;", text)
        # The reason for the split is recorded where the exec happens.
        self.assertIn("0100h-1FFFh", text)

    def test_deep_program_covers_the_missing_section_33_items(self):
        text = read(FSFAT_C)
        for case in ('fsf_report("namespace"', 'fsf_report("basic"',
                     'fsf_report("sizes"', 'fsf_report("large"',
                     'fsf_report("cache"', 'fsf_report("pool"',
                     'fsf_report("exdev"', 'fsf_report("errors"'):
            self.assertIn(case, text, "fsfat.c lacks %s" % case)
        # The size set is the pinned section-33 list plus the >64 KiB span.
        self.assertIn("1u, 127u, 128u, 129u", text)
        self.assertIn("511u, 512u, 513u", text)
        self.assertIn("65537UL", text)
        # Every size is read back and compared by an in-program check value.
        self.assertIn("fsf_expect_hash", text)
        self.assertIn("fsf_read_hash", text)


class TestIntegrationTargetTest(unittest.TestCase):
    def test_runs_the_whole_acceptance(self):
        text = read(SELFTEST_C)
        self.assertIn('process_exec("/SYSTEM/fstest.zex"', text)
        self.assertIn('process_exec("/SYSTEM/fsdeep.zex"', text)
        self.assertIn("ZOS_EXEC_MAX_DEPTH", text)
        self.assertIn("vfs_fd_active_count()", text)
        self.assertIn("process_free_banks()", text)

    def test_carries_the_two_fault_windows(self):
        text = read(SELFTEST_C)
        for fence in ("fault:stale:arm", "fault:stale:observed",
                      "fault:stale:fresh", "fault:stale:end",
                      "fault:write:arm", "fault:write:observed",
                      "fault:write:fresh", "fault:write:end"):
            self.assertIn(fence, text, "the target test lacks %r" % fence)
        # Both documented degraded paths are named outcomes.
        self.assertIn("m9 selftest: OK (card absent)", text)
        self.assertIn("m9 selftest: OK (read-only card)", text)

    def test_header_pins_the_result_codes(self):
        text = read(HEADER)
        for name in ("ZOS_M9IST_OK", "ZOS_M9IST_FAIL_FSTEST",
                     "ZOS_M9IST_FAIL_EXEC", "ZOS_M9IST_FAIL_DEPTH",
                     "ZOS_M9IST_FAIL_FAULT", "ZOS_M9IST_FAIL_LEAK"):
            self.assertIn(name, text)


if __name__ == "__main__":
    unittest.main()
