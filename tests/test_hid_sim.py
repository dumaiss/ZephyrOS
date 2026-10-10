#!/usr/bin/env python3
"""Host-side HID acceptance replay: boot the real hid-selftest image in
the Z80 simulator against the emulated IOC peer.

The image boots the production-derived nucleus and the service loader
brings up the `sio ioc hid console` bank-6 set from the recovery store;
the console service composes the HID input with the SIO output, and the
nucleus late hook runs the package-41 HID diagnostic
(tests/target/hid/hid_test.c).  Every assertion executes the real linked
bytes.

Coverage:

* the early-SIO boot transcript: the kernel banner, the loader's
  `driver <name>: hot <addr> ...` evidence lines in manifest order for
  the image's services, the nucleus console probe and the diagnostic's
  markers through the final `hid selftest: OK`, with no `FAIL` line and
  no failure marker;
* the diagnostic's protocol rows: the HID endpoint at protocol version 1
  with the IOC dependency and console endpoints present, ZHID_OP_READY = 1, the
  scripted typed pass (a B 3 Enter Backspace Ctrl-X Arrow-Up) echoed in
  FIFO order, -ZOS_EAGAIN once drained, and the same engine read through
  the console service's ZCONSOLE_OP_GETC;
* the peer-observable facts: the fetch-on-doorbell CMD_HID_INPUT
  exchanges (each asking for the ring's full free space), the MCU queue
  drained byte-for-byte, no wire traffic beyond the LINK_SYNC bring-up
  and the HID fetches, and the held-/CTSB -> released transition (the
  running console polls issue no fetch once the queue is empty);
* malformed-reply atomicity: one injected short CMD_HID_INPUT reply
  delivers nothing; the clean pair queued behind it still arrives in
  FIFO order after the engine's bounded stuck-doorbell penalty.

Skipped cleanly when build-hid-selftest/zephyros-hid-selftest.bin has
not been built (`make hid-selftest`).
"""

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import (Z80, IocCommandPeer, read_symbols,
                    skip_serial_connect_delay)  # noqa: E402

RET = 0x1234
STACK = 0xFFFE
STEP_LIMIT = 80000000
HOST_SYNC = b"\xaa\x55"

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


class HidSimBase(unittest.TestCase):
    """Shared boot, stepping and transcript helpers."""

    IMAGE_DIR = "build-hid-selftest"
    IMAGE_NAME = "zephyros-hid-selftest.bin"

    def setUp(self):
        self.bin_path = os.path.join(ROOT, self.IMAGE_DIR, self.IMAGE_NAME)
        if not os.path.exists(self.bin_path):
            self.skipTest("%s missing; build the image first" % self.bin_path)
        with open(self.bin_path, "rb") as fh:
            self.image = fh.read()
        self.syms = read_symbols(
            os.path.join(ROOT, self.IMAGE_DIR, "zephyros.map"))

    # -- machine setup --------------------------------------------------
    def make_cpu(self, peer=None):
        pages = {n: self.image[n * 0x10000:(n + 1) * 0x10000]
                 for n in range(8)}
        z = Z80(pages[0], pages[1],
                rom_pages={n: pages[n] for n in range(2, 8)})
        z.latch = 0x18
        z.sp = STACK
        z.ioc_peer = peer if peer is not None else IocCommandPeer()
        return z

    def crt0(self, z):
        z.push16(RET)
        z.pc = self.syms["__zos_crt0_init"]
        for _ in range(2000000):
            z.step()
            if z.pc == RET:
                return
        raise AssertionError("C runtime init did not return")

    def start_kernel(self, z):
        # The smoke-host-sync build waits for the AAh 55h pair before the
        # banner (the bench reset also resets the USB bridge).
        self.crt0(z)
        z.push16(RET)
        z.sio_rx.extend(HOST_SYNC)
        z.pc = self.syms["_kernel_main"]

    def boot(self, peer):
        """Boot the image to the diagnostic's final verdict, injecting
        the console/fault streams into the MCU queue at their markers.
        Returns (machine, transcript).  This image's declared service set
        always includes the console service."""
        z = self.make_cpu(peer)
        self.start_kernel(z)
        injected = []
        done = {"console": False, "fault": False}

        def on_tx(tx):
            if not done["console"] and CONSOLE_INPUT_MARKER in tx:
                # This image selects `console-serial` (work package 42e):
                # the console's input is the SERIAL service, so the pair
                # travels on SIO0/B.  The HID engine is driven directly by
                # the other phases.
                z.sio_rx.extend(HID_CONSOLE)
                done["console"] = True
                injected.append("console")
            if not done["fault"] and FAULT_MARKER in tx:
                peer.hid_queue.extend(HID_FAULT)
                peer.hid_faults.append("short")
                done["fault"] = True
                injected.append("fault")

        # Stop on either verdict: a FAIL must not burn the whole step
        # budget before the assertions report it.
        sio = self.step_until(
            z, (OK_MARKER + b"\r\n", b"hid selftest: FAIL"), on_tx=on_tx)
        self.assertIn(OK_MARKER + b"\r\n", sio)
        self.assertIn("fault", injected)
        self.assertIn("console", injected)
        self.assertIn(CONSOLE_INPUT_MARKER, sio)
        self.assertNotIn(CONSOLE_ABSENT_MARKER, sio)
        return z, sio

    def step_until(self, z, markers, on_tx=None, limit=STEP_LIMIT):
        for _ in range(limit):
            z.step()
            if on_tx is not None:
                on_tx(z.sio_tx)
            for marker in markers:
                if marker in z.sio_tx:
                    return bytes(z.sio_tx)
        raise AssertionError("did not reach any of %r (PC=%04X, tail=%r)"
                             % (markers, z.pc, bytes(z.sio_tx)[-200:]))

    # -- assertion helpers ----------------------------------------------
    def assert_order(self, text, markers):
        at = 0
        for marker in markers:
            found = text.find(marker, at)
            self.assertNotEqual(found, -1,
                                "marker %r missing from the transcript"
                                % marker)
            at = found + len(marker)

    def service_lines(self, sio):
        # The loader names each image by its ROMFS entry (`sio.zmod`).
        return re.findall(
            rb"driver ([a-z0-9-]+)\.zmod: hot [0-9A-F]{4}h"
            rb"(?: io [0-9A-F]{4}h)?(?: boot [0-9A-F]{4}h)?"
            rb"(?: bulk [0-9A-F]{4}h)?\r\n", sio)


class TestHidBootTranscript(HidSimBase):
    def test_boot_evidence_and_diagnostic_transcript(self):
        peer = IocCommandPeer(hid_bytes=HID_TYPED)
        _z, sio = self.boot(peer)

        # Kernel banner, then the loader's evidence lines in manifest
        # order, then the nucleus console probe; no boot failure.
        self.assert_order(sio, (
            b"ZephyrOS 0.0\r\n",
            b"platform: Zephyr-80\r\n",
            b"kernel entry: OK\r\n",
            b"serial: OK\r\n",
        ))
        names = [b"sio", b"ioc", b"hid", b"console-serial"]
        self.assertEqual(self.service_lines(sio), names)
        self.assertIn(b"service loader: %d images in bank 6\r\n"
                      % len(names), sio)
        self.assertIn(b"driver hot: ", sio)
        self.assertIn(b"driver boot/bulk: ", sio)
        self.assertIn(b"console: service OK\r\n", sio)
        self.assertNotIn(b"console: unbound", sio)
        self.assertNotIn(b"console: service failed", sio)
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
        # console service reads the same engine when it is in the store.
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
        peer = IocCommandPeer(hid_bytes=HID_TYPED)
        z, _sio = self.boot(peer)

        # Every fetch asked for the ring's full free space, and the MCU
        # dequeued exactly the delivered bytes: the typed pass, and the
        # clean pair behind the malformed reply.  The console pair is not
        # here: this image selects `console-serial`, so its console reads
        # the serial lane, not the HID engine.
        fetches = 3  # typed, poisoned, retry
        delivered = len(HID_TYPED) + len(HID_FAULT)
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
        self.assertEqual([req[0] for req in peer.requests
                          if req[0] != CMD_HID_INPUT], [CMD_LINK_SYNC])
        self.assertEqual(peer.a_rts_asserts, 0,
                         "the command lane used the bulk lane")

        # The queued/drained transition: with the MCU queue empty the
        # /CTSB doorbell is released, so the console's continuing polls
        # (the boot launcher) issue no further fetch.  A fetch only
        # happens while the doorbell is asserted, which is how the
        # held-doorbell fetches above could happen at all.
        self.assertEqual(peer.ctrl_read() & 0x20, 0,
                         "the doorbell is not released after the drain")
        for _ in range(400000):
            z.step()
        self.assertEqual(peer.hid_requests, [HID_FETCH_SIZE] * fetches,
                         "a fetch happened with the doorbell released")


if __name__ == "__main__":
    unittest.main()
