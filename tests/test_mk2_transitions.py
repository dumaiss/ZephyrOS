#!/usr/bin/env python3
"""The kernel's transitions on the Zephyr-80 Mk2 board.

The image (tests/target/mk2trans/mk2trans.asm, `make mk2trans`) is the
syscall gate, page zero, the IRQ executive, the event core and the Mk2
mapping state assembled for Mk2 (architecture 2.7.3-2.7.6), around a
bootstrap, stub dispatchers and a process running in the working bank.
The tests drive the process from the simulator and check that every
crossing returns to the exact context: registers, SP, the DAT entries and
control register, and their software mirrors.

The NMI-injection tests replay each switch sequence (the gate in and out,
an IRQ in and out, a remap, a working-bank load, an event delivery) with
an NMI taken after every instruction: the RT tick finds its stub and a
RAM stack, changes no mapping, and the sequence ends in the state it ends
in without the NMI.  Every boundary of every sequence also satisfies the
NMI rule's ordering: a mirror differs from the hardware only by the value
the next mapper write installs.

The Mk1 gate harness (mk1gate.ihx: the same gate sources assembled for
Mk1) runs the same null call for the gate measurement.

Skipped when the image has not been built.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import z80sim  # noqa: E402
from z80sim import Ctc, StopSet, Z80  # noqa: E402

BUILD = os.path.join(ROOT, "build-mk2trans")
IMAGE = os.path.join(BUILD, "mk2trans.bin")
MAP = os.path.join(BUILD, "mk2trans.map")
MK1_IHX = os.path.join(BUILD, "mk1gate.ihx")
MK1_MAP = os.path.join(BUILD, "mk1gate.map")
LIMIT = 4000000

PAGE = 0x2000
COMMON = 0x7F * PAGE                  # physical base of the common page
PROCESS_PAGE0 = 0x50
SYSTEM_PAGE0 = 0x40
PROCESS_IMAGE = [0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x7F]
SYSTEM_IMAGE = [0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x7F]
DAT_STATE = 0xEF10                    # memory.inc KOS_DAT_STATE_BASE
DAT_CACHED = 0xEF21
CTL_SYSTEM, CTL_WORKING = 0x02, 0x03
ARGS = 0x0100                         # the argument block, process page 0
P_TALLY = 0x0180
CTC3_VECTOR = 0x06
NMI_STUB = bytes([0xC3, 0x08, 0xEF])  # JP KOS_RT_VECTOR

SYS_SUM, SYS_FAIL, SYS_LOAD, SYS_EVENT_RETURN = 1, 2, 3, 4
SYS_SPIN, SYS_REMAP, SYS_POST, SYS_NOP = 5, 6, 7, 8

REGS = ("b", "c", "d", "e", "ix", "iy", "alt_a", "alt_f", "alt_b",
        "alt_c", "alt_d", "alt_e", "alt_h", "alt_l")
ALL_REGS = REGS + ("a", "f", "h", "l", "pc", "sp", "iff1", "iff2")


def common(addr):
    return COMMON | (addr & 0x1FFF)


def hardware(z):
    """The mapper: 16 entries and the control byte."""
    m = z.mapper
    return tuple(m.entries) + ((m.map_enable << 1) | m.dat_bank,)


def mirror(z):
    """The software mirrors: 16 entries and the control byte."""
    base = common(DAT_STATE)
    return tuple(z.phys[base:base + 17])


def cached(z):
    base = common(DAT_CACHED)
    return z.phys[base] | (z.phys[base + 1] << 8)


def snapshot(z):
    regs = tuple(getattr(z, name) for name in ALL_REGS)
    return regs, hardware(z), mirror(z), cached(z)


@unittest.skipUnless(os.path.isfile(IMAGE), "make mk2trans builds the image")
class Mk2Case(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(IMAGE, "rb") as handle:
            cls.rom = handle.read()
        cls.syms = z80sim.read_symbols(MAP)

    def sym(self, name):
        return self.syms[name]

    def boot(self, ctc3=0):
        """A board booted into the process's idle loop.  `ctc3` = the CTC3
        time constant the boot programs (0: the CTC stays off)."""
        rom = bytearray(self.rom)
        rom[self.sym("t_ctc3_constant")] = ctc3
        z = Z80(bytes(rom), b"", board="mk2")
        if ctc3:
            Ctc().attach(z)
        self.run_to(z, "p_idle")
        self.assertEqual(hardware(z), tuple(SYSTEM_IMAGE + PROCESS_IMAGE) +
                         (CTL_WORKING,))
        self.assertEqual(mirror(z), hardware(z))
        self.assertEqual(cached(z), self.sym("t_process_image"))
        return z

    def run_to(self, z, name, limit=LIMIT):
        reason, _ = z.run(limit, stops=StopSet([self.sym(name)]),
                          check_first=False)
        self.assertEqual(reason, "stop", "%s never reached" % name)

    def cell(self, z, name, size=1):
        base = common(self.sym(name))
        if size == 1:
            return z.phys[base]
        return z.phys[base] | (z.phys[base + 1] << 8)

    def set_cell(self, z, name, value):
        z.phys[common(self.sym(name))] = value

    def page0(self, addr):
        return PROCESS_PAGE0 * PAGE | addr

    def set_regs(self, z, seed):
        """Distinct values in every register the gate preserves."""
        for i, name in enumerate(REGS):
            width = 0xFFFF if name in ("ix", "iy") else 0xFF
            setattr(z, name, (seed * 37 + i * 91 + 0x5A) & width)

    def call(self, z, number, hl=ARGS, iff=True, seed=1):
        """One syscall from p_call; returns the register snapshot before."""
        self.set_regs(z, seed)
        z.a = number
        z.h, z.l = hl >> 8, hl & 0xFF
        z.pc = self.sym("p_call")
        z.iff1 = z.iff2 = iff
        before = {name: getattr(z, name) for name in REGS + ("sp",)}
        self.run_to(z, "p_return")
        return before

    def assert_preserved(self, z, before):
        for name, value in before.items():
            self.assertEqual(getattr(z, name), value, name)

    def take_irq(self, z):
        """Assert CTC3's vector and step until the CPU accepts it."""
        z.assert_int(CTC3_VECTOR)
        for _ in range(4):
            z.step()
            if not z.int_pending:
                return
        self.fail("the interrupt was not accepted")

    def arm_event(self, z):
        """Row 0 of the event table: process 1, its page 0, the process's
        trampoline and handler."""
        row = common(self.sym("_kos_event_table"))
        t, h = self.sym("p_trampoline"), self.sym("p_handler")
        z.phys[row:row + 12] = bytes([1, PROCESS_PAGE0, 0, 0, 0, 0,
                                      t & 0xFF, t >> 8, h & 0xFF, h >> 8,
                                      0, 0])
        z.phys[self.page0(P_TALLY):self.page0(P_TALLY) + 2] = bytes(2)


class TestGate(Mk2Case):
    def test_round_trips_return_the_exact_context(self):
        z = self.boot()
        state = (hardware(z), mirror(z), cached(z))
        z.phys[self.page0(ARGS):self.page0(ARGS) + 4] = \
            bytes([0x34, 0x12, 0x11, 0x11])
        before = self.call(z, SYS_SUM)
        self.assert_preserved(z, before)
        self.assertEqual((z.h << 8 | z.l, z.f & 1), (0x2345, 0))
        self.assertEqual(z.phys[self.page0(ARGS + 4):self.page0(ARGS + 6)],
                         bytes([0x45, 0x23]))
        self.assertTrue(z.iff1)
        self.assertEqual((hardware(z), mirror(z), cached(z)), state)

        before = self.call(z, SYS_FAIL, seed=2)
        self.assert_preserved(z, before)
        self.assertEqual((z.f & 1, z.a, z.h << 8 | z.l), (1, 5, 0x1234))
        self.assertEqual((hardware(z), mirror(z), cached(z)), state)

    def test_the_body_runs_in_the_system_bank_over_the_callers_page0(self):
        z = self.boot()
        self.call(z, SYS_NOP)
        self.assertEqual(self.cell(z, "t_body_control"), CTL_SYSTEM)
        self.assertEqual(self.cell(z, "t_body_page0"), PROCESS_PAGE0)
        # System entry 0 is the system page-0 image again, and the
        # crossing is closed.
        self.assertEqual(z.mapper.entries[0], SYSTEM_PAGE0)
        self.assertEqual(mirror(z)[0], SYSTEM_PAGE0)
        self.assertEqual(self.cell(z, "kos_gate_open"), 0)

    def test_a_masked_caller_returns_masked(self):
        z = self.boot()
        before = self.call(z, SYS_NOP, iff=False)
        self.assert_preserved(z, before)
        self.assertFalse(z.iff1)

    def test_a_body_that_loads_another_image_returns_the_callers(self):
        z = self.boot()
        state = (hardware(z), mirror(z), cached(z))
        before = self.call(z, SYS_LOAD)
        self.assert_preserved(z, before)
        self.assertEqual((hardware(z), mirror(z), cached(z)), state)

    def test_a_remapped_working_entry_survives_the_call(self):
        z = self.boot()
        z.a = 0x5A
        z.pc = self.sym("p_far")
        self.run_to(z, "p_far_done")
        far = list(PROCESS_IMAGE)
        far[5] = 0x5A
        self.assertEqual(list(hardware(z)[8:16]), far)
        self.assertEqual(cached(z) >> 8, 0)      # no image matches it
        for number in (SYS_LOAD, SYS_NOP, SYS_LOAD):
            before = self.call(z, number)
            self.assert_preserved(z, before)
            self.assertEqual(list(hardware(z)[8:16]), far)
            self.assertEqual(mirror(z), hardware(z))
            self.assertEqual(cached(z), self.sym("kos_gate_image"))

    def test_a_call_from_the_system_bank_faults(self):
        z = self.boot()
        z.mapper.dat_bank = 0
        z.phys[common(DAT_STATE + 16)] = CTL_SYSTEM
        z.sp = 0x0000
        z.pc = self.sym("t_system_call")
        reason, _ = z.run(LIMIT, halt=True)
        self.assertEqual(reason, "halt")
        text = common(self.cell(z, "t_panic", 2))
        self.assertEqual(bytes(z.phys[text:text + 24]),
                         b"mapping assertion failed")


class TestIrq(Mk2Case):
    def test_an_irq_in_the_program_returns_to_it(self):
        z = self.boot()
        self.set_regs(z, 3)
        z.a, z.f, z.h, z.l = 0x11, 0x22, 0x33, 0x44
        before = snapshot(z)
        self.take_irq(z)
        self.run_to(z, "p_idle")
        self.assertEqual(snapshot(z), before)
        self.assertEqual(self.cell(z, "t_irq_count"), 0)
        self.assertEqual(z.phys[common(self.sym("t_irq_count")) + 3], 1)
        self.assertEqual(self.cell(z, "t_irq_control"), CTL_SYSTEM)
        self.assertEqual(self.cell(z, "t_irq_value"), PROCESS_PAGE0)

    def test_an_irq_in_a_body_returns_to_the_body(self):
        z = self.boot()
        z.phys[self.page0(ARGS):self.page0(ARGS) + 2] = bytes([40, 0])
        self.set_regs(z, 4)
        z.a, z.h, z.l = SYS_SPIN, ARGS >> 8, ARGS & 0xFF
        z.pc = self.sym("p_call")
        z.iff1 = z.iff2 = True
        before = {name: getattr(z, name) for name in REGS + ("sp",)}
        self.run_to(z, "sys_spin_loop")
        body = snapshot(z)
        self.take_irq(z)                           # accepted in the body
        self.assertNotEqual(z.pc, self.sym("sys_spin_loop"))
        self.run_to(z, "sys_spin_loop")
        self.assertEqual(snapshot(z)[1:], body[1:])
        self.assertEqual(self.cell(z, "t_irq_value"), 0)   # system bank
        self.run_to(z, "p_return")
        self.assert_preserved(z, before)
        self.assertEqual(z.mapper.entries[0], SYSTEM_PAGE0)

    def test_an_irq_in_a_remapped_body_keeps_the_remap(self):
        # The nested system context: the caller has a far window open,
        # the body has remapped system entry 4; the IRQ returns into both.
        z = self.boot()
        z.a = 0x5A
        z.pc = self.sym("p_far")
        self.run_to(z, "p_far_done")
        z.phys[0x48 * PAGE] = 0xC4
        z.phys[self.page0(ARGS)] = 0x48
        self.set_regs(z, 5)
        z.a, z.h, z.l = SYS_REMAP, ARGS >> 8, ARGS & 0xFF
        z.pc = self.sym("p_call")
        z.iff1 = z.iff2 = True
        before = {name: getattr(z, name) for name in REGS + ("sp",)}
        working = hardware(z)[8:16]
        self.run_to(z, "sys_remap_mapped")
        inside = snapshot(z)
        self.assertEqual(z.mapper.entries[4], 0x48)
        self.take_irq(z)
        self.run_to(z, "sys_remap_mapped")
        self.assertEqual(snapshot(z)[1:], inside[1:])
        self.run_to(z, "p_return")
        self.assert_preserved(z, before)
        self.assertEqual((z.h << 8 | z.l), 0xC4)
        self.assertEqual(hardware(z)[8:16], working)
        self.assertEqual(z.mapper.entries[4], 0x44)
        self.assertEqual(mirror(z), hardware(z))


class TestEvents(Mk2Case):
    def test_an_event_at_irq_exit_returns_to_the_exact_context(self):
        z = self.boot()
        self.arm_event(z)
        self.set_cell(z, "t_post_on_irq", 1)
        self.set_regs(z, 6)
        z.a, z.f, z.h, z.l = 0x66, 0x41, 0x12, 0x34
        before = snapshot(z)
        self.take_irq(z)
        self.run_to(z, "p_trampoline")
        # The frame carries the far-window handle in force: 0, the
        # program's own pages.
        frame = z.sp
        self.assertEqual(z.rd(frame), 0)
        self.assertEqual(z.rd(frame + 1), 0)
        self.assertEqual(hardware(z)[16], CTL_WORKING)
        self.run_to(z, "p_idle")
        self.assertEqual(snapshot(z), before)
        self.assertEqual(z.rd(P_TALLY), 1)
        self.assertEqual(self.cell(z, "kos_gate_open"), 0)
        self.assertEqual(z.phys[common(self.sym("_kos_event_in_handler"))
                                + 1], 0)

    def test_an_event_at_syscall_exit_carries_the_result(self):
        z = self.boot()
        self.arm_event(z)
        before = self.call(z, SYS_POST)
        self.assert_preserved(z, before)
        self.assertEqual((z.h << 8 | z.l, z.f & 1), (0x0777, 0))
        self.assertEqual(z.rd(P_TALLY), 1)
        self.assertEqual(hardware(z), tuple(SYSTEM_IMAGE + PROCESS_IMAGE) +
                         (CTL_WORKING,))
        self.assertEqual(mirror(z), hardware(z))


class TestRtClock(Mk2Case):
    def test_rt_ticks_land_across_every_switch(self):
        # 20 kHz RT clock and CTC3 every 256 * 64 T-states, events armed:
        # a run of calls of every kind returns exactly and every NMI ran
        # the handler.
        z = self.boot(ctc3=64)
        self.arm_event(z)
        self.set_cell(z, "t_post_on_irq", 1)
        z.rt_hz = 20000
        z.phys[self.page0(ARGS):self.page0(ARGS) + 4] = bytes([3, 0, 4, 0])
        numbers = (SYS_SUM, SYS_NOP, SYS_LOAD, SYS_POST, SYS_SPIN,
                   SYS_FAIL)
        for i in range(120):
            number = numbers[i % len(numbers)]
            before = self.call(z, number, seed=i)
            self.assert_preserved(z, before)
            self.assertEqual(hardware(z),
                             tuple(SYSTEM_IMAGE + PROCESS_IMAGE) +
                             (CTL_WORKING,))
            self.assertEqual(mirror(z), hardware(z))
            if number == SYS_SUM:
                self.assertEqual(z.h << 8 | z.l, 7)
        self.assertGreater(z.nmi_count, 100)
        self.assertEqual(self.cell(z, "t_rt_count", 2), z.nmi_count)
        self.assertGreater(z.rd(P_TALLY), 10)
        self.assertGreater(z.ctc.delivered[3], 10)


class TestNmiInjection(Mk2Case):
    """An NMI after every instruction of each switch sequence."""

    # ---- the sequences: (prepare, done) on a booted board -------------
    def seq_gate(self, z, number):
        z.phys[self.page0(ARGS):self.page0(ARGS) + 4] = bytes([1, 2, 3, 4])
        self.set_regs(z, 7)
        z.a, z.h, z.l = number, ARGS >> 8, ARGS & 0xFF
        z.pc = self.sym("p_call")
        z.iff1 = z.iff2 = True
        end = self.sym("p_return")
        return lambda z: z.pc == end

    def seq_far_load(self, z):
        z.a = 0x5A
        z.pc = self.sym("p_far")
        self.run_to(z, "p_far_done")
        return self.seq_gate(z, SYS_LOAD)

    def seq_remap(self, z):
        z.phys[self.page0(ARGS)] = 0x48
        return self.seq_gate(z, SYS_REMAP)

    def seq_working_remap(self, z):
        z.a = 0x5B
        z.pc = self.sym("p_far")
        end = self.sym("p_far_done")
        return lambda z: z.pc == end

    def seq_irq(self, z):
        self.set_regs(z, 8)
        z.assert_int(CTC3_VECTOR)
        idle = self.sym("p_idle")
        return lambda z: z.pc == idle and not z.int_pending

    def seq_event(self, z):
        self.arm_event(z)
        self.set_cell(z, "t_post_on_irq", 1)
        self.set_regs(z, 9)
        z.assert_int(CTC3_VECTOR)
        idle = self.sym("p_idle")
        return lambda z: (z.pc == idle and not z.int_pending and
                          z.rd(P_TALLY) == 1)

    def sequences(self):
        return (("gate in and out", lambda z: self.seq_gate(z, SYS_SUM)),
                ("gate out reloading a remapped bank", self.seq_far_load),
                ("system remap", self.seq_remap),
                ("working remap", self.seq_working_remap),
                ("irq in and out", self.seq_irq),
                ("event delivery and return", self.seq_event))

    def reference(self, prepare):
        z = self.boot()
        done = prepare(z)
        trace = [(hardware(z), mirror(z))]
        while not done(z):
            z.step()
            trace.append((hardware(z), mirror(z)))
            self.assertLess(len(trace), 5000)
        return trace, self.final(z)

    def final(self, z):
        return (snapshot(z), z.rd(P_TALLY),
                bytes(z.phys[self.page0(ARGS):self.page0(ARGS) + 6]))

    def assert_ordering(self, name, trace):
        """Mirror before OUT: at every boundary a mirror differs from the
        hardware only in what the next mapper write installs."""
        for b, (hw, mir) in enumerate(trace):
            diff = {r for r in range(17) if hw[r] != mir[r]}
            nxt = next((j for j in range(b + 1, len(trace))
                        if trace[j][0] != trace[j - 1][0]), None)
            if nxt is None:
                self.assertEqual(diff, set(), "%s: boundary %d" % (name, b))
                continue
            new = trace[nxt][0]
            for r in diff:
                self.assertEqual(mir[r], new[r],
                                 "%s: boundary %d register %d" % (name, b, r))
                self.assertNotEqual(trace[nxt - 1][0][r], new[r])

    def test_every_sequence_orders_mirror_before_out(self):
        for name, prepare in self.sequences():
            with self.subTest(sequence=name):
                trace, _ = self.reference(prepare)
                self.assertTrue(any(trace[i][0] != trace[i + 1][0]
                                    for i in range(len(trace) - 1)),
                                "%s changes no mapping" % name)
                self.assert_ordering(name, trace)

    def test_an_nmi_after_every_instruction(self):
        rt_vector = self.sym("t_rt_tick")
        for name, prepare in self.sequences():
            trace, expect = self.reference(prepare)
            steps = len(trace) - 1
            for k in range(steps):
                with self.subTest(sequence=name, step=k):
                    z = self.boot()
                    done = prepare(z)
                    for _ in range(k):
                        z.step()
                    resume, sp = z.pc, z.sp
                    ticks = self.cell(z, "t_rt_count", 2)
                    z.nmi_pending = True
                    z.step()
                    self.assertEqual(z.pc, 0x0066)
                    # The stub is in whichever page 0 is mapped, and the
                    # return address went to RAM.
                    self.assertEqual(bytes(z.rd(0x66 + i) for i in range(3)),
                                     NMI_STUB)
                    self.assertEqual(z.mapper.translate(z.sp)[0], "sram")
                    self.assertEqual(z.rd(z.sp) | (z.rd(z.sp + 1) << 8),
                                     resume)
                    at_nmi = (hardware(z), mirror(z))
                    self.assertEqual(at_nmi, trace[k])
                    reached = False
                    while not (z.pc == resume and z.sp == sp):
                        reached |= z.pc == rt_vector
                        z.step()
                    self.assertTrue(reached)
                    self.assertEqual((hardware(z), mirror(z)), at_nmi)
                    self.assertEqual(self.cell(z, "t_rt_count", 2),
                                     ticks + 1)
                    for _ in range(steps - k):
                        z.step()
                    self.assertTrue(done(z))
                    self.assertEqual(self.final(z), expect)


@unittest.skipUnless(os.path.isfile(IMAGE) and os.path.isfile(MK1_IHX),
                     "make mk2trans builds the images")
class TestGateCost(Mk2Case):
    """The null call's round trip on both boards, on one harness: the
    T-states from the caller's RST 30h to its next instruction, less the
    dispatcher body (the same stub on both).  Informational numbers; the
    test checks that both gates return exactly."""

    def body_and_total(self, z, call, back, syms):
        dispatch = syms["_kernel_syscall_dispatch"]
        resume = syms["kos_syscall_dispatch"] + 9   # after its CALL
        z.pc = call
        start = z.tstates
        z.run(LIMIT, stops=StopSet([dispatch]), check_first=False)
        enter = z.tstates
        z.run(LIMIT, stops=StopSet([resume]), check_first=False)
        body = z.tstates - enter
        z.run(LIMIT, stops=StopSet([back]), check_first=False)
        self.assertEqual(z.pc, back)
        return z.tstates - start - body

    def mk1_board(self):
        image = bytearray(0x10000)
        with open(MK1_IHX) as handle:
            for line in handle:
                count = int(line[1:3], 16)
                addr = int(line[3:7], 16)
                if int(line[7:9], 16) == 0:
                    image[addr:addr + count] = bytes.fromhex(
                        line[9:9 + 2 * count])
        bank0 = bytearray(0x10000)
        bank0[0x0000:0x2000] = image[0x0000:0x2000]
        bank0[0xE000:] = image[0xE000:]
        bank7 = bytearray(0x10000)
        bank7[0x2000:0xE000] = image[0x2000:0xE000]
        z = Z80(bytes(bank0), bytes(bank7))
        z.latch = 0x10                 # mode 10 over bank 0
        z.sp = 0xC000
        z.iff1 = z.iff2 = True
        return z

    def test_both_gates_round_trip(self):
        z = self.boot()
        self.set_regs(z, 1)
        z.a, z.h, z.l = SYS_NOP, ARGS >> 8, ARGS & 0xFF
        z.iff1 = z.iff2 = True
        mk2 = self.body_and_total(z, self.sym("p_call"),
                                  self.sym("p_return"), self.syms)
        mk1syms = z80sim.read_symbols(MK1_MAP)
        z1 = self.mk1_board()
        mk1 = self.body_and_total(z1, mk1syms["m1_call"],
                                  mk1syms["m1_return"], mk1syms)
        self.assertEqual((z1.latch, z1.sp), (0x10, 0xC000))
        self.assertGreater(mk2, mk1)


if __name__ == "__main__":
    unittest.main()
