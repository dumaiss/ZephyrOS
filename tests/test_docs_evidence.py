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

# A cited record path; the leading character class excludes the bare
# directory reference `docs/validation/`.
REFERENCE_RE = re.compile(r"docs/validation/[A-Za-z0-9][A-Za-z0-9._-]*")


def _read(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read()


def _referenced_records():
    """Map each cited docs/validation path to the live docs citing it."""
    refs = {}
    for rel in LIVE_DOCS:
        path = os.path.join(ROOT, rel)
        if not os.path.isfile(path):
            continue
        for match in REFERENCE_RE.finditer(_read(path)):
            refs.setdefault(match.group(0), set()).add(rel)
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


if __name__ == "__main__":
    unittest.main()
