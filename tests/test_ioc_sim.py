#!/usr/bin/env python3
"""Host-side M8 IOC command-lane functional tests: replay the real M8
acceptance image (SIO + IOC loaded from ROMFS, production-derived arena)
in the Z80 simulator against the IOC command-lane peer.

Coverage (all executing the real linked bytes):

* production-style boot: SIO fallback loads, the IOC module programs
  SIO1/B once, the bounded LINK_SYNC bring-up succeeds only on a
  CRC-verified matching reply, `ioc.0` registers with command/status
  capabilities only, and the M8 target diagnostic reports the pinned
  controller level (4Ah) and host contract level (08h) through the early
  SIO;
* exact wire requests: preload-before-RTS (no underrun fill), the rolling
  sequence, the common packet bytes and CRC, the two FFh trailers, RTS
  asserted once per exchange and released every time, exactly one channel
  reset, and Enter Hunt used once for the whole boot;
* repeated PING exchanges through the registered device, fault injection
  (arbitrary leading bytes, overlap markers, CRC/length/type/sequence
  faults, delayed readiness, delayed reply, stalled clocks), retry and
  recovery without re-initialisation, bounded drains, and exact entry
  IFF restoration;
* held/released HID doorbell sampling through the semantic status only;
* link bring-up faults: transient retries re-arm Hunt per attempt, and a
  four-attempt failure rolls the module back without disturbing the SIO
  console fallback.

Skipped when the M8 image has not been built.
"""

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import Z80, IocCommandPeer, ioc_crc16, read_symbols  # noqa: E402

RET = 0x1234
STACK = 0xFFFE
STEP_LIMIT = 40000000

ZDEV_IOC = 5
ZCAP_IOC_COMMAND = 0x01000000
ZCAP_IOC_STATUS = 0x08000000
ZCAP_IOC_BULK_READ = 0x02000000
ZCAP_IOC_BULK_WRITE = 0x04000000

ZIOC_OK = 0
ZIOC_EINVAL = -2
ZIOC_EBUSY = -4
ZIOC_ETIMEDOUT = -8
ZIOC_EBADFRAME = -0x0D
ZIOC_EBADCRC = -0x0E
ZIOC_EBADSEQ = -0x0F

ZIOC_STATUS_HID_PENDING = 0x0001

CMD_LINK_SYNC = 0x0C
CMD_PING = 0x01
RSP_PING = 0x81

REQ = 0x1E00
RSP = 0x1E60

OK_LINE = b"m8 ioc selftest: OK"
FAIL_LINE = b"m8 ioc selftest: FAIL"
PROMPT = b"Z80> "


def expected_frame(rtype, seq, status, data=b""):
    """The exact common request wire bytes (without markers/trailers)."""
    length = 3 + len(data)
    body = bytes([length & 0xFF, length >> 8, rtype, seq, status]) + data
    crc = ioc_crc16(body)
    return (b"\xa5\x5a" + body +
            bytes([(crc >> 8) & 0xFF, crc & 0xFF]))


class IocSimBase(unittest.TestCase):
    IMAGE_DIR = "build-m8-ioc-command-selftest"
    IMAGE_NAME = "zephyros-m8-ioc-command-selftest.bin"

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
        return z

    def skip_delay(self, z):
        if z.pc == self.syms["_kernel_serial_connect_delay"]:
            z.pc = z.pop16()

    def boot(self, z, stop=OK_LINE, limit=STEP_LIMIT):
        z.push16(RET)
        z.pc = self.syms["__zos_crt0_init"]
        for _ in range(limit):
            z.step()
            if z.pc == RET:
                break
        else:
            raise AssertionError("C runtime init did not return")
        z.push16(RET)
        z.pc = self.syms["_kernel_main"]
        for _ in range(limit):
            z.step()
            self.skip_delay(z)
            tx = bytes(z.sio_tx)
            if stop in tx:
                return tx
        raise AssertionError("boot did not reach %r (PC=%04X, tail=%r)"
                             % (stop, z.pc, bytes(z.sio_tx)[-120:]))

    # -- calling the registered class ops from the host -----------------
    def ioc_device(self, z):
        """Locate the ZDEV_IOC registry entry (packed 16-byte layout)."""
        base = self.syms["_device_table"]
        for index in range(16):
            off = base + index * 16
            if z.bank7[off] == 0:
                continue
            if z.bank7[off + 1] == ZDEV_IOC:
                caps = (z.bank7[off + 2] | (z.bank7[off + 3] << 8) |
                        (z.bank7[off + 4] << 16) | (z.bank7[off + 5] << 24))
                ops = z.bank7[off + 10] | (z.bank7[off + 11] << 8)
                state = z.bank7[off + 12] | (z.bank7[off + 13] << 8)
                return caps, ops, state
        return None

    def write_request(self, z, rtype, status, length, data=b""):
        window = z._sram(z.latch & 0x07)
        window[REQ] = rtype
        window[REQ + 1] = status
        window[REQ + 2] = length
        for i, byte in enumerate(data[:26]):
            window[REQ + 3 + i] = byte
        return REQ

    def read_reply(self, z):
        window = z._sram(z.latch & 0x07)
        return tuple(window[RSP:RSP + 29])

    def call(self, z, target, hl, de, third=None, limit=STEP_LIMIT):
        """sdcccall(1) call as this toolchain emits it: first 16-bit arg
        in HL, second in DE, a third on the stack, and the 16-bit result
        returned in DE (SDCC 4.5 z80).  Returns the signed result."""
        sp_before = z.sp
        if third is not None:
            z.push16(third)
        z.push16(RET)
        z.h = (hl >> 8) & 0xFF
        z.l = hl & 0xFF
        z.d = (de >> 8) & 0xFF
        z.e = de & 0xFF
        z.pc = target
        for _ in range(limit):
            z.step()
            if z.pc == RET:
                z.sp = sp_before
                value = (z.d << 8) | z.e
                return value - 0x10000 if value & 0x8000 else value
        raise AssertionError("ops call at %04X did not return" % target)

    def command(self, z, ops, state, rtype, data=b"", status=0):
        req = self.write_request(z, rtype, status, len(data), data)
        fn = z.rd16(ops)
        result = self.call(z, fn, state, req, third=RSP)
        return result

    def status(self, z, ops, state):
        fn = z.rd16(ops + 6)
        return self.call(z, fn, state, 0)


class TestM8IocBoot(IocSimBase):
    def test_clean_boot_target_selftest_and_wire_discipline(self):
        peer = IocCommandPeer()
        z = self.make_cpu(peer)
        tx = self.boot(z)

        # Boot transcript: SIO fallback, host contract level, controller
        # level, repeated PINGs, semantic status, success.  Nothing from
        # the stale historical banner.
        self.assertIn(b"console: dynamic sio0.b OK\r\n", tx)
        self.assertIn(b"m8 ioc: transport level 08\r\n", tx)
        self.assertIn(b"m8 ioc: controller level 4A\r\n", tx)
        self.assertIn(b"m8 ioc: ping x4 OK\r\n", tx)
        self.assertIn(b"m8 ioc: hid status 00\r\n", tx)
        self.assertIn(OK_LINE, tx)
        self.assertNotIn(b"FAIL", tx)
        self.assertNotIn(b"PANIC", tx)
        self.assertNotIn(b"ioc: unavailable", tx)
        self.assertEqual(z.bank7[self.syms["_m8_ioc_selftest_result"]], 0)

        # Registration: command/status only in this stage.
        device = self.ioc_device(z)
        self.assertIsNotNone(device, "ioc.0 is not registered")
        caps, ops, state = device
        self.assertEqual(caps,
                         ZCAP_IOC_COMMAND | ZCAP_IOC_STATUS)
        self.assertEqual(caps & (ZCAP_IOC_BULK_READ |
                                 ZCAP_IOC_BULK_WRITE), 0)
        self.assertNotEqual(ops, 0)
        self.assertNotEqual(state, 0)

        # One channel reset (init only), one Enter Hunt for the whole
        # boot, one RTS assert/release pair per exchange.
        self.assertEqual(peer.channel_resets, 1)
        self.assertEqual(peer.hunt_writes, 1)
        self.assertEqual(peer.rts_asserts, 5)
        self.assertEqual(peer.rts_releases, 5)

        # Exact requests, in order: LINK_SYNC (seq 1) then four PINGs
        # (seq 2..5) with the echo pattern, each preceded by nothing
        # (preload) and followed by the two FFh trailers.
        self.assertEqual([request[0] for request in peer.requests],
                         [CMD_LINK_SYNC] + [CMD_PING] * 4)
        self.assertEqual([request[1] for request in peer.requests],
                         [1, 2, 3, 4, 5])
        self.assertEqual(len(peer.captures), 5)
        for index, (start, capture) in enumerate(peer.captures):
            self.assertEqual(start, 0,
                             "capture %d did not start with the preloaded "
                             "frame: %r" % (index, capture[:8]))
            rtype = peer.requests[index][0]
            seq = peer.requests[index][1]
            data = peer.requests[index][3]
            frame = expected_frame(rtype, seq, 0, data)
            self.assertEqual(capture[:len(frame)], frame,
                             "capture %d wire bytes differ" % index)
            self.assertEqual(capture[len(frame):], b"\xff\xff",
                             "trailers missing after capture %d" % index)

    def test_transient_link_faults_retry_and_rearm_hunt(self):
        # First LINK_SYNC reply is CRC-faulted, second carries a wrong
        # type, third sequence, then a clean answer: the bring-up must
        # retry, re-arming Hunt each attempt, and then register.
        peer = IocCommandPeer(link_faults=["crc", "type", "seq"])
        z = self.make_cpu(peer)
        tx = self.boot(z)
        self.assertIn(OK_LINE, tx)
        self.assertNotIn(b"ioc: unavailable", tx)
        self.assertEqual(peer.channel_resets, 1)
        self.assertEqual(peer.hunt_writes, 4)   # one per attempt
        self.assertEqual(peer.rts_asserts, peer.rts_releases)
        # Healthy transactions after establishment never hunt again.
        self.assertEqual(peer.wr3_writes.count(0xD1), 4)

    def test_failed_link_bringup_rolls_back_and_keeps_sio(self):
        peer = IocCommandPeer(link_faults=["crc"] * 4)
        z = self.make_cpu(peer)
        tx = self.boot(z, stop=PROMPT)
        # Exactly the bounded four attempts, all failures.
        self.assertEqual(peer.channel_resets, 1)
        self.assertEqual(peer.hunt_writes, 4)
        self.assertEqual(peer.rts_asserts, 4)
        self.assertEqual(peer.hunt_writes,
                         peer.wr3_writes.count(0xD1))
        # The module did not register; the target test reports the exact
        # failure, the kernel keeps running, and the SIO fallback carries
        # the unchanged monitor to its prompt.
        self.assertIsNone(self.ioc_device(z))
        self.assertIn(FAIL_LINE + b" 0x01", tx)
        self.assertIn(b"ioc: unavailable\r\n", tx)
        self.assertIn(b"console: dynamic sio0.b OK", tx)
        self.assertIn(PROMPT, tx)
        self.assertNotIn(b"PANIC", tx)
        self.assertEqual(z.bank7[self.syms["_m8_ioc_selftest_result"]], 1)


class TestM8IocCommands(IocSimBase):
    def boot_ready(self, peer=None):
        peer = peer if peer is not None else IocCommandPeer()
        z = self.make_cpu(peer)
        self.boot(z)
        device = self.ioc_device(z)
        self.assertIsNotNone(device)
        caps, ops, state = device
        return z, peer, ops, state

    def test_repeated_pings_keep_persistent_sync(self):
        z, peer, ops, state = self.boot_ready()
        for _ in range(3):
            result = self.command(z, ops, state, CMD_PING,
                                  bytes((i ^ 0x5A) & 0xFF
                                        for i in range(16)))
            self.assertEqual(result, ZIOC_OK)
            rtype, status, length = self.read_reply(z)[:3]
            self.assertEqual((rtype, status, length), (RSP_PING, 0, 26))
            self.assertEqual(peer.channel_resets, 1,
                             "an ordinary command reset the channel")
        self.assertEqual(peer.hunt_writes, 1,
                         "an ordinary command re-entered Hunt")
        self.assertEqual(peer.rts_asserts, 8)
        self.assertEqual(peer.rts_asserts, peer.rts_releases)
        self.assertTrue(peer.preload_present)
        self.assertEqual(peer.frame_start, 0)

    def test_crc_length_type_sequence_faults_and_recovery(self):
        z, peer, ops, state = self.boot_ready()
        cases = (("crc", ZIOC_EBADCRC), ("len", ZIOC_EBADFRAME),
                 ("type", ZIOC_EBADFRAME), ("seq", ZIOC_EBADSEQ))
        for fault, expected in cases:
            peer.fault = fault
            result = self.command(z, ops, state, CMD_PING)
            self.assertEqual(
                result, expected,
                "fault %r: expected %d, got %d" % (fault, expected, result))
            self.assertEqual(peer.rts_asserts, peer.rts_releases,
                             "fault %r left RTS asserted" % fault)
            self.assertEqual(peer.channel_resets, 1,
                             "fault %r reset the channel" % fault)
            # Recovery: a clean exchange immediately succeeds without any
            # re-initialisation or re-hunt.
            peer.fault = None
            result = self.command(z, ops, state, CMD_PING)
            self.assertEqual(result, ZIOC_OK,
                             "no recovery after fault %r" % fault)
        self.assertEqual(peer.hunt_writes, 1)

    def test_leading_bytes_and_overlap_marker(self):
        # Exactly the FIFO drain limit of stale bytes, with no trailing
        # pipeline byte so the measurement isolates the drain: the
        # bounded drain consumes all four and stops, and the reply is
        # still found.
        peer = IocCommandPeer(leading=(0x00, 0xFF, 0x00, 0xFF),
                              reply_tail=b"")
        z, peer, ops, state = self.boot_ready(peer)
        peer.drain_data_reads = 0
        result = self.command(z, ops, state, CMD_PING,
                              bytes(range(16)))
        self.assertEqual(result, ZIOC_OK)
        self.assertEqual(peer.drain_data_reads, 4,
                         "the drain must stop at the FIFO bound")

        # Overlapping A5 A5 5A and a partial marker ahead of the reply.
        peer.leading = (0x5A, 0xA5, 0xFF, 0xA5, 0xA5)
        result = self.command(z, ops, state, CMD_PING, bytes(range(16)))
        self.assertEqual(result, ZIOC_OK,
                         "scan failed with overlapping leading A5 bytes")
        self.assertEqual(peer.hunt_writes, 1)

    def test_delayed_readiness_and_delayed_reply(self):
        z, peer, ops, state = self.boot_ready()
        # Delayed readiness well inside the budget: the wait must block
        # until /DCDB appears and then complete normally.
        peer.ready_delay = 20000
        result = self.command(z, ops, state, CMD_PING)
        self.assertEqual(result, ZIOC_OK)
        self.assertEqual(peer.ready_delay, 0)

        # A reply that arrives only after many polls is still received.
        peer.reply_delay = 4096
        result = self.command(z, ops, state, CMD_PING)
        self.assertEqual(result, ZIOC_OK)

        # Readiness that never appears: bounded failure, and no request
        # ever reaches the wire.
        asserts_before = peer.rts_asserts
        peer.never_ready = True
        result = self.command(z, ops, state, CMD_PING)
        self.assertEqual(result, ZIOC_ETIMEDOUT)
        self.assertEqual(peer.rts_asserts, asserts_before)
        peer.never_ready = False

        # Stalled clocks: the reply never comes; the wait is bounded,
        # RTS is released, and the next exchange recovers.
        peer.stall = True
        result = self.command(z, ops, state, CMD_PING)
        self.assertEqual(result, ZIOC_ETIMEDOUT)
        self.assertEqual(peer.rts_asserts, peer.rts_releases)
        peer.stall = False
        result = self.command(z, ops, state, CMD_PING)
        self.assertEqual(result, ZIOC_OK)
        self.assertEqual(peer.channel_resets, 1)
        self.assertEqual(peer.hunt_writes, 1)

    def test_input_rejection_without_io(self):
        z, peer, ops, state = self.boot_ready()
        # Oversize native length: rejected without touching the lane.
        writes_before = peer.rts_asserts
        result = self.command(z, ops, state, CMD_PING,
                              b"\x00" * 27, status=0)
        self.assertEqual(result, ZIOC_EINVAL)
        self.assertEqual(peer.rts_asserts, writes_before)
        # Null request/reply: EINVAL as well.
        fn = z.rd16(ops)
        self.assertEqual(self.call(z, fn, state, 0, third=RSP),
                         ZIOC_EINVAL)
        self.assertEqual(self.call(z, fn, state, REQ, third=0),
                         ZIOC_EINVAL)
        self.assertEqual(peer.rts_asserts, writes_before)

    def test_exact_iff_restoration(self):
        z, peer, ops, state = self.boot_ready()
        for enabled in (True, False):
            z.iff1 = z.iff2 = enabled
            result = self.command(z, ops, state, CMD_PING)
            self.assertEqual(result, ZIOC_OK)
            self.assertEqual(z.iff1, enabled,
                             "IFF1 not restored (enabled=%s)" % enabled)
            self.assertEqual(z.iff2, enabled,
                             "IFF2 not restored (enabled=%s)" % enabled)

    def test_held_hid_doorbell_is_semantic_only(self):
        z, peer, ops, state = self.boot_ready()
        # Released doorbell: no bits.
        peer.doorbell = False
        self.assertEqual(self.status(z, ops, state), 0)
        # Held doorbell: exactly the semantic bit, never raw RR0 bits.
        peer.doorbell = True
        for _ in range(4):
            bits = self.status(z, ops, state)
            self.assertEqual(bits, ZIOC_STATUS_HID_PENDING)
            self.assertEqual(bits & ~ZIOC_STATUS_HID_PENDING, 0)
        peer.doorbell = False
        self.assertEqual(self.status(z, ops, state), 0)
        # Sampling never disturbed the lane.
        self.assertEqual(peer.rts_asserts, peer.rts_releases)
        self.assertEqual(peer.channel_resets, 1)


if __name__ == "__main__":
    unittest.main()
