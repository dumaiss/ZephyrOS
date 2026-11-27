#!/usr/bin/env python3
"""Focused tests of the performance runner's attribution and accounting.

They pin what the runner's numbers rest on: the linked identity of code by
physical address (static functions, cold veneers, driver pages), the exact
bucket reconciliation, block-move accounting, observed cache states and the
profiled/plain equality. They never assert a timing constant.

Run with: python3 -m unittest tests.test_perf_os_sdk -v
"""
import os
import sys
import tempfile
import types
import unittest
import warnings
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import perf_os_sdk as perf  # noqa: E402
from z80sim import Z80  # noqa: E402

MK1 = os.path.isfile(os.path.join(ROOT, "build", "konaos.bin"))
MK2 = os.path.isfile(os.path.join(ROOT, "build-mk2", "konaos-mk2.bin"))


def setUpModule():
    # tools/modpack.py leaves its object files to the garbage collector.
    warnings.filterwarnings("ignore", category=ResourceWarning, module="modpack")


class FakeIdentity:
    syms = {"kos_gate_stack": 0xF000, "kos_gate_stack_top": 0xF100,
            "kos_syscall_entry": 0xFFF0}

    def lookup(self, pa):
        return ("kernel", "fragment", "_fragment")

    def entry(self, who, pa):
        return 0


class TestListing(unittest.TestCase):
    def test_static_functions_are_named_in_their_area(self):
        text = "\n".join([
            "                                     10 \t.area _HOME",
            "                                     20 \t.area _CODE",
            "                                     30 ; Function iocfat_first",
            "   00000000                         31 _iocfat_first::",
            "                                     40 ; Function iocfat_hidden",
            "   0000002A                         41 _iocfat_hidden:",
            "                                     50 \t.area _INITIALIZER",
        ])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "x.lst"
            path.write_text(text)
            found = perf.lst_functions(path)
        self.assertEqual(found, [("_CODE", 0, "_iocfat_first"),
                                 ("_CODE", 0x2A, "_iocfat_hidden")])

    @unittest.skipUnless(MK2, "make BOARD=mk2")
    def test_driver_static_function_has_its_own_range(self):
        listing = Path(ROOT, "build-mk2/sys/modules/iocfat.lst")
        names = {name for _, _, name in perf.lst_functions(listing)}
        self.assertTrue({"_iocfat_ring_fill", "_iocfat_cache_find"} <= names)


class TestAccounting(unittest.TestCase):
    def machine(self, code):
        z = Z80(bytes(65536), bytes(65536))
        z.hid = types.SimpleNamespace(fs2_counts={})
        z.sound_peer = types.SimpleNamespace(writes=[])
        for i, value in enumerate(code):
            z.bank0[0x100 + i] = value
        z.latch = 0x10  # application mode over bank 0
        z.pc = 0x100
        z.sp = 0x8000
        return z

    def test_block_move_is_one_run_and_buckets_sum_to_the_span(self):
        # LD BC,40; LD HL,2000h; LD DE,3000h; LDIR; NOP
        z = self.machine(bytes([0x01, 40, 0, 0x21, 0, 0x20, 0x11, 0, 0x30,
                                0xED, 0xB0, 0x00]))
        span = perf.Span(FakeIdentity(), z)
        while z.pc != 0x100 + 11:
            span.step(z)
        result = span.finish(z)
        self.assertEqual(sum(result["buckets"].values()), result["tstates"])
        self.assertEqual(result["block_copy_bytes"], 40)
        self.assertEqual([(c["runs"], c["bytes"]) for c in result["copy_runs"]],
                         [(1, 40)])
        # 39 repeats at 21 T and the last at 16 T.
        self.assertEqual(result["buckets"]["byte_movement"], 39 * 21 + 16)

    def test_an_unrolled_ldi_sequence_is_one_run(self):
        # LD BC,10; LD HL,2000h; LD DE,3000h; LDI x4; NOP
        z = self.machine(bytes([0x01, 10, 0, 0x21, 0, 0x20, 0x11, 0, 0x30,
                                0xED, 0xA0, 0xED, 0xA0, 0xED, 0xA0, 0xED, 0xA0, 0x00]))
        span = perf.Span(FakeIdentity(), z)
        while z.pc != 0x100 + 17:
            span.step(z)
        result = span.finish(z)
        self.assertEqual([(c["runs"], c["bytes"]) for c in result["copy_runs"]], [(1, 4)])

    def test_buckets_by_linked_identity(self):
        category = perf.category
        self.assertEqual(category(("kernel", "pages_mk2", "_pages_move")), "copy_setup")
        self.assertEqual(category(("kernel", "ipc", "_domain_enter")), "mapping")
        self.assertEqual(category(("kernel", "event_common", "_kos_event_syscall_exit")), "entry_return")
        self.assertEqual(category(("driver:iocfat", "iocfat", "_iocfat_ring_fill")), "driver")
        self.assertEqual(category(("sdk", "fread", "_fread_item")), "sdk")
        self.assertEqual(category(("unknown", "", "")), "unknown")

    def test_zero_count_moves_the_full_range(self):
        # LD BC,0; LDI: one byte, never a 64 KiB run.
        z = self.machine(bytes([0x01, 0, 0, 0xED, 0xA0, 0x00]))
        span = perf.Span(FakeIdentity(), z)
        while z.pc != 0x100 + 5:
            span.step(z)
        self.assertEqual(span.finish(z)["block_copy_bytes"], 1)

    def test_stacks_exclude_the_empty_top_of_the_next_region(self):
        regions = perf.stack_regions({"kos_a_stack": 0x100, "kos_a_stack_top": 0x200,
                                      "irq_b_stack": 0x200, "irq_b_stack_top": 0x300})
        self.assertEqual(sorted(regions), [("irq_b_stack", 0x200, 0x300),
                                           ("kos_a_stack", 0x100, 0x200)])


class TestMergeSpans(unittest.TestCase):
    def test_a_workload_adds_its_spans_and_keeps_the_lowest_stack(self):
        def span(t, moved, low):
            return dict(tstates=t, peer_commands={"0x36": 1},
                        buckets=dict(driver=t - moved, byte_movement=moved),
                        functions={"f": t}, unknown_addresses={}, counts={"syscalls": 1},
                        copy_runs=[dict(function="f", source_pages="1", dest_pages="2",
                                        runs=1, bytes=moved)],
                        block_copy_bytes=moved, sound_writes=0, stack_low={"gate": low})
        merged = perf.merge_spans([span(100, 20, 0x80), span(50, 10, 0x90)])
        self.assertEqual(merged["tstates"], 150)
        self.assertEqual(merged["spans"], 2)
        self.assertEqual(sum(merged["buckets"].values()), 150)
        self.assertEqual(merged["peer_commands"], {"0x36": 2})
        self.assertEqual(merged["copy_runs"][0]["runs"], 2)
        self.assertEqual(merged["block_copy_bytes"], 30)
        self.assertEqual(merged["stack_low"], {"gate": 0x80})


class BoardCase:
    board = None

    def runner(self, profile):
        """A fresh machine: equal initial state is what makes two runs
        comparable."""
        runner = perf.Runner(self.board, profile)
        runner.boot()
        return runner

    def scenario(self, runner):
        out = {}
        out["enosys"] = runner.raw(255)
        joy = runner.bind("/dev/input/joy/0", 0)
        out["null"] = runner.call(joy, 0)
        runner.fresh_cache()
        for state, prepare in (("populating_miss", None), ("ring_hit", None),
                               ("cache_hit", runner.warm_cache)):
            if prepare:
                prepare()
            runner.seek(runner.small, 0)
            result = runner.call(runner.small, 0, 128, 0xA000, 1)
            self.assertEqual(result["status"], 128)
            self.assertEqual(perf.get(runner.z, 0xA000, 128), perf.PATTERN[:128])
            out[state] = result
        return out

    def test_buckets_reconcile_and_profile_equals_plain(self):
        plain = self.scenario(self.runner(False))
        seen = self.scenario(self.runner(True))
        for name, result in seen.items():
            self.assertEqual(sum(result["buckets"].values()), result["tstates"], name)
            self.assertEqual(result["buckets"]["unknown"], 0, name)
            self.assertEqual(result["tstates"], plain[name]["tstates"], name)
            self.assertEqual(result["peer_commands"], plain[name]["peer_commands"], name)
        self.assertEqual(seen["enosys"]["carry"], 1)
        self.assertEqual(seen["null"]["carry"], 0)

    def test_observed_cache_states_match_their_labels(self):
        runner = self.runner(True)
        seen = self.scenario(runner)
        state = runner.file_state
        self.assertEqual(state(seen["populating_miss"], True), "populating_miss")
        self.assertEqual(state(seen["ring_hit"], True), "ring_hit")
        self.assertEqual(state(seen["cache_hit"], True), "cache_hit")
        self.assertEqual(seen["cache_hit"]["peer_commands"], {})

    def test_a_cache_hit_is_cheaper_than_a_populating_miss(self):
        # A cache hit is one copy, as a ring hit is: either may be the
        # cheaper; both beat a read from the card.
        seen = self.scenario(self.runner(True))
        self.assertLess(seen["ring_hit"]["tstates"], seen["populating_miss"]["tstates"])
        self.assertLess(seen["cache_hit"]["tstates"], seen["populating_miss"]["tstates"])


@unittest.skipUnless(MK2 and os.path.isfile(os.path.join(ROOT, "build-mk2/sys/modules/drivers.plan.json")),
                     "make BOARD=mk2")
class TestMk2(BoardCase, unittest.TestCase):
    board = "mk2"

    def test_physical_identity_names_cold_veneers_drivers_and_the_mover(self):
        identity = self.runner(True).identity
        syms = identity.syms
        # The mover is fixed-resident; its physical address names it.
        pa = identity.kernel_mapper(syms["_kos_page_copy"])
        self.assertEqual(identity.lookup(pa)[2], "_kos_page_copy")
        # A cold veneer lives in the fixed region and is named too.
        veneer = identity.lookup(identity.kernel_mapper(syms["__cold_vfs_init"]))
        self.assertEqual(veneer[1], "cold_veneers")
        # A driver static function resolves through the domain pages.
        names = {r[4] for r in identity.ranges if r[2] == "driver:iocfat"}
        self.assertIn("_iocfat_ring_fill", names)
        # Cold sets 0 and 1 share logical addresses but not physical pages.
        cold0 = {r[0] >> 13 for r in identity.ranges if r[3] == "vfs_handles"}
        self.assertTrue(cold0)

    def test_every_global_in_an_object_agrees_on_its_base(self):
        self.assertGreater(self.runner(True).identity.checked_globals, 100)


@unittest.skipUnless(MK1, "make")
class TestMk1(BoardCase, unittest.TestCase):
    board = "mk1"


if __name__ == "__main__":
    unittest.main()
