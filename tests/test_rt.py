#!/usr/bin/env python3
"""The real-time interrupt (architecture 4.12, abi/kos/rt.h).

The production image boots with the emulated IO controller, whose
CMD_RT_CLOCK drives the CPU's NMI (tests/z80sim.py), and runs the card
program tests/rt/rt.c from the shell:

  - boot leaves the vector idle (JP to a lone RETN) and every bank's
    0066h jumping to it;
  - the ABI refuses a handler outside the RT region, a rate out of range,
    a second open and a second program, and an IO controller without the
    RT clock (-KOS_ENOTSUP), leaving nothing held;
  - an open interrupt runs the handler at the rate, from every mapping,
    while `/system` and SYS_REBOOT answer KOS_EBUSY;
  - an exit with the interrupt open releases it: the vector is idle again
    and the IO controller's clock is stopped;
  - an extended-memory object at slot 0 starts above the reserved page
    zero, so filling it whole leaves the bank's NMI entry intact.
"""

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from test_hwtest_sim import HwtestReplay  # noqa: E402
from replay import SHELL_PROMPT  # noqa: E402
from z80sim import Ctc, IocCommandPeer  # noqa: E402

RT_EXE = os.path.join(ROOT, "build", "user", "rt.exe")

RT_VECTOR = 0xEF08
RT_IDLE = 0xEF0B
NMI_ENTRY = 0x0066
JP_VECTOR = bytes([0xC3, RT_VECTOR & 0xFF, RT_VECTOR >> 8])
BUDGET = 2_000_000_000


@unittest.skipUnless(os.path.exists(RT_EXE), "run make")
class TestRtReplay(HwtestReplay):

    def boot(self, supported=True):
        z = self.make_cpu()
        Ctc().attach(z)
        with open(RT_EXE, "rb") as handle:
            executable = handle.read()
        z.hid = z.ioc_peer = IocCommandPeer(
            fs2_media=True, fs2_files={"RT.EXE": executable})
        z.ioc_peer.rt_clock_supported = supported
        self.boot_to_shell(z, prompt=SHELL_PROMPT)
        return z

    def run_command(self, z, command, done):
        since = len(z.glyphs)
        z.hid.hid_queue.extend(command.encode() + b"\r")
        self.run_to_pattern(z, done, since=since, limit=BUDGET)
        self.run_until_count(z, SHELL_PROMPT, 2)
        return self.trace(z, since)

    def bank(self, z, number):
        return z.bank0 if number == 0 else z.banks[number]

    def vector(self, z):
        return bytes(z.bank0[RT_VECTOR:RT_VECTOR + 5])

    def test_boot_routes_every_bank_to_the_idle_vector(self):
        z = self.boot()
        self.assertEqual(self.vector(z), bytes([0xC3, RT_IDLE & 0xFF,
                                                RT_IDLE >> 8, 0xED, 0x45]))
        for number in range(7):
            self.assertEqual(
                bytes(self.bank(z, number)[NMI_ENTRY:NMI_ENTRY + 3]),
                JP_VECTOR, "bank %d" % number)

    def test_the_abi_end_to_end(self):
        z = self.boot()
        nmis = z.nmi_count
        trace = self.run_command(z, "/fs/rt.exe main", r"rt: PASS")
        self.assertIn("rt: info 0 F000 0D00 0", trace)
        self.assertIn("rt: outside -2", trace)
        self.assertIn("rt: slow -2", trace)
        self.assertIn("rt: fast -2", trace)
        self.assertIn("rt: open 0", trace)
        self.assertIn("rt: held 1", trace)
        self.assertIn("rt: again -4", trace)
        # 50 ms is 800 ticks.  The wait polls the clock and ends at the
        # first reading at or past its deadline, and under this NMI load
        # one reading costs about 23 ticks: the count is 800 plus the
        # first reading's latency plus up to one reading.
        ticked = int(re.search(r"rt: ticked (\d+)", trace).group(1))
        self.assertTrue(780 <= ticked <= 860, ticked)
        self.assertIn("rt: system -4", trace)
        self.assertIn("rt: reboot -4", trace)
        self.assertIn("rt: child held 2", trace)
        self.assertIn("rt: child open -4", trace)
        self.assertIn("rt: child 7", trace)
        self.assertIn("rt: alive 1", trace)
        # Every tick became an NMI, and each ran the handler.
        self.assertGreater(z.nmi_count - nmis, ticked)
        self.assertEqual(z.nmi_count, z.rt_ticks)
        # The exit released what main left open.
        self.assertEqual(self.vector(z)[:3],
                         bytes([0xC3, RT_IDLE & 0xFF, RT_IDLE >> 8]))
        self.assertEqual(z.rt_hz, 0)
        self.assertEqual(z.ioc_peer.rt_requests[0], (16000, 16))
        self.assertEqual(z.ioc_peer.rt_requests[-1][0], 0)
        # A later program can take it again.
        trace = self.run_command(z, "/fs/rt.exe nosupport",
                                 r"rt: nosupport held \d")
        self.assertIn("rt: nosupport 0", trace)

    def test_a_controller_without_the_rt_clock(self):
        z = self.boot(supported=False)
        trace = self.run_command(z, "/fs/rt.exe nosupport",
                                 r"rt: nosupport held \d")
        self.assertIn("rt: nosupport -7", trace)
        self.assertIn("rt: nosupport held 0", trace)
        self.assertEqual(self.vector(z)[:3],
                         bytes([0xC3, RT_IDLE & 0xFF, RT_IDLE >> 8]))
        self.assertEqual(z.rt_hz, 0)
        self.assertEqual(z.nmi_count, 0)

    def test_objects_across_the_nmi_entry(self):
        z = self.boot()
        trace = self.run_command(z, "/fs/rt.exe xmem",
                                 r"rt: xmem objects \d+ bad \d+ ticked \d")
        self.assertIn("rt: xmem open 0", trace)
        # Every byte read back as written, while NMIs were arriving.
        self.assertRegex(trace, r"rt: xmem objects 6 bad 0 ticked 1")
        self.assertGreater(z.nmi_count, 0)
        # The NMI entry survived in every bank, and the slot-0 object lies
        # at 0000h itself: its bytes around the entry are in the bank.
        for number in range(7):
            self.assertEqual(
                bytes(self.bank(z, number)[NMI_ENTRY:NMI_ENTRY + 3]),
                JP_VECTOR, "bank %d" % number)

        def pattern(obj, at):
            return (at ^ (at >> 8) ^ (obj * 0x35)) & 0xFF

        found = []
        for number in range(6):
            page = bytes(self.bank(z, number)[0:0x200])
            for obj in range(6):
                if all(page[at] == pattern(obj, at)
                       for at in range(0x200)
                       if not NMI_ENTRY <= at < NMI_ENTRY + 3):
                    found.append((number, obj))
        self.assertEqual(len(found), 1, found)

    def test_idle_waits_for_an_interrupt_not_a_tick(self):
        z = self.boot()
        trace = self.run_command(z, "/fs/rt.exe idle",
                                 r"rt: idle counts \d+ nmis \d+")
        self.assertIn("rt: idle open 0", trace)
        counts, nmis = (int(v) for v in re.search(
            r"rt: idle counts (\d+) nmis (\d+)", trace).groups())
        # The timer ticks at 156.25 Hz: 250 counts of 25.6 us.  Twenty
        # waits span at least nineteen ticks, through hundreds of NMIs.
        self.assertGreaterEqual(counts, 19 * 250)
        self.assertGreater(nmis, 19 * 100)

    def test_the_last_source_cell_is_the_nucleus_cell(self):
        with open(os.path.join(ROOT, "abi", "kos", "rt.h")) as handle:
            cell = int(re.search(r"KOS_IRQ_LAST_SOURCE\s+0x([0-9A-Fa-f]+)u",
                                 handle.read()).group(1), 16)
        with open(os.path.join(ROOT, "build", "konaos.map")) as handle:
            mapped = int(re.search(r"([0-9A-F]{8})\s+_irq_active_source",
                                   handle.read()).group(1), 16)
        self.assertEqual(cell, mapped)

if __name__ == "__main__":
    unittest.main()
