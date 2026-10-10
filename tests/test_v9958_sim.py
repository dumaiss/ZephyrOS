#!/usr/bin/env python3
"""Host-side V9958 video service acceptance: replay the real
video-selftest image (the `sio v9958 console-serial` service set,
production geometry) in the Z80 simulator against the V9958 peer.

Coverage (all executing the real linked bytes):

* production-style boot: the resident service loader places and starts
  the three service images and prints its early-SIO evidence lines
  (`driver sio.zmod: hot ... boot ...`,
  `driver v9958.zmod: hot ... boot ... bulk ...`,
  `driver console-serial.zmod: hot ...`), the v9958 bulk row being the
  font the run-time reset re-reads through the nucleus bulk copy;
* the nucleus console line: `console: service OK` is written through
  the build-selected console driver; this image's SIO-only set selects
  `console-serial`, so the line is on the serial lane and nothing the
  console prints reaches the V9958 trace;
* the target diagnostic's early-SIO transcript: endpoint discovery
  (video class 02 at protocol version 01, serial and console peers),
  the argument-free PRESENT, the display reset, the `V41` terminal
  marker written after the reset, the malformed refusals, and the
  pinned final `video selftest: OK`, which is also the last line the
  boot writes before the launcher starts the monitor;
* the peer-observable protocol facts: the reset re-ran the complete
  bring-up - the 24-write GRAPHIC 6 register sequence appears exactly
  twice (the boot segment and the reset), both 6-KiB font atlas
  windows are written exactly twice (12,288 bytes per bring-up, the
  run-time one fed by the service's bulk segment), the full-page
  clear-screen HMMV runs once per bring-up with its CE-busy handshake,
  and the malformed-call phase performs no VDP access at all;
* the marker glyphs `V41` reach the decoded glyph trace through the
  diagnostic's direct video-endpoint writes (after its reset), proving
  the renderer path end to end.

Skipped when the image has not been built (`make video-selftest`).
"""

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from test_console_sim import ConsoleSimBase  # noqa: E402
from z80sim import skip_serial_connect_delay  # noqa: E402

STEP_LIMIT = 80000000

# Early-SIO boot transcript.
BANNER = "ZephyrOS 0.0"
PLATFORM = "platform: Zephyr-80"
KERNEL_OK = "kernel entry: OK"
SERIAL_OK = "serial: OK"

# The diagnostic's transcript lines, in order.
VIDEO_ENDPOINT = "video: endpoint class 02 version 01"
VIDEO_PEERS = "video: peers serial=01 console=01"
VIDEO_PRESENT = "video: present OK"
VIDEO_RESET = "video: reset OK"
VIDEO_MARKER = "video: marker V41"
VIDEO_REJECTS = "video: malformed rejected"
VIDEO_OK = "video selftest: OK"

# Visible side.
PROBE_LINE = "console: service OK"
MARKER_GLYPHS = "V41"

# The proven GRAPHIC 6 bring-up register sequence, in order (value,
# register): the paced head, R#0..R#11, the selector block and R#25
# WAIT_ON.  The boot segment and the run-time ZVIDEO_RESET run the same
# chain, so the sequence must appear exactly twice.
G6_TABLE = [
    (0x0A, 0), (0x00, 1), (0x1F, 2), (0x00, 3), (0x00, 4), (0xE4, 5),
    (0x3F, 6), (0x04, 7), (0x08, 8), (0x88, 9), (0x00, 10), (0x03, 11),
]
BOOTSTRAP = ([(0x00, 25), (0x08, 8), (0x00, 46)] + G6_TABLE +
             [(0x00, 14), (0x02, 15), (0x00, 16), (0x00, 17), (0x00, 18),
              (0xFC, 23), (0x00, 26), (0x00, 27), (0x04, 25)])

# The two font atlas windows (A16 = 1), each a 16-KiB pitch holding
# 32 chunks x 8 scanlines x 4 segments x 24 bytes = 6144 written bytes:
# normal at 10000h and reverse at 14000h.
ATLAS_NORMAL = (0x10000, 0x14000)
ATLAS_REVERSE = (0x14000, 0x18000)
ATLAS_BRINGUP_WRITES = 2 * 32 * 8 * 24  # both atlases, one bring-up

# The clear-screen HMMV: full 512x256 page zero in the normal-video
# background (the PALETTE setup that precedes it is asserted by the
# 64 palette writes, not modelled here).
CLEAR_FILL = (0, 0, 512, 256, 0x44)


def bringup_starts(peer):
    """Instruction timestamps where the 24-write bring-up register
    sequence starts (boot segment and run-time reset)."""
    starts = []
    writes = peer.reg_writes
    span = len(BOOTSTRAP)
    index = 0
    while index + span <= len(writes):
        if all((writes[index + k][0], writes[index + k][1]) == BOOTSTRAP[k]
               for k in range(span)):
            starts.append(writes[index][2])
            index += span
        else:
            index += 1
    return starts


def atlas_writes(peer):
    """Data-port writes into the two atlas windows."""
    return [addr for addr, _ in peer.vram_writes
            if ATLAS_NORMAL[0] <= addr < ATLAS_NORMAL[1] or
            ATLAS_REVERSE[0] <= addr < ATLAS_REVERSE[1]]


def clear_fills(peer):
    """The full-page clear-screen HMMV commands."""
    return [cmd for cmd in peer.commands
            if (cmd["code"] & 0xF0) == 0xC0 and
            cmd.get("fill") == CLEAR_FILL]


def peer_snapshot(peer):
    """Every VDP-visible trace length, for frozen-phase checks."""
    return (len(peer.reg_writes), len(peer.vram_writes),
            len(peer.addr_writes), len(peer.palette_writes),
            len(peer.indirect_writes), len(peer.latch_writes),
            len(peer.status_reads), len(peer.commands))


class VideoSimBase(ConsoleSimBase):
    """Boot, stepping and trace helpers for the video service image."""

    IMAGE_SUBDIR = "build-video-selftest"
    IMAGE_NAME = "zephyros-video-selftest.bin"

    def skip_delay(self, z):
        """Return from the kernel's serial connect spin (see
        z80sim.skip_serial_connect_delay)."""
        skip_serial_connect_delay(
            z, self.syms["_kernel_serial_connect_delay"])

    def run_to_sio(self, z, expect, limit=STEP_LIMIT):
        """Step until `expect` appears in the early-SIO output."""
        data = expect.encode("ascii")
        for i in range(limit):
            self.skip_delay(z)
            z.step()
            if i % 0x1FF == 0:
                self.pump(z)
                if data in z.sio_tx:
                    return
        self.pump(z)
        raise AssertionError(
            "did not reach %r (PC=%04X, sio tail=%r)"
            % (expect, z.pc, bytes(z.sio_tx)[-160:]))

    def boot_to_video_ok(self, z, limit=STEP_LIMIT):
        self.start_kernel(z)
        self.run_to_sio(z, VIDEO_OK + "\r\n", limit)
        return "".join(z.glyphs)


class TestVideoServiceBoot(VideoSimBase):
    """The image boots the service set, the console probe renders through
    it, and the diagnostic's transcript is the pass/fail record."""

    def test_boot_transcript_and_loader_evidence(self):
        z = self.make_cpu()
        self.boot_to_video_ok(z)
        sio = bytes(z.sio_tx).decode("ascii", "replace")

        # Kernel banner before any service exists.
        for line in (BANNER, PLATFORM, KERNEL_OK, SERIAL_OK):
            self.assertIn(line, sio)

        # The loader's evidence rows, in manifest order.  The v9958 row
        # carries the bulk placement of the font.
        rows = [
            r"driver sio.zmod: hot [0-9A-F]{4}h boot [0-9A-F]{4}h\r\n",
            r"driver v9958.zmod: hot [0-9A-F]{4}h boot [0-9A-F]{4}h "
            r"bulk [0-9A-F]{4}h\r\n",
            r"driver console-serial.zmod: hot [0-9A-F]{4}h\r\n",
        ]
        positions = []
        for row in rows:
            match = re.search(row, sio)
            self.assertIsNotNone(match, "missing loader row %r" % row)
            positions.append(match.start())
        self.assertEqual(positions, sorted(positions),
                         "loader rows are not in manifest order")

        # The diagnostic transcript, in order, ending in the pinned line.
        transcript = [VIDEO_ENDPOINT, VIDEO_PEERS, VIDEO_PRESENT,
                      VIDEO_RESET, VIDEO_MARKER, VIDEO_REJECTS, VIDEO_OK]
        positions = [sio.index(line) for line in transcript]
        self.assertEqual(positions, sorted(positions),
                         "diagnostic transcript is out of order")
        self.assertTrue(sio.rstrip("\r\n").endswith(VIDEO_OK),
                        "the last transcript line is not %r" % VIDEO_OK)
        self.assertNotIn("FAIL", sio)
        self.assertNotIn("PANIC", sio)

        # The recorded outcome byte matches the transcript.
        self.assertEqual(z.bank7[self.syms["_ace_video_selftest_result"]],
                         0x00)

    def test_console_line_and_marker_glyphs(self):
        z = self.make_cpu()
        self.boot_to_video_ok(z)
        sio = bytes(z.sio_tx).decode("ascii", "replace")
        trace = "".join(z.glyphs)

        # The console line is the build-selected driver's own output:
        # this image selects `console-serial`, so the line is on the
        # serial lane and never in the V9958 glyph trace.
        self.assertIn(PROBE_LINE, sio)
        self.assertNotIn(PROBE_LINE, trace)
        self.assertIn(VIDEO_OK, sio)

        # The marker glyphs come from the diagnostic's direct video
        # writes: the reset cleared the plane, the PUTC pass redrew at
        # the home cell.
        self.assertIn(MARKER_GLYPHS, trace)

        # The marker line records the pass on the early console too.
        self.assertIn(VIDEO_MARKER, sio)


class TestVideoServicePeerEvidence(VideoSimBase):
    """The peer's register/command trace proves what the protocol did."""

    def test_reset_reruns_the_full_bringup(self):
        z = self.make_cpu()
        self.boot_to_video_ok(z)
        peer = z.vdp

        # Two complete GRAPHIC 6 register sequences: the boot segment's
        # initial bring-up and the diagnostic's ZVIDEO_RESET re-run.
        starts = bringup_starts(peer)
        self.assertEqual(len(starts), 2,
                         "expected the boot and reset bring-ups, got %r"
                         % starts)
        reset_at = starts[1]

        # Both 6-KiB atlas windows written exactly twice over: the
        # run-time reset re-read and re-uploaded the whole font (the
        # bulk segment) through the nucleus bulk copy.
        self.assertEqual(len(atlas_writes(peer)),
                         2 * ATLAS_BRINGUP_WRITES,
                         "the reset did not re-upload both atlas pages")

        # One clear-screen HMMV per bring-up, each a real command-engine
        # execution (CE busy observed before completion).
        clears = clear_fills(peer)
        self.assertEqual(len(clears), 2)
        for clear in clears:
            self.assertIsNotNone(clear["busy_until"])

        # After the reset started: a CE-busy status read and a
        # command-engine execution prove the handshake really ran.
        after = [sample for sample in peer.status_reads if sample[0] >= reset_at]
        self.assertTrue(any(ce for _, ce in after),
                        "no CE-busy status read after the reset")
        self.assertTrue(
            any(cmd["start"] >= reset_at and cmd["busy_until"] is not None
                for cmd in peer.commands),
            "no command engine activity after the reset")

        # No stray control-port protocol errors anywhere.
        self.assertEqual(peer.protocol_errors, [])

    def test_malformed_phase_makes_no_vdp_access(self):
        z = self.make_cpu()
        self.start_kernel(z)
        self.run_to_sio(z, VIDEO_MARKER + "\r\n")
        snapshot = peer_snapshot(z.vdp)
        self.run_to_sio(z, VIDEO_REJECTS + "\r\n")
        self.assertEqual(peer_snapshot(z.vdp), snapshot,
                         "the malformed refusals touched the VDP")
        self.run_to_sio(z, VIDEO_OK + "\r\n")


if __name__ == "__main__":
    unittest.main()
