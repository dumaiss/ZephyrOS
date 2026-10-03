#!/usr/bin/env python3
"""ZephyrOS M3 map/layout checker.

Validates the sdldz80 map against the implemented mode 10/11 memory
ownership contract (see docs/memory.md):

    page zero     0000h-00FFh   reset copier only, below 0030h
    bank 7 load   2000h-BFFFh   _CODE/_HOME/_INITIALIZER/_GSINIT/_GSFINAL
    bank 7 runtime C000h-DFFFh  _DATA/_INITIALIZED (never ROM-loaded)
    common         E000h-FEFFh  _COMMON/_COMMON_DATA, gates, console, panic
    common stack   FF00h-FFFFh  reserved; nothing may be linked there

M3 adds placement checks for the device registry, console service, static
driver descriptors/ops, and driver-private state: all of it must live in
bank 7 (loadable or runtime area, below the ISR stack) and none of it may
enter permanent common memory.

Empty areas do not get an area-table entry in sdldz80 maps; they still get
s__<area>/l__<area> symbols, so ownership is checked through the symbol
table and empty areas are accepted when their zero-length placement address
lies inside the expected region.

Usage:
    mapcheck.py <zephyros.map> [platform/zephyr80/memory.inc]
"""

import re
import sys

# Defaults; overridden from platform/zephyr80/memory.inc when the path is
# given on the command line, so the checker derives its facts from the
# actual layout declarations rather than a second copy of the memory map.
PAGE_ZERO_END = 0x0100
RESET_TOP = 0x0030
BANK7_BASE = 0x2000
BANK7_LOAD_END = 0xC000
BANK7_RUNTIME_END = 0xE000
COMMON_BASE = 0xE000
COMMON_CODE_END = 0xFD00
IRQ_VECTOR_BASE = 0xFD00
IRQ_VECTOR_SIZE = 0x0101
IRQ_STATE_BASE = 0xFE01
IRQ_STACK_BASE = 0xFE80
IRQ_STACK_TOP = 0xFF00
IRQ_STACK_SIZE = 0x0080
IRQ_ISR_STACK_BASE = 0xDF00
IRQ_ISR_STACK_TOP = 0xE000
IRQ_ISR_STACK_SIZE = 0x0100
STACK_BASE = 0xFF00
MEM_TOP = 0x10000
GATE_STACK_SIZE = 64


def parse_memory_inc(path):
    """Parse the layout declarations in platform/zephyr80/memory.inc."""
    values = {}
    for line in open(path):
        line = line.split(";")[0].strip()
        match = re.match(r"([A-Za-z_]\w*)\s+=\s+(0[xX][0-9a-fA-F]+|\d+)$", line)
        if match:
            values[match.group(1)] = int(match.group(2), 0)
    return values


def apply_layout(values):
    """Apply layout declarations from memory.inc."""
    global PAGE_ZERO_END, RESET_TOP, BANK7_BASE, BANK7_LOAD_END
    global BANK7_RUNTIME_END, COMMON_BASE, COMMON_CODE_END, STACK_BASE
    global IRQ_VECTOR_BASE, IRQ_VECTOR_SIZE, IRQ_STATE_BASE
    global IRQ_STACK_BASE, IRQ_STACK_TOP, IRQ_STACK_SIZE
    global IRQ_ISR_STACK_BASE, IRQ_ISR_STACK_TOP, IRQ_ISR_STACK_SIZE
    global GATE_STACK_SIZE, OWNED_AREAS
    PAGE_ZERO_END = values["ZOS_PAGE_ZERO_END"]
    RESET_TOP = values["ZOS_RESET_TOP"]
    BANK7_BASE = values["ZOS_BANK7_BASE"]
    BANK7_LOAD_END = values["ZOS_BANK7_LOAD_END"]
    COMMON_BASE = values["ZOS_COMMON_BASE"]
    STACK_BASE = values["ZOS_COMMON_STACK_BASE"]
    BANK7_RUNTIME_END = values["ZOS_BANK7_RUNTIME_END"]
    COMMON_CODE_END = values["ZOS_COMMON_CODE_END"]
    IRQ_VECTOR_BASE = values["ZOS_IRQ_VECTOR_BASE"]
    IRQ_VECTOR_SIZE = values["ZOS_IRQ_VECTOR_SIZE"]
    IRQ_STATE_BASE = values["ZOS_IRQ_STATE_BASE"]
    IRQ_STACK_BASE = values["ZOS_IRQ_STACK_BASE"]
    IRQ_STACK_TOP = values["ZOS_IRQ_STACK_TOP"]
    IRQ_STACK_SIZE = values["ZOS_IRQ_STACK_SIZE"]
    IRQ_ISR_STACK_BASE = values["ZOS_IRQ_ISR_STACK_BASE"]
    IRQ_ISR_STACK_TOP = values["ZOS_IRQ_ISR_STACK_TOP"]
    IRQ_ISR_STACK_SIZE = values["ZOS_IRQ_ISR_STACK_SIZE"]
    GATE_STACK_SIZE = values["ZOS_GATE_STACK_SIZE"]
    OWNED_AREAS = {
        "_CODE": (BANK7_BASE, BANK7_LOAD_END),
        "_HOME": (BANK7_BASE, BANK7_LOAD_END),
        "_INITIALIZER": (BANK7_BASE, BANK7_LOAD_END),
        "_GSINIT": (BANK7_BASE, BANK7_LOAD_END),
        "_GSFINAL": (BANK7_BASE, BANK7_LOAD_END),
        "_DATA": (BANK7_LOAD_END, BANK7_RUNTIME_END),
        "_INITIALIZED": (BANK7_LOAD_END, BANK7_RUNTIME_END),
        "_COMMON": (COMMON_BASE, COMMON_CODE_END),
        "_COMMON_DATA": (COMMON_BASE, COMMON_CODE_END),
    }


# Owned areas and their allowed placement range.
OWNED_AREAS = {
    "_CODE": (BANK7_BASE, BANK7_LOAD_END),
    "_HOME": (BANK7_BASE, BANK7_LOAD_END),
    "_INITIALIZER": (BANK7_BASE, BANK7_LOAD_END),
    "_GSINIT": (BANK7_BASE, BANK7_LOAD_END),
    "_GSFINAL": (BANK7_BASE, BANK7_LOAD_END),
    "_DATA": (BANK7_LOAD_END, BANK7_RUNTIME_END),
    "_INITIALIZED": (BANK7_LOAD_END, BANK7_RUNTIME_END),
    "_COMMON": (COMMON_BASE, COMMON_CODE_END),
    "_COMMON_DATA": (COMMON_BASE, COMMON_CODE_END),
}

# Every routine that changes the memory mapping, or is needed immediately
# after a mapping change, must be common-resident.
COMMON_ROUTINES = (
    "_boot_common_entry",
    "__zos_crt0_init",
    "__zos_mode_fault",
    "zos_enter_os_mode",
    "zos_leave_os_mode",
    "zos_os_continuation",
    "zos_syscall_entry",
    "_platform_memctl_read",
    "_platform_memctl_write",
    "_platform_bank_get",
    "_platform_bank_set",
    "_early_console_init",
    "_early_console_putc",
    "_early_console_puts",
    "_panic",
    "irq_ctc0_stub",
    "irq_ff_unexpected",
    "irq_common_entry",
    "_irq_arch_lock",
    "_irq_arch_unlock",
    "_irq_arch_init",
)

# Bank-7 internal routines (visible only while mode 11 is active).
BANK7_ROUTINES = ("zos_syscall_dispatch", "_irq_dispatch")

# M3 device/driver/service routines: bank 7 only.  These are the real
# linked symbols of kernel/device/*.c and drivers/sio/*.
M3_BANK7_ROUTINES = (
    "_device_registry_init",
    "_device_register",
    "_device_get",
    "_device_find_capability",
    "_device_registry_checkpoint",
    "_device_registry_rollback",
    "_driver_init_static",
    "_console_bind",
    "_console_bind_defaults",
    "_console_putc",
    "_console_write",
    "_console_poll",
    "_console_getc",
    "_sio0b_hw_init",
    "_sio0b_putc",
    "_sio0b_write",
    "_sio0b_poll",
    "_sio0b_getc",
    "_sio0b_status",
    "_sio0b_write_ctrl",
    "_sio0b_read_data",
    "_sio0b_write_data",
)

# M3 registry/console/driver objects and private state: bank-7 data or
# code, never common memory.
M3_BANK7_OBJECTS = (
    "_device_table",
    "_console_in_handle",
    "_console_out_handle",
    "_sio0b_ops",
    "_sio0b_state",
    "_sio_driver",
    "_boot_drivers",
)

# Any symbol with one of these prefixes is M3 driver/service code or state
# and must be bank-7 resident (this sweep also covers the test-only driver
# objects present in the m3-selftest image).
M3_BANK7_PREFIXES = (
    "_device_",
    "_console_",
    "_sio0b_",
    "_sio_",
    "_driver_",
    "_boot_drivers",
    "_m3_",
)

# Transition-critical state block (common RAM).
COMMON_STATE = (
    "zos_gate_saved_sp",
    "zos_gate_saved_latch",
    "zos_gate_result_hl",
    "zos_gate_result_a",
    "zos_gate_result_c",
    "zos_gate_stack",
    "zos_gate_stack_top",
)

IRQ_STATE = (
    "irq_saved_latch", "irq_saved_sp", "irq_common_sp", "_irq_active_source",
)


class MapError(Exception):
    """The linked map violates the ZephyrOS memory contract."""


def parse_map(text):
    """Return (areas, symbols) from an sdldz80 wide map."""
    areas = {}
    symbols = {}
    for line in text.splitlines():
        area = re.match(r"\s*([A-Za-z_.][\w.]*)\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8}) =", line)
        if area:
            name, addr, size = area.group(1), int(area.group(2), 16), int(area.group(3), 16)
            if name != ".ABS.":
                areas[name] = (addr, size)
            continue
        sym = re.match(r"\s*([0-9A-Fa-f]{8})\s+(\S+)", line)
        if sym:
            symbols[sym.group(2)] = int(sym.group(1), 16)
    return areas, symbols


def check_areas(areas, symbols):
    # Page zero: only the reset copier and RST 30h vector areas, entirely
    # within 0000h-00FFh.
    reset_areas = [n for n in areas if n.startswith("_RESET")]
    page_zero_areas = [n for n in areas if n.startswith("_PAGE_ZERO")]
    if not reset_areas:
        raise MapError("map has no page-zero/reset area")
    for name in reset_areas + page_zero_areas:
        addr, size = areas[name]
        if addr >= PAGE_ZERO_END or addr + size > PAGE_ZERO_END:
            raise MapError(
                "%s at %04Xh (size %d) leaves page zero 0000h-00FFh"
                % (name, addr, size)
            )
    # The reset copier must stay strictly below the RST 30h vector.
    for name in reset_areas:
        addr, size = areas[name]
        if addr + size > RESET_TOP:
            raise MapError(
                "%s at %04Xh (size %d) overlaps the %04Xh RST 30h vector "
                "reservation" % (name, addr, size, RESET_TOP)
            )

    # Nothing may occupy the common stack reservation FF00h-FFFFh.
    for name, (addr, size) in sorted(areas.items()):
        if name in reset_areas:
            continue
        if addr + size > MEM_TOP:
            raise MapError("%s (size %d) exceeds the 16-bit space" % (name, size))
        if addr >= COMMON_BASE and addr + size > STACK_BASE:
            raise MapError(
                "%s at %04Xh (size %d) overlaps the common stack "
                "reservation FF00h-FFFFh" % (name, addr, size)
            )

    # Region ownership via s__/l__ symbols; the linker drops the area's
    # leading underscore in synthesized symbols (_CODE -> s__CODE).
    # Empty areas have no area-table entry but still get zero-length
    # placement symbols.
    for name, (start, end) in OWNED_AREAS.items():
        base = name.lstrip("_")
        for tag in ("s__", "l__"):
            if tag + base not in symbols:
                raise MapError("map has no symbol %s%s" % (tag, base))
        saddr = symbols["s__" + base]
        if not (start <= saddr < end):
            raise MapError(
                "%s at %04Xh is outside %04Xh-%04Xh"
                % (name, saddr, start, end - 1)
            )
        if name in areas:
            addr, size = areas[name]
            if addr != saddr:
                raise MapError("%s table addr %04Xh != s__%s %04Xh"
                               % (name, addr, base, saddr))
            if symbols["l__" + base] != size:
                raise MapError("l__%s = %d != table size %d"
                               % (base, symbols["l__" + base], size))
            if addr + size > end:
                raise MapError(
                    "%s at %04Xh (size %d) leaves %04Xh-%04Xh"
                    % (name, addr, size, start, end - 1)
                )
        elif symbols["l__" + base] != 0:
            raise MapError("%s is missing from the area table but l__%s != 0"
                           % (name, base))

    # Ordinary bank-7 data stops below the dedicated ISR stack, and
    # ordinary common allocation stops below the IM2 page.
    for name in ("_DATA", "_INITIALIZED"):
        if name in areas:
            addr, size = areas[name]
            if addr + size > IRQ_ISR_STACK_BASE:
                raise MapError("%s overlaps the bank-7 ISR stack at DF00h" % name)
    for name in ("_COMMON", "_COMMON_DATA"):
        if name in areas:
            addr, size = areas[name]
            if addr + size > IRQ_VECTOR_BASE:
                raise MapError("%s overlaps the IM2 reservation at FD00h" % name)


def check_symbols(areas, symbols):
    page_zero_areas = [n for n in areas if n.startswith("_PAGE_ZERO")]

    # Mapping-changing routines and immediate continuations: common only.
    for name in COMMON_ROUTINES:
        if name not in symbols:
            raise MapError("map has no symbol %s" % name)
        addr = symbols[name]
        if not (COMMON_BASE <= addr < COMMON_CODE_END):
            raise MapError(
                "%s at %04Xh is not in the common region E000h-FEFFh" % (name, addr)
            )

    # Bank-7 routines must stay inside the loadable kernel region.
    for name in BANK7_ROUTINES:
        if name not in symbols:
            raise MapError("map has no symbol %s" % name)
        addr = symbols[name]
        if not (BANK7_BASE <= addr < BANK7_LOAD_END):
            raise MapError(
                "%s at %04Xh is outside the bank-7 loadable region "
                "2000h-BFFFh" % (name, addr)
            )

    # M3: registry, console service, static drivers, ops tables, and
    # driver-private state live in bank 7 (code or runtime data), never in
    # the permanently mapped common region.
    for name in M3_BANK7_ROUTINES + M3_BANK7_OBJECTS:
        if name not in symbols:
            raise MapError("map has no M3 symbol %s" % name)
        addr = symbols[name]
        if not (BANK7_BASE <= addr < IRQ_ISR_STACK_BASE):
            raise MapError(
                "%s at %04Xh is not in the bank-7 region 2000h-DEFFh"
                % (name, addr)
            )
    for name, addr in symbols.items():
        if addr != 0 and name.startswith(M3_BANK7_PREFIXES):
            if not (BANK7_BASE <= addr < IRQ_ISR_STACK_BASE):
                raise MapError(
                    "driver/service symbol %s at %04Xh is not bank-7 "
                    "resident" % (name, addr)
                )

    # Transition state block: common RAM, gate stack of the exact size.
    for name in COMMON_STATE:
        if name not in symbols:
            raise MapError("map has no state symbol %s" % name)
        addr = symbols[name]
        if not (COMMON_BASE <= addr < STACK_BASE):
            raise MapError(
                "%s at %04Xh is not in the common region E000h-FEFFh" % (name, addr)
            )
    stack_size = symbols["zos_gate_stack_top"] - symbols["zos_gate_stack"]
    if stack_size != GATE_STACK_SIZE:
        raise MapError("gate stack is %d bytes, expected %d" % (stack_size, GATE_STACK_SIZE))

    # M2 fixed absolute reservations.  ABS area start symbols are not
    # useful in sdld maps, so validate their exported boundary labels.
    required = {
        "irq_vector_page": IRQ_VECTOR_BASE,
        "irq_vector_guard": IRQ_VECTOR_BASE + 0x100,
        "irq_transition_stack": IRQ_STACK_BASE,
        "irq_transition_stack_top": IRQ_STACK_TOP,
        "irq_isr_stack": IRQ_ISR_STACK_BASE,
        "irq_isr_stack_top": IRQ_ISR_STACK_TOP,
    }
    for name, expected in required.items():
        if symbols.get(name) != expected:
            raise MapError("%s is at %04Xh, expected %04Xh"
                           % (name, symbols.get(name, -1), expected))
    if IRQ_VECTOR_SIZE != 0x101:
        raise MapError("IM2 page plus guard size is %d, expected 257" % IRQ_VECTOR_SIZE)
    if symbols["irq_transition_stack_top"] - symbols["irq_transition_stack"] != IRQ_STACK_SIZE:
        raise MapError("IRQ transition stack size is not %d" % IRQ_STACK_SIZE)
    if symbols["irq_isr_stack_top"] - symbols["irq_isr_stack"] != IRQ_ISR_STACK_SIZE:
        raise MapError("bank-7 ISR stack size is not %d" % IRQ_ISR_STACK_SIZE)
    for name in IRQ_STATE:
        addr = symbols.get(name)
        if addr is None or not (IRQ_STATE_BASE <= addr < IRQ_STACK_BASE):
            raise MapError("%s is not in IRQ transition state FE01h-FE7Fh" % name)

    code_end = symbols.get("_irq_kernel_code_end")
    expected_end = symbols["s__CODE"] + symbols["l__CODE"]
    if code_end != expected_end:
        raise MapError("_irq_kernel_code_end is %04Xh, linked _CODE ends at %04Xh"
                       % (code_end if code_end is not None else -1, expected_end))

    # Kernel body must be in bank 7.
    if "_kernel_main" not in symbols:
        raise MapError("map has no symbol _kernel_main")
    km = symbols["_kernel_main"]
    if not (BANK7_BASE <= km < BANK7_LOAD_END):
        raise MapError("_kernel_main at %04Xh is outside the bank-7 loadable region" % km)

    # Reset copier must stay below the RST 30h reservation, and the
    # RST 30h vector must be exactly the three-byte JP stub at 0030h.
    if "_reset_end" not in symbols:
        raise MapError("map has no symbol _reset_end")
    if symbols["_reset_end"] > RESET_TOP:
        raise MapError(
            "_reset_end at %04Xh exceeds the %04Xh RST 30h reservation"
            % (symbols["_reset_end"], RESET_TOP)
        )
    if not page_zero_areas:
        raise MapError("map has no _PAGE_ZERO area for the RST 30h vector")
    if symbols.get("_zos_rst30_vector") != RESET_TOP:
        raise MapError(
            "_zos_rst30_vector is at %04Xh, expected %04Xh"
            % (symbols.get("_zos_rst30_vector", -1), RESET_TOP)
        )
    vector_size = symbols.get("_zos_rst30_end", 0) - RESET_TOP
    if vector_size != 3:
        raise MapError("RST 30h vector is %d bytes, expected exactly 3" % vector_size)


def check_map(text):
    areas, symbols = parse_map(text)
    check_areas(areas, symbols)
    check_symbols(areas, symbols)
    return areas, symbols


def main(argv):
    if len(argv) not in (2, 3):
        print(__doc__)
        return 2
    if len(argv) == 3:
        apply_layout(parse_memory_inc(argv[2]))
    try:
        areas, symbols = check_map(open(argv[1]).read())
    except MapError as exc:
        print("mapcheck: %s" % exc, file=sys.stderr)
        return 1
    print("mapcheck: OK (%d areas, %d symbols)" % (len(areas), len(symbols)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
