#!/usr/bin/env python3
"""The truncate operation's outcome replay.

Boots the production image with a writable card, runs the filesystem
program's `truncate` tail from the shell and reads its check lines: the
shrink, the stale-cache invalidation, the no-op rule, the position
clamp and the read-only refusals.  The FS2 request log pins the
operation's traffic: the shrink sends exactly one FS2 TRUNCATE, the
position clamp a second, and the no-ops none.  The shell comes back to
its prompt afterwards.

Run with: `python3 -m unittest tests.test_truncate`.
"""

import sys
import unittest

HERE = sys.path[0]
import os  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from replay import SHELL_PROMPT  # noqa: E402
from test_hwtest_sim import HwtestReplay  # noqa: E402

CMD_FS2_TRUNCATE = 0x40

COMMAND = b"/system/fs_selftest.exe truncate\r"
PASS = "fs-selftest: truncate PASS"
FAIL = "fs-selftest: truncate FAIL"

# One check() line per outcome the tail runs.
CHECKS = 14
OUTCOMES = (
    "trunc-open", "trunc-write", "trunc-cache-read", "trunc-shrink",
    "trunc-eof", "trunc-stat", "trunc-head", "trunc-end", "trunc-noop",
    "trunc-position", "trunc-ro-handle", "trunc-ro-unchanged",
    "trunc-ro-system", "trunc-call-refusal",
)


class TestTruncate(HwtestReplay):
    def test_shrink_cache_noop_position_and_refusals(self):
        z = self.start(card=True)
        peer = z.ioc_peer
        since = len(z.glyphs)
        z.hid.hid_queue.extend(COMMAND)

        # The no-op rule, pinned on the FS2 request log: after the
        # shrink exactly one TRUNCATE travelled (the program may race
        # ahead of its console, so the pins sit at markers whose
        # following calls cannot change the count).
        self.run_to(z, "fs-selftest: trunc-eof OK", since=since)
        self.assertEqual(peer.fs2_counts[CMD_FS2_TRUNCATE], 1,
                         "the shrink sent one TRUNCATE: %r"
                         % dict(peer.fs2_counts))
        # The position clamp sends the run's second TRUNCATE, and the
        # two no-ops none: a stray no-op exchange would make three.
        self.run_to(z, "fs-selftest: trunc-position OK", since=since)
        self.assertEqual(peer.fs2_counts[CMD_FS2_TRUNCATE], 2,
                         "the shrink and the clamp sent two TRUNCATEs "
                         "and the no-ops none: %r"
                         % dict(peer.fs2_counts))

        self.run_to(z, PASS, since=since)
        fresh = self.trace(z, since)
        self.assertNotIn(FAIL, fresh)
        for label in OUTCOMES:
            self.assertIn("fs-selftest: %s OK\n" % label, fresh)
        self.assertIn("fs-selftest: truncate %u checks, 0 failures\n"
                      % CHECKS, fresh)
        # The shell is back at its prompt.
        self.run_to(z, SHELL_PROMPT, since=since)


if __name__ == "__main__":
    unittest.main()
