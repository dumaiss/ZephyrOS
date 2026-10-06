#!/usr/bin/env python3
"""Documentation/evidence integrity tests (work package 24).

Tracked live documentation cites dated acceptance records under
`docs/validation/`.  Every cited path must exist in the working tree, so
documentation never carries a dangling evidence citation.

By repository policy the validation records (like the work/ contracts)
are kept locally and are deliberately not tracked in git; this test
therefore checks existence only, not git tracking or ignore rules.

Run with: make test
"""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# Live normative documentation (excludes the dated records themselves and
# the historical work/ contracts).
LIVE_DOCS = (
    "README.md",
    "docs/abi.md",
    "docs/architecture.md",
    "docs/boot.md",
    "docs/devices.md",
    "docs/memory.md",
    "docs/module-format.md",
    "docs/monitor.md",
    "docs/romfs-format.md",
    "docs/zex-format.md",
    "docs/vfs.md",
)

# The dated validation records are themselves scanned: a record that cites
# a transcript or an earlier record must have it on disk, so a durable
# record never carries a citation that resolves nowhere.  Records and the
# work/ contracts are deliberately untracked, so only existence is
# checked, never git state.
RECORD_DIR = "docs/validation"

# A cited record path; the leading character class excludes the bare
# directory reference `docs/validation/`.
REFERENCE_RE = re.compile(r"docs/validation/[A-Za-z0-9][A-Za-z0-9._-]*")


def _read(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read()


def _record_files():
    directory = os.path.join(ROOT, RECORD_DIR)
    if not os.path.isdir(directory):
        return []
    return [os.path.join(RECORD_DIR, name)
            for name in sorted(os.listdir(directory))
            if name.endswith(".md")]


def _cited_records(text):
    return {match.group(0) for match in REFERENCE_RE.finditer(text)}


def _referenced_records():
    """Map each cited docs/validation path to the live docs citing it."""
    refs = {}
    for rel in LIVE_DOCS:
        path = os.path.join(ROOT, rel)
        if not os.path.isfile(path):
            continue
        for ref in _cited_records(_read(path)):
            refs.setdefault(ref, set()).add(rel)
    return refs


class TestValidationEvidenceReferences(unittest.TestCase):
    def test_references_were_found(self):
        self.assertTrue(_referenced_records(),
                        "live documentation cites no validation records")

    def test_every_referenced_record_exists(self):
        missing = sorted(ref for ref in _referenced_records()
                         if not os.path.isfile(os.path.join(ROOT, ref)))
        self.assertEqual(missing, [],
                         "live documentation cites missing records: %s"
                         % ", ".join(missing))

    def test_records_exist(self):
        self.assertTrue(_record_files(),
                        "%s holds no dated record" % RECORD_DIR)

    def test_every_record_citation_resolves(self):
        missing = {}
        for rel in _record_files():
            for ref in _cited_records(_read(os.path.join(ROOT, rel))):
                if not os.path.isfile(os.path.join(ROOT, ref)):
                    missing.setdefault(rel, set()).add(ref)
        self.assertEqual(
            {rel: sorted(refs) for rel, refs in missing.items()}, {},
            "validation records cite missing evidence files")

    def test_hardware_transcripts_are_cited(self):
        # The M9 integration record names the physical transcripts beside it;
        # every named `*-tx-transcript`-style file must be on disk.
        record = os.path.join(ROOT, "docs", "validation",
                              "2026-10-05-m9-filesystem.md")
        if not os.path.isfile(record):
            self.skipTest("the M9 integration record is not present")
        text = _read(record)
        for match in re.finditer(r"[A-Za-z0-9._-]+\.txt", text):
            name = match.group(0)
            path = os.path.join(ROOT, "docs", "validation", name)
            self.assertTrue(os.path.isfile(path),
                            "the record cites missing transcript %s" % name)


if __name__ == "__main__":
    unittest.main()
