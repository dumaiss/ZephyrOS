#!/usr/bin/env python3
"""IPC substrate acceptance replay (work package 39, requirements 6-7).

Boots build-ipc-selftest/zephyros-ipc-selftest.bin -- the production boot
path with the fixed-core IPC substrate linked in, the six synthetic
resident services placed in bank 6 by the cold loader, and the SDK client
program booting as /SYSTEM/ipc.zex -- and checks the recorded evidence,
not just the transcript:

* the promoted package-35 conformance suite runs from the client and
  reports PASS (>= 49 checks);
* the phase markers bound per-call T-state spans; the replay measures them
  for the null call, the 256-byte console write, the 512-byte chained
  read (direct, offset slice, staged and repeated) and the M9 filesystem
  baseline on the same 512-byte chunk;
* every service-domain interval restores the caller's latch *exactly*,
  including the one that was interrupted by CTC0: two deliveries arrive
  while a call is in progress, the handler runs in the service domain on
  the nucleus ISR stack, and the call's frame, stack and latch survive;
* the descriptor copies run as window LDIRs in the domain (the 512-byte
  chain, the staged chain and the console write);
* the reserved stacks (service 5D00h-5F00h, ISR 5F00h-6000h, gate stack)
  stay inside their regions: the replay watches SP and records the
  high-water mark of each;
* the SDK lookup cache is real: exactly eight SYS_IPC_LOOKUP syscalls for
  six endpoint classes, the miss and the version query, although the
  client issues one additional cached lookup.

Ported from the package-35 spike replay (retired in this package with
the spike it measured): the same T-state table, the same tracer mechanics
and the same CTC0 stand-in, so the promoted suite is measured by the same
instrument that produced the spike's figures.

Skipped when the image has not been built.
"""

import json
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import Z80, read_symbols, skip_serial_connect_delay  # noqa: E402

RET = 0x1234
STACK = 0xFFFE
STEP_LIMIT = 60000000

IMAGE_SUBDIR = "build-ipc-selftest"
IMAGE_NAME = "zephyros-ipc-selftest.bin"

PASS = b"ipc: PASS\r\n"
CLIENT_START = b"ipc selftest: client OK\r\n"
FAIL = b"ipc: FAIL\r\n"
# The caller-buffer probe's own verdict, printed after the client exits.
APERTURE_PASS = b"ipccopy: PASS\r\n"
APERTURE_FAIL = b"ipccopy: FAIL\r\n"
PANIC = b"PANIC"
LOADER_OK = b"service loader: 8 images in bank 6\r\n"

MEMCTL_PORT = 0x00
MODE_MASK = 0x18
PAGE_MASK = 0xE0
BANK_MASK = 0x07
OS_MODE = 0x18
APPLICATION_MODE = 0x10
CTC0_PORT = 0x40
CTC0_CONTROL = 0xA7
CTC0_TIME_CONSTANT = 0x00
CTC0_VECTOR = 0x00
# Correction 01: the irqx copy op arms the same channel on a short time
# constant, so the modelled delivery lands inside the path-B aperture
# window instead of between copies.  The stand-in keys on the constant,
# which is the one writable value that separates the two profiles.
CTC0_APERTURE_TIME_CONSTANT = 0x08

PAGE_ONE = 0x20

# The resident service domain: bank 6 in the mode-11 window, with the hot
# tier (6000h-DFFFh) and the SERVICE_IO blocks (8000h-BFFFh) inside it
# (architecture 36.6, correction 01 to 36.8).
DOMAIN_BASE = 0x6000
DOMAIN_END = 0xE000
SERVICE_BASE = DOMAIN_BASE
SERVICE_END = DOMAIN_END

# The packed service images, their per-image reports and the plan the
# loader's own report is pinned to (Makefile: IPC_MODDIR).
MODULE_DIR = os.path.join(ROOT, IMAGE_SUBDIR, "modules", "ipc-modules")
SERVICE_DIR = os.path.join(ROOT, IMAGE_SUBDIR, "modules")
SERVICE_ORDER = ["ioc", "fat", "vfs", "echo", "con", "irqx"]
IPC_BOOT_MAGIC = 0xA7

SERVICE_PAGE_BITS = 0x20

# The client's measurement phases (its one-letter marker codes); 8 warm-up
# calls then 24 measured calls per phase.
PHASES = {
    "n": "null-call",
    "c": "con256-write",
    "o": "console256-direct",
    "r": "vfs-512-direct",
    "s": "vfs-64-offset",
    "t": "vfs-512-staged",
    "u": "vfs-512-repeat",
    "f": "fsread-baseline",
}
N1 = 8
N2 = 24

# The caller-buffer probe's phases (correction 01): code, case name, the
# caller buffer it names, and the copy path the correction requires.
# "window" is path A (one mode-11 LDIR), "aperture" is path B (one LDIR
# with the mode-10 aperture open), "slow" is path C (the chunked copy
# through the zos_bank_copy gate).
PROBE_PHASES = {
    "A": ("window-read", 0x0100, "window"),
    "P": ("window-single", 0x0100, "window"),
    "B": ("high1-read", 0x3000, "aperture"),
    "Q": ("high1-single", 0x3000, "aperture"),
    "C": ("high2-read", 0x7000, "aperture"),
    "R": ("high2-single", 0x7000, "aperture"),
    "D": ("high3-read", 0xC000, "aperture"),
    "S": ("high3-single", 0xC000, "aperture"),
    "E": ("behind-read", 0x9000, "slow"),
    "T": ("behind-single", 0x9000, "slow"),
    "U": ("service-slow", 0x0100, "slow"),
    "V": ("irq-aperture", 0x3000, "aperture"),
}
PROBE_N1 = 2
PROBE_N2 = 8

# The probe's console pattern (tests/target/ipc/ipccopy.c).
PROBE_PATTERN_BASE = 0x80


# Counter symbols the probe's cases are attributed by (kernel/ipc/ipc.c).
COPY_COUNTERS = ("ipc_copy_window", "ipc_copy_aperture",
                 "ipc_copy_slow")

# Phases that issue an IPC call (the others measure today's direct kernel
# path: the console write and the filesystem read the chain is compared
# against, so they must not issue any).
IPC_PHASES = ("n", "c", "o", "r", "s", "t", "u")
DIRECT_PHASES = ("f",)

# The IPC phases whose calls move payload bytes through the service domain:
# the null call ("n") moves nothing, and the two direct phases move no
# bytes through a domain at all.
COPY_PHASES = ("c", "r", "s", "t", "u")

COUNT_RE = re.compile(rb"ipc: (\d+)/(\d+) checks\r\n")
# The client marks its promoted phases with lower-case codes; the
# caller-buffer probe (correction 01) marks its cases with upper-case
# ones, so one expression reads both transcripts.
MARK_RE = re.compile(rb"ipc: ([A-Za-z])([abc])\r\n")

# The reserved stacks, named (base, top) from the linked map.
STACK_NAMES = ("service", "isr", "gate")

# Result/evidence words the client compares against (mirrors
# tests/target/include/ipc_test.h and abi/zos/ipc.h).
IPC_FRAME_SIZE = 32
IPC_BLOCK_BYTES = 512
LOOKUP_SYSCALL = 0x06
CALL_SYSCALL = 0x07

# Bytes moved per call in each probe phase (correction 01): the reads and
# single copies move one 512-byte block, the service-side slow case
# writes 16 bytes through the console, and the IRQ case runs four
# 512-byte copies per call.
PROBE_PHASE_BYTES = {name: IPC_BLOCK_BYTES
                     for name in ("window-read", "window-single",
                                  "high1-read", "high1-single",
                                  "high2-read", "high2-single",
                                  "high3-read", "high3-single",
                                  "behind-read", "behind-single")}
PROBE_PHASE_BYTES["service-slow"] = 16
PROBE_PHASE_BYTES["irq-aperture"] = 4 * IPC_BLOCK_BYTES


ONE_BYTE_T = {0: 4,
 1: 10,
 2: 7,
 3: 6,
 4: 4,
 5: 4,
 6: 7,
 7: 4,
 8: 4,
 9: 11,
 10: 7,
 11: 6,
 12: 4,
 13: 4,
 14: 7,
 15: 4,
 16: 13,
 17: 10,
 18: 7,
 19: 6,
 20: 4,
 21: 4,
 22: 7,
 23: 4,
 24: 12,
 25: 11,
 26: 7,
 27: 6,
 28: 4,
 29: 4,
 30: 7,
 31: 4,
 32: 12,
 33: 10,
 34: 16,
 35: 6,
 36: 4,
 37: 4,
 38: 7,
 39: 4,
 40: 12,
 41: 11,
 42: 16,
 43: 6,
 44: 4,
 45: 4,
 46: 7,
 47: 4,
 48: 12,
 49: 10,
 50: 13,
 51: 6,
 52: 11,
 53: 11,
 54: 10,
 55: 4,
 56: 12,
 57: 11,
 58: 13,
 59: 6,
 60: 4,
 61: 4,
 62: 7,
 63: 4,
 64: 4,
 65: 4,
 66: 4,
 67: 4,
 68: 4,
 69: 4,
 70: 7,
 71: 4,
 72: 4,
 73: 4,
 74: 4,
 75: 4,
 76: 4,
 77: 4,
 78: 7,
 79: 4,
 80: 4,
 81: 4,
 82: 4,
 83: 4,
 84: 4,
 85: 4,
 86: 7,
 87: 4,
 88: 4,
 89: 4,
 90: 4,
 91: 4,
 92: 4,
 93: 4,
 94: 7,
 95: 4,
 96: 4,
 97: 4,
 98: 4,
 99: 4,
 100: 4,
 101: 4,
 102: 7,
 103: 4,
 104: 4,
 105: 4,
 106: 4,
 107: 4,
 108: 4,
 109: 4,
 110: 7,
 111: 4,
 112: 7,
 113: 7,
 114: 7,
 115: 7,
 116: 7,
 117: 7,
 118: 4,
 119: 7,
 120: 4,
 121: 4,
 122: 4,
 123: 4,
 124: 4,
 125: 4,
 126: 7,
 127: 4,
 128: 4,
 129: 4,
 130: 4,
 131: 4,
 132: 4,
 133: 4,
 134: 7,
 135: 4,
 136: 4,
 137: 4,
 138: 4,
 139: 4,
 140: 4,
 141: 4,
 142: 7,
 143: 4,
 144: 4,
 145: 4,
 146: 4,
 147: 4,
 148: 4,
 149: 4,
 150: 7,
 151: 4,
 152: 4,
 153: 4,
 154: 4,
 155: 4,
 156: 4,
 157: 4,
 158: 7,
 159: 4,
 160: 4,
 161: 4,
 162: 4,
 163: 4,
 164: 4,
 165: 4,
 166: 7,
 167: 4,
 168: 4,
 169: 4,
 170: 4,
 171: 4,
 172: 4,
 173: 4,
 174: 7,
 175: 4,
 176: 4,
 177: 4,
 178: 4,
 179: 4,
 180: 4,
 181: 4,
 182: 7,
 183: 4,
 184: 4,
 185: 4,
 186: 4,
 187: 4,
 188: 4,
 189: 4,
 190: 7,
 191: 4,
 192: 5,
 193: 10,
 194: 10,
 195: 10,
 196: 10,
 197: 11,
 198: 7,
 199: 11,
 200: 5,
 201: 10,
 202: 10,
 204: 10,
 205: 17,
 206: 7,
 207: 11,
 208: 5,
 209: 10,
 210: 10,
 211: 11,
 212: 10,
 213: 11,
 214: 7,
 215: 11,
 216: 5,
 217: 4,
 218: 10,
 219: 11,
 220: 10,
 222: 7,
 223: 11,
 224: 5,
 225: 10,
 226: 10,
 227: 19,
 228: 10,
 229: 11,
 230: 7,
 231: 11,
 232: 5,
 233: 4,
 234: 10,
 235: 4,
 236: 10,
 238: 7,
 239: 11,
 240: 5,
 241: 10,
 242: 10,
 243: 4,
 244: 10,
 245: 11,
 246: 7,
 247: 11,
 248: 5,
 249: 6,
 250: 10,
 251: 4,
 252: 10,
 254: 7,
 255: 11}

# ED-prefixed non-block instructions used by the kernel/ISR path.
ED_T = {64: 12,
 65: 12,
 66: 15,
 67: 20,
 68: 8,
 69: 14,
 70: 8,
 71: 9,
 72: 12,
 73: 12,
 74: 15,
 75: 20,
 77: 14,
 79: 9,
 80: 12,
 81: 12,
 82: 15,
 83: 20,
 86: 8,
 87: 9,
 88: 12,
 89: 12,
 90: 15,
 91: 20,
 94: 8,
 95: 9,
 96: 12,
 97: 12,
 98: 15,
 99: 20,
 103: 18,
 104: 12,
 105: 12,
 106: 15,
 107: 20,
 111: 18,
 114: 18,
 115: 20,
 120: 12,
 121: 12,
 122: 15,
 123: 20}

# Block instructions: the simulator executes the whole block in one step
# and counts one step per byte iteration, so the repeat cost is 21 T with
# 16 T for the last iteration.
ED_BLOCK = (160,
 161,
 162,
 163,
 168,
 169,
 170,
 171,
 176,
 177,
 178,
 179,
 184,
 185,
 186,
 187)

# DD/FD (IX/IY) forms: displacement forms and the register/stack forms the
# SDCC frame helpers emit.
DD_T = {9: 15,
 25: 15,
 33: 14,
 34: 20,
 35: 10,
 36: 8,
 37: 8,
 38: 11,
 41: 15,
 42: 20,
 43: 10,
 44: 8,
 45: 8,
 46: 11,
 52: 23,
 53: 23,
 54: 19,
 57: 15,
 68: 8,
 69: 8,
 70: 19,
 76: 8,
 77: 8,
 78: 19,
 84: 8,
 85: 8,
 86: 19,
 92: 8,
 93: 8,
 94: 19,
 96: 8,
 97: 8,
 98: 8,
 99: 8,
 100: 8,
 101: 8,
 102: 19,
 103: 8,
 104: 8,
 105: 8,
 106: 8,
 107: 8,
 108: 8,
 109: 8,
 110: 19,
 111: 8,
 112: 19,
 113: 19,
 114: 19,
 115: 19,
 116: 19,
 117: 19,
 119: 19,
 124: 8,
 125: 8,
 126: 19,
 132: 8,
 133: 8,
 134: 19,
 140: 8,
 141: 8,
 142: 19,
 148: 8,
 149: 8,
 150: 19,
 156: 8,
 157: 8,
 158: 19,
 164: 8,
 165: 8,
 166: 19,
 172: 8,
 173: 8,
 174: 19,
 180: 8,
 181: 8,
 182: 19,
 188: 8,
 189: 8,
 190: 19,
 225: 14,
 227: 23,
 229: 15,
 233: 8,
 249: 10}


def opcode_cost(z):
    """T-states of the instruction at PC, or None when not covered."""
    op = z.rd(z.pc)
    if op == 0xED:
        second = z.rd((z.pc + 1) & 0xFFFF)
        if second in ED_BLOCK:
            count = (z.b << 8) | z.c
            if count == 0:
                count = 0x10000
            return 21 * count - 5
        return ED_T.get(second)
    if op == 0xCB:
        second = z.rd((z.pc + 1) & 0xFFFF)
        return 12 if (second & 7) == 6 else 8
    if op in (0xDD, 0xFD):
        second = z.rd((z.pc + 1) & 0xFFFF)
        if second == 0xCB:
            return 23
        return DD_T.get(second)
    return ONE_BYTE_T.get(op)


class IpcTracer:
    """Per-instruction hook: latch trace, LDIR trace, T-states, CTC0."""

    def __init__(self, syms, page_bits, slot_regions, stacks,
                 domain_base=DOMAIN_BASE, domain_end=DOMAIN_END):
        self.syms = syms
        self.stacks = tuple(stacks)
        self.page_bits = tuple(page_bits)
        self.slot_regions = tuple(slot_regions)
        self.domain_base = domain_base
        self.domain_end = domain_end
        # The correction-01 aperture stub's own code range: the only
        # place a mode-10 delivery can be taken, and the only place an
        # aperture LDIR can be fetched from.
        self.ape_base = syms.get("ipc_common_begin")
        self.ape_end = syms.get("ipc_common_end")
        self.marks = {}
        self.mark_counters = {}
        self.phase_order = []
        self.tx = b""
        self._tx_len = 0
        self._scanned = 0
        self.tstates = 0
        self.steps = 0
        self.missing_table = set()
        self.latch_writes = []
        self.intervals = []
        self.open = None
        self.ldirs = []
        self.ctc_armed = False
        self.ctc_control = False
        self.injections = []
        self.armed = False
        self.armed_aperture = False
        self.aperture_writes = 0
        self.retis = []
        self.syscalls = []        # {t, number, block, latch, ret} at the gate
        self._pending = None      # syscall in progress (for its span)
        self.sp_min = {}          # reserved stack -> lowest SP seen in it
        self.domain_writes = 0    # latch writes that open the service domain
        self.latch_current = OS_MODE
        self.current = None       # innermost open mapping node
        self.open = None          # innermost open service-domain node
        self.pending = []         # open nodes, outermost first
        self.excursions = []      # every node, nested ones included
        self.client_start = None  # tstates of the client's banner line

    def install(self, z):
        saved = z.io_out

        def io_out(port, value):
            if (port & 0xFF) == MEMCTL_PORT:
                self._latch_write((z.pc - 2) & 0xFFFF, value & 0xFF)
            elif (port & 0xFF) == CTC0_PORT:
                self._ctc_write(value & 0xFF)
            return saved(port, value)

        z.io_out = io_out

    def _ctc_write(self, value):
        """The kernel's CTC0 helper: vector base, control word, constant.

        The channel starts on the write that follows the control word, so
        the stand-in arms on that one write only: the vector base and the
        time constant share a value on this platform, and only the control
        word separates a fresh arming from an address write."""
        if value == CTC0_CONTROL:
            self.ctc_armed = True
            self.ctc_control = True
            return
        if not self.ctc_control:
            return
        self.ctc_control = False
        if value == CTC0_APERTURE_TIME_CONSTANT:
            # Correction 01's short period: the delivery is modelled
            # inside the next path-B aperture window.
            self.armed_aperture = True
        elif value == CTC0_TIME_CONSTANT:
            self.armed = True

    def _latch_write(self, pc, value):
        """One MEMCTL write, as a node of the save/restore tree.

        The trace is not a sequence of intervals: a call opens the service
        domain (mode 11, service page) and correction 01's copy gate opens
        a mode-10 identity excursion *inside* it once per staged chunk.
        Every write that changes the mapping is therefore matched against
        the innermost excursion still waiting for the value it replaced: a
        write of exactly that value closes it, anything else nests inside
        it.  `intervals` keeps the service-domain excursions - the nodes a
        call or the loader opened - so the domain queries keep their
        meaning, and `excursions` carries the nested ones too."""
        value &= 0xFF
        self.latch_writes.append({"t": self.tstates, "pc": pc,
                                  "value": value})
        if (value & PAGE_MASK) and (value & MODE_MASK) == OS_MODE:
            self.domain_writes += 1
        if self.is_aperture_latch(value):
            # Correction 01: the path-B aperture window.  It is
            # nucleus-internal (no application ever holds page 1), it
            # keeps the service's bank reachable in 8000h-BFFFh, and it
            # does not close the surrounding service-domain interval: the
            # service's data stays mapped, which is all a copy needs.
            self.aperture_writes += 1
        if value == self.latch_current:
            # A redundant write: the mapping did not change.
            return
        if self.pending and self.pending[-1]["before"] == value:
            # The mode-00 ROM window the recovery store uses and the
            # gate's mode-10 identity chunks are both undone by a write
            # of the value they replaced; that write closes the node.
            closed = self.pending.pop()
            closed["close_t"] = self.tstates
            closed["close_value"] = value
            closed["close_pc"] = pc
        else:
            self.open = {"open_t": self.tstates, "open_value": value,
                         "open_pc": pc, "tstates": 0, "steps": 0,
                         "ldir_bytes": 0, "inject": None, "close_t": None,
                         "close_value": None, "close_pc": None,
                         "before": self.latch_current,
                         "domain": ((value & MODE_MASK) == OS_MODE and
                                    bool(value & PAGE_MASK))}
            self.pending.append(self.open)
            self.excursions.append(self.open)
            if self.open["domain"]:
                self.intervals.append(self.open)
        self.latch_current = value
        # The innermost open node, and the innermost one that maps the
        # service domain: `open` is the latter, so a delivery or a copy is
        # attributed to the call that owns the domain, not to the chunk
        # mapping nested inside it.
        self.current = self.pending[-1] if self.pending else None
        self.open = next((entry for entry in reversed(self.pending)
                          if entry["domain"]), None)

    def watch(self, z):
        if z.pc == self.syms["zos_syscall_entry"]:
            # The gate's return PC is on top of the caller's stack: the
            # syscall's kernel-side span runs until the gate returns to it.
            entry = {"t": self.tstates, "number": z.a, "block": z.hl,
                     "latch": z.latch,
                     "ret": z.rd(z.sp) | (z.rd(z.sp + 1) << 8),
                     # Snapshot the argument block as the caller sees it
                     # at entry: the program's memory is reused later.
                     "bytes": bytes(z.rd((z.hl + i) & 0xFFFF)
                                    for i in range(4))}
            self.syscalls.append(entry)
            self._pending = entry
        elif (self._pending is not None and
                z.pc == self._pending["ret"]):
            self._pending["span"] = self.tstates - self._pending["t"]
            self._pending = None
        for name, base, top in self.stacks:
            if base <= z.sp <= top:
                low = self.sp_min.get(name)
                if low is None or z.sp < low:
                    self.sp_min[name] = z.sp
        if len(z.sio_tx) != self._tx_len:
            self._tx_len = len(z.sio_tx)
            self.tx = bytes(z.sio_tx)
            if self.client_start is None and CLIENT_START in self.tx:
                # The client's banner goes out before its first lookup, so
                # it separates the two programs' censuses (correction 01
                # boots the client from the probe through zos_exec).
                self.client_start = self.tstates
            tail = self.tx[self._scanned:]
            for match in MARK_RE.finditer(tail):
                name = match.group(1).decode()
                suffix = match.group(2).decode()
                if (name, suffix) not in self.marks:
                    self.marks[(name, suffix)] = self.tstates
                    self.mark_counters[(name, suffix)] = {
                        key: z.rd(self.syms["_" + key]) |
                        (z.rd(self.syms["_" + key] + 1) << 8)
                        for key in COPY_COUNTERS
                        if "_" + key in self.syms}
                    if suffix == "a":
                        self.phase_order.append(name)
            # Rescan only the trailing partial line: a marker is printed
            # byte by byte, so its start can be arbitrarily far back once
            # the next burst (a 256-byte console pattern) has arrived.
            end = self.tx.rfind(b"\r\n", self._scanned)
            if end >= 0:
                self._scanned = end + 2

        if (self.armed_aperture and z.iff1 and not z.int_pending and
                (z.latch & MODE_MASK) == APPLICATION_MODE and
                (z.latch & PAGE_MASK) == PAGE_ONE and
                self.ape_base is not None and
                self.ape_base <= z.pc < self.ape_end):
            # Correction 01: the delivery arrives while the copy primitive
            # holds its mode-10 aperture window open.  That is the case the
            # IRQ gate has to survive: the interrupted stack is the common
            # reservation, bank 7 is not mapped, and the window's own state
            # must come back byte for byte.
            z.assert_int(CTC0_VECTOR)
            self.armed_aperture = False
            entry = {"t": self.tstates, "pc": z.pc, "latch": z.latch,
                     "sp": z.sp, "aperture": True}
            self.injections.append(entry)
            if self.open is not None:
                self.open["inject_aperture"] = entry
        elif (self.armed and z.iff1 and not z.int_pending and
                (z.latch & PAGE_MASK) and
                self.domain_base <= z.pc < self.domain_end):
            # The C6 situation: the request arrives while the service itself
            # is running with interrupts enabled (its domain is open, so the
            # program counter is resident code, not the caller).  The CTC
            # helper's time constant started the count, so the stand-in
            # delivers the pending vector here instead of on a real timer.
            z.assert_int(CTC0_VECTOR)
            self.armed = False
            self.injections.append({"t": self.tstates, "pc": z.pc,
                                    "latch": z.latch, "sp": z.sp,
                                    "aperture": False})
            if self.open is not None:
                self.open["inject"] = self.injections[-1]

        cost = opcode_cost(z)
        if cost is None:
            self.missing_table.add((z.rd(z.pc),
                                    z.rd((z.pc + 1) & 0xFFFF)))
            cost = 0
        # A correction-01 aperture excursion may be nested inside the
        # service-domain node.  Attribute it to that surrounding domain.
        in_domain = self.open is not None
        if in_domain:
            self.open["tstates"] += cost
            self.open["steps"] += 1

        op = z.rd(z.pc)
        if op == 0xED and z.rd((z.pc + 1) & 0xFFFF) == 0x4D:
            # The kernel's own ISR epilogue must be the one that ends the
            # interrupt: the service handler runs inside it.
            self.retis.append({"t": self.tstates, "pc": z.pc})
        if op == 0xED and z.rd((z.pc + 1) & 0xFFFF) == 0xB0:
            count = (z.b << 8) | z.c
            count = count if count else 0x10000
            hl = (z.h << 8) | z.l
            de = (z.d << 8) | z.e
            self.ldirs.append({"t": self.tstates, "pc": z.pc, "count": count,
                               "src": hl, "dst": de, "latch": z.latch,
                               "in_domain": in_domain,
                               "in_aperture":
                                   self.is_aperture_latch(z.latch)})
            if in_domain:
                self.open["ldir_bytes"] += count

        self.tstates += cost
        self.steps += 1

    # --- queries ---------------------------------------------------------
    def mark(self, name, suffix):
        return self.marks.get((name, suffix))

    def per_call(self, name):
        """Per-call T-states of one phase, from the two marker segments."""
        a, b, c = (self.mark(name, suffix) for suffix in "abc")
        if a is None or b is None or c is None:
            return None
        return ((c - b) - (b - a)) / float(N2 - N1)

    def measured_call(self, name):
        """Per-call T-states of the *measured* segment of one phase."""
        b, c = self.mark(name, "b"), self.mark(name, "c")
        if b is None or c is None:
            return None
        return (c - b) / float(N2)

    def warmup_call(self, name):
        """Per-call T-states of the warm-up segment of one phase."""
        a, b = self.mark(name, "a"), self.mark(name, "b")
        if a is None or b is None:
            return None
        return (b - a) / float(N1)

    def segments(self, name):
        a, b, c = (self.mark(name, suffix) for suffix in "abc")
        return (b - a) / float(N1), (c - b) / float(N2)

    def intervals_between(self, t0, t1):
        return [entry for entry in self.intervals
                if t0 is not None and t0 <= entry["open_t"] <= t1]

    def interval_for_phase(self, name):
        t0, t1 = self.mark(name, "a"), self.mark(name, "c")
        return self.intervals_between(t0, t1)

    def region(self, address, base, size):
        return base <= address < base + size

    def ldirs_in_domain(self):
        return [entry for entry in self.ldirs if entry["in_domain"]]

    def ldirs_in_aperture(self):
        return [entry for entry in self.ldirs if entry["in_aperture"]]

    @staticmethod
    def is_aperture_latch(value):
        """Mode 10 with the aperture page up: nucleus-internal only."""
        return ((value & MODE_MASK) == APPLICATION_MODE and
                (value & PAGE_MASK) == PAGE_ONE)

    def aperture_installs(self, t0, t1):
        """The path-B primitive's own latch writes: the installs.

        Only the aperture stub's code writes a mode-10 latch with page 1
        from inside its own range.  The IRQ gate restores the same value
        from its own common entry, which is exactly the restore the
        interrupt case has to prove."""
        if self.ape_base is None:
            return []
        return [write for write in self.latch_writes
                if self.is_aperture_latch(write["value"]) and
                self.ape_base <= write["pc"] < self.ape_end and
                t0 is not None and t0 <= write["t"] <= t1]

    def aperture_ldirs(self, t0, t1):
        return [entry for entry in self.ldirs_in_aperture()
                if t0 is not None and t0 <= entry["t"] <= t1]

    def restore_of(self, write):
        """The write that restores the latch a stub install replaced.

        The install's own `ld a,(saved)`/`out` pair must put back exactly
        what was in force immediately before it, so the invariant is
        checked against the write trace, not against a copy of the value.
        """
        before = [entry for entry in self.latch_writes
                  if entry["t"] < write["t"]]
        if not before:
            return None
        previous = before[-1]["value"]
        after = [entry for entry in self.latch_writes
                 if entry["t"] > write["t"] and entry["value"] == previous]
        return after[0] if after else None

    def counters_at_mark(self, name, suffix):
        """The resolver's copy-path counters where a marker was printed.

        The counters are fixed data (they must be writable while the
        window shows another bank) and a marker is printed inside a
        syscall, so the mapping at that instant reads them directly."""
        return self.mark_counters.get((name, suffix))

    def counter_deltas(self, name):
        """Per-path counter deltas across one phase's measured segment."""
        before = self.counters_at_mark(name, "b")
        after = self.counters_at_mark(name, "c")
        if before is None or after is None:
            return None
        return {key: after[key] - before[key] for key in before}

    def probe_path(self, name):
        """Which copy path a phase's counter deltas name."""
        deltas = self.counter_deltas(name)
        if deltas is None:
            return None
        if deltas.get("ipc_copy_aperture"):
            return "aperture"
        # The console-service phase markers make a couple of independent
        # slow-path copies around every measured segment.  A probe whose
        # payload itself takes the slow path contributes PROBE_N2 more.
        if deltas.get("ipc_copy_slow", 0) >= PROBE_N2:
            return "slow"
        if deltas.get("ipc_copy_window"):
            return "window"
        return None

    def domain_entry_after(self, t0):
        """Tick of the first service-domain entry after an injection.

        The ISR executive may write the latch itself before the trampoline
        runs, so the raw write sequence is the only honest source: the
        first mode-11 write with page bits after the injection is the
        trampoline's `ipc_domain_enter`."""
        for write in self.latch_writes:
            if write["t"] >= t0 and (write["value"] & PAGE_MASK) and \
                    (write["value"] & MODE_MASK) == OS_MODE:
                return write["t"]
        return None

    def touches_slot(self, entry, base, size):
        end = entry["count"]
        for start in (entry["src"], entry["dst"]):
            if start + end <= 0x10000 and start >= base and \
                    start + end <= base + size:
                return True
        return False


class IpcSimBase(unittest.TestCase):
    SUBDIR = IMAGE_SUBDIR
    IMAGE = IMAGE_NAME

    def setUp(self):
        self.bin_path = os.path.join(ROOT, self.SUBDIR, self.IMAGE)
        if not os.path.exists(self.bin_path):
            self.skipTest("%s missing; run make ipc-selftest"
                          % self.bin_path)
        with open(self.bin_path, "rb") as handle:
            self.image = handle.read()
        self.syms = read_symbols(os.path.join(ROOT, self.SUBDIR,
                                              "zephyros.map"))
        self.z = None
        self.tracer = None
        self.image_ident = self.SUBDIR

    def make_cpu(self):
        pages = {n: self.image[n * 0x10000:(n + 1) * 0x10000]
                 for n in range(8)}
        z = Z80(pages[0], pages[1],
                rom_pages={n: pages[n] for n in range(2, 8)})
        z.latch = OS_MODE
        z.sp = STACK
        return z

    def reserved_stacks(self):
        # Half-open byte ranges: the service stack ends where the ISR
        # stack starts (5F00h), and a push reaching the region's top is
        # still inside it.
        return (
            ("service", self.syms["zos_service_stack"],
             self.syms["zos_service_stack_top"] - 1),
            ("isr", self.syms["irq_isr_stack"] + 1,
             self.syms["irq_isr_stack_top"] - 1),
            ("gate", self.syms["zos_gate_stack"],
             self.syms["zos_gate_stack_top"] - 1),
        )

    def crt0_untraced(self, z):
        z.push16(RET)
        z.pc = self.syms["__zos_crt0_init"]
        for _ in range(2000000):
            z.step()
            if z.pc == RET:
                return
        raise AssertionError("C runtime init did not return")

    def boot(self):
        """One boot serves every assertion: the image runs the whole
        surface in a single pass."""
        if getattr(self, "_booted", None) is not None:
            return self._booted
        z = self.make_cpu()
        tracer = IpcTracer(self.syms, (SERVICE_PAGE_BITS,),
                           ((SERVICE_BASE, SERVICE_END),),
                           self.reserved_stacks())
        tracer.install(z)
        self.crt0_untraced(z)
        z.push16(RET)
        z.pc = self.syms["_kernel_main"]
        delay = self.syms["_kernel_serial_connect_delay"]
        tx_len = len(z.sio_tx)
        for _ in range(STEP_LIMIT):
            skip_serial_connect_delay(z, delay)
            tracer.watch(z)
            z.step()
            if len(z.sio_tx) != tx_len:
                tx_len = len(z.sio_tx)
                if PANIC in bytes(z.sio_tx)[-96:]:
                    raise AssertionError("panic during boot: %r"
                                         % bytes(z.sio_tx)[-300:])
                if LOADER_OK in bytes(z.sio_tx)[-96:]:
                    break
        else:
            raise AssertionError(
                "the loader never placed the services (PC=%04X, tail=%r)"
                % (z.pc, bytes(z.sio_tx)[-300:]))
        # The probe boots the client and prints the image's final verdict
        # after it exits (correction 01), so the run ends there.
        for _ in range(STEP_LIMIT):
            tracer.watch(z)
            z.step()
            done = APERTURE_PASS in tracer.tx or APERTURE_FAIL in tracer.tx
            caller_mapping = ((z.latch & MODE_MASK) == APPLICATION_MODE and
                              (z.latch & PAGE_MASK) == 0)
            if (done and caller_mapping and tracer.open is None) or \
                    PANIC in tracer.tx:
                break
        else:
            raise AssertionError(
                "the probe never finished (PC=%04X, tail=%r)"
                % (z.pc, tracer.tx[-300:]))
        self.z = z
        self.tracer = tracer
        self._booted = (z, tracer)
        return z, tracer

    def transcript(self):
        return self.boot()[1].tx

    def geometry(self):
        """The build's generated memory geometry (ZOS_* constants)."""
        values = {}
        with open(os.path.join(ROOT, self.SUBDIR, "geometry",
                               "memory.inc")) as handle:
            for line in handle:
                match = re.match(r"^ZOS_([A-Z0-9_]+)\s*=\s*0x([0-9A-Fa-f]+)",
                                 line)
                if match:
                    values[match.group(1)] = int(match.group(2), 16)
        return values

    def phase_calls(self, number):
        """Syscall entries with one number, in order."""
        return [entry for entry in self.tracer.syscalls
                if entry["number"] == number]


class TestIpcAcceptance(IpcSimBase):
    """The promoted conformance suite passes and its evidence holds."""

    def test_client_passes_the_promoted_suite(self):
        tx = self.transcript()
        for line in (b"ZephyrOS 0.0\r\n", b"platform: Zephyr-80\r\n",
                     b"kernel entry: OK\r\n", b"serial: OK\r\n"):
            self.assertIn(line, tx)
        self.assertIn(LOADER_OK, tx)
        self.assertIn(b"ipc selftest: client OK\r\n", tx)
        self.assertNotIn(FAIL, tx, tx[-400:].decode("latin-1"))
        self.assertNotIn(PANIC, tx)
        self.assertIn(PASS, tx)
        match = COUNT_RE.search(tx)
        self.assertIsNotNone(match, tx[-200:].decode("latin-1"))
        passed, total = (int(group) for group in match.groups())
        self.assertGreaterEqual(total, 49,
                                "the suite must carry >= 49 checks")
        self.assertEqual(passed, total)
        print("ipc %s: %d/%d checks, six services in bank 6"
              % (self.image_ident, passed, total))

    def test_every_phase_boundary_is_marked(self):
        tracer = self.boot()[1]
        for code, name in sorted(PHASES.items()):
            for suffix in "abc":
                self.assertIsNotNone(
                    tracer.mark(code, suffix),
                    "phase %s (%s) has no %s marker" % (code, name, suffix))

    def test_no_marker_bytes_came_from_the_console_pattern(self):
        """The phase codes are printable letters and the payload bytes are
        >= 80h, so a marker can only come from phase_mark(): exactly three
        markers per phase, in a/b/c order.  Both programs write them - the
        client in lower case, the caller-buffer probe that boots it in
        upper case (correction 01)."""
        tracer = self.boot()[1]
        codes = sorted(set(PHASES) | set(PROBE_PHASES))
        self.assertEqual(len(tracer.marks), len(codes) * 3)
        for code in codes:
            a, b, c = (tracer.marks[(code, suffix)] for suffix in "abc")
            self.assertLess(a, b)
            self.assertLess(b, c)


class TestIpcMeasurement(IpcSimBase):
    """Per-call T-states, copies and the domain intervals they run in."""

    def test_per_call_spans(self):
        tracer = self.boot()[1]
        spans = {}
        warm = {}
        for code, name in sorted(PHASES.items()):
            spans[name] = tracer.measured_call(code)
            warm[name] = tracer.warmup_call(code)
            self.assertIsNotNone(spans[name], name)
            self.assertGreater(spans[name], 0, name)
        null = spans["null-call"]
        chain = spans["vfs-512-direct"]
        staged = spans["vfs-512-staged"]
        baseline = spans["fsread-baseline"]
        # The 512-byte chain crosses vfs -> fat -> ioc and back, so it
        # costs far more than the empty protocol round trip, and far less
        # than today's direct filesystem read of the same chunk.
        self.assertGreater(chain, null * 3,
                           "chain %.1f vs null %.1f" % (chain, null))
        self.assertLess(chain, baseline * 0.5,
                        "chain %.1f vs direct %.1f" % (chain, baseline))
        # Staging (the block travels through the providers' buffers)
        # costs more than the direct descriptor hand-off and less than
        # the direct path.
        self.assertGreater(staged, chain)
        self.assertLess(staged, baseline)
        self.assertLess(null, 40000.0)
        for name in ("vfs-512-direct", "vfs-512-staged", "vfs-512-repeat"):
            self.assertLess(spans[name], 250000.0, name)
        print("ipc %s: per call (measured/warm-up, T):" % self.image_ident)
        for code, name in sorted(PHASES.items(), key=lambda item: item[1]):
            print("   %-18s %10.1f / %10.1f%s"
                  % (name, spans[name], warm[name],
                     "  (%.1f T/byte)" % (spans[name] / IPC_BLOCK_BYTES)
                     if name.startswith("vfs-512") else ""))
        print("   null call %.1f T, chain/direct ratio %.2f"
              % (null, chain / baseline))

    def test_kernel_side_syscall_spans(self):
        """The gate-to-return span of the IPC syscalls: the kernel's own
        share of a call, measured from the gate entry to the return, and
        split per phase so the client-side cost is the difference."""
        tracer = self.boot()[1]
        calls = [entry for entry in tracer.syscalls
                 if entry["number"] == CALL_SYSCALL and "span" in entry]
        self.assertTrue(calls)
        self.assertTrue(all(entry["span"] > 0 for entry in calls))
        for code, name in sorted(PHASES.items()):
            a, c = tracer.mark(code, "a"), tracer.mark(code, "c")
            b = tracer.mark(code, "b")
            inside = [entry for entry in calls if a <= entry["t"] <= c]
            if code in DIRECT_PHASES:
                # The body makes no IPC call.  Up to eight calls belong to
                # the trailing marker, now written through console IPC.
                self.assertLessEqual(len(inside), 8, name)
                continue
            measured = [entry for entry in inside if entry["t"] >= b]
            self.assertTrue(measured, name)
            kernel = sum(entry["span"] for entry in measured) / float(
                len(measured))
            print("ipc %s: %-18s kernel %.1f T of %.1f T per call"
                  % (self.image_ident, name, kernel,
                     tracer.measured_call(code)))

    def test_measured_phases_issue_the_expected_calls(self):
        tracer = self.boot()[1]
        calls = [entry for entry in tracer.syscalls
                 if entry["number"] == CALL_SYSCALL]
        # 8 warm-up + 24 measured calls per IPC phase, plus the single-call
        # checks around them.  The fsread body uses the FS syscall; only
        # its boundary marker reaches IPC through the console service.
        self.assertGreaterEqual(len(calls), len(IPC_PHASES) * (N1 + N2))
        marks = tracer.marks
        census = {}
        for code, name in sorted(PHASES.items()):
            a = marks[(code, "a")]
            c = marks[(code, "c")]
            inside = [entry for entry in calls if a <= entry["t"] <= c]
            census[name] = len(inside)
            if code in DIRECT_PHASES:
                self.assertLessEqual(len(inside), 8, name)
            else:
                self.assertGreaterEqual(len(inside), N1 + N2, name)
        print("ipc %s: SYS_IPC_CALL per phase %s"
              % (self.image_ident, census))

    def test_descriptor_copies_run_through_the_domain(self):
        tracer = self.boot()[1]
        copies = tracer.ldirs_in_domain()
        self.assertTrue(copies, "no window LDIR ran in the domain")
        for code in COPY_PHASES:
            self.assertTrue(
                any(entry["ldir_bytes"] > 0
                    for entry in tracer.interval_for_phase(code)),
                "phase %s copied no bytes in the domain" % PHASES[code])
        total = sum(entry["count"] for entry in copies)
        biggest = max(entry["count"] for entry in copies)
        self.assertGreaterEqual(biggest, IPC_BLOCK_BYTES)
        touched = [entry for entry in copies
                   if tracer.touches_slot(entry, SERVICE_BASE, SERVICE_END
                                          - SERVICE_BASE)]
        self.assertTrue(touched, "no domain copy touched the service image")
        print("ipc %s: %d window LDIRs (%d bytes, largest %d) in the domain"
              % (self.image_ident, len(copies), total, biggest))

    def test_domain_intervals_open_on_the_service_page_only(self):
        tracer = self.boot()[1]
        self.assertTrue(tracer.intervals)
        for interval in tracer.intervals:
            self.assertEqual(interval["open_value"] & PAGE_MASK,
                             SERVICE_PAGE_BITS)
            self.assertEqual(interval["open_value"] & MODE_MASK, OS_MODE)
        service = self.tracer.interval_for_phase("r")
        self.assertTrue(service, "the 512-byte read opened no domain")
        self.assertTrue(any(entry["ldir_bytes"] >= IPC_BLOCK_BYTES
                            for entry in service))


class TestIpcLatchAndIrq(IpcSimBase):
    """The latch discipline: exact restore across calls and interrupts."""

    def latch_before(self, entry):
        """The latch value in force immediately before this interval.

        The loader opens the domain too (it runs the service inits through
        the upcall shim, which selects the page before any syscall), so the
        write trace - not the syscall list - is the honest source."""
        candidates = [write for write in self.tracer.latch_writes
                      if write["t"] < entry["open_t"]]
        self.assertTrue(candidates)
        return candidates[-1]["value"]

    def test_every_interval_restores_the_latch_it_found(self):
        """No mapping change survives a service call: every service-domain
        interval closed on the exact latch that was in force before it -
        correction 01's copy gate opens a mode-10 identity chunk inside
        the domain once per staged block, and those close inside it - and
        no call returns with the caller holding anything but the latch it
        called with.  The loader's own intervals, opened without a syscall
        around them, close the same way."""
        tracer = self.boot()[1]
        self.assertTrue(tracer.intervals)
        self.assertTrue(tracer.excursions)
        spans = [(entry["t"], entry["t"] + entry["span"])
                 for entry in tracer.syscalls if "span" in entry]
        self.assertTrue(spans, "no syscall span was measured")
        for interval in tracer.intervals:
            self.assertEqual(interval["open_value"] & PAGE_MASK,
                             SERVICE_PAGE_BITS)
            self.assertEqual(interval["open_value"] & MODE_MASK, OS_MODE)
            self.assertIsNotNone(interval["close_t"],
                                 "a domain interval never closed")
            self.assertEqual(interval["close_value"],
                             self.latch_before(interval),
                             "interval opened at %d closed with %02Xh, not "
                             "the latch in force before it (%02Xh)"
                             % (interval["open_t"], interval["close_value"],
                                self.latch_before(interval)))
        # A call never outlives its own domain: the interval the gate opens
        # closes before the call returns to the caller.
        inside = [interval for interval in tracer.intervals
                  if any(t0 <= interval["open_t"] <= t1
                         for t0, t1 in spans)]
        self.assertTrue(inside, "no interval came from a syscall")
        for interval in inside:
            ends = [t1 for t0, t1 in spans
                    if t0 <= interval["open_t"] <= t1]
            self.assertLessEqual(interval["close_t"], max(ends),
                                 "the domain of the call at %d outlived it"
                                 % interval["open_t"])
        # And every call hands the caller's latch back, not the service's.
        for entry in tracer.syscalls:
            if "span" not in entry:
                continue
            writes = [write for write in tracer.latch_writes
                      if entry["t"] <= write["t"] <= entry["t"] + entry["span"]]
            self.assertTrue(writes, "the call at %d wrote no latch"
                            % entry["t"])
            self.assertEqual(writes[-1]["value"], entry["latch"],
                             "the call at %d returned with %02Xh, not the "
                             "caller's %02Xh" % (entry["t"],
                                                 writes[-1]["value"],
                                                 entry["latch"]))
        print("ipc %s: %d domain intervals among %d mapping excursions, all "
              "restored, %d calls returned their caller's latch"
              % (self.image_ident, len(tracer.intervals),
                 len(tracer.excursions), len(spans)))

    def test_the_gate_restores_the_callers_latch(self):
        """Each SYS_IPC_CALL leaves the machine with the *caller's* whole
        latch: mode 10 over its own bank, page bits clear."""
        tracer = self.boot()[1]
        calls = [entry for entry in tracer.syscalls
                 if entry["number"] == CALL_SYSCALL and "span" in entry]
        self.assertTrue(calls)
        for entry in calls:
            restored = [write for write in tracer.latch_writes
                        if entry["t"] <= write["t"] <= entry["t"]
                        + entry["span"]]
            self.assertTrue(restored, "no latch write in the syscall at %d"
                            % entry["t"])
            self.assertEqual(restored[-1]["value"], entry["latch"],
                             "syscall at %d returned with %02Xh, not the "
                             "caller's %02Xh" % (entry["t"],
                                                 restored[-1]["value"],
                                                 entry["latch"]))
            self.assertEqual(entry["latch"] & MODE_MASK, APPLICATION_MODE)
            self.assertEqual(entry["latch"] & PAGE_MASK, 0)
        print("ipc %s: %d SYS_IPC_CALL returns, each with the caller's "
              "latch (mode 10, bank %d)" % (self.image_ident, len(calls),
                                            calls[0]["latch"] & BANK_MASK))

    def test_ctc0_is_delivered_while_a_call_is_in_progress(self):
        tracer = self.boot()[1]
        # Three SPIN calls arm the platform profile's timer: the unclaimed
        # one (the executive quiesces a delivery no endpoint owns), then
        # the two the service handler answers.  The probe's own deliveries
        # come from correction 01's short time constant and land inside
        # the path-B window; TestIpcCorrection01 owns that case.
        injections = [entry for entry in tracer.injections
                      if not entry["aperture"]]
        self.assertEqual(len(injections), 3,
                         "one delivery per CTC0 pass, got %r"
                         % tracer.injections)
        for injection in injections:
            self.assertTrue(injection["latch"] & PAGE_MASK,
                            "the injection did not land in the domain")
            self.assertTrue(DOMAIN_BASE <= injection["pc"] < DOMAIN_END,
                            "injected at %04X, outside the domain"
                            % injection["pc"])
            open_interval = next(
                (entry for entry in tracer.intervals
                 if entry["open_t"] <= injection["t"] and
                 (entry["close_t"] is None or
                  injection["t"] <= entry["close_t"])),
                None)
            self.assertIsNotNone(open_interval,
                                 "no call was open at %d" % injection["t"])
            # The interrupted call must still close inside itself, on the
            # latch it found: the delivery must not disturb the restore.
            calls = [entry for entry in tracer.syscalls
                     if "span" in entry and entry["t"] <= injection["t"] <=
                     entry["t"] + entry["span"]]
            self.assertTrue(calls, "the delivery landed outside every call")
            self.assertLessEqual(open_interval["close_t"],
                                 calls[0]["t"] + calls[0]["span"],
                                 "the interrupted call outlived its domain")
            self.assertEqual(open_interval["close_value"],
                             self.latch_before(open_interval))

    def test_the_nucleus_keeps_the_final_reti(self):
        tracer = self.boot()[1]
        self.assertEqual(len(tracer.retis), len(tracer.injections),
                         tracer.retis)
        for reti in tracer.retis:
            self.assertFalse(DOMAIN_BASE <= reti["pc"] < DOMAIN_END,
                             "RETI at %04X is service code" % reti["pc"])
        print("ipc %s: %d CTC0 deliveries at %s, RETIs at %s"
              % (self.image_ident, len(tracer.injections),
                 ["%04X" % entry["pc"] for entry in tracer.injections],
                 ["%04X" % entry["pc"] for entry in tracer.retis]))


class TestIpcCorrection01(IpcSimBase):
    """Correction 01: the copy path every caller-buffer location takes.

    The probe program runs five addresses through the chained 512-byte
    read and a single descriptor copy_out, one service-side slow case and
    one path-B copy with the timer armed.  The replay attributes each case
    to a path from the resolver's own counters (read out of the image at
    the phase markers) and from the latch trace, measures the per-case
    T-states against package 35's 165.6 and M9's 587.8 T/byte, and proves
    the interrupt case: a CTC0 delivery taken inside the mode-10 aperture
    window is restored, window and all, by the IRQ gate.
    """

    def test_probe_passes_the_caller_buffer_matrix(self):
        tracer = self.boot()[1]
        self.assertIn(b"ipccopy: PASS\r\n", tracer.tx)
        self.assertNotIn(b"ipccopy: FAIL\r\n", tracer.tx)
        for code in PROBE_PHASES:
            for suffix in "abc":
                self.assertIn((code, suffix), tracer.marks,
                              "probe phase %s%c was never marked"
                              % (code, suffix))
        print("ipc %s: probe verdict %r"
              % (self.image_ident,
                 [line for line in tracer.tx.decode("latin1").split("\r\n")
                  if line.startswith("ipccopy: ")][-2:]))

    def test_each_caller_address_takes_its_documented_path(self):
        tracer = self.boot()[1]
        census = {}
        for code, (name, address, path) in sorted(PROBE_PHASES.items()):
            t0, t1 = tracer.mark(code, "b"), tracer.mark(code, "c")
            deltas = tracer.counter_deltas(code)
            self.assertIsNotNone(deltas, name)
            picked = tracer.probe_path(code)
            self.assertEqual(picked, path,
                             "%s (%04Xh) took path %s, expected %s "
                             "(deltas %r)" % (name, address, picked, path,
                                              deltas))
            census[name] = (address, path, deltas)
            installs = tracer.aperture_installs(t0, t1)
            ldirs = tracer.aperture_ldirs(t0, t1)
            if path == "aperture":
                self.assertTrue(installs,
                                "%s never installed the aperture latch"
                                % name)
                self.assertTrue(ldirs,
                                "%s copied nothing through the aperture"
                                % name)
                for install in installs:
                    self.assertEqual(install["value"] & BANK_MASK,
                                     install["value"] & BANK_MASK)
                    restore = tracer.restore_of(install)
                    self.assertIsNotNone(
                        restore,
                        "%s installed %02Xh and never restored the latch "
                        "in force before it"
                        % (name, install["value"]))
            else:
                self.assertFalse(installs,
                                 "%s took the aperture path" % name)
                self.assertFalse(ldirs,
                                 "%s copied through the aperture" % name)
        print("ipc %s: caller-buffer path census %s"
              % (self.image_ident,
                 ", ".join("%s=%s" % (name, path)
                           for name, (_, path, _) in sorted(census.items()))))

    def test_service_io_rule_counts_the_out_of_range_case_slow(self):
        tracer = self.boot()[1]
        # The U case is the only one whose caller buffer is in the window
        # (path A as an address), so its slow path can only come from the
        # service's own buffer sitting outside 8000h-BFFFh.
        deltas = tracer.counter_deltas("U")
        self.assertTrue(deltas["ipc_copy_slow"],
                        "the service-side slow case took a fast path: %r"
                        % deltas)
        self.assertEqual(deltas["ipc_copy_aperture"], 0)
        # Its bytes went through the console, so the pattern is on the wire
        # sixteen bytes per call.
        marker = b"".join(bytes([PROBE_PATTERN_BASE + (i & 0x3F)])
                          for i in range(16))
        tx = tracer.tx
        start = tracer.mark("U", "b")
        self.assertGreaterEqual(tx.count(marker), 2,
                                "the slow case's bytes never reached the "
                                "console")

    def test_per_case_t_states_against_the_package_and_m9_baselines(self):
        tracer = self.boot()[1]
        table = {}
        for code, (name, _address, path) in sorted(PROBE_PHASES.items()):
            b, c = tracer.mark(code, "b"), tracer.mark(code, "c")
            per_call = (c - b) / float(PROBE_N2)
            table[name] = (path, per_call,
                           per_call / float(PROBE_PHASE_BYTES[name]))
        for name, (path, per_call, per_byte) in sorted(table.items()):
            print("ipc %s: case %-15s %-8s %10.1f T/call %7.1f T/byte"
                  % (self.image_ident, name, path, per_call, per_byte))
        # Package 35's single measured figure (165.6 T/byte) was the
        # window path; M9's baseline on the same chunk is 587.8 T/byte.
        window = table["window-read"]
        aperture = table["high1-read"]
        slow = table["behind-read"]
        self.assertLess(window[2], 250.0,
                        "the window path is %.1f T/byte" % window[2])
        self.assertLess(aperture[2], 250.0,
                        "the aperture path is %.1f T/byte" % aperture[2])
        self.assertLess(slow[2], 587.8,
                        "the chunked path is %.1f T/byte, over the M9 "
                        "baseline" % slow[2])
        self.assertGreater(slow[2], window[2],
                           "the chunked path is not slower than the window "
                           "path")
        self.assertLess(abs(window[2] - 165.6), 40.0,
                        "the window path moved to %.1f T/byte from package "
                        "35's 165.6" % window[2])
        self.assertGreater(aperture[2], 0.0)

    def test_irq_during_path_b_restores_the_window_exactly(self):
        tracer = self.boot()[1]
        injections = [entry for entry in tracer.injections
                      if entry["aperture"]]
        self.assertTrue(injections, "no delivery was taken in a window")
        stack = tracer.syms.get("_ipc_ap_stack")
        top = tracer.syms.get("_ipc_ap_stack_top")
        self.assertIsNotNone(stack)
        for entry in injections:
            self.assertEqual(entry["latch"] & MODE_MASK, APPLICATION_MODE,
                             "the delivery was not taken in mode 10")
            self.assertEqual(entry["latch"] & PAGE_MASK, PAGE_ONE,
                             "the delivery was not taken with the aperture "
                             "open")
            self.assertTrue(stack <= entry["sp"] <= top,
                            "the interrupted SP %04Xh is outside the "
                            "common aperture reservation %04Xh-%04Xh"
                            % (entry["sp"], stack, top))
            # The gate normalizes for dispatch, then puts the interrupted
            # latch back before its RETI: the next aperture-value write
            # after the delivery is that restore, and it restores the
            # exact mode-10 latch the copy primitive is in the middle of.
            after = [write for write in tracer.latch_writes
                     if write["t"] > entry["t"] and
                     tracer.is_aperture_latch(write["value"])]
            self.assertTrue(after,
                            "the delivery was never followed by the "
                            "window's latch")
            self.assertEqual(after[0]["value"], entry["latch"],
                             "the gate restored %02Xh, not the interrupted "
                             "%02Xh" % (after[0]["value"], entry["latch"]))
        retis = [entry for entry in tracer.retis
                 if any(injection["t"] <= entry["t"] for injection in
                        injections)]
        self.assertTrue(retis, "no RETI ended the aperture delivery")
        # Every install the whole run made put the latch back exactly,
        # interrupted or not.
        installs = tracer.aperture_installs(0, tracer.tstates)
        self.assertTrue(installs, "the run made no aperture installs")
        for install in installs:
            self.assertIsNotNone(
                tracer.restore_of(install),
                "an install of %02Xh at %d was never restored"
                % (install["value"], install["t"]))
        print("ipc %s: %d aperture deliveries, latch %s, SP %s, %d installs"
              % (self.image_ident, len(injections),
                 ["%02X" % entry["latch"] for entry in injections],
                 ["%04X" % entry["sp"] for entry in injections],
                 len(installs)))

    def test_counter_census_covers_every_path(self):
        tracer = self.boot()[1]
        totals = {key: 0 for key in COPY_COUNTERS}
        first = tracer.mark_counters.get(("A", "a"))
        last = tracer.mark_counters.get(("V", "c"))
        self.assertIsNotNone(first)
        self.assertIsNotNone(last)
        for key in COPY_COUNTERS:
            totals[key] = last[key] - first[key]
        for key, count in sorted(totals.items()):
            self.assertGreater(count, 0, "%s never moved" % key)
        print("ipc %s: probe-path counters %s"
              % (self.image_ident, totals))


class TestIpcServiceImages(IpcSimBase):
    """The resident service images (work package 40).

    The loader's own report is pinned against the packer's plan: the same
    tier policy, the same bases, the two architecture-36.20 ledgers and the
    boot tier's one-shot evidence, read back out of bank 6.
    """

    def plan(self):
        if getattr(self, "_plan", None) is None:
            path = os.path.join(SERVICE_DIR, "services.plan.json")
            if not os.path.exists(path):
                self.skipTest("the service images have not been packed")
            with open(path, encoding="utf-8") as handle:
                self._plan = json.load(handle)
        return self._plan

    def report(self, name):
        with open(os.path.join(MODULE_DIR, name + ".report.json"),
                  encoding="utf-8") as handle:
            return json.load(handle)

    def symbol_address(self, name, symbol):
        """The bank-6 address of a service image symbol: the packer's
        segment-relative offset in the segment the report names."""
        entry = [item for item in self.plan()["services"]
                 if item["name"] == "ipc-%s.zmod" % name][0]
        report = self.report(name)
        role = report["symbol_segments"][symbol]
        base = {"hot": entry["hot_base"], "boot": entry["boot_base"],
                "io": entry["io_base"] or 0,
                "bulk": entry["bulk_base"] or 0}[role]
        return base + report["symbols"][symbol]

    def test_loader_reports_the_packer_plan(self):
        """Every image's placement line and both ledgers match the plan."""
        tx = self.transcript()
        plan = self.plan()
        for entry in plan["services"]:
            line = "service %s: hot %04Xh" % (entry["name"],
                                              entry["hot_base"])
            if entry["io_base"] is not None:
                line += " io %04Xh" % entry["io_base"]
            if entry["boot_base"] is not None:
                line += " boot %04Xh" % entry["boot_base"]
            if entry["bulk_base"] is not None:
                line += " bulk %04Xh" % entry["bulk_base"]
            self.assertIn(line.encode() + b"\r\n", tx, line)
        nucleus = plan["nucleus"]
        self.assertIn(("service hot: %d bytes of %d\r\n"
                       % (nucleus["service_hot_bytes"],
                          nucleus["service_hot_capacity_bytes"])).encode(),
                      tx)
        self.assertIn(("service boot/bulk: %d bytes of %d\r\n"
                       % (nucleus["service_boot_bytes"],
                          nucleus["service_boot_capacity_bytes"])).encode(),
                      tx)
        print("ipc %s: six service images placed from the manifest, "
              "service hot %d/%d, service boot/bulk %d/%d"
              % (self.image_ident, nucleus["service_hot_bytes"],
                 nucleus["service_hot_capacity_bytes"],
                 nucleus["service_boot_bytes"],
                 nucleus["service_boot_capacity_bytes"]))

    def test_every_boot_segment_ran_exactly_once(self):
        """Each boot segment recorded one run in its own bank-6 BSS."""
        z = self.boot()[0]
        for name in SERVICE_ORDER:
            runs = self.symbol_address(name, "_ipc_%s_boot_runs" % name)
            magic = self.symbol_address(name, "_ipc_%s_boot_magic" % name)
            self.assertEqual(z.banks[6][runs], 1,
                             "%s boot segment ran %d times"
                             % (name, z.banks[6][runs]))
            self.assertEqual(z.banks[6][magic], IPC_BOOT_MAGIC, name)
        print("ipc %s: six boot segments, each a single execution"
              % self.image_ident)

    def test_con_slow_buffer_is_outside_the_service_io_range(self):
        """`con`'s slow buffer sits in the hot tier, not in 8000h-BFFFh."""
        plan = self.plan()
        entry = [item for item in plan["services"]
                 if item["name"] == "ipc-con.zmod"][0]
        address = entry["hot_base"] + \
            self.report("con")["symbols"]["_ipc_con_slow_buffer"]
        self.assertFalse(0x8000 <= address < 0xC000,
                         "the slow buffer is at %04Xh" % address)
        self.assertTrue(0x6000 <= address < 0xE000,
                        "the slow buffer is at %04Xh" % address)
        print("ipc %s: con slow buffer at %04Xh (hot tier)"
              % (self.image_ident, address))


class TestIpcPlacement(IpcSimBase):
    """Fixed-core placement, the service stacks and the SDK fast path."""

    def test_stack_high_water_marks(self):
        tracer = self.boot()[1]
        regions = dict((name, (base, top))
                       for name, base, top in self.reserved_stacks())
        for name in STACK_NAMES:
            base, top = regions[name]
            low = tracer.sp_min.get(name)
            self.assertIsNotNone(low, "the %s stack was never used" % name)
            self.assertGreater(low, base,
                               "%s stack underflowed to %04X" % (name, low))
            print("ipc %s: %s stack high-water %04Xh-%04Xh (%d of %d bytes "
                  "used)" % (self.image_ident, name, low, top,
                             top - low + 1, top - base + 1))
        # The substrate really switched stacks: the service frames live on
        # the reserved service stack, above the ISR stack's base.
        service_base, service_top = regions["service"]
        low = tracer.sp_min["service"]
        self.assertLess(low, service_top)
        self.assertGreater(low, service_base)

    def test_lookup_census_and_cache(self):
        """The kernel sees fewer lookups than the client issues: the SDK
        cache answers its repeat, and every SDK lookup marshals through the
        one argument block inside the caller window."""
        tracer = self.boot()[1]
        # The caller-buffer probe (correction 01) looks up five classes of
        # its own before it starts the client, so the census is taken from
        # the client's banner line on: it is the client's cache that is
        # under test, and the banner precedes its first lookup.
        start = tracer.client_start
        self.assertIsNotNone(start, "the client never started")
        lookups = [entry for entry in tracer.syscalls
                   if entry["number"] == LOOKUP_SYSCALL and
                   entry["t"] >= start]
        window = [entry for entry in lookups
                  if 0x0100 <= entry["block"] < 0x2000]
        outside = [entry for entry in lookups
                   if not 0x0100 <= entry["block"] < 0x2000]
        # The client issues nine SDK lookups: six handle discoveries, a
        # repeated discovery, the deliberate miss and the version query.
        # The cache answers the repeat, so the kernel sees eight of them;
        # the ninth lookup syscall is the client's deliberate out-of-window
        # refusal check, which never reaches the substrate.
        self.assertEqual(len(window), 8,
                         "the SDK cache let %d lookups through"
                         % len(window))
        self.assertEqual(len(outside), 1)
        self.assertEqual(sorted(entry["bytes"][0] for entry in window),
                         [9, 16, 16, 17, 18, 19, 20, 21])
        blocks = set(entry["block"] for entry in window)
        self.assertEqual(len(blocks), 1,
                         "the SDK marshals through %d blocks" % len(blocks))
        clients = set(entry["ret"] for entry in lookups
                      if entry["t"] >= start)
        self.assertEqual(len(clients), 1, clients)
        print("ipc %s: %d SYS_IPC_LOOKUP calls for nine SDK lookups "
              "(one cache hit), all through block %04X, plus one refused "
              "out-of-window block %04X"
              % (self.image_ident, len(window), next(iter(blocks)),
                 outside[0]["block"]))

    def test_fixed_core_symbols_stay_below_the_split(self):
        for name in ("ipc_asm_begin", "ipc_asm_end", "_ipc_lookup",
                     "_ipc_syscall_call", "_ipc_copy_run", "_ipc_invoke",
                     "_ipc_invoke_here", "_gw_invoke"):
            address = self.syms.get(name)
            if address is None:
                continue
            self.assertGreaterEqual(address, 0x2000, name)
            self.assertLess(address, 0x6000, name)
        self.assertEqual(self.syms["zos_service_stack"], 0x5D00)
        self.assertEqual(self.syms["zos_service_stack_top"], 0x5F00)
        self.assertEqual(self.syms["irq_isr_stack"], 0x5F00)
        self.assertEqual(self.syms["irq_isr_stack_top"], 0x6000)
        for name in ("zos_syscall_entry", "zos_gate_stack",
                     "zos_gate_stack_top"):
            self.assertGreaterEqual(self.syms[name], 0xE000, name)
        # The cold half starts at the split and the fixed core ends there.
        geometry = self.geometry()
        self.assertEqual(geometry["REV13_WINDOW_BASE"], 0x6000)
        self.assertLessEqual(geometry["FIXED_CODE_END"], 0x6000)
        self.assertEqual(self.syms["s__WINDOW"], 0x6000)
        self.assertEqual(geometry["KERNEL_CODE_END"],
                         self.syms["__kernel_code_end"])
        self.assertLessEqual(geometry["DATA_BASE"], 0x6000)
        print("ipc %s: substrate %04Xh-%04Xh, fixed code end %04Xh, cold "
              "end %04Xh, service stack %04Xh-%04Xh"
              % (self.image_ident, self.syms["ipc_asm_begin"],
                 self.syms["ipc_asm_end"], geometry["FIXED_CODE_END"],
                 self.syms["__kernel_code_end"],
                 self.syms["zos_service_stack"],
                 self.syms["zos_service_stack_top"]))


if __name__ == "__main__":
    unittest.main()
