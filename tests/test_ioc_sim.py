#!/usr/bin/env python3
"""Host-side IOC resident-driver acceptance replay (work package 41).

The IOC command/reply and bulk transport live in a bank-6 *resident
driver* driven by the versioned IOC protocol (`abi/kos/protocol.h`,
class `KOS_CLASS_IOC`) over the IPC substrate.  Work package 42d merged
the per-device images: this module replays the IOC diagnostic inside
`build-console-integration-selftest` (the `sio ioc hid v9958
console-v9958-hid` set) against the emulated MCU IOC peer and asserts,
from the outside:

* the early-SIO boot transcript: the kernel banner, the resident driver
  loader's per-image evidence lines for the image's driver set in the
  `drivers.man` order plus its bank-6 ledger tail, and the IOC
  diagnostic's phase lines ending in `ioc selftest: OK`;
* the peer-observable protocol facts: the IOC *driver boot* runs the
  bounded LINK_SYNC bring-up on its own, the diagnostic's CMD_PING and the
  frozen /SHARED bulk acceptance follow it (the image's HID driver may
  interleave its own fetch requests), exactly one command-lane channel
  reset and one Enter Hunt for the whole boot, RTS asserted once and
  released once per exchange, one verified bulk phase in each direction,
  and the fixture file deleted at the end of the bulk acceptance.

The diagnostic is a bank-7 client of the driver: descriptor-less calls
plus staged moves through the common copy gate, so the wire framing, CRC,
sequence and lane admission stay inside the driver - this module only
asserts what the peer can see.

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

from replay import (RET, STACK, STEP_LIMIT, IntegrationInputs,  # noqa: E402
                    Replay, integration_peer)
from z80sim import IocCommandPeer  # noqa: E402

# Host sync the smoke image waits for before the banner (kernel/main.c
# kernel_host_sync_wait; tools/serial_smoke.py sends the same sequence).
HOST_SYNC = b"\xaa\x55"

OK_LINE = b"ioc selftest: OK\r\n"
FAIL_LINE = b"ioc selftest: FAIL"

OK_LINE = b"ioc selftest: OK\r\n"
FAIL_LINE = b"ioc selftest: FAIL"

# Protocol vocabulary the replay decodes out of the peer (the wire the
# driver owns; abi/kos/protocol.h and drivers/ioc/ioc_proto.h).
CMD_LINK_SYNC = 0x0C
CMD_PING = 0x01
RSP_PING = 0x81
CMD_FS_OPEN = 0x22
CMD_FS_READ = 0x23
CMD_FS_WRITE = 0x24
CMD_FS_CLOSE = 0x25
CMD_FS_DELETE = 0x27
CMD_XFER_STATUS = 0x06
RSP_FS_READ = 0xA3
BULK_BYTES = 256

# The diagnostic's acceptance fixture: the dedicated separator-free
# basename the controller builds /SHARED/IOCBULKT.BIN from.
IOC_NAME = "IOCBULKTBIN"

# The diagnostic's curated transcript.  The class/version line is the
# endpoint discovery, the reply line decodes the staged 29-byte native
# frame (type, status, DATA length), the echo line carries the echoed byte
# count, the controller level and the link-synced power snapshot, and the
# last line is the verdict the replay pins.
MARKERS = (
    b"ioc: endpoint class 05 version 01\r\n",
    b"ioc: ready 01\r\n",
    b"ioc: reply 81 status 00 length 1A\r\n",
    b"ioc: echo 10 level 4A power 40\r\n",
    # The merged image runs its diagnostics with the HID stream queued, so
    # the semantic status word carries the doorbell bit (the diagnostic
    # checks that the word is nothing but that bit).
    b"ioc: status 0001\r\n",
    b"ioc: bulk write 0100 OK\r\n",
    b"ioc: bulk read 0100 OK\r\n",
    b"ioc: cleanup OK\r\n",
    b"ioc: malformed refused 05\r\n",
    b"ioc selftest: OK\r\n",
)

# The read-back pattern the diagnostic stages: byte i is i ^ 5Ah.
def ioc_pattern(length):
    return bytes((i ^ 0x5A) & 0xFF for i in range(length))


class IocSimBase(Replay, unittest.TestCase):
    """One boot of the integration image serves every assertion."""

    IMAGE_SUBDIR = "build-console-integration-selftest"
    IMAGE_NAME = "konaos-console-integration-selftest.bin"

    def setUp(self):
        self.load_image()

    def make_cpu(self, peer=None):
        z = super().make_cpu()
        z.ioc_peer = peer if peer is not None else IocCommandPeer()
        return z

    def boot(self, z, stop=OK_LINE, limit=STEP_LIMIT):
        """Start the kernel with the smoke host sync and step until
        `stop` appears (the IOC diagnostic's verdict)."""
        self.start_kernel(z, host_sync=HOST_SYNC)
        self.run_until(z, [stop], limit=limit,
                       on_step=IntegrationInputs(z.ioc_peer))
        return bytes(z.sio_tx)

    def driver_evidence(self, tx):
        """The loader's per-image evidence lines, in transcript order."""
        rows = []
        for line in tx.split(b"\r\n"):
            match = re.match(
                rb"^driver (\S+): hot ([0-9A-F]{4})h"
                rb"( io ([0-9A-F]{4})h)?( boot ([0-9A-F]{4})h)?"
                rb"( bulk ([0-9A-F]{4})h)?$", line)
            if match:
                rows.append((match.group(1).decode("ascii"),
                             match.group(2).decode("ascii")))
        return rows


class TestIocDriverBoot(IocSimBase):
    def test_host_sync_gate_holds_banner_until_sequence(self):
        # The first bytes exist only after the two-byte host sync sequence
        # completes and is fully consumed.
        z = self.make_cpu()
        self.crt0(z)
        z.push16(RET)
        z.pc = self.syms["_kernel_main"]
        for _ in range(20000):
            z.step()
            self.skip_delay(z)
        self.assertEqual(bytes(z.sio_tx), b"", "output before host sync")
        z.sio_rx.extend(b"\x00\xaa")
        for _ in range(4000):
            z.step()
            self.skip_delay(z)
        self.assertEqual(bytes(z.sio_tx), b"", "partial host sync released")
        z.sio_rx.extend(b"\x00")
        for _ in range(4000):
            z.step()
            self.skip_delay(z)
        self.assertEqual(bytes(z.sio_tx), b"", "broken host sync released")
        z.sio_rx.extend(HOST_SYNC)
        self.run_until(z, [OK_LINE],
                       on_step=IntegrationInputs(z.ioc_peer))
        tx = bytes(z.sio_tx)
        self.assertIn(b"KonaOS 0.0\r\n", tx)
        self.assertFalse(z.sio_rx, "host sync sequence not consumed")

    def test_driver_boot_transcript_and_protocol_facts(self):
        peer = integration_peer()
        z = self.make_cpu(peer)
        tx = self.boot(z)

        # --- boot transcript: banner, driver evidence, diagnostic ---
        self.assertIn(b"KonaOS 0.0\r\n", tx)
        for marker in MARKERS:
            self.assertIn(marker, tx)
        self.assertNotIn(FAIL_LINE, tx)
        self.assertNotIn(b"PANIC", tx)
        # The loader's observable evidence lines name the configured
        # driver set in order; the ledger tail counts what it placed.
        manifest = ["sio.mod", "ioc.mod", "hid.mod", "v9958.mod",
                    "console-v9958-hid.mod"]
        evidence = self.driver_evidence(tx)
        self.assertEqual([name for name, _ in evidence], manifest)
        self.assertIn(("driver loader: %d images in bank 6\r\n"
                       % len(manifest)).encode("ascii"), tx)
        self.assertRegex(tx, rb"driver hot: \d+ bytes of 32768\r\n")
        self.assertRegex(tx, rb"driver boot/bulk: \d+ bytes of 24576\r\n")

        # --- peer-observable protocol facts -------------------------
        # The driver's own init ran the bounded LINK_SYNC bring-up, then
        # the diagnostic's PING and the staged /SHARED bulk acceptance
        # followed on the same command lane, in that order.  The image's
        # HID driver interleaves its own fetch requests, so the check is
        # the diagnostic's ordered subsequence.
        kinds = [request[0] for request in peer.requests]
        wanted = [CMD_LINK_SYNC, CMD_PING,
                  CMD_FS_OPEN, CMD_FS_WRITE, CMD_XFER_STATUS,
                  CMD_FS_READ, CMD_FS_CLOSE, CMD_FS_DELETE]
        start = 0
        for kind in wanted:
            self.assertIn(kind, kinds[start:],
                          "request 0x%02X missing or out of order" % kind)
            start = kinds.index(kind, start) + 1
        # Sequence numbers are the driver's own, strictly increasing.
        self.assertEqual([request[1] for request in peer.requests],
                         list(range(1, len(peer.requests) + 1)))

        # Admission setup is the driver's, once for the whole boot: one
        # command-lane channel reset and one Enter Hunt, one RTS assert
        # and release per exchange; the bulk lane resets once, hunts once
        # (the first verified read) and carries exactly two phases.
        self.assertEqual(peer.channel_resets, 1)
        self.assertEqual(peer.hunt_writes, 1)
        self.assertEqual(peer.rts_asserts, len(peer.requests))
        self.assertEqual(peer.rts_asserts, peer.rts_releases)
        self.assertEqual(peer.a_channel_resets, 1)
        self.assertEqual(peer.a_hunt_writes, 1)
        self.assertEqual(peer.a_rts_asserts, 2)
        self.assertEqual(peer.a_rts_asserts, peer.a_rts_releases)

        # The staged bulk write carried the pattern across the wire, and
        # the read-back phase returned the same 256 bytes bound to the
        # FS_READ response class; a clean DONE closed the write.
        written = [request for request in peer.bulk_requests
                   if request[0] == CMD_FS_WRITE]
        self.assertEqual(len(written), 1)
        self.assertEqual(written[0][2], 0)
        self.assertEqual(written[0][3], ioc_pattern(BULK_BYTES))
        self.assertEqual(peer.last_status, 0)
        self.assertEqual(len(peer.bulk_sends), 1)
        send = peer.bulk_sends[0]
        self.assertEqual(send[0:2], b"\xa5\x5a")
        self.assertEqual(send[2] | (send[3] << 8), 3 + BULK_BYTES)
        self.assertEqual(send[4], RSP_FS_READ)
        self.assertEqual(send[7:7 + BULK_BYTES], ioc_pattern(BULK_BYTES))
        self.assertEqual(send[-2:], b"\x00\x00")

        # The delete that cleans the fixture file is the diagnostic's
        # last exchange, and the file is gone from the peer's /SHARED
        # namespace.
        self.assertIn(CMD_FS_DELETE, kinds)
        self.assertNotIn(IOC_NAME, peer.fs_files)


if __name__ == "__main__":
    unittest.main()
