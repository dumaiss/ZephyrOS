"""File runs: IOC-FAT's FS2_READ_RUN read path on the production image.

The firmware that adds FS2_READ_RUN (45h) offers it in CAPS (flag 1000h);
IOC-FAT then fills its read-ahead ring with one command per run, one bulk
packet per block, instead of one FS2_READ per block.  The simulator's IOC
peer models the command (`fs2_read_run=True`); without it the peer is the
deployed firmware, which IOC-FAT reads block by block.

Runs serve files larger than the block cache (16 KiB): a `cat` of one
shows the bytes the runs delivered.  A `cp` reads between writes, and a
write drops the ring, so a copy reads only what each read asks.
"""

import os
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "tests"))

from test_hwtest_sim import KEEP_CONTENT, KEEP_NAME, HwtestReplay  # noqa: E402
import test_shell_sim  # noqa: E402
from replay import SHELL_PROMPT  # noqa: E402
from z80sim import Ctc, IocCommandPeer  # noqa: E402

# Text larger than the block cache, so its open is uncached and its fills
# are runs: 210 lines of 80 bytes, 33 blocks, the last one short.
LINES = [("%03d " % n + "".join(chr(0x41 + (n + k) % 26) for k in range(74)))
         for n in range(210)]
TEXT = ("\r\n".join(LINES) + "\r\n").encode("ascii")
# Binary, larger than the block cache, not a multiple of 512.
BIG = bytes((i * 7 + (i >> 9)) & 0xFF for i in range(20000))
SMALL = b"a small file, read through the cache\r\n"
RUN = IocCommandPeer.CMD_FS2_READ_RUN
READ = IocCommandPeer.CMD_FS2_READ


class RunSession(HwtestReplay):
    """The production image with a card; `send_long` is the shell
    session's chunked line helper."""

    send_long = test_shell_sim.ShellSession.send_long

    def start_runs(self, read_run, run_fault=None):
        z = self.make_cpu()
        Ctc().attach(z)
        files = {"/".join(KEEP_NAME): KEEP_CONTENT, "BIG.DAT": BIG,
                 "TEXT.TXT": TEXT, "SMALL.TXT": SMALL}
        z.hid = z.ioc_peer = IocCommandPeer(
            fs2_media=True, fs2_files=files, fs2_read_run=read_run,
            fs2_run_fault=run_fault)
        self.boot_to_shell(z, prompt=SHELL_PROMPT)
        return z


class TestRuns(RunSession):

    def test_a_large_file_reads_in_runs_of_four_blocks(self):
        z = self.start_runs(read_run=True)
        peer = z.ioc_peer
        fresh = self.send_long(z, "cat /fs/TEXT.TXT", LINES[-1] + "\n/> ")
        # Every line arrived whole and in order.
        shown = [line for line in fresh.split("\n") if line[:3].isdigit()]
        self.assertEqual(shown, LINES)
        # 33 blocks in runs of four, the last one short; at most the very
        # first fill reads one block on its own (a ring the boot's own
        # state change left reads only what is asked).
        self.assertLessEqual(peer.fs2_counts[READ], 1)
        self.assertGreaterEqual(peer.fs2_counts[RUN], 8)
        self.assertLessEqual(peer.fs2_counts[RUN], 9)

    def test_a_copy_reads_each_block_once(self):
        z = self.start_runs(read_run=True)
        peer = z.ioc_peer
        fresh = self.send_long(z, "cp /fs/BIG.DAT /fs/COPY.DAT", "/> ")
        self.assertNotIn("cp:", fresh)
        self.assertEqual(bytes(peer.fs2_files[("COPY.DAT",)]), BIG)
        # Each write drops the ring, so no read reads ahead: one block per
        # read, 40, and the end-of-file read reads the last one again.
        self.assertEqual(peer.fs2_counts[RUN], 0)
        self.assertEqual(peer.fs2_counts[READ], 41)

    def test_the_deployed_firmware_is_read_block_by_block(self):
        z = self.start_runs(read_run=False)
        peer = z.ioc_peer
        fresh = self.send_long(z, "cp /fs/BIG.DAT /fs/COPY.DAT", "/> ")
        self.assertNotIn("cp:", fresh)
        self.assertEqual(bytes(peer.fs2_files[("COPY.DAT",)]), BIG)
        self.assertEqual(peer.fs2_counts[RUN], 0)
        self.assertEqual(peer.fs2_counts[READ], 41)

    def test_an_error_packet_fails_the_read_and_the_link_recovers(self):
        # Every run's third block is answered with the error packet.
        z = self.start_runs(read_run=True, run_fault=("io", 3))
        fresh = self.send_long(z, "cat /fs/TEXT.TXT", "/> ")
        self.assertIn("cat: ", fresh)
        # The next command re-syncs the link and is served normally.
        fresh = self.send_long(z, "cat /fs/SMALL.TXT", "/> ")
        self.assertIn(SMALL.decode("ascii").strip(), fresh)


if __name__ == "__main__":
    unittest.main()
