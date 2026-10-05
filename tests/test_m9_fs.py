"""Host tests for the M9 filesystem syscall package (27).

Covers:
  - the pinned `FS_*` syscall numbers (10h-1Fh) in the ABI header, the
    kernel dispatcher, the user runtime C wrapper and the runtime asm stub;
  - the eight M9 errno values;
  - the packed argument-block sizes/offsets declared in
    abi/zos/fs.h and mirrored by the runtime union;
  - the user-runtime wrapper surface and its local path-length guard;
  - the build wiring (m9-fs-selftest image, ROMFS acceptance manifest, the
    fstest program group, the serial smoke mode).
"""

import os
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

SYSCALL_H = ROOT / "abi" / "zos" / "syscall.h"
ERRNO_H = ROOT / "abi" / "zos" / "errno.h"
FS_H = ROOT / "abi" / "zos" / "fs.h"
KERNEL_FS = ROOT / "kernel" / "syscall" / "fs.c"
RUNTIME_H = ROOT / "user" / "sdk" / "include" / "zos" / "fs.h"
RUNTIME_C = ROOT / "user" / "sdk" / "src" / "fs.c"
RUNTIME_INTERNAL = ROOT / "user" / "sdk" / "src" / "zos_internal.h"
GATE_ASM = ROOT / "user" / "sdk" / "src" / "fsgate.asm"
VFS_H = ROOT / "kernel" / "vfs" / "vfs.h"
PROCESS_H = ROOT / "abi" / "zos" / "process.h"
VFS_PROCESS = ROOT / "kernel" / "vfs" / "process.c"
MAKEFILE = ROOT / "Makefile"
MANIFEST_FS = ROOT / "tests" / "m9" / "manifest-fs.json"
SERIAL_SMOKE = ROOT / "tools" / "serial_smoke.py"

FS_NUMBERS = {
    "ZOS_SYSCALL_FS_OPEN": 0x10,
    "ZOS_SYSCALL_FS_CLOSE": 0x11,
    "ZOS_SYSCALL_FS_READ": 0x12,
    "ZOS_SYSCALL_FS_WRITE": 0x13,
    "ZOS_SYSCALL_FS_SEEK": 0x14,
    "ZOS_SYSCALL_FS_STAT": 0x15,
    "ZOS_SYSCALL_FS_OPENDIR": 0x16,
    "ZOS_SYSCALL_FS_READDIR": 0x17,
    "ZOS_SYSCALL_FS_CLOSEDIR": 0x18,
    "ZOS_SYSCALL_FS_MKDIR": 0x19,
    "ZOS_SYSCALL_FS_RMDIR": 0x1A,
    "ZOS_SYSCALL_FS_UNLINK": 0x1B,
    "ZOS_SYSCALL_FS_RENAME": 0x1C,
    "ZOS_SYSCALL_FS_CHDIR": 0x1D,
    "ZOS_SYSCALL_FS_GETCWD": 0x1E,
    "ZOS_SYSCALL_FS_SYNC": 0x1F,
}

M9_ERRNOS = {
    "ZOS_EEXIST": 0x10,
    "ZOS_ENOTDIR": 0x11,
    "ZOS_EISDIR": 0x12,
    "ZOS_ENOTEMPTY": 0x13,
    "ZOS_EROFS": 0x14,
    "ZOS_ESTALE": 0x15,
    "ZOS_EXDEV": 0x16,
    "ZOS_EMFILE": 0x17,
}

# size -> (name, [(field, offset), ...]) from work/M9-REFERENCE.md.
ARG_BLOCKS = {
    "zfs_path_args": (4, [("path", 0), ("length", 2)]),
    "zfs_open_args": (5, [("path", 0), ("length", 2), ("flags", 4)]),
    "zfs_io_args": (5, [("fd", 0), ("buffer", 1), ("length", 3)]),
    "zfs_seek_args": (6, [("fd", 0), ("whence", 1), ("offset", 2)]),
    "zfs_stat_args": (6, [("path", 0), ("length", 2), ("out", 4)]),
    "zfs_readdir_args": (3, [("fd", 0), ("out", 1)]),
    "zfs_getcwd_args": (4, [("buffer", 0), ("length", 2)]),
    "zfs_rename_args": (8, [("from", 0), ("from_length", 2),
                            ("to", 4), ("to_length", 6)]),
    "zfs_fd_args": (1, [("fd", 0)]),
}


def _read(path):
    return Path(path).read_text()


class TestFsSyscallNumbers(unittest.TestCase):
    def test_abi_header_pins_the_family(self):
        text = _read(SYSCALL_H)
        for name, value in FS_NUMBERS.items():
            self.assertRegex(
                text, r"#define %s\s+0x%02Xu" % (name, value),
                "%s is not pinned to 0x%02X" % (name, value))

    def test_kernel_dispatcher_routes_the_family(self):
        text = _read(KERNEL_FS)
        for name in FS_NUMBERS:
            self.assertIn("case %s:" % name, text,
                          "kernel fs.c does not dispatch %s" % name)

    def test_sdk_gate_is_a_single_stub(self):
        text = _read(GATE_ASM)
        self.assertIn("_zos_fs_call::", text)
        self.assertIn("rst	0x30", text)

    def test_sdk_wrappers_use_the_abi_numbers(self):
        # The SDK has no mirror: fs.c uses the ABI symbols directly.
        text = _read(RUNTIME_C)
        for name in FS_NUMBERS:
            self.assertIn(name, text,
                          "SDK fs.c does not use %s" % name)


class TestM9Errnos(unittest.TestCase):
    def test_errors_are_pinned(self):
        text = _read(ERRNO_H)
        for name, value in M9_ERRNOS.items():
            self.assertRegex(text, r"#define %s\s+0x%02X" % (name, value),
                             "%s is not pinned to 0x%02X" % (name, value))


class TestFsArgumentBlocks(unittest.TestCase):
    def test_sizes_and_offsets_are_pinned_in_the_header(self):
        text = _read(FS_H)
        for name, (size, fields) in ARG_BLOCKS.items():
            self.assertIn("sizeof(struct %s) == %d" % (name, size), text,
                          "%s size is not pinned" % name)
            for field, offset in fields:
                self.assertIn(
                    "__builtin_offsetof(struct %s, %s) == %d"
                    % (name, field, offset), text,
                    "%s.%s offset is not pinned" % (name, field))

    def test_sdk_union_covers_every_block(self):
        text = _read(RUNTIME_INTERNAL)
        for name in ARG_BLOCKS:
            self.assertIn("struct %s" % name, text,
                          "the SDK union has no %s arm" % name)


class TestUserRuntime(unittest.TestCase):
    def test_wrapper_surface(self):
        text = _read(RUNTIME_H)
        for name in ("zos_open", "zos_close", "zos_read", "zos_write",
                     "zos_seek", "zos_stat", "zos_opendir", "zos_readdir",
                     "zos_closedir", "zos_mkdir", "zos_rmdir", "zos_unlink",
                     "zos_rename", "zos_chdir", "zos_getcwd", "zos_sync"):
            self.assertRegex(text, r"\b%s\(" % name,
                             "%s is not declared" % name)

    def test_local_path_guard(self):
        impl = _read(RUNTIME_C)
        self.assertIn("ZOS_PATH_MAX - 1u", impl)
        self.assertIn("return -ZOS_EINVAL;", impl)


class TestDescriptorPoolShape(unittest.TestCase):
    def test_vfs_header_pins_pool_geometry(self):
        text = _read(VFS_H)
        for needle in ("#define VFS_FD_MAX 8u",
                       "#define VFS_DESC_FREE      0u",
                       "#define VFS_DESC_FILE      1u",
                       "#define VFS_DESC_DIRECTORY 2u",
                       "sizeof(struct vfs_descriptor) == 17"):
            self.assertIn(needle, text)
        # The nesting depth the CWD pool is sized from is owned by
        # include/zephyros/process.h (package 28) and only reached through
        # the VFS header's include; the VFS header says so.
        self.assertIn("#define ZOS_EXEC_MAX_DEPTH 4u", _read(PROCESS_H))
        self.assertNotIn("#define ZOS_EXEC_MAX_DEPTH", text)
        self.assertIn("ZOS_EXEC_MAX_DEPTH is owned by", text)

    def test_process_exit_closes_and_is_idempotent(self):
        text = _read(VFS_PROCESS)
        self.assertIn("void vfs_process_exit(uint8_t pid)", text)
        self.assertIn("vfs_fd_close(fd)", text)


class TestBuildWiring(unittest.TestCase):
    def test_makefile_has_the_acceptance_image(self):
        text = _read(MAKEFILE)
        self.assertIn("m9-fs-selftest:", text)
        self.assertIn("ZOS_M9_FS_SELFTEST=1", text)
        self.assertIn("PK_SUFFIX := -m9-fs-selftest", text)
        self.assertIn("build-m9-fs-selftest/zephyros-m9-fs-selftest.bin",
                      text)

    def test_makefile_builds_and_packs_the_programs(self):
        text = _read(MAKEFILE)
        for needle in ("include user/sdk/zex.mk",
                       "zex_program,fstest,tests/target/m9/fstest,fstest.c",
                       "zex_program,fstest_l,tests/target/m9/fstest,fstest_l.c"):
            self.assertIn(needle, text, "Makefile lacks %s" % needle)

    def test_acceptance_manifest_carries_the_programs(self):
        import json
        data = json.loads(_read(MANIFEST_FS))
        names = {entry["name"] for entry in data["files"]}
        self.assertIn("fstest.zex", names)
        self.assertIn("fstest_l.zex", names)
        self.assertIn("ramfs.zmod", names)
        self.assertIn("monitor.zex", names)

    def test_serial_smoke_has_the_mode(self):
        text = _read(SERIAL_SMOKE)
        self.assertIn('"m9-fs"', text)
        self.assertIn("m9 fs selftest: OK", text)

    def test_target_test_seam_selects_the_image(self):
        text = _read(ROOT / "tests" / "target" / "support" / "target_test.c")
        self.assertIn("ZOS_M9_FS_SELFTEST", text)
        self.assertIn("zos_run_m9_fs_selftest", text)


class TestVfsWorkspace(unittest.TestCase):
    """One bank-7 VFS workspace replaces the six pathname buffers and the
    staging buffer (work/27-m9-fs-syscalls/correction-01), so production
    `_DATA` drops by at least 900 bytes."""

    DELETED = ("fs_staging", "fs_path1", "fs_path2", "vfs_cwd_canonical",
               "vfs_norm", "vfs_rename_target", "kernel_boot_path",
               "vfs_required_diag")

    # The package-27 production `_DATA` size the correction must beat by
    # 900 bytes (map `_DATA` area, 2026-10-05).
    DATA_BASELINE = 3047
    DATA_DROP_MIN = 900

    # Package 28 deliberately moved the 128-byte SYS_EXEC path buffer from
    # common memory into bank-7 `_DATA` (work/28-m9-nested-exec/
    # correction-01), so the ceiling carries exactly that allowance: the
    # correction's 900-byte win must still hold for everything else.
    EXEC_PATH_BUFFER = 128

    # A second pathname-sized scratch buffer would show up as its own global;
    # the normalize helpers are functions and must not match these patterns.
    PATHNAME_SHAPED = (r"path[0-9]", r"staging", r"canonical",
                       r"required_diag", r"rename_target", r"boot_path")

    def test_guarded_views_are_wired_into_debug_and_acceptance_builds(self):
        header = _read(VFS_H)
        self.assertIn("#define ZOS_VFS_WS_GUARD 0", header,
                      "the guard must default off so production pays nothing")
        self.assertRegex(
            header, r"if \(vfs_ws_view != 0u && vfs_ws_view != \(view\)\)",
            "the guard macro must detect an overlapping view claim")
        self.assertIn("vfs_ws_fault()", header)

        workspace = _read(ROOT / "kernel" / "vfs" / "workspace.c")
        self.assertIn("#if ZOS_VFS_WS_GUARD", workspace)
        self.assertIn("vfs_ws_fault", workspace)

        # The m9-fs-selftest image is the one whose simulator replay asserts
        # "no PANIC", so the guard has to be on for it.
        makefile = _read(ROOT / "Makefile")
        match = re.search(
            r"ifneq \(\$\(filter 1,([\s\S]*?)\),\)\nCFLAGS\s*\+="
            r" -DZOS_TARGET_TEST_BUILD=1 -DZOS_VFS_WS_GUARD=1", makefile)
        self.assertIsNotNone(
            match, "no acceptance block enables -DZOS_VFS_WS_GUARD=1")
        self.assertIn("$(ZOS_M9_FS_SELFTEST)", match.group(1))

    def test_view_claims_are_balanced(self):
        for rel in ("kernel/syscall/fs.c", "kernel/main.c"):
            text = _read(ROOT / rel)
            self.assertEqual(text.count("vfs_ws_enter("),
                             text.count("vfs_ws_leave("),
                             "%s claims a view without releasing it" % rel)
            self.assertIn("vfs_ws_enter(", text)

    def test_boot_loader_claims_the_path_view(self):
        text = _read(ROOT / "kernel" / "main.c")
        self.assertIn("vfs_ws_enter(VFS_WS_VIEW_PATH)", text)
        self.assertIn("vfs_ws_leave(VFS_WS_VIEW_PATH)", text)

    def test_header_declares_one_workspace_with_two_views(self):
        text = _read(VFS_H)
        self.assertIn("#define VFS_WORKSPACE_SIZE 512u", text)
        self.assertIn("#define VFS_PATH_SLOT_SIZE 128u", text)
        self.assertIn("#define VFS_PATH_SLOTS     4u", text)
        self.assertIn("extern uint8_t vfs_workspace[VFS_WORKSPACE_SIZE];",
                      text)
        for view in ("VFS_WS_STAGING", "VFS_WS_IN_A", "VFS_WS_NORM_A",
                     "VFS_WS_IN_B", "VFS_WS_NORM_B"):
            self.assertIn(view, text, "the workspace lacks %s" % view)
        self.assertIn("vfs_ws_enter", text)
        self.assertIn("vfs_ws_leave", text)

    def test_deleted_buffers_are_gone_from_the_kernel(self):
        for rel in ("kernel/syscall/fs.c", "kernel/vfs/mount.c",
                    "kernel/vfs/mutate.c", "kernel/vfs/process.c",
                    "kernel/vfs/file.c", "kernel/main.c"):
            text = _read(ROOT / rel)
            for name in self.DELETED:
                self.assertNotRegex(
                    text, r"\b%s\b" % name,
                    "%s still names the deleted buffer %s" % (rel, name))

    def test_production_map_has_the_single_workspace_symbol(self):
        map_path = ROOT / "build" / "zephyros.map"
        if not map_path.exists():
            self.skipTest("build/zephyros.map missing; run make first")
        text = _read(map_path)
        self.assertEqual(len(re.findall(r"\b_vfs_workspace\b", text)), 1,
                         "the production map must carry exactly one "
                         "vfs_workspace symbol")
        for pattern in self.PATHNAME_SHAPED:
            for name in re.findall(r"^_([A-Za-z0-9_]*%s[A-Za-z0-9_]*)"
                                   % pattern, text, re.M):
                self.fail("the production map still carries the pathname "
                          "buffer _%s" % name)
        # The deleted buffers were file-local statics, so they never appeared
        # in the global map; assert no global symbol re-introduces one.
        for name in self.DELETED:
            self.assertNotRegex(text, r"\b_%s\b" % name,
                                "the production map carries _%s" % name)
        match = re.search(
            r"^_DATA\s+[0-9A-Fa-f]{8}\s+[0-9A-Fa-f]{8}\s+=\s+(\d+)\.",
            text, re.M)
        self.assertIsNotNone(match, "no _DATA area in the production map")
        size = int(match.group(1))
        ceiling = (self.DATA_BASELINE - self.DATA_DROP_MIN
                   + self.EXEC_PATH_BUFFER)
        self.assertLessEqual(
            size, ceiling,
            "production _DATA is %d bytes; the workspace consolidation must "
            "still drop it by at least %d from %d (allowing the %d-byte "
            "SYS_EXEC path buffer package 28 moved in from common memory)"
            % (size, self.DATA_DROP_MIN, self.DATA_BASELINE,
               self.EXEC_PATH_BUFFER))


if __name__ == "__main__":
    unittest.main()
