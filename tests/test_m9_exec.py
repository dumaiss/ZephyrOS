"""Host tests for the M9 nested-exec package (28).

Covers:
  - the pinned `SYS_EXEC 05h` number, the packed argument block and the
    user-runtime wrapper surface;
  - the nesting geometry (`ZOS_EXEC_MAX_DEPTH`, `ZOS_EXEC_FRAME_SAVE`,
    `ZOS_EXEC_FRAME_SLACK`) and the continuation request records;
  - the launch/resume primitive's structural contract: depth-0 skips the
    frame copy, the resume window after the copy takes no stack action,
    the resumed process's syscall-gate state is restored, and
    `process_launch()` performs no check that can fail;
  - the mapcheck/test-harness parity for the continuation state and the
    frame-save sizing;
  - the production image carrying no acceptance instrumentation;
  - the M9 exec acceptance build wiring (image, ROMFS manifest, children,
    serial smoke mode, target seam).
"""

import json
import os
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

SYSCALL_H = ROOT / "include" / "zephyros" / "syscall.h"
PROCESS_H = ROOT / "include" / "zephyros" / "process.h"
VFS_H = ROOT / "kernel" / "vfs" / "vfs.h"
LAUNCH_ASM = ROOT / "kernel" / "process" / "launch.asm"
PROCESS_C = ROOT / "kernel" / "process" / "process.c"
EXEC_C = ROOT / "kernel" / "process" / "exec.c"
SYSCALL_C = ROOT / "kernel" / "syscall.c"
RUNTIME_H = ROOT / "user" / "runtime" / "zex_exec_runtime.h"
RUNTIME_C = ROOT / "user" / "runtime" / "zos_fs.c"
ABI_DOC = ROOT / "docs" / "abi.md"
MAKEFILE = ROOT / "Makefile"
MAPCHECK = ROOT / "tools" / "mapcheck.py"
MAPTOOLS = ROOT / "tests" / "maptools.py"
SERIAL_SMOKE = ROOT / "tools" / "serial_smoke.py"
PACK_IMAGE = ROOT / "tools" / "pack_image.py"
ZMODPACK = ROOT / "tools" / "zmodpack.py"
MANIFEST_EXEC = ROOT / "tests" / "m9" / "manifest-exec.json"
TARGET_TEST = ROOT / "tests" / "target" / "support" / "target_test.c"

# work/28-m9-nested-exec/WORK.md: the pinned error mapping.
ERROR_MAPPING = (
    ("missing file", "ZOS_ENOENT"),
    ("directory", "ZOS_EISDIR"),
    ("malformed or oversize ZEX", "ZOS_EINVAL"),
    ("no free slot or bank", "ZOS_ENOSPC"),
    ("depth limit or frame too large", "ZOS_EBUSY"),
    ("provider read error", "unchanged"),
)

# Test-only names that must never reach a production map.
PRODUCTION_FORBIDDEN = (
    "_m9_exec_selftest_result",
    "_zos_run_m9_exec_selftest",
    "_m9_exec_test_rel",
    "_zos_exec_selftest_probe",
    "_ft_main",
    "_ch_main",
    "_dp_main",
    "_rw_main",
    "_target_test_main",
)


def _read(path):
    return Path(path).read_text()


def _constant(text, name):
    """Read a `#define NAME value` (header) or `NAME = value` (mapcheck)."""
    for pattern in (r"#define\s+%s\s+(0x[0-9A-Fa-f]+|\d+)",
                    r"^%s\s*=\s*(0x[0-9A-Fa-f]+|\d+)"):
        match = re.search(pattern % name, text, re.M)
        if match is not None:
            return int(match.group(1), 0)
    return None


def _code(text):
    """Strip `;` comments so structural checks only see instructions."""
    return "\n".join(line.split(";", 1)[0] for line in text.splitlines())


class TestExecAbi(unittest.TestCase):
    def test_syscall_number_is_pinned(self):
        self.assertEqual(_constant(_read(SYSCALL_H), "ZOS_SYSCALL_EXEC"),
                         0x05)

    def test_argument_block_is_packed_and_pinned(self):
        text = _read(SYSCALL_H)
        self.assertIn("sizeof(struct zos_exec_args) == 4", text)
        self.assertIn("__builtin_offsetof(struct zos_exec_args, path) == 0",
                      text)
        self.assertIn("__builtin_offsetof(struct zos_exec_args, length) == 2",
                      text)
        # The runtime reuses the FS path block: the two records are the
        # same four packed bytes (path@0, length@2), so one scratch union
        # arm serves both families.
        impl = _read(RUNTIME_C)
        self.assertIn("zos_fs_scratch.path.path", impl)
        self.assertIn("zfs_path_args", impl)

    def test_dispatcher_routes_exec(self):
        self.assertIn("case ZOS_SYSCALL_EXEC:", _read(SYSCALL_C))
        self.assertIn("syscall_exec", _read(SYSCALL_C))

    def test_user_runtime_wrapper(self):
        header = _read(RUNTIME_H)
        self.assertIn("int zos_exec(const char *path);", header)
        self.assertIn("zex_fs_runtime.h", header)
        impl = _read(RUNTIME_C)
        self.assertIn("int zos_exec(const char *path)", impl)
        # The runtime mirrors the pinned number and calls it through the
        # shared FS gate stub.
        self.assertRegex(impl, r"#define ZOS_EXEC\s+0x05u")
        self.assertIn("zfs_call(ZOS_EXEC", impl)

    def test_error_mapping_is_documented(self):
        text = _read(ABI_DOC)
        self.assertIn("SYS_EXEC", text)
        for condition, code in ERROR_MAPPING:
            self.assertIn(condition, text,
                          "docs/abi.md lacks the %r row" % condition)
            self.assertIn(code, text)


class TestNestingGeometry(unittest.TestCase):
    def setUp(self):
        self.header = _read(PROCESS_H)

    def test_depth_is_pinned_and_explained(self):
        self.assertEqual(_constant(self.header, "ZOS_EXEC_MAX_DEPTH"), 4)
        for line in ("depth 1: kernel continuation, no saved gate frame",
                     "depth 2: frame save #0 holds the shell's SYS_EXEC",
                     "depth 4: frame save #2 holds the compiler's"):
            self.assertIn(line, self.header,
                          "the depth example loses the %r case" % line)

    def test_frame_save_is_a_measured_16_byte_multiple(self):
        frame_save = _constant(self.header, "ZOS_EXEC_FRAME_SAVE")
        slack = _constant(self.header, "ZOS_EXEC_FRAME_SLACK")
        self.assertIsNotNone(frame_save)
        self.assertIsNotNone(slack)
        self.assertEqual(frame_save % 16, 0)
        self.assertGreater(slack, 0)
        self.assertLess(slack, frame_save)
        # The measurement behind the constant is recorded beside it.
        self.assertRegex(self.header,
                         r"captured continuation SP is exactly\s*\n?\s*\*?\s*"
                         r"zos_gate_stack_top - 32")

    def test_vfs_does_not_redefine_the_depth(self):
        text = _read(VFS_H)
        self.assertNotRegex(
            text, r"#define\s+ZOS_EXEC_MAX_DEPTH",
            "the nesting depth is owned by zephyros/process.h")
        self.assertRegex(text, r"ZOS_EXEC_MAX_DEPTH is owned by")
        self.assertIn("#include \"zephyros/process.h\"", text)

    def test_continuation_records_are_pinned(self):
        for needle in ("struct zos_process_launch_req",
                       "struct zos_process_resume_req",
                       "zos_process_continuation_size",
                       "zos_process_launch", "zos_process_resume",
                       "[ZOS_EXEC_MAX_DEPTH - 1][ZOS_EXEC_FRAME_SAVE]"):
            self.assertIn(needle, self.header,
                          "process.h lacks %s" % needle)

    def test_asm_frame_offset_chain_matches_the_constant(self):
        # The gate has no multiply instruction: it scales the frame-area
        # number with a shift chain whose shape must follow
        # ZOS_EXEC_FRAME_SAVE (a * 2 + a = 3a, then N doublings).
        text = _read(LAUNCH_ASM)
        marker = "a = a * ZOS_EXEC_FRAME_SAVE"
        sites = text.count(marker)
        self.assertEqual(sites, 2,
                         "launch.asm does not scale both frame offsets")
        expected = _constant(_read(PROCESS_H), "ZOS_EXEC_FRAME_SAVE")
        for chunk in text.split(marker)[:2]:
            tail = "\n".join(chunk.splitlines()[-14:])
            doublings = len(re.findall(r"add\s+hl,hl", tail)) - 1
            self.assertIn("add\thl,de", tail)
            factor = 3 * (1 << doublings)
            self.assertEqual(factor, expected,
                             "the frame-offset chain scales by 3 * 2**%d = "
                             "%d, not ZOS_EXEC_FRAME_SAVE (%d)"
                             % (doublings, factor, expected))

    def test_path_buffer_is_bank7_data_not_common(self):
        """The SYS_EXEC path buffer is only touched in mode 11, so it has
        no business in the permanently mapped common window; the
        transition-critical state does belong there (the resume gate
        restores it across the mapping change)."""
        launch = _read(LAUNCH_ASM)
        self.assertNotIn("_zos_process_path_scratch::", launch,
                         "the path buffer is still defined in the common "
                         "assembly data section")
        self.assertNotIn(".globl _zos_process_path_scratch", launch,
                         "launch.asm still exports the path buffer")
        process = _read(PROCESS_C)
        self.assertRegex(
            process, r"char zos_process_path_scratch\[ZOS_PATH_MAX\];")
        self.assertRegex(
            process,
            r"_Static_assert\(sizeof\(zos_process_path_scratch\) == "
            r"ZOS_PATH_MAX")
        for name in ("zos_proc_cont", "zos_proc_gate_sp",
                     "zos_proc_frames", "zos_proc_frames_end"):
            self.assertIn(name, launch,
                          "the resume gate needs %s in common memory" % name)

    def test_no_document_claims_common_memory_is_unreachable(self):
        """Common memory is mapped in mode 10 like any other region;
        placement there is a reachability/budget argument, never a
        protection one."""
        for rel in ("docs/abi.md", "docs/memory.md", "docs/vfs.md",
                    "docs/architecture.md",
                    "docs/validation/2026-10-05-m9-nested-exec.md",
                    "docs/validation/2026-10-05-m9-nested-exec-"
                    "correction-01.md",
                    "kernel/process/launch.asm"):
            text = _read(ROOT / rel)
            for claim in ("mode-10 child cannot reach",
                          "cannot reach it", "cannot reach them",
                          "child cannot reach"):
                self.assertNotIn(
                    claim, text,
                    "%s still claims common memory is out of reach" % rel)

    def test_ledger_reports_common_memory(self):
        map_path = ROOT / "build" / "zephyros.map"
        if not map_path.exists():
            self.skipTest("build/zephyros.map missing; run make first")
        text = _read(map_path)
        # The path buffer must be inside the bank-7 runtime data window,
        # not in the common window.
        match = re.search(r"^\s*([0-9A-Fa-f]{8})\s+_zos_process_path_scratch",
                          text, re.M)
        self.assertIsNotNone(match, "no path-buffer symbol in the map")
        addr = int(match.group(1), 16)
        data_base = re.search(r"^\s*([0-9A-Fa-f]{8})\s+s__DATA\b", text,
                              re.M)
        self.assertIsNotNone(data_base)
        self.assertGreaterEqual(addr, int(data_base.group(1), 16))
        self.assertLess(addr, 0xDF00)

        plan = json.loads(_read(ROOT / "build" / "arena.plan.json"))
        bank7 = plan["bank7"]
        for field in ("common_code_bytes", "common_data_bytes",
                      "common_used_bytes", "common_capacity_bytes"):
            self.assertIn(field, bank7,
                          "arena.plan.json does not report %s" % field)
        self.assertEqual(bank7["common_used_bytes"],
                         bank7["common_code_bytes"]
                         + bank7["common_data_bytes"])
        self.assertEqual(bank7["common_capacity_bytes"], 0xFD00 - 0xE000)
        self.assertLess(bank7["common_used_bytes"],
                        bank7["common_capacity_bytes"])

    def test_asm_arrays_use_the_pinned_constants(self):
        text = _read(LAUNCH_ASM)
        # 2-byte slots: 2 * ZOS_EXEC_MAX_DEPTH, byte slots: 1 per depth.
        self.assertIn(".ds\t8\t\t\t; saved SP per depth (2 * "
                      "ZOS_EXEC_MAX_DEPTH)", text)
        for name in ("zos_proc_gate_sp", "zos_proc_gate_latch",
                     "zos_proc_gate_iff", "zos_proc_frames",
                     "zos_proc_frames_end"):
            self.assertIn(name, text, "launch.asm lacks %s" % name)


class TestLaunchPrimitive(unittest.TestCase):
    def setUp(self):
        self.text = _read(LAUNCH_ASM)

    def test_depth_zero_needs_no_frame_copy(self):
        self.assertIn("Depth 0 (the kernel launcher) needs no frame copy",
                      self.text)

    def test_resume_restores_the_callers_gate_state(self):
        # The syscall gate's transition state is single-depth; a nested
        # child overwrites it, so the resume must put the resumed
        # process's own copy back before the gate returns to it.
        for needle in ("zos_proc_gate_sp", "zos_proc_gate_latch",
                       "zos_proc_gate_iff"):
            self.assertIn("shadowed gate caller", self.text)
            self.assertTrue(needle in self.text)
        self.assertIn(".globl zos_gate_saved_sp", self.text)
        self.assertIn("ld\t(zos_gate_saved_sp),de", self.text)

    def test_resume_window_is_stackless(self):
        # Between the start of the LDIR and the SP restore the primitive
        # must not CALL, PUSH, POP or enable interrupts: the frame it is
        # restoring is the one it is running on.
        body = self.text.split("_zos_process_resume::", 1)[1]
        window = _code(body.split("ldir", 1)[1].split("ld\tsp,ix", 1)[0])
        for forbidden in ("push", "pop", "call", "ei"):
            self.assertNotRegex(
                window, r"\b%s\b" % forbidden,
                "the resume window between LDIR and 'ld sp,ix' uses %s"
                % forbidden)

    def test_continuation_size_returns_in_de(self):
        body = self.text.split("_zos_process_continuation_size::", 1)[1]
        body = body.split(".area", 1)[0]
        self.assertIn("ex\tde,hl\t\t\t; SDCC 16-bit return is in DE", body)


class TestPrepareThenLaunch(unittest.TestCase):
    def test_process_launch_is_infallible(self):
        body = _read(PROCESS_C).split("void process_launch(", 1)[1]
        body = body.split("\n}\n", 1)[0]
        self.assertNotRegex(body, r"\breturn\s+-",
                            "process_launch() returns an error")
        self.assertNotRegex(body, r"if\s*\(.*ZOS_E",
                            "process_launch() carries a failing check")
        self.assertIn("zos_process_launch(&req)", body)

    def test_prepare_is_split_out_and_closes_the_source(self):
        text = _read(EXEC_C)
        self.assertIn("int process_prepare(const char *path, uint8_t parent_pid",
                      text)
        self.assertIn("struct process_child *child", text)
        body = text.split("int process_prepare(", 1)[1]
        self.assertRegex(body, r"vfs_close\(&file\)",
                         "process_prepare() does not close the loader source")
        # The close happens before the child is handed on.
        self.assertLess(body.index("vfs_close(&file)"),
                        body.index("child->bank ="),
                        "process_prepare() hands on the child before "
                        "closing the executable source")

    def test_sys_exec_preflights_before_allocating(self):
        body = _read(SYSCALL_C).split("static void syscall_exec(", 1)[1]
        body = body.split("\n}\n", 1)[0]
        depth = body.index("ZOS_EXEC_MAX_DEPTH")
        frame = body.index("zos_process_continuation_size()")
        prepare = body.index("process_prepare(")
        self.assertLess(depth, prepare,
                        "the depth check runs after allocation")
        self.assertLess(frame, prepare,
                        "the frame check runs after allocation")
        self.assertIn("ZOS_EXEC_FRAME_SAVE - ZOS_EXEC_FRAME_SLACK", body)


class TestMapParity(unittest.TestCase):
    def test_mapcheck_pins_the_geometry(self):
        text = _read(MAPCHECK)
        self.assertRegex(text, r"ZOS_EXEC_MAX_DEPTH = 4\b")
        self.assertRegex(text, r"ZOS_EXEC_FRAME_SAVE = \d+")
        for name in ("zos_proc_cont", "zos_proc_ix", "zos_proc_latch",
                     "zos_proc_iff", "zos_proc_gate_sp",
                     "zos_proc_gate_latch", "zos_proc_gate_iff",
                     "zos_proc_frames", "zos_proc_frames_end"):
            self.assertIn('"%s"' % name, text,
                          "mapcheck does not pin %s" % name)
        # The path buffer is pinned as bank-7 runtime data instead: it
        # must not be listed as common state.
        common_list = text.split("M6_COMMON_STATE = (", 1)[1].split(")", 1)[0]
        self.assertNotIn("_zos_process_path_scratch", common_list,
                         "mapcheck still claims the path buffer is common")
        self.assertIn('symbols.get("_zos_process_path_scratch")', text)
        self.assertIn("is not in the bank-7", text)
        self.assertIn("runtime data region", text)

    def test_mapcheck_constants_match_the_header(self):
        header = _read(PROCESS_H)
        text = _read(MAPCHECK)
        for name in ("ZOS_EXEC_MAX_DEPTH", "ZOS_EXEC_FRAME_SAVE"):
            self.assertEqual(_constant(text, name), _constant(header, name),
                             "mapcheck %s disagrees with process.h" % name)

    def test_synthetic_map_carries_the_frame_delta(self):
        text = _read(MAPTOOLS)
        frames = re.search(r'"zos_proc_frames": (0x[0-9A-Fa-f]+)',
                           text)
        end = re.search(r'"zos_proc_frames_end": (0x[0-9A-Fa-f]+)', text)
        self.assertIsNotNone(frames)
        self.assertIsNotNone(end)
        delta = int(end.group(1), 0) - int(frames.group(1), 0)
        expected = (_constant(_read(PROCESS_H), "ZOS_EXEC_FRAME_SAVE") *
                    (_constant(_read(PROCESS_H), "ZOS_EXEC_MAX_DEPTH") - 1))
        self.assertEqual(delta, expected,
                         "the synthetic map's frame area is %d bytes, "
                         "expected %d" % (delta, expected))


class TestProductionImage(unittest.TestCase):
    def test_production_map_has_no_acceptance_symbols(self):
        map_path = ROOT / "build" / "zephyros.map"
        if not map_path.exists():
            self.skipTest("build/zephyros.map missing; run make first")
        text = _read(map_path)
        for name in PRODUCTION_FORBIDDEN:
            self.assertNotRegex(
                text, r"\b%s\b" % re.escape(name),
                "the production map carries the acceptance symbol %s"
                % name)

    def test_production_map_keeps_the_continuation_state(self):
        map_path = ROOT / "build" / "zephyros.map"
        if not map_path.exists():
            self.skipTest("build/zephyros.map missing; run make first")
        text = _read(map_path)
        for name in ("_zos_process_launch", "_zos_process_resume",
                     "_zos_process_continuation_size", "zos_proc_cont",
                     "zos_proc_frames", "zos_proc_gate_sp"):
            self.assertIn(name, text,
                          "the production map lacks %s" % name)


class TestBuildWiring(unittest.TestCase):
    def test_makefile_has_the_acceptance_image(self):
        text = _read(MAKEFILE)
        self.assertIn("m9-exec-selftest:", text)
        self.assertIn("ZOS_M9_EXEC_SELFTEST=1", text)
        self.assertIn("PK_SUFFIX := -m9-exec-selftest", text)
        self.assertIn("build-m9-exec-selftest/zephyros-m9-exec-selftest.bin",
                      text)

    def test_makefile_builds_and_packs_the_children(self):
        text = _read(MAKEFILE)
        self.assertIn("M9EXEC_DIR := tests/target/m9/exec", text)
        for name in ("fschild", "fsdeep", "fsraw"):
            self.assertIn("$(MODULES)/%s.zex:" % name, text,
                          "the Makefile does not build %s.zex" % name)
            self.assertIn("$(M9EXEC_DIR)/%s.c" % name, text)
            self.assertIn("$(M9EXEC_DIR)/%s.json" % name, text)
            self.assertIn("$(M9EXEC_DIR)/%s_entry.asm" % name, text)
        self.assertIn("tests/m9/manifest-exec.json", text)
        self.assertIn("user/runtime/zex_exec_runtime.h", text)

    def test_acceptance_manifest_carries_the_programs(self):
        data = json.loads(_read(MANIFEST_EXEC))
        names = {entry["name"] for entry in data["files"]}
        for name in ("fstest.zex", "fschild.zex", "fsdeep.zex",
                     "fsraw.zex", "ramfs.zmod"):
            self.assertIn(name, names,
                          "manifest-exec.json lacks %s" % name)

    def test_serial_smoke_has_the_mode(self):
        text = _read(SERIAL_SMOKE)
        self.assertIn('"m9-exec"', text)
        self.assertIn("m9 exec selftest: OK", text)

    def test_target_test_seam_selects_the_image(self):
        text = _read(TARGET_TEST)
        self.assertIn("ZOS_M9_EXEC_SELFTEST", text)
        self.assertIn("zos_run_m9_exec_selftest", text)

    def test_ledger_format_is_shared_by_both_tools(self):
        pack = _read(PACK_IMAGE)
        zmod = _read(ZMODPACK)
        self.assertIn("BANK7_LEDGER_LINE", pack)
        self.assertIn("COMMON_LEDGER_LINE", pack)
        self.assertIn("pack_image.COMMON_LEDGER_LINE", zmod)
        self.assertIn("common: used %d of %d bytes (code %d, data %d)",
                      pack)

    def test_serial_smoke_banner_contract(self):
        text = _read(SERIAL_SMOKE)
        self.assertIn("--quiet", text)
        self.assertIn("banner-only PASS", text)
        self.assertIn("console: unbound", text)
        self.assertIn("unavailable", text)


if __name__ == "__main__":
    unittest.main()
