#!/usr/bin/env python3
"""Host-side M1 tests: constant parity between C and assembly
representations, latch mask/bank formulas, map/layout checking, and
regression checks on the real build artifacts.

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
    "PAGE_ZERO_END", "RESET_TOP", "BANK7_BASE", "BANK7_LOAD_END",
    "BANK7_RUNTIME_END", "COMMON_BASE", "COMMON_CODE_END", "STACK_BASE",
    "IRQ_ISR_STACK_BASE", "IRQ_ISR_STACK_TOP", "IRQ_ISR_STACK_SIZE",
    "GATE_STACK_SIZE", "OWNED_AREAS", "DRIVER_ARENA_BASE",
    "DRIVER_ARENA_END", "DRIVER_GRANULE_SIZE", "DRIVER_GRANULES",
    "DRIVER_ARENA_DERIVED", "DRIVER_ARENA_MIN_SIZE",
    "DRIVER_ARENA_MIN_GRANULES", "ROMFS_STAGING_SIZE", "ROM_ACCESS_BASE",
    "ROM_ACCESS_SIZE", "IRQ_VECTOR_BASE", "IRQ_VECTOR_SIZE",
    "IRQ_STATE_BASE", "IRQ_STACK_BASE", "IRQ_STACK_TOP", "IRQ_STACK_SIZE",
)


@contextlib.contextmanager
def layout_context(layout_inc, irq_inc):
    """Apply a layout for one check and restore the module defaults."""
    saved = {name: getattr(mapcheck, name) for name in _LAYOUT_GLOBALS}
    try:
        mapcheck.apply_layout(mapcheck.parse_inc(layout_inc))
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
    "ZOS_BANK7_BASE", "ZOS_BANK7_LOAD_END", "ZOS_BANK7_RUNTIME_END",
    "ZOS_IRQ_ISR_STACK_BASE", "ZOS_IRQ_ISR_STACK_TOP",
    "ZOS_IRQ_ISR_STACK_SIZE",
    "ZOS_DRIVER_ARENA_BASE", "ZOS_DRIVER_ARENA_END",
    "ZOS_DRIVER_GRANULE_SIZE", "ZOS_DRIVER_GRANULES",
    "ZOS_DRIVER_ARENA_DERIVED",
    "ZOS_DRIVER_ARENA_MIN_SIZE", "ZOS_DRIVER_ARENA_MIN_GRANULES",
    "ZOS_COMMON_BASE", "ZOS_COMMON_CODE_END", "ZOS_COMMON_STACK_BASE",
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
    for line in open(path):
        line = _strip_comment(line, "//").strip()
        match = re.match(r"#define\s+([A-Za-z_]\w*)\s+(.+)$", line)
        if match:
            env[match.group(1)] = _strip_comment(match.group(2), "/*").strip()
    return env


def parse_asm_inc(path):
    env = {}
    for line in open(path):
        line = _strip_comment(line, ";").strip()
        match = re.match(r"([A-Za-z_]\w*)\s+=\s+(.+)$", line)
        if match:
            env[match.group(1)] = match.group(2).strip()
    return env


class TestConstantParity(unittest.TestCase):
    def setUp(self):
        self.c = parse_c_header(HARDWARE_H)
        self.asm = parse_asm_inc(HARDWARE_INC)

    def test_required_constants_present_in_both(self):
        for name in REQUIRED_HARDWARE_CONSTANTS:
            self.assertIn(name, self.c, "hardware.h missing %s" % name)
            self.assertIn(name, self.asm, "hardware.inc missing %s" % name)

    def test_shared_constants_have_identical_values(self):
        for name in sorted(set(self.c) & set(self.asm)):
            cv = evaluate(self.c[name], self.c)
            av = evaluate(self.asm[name], self.asm)
            self.assertEqual(cv, av,
                             "%s differs: C %s = %d, asm %s = %d"
                             % (name, self.c[name], cv, self.asm[name], av))

    def test_moved_constants_not_duplicated(self):
        moved = ("ZOS_IM2_I_VALUE", "ZOS_CTC0_VECTOR_BASE")
        for name in moved:
            self.assertNotIn(name, self.c, "hardware.h still defines %s" % name)
            self.assertNotIn(name, self.asm, "hardware.inc still defines %s" % name)


class TestMemoryConstantParity(unittest.TestCase):
    """memory.h and memory.inc must stay value-identical."""

    def setUp(self):
        self.c = parse_c_header(MEMORY_H)
        self.asm = parse_asm_inc(MEMORY_INC)

    def test_required_constants_present_in_both(self):
        for name in REQUIRED_MEMORY_CONSTANTS:
            self.assertIn(name, self.c, "memory.h missing %s" % name)
            self.assertIn(name, self.asm, "memory.inc missing %s" % name)

    def test_shared_constants_have_identical_values(self):
        for name in sorted(set(self.c) & set(self.asm)):
            cv = evaluate(self.c[name], self.c)
            av = evaluate(self.asm[name], self.asm)
            self.assertEqual(cv, av,
                             "%s differs: C %s = %d, asm %s = %d"
                             % (name, self.c[name], cv, self.asm[name], av))

    def test_irq_wiring_constants_not_duplicated(self):
        for name in ("ZOS_IRQ_VECTOR_BASE", "ZOS_IRQ_STATE_BASE",
                     "ZOS_IRQ_STACK_BASE", "ZOS_IM2_I_VALUE",
                     "ZOS_CTC0_VECTOR_BASE"):
            self.assertNotIn(name, self.c, "memory.h still defines %s" % name)
            self.assertNotIn(name, self.asm, "memory.inc still defines %s" % name)


class TestIrqConstantParity(unittest.TestCase):
    """irq.h and irq.inc must stay value-identical."""

    def setUp(self):
        self.c = parse_c_header(IRQ_H)
        self.asm = parse_asm_inc(IRQ_INC)

    def test_required_constants_present_in_both(self):
        for name in REQUIRED_IRQ_CONSTANTS:
            self.assertIn(name, self.c, "irq.h missing %s" % name)
            self.assertIn(name, self.asm, "irq.inc missing %s" % name)

    def test_shared_constants_have_identical_values(self):
        for name in sorted(set(self.c) & set(self.asm)):
            cv = evaluate(self.c[name], self.c)
            av = evaluate(self.asm[name], self.asm)
            self.assertEqual(cv, av,
                             "%s differs: C %s = %d, asm %s = %d"
                             % (name, self.c[name], cv, self.asm[name], av))


class TestSelftestConstantParity(unittest.TestCase):
    """selftest.h and selftest.inc must stay value-identical."""

    def test_selftest_constants_match(self):
        c = parse_c_header(SELFTEST_H)
        asm = parse_asm_inc(SELFTEST_INC)
        self.assertTrue(set(c), "selftest.h parsed no constants")
        for name in sorted(c):
            self.assertIn(name, asm, "selftest.inc missing %s" % name)
            cv = evaluate(c[name], c)
            av = evaluate(asm[name], asm)
            self.assertEqual(cv, av, "%s differs: C=%d asm=%d" % (name, cv, av))

    def test_enosys_matches_errno_header(self):
        errno_c = parse_c_header(os.path.join(ROOT, "include", "zephyros", "errno.h"))
        asm = parse_asm_inc(SELFTEST_INC)
        self.assertIn("ZOS_ENOSYS", asm, "selftest.inc missing ZOS_ENOSYS")
        cv = evaluate(errno_c["ZOS_ENOSYS"], errno_c)
        av = evaluate(asm["ZOS_ENOSYS"], asm)
        self.assertEqual(cv, av, "ZOS_ENOSYS differs: errno.h=%d selftest.inc=%d" % (cv, av))


class TestIrqSelftestConstantParity(unittest.TestCase):
    """irq_selftest.h and irq_selftest.inc must stay value-identical."""

    def test_irq_selftest_constants_match(self):
        c = parse_c_header(IRQ_SELFTEST_H)
        asm = parse_asm_inc(IRQ_SELFTEST_INC)
        self.assertTrue(set(c), "irq_selftest.h parsed no constants")
        for name in sorted(c):
            self.assertIn(name, asm, "irq_selftest.inc missing %s" % name)
            cv = evaluate(c[name], c)
            av = evaluate(asm[name], asm)
            self.assertEqual(cv, av, "%s differs: C=%d asm=%d" % (name, cv, av))

    def test_irq_selftest_enosys_matches_errno_header(self):
        errno_c = parse_c_header(os.path.join(ROOT, "include", "zephyros", "errno.h"))
        asm = parse_asm_inc(IRQ_SELFTEST_INC)
        self.assertIn("ZOS_IST_ENOSYS", asm, "irq_selftest.inc missing ZOS_IST_ENOSYS")
        cv = evaluate(errno_c["ZOS_ENOSYS"], errno_c)
        av = evaluate(asm["ZOS_IST_ENOSYS"], asm)
        self.assertEqual(cv, av,
                         "ZOS_ENOSYS differs: errno.h=%d irq_selftest.inc=%d"
                         % (cv, av))


class TestLatchFormulas(unittest.TestCase):
    """Latch semantics the banking primitives must implement."""

    def setUp(self):
        self.c = parse_c_header(HARDWARE_H)
        self.v = {name: evaluate(self.c[name], self.c) for name in self.c}

    def compose(self, page, mode, bank):
        return ((page & 7) << self.v["ZOS_ROM_PAGE_SHIFT"]) | mode | (bank & 7)

    def test_mode_composition(self):
        self.assertEqual(self.compose(0, self.v["ZOS_MODE_APPLICATION"], 0), 0x10)
        self.assertEqual(self.compose(0, self.v["ZOS_MODE_OS"], 0), 0x18)
        self.assertEqual(self.compose(1, self.v["ZOS_MODE_BOOT"], 7), 0x27)

    def test_bank_identity_preserved_across_modes(self):
        for bank in range(7):
            app = self.v["ZOS_MODE_APPLICATION"] | bank
            osv = self.v["ZOS_MODE_OS"] | bank
            self.assertEqual(app & self.v["ZOS_BANK_MASK"], bank)
            self.assertEqual(osv & self.v["ZOS_BANK_MASK"], bank)
            self.assertEqual(osv & self.v["ZOS_MODE_MASK"],
                             self.v["ZOS_MODE_OS"])
            self.assertEqual(app & self.v["ZOS_MODE_MASK"],
                             self.v["ZOS_MODE_APPLICATION"])
            # Round trip 10h|N -> 18h|N -> 10h|N keeps the identity.
            back = (osv & self.v["ZOS_BANK_MASK"]) | self.v["ZOS_MODE_APPLICATION"]
            self.assertEqual(back, app)

    def test_non_bank_mask_covers_all_other_fields(self):
        self.assertEqual(self.v["ZOS_LATCH_NON_BANK_MASK"],
                         (~self.v["ZOS_BANK_MASK"]) & 0xFF)
        self.assertEqual(self.v["ZOS_BANK_MASK"] & self.v["ZOS_LATCH_NON_BANK_MASK"], 0)

    def test_bank_set_preserves_fields(self):
        latch = 0xA0 | self.v["ZOS_MODE_OS"] | 2  # page 5, OS, bank 2
        for bank in range(7):
            new = (latch & self.v["ZOS_LATCH_NON_BANK_MASK"]) | bank
            self.assertEqual(new & self.v["ZOS_LATCH_NON_BANK_MASK"],
                             latch & self.v["ZOS_LATCH_NON_BANK_MASK"])
            self.assertEqual(new & self.v["ZOS_BANK_MASK"], bank)

    def test_bank_set_rejects_bank_7(self):
        latch = 0xA0 | self.v["ZOS_MODE_OS"] | 2
        bank_arg = 7
        accepted = (bank_arg & self.v["ZOS_BANK_MASK"]) == 7
        if accepted:
            new = latch  # rejected: latch untouched
        else:
            new = (latch & self.v["ZOS_LATCH_NON_BANK_MASK"]) | bank_arg
        self.assertEqual(new, latch)


def make_map(overrides=None, **kw):
    """Build a synthetic map resembling the real layout, then apply edits."""
    areas = {
        "_RESET0": (0x0000, 0x26),
        "_PAGE_ZERO0": (0x0030, 0x03),
        "_CODE": (0x2000, 0x6E),
        "_GSINIT": (0x206E, 0x00),
        "_GSFINAL": (0x206E, 0x01),
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
        "s__DATA": 0xC000, "l__DATA": 0,
        "s__INITIALIZED": 0xC000, "l__INITIALIZED": 0,
        "s__COMMON": 0xE000, "l__COMMON": 0x100,
        "s__COMMON_DATA": 0xE100, "l__COMMON_DATA": 0x43,
        "s__RESET": 0x0000, "l__RESET": 0x26,
        "_reset": 0x0000, "_reset_end": 0x0026,
        "_zos_rst30_vector": 0x0030, "_zos_rst30_end": 0x0033,
        "_kernel_main": 0x2000,
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
        "_device_registry_init": 0x2300, "_device_register": 0x2320,
        "_device_get": 0x2340, "_device_find": 0x2360,
        "_device_interface": 0x2370,
        "_device_registry_checkpoint": 0x2380,
        "_device_registry_rollback": 0x23A0,
        "_driver_init_static": 0x23C0,
        "_console_bind": 0x2400, "_console_bind_defaults": 0x2430,
        "_console_putc": 0x2460, "_console_write": 0x2480,
        "_console_poll": 0x24A0, "_console_getc": 0x24B0,
        "_device_table": 0xC200, "_console_in_handle": 0xC300,
        "_console_out_handle": 0xC301, "_boot_drivers": 0xC340,
        "_module_loader_init": 0x2700, "_module_load": 0x2720,
        "_module_load_source": 0x2740,
        "_module_catalog_load_all": 0x2780, "_module_count": 0x27A0,
        "_zmod_catalog": 0x26A0,
        "_module_free_granules": 0x27B0, "_module_failed_count": 0x27C0,
        "_module_get": 0x27D0, "_module_code_allowed": 0x27E0,
        "_zmod_validate_container": 0x2800, "_zmod_copy_image": 0x2880,
        "_zmod_validate_source": 0x28E0,
        "_zmod_zero_bss": 0x28A0, "_zmod_apply_relocations": 0x28C0,
        "_zmod_zero_allocation": 0x2900,
        "_romfs_init": 0x2A00, "_romfs_lookup": 0x2A80,
        "_romfs_read": 0x2B00, "_romfs_load_zmod": 0x2B80,
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
        "__driver_arena_start": 0xA000, "__driver_arena_end": 0xC000,
        "irq_vector_page": 0xFD00, "irq_vector_guard": 0xFE00,
        "irq_saved_latch": 0xFE01, "irq_saved_sp": 0xFE02,
        "irq_common_sp": 0xFE04, "_irq_active_source": 0xFE06,
        "irq_transition_stack": 0xFE80, "irq_transition_stack_top": 0xFF00,
        "irq_isr_stack": 0xDF00, "irq_isr_stack_top": 0xE000,
        "zos_gate_saved_sp": 0xE100, "zos_gate_saved_latch": 0xE102,
        "zos_gate_result_hl": 0xE103, "zos_gate_result_a": 0xE105,
        "zos_gate_result_c": 0xE106,
        "zos_gate_stack": 0xE107, "zos_gate_stack_top": 0xE147,
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
        "zos_proc_saved_sp": 0xE3D0, "zos_proc_saved_latch": 0xE3D2,
        "zos_proc_saved_iff": 0xE3D3, "zos_proc_bank": 0xE3D4,
        "zos_proc_entry": 0xE3D5,
        "_process_exec": 0x2C00, "_process_exit": 0x2C80,
        "_process_abort": 0x2CA0, "_process_alloc_bank": 0x2CC0,
        "_process_slot_alloc": 0x2CE0, "_process_slot_release": 0x2D00,
        "_process_launch_child": 0x2D20, "_zex_validate_header": 0x2D40,
        "_process_table": 0xC400, "_process_running_pid": 0xC500,
        "_process_last_status": 0xC501,
        # M7 diagnostic services and shared dispatcher helpers (bank-7
        # kernel code; no new common state).
        "_zos_diag_mem_read": 0x2E00, "_zos_diag_mem_write": 0x2E40,
        "_zos_diag_io_in": 0x2E80, "_zos_diag_io_out": 0x2EA0,
        "_zos_diag_bank_info": 0x2EC0, "_zos_diag_system_info": 0x2EE0,
        "_zos_diag_hw_in": 0x2010, "_zos_diag_hw_out": 0x2016,
        "_zos_syscall_success": 0x2632, "_zos_syscall_error": 0x263D,
        "_zos_syscall_block_ok": 0x264C,
    }
    if kw.get("areas") is not None:
        areas = dict(areas)
        for name, value in kw["areas"].items():
            if value is None:
                areas.pop(name, None)
            else:
                areas[name] = value
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


class TestMapcheck(unittest.TestCase):
    def test_valid_map_passes(self):
        mapcheck.check_map(make_map())

    def test_bank7_load_area_out_of_range(self):
        text = make_map(areas={"_CODE": (0xC100, 0x6E)})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_common_area_overlaps_stack(self):
        text = make_map(areas={"_COMMON_DATA": (0xE100, 0x1F00)})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_gate_not_in_common(self):
        text = make_map(symbols={"zos_enter_os_mode": 0x3000})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_state_not_in_common(self):
        text = make_map(symbols={"zos_gate_saved_sp": 0xC000})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_gate_stack_wrong_size(self):
        text = make_map(symbols={"zos_gate_stack_top": 0xE153})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_irq_vector_misaligned(self):
        text = make_map(symbols={"irq_vector_page": 0xFD01})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_irq_transition_stack_wrong_size(self):
        text = make_map(symbols={"irq_transition_stack_top": 0xFEF0})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_irq_isr_stack_wrong_size(self):
        text = make_map(symbols={"irq_isr_stack": 0xDF10})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_linked_kernel_code_end_mismatch(self):
        text = make_map(symbols={"_irq_kernel_code_end": 0x2070})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_kernel_main_not_in_bank7(self):
        text = make_map(symbols={"_kernel_main": 0xE100})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_reset_copier_over_rst_slot(self):
        text = make_map(symbols={"_reset_end": 0x0040})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_missing_owned_symbol_rejected(self):
        text = make_map(symbols={"s__COMMON_DATA": None})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_empty_area_with_length_rejected(self):
        text = make_map(symbols={"l__HOME": 4})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_page_zero_overflow_rejected(self):
        text = make_map(areas={"_RESET0": (0x0000, 0x0120)})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_syscall_gate_not_in_common(self):
        text = make_map(symbols={"zos_syscall_entry": 0x3000})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_dispatcher_not_in_bank7(self):
        text = make_map(symbols={"zos_syscall_dispatch": 0xE100})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_rst30_vector_wrong_address(self):
        text = make_map(symbols={"_zos_rst30_vector": 0x0040,
                                 "_zos_rst30_end": 0x0043})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_rst30_vector_wrong_size(self):
        text = make_map(symbols={"_zos_rst30_end": 0x0035})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)

    def test_missing_page_zero_area(self):
        text = make_map(areas={"_PAGE_ZERO0": None})
        with self.assertRaises(mapcheck.MapError):
            mapcheck.check_map(text)


BANNER_LINES = (
    b"ZephyrOS 0.0\r\n",
    b"platform: Zephyr-80\r\n",
    b"kernel entry: OK\r\n",
    b"serial: OK\r\n",
    b"HALT\r\n",
)


class TestRealArtifacts(unittest.TestCase):
    def setUp(self):
        self.map_path = os.path.join(ROOT, "build", "zephyros.map")
        self.bin_path = os.path.join(ROOT, "build", "zephyros.bin")

    def test_real_map_passes_mapcheck(self):
        if not os.path.exists(self.map_path):
            self.skipTest("build artifacts missing; run `make` first")
        # Production builds carry a generated derived-geometry memory.inc;
        # validate the real map against whichever layout the build used.
        generated = os.path.join(ROOT, "build", "geometry", "memory.inc")
        layout_inc = generated if os.path.exists(generated) else MEMORY_INC
        with layout_context(layout_inc, IRQ_INC):
            mapcheck.check_map(open(self.map_path).read())

    def test_m0_banner_unchanged(self):
        if not os.path.exists(self.bin_path):
            self.skipTest("build artifacts missing; run `make` first")
        image = open(self.bin_path, "rb").read()
        page1 = image[0x10000:]
        for line in BANNER_LINES:
            self.assertIn(line, page1, "banner line missing from ROM page 1: %r" % line)
        self.assertIn(b"PANIC: ", image[:0x10000], "panic path missing from ROM page 0")

    def test_mapping_routines_in_common_region(self):
        if not os.path.exists(self.map_path):
            self.skipTest("build artifacts missing; run `make` first")
        areas, symbols = mapcheck.parse_map(open(self.map_path).read())
        for name in mapcheck.COMMON_ROUTINES:
            self.assertGreaterEqual(symbols[name], mapcheck.COMMON_BASE)
            self.assertLess(symbols[name], mapcheck.STACK_BASE)

    def test_rst30_vector_bytes_target_asm_gate(self):
        """The native ABI surface is exactly JP zos_syscall_entry: no direct
        C-symbol address may appear in the page-zero vector."""
        if not os.path.exists(self.bin_path) or not os.path.exists(self.map_path):
            self.skipTest("build artifacts missing; run `make` first")
        image = open(self.bin_path, "rb").read()
        _, symbols = mapcheck.parse_map(open(self.map_path).read())
        gate = symbols["zos_syscall_entry"]
        self.assertFalse(str(gate).startswith("_"))  # no direct C symbol
        expected = bytes((0xC3, gate & 0xFF, (gate >> 8) & 0xFF))
        for page in (image[:0x10000], image[0x10000:]):
            self.assertEqual(page[0x30:0x33], expected)

    def test_selftest_image_contains_harness(self):
        selftest_bin = os.path.join(ROOT, "build-selftest", "zephyros-selftest.bin")
        if not os.path.exists(selftest_bin):
            self.skipTest("selftest image missing; run `make syscall-selftest`")
        image = open(selftest_bin, "rb").read()
        self.assertEqual(len(image), 0x80000)
        self.assertIn(b"syscall selftest", image[0x10000:])
        # Normal image must not carry the harness strings.
        if os.path.exists(self.bin_path):
            normal = open(self.bin_path, "rb").read()
            self.assertNotIn(b"syscall selftest", normal)

    def test_ff_guard_encodes_irq_ff_unexpected(self):
        """word_at(FDFFh) must be the linked irq_ff_unexpected address."""
        if not os.path.exists(self.bin_path) or not os.path.exists(self.map_path):
            self.skipTest("build artifacts missing; run `make` first")
        image = open(self.bin_path, "rb").read()
        _, symbols = mapcheck.parse_map(open(self.map_path).read())
        word = image[0xFDFF] | (image[0xFE00] << 8)
        self.assertEqual(word, symbols["irq_ff_unexpected"],
                         "word_at(FDFFh)=%04Xh != irq_ff_unexpected %04Xh"
                         % (word, symbols["irq_ff_unexpected"]))

    def test_irq_selftest_image_contains_harness(self):
        irq_bin = os.path.join(ROOT, "build-irq-selftest", "zephyros-irq-selftest.bin")
        if not os.path.exists(irq_bin):
            self.skipTest("irq-selftest image missing; run `make irq-selftest`")
        image = open(irq_bin, "rb").read()
        self.assertEqual(len(image), 0x80000)
        self.assertIn(b"irq selftest", image[0x10000:])
        if os.path.exists(self.bin_path):
            normal = open(self.bin_path, "rb").read()
            self.assertNotIn(b"irq selftest", normal)


if __name__ == "__main__":
    unittest.main()
