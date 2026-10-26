#!/usr/bin/env python3
"""Self-contained converter tests; run with ``python3 tools/test_fur2ztr.py``."""

from __future__ import annotations

import tempfile
import unittest
import struct
from pathlib import Path

import fur2ztr


PROJECT = Path(__file__).resolve().parent.parent


def small_export(effect: str = "....", note: str = "C-3") -> str:
    empty = "... .. .. ...."
    rows = [f"00 |{note} 00 0F {effect}|{empty}|{empty}|{empty}"]
    rows.extend(f"{row:02X} |{empty}|{empty}|{empty}|{empty}" for row in range(1, 4))
    return "\n".join(
        [
            "# Furnace Text Export",
            "",
            "# Song Information",
            "- name: Test",
            "- author: Test Author",
            "- system: TI-99/4A",
            "- tuning: 440",
            "",
            "# Instruments",
            "## 00: test",
            "- type: 0",
            "- macros:",
            "  - vol: 15 | 8 / 0",
            "",
            "# Sound Chips",
            "- TI SN76489",
            "  - id: 04",
            "",
            "# Subsongs",
            "## 0:",
            "- tick rate: 60",
            "- speeds: 3",
            "- pattern length: 4",
            "orders:",
            "00 | 00 00 00 00",
            "",
            "## Patterns",
            "----- ORDER 00",
            *rows,
            "",
        ]
    )


class ConverterTests(unittest.TestCase):
    def parse_text(self, text: str) -> fur2ztr.Song:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.txt"
            path.write_text(text, encoding="utf-8")
            return fur2ztr.parse_furnace(path)

    def test_night_market_round_trip(self) -> None:
        source = fur2ztr.parse_furnace(PROJECT / "songs/night-market.txt")
        data = fur2ztr.encode_ztr(source)
        decoded = fur2ztr.decode_ztr(data)
        fur2ztr.validate_round_trip(source, decoded.song)
        self.assertEqual(source.name, "Night Market")
        self.assertEqual((source.tick_rate, source.speed), (60, 3))
        self.assertEqual(source.sound_chips, ["TI SN76489"])
        self.assertEqual(source.playback_transpose, 24)
        # Furnace's reference VGM writes 81h/1Dh (divisor 465) for the
        # opening C-2. The target table must reproduce that hardware pitch.
        self.assertEqual(
            fur2ztr.build_note_table(source.tuning, source.playback_transpose)[24],
            465,
        )
        self.assertEqual(len(source.orders), 30)
        self.assertEqual(len(source.patterns), 120)
        self.assertEqual(source.warnings, [])
        for instrument in source.instruments:
            for macro in instrument.macros.values():
                self.assertTrue(macro.values)
                self.assertTrue(macro.loop < len(macro.values))
                self.assertTrue(macro.release < len(macro.values))
        effects = {
            event.effect[0]
            for events in source.patterns.values()
            for event in events
            if event.effect is not None
        }
        self.assertEqual(effects, set(fur2ztr.EFFECT_NAMES))

    def test_sparse_event_and_macro_markers(self) -> None:
        song = self.parse_text(small_export())
        self.assertEqual(len(song.patterns[(0, 0)]), 1)
        macro = song.instruments[0].macros["vol"]
        self.assertEqual((macro.loop, macro.release), (1, 2))
        data = fur2ztr.encode_ztr(song)
        self.assertLess(len(data), 1024)
        fur2ztr.validate_round_trip(song, fur2ztr.decode_ztr(data).song)

    def test_note_off_and_release_are_distinct(self) -> None:
        off = self.parse_text(small_export(note="OFF"))
        release = self.parse_text(small_export(note="==="))
        self.assertTrue(off.patterns[(0, 0)][0].note_off)
        self.assertFalse(off.patterns[(0, 0)][0].release)
        self.assertTrue(release.patterns[(0, 0)][0].release)
        self.assertFalse(release.patterns[(0, 0)][0].note_off)

    def test_notation_payload_round_trip(self) -> None:
        song = self.parse_text(small_export())
        event = song.patterns[(0, 0)][0]
        event.legato = True
        event.mode_reset = True
        event.arpeggio = 0x47
        event.hairpin = -2
        event.hairpin_ceiling = 12
        decoded = fur2ztr.decode_ztr(fur2ztr.encode_ztr(song)).song
        fur2ztr.validate_round_trip(song, decoded)
        result = decoded.patterns[(0, 0)][0]
        self.assertTrue(result.legato)
        self.assertTrue(result.mode_reset)
        self.assertEqual(result.arpeggio, 0x47)
        self.assertEqual((result.hairpin, result.hairpin_ceiling), (-2, 12))

    def test_unsupported_effect_is_reported(self) -> None:
        song = self.parse_text(small_export(effect="E312"))
        self.assertEqual(len(song.warnings), 1)
        self.assertIn("order 00 pattern 00 row 00 channel 00", song.warnings[0])
        self.assertIn("E312", song.warnings[0])

    def test_malformed_cell_fails(self) -> None:
        with self.assertRaises(fur2ztr.ConversionError):
            self.parse_text(small_export().replace("C-3 00 0F ....", "C-3 00 ...."))

    def test_undefined_instrument_fails(self) -> None:
        with self.assertRaisesRegex(fur2ztr.ConversionError, "undefined instrument 00"):
            self.parse_text(small_export().replace("## 00: test", "## 01: test"))

    def test_out_of_range_macro_release_fails_decode(self) -> None:
        song = self.parse_text(small_export())
        data = bytearray(fur2ztr.encode_ztr(song))
        instrument_offset = struct.unpack_from("<H", data, 36)[0]
        macro_offset = struct.unpack_from("<H", data, instrument_offset + 4)[0]
        count = struct.unpack_from("<H", data, macro_offset + 10)[0]
        struct.pack_into("<H", data, macro_offset + 8, count)
        with self.assertRaisesRegex(fur2ztr.ConversionError, "release is outside"):
            fur2ztr.decode_ztr(bytes(data))


if __name__ == "__main__":
    unittest.main()
