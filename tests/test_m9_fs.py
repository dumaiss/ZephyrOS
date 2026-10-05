"""Host tests for the M9 filesystem syscall package (27).

Covers:
  - the pinned `FS_*` syscall numbers (10h-1Fh) in the ABI header, the
    kernel dispatcher, the user runtime C wrapper and the runtime asm stub;
  - the eight M9 errno values;
  - the packed argument-block sizes/offsets declared in
    include/zephyros/fs.h and mirrored by the runtime union;
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

SYSCALL_H = ROOT / "include" / "zephyros" / "syscall.h"
ERRNO_H = ROOT / "include" / "zephyros" / "errno.h"
FS_H = ROOT / "include" / "zephyros" / "fs.h"
KERNEL_FS = ROOT / "kernel" / "syscall" / "fs.c"
RUNTIME_ASM = ROOT / "user" / "runtime" / "zex_fs_runtime.asm"
RUNTIME_H = ROOT / "user" / "runtime" / "zex_fs_runtime.h"
RUNTIME_C = ROOT / "user" / "runtime" / "zos_fs.c"
VFS_H = ROOT / "kernel" / "vfs" / "vfs.h"
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

    def test_runtime_asm_mirrors_the_numbers(self):
        text = _read(RUNTIME_ASM)
        for value in FS_NUMBERS.values():
            self.assertRegex(text, r"\b%X\b" % value,
                             "runtime asm does not document number %X"
                             % value)

    def test_runtime_c_mirrors_the_numbers(self):
        text = _read(RUNTIME_C)
        for name, value in FS_NUMBERS.items():
            short = name.replace("ZOS_SYSCALL_FS_", "ZOS_FS_")
            self.assertRegex(
                text, r"#define %s\s+0x%02Xu" % (short, value),
                "runtime C does not mirror %s" % short)


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

    def test_runtime_union_covers_every_block(self):
        text = _read(RUNTIME_C)
        for name in ARG_BLOCKS:
            self.assertIn("struct %s" % name, text,
                          "runtime union has no %s arm" % name)


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
        text = _read(RUNTIME_H)
        self.assertIn("#define ZEX_PATH_MAX  127", text)
        self.assertIn("#define ZOS_FS_EINVAL 0x02", text)
        impl = _read(RUNTIME_C)
        self.assertIn("ZEX_PATH_MAX", impl)
        self.assertIn("return -ZOS_FS_EINVAL;", impl)


class TestDescriptorPoolShape(unittest.TestCase):
    def test_vfs_header_pins_pool_geometry(self):
        text = _read(VFS_H)
        for needle in ("#define VFS_FD_MAX 8u",
                       "#define VFS_DESC_FREE      0u",
                       "#define VFS_DESC_FILE      1u",
                       "#define VFS_DESC_DIRECTORY 2u",
                       "#define ZOS_EXEC_MAX_DEPTH 4u",
                       "sizeof(struct vfs_descriptor) == 17"):
            self.assertIn(needle, text)

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
        for needle in ("$(MODULES)/fstest.zex:", "$(MODULES)/fstest_l.zex:",
                       "tests/target/m9/fstest/fstest.c",
                       "tests/target/m9/fstest/fstest.json",
                       "user/runtime/zos_fs.c"):
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


if __name__ == "__main__":
    unittest.main()
