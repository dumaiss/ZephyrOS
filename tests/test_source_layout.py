#!/usr/bin/env python3
"""Source-tree ownership tests.

Enforces docs/architecture.md section 1.3 and the source layout:

- implemented sources live under their owning boot/kernel/driver/test
  directories, and driver directories contain device implementation and
  the driver's own manifest only; the audience headers live under
  drivers/include, kernel/include and formats/include, the platform
  constants under platform/zephyr80/{public,private};
- platform/zephyr80/ contains declarative constants/configuration only,
  with no executable content or instruction-emitting macros;
- the driver manifests have one owner (drivers/<name>/) and the shared
  SDCC runtime support another (drivers/runtime);
- the process lifecycle (kernel/process), the common transition gates
  (kernel/memory), the public native ABI (abi/kos) and the user SDK
  (user/sdk) have explicit owners, with the EXE program sources under
  tests/target/process, the demos under user/helloworld, and the
  EXE tooling/fixtures under tests/subsys;
- all acceptance code lives under tests/target/, behind the single
  KOS_TARGET_TEST_BUILD seam; production sources contain no self-test
  implementations and no test-only SIO build variants;
- each build class offers its exact include roots, and drivers and user
  programs see only their audience headers;
- every image is composed by a systems/<name>.json file;
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
# configuration (.mk) only, split by audience:
# config.mk plus the public/ board constants and the private/ memory and
# interrupt declarations.
PLATFORM_FILES = {"config.mk"}
PLATFORM_PUBLIC_FILES = {"hardware.h", "hardware.inc"}
PLATFORM_PRIVATE_FILES = {"memory.h", "memory.inc", "irq.h", "irq.inc"}

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
    "kernel/vfs/router.c",
    "kernel/vfs/dev.c",
    "kernel/vfs/fs.c",
    "kernel/vfs/system.c",
    "kernel/syscall/object.c",
    "kernel/syscall/object.h",
    "drivers/iocfat/iocfat.c",
    "drivers/iocfat/iocfat.json",
    "kernel/vfs/handles.c",
    "kernel/vfs/workspace.c",
    "tools/romfspack.py",
    "tests/fs/fs_selftest.c",
    "tests/fs/fs_child.c",
    "tests/bench/iobench.c",
    "systems/fs.json",
    "tests/test_fs_selftest_sim.py",
    "formats/include/romfs.h",
    "formats/include/exe.h",
    "formats/include/mod.h",
    "kernel/include/kernel.h",
    "kernel/include/process.h",
    "kernel/include/platform.h",
    "kernel/include/irq_types.h",
    "drivers/include/driver.h",
    "drivers/include/ioc.h",
    "drivers/include/ioc_protocol.h",
    "platform/zephyr80/public/hardware.h",
    "platform/zephyr80/public/hardware.inc",
    "platform/zephyr80/private/memory.h",
    "platform/zephyr80/private/memory.inc",
    "platform/zephyr80/private/irq.h",
    "platform/zephyr80/private/irq.inc",
    "abi/kos/types.h",
    "abi/kos/errno.h",
    "abi/kos/syscall.h",
    "abi/kos/fs.h",
    "abi/kos/process.h",
    "abi/kos/diag.h",
    "user/sdk/exe.mk",
    "user/sdk/include/kos.h",
    "user/sdk/include/kos/console.h",
    "user/sdk/include/kos/fs.h",
    "user/sdk/include/kos/process.h",
    "user/sdk/include/kos/diag.h",
    "user/sdk/include/kos/timer.h",
    "user/sdk/include/kos/sound.h",
    "user/sdk/include/klibc/string.h",
    "user/sdk/include/klibc/fmt.h",
    "user/sdk/include/klibc/stdarg.h",
    "user/sdk/src/crt0.asm",
    "user/sdk/src/runtime.asm",
    "user/sdk/src/console_proto.c",
    "user/sdk/src/gate.asm",
    "user/sdk/src/diaggate.asm",
    "user/sdk/src/args.c",
    "user/sdk/src/fs.c",
    "user/sdk/src/exec.c",
    "user/sdk/src/console_io.c",
    "user/sdk/src/timer.c",
    "user/sdk/src/sound.c",
    "user/sdk/src/sound_out.asm",
    "user/sdk/src/kos_internal.h",
    "user/sdk/src/klibc/strlen.asm",
    "user/sdk/src/klibc/strcmp.asm",
    "user/sdk/src/klibc/strncmp.asm",
    "user/sdk/src/klibc/strchr.asm",
    "user/sdk/src/klibc/strrchr.c",
    "user/sdk/src/klibc/memcpy.asm",
    "user/sdk/src/klibc/memmove.asm",
    "user/sdk/src/klibc/memset.asm",
    "user/sdk/src/klibc/memcmp.asm",
    "user/sdk/src/klibc/strlcpy.c",
    "user/sdk/src/klibc/strlcat.c",
    "user/sdk/src/klibc/utoa.c",
    "user/sdk/src/klibc/ultoa.c",
    "user/sdk/src/klibc/itoa.c",
    "user/sdk/src/klibc/ltoa.c",
    "user/sdk/src/klibc/printf.c",
    "user/sdk/src/klibc/sdcc_arith.c",
    "user/sdk/src/klibc/sdcc_arith_signed.c",
    "user/sdk/src/klibc/sdcc_arith16.asm",
    "user/sdk/src/klibc/divmod.asm",
    "user/sdk/src/klibc/mul.asm",
    "user/sdk/src/klibc/sdcc_arith_entries.asm",
    "user/sdk/src/klibc/sdcc_arith_signed_entries.asm",
    "user/sdk/template/hello.c",
    "user/sdk/template/Makefile",
    "user/sdk/examples/Makefile",
    "user/sdk/examples/hello.c",
    "user/sdk/examples/fardemo.c",
    "user/sdk/examples/fardemo_far.c",
    "user/sdk/examples/files.c",
    "mk/kos.mk",
    "config/sdk.mk",
    "tools/ihx.py",
    "tools/sdkpack.py",
    "tests/test_sdk_package.py",
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
    "user/shell/main.c",
    "user/shell/shell.h",
    "user/shell/parse.c",
    "user/shell/path.c",
    "user/shell/lookup.c",
    "user/shell/builtin.c",
    "user/shell/fscmd.c",
    "tools/exepack.py",
    "systems/production.json",
    "systems/subsystem.json",
    "tests/subsys/gen_crosspage.py",
    "tests/subsys/gen_romfs_expect.py",
    "tests/subsys/gen_exe_fixtures.py",
    "tests/subsys/gen_process_expect.py",
    "tests/target/process/exetest_sp.asm",
    "tests/test_sdk.py",
    "tools/exemanifest.py",
    "tests/target/romfs/romfs_test.c",
    "tests/target/process/process_test.c",
    "tests/target/process/exetest.c",
    "kernel/irq/ctc.c",
    "drivers/timer/timer.c",
    "drivers/timer/timer_proto.c",
    "drivers/timer/timer_boot.c",
    "tests/timer/timer.c",
    "drivers/sio/sio.c",
    "drivers/sio/sio.h",
    "drivers/sio/sio_proto.c",
    "drivers/sio/sio_boot.c",
    "drivers/sio/sio_io.asm",
    "drivers/ioc/ioc.c",
    "drivers/ioc/ioc_driver.h",
    "drivers/ioc/ioc_io.asm",
    "drivers/ioc/ioc_proto.h",
    "drivers/include/ioc.h",
    "drivers/include/ioc_protocol.h",
    "drivers/hid/hid.c",
    "drivers/hid/hid_driver.h",
    "drivers/v9958/v9958.c",
    "drivers/v9958/v9958_driver.h",
    "drivers/v9958/v9958_io.asm",
    "drivers/v9958/assets/font_cp850_6x8.inc",
    "systems/console-serial.json",
    "systems/console-serial-missing.json",
    "systems/console-init-fail.json",
    "drivers/runtime/sdcc_indirect_calls.asm",
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
    "systems/nucleus.json",
    "systems/ipc.json",
    "systems/ns.json",
    "systems/xmem.json",
    "systems/helloworld.json",
    "tools/systemgen.py",
    "tests/test_systemgen.py",
)

# Directories that must exist: the production owners and the acceptance
# owners under tests/target/.
REQUIRED_DIRS = (
    "boot",
    "kernel/memory", "kernel/syscall", "kernel/irq", "kernel/runtime",
    "kernel/romfs", "kernel/process",
    "kernel/vfs", "drivers/iocfat", "config",
    "drivers/sio", "drivers/ioc", "drivers/hid",
    "drivers/v9958", "drivers/console", "drivers/runtime",
    "drivers/timer", "drivers/include", "kernel/include", "formats/include",
    "platform/zephyr80/public", "platform/zephyr80/private",
    "abi", "abi/kos",
    "user/sdk", "user/sdk/include", "user/sdk/src",
    "user/helloworld", "user/monitor", "user/shell",
    "recovery",
    "tests/subsys", "tests/console",
    "tests/fs", "tests/bench", "tests/io",
    "tests/target/syscall", "tests/target/irq",
    "tests/target/romfs", "tests/target/process", "tests/target/support",
    "tests/target/ns",
    "tests/target/include",
)

# Test-only SIO build variants must be confined to tests/ (the production
# module entry has no conditional test branches).
TEST_ONLY_SIO_MACROS = (
    "KOS_SIO_MODULE_IRQ_SELFTEST",
    "KOS_SIO_MODULE_INIT_FAIL",
)

# Self-test implementations live only under tests/.  Production files
# reach acceptance code solely through the labeled TEST-ONLY HOOK in
# kernel/main.c and the shared KOS_TARGET_TEST_BUILD seam.
PRODUCTION_SELFTEST_MACROS = (
    "KOS_SUBSYS_SELFTEST",
    "KOS_ROMFS_CORRUPT_SELFTEST",
    "KOS_CONSOLE_SELFTEST",
    "KOS_CONSOLE_SERIAL_SELFTEST",
    "KOS_CONSOLE_SERIAL_FAIL_SELFTEST",
    "KOS_FS_SELFTEST",
    "KOS_NS_SELFTEST",
)

# Generated-output names that must be ignored and cleaned.  build-modules is
# the shared module directory used by the MOD targets.
BUILD_VARIANTS = (
    "build",
    "build-selftest",
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


def _platform_files():
    """Every platform file: the top-level build configuration plus the
    public/ and private/ constant pairs."""
    for root in (PLATFORM_DIR,
                 os.path.join(PLATFORM_DIR, "public"),
                 os.path.join(PLATFORM_DIR, "private")):
        for name in sorted(os.listdir(root)):
            if os.path.isfile(os.path.join(root, name)):
                yield root, name


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
PRODUCTION_SOURCE_ROOTS = ("boot", "drivers", "formats", "kernel",
                           "user")
SOURCE_SUFFIXES = (".c", ".h", ".asm", ".inc")

DRIVERS_DIR = os.path.join(ROOT, "drivers")


# Directories under drivers/ that are not driver owners: the shared
# driver-facing headers and the module-local SDCC glue.
DRIVER_SUPPORT_DIRS = ("include", "runtime")


def _driver_private_headers():
    """Map every driver-private header basename to its owning driver dir."""
    owners = {}
    for name in sorted(os.listdir(DRIVERS_DIR)):
        if name in DRIVER_SUPPORT_DIRS:
            continue
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


# The headers a driver image author may read: the driver ABI, the
# client-visible protocol vocabulary, and the narrow board-constant header
# a hardware-owning driver includes.  (kernel/... is deliberately not
# reachable from the driver build.)
DRIVER_FACING_HEADERS = (
    "formats/include/mod.h",
    "kernel/include/irq_types.h",
    "drivers/include/ioc.h",
    "drivers/include/ioc_protocol.h",
    "drivers/include/driver.h",
    "abi/kos/errno.h",
    "formats/include/romfs.h",
    "abi/kos/fs.h",
    "abi/kos/ipc.h",
    "abi/kos/object.h",
    "abi/kos/protocol.h",
    "abi/kos/types.h",
    "platform/zephyr80/public/hardware.h",
)

# Declarations that must not appear anywhere in the module-facing include
# closure.  ABI field names (`irq_claim`, `irq_release`) are
# intentionally absent: they are the driver surface.  These are the kernel registry transaction/owner/storage,
# executive lifecycle, diagnostics, platform hooks, and raw memory
# primitives.
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

# The include paths a representative driver image is compiled with: only
# the driver-facing roots plus its own driver directory, never `.`.
MODULE_PROBE_FLAGS = ("-mz80", "-c", "--nostdinc", "--Werror",
                      "-I", "abi", "-I", "drivers/include",
                      "-I", "platform/zephyr80/public", "-I", "drivers/sio")

_MODULE_PROBE_SOURCE = """\
#include "driver.h"
#include "ioc.h"
#include "ioc_protocol.h"
#include "hardware.h"

/* A representative driver image: it answers a protocol frame and its
 * init takes the boot bundle the loader passes in HL. */
static const struct driver_api *probe_api;

int16_t probe_dispatch(uint8_t handle, uint8_t op,
		       struct kos_ipc_frame *frame)
{
	(void)handle;
	frame->status = 0;
	return (int16_t)(frame->op + (uint8_t)op);
}

void probe_init(const struct driver_boot *boot)
{
	probe_api = boot->api;
	(void)probe_api->irq_claim(1u, 0x6000u, 0u);
	probe_api->console_putc((uint8_t)KIOC_MAX_DATA);
}

uint8_t probe_frame(const struct kioc_frame *frame)
{
	return frame->data[IOC_OFF_HID_INPUT_MAX];
}
"""


# The driver-facing include roots: a quoted include
# resolves against the including file's directory first, then these.
DRIVER_INCLUDE_ROOTS = (
    os.path.join(ROOT, "abi"),
    os.path.join(ROOT, "drivers", "include"),
    os.path.join(ROOT, "platform", "zephyr80", "public"),
)


def _resolve_quoted_include(name, origin_dir):
    """Resolve a quoted include the way the driver build does."""
    candidates = [os.path.join(origin_dir, name)]
    candidates += [os.path.join(root, name) for root in DRIVER_INCLUDE_ROOTS]
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
    test MOD entries, manifests and glue live under their explicit
    non-production owners, behind the single test-build seam."""

    def test_driver_directories_contain_device_implementation_only(self):
        # Each driver directory holds the device implementation (the
        # `*_proto.c` protocol half and, where the driver has one, the
        # `*_boot.c` boot tier) plus its private headers, assembly and
        # manifest.
        self.assertEqual(sorted(os.listdir(os.path.join(ROOT, "drivers", "sio"))),
                         ["sio.c", "sio.h", "sio.json", "sio_boot.c",
                          "sio_io.asm", "sio_proto.c"])
        self.assertEqual(sorted(os.listdir(os.path.join(ROOT, "drivers", "ioc"))),
                         ["ioc.c", "ioc.json", "ioc_boot.c", "ioc_driver.h",
                          "ioc_io.asm", "ioc_proto.c", "ioc_proto.h"])
        self.assertEqual(sorted(os.listdir(os.path.join(ROOT, "drivers", "hid"))),
                         ["hid.c", "hid.json", "hid_driver.h"])
        self.assertEqual(sorted(os.listdir(os.path.join(ROOT, "drivers", "v9958"))),
                         ["assets", "v9958.c", "v9958.json", "v9958_boot.c",
                          "v9958_driver.h", "v9958_font.asm", "v9958_io.asm",
                          "v9958_proto.c"])
        self.assertEqual(
            sorted(os.listdir(os.path.join(ROOT, "drivers", "console"))),
            ["console-serial.json", "console-v9958-hid.json",
             "console_proto.c"])
        self.assertEqual(
            sorted(os.listdir(os.path.join(ROOT, "drivers", "timer"))),
            ["timer.c", "timer.json", "timer_boot.c", "timer_io.asm",
             "timer_proto.c"])
        self.assertEqual(
            sorted(os.listdir(os.path.join(ROOT, "drivers", "iocfat"))),
            ["iocfat.c", "iocfat.json", "iocfat_cache.asm",
             "iocfat_cfile.asm", "iocfat_memo.asm"])
        self.assertEqual(
            sorted(os.listdir(os.path.join(ROOT, "drivers", "runtime"))),
            ["sdcc_indirect_calls.asm"])

    def test_driver_manifests_live_with_their_driver(self):
        # A driver owns its manifest in its own directory
        # (drivers/<name>/<name>.json; the two console drivers share
        # drivers/console/).
        shipped = []
        for path in _walk("drivers", (".json",)):
            shipped.append(os.path.relpath(path, ROOT).replace(os.sep, "/"))
        self.assertEqual(sorted(shipped), [
            "drivers/console/console-serial.json",
            "drivers/console/console-v9958-hid.json",
            "drivers/hid/hid.json",
            "drivers/ioc/ioc.json",
            "drivers/iocfat/iocfat.json",
            "drivers/sio/sio.json",
            "drivers/timer/timer.json",
            "drivers/v9958/v9958.json",
        ])

    def test_no_driver_private_sdcc_glue(self):
        # The shared source lives in drivers/runtime/; the one copy every
        # driver image packs is that file itself.
        shared = os.path.join(ROOT, "drivers", "runtime",
                              "sdcc_indirect_calls.asm")
        for path in _walk("drivers", (".c", ".h", ".asm", ".inc")):
            if os.path.abspath(path) == shared:
                continue
            body = _code_only(path)
            for symbol in ("___sdcc_call_iy", "___sdcc_call_hl"):
                self.assertNotIn(
                    symbol, body,
                    "%s defines module-local SDCC glue; the shared source is "
                    "drivers/runtime/sdcc_indirect_calls.asm"
                    % os.path.relpath(path, ROOT))

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

    def test_production_sources_have_no_selftests(self):
        for root_name in ("boot", "drivers", "kernel", "modules"):
            for path in _walk(root_name):
                if not path.endswith((".c", ".h", ".asm", ".inc")):
                    continue
                body = _code_only(path)
                for macro in PRODUCTION_SELFTEST_MACROS:
                    self.assertNotIn(
                        macro, body,
                        "%s contains a self-test implementation; "
                        "acceptance code belongs under tests/target/"
                        % os.path.relpath(path, ROOT))

    def test_main_keeps_only_labeled_test_hooks(self):
        main = _read(os.path.join(ROOT, "kernel", "main.c"))
        self.assertIn("KOS_TARGET_TEST_BUILD", main)
        self.assertIn("tests/target/support/target_test.c", main)
        self.assertIn("TEST-ONLY HOOK", main)

    def test_production_sources_have_no_ctc_acceptance_helpers(self):
        # The CTC0 programming is test-owned; no production source may
        # define, declare, or call it.
        for root_name in ("boot", "drivers", "kernel", "modules"):
            for path in _walk(root_name, SOURCE_SUFFIXES):
                body = _code_only(path)
                for token in ("platform_ctc0_test_start",
                              "platform_ctc0_stop",
                              "kos_ctc0_test_start", "kos_ctc0_test_stop"):
                    self.assertNotIn(
                        token, body,
                        "%s contains acceptance-only CTC code %r"
                        % (os.path.relpath(path, ROOT), token))

    def test_production_headers_declare_no_test_runners(self):
        for name in ("kernel/include/irq_types.h",
                     "kernel/include/platform.h", "kernel/include/kernel.h",
                     "drivers/include/driver.h", "formats/include/mod.h",
                     "kernel/include/process.h", "formats/include/romfs.h"):
            body = _code_only(os.path.join(ROOT, name))
            self.assertNotIn("kos_run_", body,
                             "%s declares a target-test runner" % name)

    def test_makefile_has_single_test_seam(self):
        text = _read(os.path.join(ROOT, "Makefile"))
        self.assertIn("KOS_TARGET_TEST_BUILD=1", text)
        self.assertIn("tests/target/include", text)
        self.assertIn("target_test.rel", text)
        self.assertNotIn("SELFTEST_OBJS", text)


class TestCrossDriverIncludeBoundary(unittest.TestCase):
    """A production driver reaches a sibling only through a public header
    (include/, platform/) or its own directory's private headers.  A
    sibling driver's private include path and private headers are never a
    production dependency edge."""

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
        leaked = ('#include "konaos/device.h"\n'
                  '#include "ioc_proto.h"\n')
        self.assertTrue(_sibling_private_includes(
            "drivers/hid/hid.c", leaked, owners))
        relative = '#include "../ioc/ioc_proto.h"\n'
        self.assertTrue(_sibling_private_includes(
            "drivers/hid/hid.c", relative, owners))
        allowed = ('#include "ioc_protocol.h"\n'
                   '#include "hid_driver.h"\n')
        self.assertEqual(_sibling_private_includes(
            "drivers/hid/hid.c", allowed, owners), [])


class TestDriverFacingHeaderBoundary(unittest.TestCase):
    """A driver image reads the driver ABI, the client-visible protocol
    vocabulary and the board constants, but not kernel registry
    transactions, executive lifecycle controls, platform hooks, or raw
    memory primitives."""

    def test_driver_facing_closure_has_no_kernel_only_declarations(self):
        closure = _header_closure(DRIVER_FACING_HEADERS)
        offenders = []
        for rel in closure:
            text = _strip_comments(rel, _read(os.path.join(ROOT, rel)))
            for ident in KERNEL_ONLY_IDENTIFIERS:
                if re.search(r"\b%s\b" % re.escape(ident), text):
                    offenders.append("%s declares %s" % (rel, ident))
        self.assertEqual(
            offenders, [],
            "kernel-only declarations leak into the driver-facing header "
            "closure:\n%s" % "\n".join(offenders))

    def test_driver_facing_closure_keeps_the_abi_surface(self):
        headers = _header_closure(DRIVER_FACING_HEADERS)
        text = "".join(_read(os.path.join(ROOT, rel)) for rel in headers)
        # A resident driver builds against the driver ABI (abi/kos +
        # drivers/include + formats/include): the flat plan, the call
        # frame, the boot bundle and the client-visible namespace
        # vocabulary.
        for needle in ("struct mod_header", "struct mod_segment",
                       "struct mod_endpoint", "struct mod_dependency",
                       "struct mod_publication", "struct driver_api",
                       "struct driver_boot", "struct kos_ipc_frame",
                       "struct kos_ipc_call_args", "struct kos_ns_entry",
                       "KOS_TYPE_FILE", "KOS_TYPE_FS", "KOS_CLASS_FILE",
                       "struct kfs_stat", "KFILE_OP_READ", "KFS_OP_MKDIR",
                       "struct kioc_frame", "irq_source_t",
                       "irq_handler_t"):
            self.assertIn(needle, text)

    @unittest.skipUnless(shutil.which("sdcc"), "sdcc not available")
    def test_representative_driver_compiles_against_driver_headers_only(self):
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
                "a representative driver image failed to compile against "
                "the driver-facing headers alone:\n%s" % result.stdout)
            # A kernel-only reference must not resolve against these
            # headers alone; this is the negative half of the boundary.
            leak = os.path.join(tmp, "leak.c")
            with open(leak, "w") as handle:
                handle.write('#include "driver.h"\n'
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
                "a kernel-only symbol resolved from the driver-facing "
                "headers:\n%s" % result.stdout)

    @unittest.skipUnless(shutil.which("sdcc"), "sdcc not available")
    def test_driver_roots_cannot_reach_kernel_h(self):
        # The negative include test: the driver include roots carry no
        # kernel-private header, so a one-line source including
        # "kernel.h" must fail to compile against them.
        with tempfile.TemporaryDirectory() as tmp:
            source = os.path.join(tmp, "kernel_h.c")
            with open(source, "w") as handle:
                handle.write('#include "kernel.h"\n')
            result = subprocess.run(
                ["sdcc"] + list(MODULE_PROBE_FLAGS) +
                ["-o", os.path.join(tmp, "kernel_h.rel"), source],
                cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True)
            self.assertNotEqual(
                result.returncode, 0,
                "kernel.h resolved through the driver include roots, so the "
                "audience split leaks:\n%s" % result.stdout)


def _make_variable(text, name):
    """The backslash-joined value of one Makefile assignment (first match)."""
    lines = text.splitlines()
    for index, line in enumerate(lines):
        match = re.match(r"^%s\s*:?=\s*(.*)$" % re.escape(name), line)
        if not match:
            continue
        value = match.group(1)
        while value.rstrip().endswith("\\"):
            index += 1
            value = value.rstrip()[:-1] + " " + lines[index].strip()
        return " ".join(value.split())
    return None


def _include_roots(value, variables):
    """The `-I` roots of a flags value with one expansion pass."""
    expanded = re.sub(r"\$\((\w+)\)",
                      lambda match: variables.get(match.group(1), match.group(0)),
                      value or "")
    tokens = expanded.split()
    roots = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token == "-I":
            roots.append(tokens[index + 1])
            index += 2
            continue
        if token.startswith("-I") and len(token) > 2:
            roots.append(token[2:])
        index += 1
    return roots


class TestIncludeRootAllowLists(unittest.TestCase):
    """Each build class offers its exact `-I` set."""

    # Nucleus and kernel-linked tests: `.` plus the audience roots.
    NUCLEUS_ROOTS = [".", "abi", "kernel/include", "drivers/include",
                     "formats/include", "platform/zephyr80/public",
                     "platform/zephyr80/private", "$(BUILD)"]
    # Drivers (DRV_CFLAGS, MOD_CFLAGS): no `.`; MOD_CFLAGS also carries the
    # representative driver's own directory.
    DRIVER_ROOTS = ["abi", "drivers/include", "platform/zephyr80/public",
                    "$(BUILD)"]
    # The SDK and its programs: the ABI root first, then the SDK include
    # root.
    SDK_ROOTS = ["abi", "user/sdk/include"]

    def _config_variables(self):
        config = _read(os.path.join(ROOT, "platform", "zephyr80", "config.mk"))
        return {"KOS_PLATFORM_CFLAGS": _make_variable(config,
                                                      "KOS_PLATFORM_CFLAGS")}

    def test_nucleus_include_roots_are_exact(self):
        makefile = _read(os.path.join(ROOT, "Makefile"))
        roots = _include_roots(_make_variable(makefile, "CFLAGS"),
                               self._config_variables())
        self.assertEqual(roots, self.NUCLEUS_ROOTS)

    def test_driver_include_roots_are_exact(self):
        makefile = _read(os.path.join(ROOT, "Makefile"))
        for name, extra in (("DRV_CFLAGS", []),
                            ("MOD_CFLAGS", ["drivers/sio"])):
            with self.subTest(variable=name):
                roots = _include_roots(_make_variable(makefile, name), {})
                self.assertEqual(roots, self.DRIVER_ROOTS + extra, name)
                self.assertNotIn(".", roots,
                                 "%s must not offer the kernel tree" % name)

    def test_sdk_include_roots_are_exact(self):
        kos_mk = _read(os.path.join(ROOT, "mk", "kos.mk"))
        roots = _include_roots(
            _make_variable(kos_mk, "EXE_CFLAGS"),
            {"EXE_ABI_ROOT": "abi", "KOS_INCLUDE": "user/sdk/include"})
        self.assertEqual(roots, self.SDK_ROOTS)
        # The in-tree values the expansion used come from the wrapper.
        exe_mk = _read(os.path.join(ROOT, "user", "sdk", "exe.mk"))
        self.assertIn("EXE_ABI_ROOT := abi", exe_mk)
        self.assertIn("KOS_INCLUDE  := $(KOS_SDK_ROOT)/include", exe_mk)


class TestNoResidentBlockDriver(unittest.TestCase):
    """No resident block driver until the first block consumer (a
    feature with no consumer is not made resident, for example
    `sd-ioc.mod`).  This keeps the deferred-driver decision mechanical
    instead of a convention."""

    # A production ROMFS/module entry that names a block driver.
    BLOCK_NAME_RE = re.compile(r"(?i)block|(^|[-_])sd([-_.]|$)")

    def test_no_kernel_block_directory(self):
        self.assertFalse(
            os.path.isdir(os.path.join(ROOT, "kernel", "block")),
            "a kernel block/ directory must not exist")

    def test_no_sd_driver_directory(self):
        self.assertFalse(
            os.path.isdir(os.path.join(ROOT, "drivers", "sd")),
            "drivers/sd/ must not exist until the first block consumer")

    def test_no_production_block_module_manifest(self):
        # Each driver carries its manifest in its own directory.
        offenders = []
        for path in _walk("drivers", (".json",)):
            if self.BLOCK_NAME_RE.search(os.path.basename(path)):
                offenders.append(os.path.relpath(path, ROOT))
        self.assertEqual(
            offenders, [],
            "a production driver manifest names a block driver: %s"
            % ", ".join(offenders))

    def test_production_romfs_packs_no_block_driver(self):
        system = json.loads(
            _read(os.path.join(ROOT, "systems", "production.json")))
        offenders = [name for name in system["drivers"] + system["store"]
                     if self.BLOCK_NAME_RE.search(name)]
        self.assertEqual(
            offenders, [],
            "the production ROMFS packs a block driver: %s"
            % ", ".join(offenders))

    def test_makefile_builds_no_block_driver(self):
        text = _read(os.path.join(ROOT, "Makefile"))
        for needle in ("sd-ioc.mod", "/sd.mod", "BOOT_SD_MOD",
                       "KOS_BLOCK"):
            self.assertNotIn(
                needle, text,
                "the Makefile still builds or boots a block driver: %s"
                % needle)


class TestPlatformBoundary(unittest.TestCase):
    def test_platform_contains_only_constant_configuration_files(self):
        actual = set(os.listdir(PLATFORM_DIR))
        allowed = PLATFORM_FILES | {"public", "private"}
        self.assertEqual(actual, allowed,
                         "platform/zephyr80/ must contain constants and "
                         "configuration only (unexpected: %s, missing: %s)"
                         % (sorted(actual - allowed),
                            sorted(allowed - actual)))
        self.assertEqual(
            set(os.listdir(os.path.join(PLATFORM_DIR, "public"))),
            PLATFORM_PUBLIC_FILES,
            "platform/zephyr80/public/ must hold the board constants")
        self.assertEqual(
            set(os.listdir(os.path.join(PLATFORM_DIR, "private"))),
            PLATFORM_PRIVATE_FILES,
            "platform/zephyr80/private/ must hold the memory/IRQ constants")

    def test_no_target_code_suffixes(self):
        for directory, name in _platform_files():
            self.assertIn(os.path.splitext(name)[1], (".h", ".inc", ".mk"),
                          "platform file %s has a target-code suffix" % name)

    def test_no_executable_content(self):
        for directory, name in _platform_files():
            path = os.path.join(directory, name)
            body = _strip_comments(path, _read(path))
            for lineno, line in enumerate(body.splitlines(), start=1):
                for pattern in EMITTER_PATTERNS:
                    self.assertIsNone(
                        pattern.match(line),
                        "%s:%d looks like target-emitting content: %r"
                        % (name, lineno, line.strip()))

    def test_no_function_like_macros(self):
        for directory, name in _platform_files():
            if not name.endswith(".h"):
                continue
            path = os.path.join(directory, name)
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
        text = _read(os.path.join(ROOT, "kernel", "include", "platform.h"))
        for header in ("hardware.h", "memory.h", "irq.h"):
            self.assertIn('#include "%s"' % header, text)


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
    ("platform/zephyr80/public/hardware.h", "platform/zephyr80/public/hardware.inc"),
    ("platform/zephyr80/private/memory.h", "platform/zephyr80/private/memory.inc"),
    ("platform/zephyr80/private/irq.h", "platform/zephyr80/private/irq.inc"),
) + ACCEPTANCE_CONSTANT_PAIRS


class TestConstantParity(unittest.TestCase):
    """C and assembly must agree on every shared constant: a constant
    that drifts between `hardware.h` and `hardware.inc` is a silent
    memory-map bug."""

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
            os.path.join(ROOT, "abi", "kos", "errno.h"))
        pairs = (("tests/target/include/syscall_entry.inc", "KOS_ENOSYS"),
                 ("tests/target/include/irq_entry.inc", "KOS_IST_ENOSYS"))
        for asm_rel, name in pairs:
            asm = parse_asm_inc(os.path.join(ROOT, asm_rel))
            self.assertIn(name, asm, "%s missing %s" % (asm_rel, name))
            self.assertEqual(evaluate(errno_c["KOS_ENOSYS"], errno_c),
                             evaluate(asm[name], asm),
                             "%s differs from errno.h" % name)


class TestKernelNameComparison(unittest.TestCase):
    """One shared kernel ASCII name comparison: a single implementation
    in `kernel/lib/name.c`, every caller using it with its own semantics
    (VFS/ROMFS fold; ROMFS passes its bounded field width)."""

    def test_single_implementation(self):
        helper = _code_only(os.path.join(ROOT, "kernel", "lib", "name.c"))
        self.assertRegex(
            helper,
            r"\buint8_t\s+kos_ascii_equal\s*\(\s*const\s+char\s*\*",
            "kernel/lib/name.c must define the one shared comparison")

    def test_callers_keep_their_exact_semantics(self):
        # (file, expected selector text) - the two trailing arguments are
        # the field bound and the case-fold selector.
        expected = {
            # The namespace router's publication table and the ROMFS
            # reader are the callers.
            "kernel/vfs/dev.c": "0u, 1u",
            "kernel/romfs/romfs.c": "ROMFS_NAME_SIZE, 1u",
        }
        for rel, selector in expected.items():
            body = _code_only(os.path.join(ROOT, rel))
            calls = re.findall(r"kos_ascii_equal\(.{0,120}", body, re.S)
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
    """User-space sources and EXE programs reach only the public ABI
    (abi/kos) and the SDK (user/sdk); they never see kernel/ or
    platform/."""

    EXE_FILES = (
        "tests/target/process/exetest.c",
        "tests/target/process/exetest_sp.asm",
        "tests/fs/fs_selftest.c",
        "tests/fs/fs_child.c",
        "tests/device/devtest.c",
        "tests/bench/iobench.c",
        "tests/io/iomix.c",
        "tests/io/iomix_child.c",
        "tests/io/iostream.c",
    )
    FORBIDDEN = (
        re.compile(r'#\s*include\s+"konaos/'),
        re.compile(r'#\s*include\s+"kernel/'),
        re.compile(r'#\s*include\s+"platform/'),
        re.compile(r'#\s*include\s+"include/'),
        re.compile(r'#\s*include\s+"device\.h"'),
        re.compile(r'#\s*include\s+"driver\.h"'),
        re.compile(r'#\s*include\s+"vfs\.h"'),
    )

    # The native shell is one of those user programs, but it sits beside
    # the SDK, so it must also stay out of the SDK's own private header
    # (user/sdk/src/kos_internal.h), which only the SDK sources use.
    SHELL_DIR = "user/shell"
    SHELL_FORBIDDEN = FORBIDDEN + (
        re.compile(r'#\s*include\s+"kos_internal\.h"'),
    )

    def _user_files(self):
        for path in _walk("user", SOURCE_SUFFIXES):
            yield path, os.path.relpath(path, ROOT).replace(os.sep, "/")
        for rel in self.EXE_FILES:
            yield os.path.join(ROOT, rel), rel

    def test_shell_sources_reach_the_public_sdk_only(self):
        for path in _walk(self.SHELL_DIR, SOURCE_SUFFIXES):
            body = _strip_comments(path, _read(path))
            for pattern in self.SHELL_FORBIDDEN:
                self.assertIsNone(
                    pattern.search(body),
                    "%s reaches a non-public header"
                    % os.path.relpath(path, ROOT))

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
        kos_mk = _read(os.path.join(ROOT, "mk", "kos.mk"))
        self.assertIn("-I$(EXE_ABI_ROOT) -I$(KOS_INCLUDE)", kos_mk)
        for name in ("mk/kos.mk", "user/sdk/exe.mk"):
            rule = _read(os.path.join(ROOT, name))
            for token in ("-I include", "-Iinclude", "-I platform",
                          "-Iplatform", "-I kernel", "-Ikernel",
                          "drivers/"):
                self.assertNotIn(
                    token, rule,
                    "%s offers a non-public path: %r" % (name, token))
        # The exe_program declarations name no include/ or platform/ rule.
        makefile = _read(os.path.join(ROOT, "Makefile"))
        for line in makefile.splitlines():
            if "exe_program" in line:
                for token in ("include/", "platform/"):
                    self.assertNotIn(
                        token, line,
                        "a exe_program line names %r" % token)

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
                if pattern == "**/build/**" and name == "build":
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

    def test_build_ignore_patterns_reach_only_build_output(self):
        # `**/build/**` ignores every program's build directory, the tree's
        # and each one under UserSoftware/, so no source may live in a
        # directory named `build`; the `build-*` variants stay anchored at
        # the root.
        import subprocess
        for pattern in self._gitignore_patterns():
            if "build" in pattern and pattern != "**/build/**":
                self.assertTrue(pattern.startswith("/"),
                                "build ignore pattern is not root-anchored: %r"
                                % pattern)
        try:
            result = subprocess.run(
                ["git", "ls-files"], cwd=ROOT, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, text=True)
        except OSError:
            self.skipTest("git not available")
        if result.returncode != 0:
            self.skipTest("not a git work tree")
        under_build = [path for path in result.stdout.splitlines()
                       if "build" in path.split("/")[:-1]]
        self.assertEqual(under_build, [],
                         "tracked files under a directory named build: %s"
                         % under_build)

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


class TestSystemComposition(unittest.TestCase):
    def _image_systems(self):
        """The system each image target selects, as target -> name."""
        text = _read(os.path.join(ROOT, "Makefile"))
        pairs = {}
        target = None
        for line in text.splitlines():
            if line and not line.startswith(("\t", " ")):
                match = re.match(r"^([A-Za-z][A-Za-z0-9-]*):", line)
                target = match.group(1) if match else None
            elif line.startswith("\t") and target is not None:
                for name in re.findall(r"SYSTEM=([a-z0-9-]+)", line):
                    pairs.setdefault(name, set()).add(target)
        return pairs

    def test_every_system_file_is_selected_by_an_image_target(self):
        systems = sorted(name[:-5] for name in os.listdir(
            os.path.join(ROOT, "systems")) if name.endswith(".json"))
        selected = set(self._image_systems())
        # The default build is a system too (SYSTEM ?= production).
        text = _read(os.path.join(ROOT, "Makefile"))
        match = re.search(r"^SYSTEM\s*\?=\s*([a-z0-9-]+)", text, re.M)
        self.assertIsNotNone(match, "no SYSTEM default in the Makefile")
        selected.add(match.group(1))
        self.assertEqual(sorted(selected), systems,
                         "image targets and systems/ disagree")

    def test_every_selected_system_has_a_description(self):
        systems = set(name[:-5] for name in os.listdir(
            os.path.join(ROOT, "systems")) if name.endswith(".json"))
        for name in self._image_systems():
            self.assertIn(name, systems,
                          "SYSTEM=%s has no systems/%s.json" % (name, name))

    def test_the_suffix_table_precedes_the_immediate_assignment(self):
        """The image-name suffix is read by an immediate assignment: it
        must follow the rows it reads, or every image target resolves to
        the unsuffixed production image name (the image matrix catches
        it, this names it)."""
        text = _read(os.path.join(ROOT, "Makefile"))
        rows = [index for index, line in enumerate(text.splitlines())
                if line.startswith("SYSTEM_SUFFIX.")]
        uses = [index for index, line in enumerate(text.splitlines())
                if line.startswith("PK_SUFFIX :=")]
        self.assertTrue(rows, "no SYSTEM_SUFFIX rows in the Makefile")
        self.assertTrue(uses, "no PK_SUFFIX assignment in the Makefile")
        self.assertGreater(min(uses), max(rows),
                           "PK_SUFFIX is assigned before the suffix table")

    def test_the_store_is_built_from_the_system_file(self):
        # The store manifest, the boot manifest and the resolved driver
        # manifests are generated inputs: the Makefile builds them with
        # tools/systemgen.py and the packer reads the generated store.
        text = _read(os.path.join(ROOT, "Makefile"))
        self.assertIn("tools/systemgen.py $(SYSTEM_JSON)", text)
        self.assertIn("$(BUILD)/store.json", text)
        self.assertIn("$(BUILD)/drivers/", text)


if __name__ == "__main__":
    unittest.main()
