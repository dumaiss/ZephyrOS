#!/usr/bin/env python3
"""Host-side HID acceptance replay: the HID diagnostic runs in the
console-integration image (work package 42d merged the per-device
images); this module replays that image in the Z80 simulator against the
emulated IOC peer.

The image boots the production-derived nucleus and the driver loader
brings up the `sio ioc hid v9958 console-v9958-hid` bank-6 set from the
recovery store; the console composes the HID input with the V9958 output
(the production selection), and the HID diagnostic
(tests/target/hid/hid_test.c) runs in the late hook before the video and
integration groups.  Every assertion executes the real linked bytes.

Coverage:

* the early-SIO boot transcript: the kernel banner, the loader's
  `driver <name>: hot <addr> ...` evidence lines in manifest order for
  the image's drivers, and the diagnostic's markers through the final
  `hid selftest: OK`, with no `FAIL`;
* the scripted typed pass: the replay queues a, B, 3, Enter, Backspace,
  Ctrl-X, Arrow-Up for the MCU, the engine fetches on the doorbell and
  the diagnostic echoes every byte in FIFO order;
* the console phase: the console endpoint's input is the HID driver in
  this build, so the pair the replay queues for the MCU arrives through
  the console's GETC;
* malformed-reply atomicity and the doorbell release after the drain.

Skipped when the image has not been built
(`make console-integration-selftest`).
"""

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from replay import (STEP_LIMIT, IntegrationInputs,  # noqa: E402
                    Replay, integration_peer)

HOST_SYNC = b"\xaa\x55"

CMD_LINK_SYNC = 0x0C
CMD_HID_INPUT = 0x0E

CMD_LINK_SYNC = 0x0C
CMD_HID_INPUT = 0x0E

# The pinned scripts the diagnostic and this replay agree on.
HID_TYPED = bytes((0x61, 0x42, 0x33, 0x0D, 0x08, 0x18, 0x1B, 0x5B, 0x41))
HID_CONSOLE = bytes((0x4F, 0x4B))
HID_FAULT = bytes((0x71, 0x72))
# Every fetch asks for the 16-byte HID ring's full free space (the
# protocol caps a reply at 24 DATA bytes, so the ring rules).
HID_FETCH_SIZE = 16

# Report markers the diagnostic contract pins.
OK_MARKER = b"hid selftest: OK"
CONSOLE_INPUT_MARKER = b"hid: console input\r\n"
CONSOLE_ABSENT_MARKER = b"hid: console absent\r\n"
FAULT_MARKER = b"hid: fault pass\r\n"


class HidSimBase(Replay, unittest.TestCase):
    """Shared boot, stepping and transcript helpers."""

    IMAGE_SUBDIR = "build-console-integration-selftest"
    IMAGE_NAME = "konaos-console-integration-selftest.bin"

    def setUp(self):
        self.load_image()

    def make_cpu(self, peer=None):
        z = super().make_cpu()
        z.ioc_peer = peer if peer is not None else IocCommandPeer()
        return z

    def boot(self, peer):
        """Boot the image to the diagnostic's final verdict, feeding the
        streams the merged image's diagnostics expect through the shared
        input driver.  Returns (machine, transcript)."""
        z = self.make_cpu(peer)
        self.start_kernel(z, host_sync=HOST_SYNC)
        # Stop on either verdict: a FAIL must not burn the whole step
        # budget before the assertions report it.
        self.run_until(z, (OK_MARKER + b"\r\n", b"hid selftest: FAIL"),
                       on_step=IntegrationInputs(peer))
        sio = bytes(z.sio_tx)
        self.assertIn(OK_MARKER + b"\r\n", sio)
        self.assertIn(CONSOLE_INPUT_MARKER, sio)
        self.assertNotIn(CONSOLE_ABSENT_MARKER, sio)
        return z, sio

    # -- assertion helpers ----------------------------------------------
    def assert_order(self, text, markers):
        at = 0
        for marker in markers:
            found = text.find(marker, at)
            self.assertNotEqual(found, -1,
                                "marker %r missing from the transcript"
                                % marker)
            at = found + len(marker)

    def driver_lines(self, sio):
        # The loader names each image by its ROMFS entry (`sio.mod`).
        return re.findall(
            rb"driver ([a-z0-9-]+)\.mod: hot [0-9A-F]{4}h"
            rb"(?: io [0-9A-F]{4}h)?(?: boot [0-9A-F]{4}h)?"
            rb"(?: bulk [0-9A-F]{4}h)?\r\n", sio)


class TestHidBootTranscript(HidSimBase):
    def test_boot_evidence_and_diagnostic_transcript(self):
        peer = integration_peer()
        _z, sio = self.boot(peer)

        # Kernel banner, then the loader's evidence lines in manifest
        # order, then the nucleus console probe; no boot failure.
        self.assert_order(sio, (
            b"KonaOS 0.0\r\n",
            b"platform: Zephyr-80\r\n",
            b"kernel entry: OK\r\n",
            b"serial: OK\r\n",
        ))
        names = [b"sio", b"ioc", b"hid", b"v9958", b"console-v9958-hid"]
        self.assertEqual(self.driver_lines(sio), names)
        self.assertIn(b"driver loader: %d images in bank 6\r\n"
                      % len(names), sio)
        self.assertIn(b"driver hot: ", sio)
        self.assertIn(b"driver boot/bulk: ", sio)
        # The production console routes its boot line to the V9958 lane,
        # not this early-SIO transcript.
        self.assertNotIn(b"console: driver OK\r\n", sio)
        self.assertNotIn(b"console: unbound", sio)
        self.assertNotIn(b"console: driver failed", sio)
        self.assertNotIn(b"console: unavailable", sio)

        # The diagnostic's protocol rows, in order.
        markers = [
            b"hid: class 04 v01 handle ",
            b"hid: ioc class 05 v01 handle ",
            b"hid: ready 1\r\n",
            b"hid: poll 1\r\n",
            b"hid: typed 09\r\n",
        ]
        markers += [b"hid: console input\r\n", b"hid: console OK\r\n"]
        markers += [
            b"hid: fault pass\r\n",
            b"hid: fault OK\r\n",
            b"hid: rejected calls OK\r\n",
            b"hid selftest: OK\r\n",
        ]
        self.assert_order(sio, tuple(markers))
        handles = re.search(
            rb"hid: class 04 v01 handle ([0-9A-F]{2})\r\n"
            rb"hid: ioc class 05 v01 handle ([0-9A-F]{2})\r\n", sio)
        self.assertIsNotNone(handles, "endpoint discovery rows missing")
        self.assertNotEqual(handles.group(1), handles.group(2),
                            "HID and IOC answered the same endpoint handle")

        # The typed pass echoes exactly the scripted FIFO bytes; the
        # console driver reads the same engine when it is in the store.
        typed_block = b"".join(b"hid: rx %02X\r\n" % byte
                               for byte in HID_TYPED)
        self.assertIn(typed_block, sio)
        self.assertIn(b"hid: con rx 4F\r\nhid: con rx 4B\r\n", sio)

        # Malformed-reply atomicity: between the fault markers only the
        # clean pair is delivered; the short reply's single payload byte
        # (00h) and the peer's synthetic fill (EAh) never appear.
        fault_region = sio.split(FAULT_MARKER, 1)[1].split(
            b"hid: fault OK\r\n", 1)[0]
        self.assertEqual(re.findall(rb"hid: rx ([0-9A-F]{2})",
                                    fault_region),
                         [b"%02X" % byte for byte in HID_FAULT])
        self.assertNotIn(b"hid: rx 00", sio)
        self.assertNotIn(b"hid: rx EA", sio)

        self.assertNotIn(b"hid selftest: FAIL", sio)


class TestHidPeerFacts(HidSimBase):
    def test_fetch_on_doorbell_drain_and_release(self):
        peer = integration_peer()
        z, _sio = self.boot(peer)

        # Every fetch asked for the ring's full free space, and the MCU
        # dequeued exactly the delivered bytes: the typed pass, the clean
        # pair behind the malformed reply, and the console pair (this
        # image's console input is the HID engine).
        fetches = 4  # typed, poisoned, retry, console
        delivered = len(HID_TYPED) + len(HID_FAULT) + len(HID_CONSOLE)
        self.assertEqual(peer.hid_requests, [HID_FETCH_SIZE] * fetches)
        self.assertEqual(peer.hid_delivered, delivered)
        self.assertEqual(list(peer.hid_queue), [],
                         "the MCU queue was not drained")
        self.assertEqual(peer.hid_faults, [],
                         "the injected malformed reply was not consumed")

        # The wire carried only the LINK_SYNC bring-up and the HID
        # fetches: STATUS sampling and the penalty are register-level.
        hid_requests = [req for req in peer.requests
                        if req[0] == CMD_HID_INPUT]
        self.assertEqual(len(hid_requests), fetches)
        for request in hid_requests:
            self.assertEqual(request[3], bytes([HID_FETCH_SIZE]))
        # The non-HID traffic is the IOC driver's own bring-up plus the
        # IOC diagnostic's exchanges (the group that runs before HID in
        # this image).
        non_hid = [req[0] for req in peer.requests
                   if req[0] != CMD_HID_INPUT]
        self.assertEqual(non_hid[0], CMD_LINK_SYNC)
        # The A lane's two exchanges belong to the IOC diagnostic that
        # runs earlier in this image (its one bulk phase per direction);
        # the HID fetches add none.
        self.assertEqual(peer.a_rts_asserts, 2,
                         "the HID engine used the bulk lane")

        # The queued/drained transition: with the MCU queue empty the
        # /CTSB doorbell is released, so a continuing poll issues no
        # further fetch.  A fetch only happens while the doorbell is
        # asserted, which is how the held-doorbell fetches above could
        # happen at all.
        self.assertEqual(peer.ctrl_read() & 0x20, 0,
                         "the doorbell is not released after the drain")
        for _ in range(200000):
            z.step()
        self.assertEqual(peer.hid_requests, [HID_FETCH_SIZE] * fetches,
                         "a fetch happened with the doorbell released")


if __name__ == "__main__":
    unittest.main()
