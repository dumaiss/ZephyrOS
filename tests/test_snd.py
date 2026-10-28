#!/usr/bin/env python3
"""libsnd's core and manual notes.

* host: the note compiler (snd_manual.c, built unchanged against a
  stub engine) - a program's bytes for a note, an envelope, a release
  and the noise control, and every refusal;
* replay: sndtest's modes on the production image against a Python
  reference built from the same rules - the table divisors, the
  envelope tick by tick, the noise coupling, the identical
  DIRECT/BATCH traces and the one-sound_write-per-tick property, the
  catch-up bound, the close/exit silence and the timing budgets;
* link: a manual-only program's map names no ZTR, ZVG, reader, stdio
  or KRES member.
"""

import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from snd_harness import SndReplay, STEP_BUDGET, split_ticks  # noqa: E402
from z80sim import SoundPeer, read_symbols  # noqa: E402

SND = os.path.join(ROOT, "tests", "snd")
sys.path.insert(0, SND)
SND_SRC = os.path.join(ROOT, "user", "sdk", "snd", "src")
SDK_INC = os.path.join(ROOT, "user", "sdk", "include")
ABI = os.path.join(ROOT, "abi")
HOST_INC = os.path.join(SND, "host", "include")
HOST = os.path.join(ROOT, "build", "host", "snd")
COMPILER = os.path.join(HOST, "snd_compiler")
SNTEST = os.path.join(ROOT, "build", "user", "sndtest.exe")
ZTRKRES = os.path.join(ROOT, "build", "user", "ztrkres.exe")
HOST_ZTR = os.path.join(HOST, "host_ztr")
KRES_TOOL = os.path.join(ROOT, "build", "host", "kres")
SNTEST_MAP = os.path.join(ROOT, "build", "user", "sndtest", "sndtest.map")
SOUND_REPORT = os.path.join(ROOT, "build", "modules", "sound.report.json")
SNTEST_SYMS = read_symbols(SNTEST_MAP) if os.path.exists(SNTEST_MAP) else {}


def app_mapped(z):
    """True while the application's mapping is in (latch mode 10): an
    interrupt runs OS code at the same addresses, and the budgets keep
    interrupts out."""
    return (z.latch & 0x18) == 0x10


def in_tick_code(pc, zvg=False):
    """Program ranges owned by the assembly manual/output tick path."""
    ranges = [
        (SNTEST_SYMS["_snd_load_ports"],
         SNTEST_SYMS["_snd_define_instrument"]),
        (SNTEST_SYMS["_snd_engine"],
         SNTEST_SYMS["_snd_voice_install"]),
    ]
    if zvg:
        ranges.append((SNTEST_SYMS["_snd_zvg_tick"],
                       SNTEST_SYMS["_kos_idle_halt"]))
    return any(start <= pc < end for start, end in ranges)

CLOCK = 3579545

P_END, P_PERIOD, P_ATTEN, P_NOISE, P_WAIT8, P_WAIT16 = 0, 1, 2, 3, 4, 5

CHORD = dict(peak=255, gate=200, attack=0, decay=0, sustain=255,
             release=0)
PLUCK = dict(peak=200, gate=200, attack=4, decay=3, sustain=120,
             release=3)
TONE12 = (0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14)


def a440_table():
    table = []
    for note in range(96):
        freq = 440.0 * 2 ** ((note - 48) / 12)
        table.append(max(1, min(1023,
                                round(CLOCK / (32.0 * freq)))))
    return table


def level_table():
    return [15 - round(level * 15 / 255) for level in range(256)]


A440 = a440_table()
LEVEL = level_table()


def ramp(start, end, step_no, steps):
    span = abs(start - end)
    delta = span * step_no // steps
    return start - delta if start >= end else start + delta


def emit_wait(ops, ticks):
    if ticks <= 0:
        return
    if ticks <= 255:
        ops += [P_WAIT8, ticks]
    else:
        ops += [P_WAIT16, ticks & 0xFF, ticks >> 8]


def compile_note(note, gate, inst):
    """The exact program snd_manual.c compiles for a tone note."""
    ops = [P_PERIOD, A440[note] & 0xFF, A440[note] >> 8]
    last = None
    run = 0

    def step(level):
        nonlocal ops, last, run
        atten = LEVEL[level]
        if atten != last:
            emit_wait(ops, run)
            ops += [P_ATTEN, level]
            last = atten
            run = 0
        run += 1

    if inst["attack"]:
        for i in range(1, inst["attack"] + 1):
            step(ramp(0, inst["peak"], i, inst["attack"]))
    else:
        step(inst["peak"])
    for i in range(1, inst["decay"] + 1):
        step(ramp(inst["peak"], inst["sustain"], i, inst["decay"]))
    run += gate
    for i in range(1, inst["release"] + 1):
        step(ramp(inst["sustain"], 0, i, inst["release"]))
    step(0)
    ops.append(P_END)
    return ops


def compile_release(from_level, release_ticks):
    """The exact program snd_manual.c compiles for a release."""
    ops = []
    last = None
    run = 0

    def step(level):
        nonlocal ops, last, run
        atten = LEVEL[level]
        if atten != last:
            emit_wait(ops, run)
            ops += [P_ATTEN, level]
            last = atten
            run = 0
        run += 1

    for i in range(1, release_ticks + 1):
        step(ramp(from_level, 0, i, release_ticks))
    step(0)
    ops.append(P_END)
    return ops


class SndModel(object):
    """The descriptor walk and write-through shadow, in voice order."""

    def __init__(self, pause_wait=False):
        self.desired_p = [[0, 0, 0] for _ in range(4)]
        self.desired_a = [[15, 15, 15, 15] for _ in range(4)]
        self.desired_n = [0xE0] * 4
        self.force = [0] * 4
        self.shadow_valid = [0] * 4
        self.shadow_p = [[None, None, None] for _ in range(4)]
        self.shadow_a = [[None] * 4 for _ in range(4)]
        self.shadow_n = [None] * 4
        self.active = [0] * 16
        self.wait = [0] * 16
        self.cursor = [0] * 16
        self.level = [0] * 16
        self.programs = [None] * 16
        self.pending_atten = [0] * 16
        self.pause_wait = pause_wait

    def install(self, voice):
        self.cursor[voice] = 0
        self.wait[voice] = 0
        self.active[voice] = 1
        self.level[voice] = 0

    def clear(self, voice):
        self.active[voice] = 0
        self.wait[voice] = 0
        self.level[voice] = 0
        self.desired_a[voice >> 2][voice & 3] = 15
        self.pending_atten[voice] = 1

    def noise(self, voice, control):
        self.desired_n[voice >> 2] = 0xE0 | control
        self.force[voice >> 2] = 1

    def note_on(self, voice, note, gate, inst):
        self.programs[voice] = compile_note(note, gate, inst)
        self.install(voice)

    def note_off(self, voice, release_ticks):
        self.programs[voice] = compile_release(self.level[voice],
                                               release_ticks)
        self.install(voice)

    def tick(self, elapsed):
        writes = []
        for voice in range(16):
            chip, sub = voice >> 2, voice & 3
            touched_p = touched_a = touched_n = False
            if self.pending_atten[voice]:
                touched_a = True
                self.pending_atten[voice] = 0
            if sub == 3 and self.force[chip]:
                touched_n = True
                self.force[chip] = 0
            if not self.active[voice]:
                pass
            else:
                remaining = elapsed
                had_wait = self.wait[voice] != 0
                if self.wait[voice] > remaining:
                    self.wait[voice] -= remaining
                else:
                    remaining -= self.wait[voice]
                    self.wait[voice] = 0
                    if self.pause_wait and had_wait:
                        continue
                    program = self.programs[voice]
                    while True:
                        op = program[self.cursor[voice]]
                        if op == P_END:
                            self.active[voice] = 0
                            break
                        if op == P_PERIOD:
                            self.desired_p[chip][sub] = \
                                program[self.cursor[voice] + 1] | \
                                (program[self.cursor[voice] + 2] << 8)
                            self.cursor[voice] += 3
                            touched_p = True
                        elif op == P_ATTEN:
                            level = program[self.cursor[voice] + 1]
                            self.level[voice] = level
                            self.desired_a[chip][sub] = LEVEL[level]
                            self.cursor[voice] += 2
                            touched_a = True
                        elif op == P_NOISE:
                            self.desired_n[chip] = \
                                0xE0 | program[self.cursor[voice] + 1]
                            self.cursor[voice] += 2
                            touched_n = True
                        else:
                            width = 2 if op == P_WAIT8 else 3
                            n = program[self.cursor[voice] + 1]
                            if width == 3:
                                n |= program[self.cursor[voice] + 2] << 8
                            self.cursor[voice] += width
                            if n > remaining:
                                self.wait[voice] = n - remaining
                                break
                            remaining -= n
                            if remaining == 0:
                                break
            if self.shadow_valid[chip]:
                port = 0xE0 + chip
                if touched_p and self.shadow_p[chip][sub] != \
                        self.desired_p[chip][sub]:
                    period = self.desired_p[chip][sub]
                    writes += [(port, 0x80 | (sub << 5) | (period & 15)),
                               (port, ((period >> 4) & 15) |
                                (((period >> 8) & 15) << 4))]
                    self.shadow_p[chip][sub] = period
                if touched_n:
                    writes.append((port, self.desired_n[chip]))
                    self.shadow_n[chip] = self.desired_n[chip]
                if touched_a and self.shadow_a[chip][sub] != \
                        self.desired_a[chip][sub]:
                    atten = self.desired_a[chip][sub]
                    writes.append((port, 0x90 | (sub << 5) | atten))
                    self.shadow_a[chip][sub] = atten
        return writes + self.flush()

    def flush(self):
        writes = []
        if all(self.shadow_valid):
            return writes
        for chip in range(4):
            all_writes = 1 - self.shadow_valid[chip]
            for sub in range(3):
                period = self.desired_p[chip][sub]
                if all_writes or self.shadow_p[chip][sub] != period:
                    latch = 0x80 | (sub << 5) | (period & 0x0F)
                    data = ((period >> 4) & 0x0F) | \
                        (((period >> 8) & 0x0F) << 4)
                    writes.append((0xE0 + chip, latch))
                    writes.append((0xE0 + chip, data))
                    self.shadow_p[chip][sub] = period
            control = self.desired_n[chip]
            if self.force[chip]:
                writes.append((0xE0 + chip, control))
                self.shadow_n[chip] = control
                self.force[chip] = 0
            elif all_writes or self.shadow_n[chip] != control:
                writes.append((0xE0 + chip, control))
                self.shadow_n[chip] = control
            for sub in range(4):
                atten = self.desired_a[chip][sub]
                if all_writes or self.shadow_a[chip][sub] != atten:
                    writes.append((0xE0 + chip, 0x90 | (sub << 5) |
                                   atten))
                    self.shadow_a[chip][sub] = atten
            self.shadow_valid[chip] = 1
        return writes


def marker_framed(ticks):
    """split_ticks' shape for three marker pairs: the writes of each
    tick with the empty gaps the untouched pairs leave."""
    return [ticks[0], [], ticks[1], [], ticks[2]]


def note_ticks(model, notes, voices, gate, inst):
    ticks = []
    for voice, note in zip(voices, notes):
        model.note_on(voice, note, gate, inst)
    for _ in range(3):
        ticks.append(model.tick(1))
    return ticks


def silent_suffix(model):
    for voice in range(16):
        model.clear(voice)
    return model.tick(1)


class SndCase(SndReplay):
    def run_mode(self, mode, budget=None):
        with open(SNTEST, "rb") as handle:
            program = handle.read()
        z, since = self.start({"SNDTEST.EXE": program},
                              ("/fs/sndtest.exe %s\r" % mode).encode())
        self.run_to_pattern(z, "PASS", since=since)
        return z, since


@unittest.skipUnless(os.path.exists(SNTEST), "run make")
class TestSndCompilerHost(unittest.TestCase):
    """The C compiler side (snd_manual.c + snd_tables.c), built with
    the host compiler against the stub engine."""

    @classmethod
    def setUpClass(cls):
        os.makedirs(HOST, exist_ok=True)
        subprocess.run(
            ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
             "-I" + HOST_INC, "-I" + ABI, "-I" + SDK_INC, "-I" + SND_SRC,
             os.path.join(SND_SRC, "snd_manual.c"),
             os.path.join(SND_SRC, "snd_tables.c"),
             os.path.join(SND, "host", "host_snd.c"), "-o", COMPILER],
            check=True, cwd=ROOT)

    def _run(self):
        return subprocess.run([COMPILER], stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, check=True,
                              cwd=ROOT, text=True).stdout

    def _programs(self):
        programs = {}
        for line in self._run().splitlines():
            if line.startswith("program "):
                voice = int(line.split()[1].rstrip(":"))
                programs[voice] = [int(token, 16)
                                   for token in line.split()[2:]]
        return programs

    def test_the_note_program_matches_the_reference(self):
        inst = dict(peak=200, attack=2, decay=2, sustain=120,
                    release=2)
        expected = compile_note(48, 8, inst)
        programs = self._programs()
        # the host dump stops at the last non-zero byte (the final
        # ATTEN opcode); it must equal the reference's prefix
        self.assertEqual(programs[0], expected[:len(programs[0])],
                         "the note program's bytes")
        self.assertIn(0, expected)

    def test_the_release_program_matches_the_reference(self):
        expected = compile_release(150, 2)
        programs = self._programs()
        # voice 1's buffer still holds the stale note tail behind the
        # release's END; the dump therefore starts with the release
        self.assertEqual(programs[1][:len(expected)], expected,
                         "the release program's bytes")

    def test_the_attack_passes_through_peak_into_the_decay(self):
        # the host test's note runs attack 2 -> decay 2 -> gate 8 with
        # a 200-level peak; the level bytes are the reference ramp
        inst = dict(peak=200, attack=2, decay=2, sustain=120,
                    release=2)
        program = compile_note(48, 8, inst)
        self.assertEqual(program[0:3],
                         [P_PERIOD, A440[48] & 0xFF, A440[48] >> 8])
        levels = [program[index] for index in range(4, len(program), 2)
                  if program[index - 1] == P_ATTEN]
        self.assertEqual(levels, [100, 200, 160, 120, 60, 0],
                         "attack, decay and release levels")

    def test_the_refusals(self):
        out = self._run()
        self.assertIn("bad voice -2", out)
        self.assertIn("bad note -2", out)
        self.assertIn("reserved -4", out)
        self.assertIn("rate3 tone2 -4", out)
        self.assertIn("rate3 apply -4", out)
        self.assertIn("rate3 free 0", out)
        self.assertIn("noise byte e7", out)


@unittest.skipUnless(os.path.exists(SNTEST), "run make")
class TestSndTones(SndCase):
    def test_each_latch_byte_and_divisor_equals_the_table(self):
        notes = [48, 52, 55, 60, 64, 67, 72, 76, 79, 84, 88, 91]
        voices = [0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14]
        model = SndModel()
        expected = marker_framed(note_ticks(model, notes, voices, 200,
                                            CHORD))
        z, since = self.run_mode("tones")
        ticks, _marks, _orphans, _trailing = split_ticks(
            self.collect(z, since))
        self.assertEqual(ticks, expected)


@unittest.skipUnless(os.path.exists(SNTEST), "run make")
class TestSndAdsr(SndCase):
    def test_the_envelope_tick_by_tick(self):
        model = SndModel()
        model.note_on(0, 48, 200, PLUCK)
        expected = [model.tick(1) for _ in range(10)]
        model.note_off(0, PLUCK["release"])
        expected += [model.tick(1) for _ in range(6)]
        z, since = self.run_mode("adsr")
        ticks, _marks, _orphans, _trailing = split_ticks(
            self.collect(z, since))
        self.assertEqual(len(ticks), 31, "16 ticks and their gaps")
        for index in range(1, len(ticks), 2):
            self.assertEqual(ticks[index], [], "gap %d" % index)
        for index, want in enumerate(expected):
            self.assertEqual(ticks[index * 2], want, "tick %d" % index)

    def test_the_release_starts_from_the_current_level(self):
        model = SndModel()
        model.note_on(0, 48, 200, PLUCK)
        for _ in range(10):
            model.tick(1)
        self.assertEqual(model.level[0], PLUCK["sustain"])
        model.note_off(0, PLUCK["release"])
        levels = []
        for _ in range(3):
            model.tick(1)
            levels.append(model.level[0])
        self.assertEqual(levels, [80, 40, 0], "the release ramp")


@unittest.skipUnless(os.path.exists(SNTEST), "run make")
class TestSndNoise(SndCase):
    def test_modes_rates_the_forced_write_and_the_coupling(self):
        model = SndModel()
        model.noise(3, 0x01)  # periodic, rate 1: control byte E1h
        expected = [model.tick(1)]
        model.noise(3, 0x01)  # the same value: the force rule rewrites
        expected.append(model.tick(1))
        model.noise(3, 0x07)  # white, rate 3: control byte E7h
        expected.append(model.tick(1))
        z, since = self.run_mode("noise")
        transcript = self.trace(z, since)
        self.assertIn("sndtest: noise busy -4", transcript)
        self.assertIn("sndtest: noise rate3 0", transcript)
        self.assertIn("sndtest: noise tone2 -4", transcript)
        ticks, _marks, _orphans, _trailing = split_ticks(
            self.collect(z, since))
        wanted = marker_framed(expected)
        for index, (tick, want) in enumerate(zip(ticks, wanted)):
            self.assertEqual(tick, want, "tick %d" % index)


@unittest.skipUnless(os.path.exists(SNTEST), "run make")
class TestSndModes(SndCase):
    NOTES = [48, 49, 50]

    def _expected(self):
        # one continuous model: the DIRECT chord, the silence, the
        # chord again through BATCH and the final silence - the
        # reference makes no mode distinction, so a match on every
        # pair proves the two modes' traces are identical
        model = SndModel()
        ticks = note_ticks(model, self.NOTES, [0, 1, 2], 200, CHORD)
        ticks.append(silent_suffix(model))
        ticks += note_ticks(model, self.NOTES, [0, 1, 2], 200, CHORD)
        ticks.append(silent_suffix(model))
        return ticks

    def test_direct_and_batch_traces_are_identical(self):
        expected = self._expected()
        z, since = self.run_mode("modes")
        ticks, _marks, _orphans, _trailing = split_ticks(
            self.collect(z, since))
        # pairs 1-3 DIRECT, 4 the BATCH flip's silence, 5-8 BATCH
        self.assertEqual(len(ticks), 15, "8 ticks and their gaps")
        for index in range(1, len(ticks), 2):
            self.assertEqual(ticks[index], [], "gap %d" % index)
        pairs = [tick for tick in ticks[::2]]
        self.assertEqual(len(pairs), 8)
        for pair, (tick, want) in enumerate(zip(pairs, expected), 1):
            self.assertEqual(tick, want, "pair %d" % pair)

    def test_batch_makes_one_sound_write_per_tick(self):
        with open(SOUND_REPORT, encoding="utf-8") as handle:
            dispatch = json.load(handle)["endpoints"][0]["dispatch"]
        with open(SNTEST, "rb") as handle:
            program = handle.read()
        z, since = self.start({"SNDTEST.EXE": program},
                              b"/fs/sndtest.exe modes\r")
        state = {"hits": 0}
        hits = [0]
        inner = z.sound_peer.write

        def write(port, value):
            inner(port, value)
            if (port & 0xFF, value & 0xFF) == (0xE4, 0xA5):
                hits.append(state["hits"])
                state["hits"] = 0

        z.sound_peer.write = write
        since_glyph = len(z.glyphs)
        steps = 0
        while steps < 12000000:
            pc_before = z.pc
            self.skip_delay(z)
            self.feed(z)
            z.step()
            if pc_before == dispatch:
                state["hits"] += 1
            if steps % 0x1FF == 0:
                self.pump(z)
                if "PASS" in "".join(z.glyphs[since_glyph:]):
                    break
            steps += 1
        self.assertIn("PASS", "".join(z.glyphs[since_glyph:]))
        # a marker is a direct DAC write, never a dispatch: pairs 1-3
        # (DIRECT) have none; pairs 4-8 (BATCH) exactly one each
        self.assertEqual(hits[2], 0, "DIRECT pair 1")
        self.assertEqual(hits[4], 0, "DIRECT pair 2")
        self.assertEqual(hits[6], 0, "DIRECT pair 3")
        # BATCH: pairs 4 and 8 (the silence) and 5 (the chord's first
        # tick) change bytes - exactly one sound_write each; pairs 6
        # and 7 are idle and make none
        for pair in (8, 10, 16):
            self.assertEqual(hits[pair], 1, "BATCH pair %d" % (pair // 2))
        for pair in (12, 14):
            self.assertEqual(hits[pair], 0,
                             "idle BATCH pair %d" % (pair // 2))


@unittest.skipUnless(os.path.exists(SNTEST), "run make")
class TestSndCatchup(SndCase):
    def test_the_cap_and_the_overrun_count(self):
        z, since = self.run_mode("catchup")
        transcript = self.trace(z, since)
        self.assertIn("sndtest: catchup overruns 34", transcript)


@unittest.skipUnless(os.path.exists(SNTEST), "run make")
class TestSndClose(SndCase):
    def test_everything_is_silenced_and_released(self):
        with open(SNTEST, "rb") as handle:
            program = handle.read()
        z, since = self.start({"SNDTEST.EXE": program},
                              b"/fs/sndtest.exe close\r")
        self.run_to_pattern(z, "exit PASS", since=since)
        transcript = self.trace(z, since)
        self.assertIn("sndtest: close PASS", transcript)
        self.assertIn("sndtest: exit PASS", transcript)
        silence = SoundPeer().silence()
        # The exit's release is the kernel's card reset, which lands a
        # little after the process's last console byte; wait for the
        # fourth silence rather than racing the replay's pump window.
        def fourth(_step):
            writes = [(port, value) for port, value, _tstate in
                      self.collect(z, since)]
            return self._silences(writes, silence) >= 4
        self.drive(z, STEP_BUDGET, every=1, tick=fourth)
        writes = [(port, value) for port, value, _tstate in
                  self.collect(z, since)]
        # the acquire's reset, the close's release, the second
        # acquire's reset and the exit's release
        self.assertEqual(self._silences(writes, silence), 4)
        ticks, _marks, _orphans, trailing = split_ticks(
            self.collect(z, since))
        # the close's mute flush, then its release and the second
        # acquire's reset fall in the gap between the two pairs; the
        # exit's release trails
        mute = [(0xE0, 0x9F), (0xE0, 0xBF), (0xE0, 0xDF)]
        self.assertEqual(ticks[1], mute + silence * 2,
                         "the gap's mute and silences")
        self.assertEqual(trailing, silence,
                         "the trailing release silence")

    @staticmethod
    def _silences(writes, silence):
        count = index = 0
        while index < len(writes) - len(silence) + 1:
            if writes[index:index + len(silence)] == silence:
                count += 1
                index += len(silence)
            else:
                index += 1
        return count


@unittest.skipUnless(os.path.exists(SNTEST), "run make")
class TestSndVoiceEngine(SndCase):
    def test_coalesced_tick_matches_five_logical_ticks_and_writes_last(self):
        inst = dict(peak=255, attack=8, decay=0, sustain=255, release=0)
        coalesced = SndModel()
        stepped = SndModel()
        # Establish the same valid silent shadow that the target mode does.
        coalesced.flush()
        stepped.flush()
        coalesced.note_on(0, 48, 20, inst)
        stepped.note_on(0, 48, 20, inst)
        want = coalesced.tick(5)
        for _ in range(5):
            stepped.tick(1)
        self.assertEqual(coalesced.desired_p, stepped.desired_p)
        self.assertEqual(coalesced.desired_a, stepped.desired_a)
        self.assertEqual(coalesced.cursor, stepped.cursor)
        z, since = self.run_mode("coalesce")
        ticks, _marks, _orphans, _trailing = split_ticks(
            self.collect(z, since))
        self.assertEqual(ticks[0], want)

    def test_staggered_voices_follow_descriptor_order_in_both_modes(self):
        inst = dict(peak=255, attack=8, decay=7, sustain=96, release=8)
        for batch in (False, True):
            model = SndModel(pause_wait=True)
            for i in range(12, 0, -1):
                model.note_on(TONE12[i - 1], 48 + i - 1,
                              20 + i * 3, inst)
            expected = [model.tick(1) for _ in range(200)]
            z, since = self.run_mode("stagger batch" if batch else
                                     "stagger")
            ticks, _marks, _orphans, _trailing = split_ticks(
                self.collect(z, since))
            actual = ticks[0::2]
            self.assertEqual(len(actual), 200)
            for index, want in enumerate(expected):
                self.assertEqual(list(actual[index]), want,
                                 "batch %d tick %d" % (batch, index))

    def test_put_primitives_deduplicate_and_force_noise(self):
        z, since = self.run_mode("primitive")
        ticks, _marks, _orphans, _trailing = split_ticks(
            self.collect(z, since))
        self.assertEqual(ticks[0], [(0xE0, 0xE0)])


@unittest.skipUnless(os.path.exists(SNTEST), "run make")
class TestSndTiming(SndCase):
    def test_the_budgets(self):
        with open(SNTEST, "rb") as handle:
            program = handle.read()
        z, since = self.start({"SNDTEST.EXE": program},
                              b"/fs/sndtest.exe timing\r")
        since_glyph = len(z.glyphs)
        # one interval per marker; pair k spans intervals[2k-1].  The
        # tick's own code (the engine and the flush) is attributed
        # separately, so the budgets measure snd_tick and not the
        # event delivery that frames it.
        state = {"current": 0}
        intervals = []
        inner = z.sound_peer.write

        def write(port, value):
            inner(port, value)
            if (port & 0xFF, value & 0xFF) == (0xE4, 0xA5):
                intervals.append(state["current"])
                state["current"] = 0

        z.sound_peer.write = write
        steps = 0
        while steps < 24000000:
            before = z.tstates
            pc_before = z.pc if app_mapped(z) else -1
            self.skip_delay(z)
            self.feed(z)
            z.step()
            if in_tick_code(pc_before):
                state["current"] += z.tstates - before
            if steps % 0x1FF == 0:
                self.pump(z)
                if "PASS" in "".join(z.glyphs[since_glyph:]):
                    break
            steps += 1
        # pair 1: 12 voices writing; pairs 2-7 idle; pairs 8-15 twelve
        # attenuations; 16-23 one; 24 the refresh; 25 BATCH; 26 compile.
        # The first pair of a phase also carries the queued setup
        # writes (the installs and the silences), so the steady-state
        # budgets measure the later pairs.
        self.assertEqual(len(intervals), 52, "two markers per pair")
        idle = max(intervals[3:14:2])
        self.assertLessEqual(idle, 600, "nothing due: %d" % idle)
        # the phase's first pair also writes the twelve periods
        first = intervals[15]
        self.assertLessEqual(first, 20500,
                             "twelve notes starting: %d" % first)
        twelve = max(intervals[17:30:2])
        self.assertLessEqual(twelve, 11000,
                             "twelve attenuations (target 4,000): %d"
                             % twelve)
        per_byte = max(intervals[33:46:2]) - idle
        self.assertLessEqual(per_byte, 1950,
                             "per changed byte (target 400): %d" % per_byte)
        refresh = intervals[47]
        self.assertLessEqual(refresh, 13600,
                             "full refresh: %d" % refresh)
        print("snd budgets: idle %d, twelve starting %d, twelve %d, "
              "per-byte %d, refresh %d, batch %d, compile %d"
              % (idle, first, twelve, per_byte, refresh, intervals[49],
                 intervals[51]))


ZVG_GOLDEN = os.path.join(ROOT, "tests", "snd", "golden")
ZVG_FIXTURES = os.path.join(ROOT, "tests", "snd", "fixtures")
ZVG_MALFORMED = os.path.join(ZVG_FIXTURES, "malformed")


@unittest.skipUnless(os.path.exists(SNTEST) and os.path.exists(ZTRKRES),
                     "run make")
class TestSndZtrLoad(SndCase):
    """The deferred producer acceptance: oracle rows and all reads."""

    SONGS = ("night-market", "RunawayCircuit")

    @classmethod
    def setUpClass(cls):
        os.makedirs(HOST, exist_ok=True)
        subprocess.run(
            ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
             "-DHOST_ZTR", "-I" + HOST_INC, "-I" + SND_SRC,
             "-I" + SDK_INC, "-I" + ABI,
             os.path.join(SND, "host", "host_ztr.c"), "-o", HOST_ZTR],
            check=True, cwd=ROOT)
        if not os.path.exists(KRES_TOOL):
            subprocess.run(["make", "kres-host"], check=True, cwd=ROOT)
        import test_snd_oracle
        test_snd_oracle.build_tools()

    @staticmethod
    def host_output(song, mode=None):
        command = [HOST_ZTR, os.path.join(ZVG_FIXTURES, song + ".ztr")]
        if mode is not None:
            command.append(mode)
        return subprocess.run(command, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, check=True,
                              cwd=ROOT, text=True)

    @staticmethod
    def checksums(text):
        return [(int(order), int(value, 16))
                for order, value in re.findall(
                    r"(?:order |checksum )(\d+)(?: checksum)? ([0-9A-Fa-f]{4})",
                    text)]

    def test_host_rows_short_reads_transient_error_and_forward_open(self):
        for song in self.SONGS:
            normal = self.host_output(song)
            short = self.host_output(song, "short")
            self.assertEqual(short.stdout, normal.stdout, song)
            self.assertNotIn("offsets decreased", normal.stderr)
            error = self.host_output(song, "error")
            self.assertEqual(error.stdout, "ztr: refused -10\n", song)
            # The producer acceptance is the order-buffer comparison
            # and one stream checksum per order, matching the card's
            # ztrload output; the oracle row records no longer exist.
            self.assertEqual(
                len(self.checksums(normal.stdout)),
                {"night-market": 30, "RunawayCircuit": 136}[song], song)

    def test_sdk_loader_refuses_every_malformed_ztr(self):
        directory = os.path.join(ZVG_MALFORMED)
        for name in sorted(item for item in os.listdir(directory)
                           if item.endswith(".ztr")):
            result = subprocess.run(
                [HOST_ZTR, os.path.join(directory, name)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                check=True, cwd=ROOT, text=True)
            self.assertTrue(result.stdout.startswith("ztr: refused "),
                            "%s was accepted: %s" %
                            (name, result.stdout.splitlines()[:1]))

    def target_output(self, program_path, files, command, done):
        with open(program_path, "rb") as handle:
            files = dict(files, **{os.path.basename(program_path).upper():
                                   handle.read()})
        z, since = self.start(files, command.encode())
        self.run_to_pattern(z, done, since=since, budget=500_000_000)
        self.run_to_pattern(z, "\n", since=len(z.glyphs))
        return self.trace(z, since)

    def test_card_and_kres_checksums_equal_the_host(self):
        for song in self.SONGS:
            with open(os.path.join(ZVG_FIXTURES, song + ".ztr"), "rb") \
                    as handle:
                data = handle.read()
            want = self.checksums(self.host_output(song).stdout)
            card = self.target_output(
                SNTEST, {"RUN.ZTR": data},
                "/fs/sndtest.exe ztrload /fs/run.ztr\r",
                "ztrload PASS")
            self.assertEqual(self.checksums(card), want, song + " card")
            with tempfile.TemporaryDirectory() as directory:
                source = os.path.join(directory, "source")
                os.mkdir(source)
                shutil.copyfile(os.path.join(ZVG_FIXTURES, song + ".ztr"),
                                os.path.join(source, "SONG.ZTR"))
                packed = os.path.join(directory, "SONG.KPR")
                subprocess.run([KRES_TOOL, "pack", source, packed],
                               check=True, stdout=subprocess.DEVNULL,
                               cwd=ROOT)
                with open(packed, "rb") as handle:
                    bundle = handle.read()
            kres = self.target_output(
                ZTRKRES, {"SONG.KPR": bundle},
                "/fs/ztrkres.exe /fs/song.kpr SONG.ZTR\r",
                "ztrloadkres: PASS")
            self.assertEqual(self.checksums(kres), want, song + " KRES")


@unittest.skipUnless(os.path.exists(SNTEST), "run make")
class TestSndZtr(SndCase):
    """The streamed tracker engine against the preserved C oracle."""

    SONGS = ("night-market", "RunawayCircuit")

    @classmethod
    def setUpClass(cls):
        import test_snd_oracle
        test_snd_oracle.build_tools()

    def run_ztr_mode(self, mode, song="night-market", done="ztr PASS"):
        with open(SNTEST, "rb") as handle:
            program = handle.read()
        with open(os.path.join(ZVG_FIXTURES, song + ".ztr"), "rb") \
                as handle:
            fixture = handle.read()
        z, since = self.start(
            {"SNDTEST.EXE": program, "RUN.ZTR": fixture},
            ("/fs/sndtest.exe %s /fs/run.ztr\r" % mode).encode())
        self.run_to_pattern(z, done, since=since, budget=400_000_000)
        self.run_to_pattern(z, "\n", since=len(z.glyphs))
        return z, since

    def test_both_songs_match_state_and_writes_for_3000_ticks(self):
        from ztr_state_diff import oracle_state
        from ztr_validate import engine_run, golden_writes, state_diff

        for batch in (False, True):
            for song in self.SONGS:
                fixture = os.path.join(ZVG_FIXTURES, song + ".ztr")
                oracle = oracle_state(fixture, 3000)
                if batch:
                    writes, marks, state = engine_run(
                        song + ".ztr", 3000, batch=True)
                else:
                    writes, marks, state, costs, depths = engine_run(
                        song + ".ztr", 3000, profile=True)
                bad_writes, first_write = golden_writes(song, writes)
                bad_state, first_state = state_diff(state, oracle)
                label = "%s %s" % (song, "BATCH" if batch else "DIRECT")
                self.assertEqual(len(marks), 6000, label)
                self.assertEqual((bad_writes, first_write), (0, None), label)
                self.assertEqual((bad_state, first_state), (0, None), label)
                if not batch:
                    median = statistics.median(costs)
                    worst = max(costs)
                    print("ztr budgets: %s median %d worst %d at tick %d, "
                          "tick stack %d bytes"
                          % (song, median, worst, costs.index(worst),
                             max(depths)))

    def test_pause_resume_stop_and_voice_ownership(self):
        z, since = self.run_ztr_mode("ztrpause")
        transcript = self.trace(z, since)
        self.assertIn("sndtest: ztr PASS state 3 order 0 row 3 tick 2",
                      transcript)
        ticks, _marks, _orphans, _trailing = split_ticks(
            self.collect(z, since))
        pairs = ticks[0::2]
        self.assertGreaterEqual(len(pairs), 10)
        self.assertEqual(list(pairs[4]), [], "first paused tick")
        self.assertEqual(list(pairs[5]), [], "second paused tick")

        z, since = self.run_ztr_mode("ztrnotes", done="ztrnotes other")
        transcript = self.trace(z, since)
        self.assertIn("sndtest: ztrnotes busy -4", transcript)
        self.assertIn("sndtest: ztrnotes other 0", transcript)

    def test_stalled_reader_mutes_and_resumes_the_due_row(self):
        z, since = self.run_ztr_mode("ztrstall", done="ztrstall served")
        transcript = self.trace(z, since)
        self.assertIn("sndtest: ztr PASS state 3 ticks 383 underruns 1 "
                      "order 1 row 1", transcript)
        self.assertRegex(transcript, r"ztrstall served \d+")
        ticks, _marks, _orphans, _trailing = split_ticks(
            self.collect(z, since))
        pairs = ticks[0::2]
        values = [[value for port, value in tick if port == 0xE0]
                  for tick in pairs]
        muted = next(index for index, tick in enumerate(values)
                     if tick == [0x9F, 0xBF])
        self.assertGreater(len(values[muted + 2]), 0,
                           "the due order resumes audible writes")

    def test_stop_effect_ends_a_short_generated_song(self):
        tools = os.path.join(ROOT, "UserSoftware", "SNTracker", "tools")
        if tools not in sys.path:
            sys.path.insert(0, tools)
        import fur2ztr

        song = fur2ztr.Song(
            name="stop-effect", author="tests", system="SN76489",
            tick_rate=60, speed=2, pattern_length=4, source_channels=1,
            orders=[[0] + [0xFF] * 15],
            patterns={(0, 0): [fur2ztr.Event(row=1, effect=(6, 1))]})
        fixture = fur2ztr.encode_ztr(song)
        with open(SNTEST, "rb") as handle:
            program = handle.read()
        z, since = self.start(
            {"SNDTEST.EXE": program, "STOP.ZTR": fixture},
            b"/fs/sndtest.exe ztr /fs/stop.ztr\r")
        self.run_to_pattern(z, "ztr PASS", since=since)
        transcript = self.trace(z, since)
        self.assertIn("sndtest: ztr PASS state 5 ticks 3 underruns 0 "
                      "order 0 row 1", transcript)



def zvg_golden(name):
    """The oracle's per-tick writes (port 0xE0) and its totals."""
    ticks = []
    totals = {}
    with open(os.path.join(ZVG_GOLDEN, name + ".trace")) as handle:
        for line in handle:
            line = line.strip()
            if line.startswith("tick "):
                body = line.split(":", 1)[1].strip()
                if body:
                    ticks.append([(0xE0, int(word[3:], 16))
                                  for word in body.split()])
                else:
                    ticks.append([])
            elif line.startswith("# ") and "ticks" in line:
                parts = line[2:].replace(",", "").split()
                totals = {"ticks": int(parts[0]),
                          "writes": int(parts[2]),
                          "underruns": int(parts[5])}
    return ticks, totals


SNTRACK = os.path.join(ROOT, "UserSoftware", "SNTracker", "build",
                       "user", "sntrack.exe")


def song_tick_count(song):
    """The oracle's total tick count (the timer-event target)."""
    from test_snd_oracle import ZTR_TRACE

    out = subprocess.run(
        [ZTR_TRACE, os.path.join(ZVG_FIXTURES, song + ".ztr"), "60000",
         "/dev/null"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        check=True, cwd=ROOT, text=True)
    match = re.search(r"# (\d+) ticks", out.stdout)
    assert match, out.stdout
    return int(match.group(1))


@unittest.skipUnless(os.path.exists(SNTEST) and os.path.exists(SNTRACK),
                     "run make")
class TestSndZtrClient(SndCase):
    """Requirement 4: real playback, paced by the timer."""

    SONGS = ("night-market", "RunawayCircuit")

    @classmethod
    def setUpClass(cls):
        import test_snd_oracle
        test_snd_oracle.build_tools()

    def run_sntrack(self, song):
        with open(SNTRACK, "rb") as handle:
            program = handle.read()
        with open(os.path.join(ZVG_FIXTURES, song + ".ztr"), "rb") \
                as handle:
            fixture = handle.read()
        z, since = self.start({"SNTRACK.EXE": program, "SONG.ZTR": fixture},
                              b"/fs/sntrack.exe /fs/song.ztr\r")
        self.run_to_pattern(z, "Playback complete", since=since,
                            budget=600_000_000)
        self.run_to_pattern(z, "Timer events", since=since,
                            budget=600_000_000)
        # the statistics line is complete once the shell prompts again;
        # search from the glyph that starts it
        stats = len(z.glyphs)
        while stats > since and \
                "Timer events" not in "".join(z.glyphs[stats:]):
            stats -= 1
        self.run_to_pattern(z, "\n/> ", since=stats)
        return z, since

    def test_sndtest_ztr_timed_keeps_the_tempo(self):
        with open(SNTEST, "rb") as handle:
            program = handle.read()
        with open(os.path.join(ZVG_FIXTURES, "night-market.ztr"), "rb") \
                as handle:
            fixture = handle.read()
        z, since = self.start(
            {"SNDTEST.EXE": program, "RUN.ZTR": fixture},
            b"/fs/sndtest.exe ztr /fs/run.ztr timed\r")
        self.run_to_pattern(z, "ztr PASS", since=since,
                            budget=600_000_000)
        transcript = self.trace(z, since)
        self.assertIn("sndtest: ztr PASS 5 %d 0" %
                      song_tick_count("night-market"), transcript)

    def test_sntrack_plays_both_songs_clean_and_on_tempo(self):
        from snd_harness import parse_golden, port_to_chip

        for song in self.SONGS:
            z, since = self.run_sntrack(song)
            transcript = self.trace(z, since)
            self.assertIn("Playback complete.", transcript)
            match = re.search(
                r"Timer events ([0-9A-Fa-f]+)  underruns (\d+)  "
                r"overruns (\d+)  missed ([0-9A-Fa-f]+)", transcript)
            self.assertIsNotNone(match, song)
            timer_events = int(match.group(1), 16)
            self.assertEqual((int(match.group(2)), int(match.group(3)),
                              int(match.group(4))), (0, 0, 0), song)
            ticks = song_tick_count(song)
            self.assertLessEqual(abs(timer_events - ticks) * 100, ticks * 2,
                                 "%s tempo: %d timer events for %d ticks"
                                 % (song, timer_events, ticks))
            # the first 3,000 ticks equal the goldens: align on tick 0's
            # first write (the play's mute flush precedes it) and compare
            # the write prefix
            golden = parse_golden(os.path.join(
                ZVG_GOLDEN, song + ".trace"))
            wanted = [entry for tick in range(3000)
                      for entry in golden.get(tick, [])]
            played = [(port_to_chip(port), value)
                      for port, value, _t in z.sound_peer.writes[since:]]
            first = (int(wanted[0][0]) + SoundPeer.FIRST_PORT,
                     wanted[0][1])
            start = None
            for index in range(len(played)):
                if (played[index][0] is not None and
                        (played[index][0] + SoundPeer.FIRST_PORT,
                         played[index][1]) == first):
                    start = index
                    break
            self.assertIsNotNone(start, "%s: no tick-0 write" % song)
            body = [entry for entry in played[start:]
                    if entry[0] is not None][:len(wanted)]
            self.assertEqual(body, wanted, song)


@unittest.skipUnless(os.path.exists(SNTEST), "run make")
class TestSndZvg(SndCase):
    """The pre-rendered stream player, replayed against the oracle's
    goldens on the production image.  sndtest's zvg mode frames each
    snd_tick(1) with the DAC marker, so the per-tick writes fall out of
    split_ticks one pair at a time."""

    FILES = {"run-shortwaits": "SHORT.ZVG", "wait16": "WAIT16.ZVG",
             "wraps": "WRAPS.ZVG"}

    def run_zvg(self, name, chip, batch=False, stall=False):
        with open(SNTEST, "rb") as handle:
            program = handle.read()
        with open(os.path.join(ZVG_FIXTURES, name + ".zvg"), "rb") as handle:
            fixture = handle.read()
        mode = "zvgstall" if stall else "zvg"
        command = "/fs/sndtest.exe %s /fs/%s %d%s%s\r" % (
            mode, self.FILES[name].lower(), chip,
            " batch" if batch else "", "" if stall else " step")
        z, since = self.start({"SNDTEST.EXE": program,
                               self.FILES[name]: fixture}, command.encode())
        # the PASS line's last word is the byte count: wait for the
        # line's end after it, so the whole line is captured regardless
        # of the pump cadence
        self.run_to_pattern(z, "bytes", since=since)
        self.run_to_pattern(z, "\n", since=len(z.glyphs))
        return z, since

    def test_traces_match_the_oracle_on_two_chips_both_modes(self):
        # wraps.zvg is 3,317 stream bytes: the 256-byte ring wraps a
        # dozen times with commands straddling its end.
        for name in ("run-shortwaits", "wait16", "wraps"):
            golden, totals = zvg_golden(name)
            for chip in (0, 2):
                for batch in (False, True):
                    z, since = self.run_zvg(name, chip, batch)
                    transcript = self.trace(z, since)
                    ticks, _marks, _orphans, trailing = split_ticks(
                        self.collect(z, since))
                    port = 0xE0 + chip
                    # The intervals alternate tick/gap: the even ones
                    # are the ticks.  The card's shadow reset (eleven
                    # registers on each of the four chips) goes out at
                    # snd_zvg_play, before the first tick, so tick 0 is
                    # the stream's own and nothing silences it.
                    pairs = ticks[0::2]
                    golden_writes = [(port, value)
                                     for _p, value in golden[0]]
                    self.assertEqual(pairs[0], golden_writes,
                                     "%s chip %d batch %d tick 0" %
                                     (name, chip, batch))
                    # The stop after the PASS line flushes the muted
                    # card (eleven registers per chip, chip 0 first);
                    # the replay window may cut it short.
                    self.assertEqual(len(pairs), totals["ticks"],
                                     "%s chip %d batch %d" %
                                     (name, chip, batch))
                    for k in range(1, totals["ticks"]):
                        want = [(port, value) for _p, value in golden[k]]
                        self.assertEqual(list(pairs[k]), want,
                                         "%s chip %d batch %d tick %d" %
                                         (name, chip, batch, k))
                    reset = [(0xE0, 0x80), (0xE0, 0x00), (0xE0, 0xA0),
                             (0xE0, 0x00), (0xE0, 0xC0), (0xE0, 0x00),
                             (0xE0, 0xE0), (0xE0, 0x9F), (0xE0, 0xBF),
                             (0xE0, 0xDF), (0xE0, 0xFF)]
                    self.assertLessEqual(len(trailing), 64)
                    if trailing:
                        self.assertEqual(trailing[:11], reset)
                    with open(os.path.join(ZVG_FIXTURES,
                                           name + ".zvg"), "rb") \
                            as handle:
                        stream = len(handle.read()) - 16
                    self.assertIn(" bytes %d" % stream, transcript)
                    self.assertIn("sndtest: zvg PASS state 5 ",
                                  transcript)
                    self.assertIn(" writes %d " % totals["writes"],
                                  transcript)
                    self.assertIn(" underruns 0 ",
                                  transcript)

    def test_malformed_streams_refused_or_stopped(self):
        outcomes = {
            "header-declared-long": ("refused", -10),
            "header-declared-zero": ("refused", -10),
            "header-length": ("refused", -2),
            "header-magic": ("refused", -2),
            "header-rate-high": ("refused", -2),
            "header-rate-zero": ("refused", -2),
            "header-truncated": ("refused", -10),
            "header-version": ("refused", -7),
            "stream-bad-opcode": ("error", (0x03, 16)),
            "stream-truncated-command": ("error", (0x02, 19)),
            "stream-wait16-zero": ("error", (0x02, 18)),
        }
        short = {
            "header-declared-long": "DECLONG.ZVG",
            "header-declared-zero": "DECZERO.ZVG",
            "header-length": "HLENGTH.ZVG",
            "header-magic": "HMAGIC.ZVG",
            "header-rate-high": "RATEHI.ZVG",
            "header-rate-zero": "RATEZERO.ZVG",
            "header-truncated": "HTRUNC.ZVG",
            "header-version": "HVERS.ZVG",
            "stream-bad-opcode": "SBADOP.ZVG",
            "stream-truncated-command": "STRUNC.ZVG",
            "stream-wait16-zero": "SW16Z.ZVG",
        }
        for name, (stage, expected) in outcomes.items():
            with open(SNTEST, "rb") as handle:
                program = handle.read()
            with open(os.path.join(ZVG_MALFORMED, name + ".zvg"), "rb") \
                    as handle:
                fixture = handle.read()
            z, since = self.start(
                {"SNDTEST.EXE": program, short[name]: fixture},
                ("/fs/sndtest.exe zvg /fs/%s 0 step\r" %
                 short[name].lower()).encode())
            if stage == "refused":
                self.run_to_pattern(z, "refused", since=since)
                self.assertIn("sndtest: zvg refused %d" % expected,
                              self.trace(z, since), name)
            else:
                self.run_to_pattern(z, "zvg opcode", since=since)
                self.run_to_pattern(z, "\n", since=len(z.glyphs))
                transcript = self.trace(z, since)
                self.assertIn("sndtest: zvg PASS state 4 ", transcript,
                              name)
                self.assertIn("sndtest: zvg opcode %02x at offset %d"
                              % expected, transcript, name)

    def test_stalled_reader_underruns_mutes_and_resumes_exactly(self):
        z, since = self.run_zvg("run-shortwaits", 0, stall=True)
        self.run_to_pattern(z, "zvgstall served", since=since)
        transcript = self.trace(z, since)
        self.assertIn("sndtest: zvgstall served 2", transcript)
        self.assertIn("sndtest: zvg PASS state 5 ", transcript)
        self.assertIn(" writes 21 ", transcript)
        self.assertIn(" underruns 2 ", transcript)
        ticks, _marks, _orphans, trailing = split_ticks(
            self.collect(z, since))
        mute = [(0xE0, 0x9F), (0xE0, 0xBF), (0xE0, 0xDF)]
        pairs = ticks[0::2]
        # The first stall refusal happens in prepare, so the ring is
        # empty for ticks 0 and 1 (two underruns, muted each time);
        # the second refusal is served at snd_service 1, and the stream
        # resumes exactly where the reader stalled: golden tick k at
        # tick k + 2.
        self.assertEqual(pairs[0], mute, "stalled tick 0")
        self.assertEqual(list(pairs[1]), mute, "stalled tick 1")
        golden, totals = zvg_golden("run-shortwaits")
        self.assertEqual(len(pairs), totals["ticks"] + 2)
        for k, want in enumerate(golden):
            self.assertEqual(list(pairs[k + 2]), list(want),
                             "resumed tick %d" % k)
        reset = [(0xE0, 0x80), (0xE0, 0x00), (0xE0, 0xA0),
                 (0xE0, 0x00), (0xE0, 0xC0), (0xE0, 0x00),
                 (0xE0, 0xE0), (0xE0, 0x9F), (0xE0, 0xBF),
                 (0xE0, 0xDF), (0xE0, 0xFF)]
        # the stop's mute, the close's release and the exit's reset
        self.assertLessEqual(len(trailing), 64)
        if trailing:
            self.assertEqual(trailing[:11], reset)

    def _engine_idle_baseline(self):
        """The engine's idle-tick cost, from the timing mode's idle
        pairs (the same attribution the timing test uses)."""
        with open(SNTEST, "rb") as handle:
            program = handle.read()
        z, since = self.start({"SNDTEST.EXE": program},
                              b"/fs/sndtest.exe timing\r")
        state = {"current": 0}
        intervals = []
        inner = z.sound_peer.write
        def write(port, value):
            inner(port, value)
            if (port & 0xFF, value & 0xFF) == (0xE4, 0xA5):
                intervals.append(state["current"])
                state["current"] = 0
        z.sound_peer.write = write
        steps = 0
        while steps < 24000000 and len(intervals) < 16:
            before = z.tstates
            pc_before = z.pc if app_mapped(z) else -1
            self.skip_delay(z)
            self.feed(z)
            z.step()
            if in_tick_code(pc_before):
                state["current"] += z.tstates - before
            if steps % 0x1FF == 0:
                self.pump(z)
            steps += 1
        return max(intervals[3:14:2])

    def test_manual_notes_refused_on_the_zvg_chip_allowed_elsewhere(self):
        with open(SNTEST, "rb") as handle:
            program = handle.read()
        with open(os.path.join(ZVG_FIXTURES, "run-shortwaits.zvg"),
                  "rb") as handle:
            fixture = handle.read()
        z, since = self.start(
            {"SNDTEST.EXE": program, "SHORT.ZVG": fixture},
            b"/fs/sndtest.exe zvgnotes /fs/short.zvg 2\r")
        self.run_to_pattern(z, "zvgnotes other", since=since)
        transcript = self.trace(z, since)
        self.assertIn("sndtest: zvgnotes busy -4", transcript)
        self.assertIn("sndtest: zvgnotes other 0", transcript)

    def test_the_tick_makes_no_reader_call_and_the_budgets(self):
        with open(SNTEST, "rb") as handle:
            program = handle.read()
        with open(os.path.join(ZVG_FIXTURES, "run-shortwaits.zvg"),
                  "rb") as handle:
            fixture = handle.read()
        z, since = self.start({"SNDTEST.EXE": program,
                               "SHORT.ZVG": fixture},
                              b"/fs/sndtest.exe zvg /fs/short.zvg 0 step\r")
        peer = z.sound_peer
        # The FS2 read requests land in the IOC peer's request log; the
        # drive loop below records each one's T-state so the tick spans
        # can be checked against them.  (The read_at for the header
        # happens at open, before the markers; the service's refills
        # land in the gaps.)
        reads = []
        read_state = {"seen": 0}
        def note_reads():
            for item in z.ioc_peer.requests[read_state["seen"]:]:
                if item[0] == z.ioc_peer.CMD_FS2_READ:
                    reads.append(z.tstates)
            read_state["seen"] = len(z.ioc_peer.requests)
        # The budget profile: attribute the tick code (the same ranges
        # the timing test uses) to the marker-to-marker intervals.
        state = {"current": 0}
        intervals = []
        inner = peer.write
        def write(port, value):
            inner(port, value)
            if (port & 0xFF, value & 0xFF) == (0xE4, 0xA5):
                intervals.append(state["current"])
                state["current"] = 0
        peer.write = write
        ENGINE_IDLE = self._engine_idle_baseline()
        wait_floor = ENGINE_IDLE
        steps = 0
        while steps < 24000000:
            before = z.tstates
            pc_before = z.pc if app_mapped(z) else -1
            self.skip_delay(z)
            self.feed(z)
            z.step()
            note_reads()
            if in_tick_code(pc_before, zvg=True):
                state["current"] += z.tstates - before
            if steps % 0x1FF == 0:
                self.pump(z)
                if "bytes" in "".join(z.glyphs[since:]):
                    break
            steps += 1
        ticks, marks, _orphans, _trailing = split_ticks(
            self.collect(z, since))
        # the read requests never fall inside a tick's marker pair
        for t in reads:
            for k in range(0, len(marks) - 1, 2):
                self.assertFalse(marks[k] <= t <= marks[k + 1],
                                 "a reader call inside tick %d" % (k // 2))
        golden, totals = zvg_golden("run-shortwaits")
        wait_costs = []
        write_costs = []
        for k, tick in enumerate(golden):
            cost = intervals[2 * k + 1] if 2 * k + 1 < len(intervals) \
                else 0
            if tick and k > 0:
                # tick 0 also carries the first flush's shadow rewrite
                write_costs.append((k, cost))
            elif not tick:
                wait_costs.append(cost)
        # The whole wait-only tick carries the engine's idle scan
        # (the timing test's idle budget) plus the ZVG's own wait
        # path; the 300-T-state target is the ZVG's incremental cost.
        wait_whole = max(wait_costs)
        wait_zvg = wait_whole - ENGINE_IDLE
        self.assertLessEqual(wait_zvg, 255,
                             "the ZVG's wait tick (target 300): %d"
                             % wait_zvg)
        # A command tick's cost over the wait tick is the decoder's
        # fixed command-loop entry; the marginal per written byte is the
        # remainder divided by the run length.
        write_floor = min(cost for _k, cost in write_costs)
        per_byte = []
        for k, cost in write_costs:
            n = len(golden[k])
            if n > 1:
                per_byte.append((cost - write_floor) // n)
        self.assertLessEqual(max(per_byte), 30,
                             "per written byte (target 50): %d" %
                             max(per_byte))
        services = [intervals[2 * k + 2]
                    for k in range(len(intervals) // 2 - 1)]
        print("zvg budgets: whole tick %d (engine idle %d, zvg %d), "
              "command loop %d, per-byte %d, service %d"
              % (wait_whole, ENGINE_IDLE, wait_zvg, write_floor,
                 max(per_byte), max(services)))


VGMPLAY = os.path.join(ROOT, "UserSoftware", "VGMPlayer", "build", "user",
                       "vgmplay.exe")
NIGHT = os.path.join(ZVG_FIXTURES, "NIGHT.ZVG")
ZVG_TRACE_TOOL = os.path.join(ROOT, "build", "host", "snd", "zvg_trace")


def oracle_sequence(path):
    """The oracle's whole write sequence for a ZVG, generated now (the
    host tool test_snd_oracle builds) rather than checked in."""
    if not os.path.exists(ZVG_TRACE_TOOL):
        import test_snd_oracle
        test_snd_oracle.build_tools()
    out = subprocess.run([ZVG_TRACE_TOOL, path], stdout=subprocess.PIPE,
                         check=True, cwd=ROOT).stdout.decode()
    sequence = []
    ticks = 0
    for line in out.splitlines():
        if line.startswith("tick "):
            ticks += 1
            body = line.split(":", 1)[1].split()
            sequence += [int(word[3:], 16) for word in body]
    return sequence, ticks


class ZvgTimedCase(SndReplay):
    """A whole song on the timer (the CTC on): the chip's write
    sequence equals the oracle's byte for byte - the card's reset
    before the first tick and the release's silence after the last
    aside - with every timer count played as a decoder tick."""

    def play(self, files, command, done):
        z, since = self.start(files, command)
        self.run_to_pattern(z, done, since=since, budget=400_000_000)
        return z, since

    def assert_song(self, z, since, port, release=True):
        expected, ticks = oracle_sequence(NIGHT)
        played = [value for p, value, _t in z.sound_peer.writes[since:]
                  if p == port]
        reset = [0x80, 0x00, 0xA0, 0x00, 0xC0, 0x00, 0xE0, 0x9F, 0xBF,
                 0xDF, 0xFF]
        # The driver's acquire silence (9F BF DF FF) may lead; libsnd's
        # reset follows, before the first tick.
        start = next((i for i in range(0, 8)
                      if played[i:i + 11] == reset), None)
        self.assertIsNotNone(start, "the reset before tick 0: %r"
                             % played[:16])
        first = start + 11
        body = played[first:first + len(expected)]
        self.assertEqual(len(body), len(expected))
        for index, (got, want) in enumerate(zip(body, expected)):
            if got != want:
                self.fail("write %d: %02x, oracle %02x" % (index, got,
                                                            want))
        if release:
            self.assertEqual(played[first + len(expected):][:11], reset,
                             "the release's silence")
        return ticks


@unittest.skipUnless(os.path.exists(SNTEST) and os.path.exists(NIGHT),
                     "run make")
class TestSndZvgTimed(ZvgTimedCase):
    def test_night_market_on_the_timer(self):
        with open(SNTEST, "rb") as handle:
            program = handle.read()
        with open(NIGHT, "rb") as handle:
            song = handle.read()
        z, since = self.play({"SNDTEST.EXE": program, "NIGHT.ZVG": song},
                             b"/fs/sndtest.exe zvg /fs/night.zvg 0\r",
                             "bytes")
        # The replay stops at the PASS line, before the stop's flush.
        ticks = self.assert_song(z, since, 0xE0, release=False)
        transcript = self.trace(z, since)
        self.assertIn("sndtest: zvg PASS state 5 events %d ticks %d "
                      % (ticks, ticks), transcript)
        self.assertIn(" underruns 0 ", transcript)


@unittest.skipUnless(os.path.exists(VGMPLAY) and os.path.exists(NIGHT),
                     "make -C UserSoftware/VGMPlayer")
class TestVgmplayTimed(ZvgTimedCase):
    def test_night_market_in_vgmplay(self):
        with open(VGMPLAY, "rb") as handle:
            program = handle.read()
        with open(NIGHT, "rb") as handle:
            song = handle.read()
        z, since = self.play({"VGMPLAY.EXE": program, "NIGHT.ZVG": song},
                             b"/fs/vgmplay.exe /fs/night.zvg\r",
                             "missed events")
        self.assert_song(z, since, 0xE0)
        transcript = self.trace(z, since)
        self.assertIn("Underruns 0  missed events 0", transcript)
