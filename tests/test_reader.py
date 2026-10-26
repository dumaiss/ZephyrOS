#!/usr/bin/env python3
"""tests/test_reader.py - the shared read-at reader (package 99).

- Host: the memory adapter and `kos_reader_read_full`, built with the
  host compiler against a fake reader that returns short chunks,
  transient errors and an early end.
- Target: readertest on the production image, the same fixture through
  all four adapters with matching checksums and probes, the FS2 open
  count (KRES opens no second file), the sources still alive after the
  readers are dropped, the readermap link proof, and the framed
  per-adapter read_at cost report.

Run with: make
"""

import hashlib
import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from z80sim import IocCommandPeer  # noqa: E402
from snd_harness import SndReplay, SHELL_PROMPT  # noqa: E402

READER = os.path.join(ROOT, "tests", "reader")
FIXTURES = os.path.join(READER, "fixtures")
HOST = os.path.join(READER, "host")
HOST_INC = os.path.join(HOST, "include")
SDK_SRC = os.path.join(ROOT, "user", "sdk", "src")
SDK_INC = os.path.join(ROOT, "user", "sdk", "include")
READERTEST = os.path.join(ROOT, "build", "user", "readertest.exe")
READERMAP = os.path.join(ROOT, "build", "user", "readermap.exe")
READERTEST_MAP = os.path.join(ROOT, "build", "user", "readertest",
                              "readertest.map")
READERMAP_MAP = os.path.join(ROOT, "build", "user", "readermap",
                             "readermap.map")

CARD_DAT = os.path.join(FIXTURES, "card.dat")
READER_KPR = os.path.join(FIXTURES, "reader.kpr")

CARD_SHA = ("8ab783947bae918d01b74448625dfd0fef119e16e97d6fc9fdf"
            "15e310c87cb01")
KPR_SHA = ("3fabfac55a61e22c5321beaf35405cbd9ce21eedb4690f464fa6"
           "f3dc202164f6")


def _digest(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


class TestReaderFixtures(unittest.TestCase):
    def test_the_card_fixture_is_pinned(self):
        self.assertEqual(_digest(CARD_DAT), CARD_SHA)

    def test_the_sealed_container_holds_the_card_fixture(self):
        self.assertEqual(_digest(READER_KPR), KPR_SHA)
        kres = os.path.join(ROOT, "build", "host", "kres")
        if not os.path.isfile(kres):
            subprocess.run(["make", "kres-host"], check=True, cwd=ROOT,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = os.path.join(FIXTURES, "extracted.dat")
        subprocess.run([kres, "extract", READER_KPR, "card.dat", out],
                       check=True, cwd=ROOT, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
        try:
            self.assertEqual(_digest(out), CARD_SHA)
        finally:
            os.unlink(out)


class TestReaderHost(unittest.TestCase):
    """reader.c + reader_full.c with the host compiler against a fake
    reader that returns short chunks, transient errors and an early
    end."""

    @classmethod
    def setUpClass(cls):
        os.makedirs(HOST, exist_ok=True)
        binary = os.path.join(HOST, "host_reader")
        subprocess.run(
            ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
             "-I" + HOST_INC, "-I" + SDK_INC,
             os.path.join(SDK_SRC, "reader.c"),
             os.path.join(SDK_SRC, "reader_full.c"),
             os.path.join(HOST, "host_reader.c"), "-o", binary],
            check=True, cwd=ROOT)
        cls.output = subprocess.run(
            [binary], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            check=True, cwd=ROOT, text=True).stdout

    def test_the_memory_adapter_and_read_full(self):
        self.assertIn("host: reader PASS", self.output)


@unittest.skipUnless(os.path.exists(READERTEST), "run make")
class TestReaderCard(SndReplay):
    """readertest on the production image."""

    def _start(self, command):
        with open(READERTEST, "rb") as handle:
            program = handle.read()
        with open(CARD_DAT, "rb") as handle:
            card = handle.read()
        with open(READER_KPR, "rb") as handle:
            kpr = handle.read()
        z, since = self.start(
            {"RDRTEST.EXE": program, "CARD.DAT": card,
             "READER.KPR": kpr},
            ("/fs/rdrtest.exe %s\r" % command).encode())
        self.run_to_pattern(z, "reader PASS", since=since)
        return z, since

    def test_the_four_adapters_read_the_same_bytes(self):
        z, since = self._start("")
        transcript = self.trace(z, since)
        self.assertIn("readertest: probes 64 ok", transcript)
        self.assertIn("readertest: kres probes 64 ok", transcript)
        self.assertIn("readertest: sources alive", transcript)
        self.assertIn("readertest: kres alive", transcript)
        sums = [line for line in transcript.splitlines()
                if line.startswith("readertest: sums ")]
        self.assertEqual(len(sums), 1, transcript)
        fields = sums[0].split()
        values = [int(fields[2]), int(fields[3]), int(fields[4])]
        self.assertEqual(values[0], values[1], transcript)
        self.assertEqual(values[0], values[2], transcript)
        self.assertEqual(int(fields[5]), 0, "the kres placeholder")
        self.assertEqual(int(fields[7]), 1024, "the fixture size")
        self.assertEqual(int(fields[9]), 0, "the checksum result")

    def test_kres_opens_no_second_file(self):
        with open(READERTEST, "rb") as handle:
            program = handle.read()
        with open(CARD_DAT, "rb") as handle:
            card = handle.read()
        with open(READER_KPR, "rb") as handle:
            kpr = handle.read()

        class CountingPeer(IocCommandPeer):
            def __init__(self, *args, **kwargs):
                super().__init__(*args, **kwargs)
                self.fs2_open_names = []

            def _fs2_open(self, seq, data, writable):
                leaf = self._fs2_unpack(bytes(data[1:12])) \
                    if writable else self._fs2_unpack(bytes(data[:11]))
                self.fs2_open_names.append(leaf)
                return super()._fs2_open(seq, data, writable)

        z = self.make_cpu()
        z.hid = z.ioc_peer = CountingPeer(
            fs2_media=True,
            fs2_files={"RDRTEST.EXE": program, "CARD.DAT": card,
                       "READER.KPR": kpr})
        self.boot_to_shell(z, prompt=SHELL_PROMPT)
        since = len(z.glyphs)
        z.hid.hid_queue.extend(b"/fs/rdrtest.exe\r")
        self.run_to_pattern(z, "reader PASS", since=since)
        self.assertEqual(
            z.ioc_peer.fs2_open_names,
            ["RDRTEST.EXE", "CARD.DAT", "CARD.DAT", "READER.KPR"],
            "the exe, one open per source, none by an adapter")

    def test_the_timing_report(self):
        """The framed timing run: bracket each phase on its printed
        line and subtract the empty loop, so the per-call read_at cost
        of each adapter is reported (WORK.md's report row)."""
        with open(READERTEST, "rb") as handle:
            program = handle.read()
        with open(CARD_DAT, "rb") as handle:
            card = handle.read()
        with open(READER_KPR, "rb") as handle:
            kpr = handle.read()
        z, since = self.start(
            {"RDRTEST.EXE": program, "CARD.DAT": card,
             "READER.KPR": kpr},
            b"/fs/rdrtest.exe timing\r")
        brackets = {}
        steps = 0
        while steps < 24000000:
            self.skip_delay(z)
            self.feed(z)
            z.step()
            if steps % 0x1FF == 0:
                self.pump(z)
            tail = "".join(z.glyphs[since:])
            for phase in ("reader-time begin", "reader-time handle",
                          "reader-time stdio", "reader-time memory",
                          "reader-time zero", "kres-time begin",
                          "kres-time kres", "kres-time zero"):
                if phase not in brackets and phase in tail:
                    brackets[phase] = z.tstates
            if "reader PASS" in tail:
                break
            steps += 1
        self.assertIn("reader PASS", "".join(z.glyphs[since:]))
        for phase in ("reader-time begin", "reader-time handle",
                      "reader-time stdio", "reader-time memory",
                      "reader-time zero", "kres-time begin",
                      "kres-time kres", "kres-time zero"):
            self.assertIn(phase, brackets, phase)
        zero = brackets["reader-time zero"] - \
            brackets["reader-time memory"]
        per_call = {}
        for name, prev in (("handle", "reader-time begin"),
                           ("stdio", "reader-time handle"),
                           ("memory", "reader-time stdio")):
            per_call[name] = (brackets["reader-time " + name] -
                              brackets[prev] - zero) // 16
        kzero = brackets["kres-time zero"] - brackets["kres-time kres"]
        per_call["kres"] = (brackets["kres-time kres"] -
                            brackets["kres-time begin"] - kzero) // 16
        print("reader read_at per call (T-states): handle %d, stdio %d, "
              "kres %d, memory %d"
              % (per_call["handle"], per_call["stdio"], per_call["kres"],
                 per_call["memory"]))
        for name in ("handle", "stdio", "kres", "memory"):
            self.assertGreater(per_call[name], 0, name)


@unittest.skipUnless(os.path.exists(READERMAP), "run make")
class TestReaderMap(SndReplay):
    """readermap: the memory adapter alone links nothing else."""

    def test_the_memory_adapter_links_alone(self):
        with open(READERMAP, "rb") as handle:
            program = handle.read()
        z, since = self.start({"RDRMAP.EXE": program},
                              b"/fs/rdrmap.exe\r")
        self.run_to_pattern(z, "map PASS", since=since)
        self.assertIn("readermap: map PASS", self.trace(z, since))
        with open(READERMAP_MAP) as handle:
            symbols = set(handle.read().split())
        self.assertIn("_kos_reader_from_memory", symbols,
                      "the memory member")
        for name in ("_kos_reader_from_handle", "_kos_reader_from_stdio",
                     "_kos_reader_from_kres", "_seek", "_read",
                     "_fread", "_fseek", "_kres_seek", "_kres_read"):
            self.assertNotIn(name, symbols,
                             "%s must not link" % name)

    def test_readertest_links_all_four_adapters(self):
        with open(READERTEST_MAP) as handle:
            symbols = set(handle.read().split())
        for name in ("_kos_reader_from_handle", "_kos_reader_from_stdio",
                     "_kos_reader_from_kres",
                     "_kos_reader_from_memory", "_kos_reader_read_full"):
            self.assertIn(name, symbols, name)


if __name__ == "__main__":
    unittest.main()
