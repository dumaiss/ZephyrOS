#!/usr/bin/env python3
"""The sound-series oracles (package 97).

Four families:

* fixtures: the two ZTR songs are the Zephyr-80 sources stamped with
  the content CRC by the checked-in generator, the ZVG fixtures rebuild
  byte-for-byte from the checked-in generator;
* the oracle tools build on the host and reproduce the checked-in
  goldens byte for byte;
* the malformed-fixture outcomes table matches the oracles' current
  output (tests/snd/golden/malformed.md);
* the harness self-test: sndprobe's marker-ticked writes and their
  T-state intervals decode exactly on the production image.
"""

import hashlib
import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from snd_harness import SndReplay, split_ticks  # noqa: E402

SND = os.path.join(ROOT, "tests", "snd")
FIXTURES = os.path.join(SND, "fixtures")
MALFORMED = os.path.join(FIXTURES, "malformed")
GOLDEN = os.path.join(SND, "golden")
ORACLE = os.path.join(SND, "oracle")
Z80 = os.path.normpath(os.path.join(ROOT, "..", "Zephyr-80"))
Z80_SONGS = os.path.join(Z80, "Code", "HOST", "SNTracker", "songs")

HOST = os.path.join(ROOT, "build", "host", "snd")
ZTR_TRACE = os.path.join(HOST, "ztr_trace")
ZVG_TRACE = os.path.join(HOST, "zvg_trace")
GEN = os.path.join(FIXTURES, "gen_fixtures.py")
MAKE_MALFORMED = os.path.join(ORACLE, "make_malformed.py")

# trace name -> (fixture name, tool, extra args)
GOLDEN_FIXTURES = {
    "night-market.trace": ("night-market.ztr", ZTR_TRACE, ("3000",)),
    "RunawayCircuit.trace": ("RunawayCircuit.ztr", ZTR_TRACE, ("3000",)),
    "run-shortwaits.trace": ("run-shortwaits.zvg", ZVG_TRACE, ()),
    "wait16.trace": ("wait16.zvg", ZVG_TRACE, ()),
    "wraps.trace": ("wraps.zvg", ZVG_TRACE, ()),
}
ZTR_SONGS = ("night-market.ztr", "RunawayCircuit.ztr")
ZVG_SONGS = ("run-shortwaits.zvg", "wait16.zvg")

SNPROBE = os.path.join(ROOT, "build", "user", "sndprobe.exe")


def build_tools():
    os.makedirs(HOST, exist_ok=True)
    command = ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
               "-DZTR_ROWS_DUMP",
               "-I%s/include" % ORACLE, "-I" + ORACLE]
    subprocess.run(command +
                   [os.path.join(ORACLE, "ztr_trace.c"),
                    os.path.join(ORACLE, "ztr.c"),
                    os.path.join(ORACLE, "player.c"),
                    os.path.join(ORACLE, "psg.c"),
                    "-o", ZTR_TRACE], check=True, cwd=ROOT)
    subprocess.run(command +
                   [os.path.join(ORACLE, "zvg_trace.c"),
                    "-o", ZVG_TRACE], check=True, cwd=ROOT)


def run(tool, *args):
    return subprocess.run([tool] + list(args),
                          stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, check=True, cwd=ROOT)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        digest.update(handle.read())
    return digest.hexdigest()


class TestFixtures(unittest.TestCase):
    def test_the_ztr_songs_are_the_encoded_zephyr80_sources(self):
        """The good ZTRs are exactly encode_ztr() of the Zephyr-80
        sources (the generator itself is not run here: it would
        overwrite the committed fixtures)."""
        spec = importlib.util.spec_from_file_location("gen", GEN)
        gen = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gen)
        for name in ZTR_SONGS:
            with open(os.path.join(Z80_SONGS, name), "rb") as handle:
                expected = gen.encode_ztr(handle.read())
            with open(os.path.join(FIXTURES, name), "rb") as handle:
                actual = handle.read()
            self.assertEqual(actual, expected,
                             "%s is not its encoded Zephyr-80 source" % name)
            self.assertTrue(actual[0x16] or actual[0x17],
                            "%s carries no content CRC" % name)

    def test_the_zvg_fixtures_have_the_zvgc_shape(self):
        for name in ZVG_SONGS:
            with open(os.path.join(FIXTURES, name), "rb") as handle:
                data = handle.read()
            self.assertEqual(data[0:4], b"ZVGC")
            self.assertIn(data[4], (1, 2))
            self.assertEqual(data[7], 16)
            self.assertGreater(data[5], 0)
            self.assertLessEqual(data[5], 180)

    def test_the_generator_rebuilds_the_zvg_fixtures(self):
        """The good ZVGs are exactly what the checked-in generator
        compiles (the generator itself is not run here: it would
        overwrite the committed fixtures)."""
        spec = importlib.util.spec_from_file_location("gen", GEN)
        gen = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gen)
        with tempfile.TemporaryDirectory() as scratch:
            for name in ZVG_SONGS:
                output = os.path.join(scratch, name)
                if name == "run-shortwaits.zvg":
                    events, end, _long, _long_end = gen.good_song_events()
                else:
                    _short, _short_end, events, end = \
                        gen.good_song_events()
                gen.compile_zvg(gen.build_vgm(events, end), output)
                with open(os.path.join(FIXTURES, name), "rb") as handle:
                    self.assertEqual(handle.read(),
                                     open(output, "rb").read(),
                                     "%s is not what the generator builds"
                                     % name)
            output = os.path.join(scratch, "wraps.zvg")
            events, end = gen.wrap_song_events()
            gen.compile_zvg(gen.build_vgm(events, end), output)
            with open(os.path.join(FIXTURES, "wraps.zvg"), "rb") as handle:
                self.assertEqual(handle.read(), open(output, "rb").read(),
                                 "wraps.zvg is not what the generator "
                                 "builds")

    def test_malformed_fixtures_cover_both_formats(self):
        malformed = sorted(os.listdir(MALFORMED))
        self.assertGreaterEqual(sum(name.endswith(".ztr")
                                    for name in malformed), 25)
        self.assertGreaterEqual(sum(name.endswith(".zvg")
                                    for name in malformed), 8)
        good = [sha256(os.path.join(FIXTURES, name))
                for name in ZTR_SONGS + ZVG_SONGS]
        for name in malformed:
            self.assertNotIn(sha256(os.path.join(MALFORMED, name)), good,
                             "%s equals a good fixture" % name)


class TestOracleTools(unittest.TestCase):
    """The tools build and reproduce the goldens byte for byte."""

    @classmethod
    def setUpClass(cls):
        build_tools()

    def test_the_oracle_tools_reproduce_the_goldens(self):
        for trace_name, (fixture, tool, args) in \
                GOLDEN_FIXTURES.items():
            with open(os.path.join(GOLDEN, trace_name),
                      encoding="ascii") as handle:
                golden = handle.read()
            result = run(tool, os.path.join(FIXTURES, fixture), *args)
            self.assertEqual(result.stdout.decode("ascii"), golden,
                             trace_name)

    def test_the_malformed_outcomes_match_the_pinned_table(self):
        table = subprocess.run([sys.executable, MAKE_MALFORMED],
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, check=True,
                               cwd=ROOT, text=True).stdout
        with open(os.path.join(GOLDEN, "malformed.md"),
                  encoding="ascii") as handle:
            self.assertEqual(table, handle.read())

    def test_every_malformed_fixture_is_refused(self):
        for name in sorted(os.listdir(MALFORMED)):
            tool = ZTR_TRACE if name.endswith(".ztr") else ZVG_TRACE
            result = subprocess.run(
                [tool, os.path.join(MALFORMED, name)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                cwd=ROOT)
            self.assertNotEqual(result.returncode, 0,
                                "%s was accepted by the oracle" % name)


@unittest.skipUnless(os.path.exists(SNPROBE), "run make")
class TestHarnessSelfTest(SndReplay):
    """The harness replays sndprobe and decodes its ticks exactly."""

    EXPECTED_TICKS = [[(0xE0, 0x81), (0xE0, 0x1D), (0xE0, 0x94)],
                      [(0xE0, 0x82), (0xE0, 0x2D), (0xE0, 0x95)],
                      [(0xE0, 0x83), (0xE0, 0x3D), (0xE0, 0x96)],
                      [], []]

    def test_sndprobes_writes_and_ticks_decode_exactly(self):
        self.load_image()
        with open(SNPROBE, "rb") as handle:
            program = handle.read()
        z, since = self.start({"SNDPROBE.EXE": program},
                              b"/fs/sndprobe.exe\r")
        self.run_to_pattern(z, "sndprobe: PASS", since=since)
        writes = self.collect(z, since)
        ticks, marks, orphans, trailing = split_ticks(writes)
        self.assertEqual(ticks, self.EXPECTED_TICKS)
        self.assertEqual(len(marks), 6)
        self.assertTrue(orphans, "the acquire's reset precedes the ticks")
        from z80sim import SoundPeer
        self.assertEqual(trailing, SoundPeer().silence(),
                         "the close's release silence trails the last "
                         "marker")
        states = [tstate for _port, _value, tstate in writes]
        self.assertEqual(states, sorted(states))

    def test_the_timing_mode_measures_exactly(self):
        """The two calibrated delays differ by 13 * (64 - 32) = 416
        T-states; the interrupt service is excluded by bank."""
        self.load_image()
        with open(SNPROBE, "rb") as handle:
            program = handle.read()
        z, since = self.start({"SNDPROBE.EXE": program},
                              b"/fs/sndprobe.exe\r")
        since_glyph = len(z.glyphs)
        intervals, marks = self.profiled(
            z, until=lambda z: "sndprobe: PASS" in
            "".join(z.glyphs[since_glyph:]))
        self.assertEqual(len(marks), 6, marks)
        self.assertEqual(intervals[5] - intervals[4], 416,
                         "the two delays differ by 13*32 T-states")
        for interval in intervals:
            self.assertGreater(interval, 0)


if __name__ == "__main__":
    unittest.main()
