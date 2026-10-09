#!/usr/bin/env python3
"""Host-side IOC resident-service acceptance replay (work package 41).

Package 41 moved the IOC command/reply and bulk transport out of the
nucleus into a bank-6 *resident service* driven by the versioned IOC
protocol (`abi/zos/protocol.h`, class `ZOS_CLASS_IOC`) over the package-39
IPC substrate.  This module replays the real linked acceptance image
(`build-ioc-selftest/zephyros-ioc-selftest.bin`, `make ioc-selftest`) in
the Z80 simulator against the emulated MCU IOC peer and asserts, from the
outside:

* the early-SIO boot transcript: the kernel banner, the resident service
  loader's per-image evidence lines for the image's service set in the
  `services.man` order plus its bank-6 ledger tail, the console
  composition result, and the nucleus diagnostic's phase lines ending in
  `ioc selftest: OK` (the diagnostic records its verdict in
  `ace_ioc_selftest_result`);
* the peer-observable protocol facts: the IOC *service boot* runs the
  bounded LINK_SYNC bring-up on its own, the diagnostic's CMD_PING and the
  frozen /SHARED bulk acceptance follow it, exactly one command-lane
  channel reset and one Enter Hunt for the whole boot, RTS asserted once
  and released once per exchange, one verified bulk phase in each
  direction, and no request after the diagnostic's bulk phase.

The diagnostic is a bank-7 client of the service (the
`kernel/module/ioc_shim.c` pattern): descriptor-less calls plus staged
moves through the common copy gate, so the wire framing, CRC, sequence and
lane admission stay inside the service - this module only asserts what the
peer can see.  The service (not the diagnostic) owns SIO1/B and SIO1/A; the
diagnostic sees native `struct zioc_frame` records.

Skipped when the IOC image has not been built.
"""

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import (Z80, IocCommandPeer, V9958Peer, read_symbols,
                    skip_serial_connect_delay)  # noqa: E402

RET = 0x1234
STACK = 0xFFFE
STEP_LIMIT = 40000000

# Host sync the smoke image waits for before the banner (kernel/main.c
# kernel_host_sync_wait; tools/serial_smoke.py sends the same sequence).
HOST_SYNC = b"\xaa\x55"

OK_LINE = b"ioc selftest: OK"
FAIL_LINE = b"ioc selftest: FAIL"

# Protocol vocabulary the replay decodes out of the peer (the wire the
# service owns; abi/zos/protocol.h and drivers/ioc/ioc_proto.h).
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
    b"ioc: status 0000\r\n",
    b"ioc: bulk write 0100 OK\r\n",
    b"ioc: bulk read 0100 OK\r\n",
    b"ioc: cleanup OK\r\n",
    b"ioc: malformed refused 05\r\n",
    b"ioc selftest: OK\r\n",
)

# The read-back pattern the diagnostic stages: byte i is i ^ 5Ah.
def ioc_pattern(length):
    return bytes((i ^ 0x5A) & 0xFF for i in range(length))


class IocSimBase(unittest.TestCase):
    IMAGE_DIR = "build-ioc-selftest"
    IMAGE_NAME = "zephyros-ioc-selftest.bin"

    def setUp(self):
        self.bin_path = os.path.join(ROOT, self.IMAGE_DIR, self.IMAGE_NAME)
        if not os.path.exists(self.bin_path):
            self.skipTest("%s missing; build the image first" % self.bin_path)
        with open(self.bin_path, "rb") as fh:
            self.image = fh.read()
        self.syms = read_symbols(
            os.path.join(ROOT, self.IMAGE_DIR, "zephyros.map"))

    def make_cpu(self, peer=None):
        pages = {n: self.image[n * 0x10000:(n + 1) * 0x10000]
                 for n in range(8)}
        z = Z80(pages[0], pages[1],
                rom_pages={n: pages[n] for n in range(2, 8)})
        z.latch = 0x18
        z.sp = STACK
        z.ioc_peer = peer if peer is not None else IocCommandPeer()
        z.vdp_peer = V9958Peer()
        return z

    def skip_delay(self, z):
        skip_serial_connect_delay(
            z, self.syms["_kernel_serial_connect_delay"])

    def crt0(self, z):
        z.push16(RET)
        z.pc = self.syms["__zos_crt0_init"]
        for _ in range(2000000):
            z.step()
            if z.pc == RET:
                return
        raise AssertionError("C runtime init did not return")

    def start_kernel(self, z):
        """Reset into the kernel with the host sync the smoke image waits
        for already on the serial input (the bench's reset also resets the
        USB bridge, so the host reopens its port before the boot log)."""
        self.crt0(z)
        z.push16(RET)
        z.sio_rx.extend(HOST_SYNC)
        z.pc = self.syms["_kernel_main"]

    def run_until(self, z, stop=OK_LINE, limit=STEP_LIMIT):
        """Step an already-started kernel until `stop` appears in the
        early-SIO transcript."""
        for _ in range(limit):
            z.step()
            self.skip_delay(z)
            tx = bytes(z.sio_tx)
            if stop in tx:
                return tx
        raise AssertionError("boot did not reach %r (PC=%04X, tail=%r)"
                             % (stop, z.pc, bytes(z.sio_tx)[-160:]))

    def boot(self, z, stop=OK_LINE, limit=STEP_LIMIT):
        """Start the kernel and step until `stop` appears."""
        self.start_kernel(z)
        return self.run_until(z, stop, limit)

    def service_manifest(self):
        """The image's boot manifest: the resident services the loader
        brings up, in load order (services.man, 24-byte name entries)."""
        path = os.path.join(ROOT, self.IMAGE_DIR, "modules", "services.man")
        with open(path, "rb") as fh:
            data = fh.read()
        return [data[at:at + 24].split(b"\x00")[0].decode("ascii")
                for at in range(0, len(data), 24)]

    def service_evidence(self, tx):
        """The loader's per-image evidence lines, in transcript order."""
        rows = []
        for line in tx.split(b"\r\n"):
            match = re.match(
                rb"^service (\S+): hot ([0-9A-F]{4})h"
                rb"( io ([0-9A-F]{4})h)?( boot ([0-9A-F]{4})h)?"
                rb"( bulk ([0-9A-F]{4})h)?$", line)
            if match:
                rows.append((match.group(1).decode("ascii"),
                             match.group(2).decode("ascii")))
        return rows


class TestIocServiceBoot(IocSimBase):
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
        tx = self.run_until(z)
        self.assertIn(b"ZephyrOS 0.0\r\n", tx)
        self.assertFalse(z.sio_rx, "host sync sequence not consumed")

    def test_service_boot_transcript_and_protocol_facts(self):
        peer = IocCommandPeer()
        z = self.make_cpu(peer)
        tx = self.boot(z)

        # --- boot transcript: banner, service evidence, diagnostic ---
        self.assertIn(b"ZephyrOS 0.0\r\n", tx)
        self.assertIn(b"console: service OK\r\n", tx)
        for marker in MARKERS:
            self.assertIn(marker, tx)
        self.assertNotIn(FAIL_LINE, tx)
        self.assertNotIn(b"PANIC", tx)
        self.assertEqual(z.bank7[self.syms["_ace_ioc_selftest_result"]], 0,
                         "the diagnostic recorded a failing phase")

        # The loader's evidence lines name the image's service set in the
        # services.man order; the ledger tail counts what it placed.
        manifest = [name for name in self.service_manifest()
                    if name.endswith(".zmod")]
        self.assertTrue(manifest, "services.man names no service image")
        evidence = self.service_evidence(tx)
        self.assertEqual([name for name, _ in evidence], manifest)
        self.assertIn(("service loader: %d images in bank 6\r\n"
                       % len(manifest)).encode("ascii"), tx)
        self.assertRegex(tx, rb"service hot: \d+ bytes of 32768\r\n")
        self.assertRegex(tx, rb"service boot/bulk: \d+ bytes of 24576\r\n")

        # --- peer-observable protocol facts -------------------------
        # The service's own init ran the bounded LINK_SYNC bring-up, then
        # the diagnostic's PING and the staged /SHARED bulk acceptance
        # followed on the same command lane, in that order and sequence.
        self.assertEqual([request[0] for request in peer.requests],
                         [CMD_LINK_SYNC, CMD_PING,
                          CMD_FS_OPEN, CMD_FS_WRITE, CMD_XFER_STATUS,
                          CMD_FS_READ, CMD_FS_CLOSE, CMD_FS_DELETE])
        self.assertEqual([request[1] for request in peer.requests],
                         list(range(1, len(peer.requests) + 1)))

        # Admission setup is the service's, once for the whole boot: one
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

        # No request after the diagnostic's bulk phase: the delete that
        # cleans the fixture file is the last exchange on the lane, and
        # the file is gone from the peer's /SHARED namespace.
        self.assertEqual(peer.requests[-1][0], CMD_FS_DELETE)
        self.assertNotIn(IOC_NAME, peer.fs_files)


if __name__ == "__main__":
    unittest.main()
