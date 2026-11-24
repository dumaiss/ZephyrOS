#!/usr/bin/env python3
"""Acceptance of the Zephyr-80 Mk2 memory port: architecture 2.7.7.

Section 2.7.7 lists ten properties the Mk2 design must demonstrate on the
emulator before a board exists.  ITEMS maps each one to the simulator
tests that prove it, and each item's test here runs exactly those tests:
the item passes when every one of them passes, and is skipped (never
passed) when any of them is skipped because its image has not been
built.  TestMap holds the map itself: ten items, each with tests, every
named test existing.

The images: `make BOARD=mk2`, `make mk2boot mk2trans mk2proc`; the
standalone-SDK programs also need `make sdk BOARD=mk2`.
"""

import io
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# (item, the property as 2.7.7 states it, the tests proving it)
ITEMS = (
    (1, "Arbitrary LS612 power-on entries do not affect reset/pass-mode "
        "boot.",
     ("test_mk2_decoder.TestReset.test_reset_selects_system_bank_and_pass_mode",
      "test_mk2_decoder.TestReset.test_entries_differ_on_every_reset",
      "test_mk2_decoder.TestReset.test_boot_never_depends_on_the_entries",
      "test_mk2_decoder.TestReset.test_entry_writes_do_not_leave_pass_mode",
      "test_mk2_boot.TestMk2Boot.test_boots_maps_hands_off_and_reboots",
      "test_mk2_shell.TestBoot.test_boot_lands_on_the_prompt_without_a_card")),
    (2, "Every one of the sixteen mapping registers translates the correct "
        "logical 8 KiB region and preserves its 13-bit offset.",
     ("test_mk2_decoder.TestTranslation.test_all_sixteen_entries_map_their_region",
      "test_mk2_decoder.TestTranslation.test_every_page_number_translates")),
    (3, "DAT bank selection changes the whole map instantly and switching "
        "back recovers the previous map without reprogramming entries.",
     ("test_mk2_decoder.TestTranslation.test_dat_bank_switch_changes_the_map_and_back",
      "test_mk2_decoder.TestTranslation.test_control_can_reach_pass_mode_again",
      "test_mk2_boot.TestMk2Boot.test_check_values_are_read_through_each_map",
      "test_mk2_processes.TestMappingRules.test_an_unchanged_image_is_not_reloaded")),
    (4, "System/working contexts share E000h-FFFFh; entry code and stacks "
        "never disappear during the context switch.",
     ("test_mk2_decoder.TestTranslation.test_common_page_shared_by_entries_7_and_15",
      "test_mk2_decoder.TestTranslation.test_code_runs_across_a_bank_switch_from_mapped_memory",
      "test_mk2_boot.TestMk2Boot.test_payload_runs_from_the_common_page",
      "test_mk2_transitions.TestNmiInjection.test_an_nmi_after_every_instruction")),
    (5, "Normal OUT (n),A mapper writes honor the actual Z80 upper I/O "
        "address byte; the 800Fh control write never alters a DAT entry.",
     ("test_mk2_decoder.TestPortDecode",
      "test_mk2_decoder.TestStrictEntryWrites")),
    (6, "ROM reads, ignored ROM writes, SRAM writes and explicit ROM-to-RAM "
        "boot copying follow the Mk2 model, not the Mk1 shadow decoder.",
     ("test_mk2_decoder.TestReset.test_pass_mode_shows_the_first_64k_of_rom",
      "test_mk2_decoder.TestTranslation.test_all_sixteen_entries_map_their_region",
      "test_mk2_decoder.TestBoardSelection.test_mk2_has_no_mk1_latch",
      "test_mk2_boot.TestMk2Boot.test_mapping_switch_runs_in_rom_page_zero",
      "test_mk2_nucleus.TestBoot.test_handoff_state",
      "test_mk2_nucleus.TestRomfs")),
    (7, "Syscall and driver IPC entry/return preserve caller buffers, "
        "current mapping and SP, including from a mapped far window.",
     ("test_mk2_transitions.TestGate",
      "test_mk2_processes.TestObjects.test_data_survives_and_is_correct_through_read_write_and_map",
      "test_mk2_shell.TestPrograms.test_a_card_program_streams_through_the_driver",
      "test_mk2_shell.TestIpcCost",
      "test_mk2_programs.TestPlacedProgram",
      "test_mk2_programs.TestStandaloneSdk.test_fardata_maps_its_data_segment_and_calls_far_code")),
    (8, "IM2, NMI and nested kernel/driver execution return to the exact "
        "interrupted context and stack.",
     ("test_mk2_transitions.TestIrq",
      "test_mk2_transitions.TestEvents",
      "test_mk2_transitions.TestRtClock",
      "test_mk2_transitions.TestNmiInjection",
      "test_mk2_nucleus.TestBoot.test_a_ctc_tick_is_taken_and_counted",
      "test_mk2_shell.TestIrqTrampoline",
      "test_mk2_programs.TestPlacedProgram",
      "test_mk2_programs.TestStandaloneSdk.test_pcmplay_plays_on_the_rt_interrupt")),
    (9, "Process exec/exit, DAT-image invalidation, far windows and "
        "allocation pass the corresponding KonaOS acceptance tests.",
     ("test_mk2_processes.TestPages",
      "test_mk2_processes.TestExec",
      "test_mk2_processes.TestObjects",
      "test_mk2_processes.TestMappingRules",
      "test_mk2_shell.TestPrograms",
      "test_mk2_programs.TestPrograms",
      "test_mk2_programs.TestExerciser",
      "test_mk2_programs.TestStandaloneSdk",
      "test_mk2_programs.TestBoardCheck")),
    (10, "Reset and SYS_REBOOT both reach a deterministic ROM boot view.",
     ("test_mk2_decoder.TestReset.test_ioc_reset_request_resets_the_board",
      "test_mk2_boot.TestMk2Boot.test_ioc_reset_returns_to_the_pass_mode_rom_view",
      "test_mk2_shell.TestReboot")),
)


def load(names):
    return unittest.defaultTestLoader.loadTestsFromNames(names)


def count(suite):
    return suite.countTestCases()


class TestMap(unittest.TestCase):
    def test_every_item_has_existing_tests(self):
        self.assertEqual([item for item, _, _ in ITEMS], list(range(1, 11)))
        for item, _, names in ITEMS:
            with self.subTest(item=item):
                self.assertTrue(names)
                for name in names:
                    # A misspelt name loads as a failing _FailedTest.
                    suite = load([name])
                    self.assertGreater(count(suite), 0, name)
                    for case in _cases(suite):
                        self.assertNotIn("_FailedTest", type(case).__name__,
                                         name)


def _cases(suite):
    for test in suite:
        if isinstance(test, unittest.TestSuite):
            yield from _cases(test)
        else:
            yield test


class TestPortSequence(unittest.TestCase):
    """One test per 2.7.7 item: its proving tests, run here."""

    def prove(self, number):
        _, text, names = ITEMS[number - 1]
        suite = load(names)
        result = unittest.TestResult()
        with _quiet():
            suite.run(result)
        problems = result.failures + result.errors
        if problems:
            self.fail("item %d (%s): %s" % (number, text, "; ".join(
                "%s: %s" % (test.id(), trace.strip().splitlines()[-1])
                for test, trace in problems)))
        if result.skipped:
            self.skipTest("item %d not proved here: %s" % (number, "; ".join(
                sorted({reason for _, reason in result.skipped}))))
        self.assertEqual(result.testsRun, count(suite))

    def test_item_01_power_on_entries_do_not_affect_boot(self):
        self.prove(1)

    def test_item_02_every_entry_translates_its_region(self):
        self.prove(2)

    def test_item_03_bank_selection_switches_the_whole_map(self):
        self.prove(3)

    def test_item_04_common_page_survives_the_context_switch(self):
        self.prove(4)

    def test_item_05_port_decode_and_control_write(self):
        self.prove(5)

    def test_item_06_rom_and_sram_follow_the_mk2_model(self):
        self.prove(6)

    def test_item_07_syscalls_and_ipc_preserve_the_caller(self):
        self.prove(7)

    def test_item_08_interrupts_return_to_the_exact_context(self):
        self.prove(8)

    def test_item_09_processes_far_windows_and_allocation(self):
        self.prove(9)

    def test_item_10_reset_and_reboot_reach_the_rom_view(self):
        self.prove(10)


class _quiet:
    """Hold the proving tests' informational stderr lines (costs)."""

    def __enter__(self):
        self.saved = sys.stderr
        sys.stderr = io.StringIO()

    def __exit__(self, *exc):
        sys.stderr = self.saved
        return False


if __name__ == "__main__":
    unittest.main()
