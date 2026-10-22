#!/usr/bin/env python3
"""Console-order and stream outcome acceptance, in the filesystem image.

Both halves run from the monitor's X in the `fs` image, so the transcript
is the evidence:

- `/system/iomix.exe` writes markers with no newline, reads an edited
  line, runs a child and exits with a marker still buffered.  With
  line-buffered console output (docs/sdk.md) the transcript must be
  exactly the one an unbuffered program produces: the prompt before the
  echo of the line it prompted for, the marker before the child's line,
  and the last marker before the monitor reports the status - which only
  the flush before every read, before `run` and at exit can deliver.

- `/system/iostream.exe` runs one stdio session over `/fs/IOSTREAM.TMP`:
  write, append, read back with `fgets` and `fread`, `ftell` after reads
  and writes, `fseek` inside and outside the stream's buffer, `rewind`, a
  write after a `fseek`, and a byte-for-byte comparison of the whole
  file.  Every outcome it reports must be OK and the exit status zero.

Run with: `python3 -m unittest tests.test_io_sim`.
"""

import os
import re
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from replay import STEP_LIMIT, Replay  # noqa: E402
from z80sim import Ctc, IocCommandPeer  # noqa: E402

IMAGE_SUBDIR = "build-fs-selftest"
IMAGE_NAME = "konaos-fs-selftest.bin"

MONITOR_PROMPT = "Z80> "
EXEC_STATUS = "exec: exit status 0x0000\r\n"

MIX_RUN = b"X /system/iomix.exe\r"
MIX_PROMPT = "iomix: prompt "
MIX_INPUT = b"hello\r"
# The whole exchange, as an unbuffered program would produce it: the
# prompt, the driver's echo of the typed line, the line the program read,
# the marker `run` flushed, the child's line, and the final marker with no
# newline of its own - flushed by the exit path, immediately before the
# monitor's status.
MIX_TRANSCRIPT = ("iomix: prompt hello\r\n"
                  "iomix: read \"hello\"\r\n"
                  "iomix: before-child iomix: child ran\r\n"
                  "iomix: after-childexec: exit status 0x0000\r\n")

STREAM_RUN = b"X /system/iostream.exe\r"
STREAM_BEGIN = "io-stream: begin\r\n"
STREAM_PASS = "io-stream: PASS"
STREAM_FAIL = "io-stream: FAIL"
STREAM_CHECKS = "io-stream: 37 checks, 0 failures"
# Every outcome the stream session must report, in order.
STREAM_OUTCOMES = (
    "fopen-write", "fputs-lines", "fwrite-pattern", "write-ftell",
    "write-close", "fopen-append", "append-ftell", "append-fputs",
    "append-ftell-end", "append-close", "fopen-read", "fgets-line",
    "fgets-ftell", "fgets-second", "fread-bytes", "read-ftell",
    "seek-cur", "seek-cur-bytes", "seek-set", "seek-outside-bytes",
    "seek-end", "eof-flag", "rewind", "read-back-bytes", "fread-eof",
    "fclose-read", "fopen-write-back", "write-seek", "write-after-seek",
    "write-after-seek-close", "fopen-reread", "reread-seek",
    "reread-bytes", "size-unchanged", "reread-file-bytes",
    "fclose-reread", "unlink",
)


class IoSdkBase(Replay, unittest.TestCase):
    """The filesystem image, with the card peer and the production clock:
    the monitor is the launcher and the transcript is the evidence."""

    IMAGE_SUBDIR = IMAGE_SUBDIR
    IMAGE_NAME = IMAGE_NAME

    def setUp(self):
        image = os.path.join(ROOT, IMAGE_SUBDIR, IMAGE_NAME)
        if not os.path.isfile(image):
            self.skipTest("%s is missing; run make fs-selftest" % image)
        self.load_image()

    def make_cpu(self, **peer_kwargs):
        z = super().make_cpu()
        # The emulated CTC drives the timer driver's channel-2 interrupt,
        # so the production clock runs: `iobench` measures with it and the
        # console programs see the same machine they run on.
        Ctc().attach(z)
        peer = IocCommandPeer(**peer_kwargs)
        z.hid = peer
        z.ioc_peer = peer
        return z

    def boot_to_prompt(self, z, limit=STEP_LIMIT):
        """Boot to the monitor's serial prompt (the image's boot program
        runs first and hands the console over on exit)."""
        self.start_kernel(z)
        self.run_until(z, [MONITOR_PROMPT, b"PANIC"], limit=limit)
        return z

    def command(self, z, text, stops):
        """Type one monitor command and return its transcript."""
        start = len(z.sio_tx)
        z.sio_rx.extend(text)
        self.run_until(z, list(stops), since=start, limit=STEP_LIMIT)
        return bytes(z.sio_tx[start:]).decode("ascii", "replace")


class TestConsoleOrderAndStreams(IoSdkBase):
    """The console-order program and the stream session, on a card."""

    def test_the_console_order_is_an_unbuffered_program_s(self):
        """`iomix` at the monitor: the marker written before the read
        arrives before the echo, the marker written before `run` arrives
        before the child's line, and the marker left in the buffer is
        flushed by the exit path, just before the monitor's status."""
        z = self.boot_to_prompt(self.make_cpu(fs2_media=True))
        start = len(z.sio_tx)
        z.sio_rx.extend(MIX_RUN)
        self.run_until(z, [MIX_PROMPT], since=start, limit=STEP_LIMIT)
        # The prompt is on the wire and the program is waiting for a line.
        z.sio_rx.extend(MIX_INPUT)
        self.run_until(z, [EXEC_STATUS, b"PANIC"], since=start,
                       limit=STEP_LIMIT)
        text = bytes(z.sio_tx[start:]).decode("ascii", "replace")

        self.assertIn(MIX_TRANSCRIPT, text)

    def test_the_stream_session_outcomes_in_the_filesystem_image(self):
        """`iostream` at the monitor reports every stream outcome, the
        pinned check count and the verdict, and exits 0."""
        z = self.boot_to_prompt(self.make_cpu(fs2_media=True))
        text = self.command(z, STREAM_RUN,
                            [EXEC_STATUS, STREAM_FAIL, b"PANIC"])

        self.assertIn(STREAM_BEGIN, text)
        for label in STREAM_OUTCOMES:
            self.assertIn("io-stream: %s OK\r\n" % label, text)
        self.assertNotIn(STREAM_FAIL, text)
        self.assertIn(STREAM_CHECKS, text)
        self.assertIn(STREAM_PASS, text)
        self.assertIn(EXEC_STATUS, text)


BENCH_RUN = b"X /system/iobench.exe\r"
BENCH_BEGIN = "iobench: begin\r\n"
BENCH_END = "iobench: end\r\n"
# The rows that need no card, and the ones that report SKIP without one.
BENCH_ROWS = ("ipc.null", "con.putchar", "con.printf", "con.write",
              "con.mixed", "con.space")
# The rows that need the video driver: this image's console is serial,
# so they report SKIP here; the production-image replay runs them for
# real.
BENCH_VIDEO_ROWS = ("con.held", "vdp.hmmm6", "vdp.hmmm12",
                    "vdp.direct.on", "vdp.direct.off")
BENCH_ROW_RE = (r"iobench: %s ops \d+ bytes \d+ min \d+ "
                r"mean \d+ max \d+ per-byte \d+\r\n")
BENCH_SKIP_RE = re.compile(r"iobench: file\.\w+ SKIP no card \(-\d+\)\r\n")
BENCH_VIDEO_SKIP_RE = re.compile(
    r"iobench: (?:con\.held|vdp\.hmmm6|vdp\.hmmm12|vdp\.direct\.on|"
    r"vdp\.direct\.off) "
    r"SKIP no video \(-\d+\)\r\n")


class TestTheBenchRows(IoSdkBase):
    """The bench with no card: its instrument and its row shape, fast.

    The figures themselves are measurements taken with a card present;
    this runs the same program in the same image without a card, so the
    rows that need one report SKIP and the rest cost no card traffic.
    """

    def test_the_rows_run_and_the_file_rows_skip(self):
        z = self.boot_to_prompt(self.make_cpu(fs2_media=False))
        text = self.command(z, BENCH_RUN,
                            [EXEC_STATUS, b"iobench: SKIP", b"PANIC"])

        self.assertIn(BENCH_BEGIN, text)
        for row in BENCH_ROWS:
            self.assertRegex(text, BENCH_ROW_RE % re.escape(row))
        self.assertEqual(len(BENCH_VIDEO_SKIP_RE.findall(text)),
                         len(BENCH_VIDEO_ROWS))
        for row in BENCH_VIDEO_ROWS:
            self.assertRegex(text, r"iobench: %s SKIP no video \(-\d+\)\r\n"
                             % re.escape(row))
        self.assertEqual(len(BENCH_SKIP_RE.findall(text)), 3)
        # The closing summary repeats every row in one screen.
        summary = text[text.index("iobench: summary\r\n"):]
        for row in BENCH_ROWS:
            self.assertRegex(summary, r"sum: %s mean \d+ per-byte \d+\r\n"
                             % re.escape(row))
        for row in BENCH_VIDEO_ROWS + ("file.byte", "file.getc",
                                       "file.block"):
            self.assertIn("sum: %s SKIP\r\n" % row, summary)
        self.assertIn(BENCH_END, text)
        self.assertIn(EXEC_STATUS, text)


if __name__ == "__main__":
    unittest.main()
