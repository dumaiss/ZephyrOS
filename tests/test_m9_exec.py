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
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

SYSCALL_H = ROOT / "abi" / "zos" / "syscall.h"
PROCESS_H = ROOT / "include" / "zephyros" / "process.h"
PROCESS_ABI_H = ROOT / "abi" / "zos" / "process.h"
VFS_H = ROOT / "kernel" / "vfs" / "vfs.h"
LAUNCH_ASM = ROOT / "kernel" / "process" / "launch.asm"
PROCESS_C = ROOT / "kernel" / "process" / "process.c"
EXEC_C = ROOT / "kernel" / "process" / "exec.c"
SYSCALL_C = ROOT / "kernel" / "syscall.c"
SDK_PROCESS_H = ROOT / "user" / "sdk" / "include" / "zos" / "process.h"
SDK_EXEC_C = ROOT / "user" / "sdk" / "src" / "exec.c"
SDK_INTERNAL_H = ROOT / "user" / "sdk" / "src" / "zos_internal.h"
SDK_ARGS_C = ROOT / "user" / "sdk" / "src" / "args.c"
SDK_CRT0_ASM = ROOT / "user" / "sdk" / "src" / "crt0.asm"
MAIN_C = ROOT / "kernel" / "main.c"
CONFIG_MK = ROOT / "config" / "system.mk"
BOOTPROGRAM = ROOT / "tools" / "bootprogram.py"
ZEX_DOC = ROOT / "docs" / "zex-format.md"
FSTEST_C = ROOT / "tests" / "target" / "m9" / "fstest" / "fstest.c"
FSCHILD_C = ROOT / "tests" / "target" / "m9" / "exec" / "fschild.c"
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
        self.assertIn("sizeof(struct zos_exec_args) == 8", text)
        for field, offset in (("path", 0), ("length", 2), ("args", 4),
                              ("args_length", 6)):
            self.assertIn(
                "__builtin_offsetof(struct zos_exec_args, %s) == %d"
                % (field, offset), text,
                "zos_exec_args.%s offset is not pinned" % field)
        # The startup block's geometry is pinned against the page-zero
        # window: 0080h + version + length + tail + NUL == 0100h.
        abi = _read(PROCESS_ABI_H)
        self.assertIn("ZOS_STARTUP_BASE     0x0080u", abi)
        self.assertIn("ZOS_STARTUP_VERSION  1u", abi)
        self.assertIn("ZOS_STARTUP_TAIL_MAX 125u", abi)
        self.assertIn(
            "ZOS_STARTUP_BASE + 2u + ZOS_STARTUP_TAIL_MAX + 1u == 0x0100u",
            abi)
        # The SDK marshals the block through its shared scratch union arm
        # (the SYS_EXEC block is 8 bytes, unlike the 4-byte FS path block).
        self.assertIn("struct zos_exec_args    exec;",
                      _read(SDK_INTERNAL_H))
        self.assertIn("zos_scratch.exec.args_length", _read(SDK_EXEC_C))

    def test_dispatcher_routes_exec(self):
        self.assertIn("case ZOS_SYSCALL_EXEC:", _read(SYSCALL_C))
        self.assertIn("syscall_exec", _read(SYSCALL_C))

    def test_user_sdk_wrapper(self):
        header = _read(SDK_PROCESS_H)
        self.assertIn("int zos_exec(const char *path, const char *args);",
                      header)
        impl = _read(SDK_EXEC_C)
        self.assertIn("int zos_exec(const char *path, const char *args)",
                      impl)
        # The SDK uses the ABI number directly and calls it through the
        # shared FS gate stub; there is no mirror.
        self.assertIn("ZOS_SYSCALL_EXEC", impl)
        self.assertIn("zos_fs_call", impl)
        # A null/empty tail is legal; an over-long tail is refused locally
        # before the syscall, like the path length.
        self.assertIn("zexec_tail_len", impl)
        self.assertIn("an == 0xFFFFu", impl)

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
        self.assertEqual(_constant(_read(PROCESS_ABI_H), "ZOS_EXEC_MAX_DEPTH"), 4)
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
        args = body.index("ZOS_STARTUP_TAIL_MAX")
        window = body.index("zos_syscall_buffer_ok(args, args_length)")
        prepare = body.index("process_prepare(")
        self.assertLess(depth, prepare,
                        "the depth check runs after allocation")
        self.assertLess(frame, prepare,
                        "the frame check runs after allocation")
        self.assertLess(args, prepare,
                        "the argument-length check runs after allocation")
        self.assertLess(window, prepare,
                        "the argument-window check runs after allocation")
        self.assertIn("ZOS_EXEC_FRAME_SAVE - ZOS_EXEC_FRAME_SLACK", body)
        # The block grew to 8 bytes and carries args/args_length.
        self.assertIn("block_ok(block, 8)", body)
        self.assertIn("block + 4", body)
        self.assertIn("block + 6", body)


class TestStartupBlock(unittest.TestCase):
    def test_launch_gate_enters_with_the_block_pointer(self):
        text = _read(LAUNCH_ASM)
        self.assertIn("ld\thl,#0x0080", text)
        self.assertNotIn("ld\thl,#0x0000", text,
                         "the launch gate still enters with a null HL")

    def test_prepare_builds_the_block_after_closing_the_source(self):
        text = _read(EXEC_C)
        body = text.split("int process_prepare(", 1)[1]
        self.assertLess(
            body.index("vfs_close(&file)"),
            body.index("exec_startup_block("),
            "the startup block is built before the source is closed")
        # The tail is read from the parent bank through the checked gate
        # into the existing 128-byte common staging view (free once the
        # source is closed) and written to the child page zero.
        for needle in ("ZOS_STARTUP_VERSION", "ZOS_STARTUP_BASE",
                       "rom_access_staging", "zos_bank_copy",
                       "ZOS_BANK_OP_READ", "ZOS_BANK_OP_WRITE"):
            self.assertIn(needle, text, "exec.c lacks %s" % needle)
        # It never stores the tail: no new static buffer or data symbol.
        self.assertNotIn("zos_startup_scratch", text)
        self.assertNotRegex(text, r"^\s*(static\s+)?(char|uint8_t)\s+\w+\[",
                            "exec.c declares a new array")

    def test_prepare_takes_the_argument_tail(self):
        header = _read(PROCESS_H)
        self.assertIn("int process_prepare(const char *path, uint8_t parent_pid, "
                      "uint16_t args,", header)
        self.assertIn("uint16_t args_length, struct process_child *child);",
                      header)
        # The kernel launcher passes an empty tail.
        self.assertIn("process_prepare(path, PROCESS_KERNEL_PARENT, 0, 0,",
                      _read(PROCESS_C))

    def test_launch_and_prepare_keep_zero_new_data(self):
        # The startup block is transported through existing common/bank-7
        # buffers; the only new object the runtime adds is the optional
        # 2-byte user-side accessor, never kernel data.
        text = _read(EXEC_C)
        self.assertNotRegex(text, r"^\s*(static\s+)?(char|uint8_t)\s+\w+\[",
                            "exec.c declares a new array")


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
        abi = _read(PROCESS_ABI_H)
        text = _read(MAPCHECK)
        for name, source in (("ZOS_EXEC_MAX_DEPTH", abi),
                             ("ZOS_EXEC_FRAME_SAVE", header)):
            self.assertEqual(_constant(text, name), _constant(source, name),
                             "mapcheck %s disagrees with the process headers"
                             % name)

    def test_synthetic_map_carries_the_frame_delta(self):
        text = _read(MAPTOOLS)
        frames = re.search(r'"zos_proc_frames": (0x[0-9A-Fa-f]+)',
                           text)
        end = re.search(r'"zos_proc_frames_end": (0x[0-9A-Fa-f]+)', text)
        self.assertIsNotNone(frames)
        self.assertIsNotNone(end)
        delta = int(end.group(1), 0) - int(frames.group(1), 0)
        expected = (_constant(_read(PROCESS_H), "ZOS_EXEC_FRAME_SAVE") *
                    (_constant(_read(PROCESS_ABI_H), "ZOS_EXEC_MAX_DEPTH") - 1))
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


class TestBootProgram(unittest.TestCase):
    """The ZOS_BOOT_PROGRAM build setting (package 28b) is validated by
    tools/bootprogram.py against the selected recovery manifest."""

    def _run(self, program, manifest):
        return subprocess.run(
            [sys.executable, str(BOOTPROGRAM), "check", program, str(manifest)],
            capture_output=True, text=True)

    def test_production_setting_is_valid(self):
        result = self._run("/SYSTEM/monitor.zex", ROOT / "recovery" /
                           "manifest.json")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_acceptance_setting_is_valid(self):
        result = self._run("/SYSTEM/fstest.zex", ROOT / "tests" / "m9" /
                           "manifest-exec.json")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_a_non_system_path(self):
        for program in ("monitor.zex", "/DATA/monitor.zex",
                        "/SYSTEM", "/SYSTEM/sub/monitor.zex"):
            result = self._run(program, ROOT / "recovery" / "manifest.json")
            self.assertNotEqual(result.returncode, 0,
                                "%r was accepted" % program)
            self.assertIn("boot program:", result.stderr)

    def test_rejects_an_over_long_path(self):
        program = "/SYSTEM/" + "a" * 121 + ".zex"  # 129 bytes
        result = self._run(program, ROOT / "recovery" / "manifest.json")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("over the 127-byte limit", result.stderr)

    def test_rejects_a_name_absent_from_the_manifest(self):
        result = self._run("/SYSTEM/absent.zex", ROOT / "recovery" /
                           "manifest.json")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not in the selected recovery manifest", result.stderr)

    def test_rejects_a_non_zex_manifest_entry(self):
        # sio.zmod is present but is not a ZEX.
        result = self._run("/SYSTEM/sio.zmod", ROOT / "recovery" /
                           "manifest.json")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not a ZEX", result.stderr)

    def test_build_wires_the_setting(self):
        config = _read(CONFIG_MK)
        self.assertIn("ZOS_BOOT_PROGRAM ?= /SYSTEM/monitor.zex", config)
        makefile = _read(MAKEFILE)
        self.assertIn("include config/system.mk", makefile)
        self.assertIn("tools/bootprogram.py check", makefile)
        self.assertIn("boot_program.stamp", makefile)
        self.assertRegex(makefile,
                         r"-DZOS_BOOT_PROGRAM='\"\$\(ZOS_BOOT_PROGRAM\)\"'")
        # The kernel takes exactly one path from that constant.
        main = _read(MAIN_C)
        self.assertIn("static const char kernel_boot_program[] = "
                      "ZOS_BOOT_PROGRAM;", main)
        self.assertNotIn('"/SYSTEM/monitor.zex"', main,
                         "kernel/main.c still hard-codes the boot path")

    def test_launcher_is_renamed_and_reports_the_configured_path(self):
        main = _read(MAIN_C)
        self.assertIn("static void kernel_boot_launcher(void)", main)
        self.assertNotIn("kernel_monitor_launcher", main)
        self.assertIn("process_exec(kernel_boot_program, &status)", main)
        # The launch-failure diagnostic names the configured program.
        self.assertIn('early_console_puts(": launch failed 0x")', main)


class TestArgsRuntime(unittest.TestCase):
    """The SDK startup glue and splitter (runtime policy)."""

    def test_crt0_saves_the_block_and_calls_main(self):
        text = _read(SDK_CRT0_ASM)
        self.assertIn("_zos_startup_block", text)
        self.assertIn("call\t_zos_split", text)
        self.assertIn("call\t_main", text)
        self.assertIn("call\t_zex_exit", text)

    def test_accessor_and_splitter_surface(self):
        header = _read(SDK_PROCESS_H)
        for needle in ("#define ZOS_ARGV_MAX 16",
                       "struct zos_startup *zos_args(void);",
                       "int zos_split(char *tail, char **argv, int max);"):
            self.assertIn(needle, header, "zos/process.h lacks %r" % needle)
        abi = _read(PROCESS_ABI_H)
        for needle in ("ZOS_STARTUP_BASE     0x0080u",
                       "ZOS_STARTUP_VERSION  1u",
                       "ZOS_STARTUP_TAIL_MAX 125u",
                       "struct zos_startup {"):
            self.assertIn(needle, abi, "abi/zos/process.h lacks %r" % needle)
        impl = _read(SDK_ARGS_C)
        self.assertIn("int zos_split(char *tail, char **argv, int max)", impl)

    def test_splitter_rules_are_documented(self):
        doc = _read(ZEX_DOC)
        self.assertIn("Startup block", doc)
        self.assertRegex(doc, r"double quote")
        self.assertRegex(doc, r"whitespace")
        self.assertIn("0080h", doc)

    def test_acceptance_programs_use_the_sdk(self):
        makefile = _read(MAKEFILE)
        for needle in ("zex_program,fstest,tests/target/m9/fstest,fstest.c",
                       "zex_program,fschild,tests/target/m9/exec,fschild.c"):
            self.assertIn(needle, makefile, "the Makefile lacks %r" % needle)
        # fstest checks the kernel-launched block and drives the cases.
        fstest = _read(FSTEST_C)
        for needle in ("zos_args()", "ZOS_STARTUP_VERSION",
                       '"two words \\"a b\\" tail"',
                       "ZOS_STARTUP_TAIL_MAX + 1u", "0x0000u"):
            self.assertIn(needle, fstest, "fstest.c lacks %r" % needle)
        # fschild takes its argv from crt0's main(), not a private splitter.
        fschild = _read(FSCHILD_C)
        self.assertIn("int main(int argc, char **argv)", fschild)
        self.assertIn("argv[i]", fschild)


class TestBuildWiring(unittest.TestCase):
    def test_makefile_has_the_acceptance_image(self):
        text = _read(MAKEFILE)
        self.assertIn("m9-exec-selftest:", text)
        self.assertIn("ZOS_M9_EXEC_SELFTEST=1", text)
        self.assertIn("PK_SUFFIX := -m9-exec-selftest", text)
        self.assertIn("build-m9-exec-selftest/zephyros-m9-exec-selftest.bin",
                      text)

    def test_makefile_builds_the_children_through_the_sdk(self):
        text = _read(MAKEFILE)
        self.assertIn("include user/sdk/zex.mk", text)
        for needle in (
                "zex_program,fschild,tests/target/m9/exec,fschild.c",
                "zex_program,fsdeep,tests/target/m9/exec,fsdeep.c",
                "zex_program,fsraw,tests/target/m9/exec,"
                "fsraw.c fsraw_entry.asm",
                "NO_CRT0=1,ENTRY=rw_entry"):
            self.assertIn(needle, text, "the Makefile lacks %r" % needle)
        self.assertIn("tests/m9/manifest-exec.json", text)

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
