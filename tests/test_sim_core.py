"""The C Z80 core against the Python core, in lockstep.

The C core (tests/simcore, `make sim-core`) must be the Python core's
semantics exactly.  An opcode fuzz runs random instructions, on every
opcode page, from random register, memory and latch states through both
cores and compares the whole state after each one.  The replay lockstep
runs both cores side by side through the production boot to the shell
and through `tui_test foundation` on the serial and V9958 lanes, and
compares the state at every instruction.  Skipped, with the reason, when
the library is not built.
"""

import os
import random
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))

import z80sim  # noqa: E402
from simcore import core  # noqa: E402

FUZZ_COUNT = int(os.environ.get("KOS_SIM_FUZZ", "200000"))
STATE = (core.REGISTERS + core.FLAGS + core.COUNTERS +
         ("cycles", "tstates"))


def c_class():
    return core.make_core_class(
        z80sim.PythonZ80, z80sim.Ctc,
        (z80sim.ONE_BYTE_T, z80sim.ED_T, z80sim.DD_T, z80sim.ED_BLOCK,
         z80sim.INTERRUPT_TSTATES))


def state_of(z):
    return tuple(getattr(z, name) for name in STATE)


def same_memory(one, other):
    """Bank by bank, compared in place."""
    return all(one._sram(bank) == other._sram(bank) for bank in range(8))


def logged(z, log):
    """Record every port access of `z` into `log` (the port model is the
    Python one on both cores)."""
    io_in = z.io_in
    io_out = z.io_out

    def logged_in(port):
        value = io_in(port)
        log.append(("in", port & 0xFF, value))
        return value

    def logged_out(port, value):
        log.append(("out", port & 0xFF, value & 0xFF))
        io_out(port, value)

    z.io_in = logged_in
    z.io_out = logged_out


def outcome(z, log):
    """Step once: the exception (type and text), if any."""
    try:
        z.step()
    except BaseException as exc:  # SystemExit for unsupported encodings
        return (type(exc).__name__, str(exc))
    return None


@unittest.skipUnless(os.path.isfile(core.LIBRARY),
                     "the C core is not built (make sim-core)")
class OpcodeFuzz(unittest.TestCase):
    """Random single instructions through both cores."""

    PAGES = (None, 0xCB, 0xED, 0xDD, 0xFD, (0xDD, 0xCB), (0xFD, 0xCB))
    LATCHES = (0x08, 0x09, 0x0E, 0x18, 0x19, 0x1E, 0x38, 0x58, 0xF8,
               0x10, 0x13, 0x30, 0xF0)

    def machines(self, rng):
        page0 = rng.randbytes(0x10000)
        page1 = rng.randbytes(0x10000)
        python = z80sim.PythonZ80(page0, page1)
        native = c_class()(page0, page1)
        for bank in range(1, 7):
            data = rng.randbytes(0x10000)
            python.banks[bank][:] = data
            native.banks[bank][:] = data
        return python, native

    def test_random_instructions_match(self):
        rng = random.Random(0x5A80)
        python, native = self.machines(rng)
        logs = ([], [])
        logged(python, logs[0])
        logged(native, logs[1])
        for trial in range(FUZZ_COUNT):
            if trial % 4096 == 0 and trial:
                # Fresh memory now and then, so stores do not converge.
                python, native = self.machines(rng)
                logs = ([], [])
                logged(python, logs[0])
                logged(native, logs[1])
            values = {name: rng.getrandbits(8) for name in
                      core.REGISTERS if name not in
                      ("pc", "sp", "ix", "iy")}
            values.update(pc=rng.getrandbits(16), sp=rng.getrandbits(16),
                          ix=rng.getrandbits(16), iy=rng.getrandbits(16),
                          im=rng.choice((0, 1, 2)))
            # Keep block instructions short: B or BC small.
            if rng.random() < 0.9:
                values["b"] = rng.choice((0, 0, 1, 2))
            latch = rng.choice(self.LATCHES)
            flags = {"iff1": rng.random() < 0.5, "iff2": rng.random() < 0.5,
                     "halted": rng.random() < 0.02,
                     "int_pending": rng.random() < 0.05,
                     "int_vector": rng.getrandbits(8),
                     "ei_delay": rng.choice((0, 0, 0, 1, 2))}
            page = rng.choice(self.PAGES)
            if page is None:
                prefix = b""
            elif isinstance(page, tuple):
                prefix = bytes(page)
            else:
                prefix = bytes((page,))
            body = bytes(rng.getrandbits(8) for _ in range(4))
            code = prefix + body
            for z in (python, native):
                z.latch = latch
                for name, value in values.items():
                    setattr(z, name, value)
                for name, value in flags.items():
                    setattr(z, name, value)
                for offset, byte in enumerate(code):
                    z.wr((values["pc"] + offset) & 0xFFFF, byte)
            del logs[0][:], logs[1][:]
            got = (outcome(python, logs[0]), outcome(native, logs[1]))
            where = "trial %d latch %02X code %s regs %r flags %r" % (
                trial, latch, code.hex(), values, flags)
            self.assertEqual(got[0], got[1], where)
            self.assertEqual(state_of(python), state_of(native), where)
            self.assertEqual(logs[0], logs[1], where)
            self.assertTrue(same_memory(python, native), where)


def lockstep(test, pair, limit, until, every=0x1FFF):
    """Step the (Python, C) machines of `pair` together: the delay skip,
    one step each, then the whole state and the port calls compared; on
    the periodic step both are pumped and their transcripts compared.
    Ends when `until(z)` holds for the Python machine (checked at the
    pump) and returns the steps run."""
    (python_replay, python), (native_replay, native) = pair
    logs = ([], [])
    logged(python, logs[0])
    logged(native, logs[1])
    for step in range(limit):
        python_replay.skip_delay(python)
        native_replay.skip_delay(native)
        got = (outcome(python, logs[0]), outcome(native, logs[1]))
        if got[0] != got[1] or state_of(python) != state_of(native) or \
                logs[0] != logs[1]:
            test.fail("diverged at step %d: pc %04X/%04X op %s, %r vs %r, "
                      "io %r vs %r" % (
                          step, python.pc, native.pc,
                          bytes(python.rd((python.pc + i) & 0xFFFF)
                                for i in range(4)).hex(),
                          got[0], got[1], logs[0][-4:], logs[1][-4:]))
        if got[0] is not None:
            test.fail("both cores raised at step %d: %r" % (step, got[0]))
        del logs[0][:], logs[1][:]
        if step % every == 0:
            python_replay.pump(python)
            native_replay.pump(native)
            test.assertEqual(bytes(python.sio_tx), bytes(native.sio_tx))
            test.assertEqual(python.glyphs, native.glyphs)
            if until(python):
                test.assertTrue(same_memory(python, native))
                return step
    test.fail("lockstep did not reach its end in %d steps" % limit)


@unittest.skipUnless(os.path.isfile(core.LIBRARY),
                     "the C core is not built (make sim-core)")
class ReplayLockstep(unittest.TestCase):
    """Both cores through the production boot and a TUI run, on the
    serial and the V9958 lane."""

    @classmethod
    def setUpClass(cls):
        import replay as replay_module
        import test_tui_sim
        cls.replay_module = replay_module
        cls.tui = test_tui_sim

    def machines(self, lane):
        """Two identical booted-from-reset machines, (Python, C)."""
        base = lane
        base.setUpClass()
        self.addCleanup(base.tearDownClass)
        pair = []
        saved = self.replay_module.Z80
        try:
            for cls in (z80sim.PythonZ80, c_class()):
                self.replay_module.Z80 = cls
                replay = self.tui._Replay(base.image, base.SUBDIR)
                z = replay.make_cpu()
                if base.SERIAL:
                    z.hid = z.ioc_peer = z80sim.IocCommandPeer()
                    replay.start_kernel(z, host_sync=b"\xaa\x55")
                else:
                    replay.start_kernel(z)
                pair.append((replay, z))
        finally:
            self.replay_module.Z80 = saved
        return pair

    def run_lane(self, lane):
        pair = self.machines(lane)
        prompt = self.tui.SHELL_PROMPT
        if lane.SERIAL:
            lockstep(self, pair, self.tui.STEP_LIMIT,
                     lambda z: prompt.encode("ascii") in bytes(z.sio_tx))
            for _replay, z in pair:
                z.sio_rx.extend(b"/system/tui_test.exe foundation\r")
            lockstep(self, pair, self.tui.STEP_LIMIT,
                     lambda z: b"tui: PASS" in bytes(z.sio_tx))
        else:
            lockstep(self, pair, self.tui.STEP_LIMIT,
                     lambda z: prompt in "".join(z.glyphs))
            for _replay, z in pair:
                z.hid.hid_queue.extend(b"/system/tui_test.exe foundation\r")
            lockstep(self, pair, self.tui.STEP_LIMIT,
                     lambda z: "tui: PASS" in "".join(z.glyphs))

    def test_serial_lane_boot_and_tui_run_match(self):
        self.run_lane(self.tui.TestTuiSerialScreen)

    def test_v9958_lane_boot_and_tui_run_match(self):
        self.run_lane(self.tui.TestTuiV9958Screen)


if __name__ == "__main__":
    unittest.main()
