#!/usr/bin/env python3
"""Source-tree ownership tests (work packages 09-10, extended for M6).

Enforces docs/architecture.md section 3.1 and the current layout:

- implemented sources live under their owning boot/kernel/driver/test
  directories, and driver directories contain device implementation only;
- platform/zephyr80/ contains declarative constants/configuration only,
  with no executable content or instruction-emitting macros;
- the driver services' manifests have one owner (services/manifests/)
  and the shared SDCC runtime support another (modules/runtime);
- the process lifecycle (kernel/process), the common transition gates
  (kernel/memory), the public native ABI (abi/zos) and the user SDK
  (user/sdk) have explicit owners, with the ZEX program sources under
  tests/target/process, the demos under user/helloworld, and the
  ZEX tooling/fixtures under tests/subsys;
- all milestone acceptance code lives under tests/target/, behind the
  single ZOS_TARGET_TEST_BUILD seam; production sources contain no
  self-test implementations (the module fixtures under tests/modules/
  went with the module form, work package 42c);
- the retired M3 static-driver mechanism, driver-private SDCC glue, and
  test-only SIO build variants cannot return;
- the Mn-1 retention window holds: only the M10 acceptance
  configurations exist (work/00-SUPERVISOR.md), no pre-M10 acceptance
  target, image directory or boot-mode flag can come back (correction 01
  to work package 42b deletes the M9 machinery outright), and the
  mechanism coverage that replaced them is present;
- live build/source documentation contains no references to the removed
  implementation paths (dated acceptance records under docs/validation/
  and the work/ contracts are intentionally excluded);
- root build* directories are generated output only: .gitignore covers
  every build variant and make clean removes them all.

Run with: make test
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from maptools import evaluate, parse_asm_inc, parse_c_header  # noqa: E402

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
    "kernel/memory/banking.h",
    "kernel/runtime/sdcc_helpers.asm",
    "kernel/romfs/romfs.c",
    "kernel/romfs/rom_access.asm",
    "kernel/process/process.c",
    "kernel/process/exec.c",
    "kernel/process/launch.asm",
    "kernel/memory/user_copy.asm",
    "kernel/vfs/vfs.h",
    "kernel/vfs/path.c",
    "kernel/vfs/ns.h",
    "kernel/vfs/ns.c",
    "kernel/vfs/ns_file.c",
    "kernel/syscall/object.c",
    "kernel/syscall/object.h",
    "fs/iocfat/iocfat.c",
    "services/manifests/iocfat.json",
    "kernel/vfs/process.c",
    "kernel/vfs/workspace.c",
    "tools/romfspack.py",
    "tests/fs/fs_selftest.c",
    "tests/fs/fs_child.c",
    "tests/fs/manifest.json",
    "tests/test_fs_selftest_sim.py",
    "include/zephyros/romfs.h",
    "include/zephyros/process.h",
    "include/zephyros/zex.h",
    "abi/zos/types.h",
    "abi/zos/errno.h",
    "abi/zos/syscall.h",
    "abi/zos/fs.h",
    "abi/zos/process.h",
    "abi/zos/diag.h",
    "user/sdk/zex.mk",
    "user/sdk/include/zos.h",
    "user/sdk/include/zos/console.h",
    "user/sdk/include/zos/fs.h",
    "user/sdk/include/zos/process.h",
    "user/sdk/include/zos/diag.h",
    "user/sdk/include/zlibc/string.h",
    "user/sdk/include/zlibc/fmt.h",
    "user/sdk/include/zlibc/stdarg.h",
    "user/sdk/src/crt0.asm",
    "user/sdk/src/runtime.asm",
    "user/sdk/src/console_service.c",
    "user/sdk/src/gate.asm",
    "user/sdk/src/diaggate.asm",
    "user/sdk/src/args.c",
    "user/sdk/src/fs.c",
    "user/sdk/src/exec.c",
    "user/sdk/src/console_io.c",
    "user/sdk/src/zos_internal.h",
    "user/sdk/src/zlibc/strlen.c",
    "user/sdk/src/zlibc/strcmp.c",
    "user/sdk/src/zlibc/strncmp.c",
    "user/sdk/src/zlibc/strchr.c",
    "user/sdk/src/zlibc/strrchr.c",
    "user/sdk/src/zlibc/memcpy.c",
    "user/sdk/src/zlibc/memmove.c",
    "user/sdk/src/zlibc/memset.c",
    "user/sdk/src/zlibc/memcmp.c",
    "user/sdk/src/zlibc/strlcpy.c",
    "user/sdk/src/zlibc/strlcat.c",
    "user/sdk/src/zlibc/utoa.c",
    "user/sdk/src/zlibc/ultoa.c",
    "user/sdk/src/zlibc/itoa.c",
    "user/sdk/src/zlibc/ltoa.c",
    "user/sdk/src/zlibc/printf.c",
    "user/sdk/src/zlibc/sdcc_arith.c",
    "user/sdk/src/zlibc/sdcc_arith_signed.c",
    "user/sdk/src/zlibc/sdcc_arith_entries.asm",
    "user/sdk/src/zlibc/sdcc_arith_signed_entries.asm",
    "user/sdk/template/hello.c",
    "user/helloworld/hello.c",
    "user/helloworld/Makefile",
    "user/helloworld/README.md",
    "user/monitor/monitor.c",
    "user/monitor/monitor.h",
    "user/monitor/monitor_mem.c",
    "user/monitor/monitor_io.c",
    "user/monitor/hexfmt.c",
    "user/monitor/hexfmt.h",
    "user/monitor/hexdump.c",
    "user/monitor/hexdump.h",
    "user/monitor/status.c",
    "user/monitor/status.h",
    "tools/zexpack.py",
    "recovery/manifest.json",
    "tests/subsys/manifest.json",
    "tests/subsys/gen_crosspage.py",
    "tests/subsys/gen_romfs_expect.py",
    "tests/subsys/gen_zex_fixtures.py",
    "tests/subsys/gen_process_expect.py",
    "tests/target/process/zextest_sp.asm",
    "tests/test_sdk.py",
    "tools/zexmanifest.py",
    "tests/target/romfs/romfs_test.c",
    "tests/target/process/process_test.c",
    "tests/target/process/zextest.c",
    "drivers/ctc/ctc.c",
    "drivers/sio/sio.c",
    "drivers/sio/sio.h",
    "drivers/sio/sio_service.c",
    "drivers/sio/sio_boot.c",
    "drivers/sio/sio_io.asm",
    "drivers/ioc/ioc.c",
    "drivers/ioc/ioc_driver.h",
    "drivers/ioc/ioc_io.asm",
    "drivers/ioc/ioc_proto.h",
    "include/zephyros/ioc.h",
    "include/zephyros/ioc_protocol.h",
    "drivers/hid/hid.c",
    "drivers/hid/hid_driver.h",
    "drivers/v9958/v9958.c",
    "drivers/v9958/v9958_driver.h",
    "drivers/v9958/v9958_io.asm",
    "drivers/v9958/assets/font_cp850_6x8.inc",
    "tests/target/video/v9958_test.c",
    "tests/target/include/video_selftest.h",
    "tests/target/ioc/ioc_test.c",
    "tests/target/include/ioc_selftest.h",
    "tests/target/hid/hid_test.c",
    "tests/target/include/hid_selftest.h",
    "tests/target/console/integration_test.c",
    "tests/target/include/console_integration_selftest.h",
    "tests/console/manifest-serial.json",
    "tests/console/manifest-serial-missing.json",
    "modules/runtime/sdcc_indirect_calls.asm",
    "tests/target/syscall/syscall_entry.asm",
    "tests/target/syscall/syscall_test.c",
    "tests/target/irq/irq_entry.asm",
    "tests/target/irq/irq_test.c",
    "tests/target/include/syscall_selftest.h",
    "tests/target/include/syscall_entry.inc",
    "tests/target/include/irq_entry.inc",
    "tests/target/include/romfs_selftest.h",
    "tests/target/include/process_selftest.h",
    "tests/target/include/ctc_test.h",
    "tests/target/nucleus/nucleus_entry.asm",
    "tests/target/nucleus/nucleus_test.c",
    "tests/target/include/nucleus_selftest.h",
    "tests/target/include/nucleus_entry.inc",
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
    "kernel/device/driver.c",
    "kernel/irq/irq_selftest.c",
    "kernel/irq/selftest.c",
    "drivers/test/irq_probe.c",
    "drivers/test/fixtures.c",
    "drivers/test/irq_probe_io.asm",
    "include/zephyros/selftest.h",
    "include/zephyros/irq_selftest.h",
    "tests/target/zephyr80/selftest.asm",
    "tests/target/zephyr80/irq_selftest.asm",
    "user/runtime",
    "user/monitor/monitor_entry.asm",
    "user/monitor/monitor.json",
    "tests/subsys/zextest.json",
    "tests/target/process/zextest_entry.asm",
    # Correction 01 to work package 42b deleted the whole M9 acceptance
    # surface: its subjects are gone from M10, so the images, programs,
    # manifests and host modules may not come back.
    "tests/target/m9",
    "tests/target/m9/fstest",
    "tests/target/m9/exec",
    "tests/target/m9/iocfat",
    "tests/target/m9/fstest/fstest.c",
    "tests/target/m9/fstest/fstest_l.c",
    "tests/target/m9/fstest/fsfat.c",
    "tests/target/m9/exec/fschild.c",
    "tests/target/m9/exec/fsdeep.c",
    "tests/target/m9/exec/fsraw.c",
    "tests/target/m9/exec/fsraw_entry.asm",
    "tests/target/m9/iocfat/iocfat.c",
    "tests/target/m9/m9_exec_test.c",
    "tests/target/m9/m9_selftest.c",
    "tests/target/m9/m9_selftest_internal.h",
    "tests/target/m9/m9_selftest_runner.c",
    "tests/target/include/m9_selftest.h",
    "tests/target/include/m9_exec_selftest.h",
    "tests/target/include/m9_integration_selftest.h",
    "tests/target/include/m9_iocfat_selftest.h",
    "tests/m9/manifest-exec.json",
    "tests/m9/manifest-iocfat.json",
    "tests/m9/manifest-selftest.json",
    "tests/test_m9_exec.py",
    "tests/test_m9_exec_sim.py",
    "tests/test_m9_selftest.py",
    "tests/test_m9_selftest_sim.py",
    "tests/test_iocfat_sim.py",
    "tests/test_syscall_sim.py",
)

# Directories that must exist after the production/test separation.  Every
# acceptance implementation has a milestone owner under tests/target/;
# the module-format owners (kernel/module, tests/modules) are retired with
# the arena (work package 42c) and are asserted absent below.
REQUIRED_DIRS = (
    "boot",
    "kernel/memory", "kernel/syscall", "kernel/irq", "kernel/runtime",
    "kernel/romfs", "kernel/process",
    "kernel/vfs", "fs/iocfat", "services/manifests", "config",
    "drivers/sio", "drivers/ctc", "drivers/ioc", "drivers/hid",
    "drivers/v9958", "modules/runtime",
    "abi", "abi/zos",
    "user/sdk", "user/sdk/include", "user/sdk/src",
    "user/helloworld", "user/monitor",
    "recovery",
    "tests/subsys", "tests/console",
    "tests/fs",
    "tests/target/syscall", "tests/target/irq",
    "tests/target/romfs", "tests/target/process", "tests/target/support",
    "tests/target/ioc", "tests/target/hid", "tests/target/video",
    "tests/target/console", "tests/target/ns",
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
    "ZOS_SUBSYS_SELFTEST",
    "ZOS_ROMFS_CORRUPT_SELFTEST",
    "ZOS_IOC_SELFTEST",
    "ZOS_HID_SELFTEST",
    "ZOS_VIDEO_SELFTEST",
    "ZOS_CONSOLE_SELFTEST",
    "ZOS_CONSOLE_INTEGRATION_SELFTEST",
    "ZOS_CONSOLE_SERIAL_SELFTEST",
    "ZOS_CONSOLE_SERIAL_FAIL_SELFTEST",
    "ZOS_M9_VFS_SELFTEST",
    "ZOS_M9_PROVIDER_SELFTEST",
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
    "build-ioc-selftest",
    "build-hid-selftest",
    "build-video-selftest",
    "build-console-selftest",
    "build-console-integration-selftest",
    "build-console-serial-selftest",
    "build-console-serial-fail-selftest",
    "build-m9-vfs-selftest",
    "build-m9-required-mount-selftest",
    "build-m9-provider-selftest",
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

    Backslash continuations are joined first, so a rule header split over
    several lines (with its prerequisite list) is one logical line and a
    recipe line is attributed to the rule it belongs to.  Only a logical
    line that looks like a rule (a `target: prereq` header, not a
    `VAR := value` assignment) owns the recipe lines that follow it.
    """
    logical = []
    pending = ""
    for line in text.splitlines():
        if pending:
            pending += " " + line.strip()
        else:
            pending = line
        if pending.rstrip().endswith("\\"):
            pending = pending.rstrip()[:-1]
            continue
        logical.append(pending)
        pending = ""
    if pending:
        logical.append(pending)
    header = ""
    for line in logical:
        if line.startswith("\t"):
            yield header, line
        elif line.strip() and not line.lstrip().startswith("#"):
            header = line if (":" in line and ":=" not in line) else ""
        else:
            header = ""


# The headers a service image author may read: the M10 service ABI, the
# client-visible protocol vocabulary, and the narrow board-constant header
# a hardware-owning driver includes.  (kernel/... is deliberately not
# reachable from the service build.)
SERVICE_FACING_HEADERS = (
    "include/zephyros/service.h",
    "include/zephyros/irq.h",
    "include/zephyros/ioc.h",
    "include/zephyros/ioc_protocol.h",
    "include/zephyros/ipc.h",
    "include/zephyros/errno.h",
    "include/zephyros/romfs.h",
    "abi/zos/fs.h",
    "abi/zos/ipc.h",
    "abi/zos/object.h",
    "abi/zos/protocol.h",
    "abi/zos/types.h",
    "platform/zephyr80/hardware.h",
)

# Declarations that must not appear anywhere in the module-facing include
# closure.  ABI field names (`irq_claim`, `irq_release`) are
# intentionally absent: they are the service surface.  These are the kernel registry transaction/owner/storage,
# executive lifecycle, diagnostics, platform hooks, and raw memory
# primitives (work package 22).
KERNEL_ONLY_IDENTIFIERS = (
    "irq_init", "irq_registry_checkpoint", "irq_registry_rollback",
    "irq_registry_set_owner", "irq_registry_owner",
    "irq_unexpected_count", "irq_unclaimed_count", "irq_last_source",
    "irq_arch_lock", "irq_arch_unlock", "irq_arch_init",
    "platform_irq_vector", "platform_irq_quiesce",
    "platform_ctc0_test_start", "platform_ctc0_stop",
    "platform_memctl_read", "platform_memctl_write",
    "platform_bank_get", "platform_bank_set",
    "vfs_init", "vfs_phase",
    "vfs_find_provider", "vfs_provider_checkpoint", "vfs_provider_rollback",
    "vfs_provider_set_owner", "vfs_provider_owner", "vfs_path_normalize",
    "vfs_name_equal", "vfs_mount_resolve", "vfs_resolve_path",
    "vfs_is_mountpoint", "vfs_mount_is_root", "vfs_mount_is_root_level",
    "vfs_open", "vfs_read", "vfs_write", "vfs_seek", "vfs_sync", "vfs_close",
    "vfs_stat", "vfs_mkdir", "vfs_rmdir", "vfs_unlink", "vfs_rename",
    "vfs_opendir", "vfs_readdir", "vfs_closedir",
    "vfs_file_source", "vfs_mount_state",
    "vfs_mount_table", "vfs_mount_count", "vfs_mount_runtime",
    "vfs_workspace", "vfs_ws_enter", "vfs_ws_leave", "vfs_stat_norm",
    "VFS_O_EXEC", "VFS_MOUNT_SYSTEM",
)

# The include paths a representative module is compiled with: only the
# shared include/ and platform/ surfaces plus its own driver directory.
MODULE_PROBE_FLAGS = ("-mz80", "-c", "--nostdinc", "--Werror",
                      "-I", "abi", "-I", "include", "-I", "platform",
                      "-I", "drivers/sio")

_MODULE_PROBE_SOURCE = """\
#include "zephyros/service.h"
#include "zephyros/irq.h"
#include "zephyros/ioc.h"
#include "zephyros/ioc_protocol.h"
#include "zephyros/ipc.h"
#include "zephyr80/hardware.h"

/* A representative service image: it reads the flat plan, answers a
 * protocol frame, and its init takes the boot bundle the loader passes
 * in HL. */
static const struct ipc_service_api *probe_api;

int16_t probe_dispatch(uint8_t handle, uint8_t op,
		       struct zos_ipc_frame *frame)
{
	(void)handle;
	frame->status = 0;
	return (int16_t)(frame->op + (uint8_t)op);
}

void probe_init(const struct ipc_service_boot *boot)
{
	probe_api = boot->api;
	(void)probe_api->irq_claim(1u, 0x6000u, 0u);
	probe_api->console_putc((uint8_t)ZIOC_MAX_DATA);
}

uint8_t probe_plan(const struct zmod_header *header)
{
	return header->segment_count;
}

uint8_t probe_frame(const struct zioc_frame *frame)
{
	return frame->data[IOC_OFF_HID_INPUT_MAX];
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

    def test_retired_owners_are_gone(self):
        # Package 42c: the module/arena form is deleted outright, so no
        # source, header or fixture of it may come back.
        for rel in ("kernel/module", "kernel/module/arena.asm",
                    "tests/modules", "include/zephyros/module.h",
                    "include/zephyros/driver.h", "include/zephyros/device.h",
                    "include/zephyros/vfs.h", "include/zephyros/hid.h",
                    "include/zephyros/video.h", "include/zephyros/block.h",
                    "tools/mountgen.py", "config/mounts.json"):
            self.assertFalse(os.path.exists(os.path.join(ROOT, rel)),
                             "%s must stay deleted (work package 42c)" % rel)

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
        # Package 41: each driver directory holds the device implementation
        # (`*_service.c` protocol/init half and, where the driver has one,
        # the `*_boot.c` boot tier) plus its private headers and assembly.
        # The M8/M9 `*_module.c` registration adapters were retired with
        # the module form of the four drivers.
        self.assertEqual(sorted(os.listdir(os.path.join(ROOT, "drivers", "sio"))),
                         ["sio.c", "sio.h", "sio_boot.c", "sio_io.asm",
                          "sio_service.c"])
        self.assertEqual(sorted(os.listdir(os.path.join(ROOT, "drivers", "ctc"))),
                         ["ctc.c"])
        self.assertEqual(sorted(os.listdir(os.path.join(ROOT, "drivers", "ioc"))),
                         ["ioc.c", "ioc_boot.c", "ioc_driver.h", "ioc_io.asm",
                          "ioc_proto.h", "ioc_service.c"])
        self.assertEqual(sorted(os.listdir(os.path.join(ROOT, "drivers", "hid"))),
                         ["hid.c", "hid_driver.h"])
        self.assertEqual(sorted(os.listdir(os.path.join(ROOT, "drivers", "v9958"))),
                         ["assets", "v9958.c", "v9958_boot.c",
                          "v9958_driver.h", "v9958_font.asm", "v9958_io.asm",
                          "v9958_service.c"])

    def test_no_json_manifests_under_drivers(self):
        for path in _walk("drivers"):
            self.assertFalse(
                path.endswith(".json"),
                "a service manifest belongs under services/manifests/: %s"
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
        self.assertNotIn("zos_target_test_irq_hold", main)

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
        for name in ("irq.h", "platform.h", "kernel.h", "ipc.h",
                     "service.h", "process.h", "romfs.h"):
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


class TestServiceFacingHeaderBoundary(unittest.TestCase):
    """A service image reads the M10 service ABI, the client-visible
    protocol vocabulary and the board constants, but not kernel registry
    transactions, executive lifecycle controls, platform hooks, or raw
    memory primitives (work package 22; the M8 module form that defined
    this boundary is retired, work package 42c)."""

    def test_service_facing_closure_has_no_kernel_only_declarations(self):
        closure = _header_closure(SERVICE_FACING_HEADERS)
        offenders = []
        for rel in closure:
            text = _strip_comments(rel, _read(os.path.join(ROOT, rel)))
            for ident in KERNEL_ONLY_IDENTIFIERS:
                if re.search(r"\b%s\b" % re.escape(ident), text):
                    offenders.append("%s declares %s" % (rel, ident))
        self.assertEqual(
            offenders, [],
            "kernel-only declarations leak into the service-facing header "
            "closure:\n%s" % "\n".join(offenders))

    def test_service_facing_closure_keeps_the_abi_surface(self):
        headers = _header_closure(SERVICE_FACING_HEADERS)
        text = "".join(_read(os.path.join(ROOT, rel)) for rel in headers)
        # A resident service builds against the M10 service ABI
        # (abi/zos + include/zephyros): the flat plan, the call frame, the
        # boot bundle and the client-visible namespace vocabulary.
        for needle in ("struct zmod_header", "struct zmod_segment",
                       "struct zmod_endpoint", "struct zmod_dependency",
                       "struct zmod_publication", "struct ipc_service_api",
                       "struct ipc_service_boot", "struct zos_ipc_frame",
                       "struct zos_ipc_call_args", "struct zos_ns_entry",
                       "ZOS_TYPE_FILE", "ZOS_TYPE_FS", "ZOS_CLASS_FILE",
                       "struct zfs_stat", "ZFILE_OP_READ", "ZFS_OP_MKDIR",
                       "struct zioc_frame", "irq_source_t",
                       "irq_handler_t"):
            self.assertIn(needle, text)
        # The device model is gone: nothing may bring its vocabulary back.
        # The retired device model: the identifiers are assembled here
        # so this file does not itself carry the tokens the grep gate
        # keeps out of the tree.
        retired_tokens = ("device" + "_register", "zdevice" + "_desc",
                          "zchar" + "_ops", "ZDEV" + "_",
                          "zdriver" + "_api")
        for retired in retired_tokens:
            self.assertNotIn(retired, text)

    @unittest.skipUnless(shutil.which("sdcc"), "sdcc not available")
    def test_representative_service_compiles_against_service_headers_only(self):
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
                "a representative service image failed to compile against "
                "the service-facing headers alone:\n%s" % result.stdout)
            # A kernel-only reference must not resolve against these
            # headers alone; this is the negative half of the boundary.
            leak = os.path.join(tmp, "leak.c")
            with open(leak, "w") as handle:
                handle.write('#include "zephyros/service.h"\n'
                             "int probe_leak(void) {\n"
                             "\tvfs_mount_table[0] = 0;\n"
                             "\treturn 0;\n}\n")
            result = subprocess.run(
                ["sdcc"] + list(MODULE_PROBE_FLAGS) +
                ["-o", os.path.join(tmp, "leak.rel"), leak],
                cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True)
            self.assertNotEqual(
                result.returncode, 0,
                "a kernel-only symbol resolved from the service-facing "
                "headers:\n%s" % result.stdout)


class TestNoResidentBlockDriver(unittest.TestCase):
    """M9 defined a block class but keeps no resident block driver
    (architecture 32.15/32.39: a feature with no consumer is not made
    resident, for example `sd-ioc.zmod`).  This keeps the deferred-driver
    decision mechanical instead of a convention."""

    # A production ROMFS/module entry that names a block driver.
    BLOCK_NAME_RE = re.compile(r"(?i)block|(^|[-_])sd([-_.]|$)")

    def test_no_kernel_block_directory(self):
        self.assertFalse(
            os.path.isdir(os.path.join(ROOT, "kernel", "block")),
            "a kernel block/ directory must not exist in M9")

    def test_no_sd_driver_directory(self):
        self.assertFalse(
            os.path.isdir(os.path.join(ROOT, "drivers", "sd")),
            "drivers/sd/ must not exist until the first block consumer")

    def test_no_driver_registers_a_block_device(self):
        offenders = []
        for path in _walk("drivers", SOURCE_SUFFIXES):
            if "ZDEV" + "_BLOCK" in _code_only(path):
                offenders.append(os.path.relpath(path, ROOT))
        self.assertEqual(
            offenders, [],
            "a driver registers a block class in M9: %s"
            % ", ".join(offenders))

    def test_no_production_block_module_manifest(self):
        manifests = os.path.join(ROOT, "services", "manifests")
        offenders = [name for name in sorted(os.listdir(manifests))
                     if self.BLOCK_NAME_RE.search(name)]
        self.assertEqual(
            offenders, [],
            "a production service manifest names a block driver: %s"
            % ", ".join(offenders))

    def test_production_romfs_packs_no_block_driver(self):
        manifest = json.loads(
            _read(os.path.join(ROOT, "recovery", "manifest.json")))
        offenders = [entry["name"] for entry in manifest["files"]
                     if self.BLOCK_NAME_RE.search(entry["name"])]
        self.assertEqual(
            offenders, [],
            "the production ROMFS packs a block driver: %s"
            % ", ".join(offenders))

    def test_makefile_builds_no_block_driver(self):
        text = _read(os.path.join(ROOT, "Makefile"))
        for needle in ("sd-ioc.zmod", "/sd.zmod", "BOOT_SD_ZMOD",
                       "ZOS_BLOCK"):
            self.assertNotIn(
                needle, text,
                "the Makefile still builds or boots a block driver: %s"
                % needle)


class TestPlatformBoundary(unittest.TestCase):
    def test_platform_contains_only_constant_configuration_files(self):
        actual = set(os.listdir(PLATFORM_DIR))
        allowed = PLATFORM_FILES
        self.assertEqual(actual, allowed,
                         "platform/zephyr80/ must contain constants and "
                         "configuration only (unexpected: %s, missing: %s)"
                         % (sorted(actual - allowed),
                            sorted(allowed - actual)))

    def test_no_target_code_suffixes(self):
        for name in sorted(os.listdir(PLATFORM_DIR)):
            self.assertIn(os.path.splitext(name)[1], (".h", ".inc", ".mk"),
                          "platform file %s has a target-code suffix" % name)

    def test_no_executable_content(self):
        for directory, names in ((PLATFORM_DIR, sorted(os.listdir(PLATFORM_DIR))),):
            for name in names:
                path = os.path.join(directory, name)
                if os.path.isdir(path):
                    continue
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


# The milestone acceptance window (work/00-SUPERVISOR.md, "Milestone
# retention and compatibility"): only the current milestone and its
# predecessor keep acceptance machinery.  This list moves forward at the
# start of each milestone -- bump the lower bound when a milestone closes
# and add the new milestone on the right.
ALLOWED_MILESTONES = (10,)

# Acceptance configurations retired by the Mn-1 rule.  Each name belonged
# to exactly one milestone; none may come back, because their mechanism
# coverage now lives on the current boot path (the subsystem images and the
# M10 acceptance images).  Correction 01 to work package 42b retired the M9
# images and their hardware smoke targets outright.
RETIRED_ACCEPTANCE_TARGETS = (
    "syscall-selftest", "irq-selftest", "m3-selftest",
    "m4-loader-selftest", "m4-selftest", "m4-placement-selftest",
    "m4-failure-selftest", "m5-selftest", "m5-corrupt-selftest",
    "m6-selftest", "m6-failure-selftest", "m7-selftest",
    "m8-ioc-selftest", "m8-hid-selftest", "m8-hid-noioc-selftest",
    "m8-hid-badioc-selftest", "m8-v9958-selftest",
    "m8-v9958-fontfail-selftest", "m8-console-selftest", "m8-selftest",
    "m8-fallback-selftests",
    "smoke-hardware-selftest", "smoke-hardware-irq", "smoke-hardware-m3",
    "smoke-hardware-m4", "smoke-hardware-m4-failure", "smoke-hardware-m5",
    "smoke-hardware-m5-corrupt", "smoke-hardware-m6",
    "smoke-hardware-m6-failure", "smoke-hardware-m7",
    "smoke-hardware-m8", "smoke-hardware-m8-ioc", "smoke-hardware-m8-hid",
    "smoke-hardware-m8-v9958", "smoke-hardware-m8-console",
    "smoke-hardware-m8-fb-nohid", "smoke-hardware-m8-fb-nohid-nov9958",
    "smoke-hardware-m8-fb-badioc", "smoke-hardware-m8-fb-arena",
    "m9-selftest", "m9-exec-selftest", "m9-iocfat-selftest",
    "smoke-hardware-m9", "smoke-hardware-m9-exec",
    "smoke-hardware-m9-iocfat",
)

# Retired boot modes and boot-phase hooks.  Production keeps exactly one
# boot path (VFS/ROMFS boot with the derived arena) and one test seam.
RETIRED_BOOT_MODES = (
    "ZOS_ROMFS_BOOT", "ZOS_MONITOR_BOOT", "ZOS_M4_SIO_IRQ",
    "ZOS_M4_ARENA_BASE", "ZOS_M4_ARENA_END", "ZOS_M4_PLACEMENT",
)

# Live files that may not name any of the retired names above.
RETENTION_SCAN_ROOTS = ("boot", "drivers", "fs", "include", "kernel",
                        "modules", "platform", "tools", "user")


class TestMilestoneRetentionWindow(unittest.TestCase):
    """The Mn-1 retention rule, enforced mechanically."""

    def test_only_the_allowed_milestones_have_target_directories(self):
        target_dir = os.path.join(ROOT, "tests", "target")
        for name in sorted(os.listdir(target_dir)):
            match = re.match(r"^m(\d+)$", name)
            if not match:
                continue
            milestone = int(match.group(1))
            self.assertIn(
                milestone, ALLOWED_MILESTONES,
                "tests/target/%s/ exists outside the retention window %r; "
                "pre-Mn-1 acceptance code is retired, its mechanism "
                "coverage belongs under tests/target/<subsystem>/"
                % (name, ALLOWED_MILESTONES))

    def test_only_the_allowed_milestones_have_make_targets(self):
        text = _read(os.path.join(ROOT, "Makefile"))
        for line in text.splitlines():
            match = re.match(r"^(m(\d+)-[A-Za-z0-9_.-]*):", line)
            if not match:
                continue
            milestone = int(match.group(2))
            self.assertIn(
                milestone, ALLOWED_MILESTONES,
                "Makefile target %s is outside the retention window %r"
                % (match.group(1), ALLOWED_MILESTONES))

    def test_retired_acceptance_targets_are_gone(self):
        text = _read(os.path.join(ROOT, "Makefile"))
        for name in RETIRED_ACCEPTANCE_TARGETS:
            self.assertNotIn(
                "\n%s:" % name, text,
                "retired acceptance target %s reappeared in the Makefile"
                % name)

    def test_retired_boot_modes_do_not_reappear(self):
        for root_name in RETENTION_SCAN_ROOTS:
            for path in _walk(root_name, (".c", ".h", ".asm", ".inc",
                                          ".py")):
                body = _code_only(path)
                for token in RETIRED_BOOT_MODES:
                    self.assertNotIn(
                        token, body,
                        "%s names the retired boot mode %r; production has "
                        "one boot path"
                        % (os.path.relpath(path, ROOT), token))
        text = _read(os.path.join(ROOT, "Makefile"))
        for token in RETIRED_BOOT_MODES:
            self.assertNotIn(token, text,
                             "Makefile names the retired boot mode %r"
                             % token)

    def test_retired_smoke_modes_are_gone(self):
        text = _read(os.path.join(ROOT, "tools", "serial_smoke.py"))
        for mode in ("selftest", "irq", "m3", "m4", "m4-failure", "m5",
                     "m5-corrupt", "m6", "m6-failure", "m7", "m8-ioc",
                     "m8-hid", "m8-v9958", "m8-console", "m8",
                     "m8-fb-nohid", "m8-fb-nohid-nov9958", "m8-fb-badioc",
                     "m8-fb-arena"):
            self.assertNotIn(
                '"%s"' % mode, text,
                "retired serial_smoke mode %r reappeared" % mode)
        # The current acceptance modes stay available.
        self.assertIn('"subsystem"', text)
        self.assertIn('"console-integration"', text)
        self.assertIn('"console-serial"', text)
        self.assertIn('"fs"', text)

    def test_retired_milestone_data_directories_are_gone(self):
        for name in ("m5", "m6", "m7", "m8"):
            self.assertFalse(
                os.path.isdir(os.path.join(ROOT, "tests", name)),
                "tests/%s/ still holds retired milestone data" % name)

    def test_embedded_catalog_boot_path_is_gone(self):
        self.assertFalse(
            os.path.exists(os.path.join(ROOT, "kernel", "module",
                                        "embedded.c")),
            "the embedded-catalog boot path is retired")
        for token in ("module_catalog_load_all", "zmod_catalog",
                      "zmod_catalog_entry"):
            for root_name in ("kernel", "include", "boot"):
                for path in _walk(root_name, SOURCE_SUFFIXES):
                    self.assertNotIn(
                        token, _code_only(path),
                        "%s still names the retired embedded catalog (%s)"
                        % (os.path.relpath(path, ROOT), token))


# Acceptance constant pairs: the C header is the source of truth and the
# .inc is the assembly listing of the same acceptance image, so a drift
# between them is a silent test bug.
ACCEPTANCE_CONSTANT_PAIRS = (
    ("tests/target/include/syscall_selftest.h",
     "tests/target/include/syscall_entry.inc"),
    ("tests/target/include/irq_selftest.h",
     "tests/target/include/irq_entry.inc"),
    ("tests/target/include/nucleus_selftest.h",
     "tests/target/include/nucleus_entry.inc"),
)

# C/assembly constant pairs that must stay value-identical: the platform
# declarations, the interrupt-executive vectors, and the acceptance pairs
# above.
CONSTANT_PARITY_PAIRS = (
    ("platform/zephyr80/hardware.h", "platform/zephyr80/hardware.inc"),
    ("platform/zephyr80/memory.h", "platform/zephyr80/memory.inc"),
    ("platform/zephyr80/irq.h", "platform/zephyr80/irq.inc"),
) + ACCEPTANCE_CONSTANT_PAIRS


class TestConstantParity(unittest.TestCase):
    """C and assembly must agree on every shared constant.

    These are the parity loops the retired `tests/test_m1.py` owned; they
    are a mechanism (a constant that drifts between `hardware.h` and
    `hardware.inc` is a silent memory-map bug), so they moved here with
    the rest of the source-layout checks."""

    def test_shared_constants_have_identical_values(self):
        for c_rel, asm_rel in CONSTANT_PARITY_PAIRS:
            with self.subTest(pair="%s <-> %s" % (c_rel, asm_rel)):
                c = parse_c_header(os.path.join(ROOT, c_rel))
                asm = parse_asm_inc(os.path.join(ROOT, asm_rel))
                self.assertTrue(c, "%s parsed no constants" % c_rel)
                self.assertTrue(asm, "%s parsed no constants" % asm_rel)
                shared = sorted(set(c) & set(asm))
                self.assertTrue(
                    shared, "%s and %s share no constant" % (c_rel, asm_rel))
                for name in shared:
                    cv = evaluate(c[name], c)
                    av = evaluate(asm[name], asm)
                    self.assertEqual(
                        cv, av,
                        "%s: C %s = %d, assembly %s = %d"
                        % (name, c[name], cv, asm[name], av))

    def test_acceptance_constant_pairs_are_complete(self):
        # The acceptance .inc files exist for their assembly listing only,
        # so every C constant must be present in the assembly view too.
        for c_rel, asm_rel in ACCEPTANCE_CONSTANT_PAIRS:
            c = parse_c_header(os.path.join(ROOT, c_rel))
            asm = parse_asm_inc(os.path.join(ROOT, asm_rel))
            for name in sorted(c):
                self.assertIn(name, asm, "%s missing %s" % (asm_rel, name))

    def test_errno_codes_match_the_assembly_view(self):
        errno_c = parse_c_header(
            os.path.join(ROOT, "abi", "zos", "errno.h"))
        pairs = (("tests/target/include/syscall_entry.inc", "ZOS_ENOSYS"),
                 ("tests/target/include/irq_entry.inc", "ZOS_IST_ENOSYS"))
        for asm_rel, name in pairs:
            asm = parse_asm_inc(os.path.join(ROOT, asm_rel))
            self.assertIn(name, asm, "%s missing %s" % (asm_rel, name))
            self.assertEqual(evaluate(errno_c["ZOS_ENOSYS"], errno_c),
                             evaluate(asm[name], asm),
                             "%s differs from errno.h" % name)


class TestKernelNameComparison(unittest.TestCase):
    """One shared kernel ASCII name comparison (work package 30c, F3).

    The VFS, ROMFS and device registry used to carry three independent
    comparisons.  This pins the consolidation: a single implementation in
    `kernel/lib/name.c`, every caller using it with its own semantics
    (VFS/ROMFS fold, the registry does not; ROMFS passes its bounded field
    width), and no legacy copy left behind."""

    LEGACY = ("vfs_name_equal", "romfs_name_match", "device_name_equal")

    def _kernel_sources(self):
        for path in _walk("kernel", SOURCE_SUFFIXES):
            yield path

    def test_single_implementation(self):
        helper = _code_only(os.path.join(ROOT, "kernel", "lib", "name.c"))
        self.assertRegex(
            helper,
            r"\buint8_t\s+zos_ascii_equal\s*\(\s*const\s+char\s*\*",
            "kernel/lib/name.c must define the one shared comparison")
        for legacy in self.LEGACY:
            self.assertNotIn(legacy, helper)

    def test_no_legacy_comparison_remains(self):
        stale = []
        for path in self._kernel_sources():
            body = _code_only(path)
            for legacy in self.LEGACY:
                if re.search(r"\b%s\b" % legacy, body):
                    stale.append("%s: %s"
                                 % (os.path.relpath(path, ROOT), legacy))
        self.assertEqual(stale, [],
                         "legacy name comparisons still present:\n%s"
                         % "\n".join(stale))

    def test_callers_keep_their_exact_semantics(self):
        # (file, expected selector text) - the two trailing arguments are
        # the field bound and the case-fold selector.
        expected = {
            # Package 42b: the namespace router's publication table is the
            # only other caller; the M9 dir/mount/registry callers are gone.
            "kernel/vfs/ns.c": "0u, 1u",
            "kernel/romfs/romfs.c": "ROMFS_NAME_SIZE, 1u",
        }
        for rel, selector in expected.items():
            body = _code_only(os.path.join(ROOT, rel))
            calls = re.findall(r"zos_ascii_equal\(.{0,120}", body, re.S)
            self.assertTrue(calls, "%s does not call the shared helper" % rel)
            for call in calls:
                flat = " ".join(call.split())
                self.assertIn(selector, flat,
                              "%s must pass `%s` (got %r)"
                              % (rel, selector, flat))

    def test_helper_is_linked_into_every_image(self):
        linker = _read(os.path.join(ROOT, "linker", "zephyr80.lk"))
        self.assertIn("build/name.rel", linker)
        makefile = _read(os.path.join(ROOT, "Makefile"))
        self.assertIn("$(BUILD)/name.rel", makefile)
        self.assertIn("kernel/lib/name.c", makefile)


class TestUserSdkBoundary(unittest.TestCase):
    """User-space sources and ZEX programs reach only the public ABI
    (abi/zos) and the SDK (user/sdk); they never see include/zephyros/,
    kernel/ or platform/ (work package 28c)."""

    ZEX_FILES = (
        "tests/target/process/zextest.c",
        "tests/target/process/zextest_sp.asm",
        "tests/fs/fs_selftest.c",
        "tests/fs/fs_child.c",
    )
    FORBIDDEN = (
        re.compile(r'#\s*include\s+"zephyros/'),
        re.compile(r'#\s*include\s+"kernel/'),
        re.compile(r'#\s*include\s+"platform/'),
        re.compile(r'#\s*include\s+"include/'),
        re.compile(r'#\s*include\s+"device\.h"'),
        re.compile(r'#\s*include\s+"driver\.h"'),
        re.compile(r'#\s*include\s+"vfs\.h"'),
    )

    def _user_files(self):
        for path in _walk("user", SOURCE_SUFFIXES):
            yield path, os.path.relpath(path, ROOT).replace(os.sep, "/")
        for rel in self.ZEX_FILES:
            yield os.path.join(ROOT, rel), rel

    def test_user_sources_stay_on_the_public_boundary(self):
        violations = []
        for path, rel in self._user_files():
            body = _strip_comments(path, _read(path))
            for pattern in self.FORBIDDEN:
                match = pattern.search(body)
                if match:
                    violations.append("%s: %s"
                                      % (rel, match.group(0).strip()))
        self.assertEqual(
            violations, [],
            "user sources reach a kernel/platform header:\n%s"
            % "\n".join(violations))

    def test_user_build_rule_offers_only_abi_and_sdk(self):
        zex_mk = _read(os.path.join(ROOT, "user", "sdk", "zex.mk"))
        self.assertIn("-I abi -I $(ZOS_SDK_INCLUDE)", zex_mk)
        for token in ("-I include", "-Iinclude", "-I platform",
                      "-Iplatform", "-I kernel", "-Ikernel",
                      "drivers/"):
            self.assertNotIn(
                token, zex_mk,
                "the SDK build rule offers a non-public path: %r" % token)
        # The zex_program declarations name no include/ or platform/ rule.
        makefile = _read(os.path.join(ROOT, "Makefile"))
        for line in makefile.splitlines():
            if "zex_program" in line:
                for token in ("include/", "platform/"):
                    self.assertNotIn(
                        token, line,
                        "a zex_program line names %r" % token)

    def test_sdk_headers_do_not_include_kernel_paths(self):
        for path in _walk("user/sdk/include", (".h",)):
            body = _strip_comments(path, _read(path))
            for pattern in self.FORBIDDEN:
                self.assertIsNone(
                    pattern.search(body),
                    "%s reaches a kernel/platform header"
                    % os.path.relpath(path, ROOT))


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
