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

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import mapcheck  # noqa: E402

HARDWARE_H = os.path.join(ROOT, "platform", "zephyr80", "hardware.h")
HARDWARE_INC = os.path.join(ROOT, "platform", "zephyr80", "hardware.inc")
SELFTEST_H = os.path.join(ROOT, "include", "zephyros", "selftest.h")
SELFTEST_INC = os.path.join(ROOT, "platform", "zephyr80", "selftest.inc")
IRQ_SELFTEST_H = os.path.join(ROOT, "include", "zephyros", "irq_selftest.h")
IRQ_SELFTEST_INC = os.path.join(ROOT, "platform", "zephyr80", "irq_selftest.inc")

REQUIRED_CONSTANTS = (
    "ZOS_MEMCTL_PORT",
    "ZOS_SIO0A_DATA", "ZOS_SIO0A_CTRL",
    "ZOS_SIO0B_DATA", "ZOS_SIO0B_CTRL",
    "ZOS_BANK_MASK", "ZOS_LATCH_NON_BANK_MASK",
    "ZOS_RAM_SHADOW", "ZOS_ROM_DIS", "ZOS_MODE_MASK",
    "ZOS_ROM_PAGE_SHIFT", "ZOS_ROM_PAGE_MASK",
    "ZOS_MODE_BOOT", "ZOS_MODE_SHADOW",
    "ZOS_MODE_APPLICATION", "ZOS_MODE_OS",
    "ZOS_SIO_RR0_RX_AVAILABLE", "ZOS_SIO_RR0_TX_EMPTY", "ZOS_SIO_RR0_CTS",
    "ZOS_SIO_TX_READY",
    "ZOS_SIO_WR0_CHANNEL_RESET", "ZOS_SIO_WR4_X16_1STOP_NOPARITY",
    "ZOS_SIO_WR3_RX8_AUTO_ENABLES", "ZOS_SIO_WR5_TX8_DTR_RTS_RELEASE",
    "ZOS_SIO_WR1_INT_DISABLED", "ZOS_SIO_TX_TIMEOUT",
    "ZOS_CTC0_PORT", "ZOS_CTC0_VECTOR_BASE", "ZOS_CTC0_CONTROL_TEST",
    "ZOS_CTC0_TIME_CONSTANT", "ZOS_CTC0_RESET", "ZOS_IM2_I_VALUE",
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
        for name in REQUIRED_CONSTANTS:
            self.assertIn(name, self.c, "hardware.h missing %s" % name)
            self.assertIn(name, self.asm, "hardware.inc missing %s" % name)

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
        "irq_vector_page": 0xFD00, "irq_vector_guard": 0xFE00,
        "irq_saved_latch": 0xFE01, "irq_saved_sp": 0xFE02,
        "irq_common_sp": 0xFE04, "_irq_active_source": 0xFE06,
        "irq_transition_stack": 0xFE80, "irq_transition_stack_top": 0xFF00,
        "irq_isr_stack": 0xDF00, "irq_isr_stack_top": 0xE000,
        "zos_gate_saved_sp": 0xE100, "zos_gate_saved_latch": 0xE102,
        "zos_gate_result_hl": 0xE103, "zos_gate_result_a": 0xE105,
        "zos_gate_result_c": 0xE106,
        "zos_gate_stack": 0xE107, "zos_gate_stack_top": 0xE147,
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
        self.assertEqual(len(image), 131072)
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
        self.assertEqual(len(image), 131072)
        self.assertIn(b"irq selftest", image[0x10000:])
        if os.path.exists(self.bin_path):
            normal = open(self.bin_path, "rb").read()
            self.assertNotIn(b"irq selftest", normal)


if __name__ == "__main__":
    unittest.main()
