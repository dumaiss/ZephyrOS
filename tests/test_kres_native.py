#!/usr/bin/env python3
"""The KRES KonaOS edition: native replays on the production image.

Boots the production image with a writable card, runs krestest and
kresread from the shell and checks their printed outcomes, then reads
the containers the runs left on the card back with the host tool:

- host to native: a container packed by `build/host/kres pack` is read
  by `krestest read`, and its checksums match;
- native build: `krestest build` makes a sealed container the host
  tool verifies, byte for byte;
- native to host: `krestest edit` makes a container the host tool
  verifies; B extracts to the written pattern; the growth moved B to a
  fresh extent (the old one is unreferenced) and the commit advances
  the area and generation;
- the two-slot rule, from the same run: a container opens beside a
  direct file, a second container is refused;
- growth and commit: the same run compacted shortens the file
  (truncate), keeps B intact and leaves no recovery name;
- mid-flush cutoff: an `edit2` replay stopped after the second
  catalog-area write still verifies, with the previous catalog (B
  intact);
- sealed and read-only: writes and creates are refused and the
  container is unchanged;
- the read-only link: kresread's map links only the read path, the
  adapter's read side and the cache.

The card holds KEEP.TXT untouched in every run, exactly as the
filesystem suite's runs do.

Run with: `python3 -m unittest tests.test_kres_native`.
"""

import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from replay import SHELL_PROMPT  # noqa: E402
from test_hwtest_sim import HwtestReplay  # noqa: E402
from z80sim import IocCommandPeer  # noqa: E402

TOOL = os.path.join(ROOT, "build", "host", "kres")
KRESTEST_EXE = os.path.join(ROOT, "build", "user", "krestest.exe")
KRESREAD_EXE = os.path.join(ROOT, "build", "user", "kresread.exe")
KRESREAD_MAP = os.path.join(ROOT, "build", "user", "kresread",
                            "kresread.map")

KEEP_NAME = "KEEP.TXT"
KEEP_CONTENT = b"keep me on the card"

CMD_FS2_WRITE = 0x3E

# The krestest.c patterns and their checksums.
INFO_BYTES = 64
DATA_BYTES = 1500
EDIT_BYTES = 1000


def pattern_info():
    return bytes((i * 3 + 1) & 0xFF for i in range(INFO_BYTES))


def pattern_data(count=DATA_BYTES):
    return bytes((i * 7 + 5) & 0xFF for i in range(count))


INFO_SUM = sum(pattern_info()) & 0xFFFF
DATA_SUM = sum(pattern_data()) & 0xFFFF
EDIT_SUM = sum(pattern_data(EDIT_BYTES)) & 0xFFFF

# The layout facts the host-side assertions parse (kres_layout.h):
# the header fields and the catalog areas, in bytes.
HDR_RES_COUNT = 0x12
HDR_NEXT_RECORD = 0x14
HDR_ACTIVE_AREA = 0x10
HDR_GENERATION = 0x0C
ENT_START = 0x30
ENT_ALLOCATED = 0x34
ENT_USED = 0x38
AREA_A_BASE = 1 * 512
AREA_B_BASE = 5 * 512
DATA_BASE_MUT = 9 * 512


def get16(data, offset):
    return int.from_bytes(data[offset:offset + 2], "little")


def get32(data, offset):
    return int.from_bytes(data[offset:offset + 4], "little")


class KresPeer(IocCommandPeer):
    """The card peer, recording every FS2 write's offset so the
    mid-flush cutoff can stop after exactly two catalog records."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.fs2_write_offsets = []

    def _fs2_write(self, seq, data):
        self.fs2_write_offsets.append(
            int.from_bytes(bytes(data[2:6]), "little"))
        return super()._fs2_write(seq, data)


class KresReplay(HwtestReplay):
    """The production image and the card with krestest and kresread."""

    def setUp(self):
        self.load_image()
        for path in (KRESTEST_EXE, KRESREAD_EXE):
            if not os.path.exists(path):
                self.skipTest("%s missing; run make" % path)

    def start(self, files):
        """Boot to the card shell with `files` on the card plus the
        two programs and the keep file."""
        card = {KEEP_NAME: KEEP_CONTENT}
        card.update(files)
        with open(KRESTEST_EXE, "rb") as handle:
            card["KRESTEST.EXE"] = handle.read()
        with open(KRESREAD_EXE, "rb") as handle:
            card["KRESREAD.EXE"] = handle.read()
        z = self.make_cpu()
        z.hid = z.ioc_peer = KresPeer(fs2_media=True, fs2_files=card)
        self.boot_to_shell(z, prompt=SHELL_PROMPT)
        return z

    def run_command(self, z, command, until, since):
        z.hid.hid_queue.extend(command.encode("ascii") + b"\r")
        self.run_to(z, until, since=since)

    def run_krestest(self, z, mode, until, since):
        self.run_command(z, "/fs/krestest.exe " + mode, until, since)

    def card_bytes(self, z, name):
        self.assertIn((name,), z.ioc_peer.fs2_files,
                      "the card holds %s" % name)
        return bytes(z.ioc_peer.fs2_files[(name,)])

    def card_has(self, z, name):
        return (name,) in z.ioc_peer.fs2_files

    # -- host tool wrappers -------------------------------------------
    def run_tool(self, *args, **kwargs):
        return subprocess.run([TOOL] + list(args), check=True,
                              capture_output=True, **kwargs)

    def tool_bytes(self, container, name):
        """The host tool's extract of `name`, byte for byte."""
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "out.bin")
            self.run_tool("extract", container, name, out)
            with open(out, "rb") as handle:
                return handle.read()

    # -- entry-line helpers -------------------------------------------
    @staticmethod
    def entry_lines(trace, prefix):
        """A dict of name -> (length, sum, result) from the printed
        entry lines."""
        entries = {}
        for line in trace.splitlines():
            parts = line.split(" ")
            if len(parts) != 6 or parts[0] != prefix + ":":
                continue
            if parts[1] != "entry":
                continue
            entries[parts[2]] = (int(parts[3]), int(parts[4]))
        return entries

    def line_value(self, trace, label, prefix="krestest"):
        """The integer printed after `label` on its line, or None."""
        for line in trace.splitlines():
            parts = line.split(" ")
            if len(parts) == 3 and parts[0] == prefix + ":" and \
                    parts[1] == label:
                return int(parts[2])
        return None


class TestKresNative(KresReplay):
    def test_host_to_native_read_readonly_and_read_map(self):
        """Host-packed container -> krestest read; readonly refusals;
        kresread (the read-only link program) on the card."""
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "src")
            os.mkdir(src)
            for name, content in (("INFO", pattern_info()),
                                  ("DATA", pattern_data())):
                with open(os.path.join(src, name), "wb") as handle:
                    handle.write(content)
            built = os.path.join(tmp, "BUILT.KPR")
            self.run_tool("pack", src, built)
            with open(built, "rb") as handle:
                packed = handle.read()

        z = self.start({"BUILT.KPR": packed})
        since = len(z.glyphs)
        self.run_krestest(z, "read", "krestest: read-close 0", since)
        trace = self.trace(z, since)
        self.assertNotIn("FAIL", trace)
        self.assertEqual(self.line_value(trace, "read-open"), 0)
        entries = self.entry_lines(trace, "krestest")
        self.assertEqual(entries, {"INFO": (INFO_BYTES, INFO_SUM),
                                   "DATA": (DATA_BYTES, DATA_SUM)})

        since = len(z.glyphs)
        self.run_krestest(z, "readonly", "krestest: readonly-close 0",
                          since)
        trace = self.trace(z, since)
        self.assertEqual(self.line_value(trace, "readonly-open"), 0)
        self.assertEqual(self.line_value(trace, "readonly-get"), 0)
        self.assertEqual(self.line_value(trace, "readonly-sum"), DATA_SUM)
        # The read-opened resource refuses the write (EINVAL), the
        # sealed container refuses the create (EROFS).
        self.assertEqual(self.line_value(trace, "readonly-write"), -2)
        self.assertEqual(self.line_value(trace, "readonly-create"), -20)
        self.assertEqual(self.card_bytes(z, "BUILT.KPR"), packed,
                         "the sealed container is unchanged")

        # The read-only program runs against the same container.
        since = len(z.glyphs)
        self.run_command(z, "/fs/kresread.exe", "kresread: close 0",
                         since)
        trace = self.trace(z, since)
        self.assertEqual(self.line_value(trace, "open", prefix="kresread"),
                         0)
        entries = self.entry_lines(trace, "kresread")
        self.assertEqual(entries, {"INFO": (INFO_BYTES, INFO_SUM),
                                   "DATA": (DATA_BYTES, DATA_SUM)})

        # Back at the shell's prompt.
        self.run_to(z, SHELL_PROMPT, since=since)

    def test_read_only_link_splits_the_members(self):
        """kresread's map carries the read path, the adapter's read
        side and the cache - and nothing mutation-side."""
        with open(KRESREAD_MAP, encoding="latin-1") as handle:
            text = handle.read()
        for module in ("kres_util.rel", "kres_read.rel",
                       "kres_adapter.rel", "kres_cache_xmem.rel"):
            self.assertIn("[ %s ]" % module, text,
                          "the reader links %s" % module)
        for symbol in ("_kres_install_mut", "_kres_create_container",
                       "_kres_flush", "_kres_compact", "_kres_verify",
                       "_kres_extract", "_kres_build_begin",
                       "_kres_write"):
            self.assertIsNone(re.search(r"\b%s\b" % symbol, text),
                              "the reader links no %s" % symbol)

    def _run_edit_boot(self):
        """Boot, run edit, and return (z, post_edit_bytes)."""
        z = self.start({})
        since = len(z.glyphs)
        self.run_krestest(z, "edit", "krestest: edit-close 0", since)
        trace = self.trace(z, since)
        self.assertNotIn("FAIL", trace)
        self.assertEqual(self.line_value(trace, "edit-open"), 0)
        self.assertGreaterEqual(self.line_value(trace,
                                                "edit-extra-open"), 1)
        for label in ("edit-create", "edit-write", "edit-rename",
                      "edit-create2", "edit-delete", "edit-flush",
                      "edit-close"):
            self.assertEqual(self.line_value(trace, label), 0)
        # The two-slot rule: both card slots were held, so the second
        # container open is refused (KOS_EMFILE).
        self.assertEqual(self.line_value(trace, "edit-second-open"), -23)
        return z, self.card_bytes(z, "EDITED.KPR")

    def test_native_to_host_growth_commit_compact_and_extract(self):
        """The edit run, read back by the host tool, then compact and
        extract on the card."""
        z, edited = self._run_edit_boot()
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "EDITED.KPR")
            with open(path, "wb") as handle:
                handle.write(edited)

            # Native to host: verify passes, B extracts byte for byte.
            self.run_tool("verify", path)
            self.assertEqual(self.tool_bytes(path, "B"),
                             pattern_data(EDIT_BYTES))
            listed = self.run_tool("list", path).stdout.decode()
            self.assertIn("%-28s %6u %6u" % ("B", EDIT_BYTES, 4),
                          listed, listed)
            # Growth and commit: the header switched to area B,
            # generation 1, next_record 16, and B lives at a fresh
            # extent (10) - the old extent (9) is unreferenced.
            self.assertEqual(edited[HDR_ACTIVE_AREA], 1)
            self.assertEqual(get32(edited, HDR_GENERATION), 1)
            self.assertEqual(get16(edited, HDR_RES_COUNT), 1)
            self.assertEqual(get32(edited, HDR_NEXT_RECORD), 16)
            entry = edited[AREA_B_BASE + ENT_START:
                           AREA_B_BASE + ENT_START + 16]
            self.assertEqual(get32(entry, 0), 10)
            self.assertEqual(get32(entry, 4), 4)
            self.assertEqual(get32(entry, 8), EDIT_BYTES)

        # Compaction on the card: shorter (truncate), B intact, the
        # recovery name gone.
        since = len(z.glyphs)
        self.run_krestest(z, "compact", "krestest: verify 0", since)
        trace = self.trace(z, since)
        self.assertEqual(self.line_value(trace, "compact"), 0)
        compacted = self.card_bytes(z, "EDITED.KPR")
        # B's extent starts at record 9: 9 full records plus the
        # 1,000 live bytes (the tail may end in a partial record).
        self.assertEqual(len(compacted), 9 * 512 + EDIT_BYTES,
                         "compact dropped the unreferenced extents")
        self.assertFalse(self.card_has(z, "EDITED#.KPR"),
                         "no recovery name remains")
        self.assertFalse(self.card_has(z, "EDITED$.KPR"),
                         "no temporary name remains")
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "EDITED.KPR")
            with open(path, "wb") as handle:
                handle.write(compacted)
            self.run_tool("verify", path)
            self.assertEqual(self.tool_bytes(path, "B"),
                             pattern_data(EDIT_BYTES))

        # Extract B to a card file.
        since = len(z.glyphs)
        self.run_krestest(z, "extract", "krestest: extract-size 1000",
                          since)
        trace = self.trace(z, since)
        for label in ("extract-open", "extract", "extract-close"):
            self.assertEqual(self.line_value(trace, label), 0)
        self.assertEqual(self.card_bytes(z, "EXTRACT.BIN"),
                         pattern_data(EDIT_BYTES))
        self.run_to(z, SHELL_PROMPT, since=since)

    def test_midflush_cutoff_keeps_the_previous_catalog(self):
        """A replay stopped after the second catalog-area write still
        verifies, with the previous catalog."""
        _, edited = self._run_edit_boot()
        z = self.start({"EDITED.KPR": edited})
        # The flush's catalog writes land in area A (offsets 512..2560)
        # and the header write at offset 0 is the commit point: stop
        # after at least two catalog records and before the header.
        peer = z.ioc_peer

        def cutoff(_step):
            area_a = [o for o in peer.fs2_write_offsets
                      if AREA_A_BASE <= o < AREA_B_BASE]
            return 0 not in peer.fs2_write_offsets and len(area_a) >= 2

        z.hid.hid_queue.extend(b"/fs/krestest.exe edit2\r")
        self.assertIsNotNone(self.drive(z, 4_000_000, tick=cutoff,
                                        every=1),
                             "the run reached no mid-flush window")
        cut = self.card_bytes(z, "EDITED.KPR")
        # The header was not committed: still area B, generation 1,
        # one resource, and B verifies with its full bytes.
        self.assertEqual(cut[HDR_ACTIVE_AREA], 1)
        self.assertEqual(get32(cut, HDR_GENERATION), 1)
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "EDITED.KPR")
            with open(path, "wb") as handle:
                handle.write(cut)
            self.run_tool("verify", path)
            listed = self.run_tool("list", path).stdout.decode()
            self.assertIn("%-28s %6u %6u" % ("B", EDIT_BYTES, 4),
                          listed, listed)
            self.assertEqual(self.tool_bytes(path, "B"),
                             pattern_data(EDIT_BYTES))

    def test_native_build_seals_a_container(self):
        """krestest build's sealed container, verified and read back
        by the host tool and by krestest read."""
        z = self.start({})
        since = len(z.glyphs)
        self.run_krestest(z, "build", "krestest: verify 0", since)
        trace = self.trace(z, since)
        self.assertNotIn("FAIL", trace)
        self.assertEqual(self.line_value(trace, "build-begin"), 0)
        self.assertEqual(self.line_value(trace, "build-add"), 0)
        # kres_build_write reports the bytes it took.
        self.assertEqual(self.line_value(trace, "build-write"),
                         INFO_BYTES)
        self.assertEqual(self.line_value(trace, "build-add2"), 0)
        self.assertEqual(self.line_value(trace, "build-write2"), 0)
        self.assertEqual(self.line_value(trace, "build-end"), 0)
        self.assertEqual(self.line_value(trace, "verify"), 0)
        built = self.card_bytes(z, "BUILT.KPR")
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "BUILT.KPR")
            with open(path, "wb") as handle:
                handle.write(built)
            self.run_tool("verify", path)
            self.assertEqual(self.tool_bytes(path, "INFO"),
                             pattern_info())
            self.assertEqual(self.tool_bytes(path, "DATA"),
                             pattern_data())

        since = len(z.glyphs)
        self.run_krestest(z, "read", "krestest: read-close 0", since)
        trace = self.trace(z, since)
        entries = self.entry_lines(trace, "krestest")
        self.assertEqual(entries, {"INFO": (INFO_BYTES, INFO_SUM),
                                   "DATA": (DATA_BYTES, DATA_SUM)})
        self.run_to(z, SHELL_PROMPT, since=since)


if __name__ == "__main__":
    unittest.main()
