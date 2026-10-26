#!/usr/bin/env python3
"""End-to-end and semantic tests for the direct CSM compiler."""

from __future__ import annotations

import hashlib
import tempfile
import unittest
from pathlib import Path

import csm_language
import csmc
import fur2ztr


PROJECT = Path(__file__).resolve().parent.parent
RUNAWAY = PROJECT / "songs/RunawayCircuit.csm"

ARTICULATION_SOURCE = r"""
song ArticulationTest {
    tick_rate = 60;
    ticks_per_row = 3;
    rows_per_quarter = 2;
    tuning = 440;
    meter = meter(4, 4);
    roles = [role(lead, 8)];
    play = main;
    pattern phrase = | c4@6^4^7 -d4@15>1 = ^2 ^ _ ~ e4<2 |;
    section only { lead = phrase; }
    instrument Voice {
        priority = required;
        level = 255;
        gate = 6;
        envelope = adsr(0, 0, 255, 3);
    }
    orchestra Basic {
        layer lead.main {
            source = lead;
            instrument = Voice;
            route = C;
        }
    }
    scene normal { enables = [lead.main]; }
    form main {
        using = Basic;
        sequence = [play(only, normal)];
        loop = 0;
    }
}
target Zephyr {
    backend = sn76489x4;
    clock = 3579545;
    transpose = 0;
    routes { C = psg0; }
    realize Voice { waveform = square; }
}
"""

HANDOVER_SOURCE = ARTICULATION_SOURCE.replace(
    "roles = [role(lead, 8)];",
    "roles = [role(lead, 8), role(next, 8)];",
).replace(
    "pattern phrase = | c4@6^4^7 -d4@15>1 = ^2 ^ _ ~ e4<2 |;\n    section only { lead = phrase; }",
    "pattern phrase = | c4@24^4^7 _ _ _ _ _ _ _ |;\n"
    "    pattern answer = | d4@24 _ _ _ _ _ _ _ |;\n"
    "    section first { lead = phrase; }\n"
    "    section second { next = answer; }",
).replace(
    "gate = 6;",
    "gate = 24;",
).replace(
    "envelope = adsr(0, 0, 255, 3);",
    "envelope = adsr(0, 0, 255, 0);",
).replace(
    "layer lead.main {\n            source = lead;\n            instrument = Voice;\n            route = C;\n        }",
    "layer lead.main {\n            source = lead;\n            instrument = Voice;\n            route = C;\n        }\n"
    "        layer next.main {\n            source = next;\n            instrument = Voice;\n            route = C;\n        }",
).replace(
    "scene normal { enables = [lead.main]; }",
    "scene first_scene { enables = [lead.main]; }\n"
    "    scene second_scene { enables = [next.main]; }",
).replace(
    "sequence = [play(only, normal)];",
    "sequence = [play(first, first_scene), play(second, second_scene)];",
)


def compiler_for_text(source: str) -> csmc.Compiler:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "test.csm"
        path.write_text(source, encoding="utf-8")
        ir, diagnostics = csm_language.compile_source(path)
    if diagnostics.error_count:
        raise AssertionError([item.message for item in diagnostics.items])
    return csmc.Compiler(ir)


class DirectCompilerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.song, cls.data = csmc.compile_path(RUNAWAY)

    def test_runaway_compiles_and_round_trips(self) -> None:
        decoded = fur2ztr.decode_ztr(self.data)
        fur2ztr.validate_round_trip(self.song, decoded.song)
        self.assertEqual(self.song.name, "RunawayCircuit")
        self.assertEqual((self.song.tick_rate, self.song.speed), (62, 3))
        self.assertEqual((self.song.pattern_length, len(self.song.orders)), (32, 136))
        self.assertEqual(len(self.song.instruments), 11)
        self.assertLessEqual(len(self.data), 20 * 1024)

    def test_backend_features_are_present(self) -> None:
        instruments = {item.name: item for item in self.song.instruments}
        self.assertIn("pitch", instruments["Lead"].macros)
        self.assertIn("pitch", instruments["LeadDetune (+8/128)"].macros)
        self.assertEqual(instruments["Kick"].macros["duty"].values[0].value, 0)
        self.assertEqual(instruments["Snare"].macros["duty"].values[0].value, 1)
        self.assertEqual(instruments["Hat"].macros["duty"].values[0].value, 1)
        self.assertEqual(
            [item.value for item in instruments["Kick"].macros["phaseReset"].values],
            [1, 0],
        )
        effects = [
            event.effect
            for events in self.song.patterns.values()
            for event in events
            if event.effect is not None
        ]
        self.assertTrue(all(effect == (fur2ztr.EFFECTS[0xFF][0], 0) for effect in effects))
        self.assertFalse(any(
            event.mode_reset
            for events in self.song.patterns.values()
            for event in events
        ))

    def test_articulation_and_dynamics_notation_compiles(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "articulation.csm"
            path.write_text(ARTICULATION_SOURCE, encoding="utf-8")
            song, data = csmc.compile_path(path)
        fur2ztr.validate_round_trip(song, fur2ztr.decode_ztr(data).song)
        events = [event for stream in song.patterns.values() for event in stream]
        self.assertTrue(any(event.legato and event.note is not None for event in events))
        self.assertTrue(any(event.arpeggio == 0x47 for event in events))
        self.assertTrue(any(event.arpeggio == 0x22 for event in events))
        self.assertTrue(any(event.arpeggio == -1 for event in events))
        self.assertTrue(any(event.hairpin == -1 and event.hairpin_ceiling == 15 for event in events))
        self.assertTrue(any(event.hairpin == 0 for event in events))
        self.assertTrue(any(event.hairpin == 2 for event in events))
        self.assertFalse(any(event.release and event.row == 2 for event in events))

    def test_voice_handover_resets_persistent_modes(self) -> None:
        compiler = compiler_for_text(HANDOVER_SOURCE)
        song = compiler.build_song()
        self.assertEqual(compiler.allocations["lead.main"], compiler.allocations["next.main"])
        self.assertTrue(any(
            event.note is not None and event.mode_reset
            for stream in song.patterns.values()
            for event in stream
        ))

    def test_legato_and_standalone_modifiers_require_a_sounding_note(self) -> None:
        for original, replacement, message in (
            ("c4@6^4^7 -d4@15>1", "~ -d4@15>1", "legato pitch has no sustained note"),
            ("c4@6^4^7 -d4@15>1", "~ >1", "modifier has no sounding note"),
        ):
            source = ARTICULATION_SOURCE.replace(original, replacement, 1)
            compiler = compiler_for_text(source)
            with self.assertRaisesRegex(csmc.CsmCompileError, message):
                compiler.build_song()

    def test_notation_ranges_and_alignment_are_checked(self) -> None:
        for original, replacement, code in (
            ("c4@6^4^7", "c4@6^4^16", "PAT015"),
            ("-d4@15>1", "-d4@15>16", "PAT016"),
        ):
            source = ARTICULATION_SOURCE.replace(original, replacement, 1)
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "invalid.csm"
                path.write_text(source, encoding="utf-8")
                _ir, diagnostics = csm_language.compile_source(path)
            self.assertTrue(any(item.code == code for item in diagnostics.items))

        compiler = compiler_for_text(ARTICULATION_SOURCE.replace("-d4@15>1", "-d4@5>1", 1))
        with self.assertRaisesRegex(csmc.CsmCompileError, "does not align"):
            compiler.build_song()

    def test_allocator_respects_routes_and_reuses_idle_voice(self) -> None:
        compiler = compiler_for_text(RUNAWAY.read_text(encoding="utf-8"))
        compiler.build_song()
        self.assertTrue(all(channel is not None for channel in compiler.allocations.values()))
        self.assertEqual(compiler.allocations["bg.alternate[L]"] // 4, 0)
        self.assertEqual(compiler.allocations["bg.alternate[R]"] // 4, 1)
        self.assertEqual(compiler.allocations["drum.kick"], compiler.allocations["drum.snare"])

    def test_optional_layers_are_dropped_when_route_is_full(self) -> None:
        source = RUNAWAY.read_text(encoding="utf-8").replace(
            "C = [psg2, psg3];", "C = psg2;", 1
        )
        compiler = compiler_for_text(source)
        compiler.build_song()
        dropped = [unit for unit in compiler.allocation_units if compiler.allocations[unit.key] is None]
        self.assertTrue(dropped)
        self.assertTrue(all(not unit.required for unit in dropped))
        self.assertTrue(all(compiler.allocations[unit.key] is not None for unit in compiler.allocation_units if unit.required))

    def test_required_overcommit_is_an_error(self) -> None:
        source = RUNAWAY.read_text(encoding="utf-8").replace(
            "C = [psg2, psg3];", "C = psg2;", 1
        ).replace("priority \t= optional;", "priority \t= required;")
        compiler = compiler_for_text(source)
        with self.assertRaisesRegex(csmc.CsmCompileError, "cannot allocate required"):
            compiler.build_song()

    def test_explicit_binding_precolours_an_allocation(self) -> None:
        source = RUNAWAY.read_text(encoding="utf-8").replace(
            "    limits {",
            "    bind {\n        lead.main = psg3.tone2;\n    }\n\n    limits {",
            1,
        )
        compiler = compiler_for_text(source)
        compiler.build_song()
        self.assertEqual(compiler.allocations["lead.main"], 14)

    def test_backend_extension_is_preserved_and_validated(self) -> None:
        compiler = compiler_for_text(RUNAWAY.read_text(encoding="utf-8"))
        kick = compiler.realization_nodes["Kick"]
        self.assertEqual(csmc.properties(kick["extensions"]["sn76489"])["phase_reset"], 1)
        source = RUNAWAY.read_text(encoding="utf-8").replace("phase_reset = 1;", "unknown = 1;", 1)
        with self.assertRaisesRegex(csmc.CsmCompileError, "unsupported sn76489 extension property"):
            compiler_for_text(source)

    def test_output_is_deterministic(self) -> None:
        _song, again = csmc.compile_path(RUNAWAY)
        self.assertEqual(hashlib.sha256(again).digest(), hashlib.sha256(self.data).digest())

    def test_note_duration_is_retained_in_semantic_ir(self) -> None:
        source = RUNAWAY.read_text(encoding="utf-8").replace("| c3  ~ ~ c3", "| c3@9 ~ ~ c3", 1)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "duration.csm"
            path.write_text(source, encoding="utf-8")
            ir, diagnostics = csm_language.compile_source(path)
        self.assertEqual(diagnostics.error_count, 0)
        first = ir["song"]["patterns"]["bass.C"]["expression"]["terms"][0]["slots"][0]["events"][0]
        self.assertEqual(first["duration_ticks"], 9)

    def test_note_shaped_qualified_name_component_is_an_identifier(self) -> None:
        source = ARTICULATION_SOURCE.replace(
            "pattern phrase = |",
            "pattern lead.b00 = |",
            1,
        ).replace(
            "section only { lead = phrase; }",
            "section only { lead = lead.b00; }",
            1,
        )
        compiler = compiler_for_text(source)
        song = compiler.build_song()
        self.assertTrue(song.orders)

    def test_explicit_duration_with_continuations_compiles(self) -> None:
        source = RUNAWAY.read_text(encoding="utf-8").replace(
            "pattern bass.C  = | c3  ~ ~ c3", "pattern bass.C  = | c3@36 _ _ c3", 1
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "continued.csm"
            path.write_text(source, encoding="utf-8")
            song, data = csmc.compile_path(path)
        self.assertEqual(song.name, "RunawayCircuit")
        self.assertTrue(data.startswith(b"ZTR1"))

    def test_silent_rest_cannot_overlap_explicit_duration(self) -> None:
        source = RUNAWAY.read_text(encoding="utf-8").replace(
            "pattern bass.C  = | c3  ~ ~ c3", "pattern bass.C  = | c3@36 ~ ~ c3", 1
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "overlap.csm"
            path.write_text(source, encoding="utf-8")
            with self.assertRaisesRegex(csmc.CsmCompileError, "silent rest overlaps"):
                csmc.compile_path(path)

    def test_integer_volume_table_compiles(self) -> None:
        macro, release_ticks, active_release = csmc.compile_volume_macro(
            "TableTest",
            {"level": 255, "envelope": {"call": "table", "args": [[255, 128, 0], 2, -1, 2]}},
        )
        self.assertEqual((macro.step, macro.loop, macro.release), (2, -1, 2))
        self.assertEqual([item.value for item in macro.values], [15, 12, 0])
        self.assertEqual(release_ticks, 2)
        self.assertTrue(active_release)

    def test_unaligned_gate_is_rejected(self) -> None:
        source = RUNAWAY.read_text(encoding="utf-8").replace("gate\t\t=\t9;", "gate\t\t=\t8;", 1)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "unaligned.csm"
            path.write_text(source, encoding="utf-8")
            with self.assertRaisesRegex(csmc.CsmCompileError, "align"):
                csmc.compile_path(path)

    def test_undeclared_pattern_is_an_error(self) -> None:
        source = RUNAWAY.read_text(encoding="utf-8").replace("bass.C*8", "bass.Missing*8", 1)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "missing.csm"
            path.write_text(source, encoding="utf-8")
            _ir, diagnostics = csm_language.compile_source(path)
        self.assertGreater(diagnostics.error_count, 0)
        self.assertTrue(any(item.code == "SEM054" for item in diagnostics.items))


if __name__ == "__main__":
    unittest.main()
