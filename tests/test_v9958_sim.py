#!/usr/bin/env python3
"""Host-side V9958 video service acceptance: the video diagnostic runs in
the console-integration image (work package 42d merged the per-device
images); this module replays that image's `sio ioc hid v9958
console-v9958-hid` service set, production geometry, against the V9958
peer.

Coverage (all executing the real linked bytes):

* the boot transcript: the nucleaus banner, the resident service loader's
  evidence rows for the image's set, and the video diagnostic's early-SIO
  transcript (endpoint discovery, argument-free PRESENT, the display
  reset, the `V41` terminal marker, the malformed refusals and the pinned
  `video selftest: OK`);
* the peer-observable protocol facts: the reset re-ran the complete
  bring-up - the 24-write GRAPHIC 6 register sequence appears exactly
  twice (the boot segment and the reset), both 6-KiB font atlas windows
  are written exactly twice (12,288 bytes per bring-up, the run-time one
  fed by the service's bulk segment), the full-page clear-screen HMMV
  runs once per bring-up with its CE-busy handshake, and the
  malformed-call phase performs no VDP access at all;
* the marker glyphs `V41` reach the decoded glyph trace through the
  diagnostic's direct video-endpoint writes (after its reset), proving
  the renderer path end to end.

Skipped when the image has not been built (`make console-integration-selftest`).
"""

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from replay import (BANNER, PROMPT, STEP_LIMIT, ConsoleReplay,  # noqa: E402
                    IntegrationInputs, integration_peer)

STEP_LIMIT = 80000000

# Early-SIO boot transcript.  BANNER and PROMPT come from the replay
# helper (the monitor's visible lane).
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

# Visible side.  This image selects the production HID/V9958 console, so
# the nucleus console line and the video diagnostic's marker share the
# V9958 lane with the monitor.
PROBE_LINE = "console: service OK"
MARKER_GLYPHS = "V41"
SMOKE_HOST_SYNC = b"\xaa\x55"

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


class VideoSimBase(ConsoleReplay, unittest.TestCase):
    """Boot, stepping and trace helpers for the video diagnostic."""

    IMAGE_SUBDIR = "build-console-integration-selftest"
    IMAGE_NAME = "zephyros-console-integration-selftest.bin"

    def setUp(self):
        self.load_image()

    def run_to_sio(self, z, expect, limit=STEP_LIMIT):
        """Step until `expect` appears in the early-SIO output."""
        data = expect.encode("ascii") if isinstance(expect, str) else expect
        self.run_until(z, [data], limit=limit, since=0)

    def boot_to_video_ok(self, z, limit=STEP_LIMIT):
        inputs = IntegrationInputs(z.ioc_peer)
        self.start_kernel(z, host_sync=SMOKE_HOST_SYNC)
        self.run_until(z, [VIDEO_OK + "\r\n"], limit=limit,
                       on_step=inputs)
        return self.trace(z)


class TestVideoServiceBoot(VideoSimBase):
    """The image boots the service set, the console probe renders through
    it, and the diagnostic's transcript is the pass/fail record."""

    def test_boot_transcript_and_loader_evidence(self):
        z = self.make_cpu()
        z.ioc_peer = integration_peer()
        self.boot_to_video_ok(z)
        sio = bytes(z.sio_tx).decode("ascii", "replace")

        # Kernel banner before any service exists.
        for line in (BANNER, PLATFORM, KERNEL_OK, SERIAL_OK):
            self.assertIn(line, sio)

        # The loader's evidence rows, in manifest order.  The v9958 row
        # carries the bulk placement of the font.
        rows = [
            r"driver sio.zmod: hot [0-9A-F]{4}h boot [0-9A-F]{4}h\r\n",
            r"driver ioc.zmod: hot [0-9A-F]{4}h boot [0-9A-F]{4}h\r\n",
            r"driver hid.zmod: hot [0-9A-F]{4}h\r\n",
            r"driver v9958.zmod: hot [0-9A-F]{4}h boot [0-9A-F]{4}h "
            r"bulk [0-9A-F]{4}h\r\n",
            r"driver console-v9958-hid.zmod: hot [0-9A-F]{4}h\r\n",
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
        # The video group is followed by the integration group in the
        # same image; the video marker is still its own pass record.
        self.assertIn(VIDEO_OK + "\r\n", sio)
        self.assertNotIn("FAIL", sio)
        self.assertNotIn("PANIC", sio)

        # The recorded outcome byte matches the transcript.
        self.assertEqual(z.bank7[self.syms["_ace_video_selftest_result"]],
                         0x00)

    def test_console_line_and_marker_glyphs(self):
        z = self.make_cpu()
        z.ioc_peer = integration_peer()
        self.boot_to_video_ok(z)
        sio = bytes(z.sio_tx).decode("ascii", "replace")
        trace = self.trace(z)

        # The console line is the build-selected driver's own output:
        # this image selects `console-v9958-hid`, so the line is on the
        # V9958 lane and never on the serial link.
        self.assertNotIn(PROBE_LINE, sio)
        self.assertIn(PROBE_LINE, trace)
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
        z.ioc_peer = integration_peer()
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
        z.ioc_peer = integration_peer()
        inputs = IntegrationInputs(z.ioc_peer)
        self.start_kernel(z, host_sync=SMOKE_HOST_SYNC)
        self.run_until(z, [VIDEO_MARKER + "\r\n"], on_step=inputs)
        snapshot = peer_snapshot(z.vdp)
        self.run_until(z, [VIDEO_REJECTS + "\r\n"], on_step=inputs)
        self.assertEqual(peer_snapshot(z.vdp), snapshot,
                         "the malformed refusals touched the VDP")
        self.run_until(z, [VIDEO_OK + "\r\n"], on_step=inputs)


if __name__ == "__main__":
    unittest.main()
