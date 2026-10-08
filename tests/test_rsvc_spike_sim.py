#!/usr/bin/env python3
"""Resident-service spike replay (work package 35, requirement 4).

Boots build-rsvc-spike-6000/zephyros-rsvc-spike.bin -- transport A on
decoder revision 13, the service bank in the 6000h-DFFFh window -- and
measures the spike from the outside:

* the client program's console transcript: every phase marker, the
  in-program conformance checks, the stack high-water marks, the evidence
  counters and the final PASS/FAIL;
* the memory-control latch trace: which page bits each service call opened,
  where the domain entry wedged itself between the client phase markers,
  and the window region the direct copies run through;
* the copy trace: every LDIR executed while the domain was open, so the
  payload transfers really went through the nucleus window and not through
  a cross-domain pointer;
* the CTC0 stand-in: the client's IRQ phase programs the real CTC0 test
  helper, so this replay arms on that programming sequence and asserts the
  IM2 vector while a service call is in progress (interrupts are only
  enabled inside the service spin), then measures delivery, the
  service-domain span and the handler cost as the difference between the
  baseline and the service passes.

T-state accounting uses the opcode table below; an interval that executes
an opcode the table does not cover fails the coverage test, so the printed
numbers are measured, not extrapolated.

The tests skip when the image has not been built.
"""

import os
import re
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import Z80, read_symbols  # noqa: E402

# Zilog Z80 T-states (Zilog Z80 CPU User Manual timing tables) for the
# opcodes the measured intervals execute.  Cost of an instruction is looked
# up before it executes; an opcode that is not in the table is reported as
# missing and fails test_tstate_table_covers_measured_intervals.
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

RET = 0x1234
STACK = 0xFFFE
STEP_LIMIT = 60000000

MARKER = b"rsvc spike: primary OK\r\n"
PASS = b"rsvc: PASS\r\n"
FAIL = b"rsvc: FAIL\r\n"
PANIC = b"PANIC"

MEMCTL_PORT = 0x00
CTC0_PORT = 0x40
CTC0_CONTROL = 0xA7
CTC0_TIME_CONSTANT = 0x00
VOID = 0xFF
MODE_MASK = 0x18
PAGE_MASK = 0xE0
OS_MODE = 0x18
CTC0_VECTOR = 0x00
# The resident domain is the mode-11 window: a program counter inside
# 6000h-DFFFh with the page bits set is resident service code.
DOMAIN_BASE = 0x6000
DOMAIN_END = 0xE000

N1 = 8
N2 = 24

# Every phase the client measures, in emission order, and the ones that
# measure today's direct kernel path instead of a resident service call
# (those take no service domain at all).
PHASES = ("null16", "null8", "null32", "con1", "con16", "con64", "con256",
          "console1", "console256", "read", "staged", "fastpath", "fullpath",
          "fsread")
DIRECT_PHASES = ("console1", "console256", "fsread")

MARK_RE = re.compile(rb"rsvc: ([A-Za-z0-9_]+)([abc])\r\n")
CHECKS_RE = re.compile(rb"rsvc: (\d+)/(\d+) checks\r\n")
STACKS_RE = re.compile(rb"rsvc: stacks service (\d+) gate (\d+) isr (\d+)\r\n")
CALLS_RE = re.compile(rb"rsvc: calls (\d+) nested (\d+)\r\n")
IRQ_RE = re.compile(rb"rsvc: irq (\d+)/(\d+) chain (\d+) con (\d+)\r\n")
FAST_RE = re.compile(rb"rsvc: fast (\d+) refused (\d+)/(\d+) forged (\d+) "
                     rb"capfail (\d+)\r\n")

SERVICE_STACK = 384
GATE_STACK = 256
ISR_STACK = 256

_BOOT = {}


class RsvcTracer:
    """Per-instruction hook: latch trace, LDIR trace, T-states, CTC0."""

    def __init__(self, syms, page_bits, slot_regions,
                 domain_base=DOMAIN_BASE, domain_end=DOMAIN_END):
        self.syms = syms
        self.page_bits = tuple(page_bits)
        self.slot_regions = tuple(slot_regions)
        self.domain_base = domain_base
        self.domain_end = domain_end
        self.marks = {}
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
        self.injections = []
        self.armed = False
        self.retis = []

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
        """The kernel's CTC0 helper: vector base, control word, constant."""
        if value == CTC0_CONTROL:
            self.ctc_armed = True
        elif value == CTC0_TIME_CONSTANT and self.ctc_armed:
            # The time constant starts the channel: the kernel's helper
            # writes it last, so a fresh arming is now live.
            self.armed = True

    def _latch_write(self, pc, value):
        self.latch_writes.append({"t": self.tstates, "pc": pc, "value": value})
        if (value & MODE_MASK) == OS_MODE and (value & PAGE_MASK):
            if self.open is None:
                self.open = {"open_t": self.tstates, "open_value": value,
                             "open_pc": pc, "tstates": 0, "steps": 0,
                             "ldir_bytes": 0, "inject": None, "close_t": None}
            return
        if self.open is not None and (value & MODE_MASK):
            # Only a real mode change closes the domain: the mode-00 ROM
            # window the recovery store uses also clears the page bits.
            self.open["close_t"] = self.tstates
            self.open["close_value"] = value
            self.open["close_pc"] = pc
            self.intervals.append(self.open)
            self.open = None

    def watch(self, z):
        if len(z.sio_tx) != self._tx_len:
            self._tx_len = len(z.sio_tx)
            self.tx = bytes(z.sio_tx)
            tail = self.tx[self._scanned:]
            for match in MARK_RE.finditer(tail):
                name = match.group(1).decode()
                suffix = match.group(2).decode()
                if (name, suffix) not in self.marks:
                    self.marks[(name, suffix)] = self.tstates
                    if suffix == "a":
                        self.phase_order.append(name)
            # Rescan only the trailing partial line: a marker is printed
            # byte by byte, so its start can be arbitrarily far back once
            # the next burst (a 256-byte console pattern) has arrived.
            end = self.tx.rfind(b"\r\n", self._scanned)
            if end >= 0:
                self._scanned = end + 2

        if (self.armed and z.iff1 and not z.int_pending and
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
                                    "latch": z.latch, "sp": z.sp})
            if self.open is not None:
                self.open["inject"] = self.injections[-1]

        cost = opcode_cost(z)
        if cost is None:
            self.missing_table.add((z.rd(z.pc),
                                    z.rd((z.pc + 1) & 0xFFFF)))
            cost = 0
        if self.open is not None:
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
                               "src": hl, "dst": de,
                               "in_domain": self.open is not None})
            if self.open is not None:
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

    def domain_entry_after(self, t0):
        """Tick of the first service-domain entry after an injection.

        The ISR executive may write the latch itself before the trampoline
        runs, so the raw write sequence is the only honest source: the
        first mode-11 write with page bits after the injection is the
        trampoline's `rsvc_domain_enter`."""
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


class RsvcSpikeSim(unittest.TestCase):
    SUBDIR = "build-rsvc-spike-6000"
    IMAGE = "zephyros-rsvc-spike.bin"
    PAGE_BITS = (0x20,)
    SLOT_REGIONS = ((0x6000, 0x8000),)
    DOMAIN_BASE = DOMAIN_BASE
    DOMAIN_END = DOMAIN_END

    def setUp(self):
        self.bin_path = os.path.join(ROOT, self.SUBDIR, self.IMAGE)
        if not os.path.exists(self.bin_path):
            self.skipTest("%s missing; run make rsvc-spike-selftest"
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

    def crt0_untraced(self, z):
        z.push16(RET)
        z.pc = self.syms["__zos_crt0_init"]
        for _ in range(2000000):
            z.step()
            if z.pc == RET:
                return
        raise AssertionError("C runtime init did not return")

    def boot(self):
        if self.SUBDIR in _BOOT:
            self.z, self.tracer = _BOOT[self.SUBDIR]
            return self.z, self.tracer
        z = self.make_cpu()
        tracer = RsvcTracer(self.syms, self.PAGE_BITS, self.SLOT_REGIONS,
                            self.DOMAIN_BASE, self.DOMAIN_END)
        tracer.install(z)
        self.crt0_untraced(z)
        z.push16(RET)
        z.pc = self.syms["_kernel_main"]
        delay = self.syms["_kernel_serial_connect_delay"]
        tx_len = len(z.sio_tx)
        for _ in range(STEP_LIMIT):
            if z.pc == delay:
                z.pc = z.pop16()
            z.step()
            if len(z.sio_tx) != tx_len:
                tx_len = len(z.sio_tx)
                tail = bytes(z.sio_tx)[-96:]
                if PANIC in tail:
                    raise AssertionError("panic during boot: %r"
                                         % bytes(z.sio_tx)[-300:])
                if MARKER in tail:
                    break
        else:
            raise AssertionError("the spike program never started "
                                 "(PC=%04X, tail=%r)"
                                 % (z.pc, bytes(z.sio_tx)[-300:]))
        for _ in range(STEP_LIMIT):
            tracer.watch(z)
            z.step()
            if (PASS in tracer.tx or FAIL in tracer.tx or
                    PANIC in tracer.tx):
                break
        else:
            raise AssertionError("the spike program never finished "
                                 "(PC=%04X, tail=%r)"
                                 % (z.pc, tracer.tx[-300:]))
        self.z = z
        self.tracer = tracer
        _BOOT[self.SUBDIR] = (z, tracer)
        return z, tracer

    def transcript(self):
        return self.boot()[1].tx

    # --- shared checks ----------------------------------------------------
    def test_program_passes(self):
        tx = self.transcript()
        self.assertIn(MARKER, tx)
        self.assertNotIn(FAIL, tx, tx[-600:].decode("latin-1"))
        self.assertIn(PASS, tx, tx[-600:].decode("latin-1"))
        match = CHECKS_RE.search(tx)
        self.assertIsNotNone(match, tx[-600:].decode("latin-1"))
        passed, total = int(match.group(1)), int(match.group(2))
        self.assertEqual(passed, total)
        self.assertGreaterEqual(total, 40)

    def test_tstate_table_covers_the_measured_intervals(self):
        _, tracer = self.boot()
        self.assertEqual(sorted("%02X %02X" % pair
                                for pair in tracer.missing_table), [])

    def test_every_phase_is_measured(self):
        _, tracer = self.boot()
        for name in PHASES:
            for suffix in "abc":
                self.assertIn((name, suffix), tracer.marks,
                              "%s%sa/b/c markers" % (name, suffix))
        self.assertEqual(tracer.phase_order, list(PHASES))

    def test_phase_costs(self):
        _, tracer = self.boot()
        report = []
        for name in PHASES:
            cost = tracer.per_call(name)
            self.assertIsNotNone(cost)
            self.assertGreater(cost, 0.0, name)
            first, second = tracer.segments(name)
            report.append("  %-12s %8.1f T  (segments %8.1f / %8.1f)"
                          % (name, cost, first, second))
        print("rsvc %s: per-call T-states" % self.image_ident)
        print("\n".join(report))
        per_byte = [(name, tracer.per_call(name) / size)
                    for name, size in (("con1", 1), ("con16", 16),
                                       ("con64", 64), ("con256", 256),
                                       ("console1", 1), ("console256", 256))]
        for name, value in per_byte:
            self.assertGreater(value, 0.0, name)
        # Batching must amortise the crossing: cycles per byte fall with
        # the payload, for the resident service and today's syscall alike.
        self.assertLess(tracer.per_call("con256") / 256.0,
                        tracer.per_call("con1"))
        self.assertLess(tracer.per_call("con16") / 16.0,
                        tracer.per_call("con1"))
        self.assertLess(tracer.per_call("con64") / 64.0,
                        tracer.per_call("con16") / 16.0)
        self.assertLess(tracer.per_call("console256") / 256.0,
                        tracer.per_call("console1"))
        # The staged variant copies the block an extra time.
        self.assertGreaterEqual(tracer.per_call("staged"),
                                tracer.per_call("read"))
        self.assertGreaterEqual(tracer.per_call("staged"),
                                tracer.per_call("read"))

    def test_latch_intervals_open_only_the_spike_domain(self):
        _, tracer = self.boot()
        pages = set()
        for interval in tracer.intervals:
            pages.add(interval["open_value"] & PAGE_MASK)
        self.assertEqual(pages, set(self.PAGE_BITS))
        phase_pages = {}
        for name in PHASES:
            entries = tracer.interval_for_phase(name)
            # The direct phases measure today's kernel path: no service
            # call runs, so no domain interval exists at all.
            if name not in DIRECT_PHASES:
                self.assertTrue(entries, name)
            phase_pages[name] = set(entry["open_value"] & PAGE_MASK
                                    for entry in entries)
        for name, seen in sorted(phase_pages.items()):
            self.assertTrue(seen.issubset(set(self.PAGE_BITS)),
                            "%s opened %r" % (name, seen))
        for name in DIRECT_PHASES:
            self.assertEqual(phase_pages[name], set(), name)
        # Every phase with a bulk payload must copy through the domain.
        for name in ("read", "staged", "fastpath", "fullpath"):
            self.assertTrue(any(entry["ldir_bytes"] > 0
                                for entry in tracer.interval_for_phase(name)),
                            name)

    def test_copies_run_through_the_domain(self):
        _, tracer = self.boot()
        copies = tracer.ldirs_in_domain()
        self.assertTrue(copies)
        total = sum(entry["count"] for entry in copies)
        self.assertGreaterEqual(total, 512 * 8)
        for base, size in self.SLOT_REGIONS:
            touched = [entry for entry in copies
                       if tracer.touches_slot(entry, base, size)]
            self.assertTrue(touched,
                            "no domain copy in %04X-%04X"
                            % (base, base + size))
        biggest = max(entry["count"] for entry in copies)
        self.assertGreaterEqual(biggest, 512)
        print("rsvc %s: %d LDIR copies (%d bytes) inside the domain"
              % (self.image_ident, len(copies), total))

    def test_stack_high_water_marks(self):
        tx = self.transcript()
        match = STACKS_RE.search(tx)
        self.assertIsNotNone(match, tx[-400:].decode("latin-1"))
        service, gate, isr = (int(group) for group in match.groups())
        for value in (service, gate, isr):
            self.assertGreater(value, 0)
        self.assertLess(service, SERVICE_STACK)
        self.assertLess(gate, GATE_STACK)
        self.assertLess(isr, ISR_STACK)
        calls = CALLS_RE.search(tx)
        self.assertIsNotNone(calls)
        self.assertEqual(int(calls.group(2)), 4)
        print("rsvc %s: stacks service %d/%d gate %d/%d isr %d/%d, "
              "nesting %d, calls %s"
              % (self.image_ident, service, SERVICE_STACK, gate, GATE_STACK,
                 isr, ISR_STACK, int(calls.group(2)), calls.group(1).decode()))

    def test_irq_is_delivered_into_the_service_domain(self):
        _, tracer = self.boot()
        self.assertEqual(len(tracer.injections), 2,
                         "one IRQ per CTC0 pass, got %r" % tracer.injections)
        tx = self.transcript()
        match = IRQ_RE.search(tx)
        self.assertIsNotNone(match)
        self.assertEqual(int(match.group(1)), 1)
        self.assertEqual(int(match.group(2)), 2)
        spans = []
        for injection in tracer.injections:
            entry = tracer.domain_entry_after(injection["t"])
            self.assertIsNotNone(entry, "no domain entry after %r" % injection)
            reti = next(reti for reti in tracer.retis
                        if reti["t"] > injection["t"])
            spans.append((entry - injection["t"], reti["t"] - entry))
        baseline, service = spans
        # The service pass runs the handler the baseline pass skips, so its
        # in-domain interrupt window is the longer one by exactly that much.
        self.assertGreater(service[1], baseline[1])
        # The nucleus keeps the final RETI: one per delivery, executed by
        # kernel code (fixed region or common), never by the resident bank
        # the handler lives in.
        self.assertEqual(len(tracer.retis), 2, tracer.retis)
        for reti in tracer.retis:
            self.assertFalse(self.DOMAIN_BASE <= reti["pc"] <
                             self.DOMAIN_END,
                             "RETI at %04X is resident code" % reti["pc"])
        print("rsvc %s: IRQ delivery %d T, ISR span baseline %d / service "
              "%d T, handler cost %d T"
              % (self.image_ident, service[0], baseline[1], service[1],
                 service[1] - baseline[1]))


    def test_domain_never_changes_page(self):
        _, tracer = self.boot()
        self.assertEqual(set(entry["open_value"] & PAGE_MASK
                             for entry in tracer.intervals), {0x20})
        self.assertGreaterEqual(len(tracer.intervals), len(PHASES) * N1)

    def test_fast_path_costs_less_than_the_forced_full_path(self):
        """One domain, so the same-domain fast path is actually reachable
        and the client-visible difference is its cost."""
        _, tracer = self.boot()
        self.assertGreater(tracer.per_call("fullpath"),
                           tracer.per_call("fastpath"))
        print("rsvc %s: fast path saves %.1f T per 512-byte chain"
              % (self.image_ident,
                 tracer.per_call("fullpath") - tracer.per_call("fastpath")))

    def test_fixed_core_symbols_stay_below_the_split(self):
        """The fixed core is 2000h-5FFFh: the IRQ executive, the IPC
        substrate and the ISR stack must be below it, and the relocated
        ISR stack must sit at the top of the fixed region (5F00h-6000h).
        The syscall and IRQ gates stay common-resident (E000h and up)."""
        for name in ("_irq_dispatch", "_rsvc_domain_enter", "_rsvc_invoke",
                     "_zos_rsvc_dispatch"):
            address = self.syms[name]
            self.assertLess(address, 0x6000, name)
            self.assertGreaterEqual(address, 0x2000, name)
        # The ISR stack sits at the top of the fixed region, so its top is
        # the split address itself.
        self.assertEqual(self.syms["irq_isr_stack"], 0x5F00)
        self.assertEqual(self.syms["irq_isr_stack_top"], 0x6000)
        for name in ("zos_syscall_entry", "zos_gate_stack_top"):
            self.assertGreaterEqual(self.syms[name], 0xE000, name)
        # And the linked fixed core really ends below the split.
        kernel_end = self.syms["__kernel_code_end"]
        self.assertLessEqual(kernel_end, 0x6000)
        print("rsvc %s: linked fixed code end %04Xh, ISR stack %04Xh-"
              "%04Xh" % (self.image_ident, kernel_end,
                         self.syms["irq_isr_stack"],
                         self.syms["irq_isr_stack_top"]))


if __name__ == "__main__":
    unittest.main()
