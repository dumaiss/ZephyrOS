"""The simulator's Z80 core against its recorded behaviour.

An opcode fuzz runs on the C core (tests/simcore): random single
instructions, on every opcode page, from random register, memory, latch
and interrupt states (IFF1/IFF2, a pending interrupt, HALT, the EI delay).
Every step's outcome, whole register state and port accesses, and the
memory at each refresh, fold into one SHA-256 that
tests/fixtures/sim_trace.json records.  The workload uses no OS image, so
only a change to the core changes the digest.

The record comes from a core run in lockstep against an independent
instruction model.  A deliberate core change regenerates it:
KOS_SIM_TRACE_UPDATE=1 python3 -m unittest tests.test_sim_trace (review
the cause first).  Skipped, with the reason, when the C core library is
not built.
"""

import hashlib
import json
import os
import random
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))

import z80sim  # noqa: E402
from simcore import core  # noqa: E402

FIXTURE = os.path.join(os.path.dirname(__file__), "fixtures", "sim_trace.json")
UPDATE = os.environ.get("KOS_SIM_TRACE_UPDATE") == "1"
FUZZ_COUNT = 200000
STATE = (core.REGISTERS + core.FLAGS + core.COUNTERS +
         ("cycles", "tstates"))


def state_of(z):
    return tuple(getattr(z, name) for name in STATE)


def logged(z, log):
    """Record every port access of `z` into `log`."""
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


def outcome(z):
    """Step once: the exception (type and text), if any."""
    try:
        z.step()
    except BaseException as exc:  # SystemExit for unsupported encodings
        return (type(exc).__name__, str(exc))
    return None


def memory_digest(z):
    h = hashlib.sha256()
    for bank in range(8):
        h.update(z._sram(bank))
    return h.hexdigest()


def recorded(name):
    try:
        with open(FIXTURE) as handle:
            return json.load(handle).get(name)
    except FileNotFoundError:
        return None


def check(test, name, digest, steps):
    """Compare (or, with KOS_SIM_TRACE_UPDATE=1, record) one workload."""
    entry = {"sha256": digest, "steps": steps}
    if UPDATE:
        data = {}
        if os.path.exists(FIXTURE):
            with open(FIXTURE) as handle:
                data = json.load(handle)
        data[name] = entry
        os.makedirs(os.path.dirname(FIXTURE), exist_ok=True)
        with open(FIXTURE, "w") as handle:
            json.dump(data, handle, indent=2, sort_keys=True)
            handle.write("\n")
        return
    want = recorded(name)
    test.assertIsNotNone(want, "%s: no recorded trace in %s" % (name, FIXTURE))
    test.assertEqual(entry, want, "%s: the core's behaviour changed" % name)


@unittest.skipUnless(os.path.isfile(core.LIBRARY),
                     "the C core is not built (make sim-core)")
class OpcodeFuzzTrace(unittest.TestCase):
    """Random single instructions, folded into one digest."""

    PAGES = (None, 0xCB, 0xED, 0xDD, 0xFD, (0xDD, 0xCB), (0xFD, 0xCB))
    LATCHES = (0x08, 0x09, 0x0E, 0x18, 0x19, 0x1E, 0x38, 0x58, 0xF8,
               0x10, 0x13, 0x30, 0xF0)

    def machine(self, rng, digest):
        z = z80sim.Z80(rng.randbytes(0x10000), rng.randbytes(0x10000))
        for bank in range(1, 7):
            z.banks[bank][:] = rng.randbytes(0x10000)
        log = []
        logged(z, log)
        return z, log

    def test_random_instructions_match_the_record(self):
        rng = random.Random(0x5A80)
        digest = hashlib.sha256()
        z, log = self.machine(rng, digest)
        for trial in range(FUZZ_COUNT):
            if trial % 4096 == 0 and trial:
                digest.update(memory_digest(z).encode())
                z, log = self.machine(rng, digest)
            values = {name: rng.getrandbits(8) for name in
                      core.REGISTERS if name not in
                      ("pc", "sp", "ix", "iy")}
            values.update(pc=rng.getrandbits(16), sp=rng.getrandbits(16),
                          ix=rng.getrandbits(16), iy=rng.getrandbits(16),
                          im=rng.choice((0, 1, 2)))
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
            code = prefix + bytes(rng.getrandbits(8) for _ in range(4))
            z.latch = latch
            for name, value in values.items():
                setattr(z, name, value)
            for name, value in flags.items():
                setattr(z, name, value)
            for offset, byte in enumerate(code):
                z.wr((values["pc"] + offset) & 0xFFFF, byte)
            del log[:]
            digest.update(repr((outcome(z), state_of(z), log)).encode())
        digest.update(memory_digest(z).encode())
        check(self, "opcode-fuzz", digest.hexdigest(), FUZZ_COUNT)


if __name__ == "__main__":
    unittest.main()
