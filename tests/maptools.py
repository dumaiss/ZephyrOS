#!/usr/bin/env python3
"""Shared host-test helpers for synthetic linker maps and constant parsing.

Extracted from tests/test_m1.py so kept test modules no longer depend on
the retired M1 acceptance module.

  * evaluate/parse_c_header/parse_asm_inc: tiny C and assembly constant
    readers used against include/ and platform/ headers.
  * make_map: builder for a synthetic ROMFS-boot linker map that mapcheck
    accepts, used by the layout regression tests.
  * layout_context: apply a generated memory.inc/irq.inc for one check.

Run with: make test
"""

import os
import re
import sys
import unittest
import contextlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import mapcheck  # noqa: E402

# Layout globals apply_layout()/apply_irq_layout() mutate; snapshot and
# restore them around tests that need a specific generated memory.inc so
# synthetic-map tests keep the committed defaults.
_LAYOUT_GLOBALS = (
    "PAGE_ZERO_END", "RESET_TOP", "BANK7_BASE", "BANK7_RUNTIME_END",
    "BANK7_TOP", "COMMON_BASE", "COMMON_CODE_END", "STACK_BASE",
    "WINDOW_BASE", "WINDOW_DATA_BASE",
    "SERVICE_STACK_BASE", "SERVICE_STACK_TOP", "SERVICE_STACK_SIZE",
    "IRQ_ISR_STACK_BASE", "IRQ_ISR_STACK_TOP", "IRQ_ISR_STACK_SIZE",
    "GATE_STACK_SIZE", "OWNED_AREAS", "DRIVER_ARENA_BASE",
    "DRIVER_ARENA_END", "DRIVER_GRANULE_SIZE", "DRIVER_GRANULES",
    "DATA_BASE", "FIXED_CODE_END", "FIXED_REQUIRED_BYTES",
    "FIXED_OVERFLOW_BYTES", "FIXED_STACKS_BYTES", "FIXED_ROOM_BYTES",
    "COMMON_CEILING", "COMMON_M10_RESERVATION",
    "ROMFS_STAGING_SIZE", "ROM_ACCESS_BASE",
    "ROM_ACCESS_SIZE", "IRQ_VECTOR_BASE", "IRQ_VECTOR_SIZE",
    "IRQ_STATE_BASE", "IRQ_STACK_BASE", "IRQ_STACK_TOP", "IRQ_STACK_SIZE",
)


@contextlib.contextmanager
def layout_context(layout_inc, irq_inc):
    """Apply a layout for one check and restore the module defaults."""
    saved = {name: getattr(mapcheck, name) for name in _LAYOUT_GLOBALS}
    try:
        mapcheck.apply_layout(mapcheck.parse_inc(layout_inc), layout_inc)
        mapcheck.apply_irq_layout(mapcheck.parse_inc(irq_inc))
        yield
    finally:
        for name, value in saved.items():
            setattr(mapcheck, name, value)

PLATFORM_DIR = os.path.join(ROOT, "platform", "zephyr80")
TARGET_TEST_INC = os.path.join(ROOT, "tests", "target", "include")
HARDWARE_H = os.path.join(PLATFORM_DIR, "hardware.h")
HARDWARE_INC = os.path.join(PLATFORM_DIR, "hardware.inc")
MEMORY_H = os.path.join(PLATFORM_DIR, "memory.h")
MEMORY_INC = os.path.join(PLATFORM_DIR, "memory.inc")
IRQ_H = os.path.join(PLATFORM_DIR, "irq.h")
IRQ_INC = os.path.join(PLATFORM_DIR, "irq.inc")
SELFTEST_H = os.path.join(TARGET_TEST_INC, "selftest.h")
SELFTEST_INC = os.path.join(TARGET_TEST_INC, "selftest.inc")
IRQ_SELFTEST_H = os.path.join(TARGET_TEST_INC, "irq_selftest.h")
IRQ_SELFTEST_INC = os.path.join(TARGET_TEST_INC, "irq_selftest.inc")

REQUIRED_HARDWARE_CONSTANTS = (
    "ZOS_MEMCTL_PORT",
    "ZOS_SIO0A_DATA", "ZOS_SIO0A_CTRL",
    "ZOS_SIO0B_DATA", "ZOS_SIO0B_CTRL",
    "ZOS_BANK_MASK", "ZOS_LATCH_NON_BANK_MASK",
    "ZOS_MEM_MODE0", "ZOS_MEM_MODE1", "ZOS_MODE_MASK",
    "ZOS_ROM_PAGE_SHIFT", "ZOS_ROM_PAGE_MASK",
    "ZOS_MODE_BOOT", "ZOS_MODE_FLAT",
    "ZOS_MODE_APPLICATION", "ZOS_MODE_OS",
    "ZOS_SIO_RR0_RX_AVAILABLE", "ZOS_SIO_RR0_TX_EMPTY", "ZOS_SIO_RR0_CTS",
    "ZOS_SIO_TX_READY",
    "ZOS_SIO_WR0_CHANNEL_RESET", "ZOS_SIO_WR4_X16_1STOP_NOPARITY",
    "ZOS_SIO_WR3_RX8_NO_AUTO_ENABLES", "ZOS_SIO_WR5_TX8_DTR_RTS_ASSERT",
    "ZOS_SIO_WR1_INT_DISABLED", "ZOS_SIO_TX_TIMEOUT",
    "ZOS_CTC0_PORT", "ZOS_CTC0_CONTROL_TEST",
    "ZOS_CTC0_TIME_CONSTANT", "ZOS_CTC0_RESET",
)

REQUIRED_MEMORY_CONSTANTS = (
    "ZOS_RESET_TOP", "ZOS_PAGE_ZERO_END",
    "ZOS_BANK7_BASE", "ZOS_REV13_WINDOW_BASE", "ZOS_BANK7_RUNTIME_END",
    "ZOS_IRQ_ISR_STACK_BASE", "ZOS_IRQ_ISR_STACK_TOP",
    "ZOS_IRQ_ISR_STACK_SIZE",
    "ZOS_SERVICE_STACK_BASE", "ZOS_SERVICE_STACK_TOP",
    "ZOS_SERVICE_STACK_SIZE",
    "ZOS_DRIVER_GRANULE_SIZE",
    "ZOS_COMMON_BASE", "ZOS_COMMON_CODE_END", "ZOS_COMMON_STACK_BASE",
    "ZOS_COMMON_CEILING", "ZOS_COMMON_M10_RESERVATION",
    "ZOS_STACK_INIT_SP", "ZOS_GATE_STACK_SIZE",
    "ZOS_ROM_PAGE_SIZE", "ZOS_ROM_PAGE_COUNT", "ZOS_BOOT_IMAGE_SIZE",
    "ZOS_ROM_IMAGE_SIZE",
    "ZOS_ROMFS_FIRST_PAGE", "ZOS_ROMFS_PAGE_COUNT",
    "ZOS_ROMFS_WINDOW_SIZE", "ZOS_ROMFS_CAPACITY",
    "ZOS_ROMFS_STAGING_SIZE", "ZOS_ROM_ACCESS_BASE", "ZOS_ROM_ACCESS_SIZE",
)

REQUIRED_IRQ_CONSTANTS = (
    "ZOS_IRQ_VECTOR_BASE", "ZOS_IRQ_VECTOR_SIZE",
    "ZOS_IRQ_STATE_BASE", "ZOS_IRQ_STATE_HEADER_SIZE",
    "ZOS_IRQ_STACK_BASE", "ZOS_IRQ_STACK_TOP", "ZOS_IRQ_STACK_SIZE",
    "ZOS_IM2_I_VALUE", "ZOS_CTC0_VECTOR_BASE",
)


# --- tiny constant-expression evaluator (hex/dec, | & << >> + - ~ parens) ---

class _Parser:
    def __init__(self, text, env):
        self.text = text
        self.env = env
        self.pos = 0
        self.length = len(text)

    def _skip(self):
        while self.pos < self.length and self.text[self.pos].isspace():
            self.pos += 1

    def _peek(self):
        self._skip()
        if self.pos >= self.length:
            return ""
        for tok in ("<<", ">>"):
            if self.text.startswith(tok, self.pos):
                return tok
        return self.text[self.pos]

    def _accept(self, tok):
        self._skip()
        if self.text.startswith(tok, self.pos):
            self.pos += len(tok)
            return True
        return False

    def parse(self):
        value = self.parse_or()
        self._skip()
        if self.pos != self.length:
            raise ValueError("trailing tokens in %r" % self.text)
        return value

    def parse_or(self):
        value = self.parse_and()
        while self._accept("|"):
            value |= self.parse_and()
        return value

    def parse_and(self):
        value = self.parse_shift()
        while self._accept("&"):
            value &= self.parse_shift()
        return value

    def parse_shift(self):
        value = self.parse_add()
        while True:
            if self._accept("<<"):
                value = (value << self.parse_add()) & 0xFFFFFFFF
            elif self._accept(">>"):
                value >>= self.parse_add()
            else:
                return value

    def parse_add(self):
        value = self.parse_unary()
        while True:
            if self._accept("+"):
                value += self.parse_unary()
            elif self._accept("-"):
                value -= self.parse_unary()
            else:
                return value

    def parse_unary(self):
        if self._accept("~"):
            return ~self.parse_unary() & 0xFFFFFFFF
        if self._accept("-"):
            return -self.parse_unary()
        return self.parse_primary()

    def parse_primary(self):
        self._skip()
        match = re.match(r"(0[xX][0-9a-fA-F]+|\d+)[uUlL]*", self.text[self.pos:])
        if match:
            self.pos += len(match.group(0))
            return int(match.group(1), 0)
        match = re.match(r"[A-Za-z_]\w*", self.text[self.pos:])
        if match:
            name = match.group(0)
            self.pos += len(name)
            if name not in self.env:
                raise KeyError("undefined constant %s" % name)
            return evaluate(self.env[name], self.env)
        if self._accept("("):
            value = self.parse_or()
            if not self._accept(")"):
                raise ValueError("unbalanced parentheses")
            return value
        raise ValueError("unexpected token in %r at %d" % (self.text, self.pos))


def evaluate(text, env):
    text = str(text).strip()
    return _Parser(text, env).parse()


def _strip_comment(text, marker):
    index = text.find(marker)
    return text[:index] if index >= 0 else text


def parse_c_header(path):
    env = {}
    with open(path) as handle:
        for line in handle:
            line = _strip_comment(line, "//").strip()
            match = re.match(r"#define\s+([A-Za-z_]\w*)\s+(.+)$", line)
            if match:
                env[match.group(1)] = _strip_comment(match.group(2),
                                                     "/*").strip()
    return env


def parse_asm_inc(path):
    env = {}
    with open(path) as handle:
        for line in handle:
            line = _strip_comment(line, ";").strip()
            match = re.match(r"([A-Za-z_]\w*)\s+=\s+(.+)$", line)
            if match:
                env[match.group(1)] = match.group(2).strip()
    return env

def make_map(overrides=None, **kw):
    """Build a synthetic map resembling the real layout, then apply edits.

    The fixture is a *ROMFS-boot* image (ZOS_ROMFS_BOOT=1): the M9 VFS
    symbols are present and the embedded boot catalog is not, which is the
    only combination mapcheck accepts.  Tests that need the
    embedded-catalog variant pass the catalog symbols explicitly, and the
    two boot modes are mutually exclusive (see
    work/25-m9-vfs-bootstrap-romfs/correction-01).
    """
    areas = {
        "_RESET0": (0x0000, 0x26),
        "_PAGE_ZERO0": (0x0030, 0x03),
        "_CODE": (0x2000, 0x6E),
        "_GSINIT": (0x206E, 0x00),
        "_GSFINAL": (0x206E, 0x01),
        # Fixed runtime data and the cold half.  The synthetic geometry
        # mirrors tools/mapcheck.py's host defaults: the fixed data base
        # is 5A00h, the cold code ends at the arena start 6200h, and the
        # cold data base saturates the half at D800h.
        "_DATA": (0x5A00, 0x00),
        "_INITIALIZED": (0x5A00, 0x00),
        "_WINDOW": (0x6000, 0x200),
        "_WINDOW_DATA": (0xD800, 0x00),
        "_IRQ_ISR_STACK": (0x0000, 0x0100),
        "_COMMON": (0xE000, 0x100),
        "_COMMON_DATA": (0xE100, 0x43),
        "_ROM_ACCESS_STUB0": (0x0000, 0x0A),
    }
    symbols = {
        "s__CODE": 0x2000, "l__CODE": 0x6E,
        "s__HOME": 0x206E, "l__HOME": 0,
        "s__INITIALIZER": 0x206E, "l__INITIALIZER": 0,
        "s__GSINIT": 0x206E, "l__GSINIT": 0,
        "s__GSFINAL": 0x206E, "l__GSFINAL": 1,
        "s__DATA": 0x5A00, "l__DATA": 0,
        "s__INITIALIZED": 0x5A00, "l__INITIALIZED": 0,
        "s__WINDOW": 0x6000, "l__WINDOW": 0x200,
        "s__WINDOW_DATA": 0xD800, "l__WINDOW_DATA": 0,
        "s__COMMON": 0xE000, "l__COMMON": 0x100,
        "s__COMMON_DATA": 0xE100, "l__COMMON_DATA": 0x43,
        "s__RESET": 0x0000, "l__RESET": 0x26,
        "_reset": 0x0000, "_reset_end": 0x0026,
        "_zos_rst30_vector": 0x0030, "_zos_rst30_end": 0x0033,
        # Cold half: the boot/idle path runs only with P = 0.
        "_kernel_main": 0x6000,
        "_kernel_syscall_dispatch": 0x6020,
        "zos_syscall_dispatch": 0x2100,
        "_boot_common_entry": 0xE000, "__zos_crt0_init": 0xE02F,
        "__zos_mode_fault": 0xE080, "zos_enter_os_mode": 0xE090,
        "zos_leave_os_mode": 0xE0A0, "zos_os_continuation": 0xE09C,
        "zos_syscall_entry": 0xE0A8,
        "_platform_memctl_read": 0xE0B0, "_platform_memctl_write": 0xE0B4,
        "_platform_bank_get": 0xE0B8, "_platform_bank_set": 0xE0BC,
        "_early_console_init": 0xE0C0, "_early_console_putc": 0xE0E0,
        "_early_console_puts": 0xE0F0, "_panic": 0xE0F8,
        "irq_ctc0_stub": 0xE110, "irq_ff_unexpected": 0xE130,
        "irq_common_entry": 0xE140, "_irq_arch_lock": 0xE190,
        "_irq_arch_unlock": 0xE198, "_irq_arch_init": 0xE1A0,
        "_irq_dispatch": 0x2200, "_irq_kernel_code_end": 0x206E,
        "_zos_upcall": 0x2400,
        "zos_service_stack": 0x5D00, "zos_service_stack_top": 0x5F00,
        "_device_registry_init": 0x2300, "_device_register": 0x2320,
        "_device_get": 0x2340, "_device_find": 0x2360,
        "_device_interface": 0x2370,
        "_device_registry_checkpoint": 0x2380,
        "_device_registry_rollback": 0x23A0,
        "_driver_init_static": 0x23C0,
        "_console_bind": 0x2400, "_console_bind_defaults": 0x2430,
        "_console_putc": 0x2460, "_console_write": 0x2480,
        "_console_poll": 0x24A0, "_console_getc": 0x24B0,
        "_device_table": 0xD820, "_console_in_handle": 0xD8A0,
        "_console_out_handle": 0xD8A1, "_boot_drivers": 0xD8C0,
        "_module_loader_init": 0x2700,
        "_module_load_source": 0x2740,
        "_module_count": 0x27A0,
        "_module_free_granules": 0x27B0, "_module_failed_count": 0x27C0,
        "_module_get": 0x27D0, "_module_code_allowed": 0x27E0,
        "_zmod_copy_image": 0x2880,
        "_zmod_validate_source": 0x28E0,
        "_zmod_zero_bss": 0x28A0, "_zmod_apply_relocations": 0x28C0,
        "_zmod_zero_allocation": 0x2900,
        "_romfs_init": 0x2A00, "_romfs_read": 0x2B00,
        "_romfs_lookup_ci": 0x2B10, "_romfs_entry_at": 0x2B20,
        "_romfs_resource_size": 0x2BA0,
        "_romfs_resource_read": 0x2BC0,
        "_rom_access_copy": 0xE1B0,
        "_rom_access_staging": 0xE200,
        "rom_access_saved_latch": 0xE280,
        "rom_access_saved_iff": 0xE281,
        "rom_access_page": 0xE282,
        "rom_access_offset": 0xE283,
        "rom_access_length": 0xE285,
        "rom_access_busy": 0xE287,
        "rom_access_trampoline": 0xEEFE,
        "rom_access_sync": 0xEF00,
        "rom_access_sync_end": 0xEF08,
        "_irq_registry_checkpoint": 0x2920,
        "_irq_registry_rollback": 0x2940,
        "_irq_registry_set_owner": 0x2960,
        "__driver_arena_start": 0x6200, "__driver_arena_end": 0x6A00,
        "irq_vector_page": 0xFD00, "irq_vector_guard": 0xFE00,
        "irq_saved_latch": 0xFE01, "irq_saved_sp": 0xFE02,
        "irq_common_sp": 0xFE04, "_irq_active_source": 0xFE06,
        "irq_transition_stack": 0xFE80, "irq_transition_stack_top": 0xFF00,
        "irq_isr_stack": 0x5F00, "irq_isr_stack_top": 0x6000,
        "zos_gate_saved_sp": 0xE100, "zos_gate_saved_latch": 0xE102,
        "zos_gate_result_hl": 0xE103, "zos_gate_result_a": 0xE105,
        "zos_gate_result_c": 0xE106,
        "zos_gate_stack": 0xE107, "zos_gate_stack_top": 0xE207,
        # M6 process gates (common), copy state (common), and bank-7
        # process/ZEX code and data.
        "_zos_process_launch": 0xE300, "_zos_process_resume": 0xE310,
        "zos_process_return_trap": 0xE320,
        "_zos_process_page_zero_vector": 0xE330,
        "_zos_bank_copy": 0xE340,
        "_zos_user_copy_staging": 0xE370,
        "zos_copy_op": 0xE3B0, "zos_copy_bank": 0xE3B1,
        "zos_copy_addr": 0xE3B2, "zos_copy_buffer": 0xE3B4,
        "zos_copy_length": 0xE3B6, "zos_copy_saved_sp": 0xE3B8,
        "zos_copy_saved_latch": 0xE3BA, "zos_copy_saved_iff": 0xE3BB,
        "zos_copy_stack": 0xE3BC, "zos_copy_stack_top": 0xE3CC,
        "zos_proc_cont": 0xE3D0, "zos_proc_ix": 0xE3D8,
        "zos_proc_latch": 0xE3E0,
        "zos_proc_iff": 0xE3E4, "zos_proc_index": 0xE3E8,
        "zos_proc_status": 0xE3E9, "zos_proc_bank": 0xE3EB,
        "zos_proc_entry": 0xE3EC,
        "zos_proc_gate_sp": 0xE470, "zos_proc_gate_latch": 0xE478,
        "zos_proc_gate_iff": 0xE47C,
        "zos_proc_frames": 0xE48E, "zos_proc_frames_end": 0xE51E,
        "_process_exec": 0x2C00, "_process_prepare": 0x2C20,
        "_process_launch": 0x2C40,
        "_process_exit": 0x2C80,
        "_process_abort": 0x2CA0, "_process_alloc_bank": 0x2CC0,
        "_process_slot_alloc": 0x2CE0, "_process_slot_release": 0x2D00,
        "_zex_validate_header": 0x2D40,
        "_process_table": 0xD900, "_process_running_pid": 0xDD00,
        "_process_last_status": 0xDD01,
        # Bank-7 runtime data: the SYS_EXEC path buffer is not
        # transition-critical, so it lives with the rest of `_DATA`.
        "_zos_process_path_scratch": 0xDD02,
        # M7 diagnostic services and shared dispatcher helpers (bank-7
        # kernel code; no new common state).
        "_zos_diag_mem_read": 0x2E00, "_zos_diag_mem_write": 0x2E40,
        "_zos_diag_io_in": 0x2E80, "_zos_diag_io_out": 0x2EA0,
        "_zos_diag_bank_info": 0x2EC0, "_zos_diag_system_info": 0x2EE0,
        "_zos_diag_hw_in": 0x2010, "_zos_diag_hw_out": 0x2016,
        "_zos_syscall_success": 0x2632, "_zos_syscall_error": 0x263D,
        "_zos_syscall_block_ok": 0x264C,
        # M9 VFS core, ROMFS provider and generated mount table.
        "_vfs_init": 0x3000, "_vfs_mount_all": 0x3020,
        "_vfs_phase": 0x3040, "_vfs_register_filesystem": 0x3060,
        "_vfs_find_provider": 0x3080,
        "_vfs_provider_checkpoint": 0x3090,
        "_vfs_provider_rollback": 0x3098,
        "_vfs_provider_set_owner": 0x30A0,
        "_vfs_mount_resolve": 0x3100, "_vfs_resolve_path": 0x3120,
        "_vfs_is_mountpoint": 0x3140, "_vfs_mount_is_root": 0x3160,
        "_vfs_mount_is_root_level": 0x3170,
        "_vfs_open": 0x3180, "_vfs_read": 0x31A0, "_vfs_write": 0x31C0,
        "_vfs_seek": 0x31C8,
        "_vfs_sync": 0x31D0, "_vfs_close": 0x31E0,
        "_vfs_stat": 0x3200, "_vfs_mkdir": 0x3210,
        "_vfs_rmdir": 0x3220, "_vfs_unlink": 0x3230,
        "_vfs_rename": 0x3238,
        "_vfs_opendir": 0x3240, "_vfs_readdir": 0x3260,
        "_vfs_closedir": 0x3280, "_vfs_file_source": 0x32A0,
        "_romfs_provider": 0xDA00, "_vfs_mount_table": 0xDA20,
        "_vfs_mount_count": 0xDA40, "_vfs_mount_state": 0xDA50,
    }
    if kw.get("areas") is not None:
        areas = dict(areas)
        for name, value in kw["areas"].items():
            if value is None:
                areas.pop(name, None)
            else:
                areas[name] = value
    if kw.get("drop_prefixes"):
        # Remove every symbol carrying one of these prefixes before the
        # explicit overrides are applied, so a test can still add one back.
        prefixes = tuple(kw["drop_prefixes"])
        symbols = {name: addr for name, addr in symbols.items()
                   if not name.startswith(prefixes)}
    if kw.get("symbols") is not None:
        symbols = dict(symbols)
        for name, value in kw["symbols"].items():
            if value is None:
                symbols.pop(name, None)
            else:
                symbols[name] = value
    if overrides:
        overrides(areas, symbols)
    lines = []
    for name, (addr, size) in sorted(areas.items()):
        lines.append("%-20s %08X    %08X =           0. bytes (REL,CON)"
                     % (name, addr, size))
    for name, addr in sorted(symbols.items()):
        lines.append("     %08X  %-32s zosmap" % (addr, name))
    return "\n".join(lines) + "\n"
