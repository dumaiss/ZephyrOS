#!/usr/bin/env python3
"""Host-side M8 IOC command+bulk functional tests: replay the real M8
acceptance image (SIO + IOC loaded from ROMFS, production-derived arena)
in the Z80 simulator against the IOC command+bulk peer.

Coverage (all executing the real linked bytes):

* production-style boot: SIO fallback loads, the IOC module programs
  SIO1/B and SIO1/A once, the bounded LINK_SYNC bring-up succeeds only on
  a CRC-verified matching reply, `ioc.0` registers with all four
  capabilities, and the M8 target diagnostic runs the complete
  READY/DONE bulk acceptance over the /SHARED service (write
  1/128/512-byte sections with matching DONE records, read them back,
  verify, delete) and reports the pinned controller level (4Ah) and host
  contract level (08h) through the early SIO;
* exact command wire requests: preload-before-RTS (no underrun fill), the
  rolling sequence, the common packet bytes and CRC, the two FFh
  trailers, RTS asserted once per exchange and released every time,
  exactly one command-lane reset, and Enter Hunt used once for the whole
  boot;
* exact bulk wire behavior: AAh WR6/WR7 fill, the TX CRC generator armed
  before the sacrificial 00h lead-in, marker plus bound header, OUTI
  payload, the terminating underrun EOM reset, the measured CRC-out
  guard, RTS release and CTS deassertion; and on reads the bounded drain
  of persistent pipeline residue, DCD admission, bounded marker scan,
  wire-speed header capture, INI payload, the two bypassed trailer bytes,
  RTS release and CTS confirmation;
* READY/DONE interlock: invalid sequencing fails before any lane I/O,
  commands are rejected while a bulk phase is admitted, only
  CMD_XFER_STATUS is accepted while a write awaits DONE, and a failed or
  mismatched DONE is never reported as a completed write;
* fault injection on both directions (header length/type/sequence/
  status, stall, no DCD, no CTS release/deassertion, controller DONE
  failure, host underrun), whole-link recovery before normal traffic,
  exact memory canaries, lane state, RTS and exact entry IFF restoration.

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
ZIOC_EIO = -0x0A

ZIOC_STATUS_HID_PENDING = 0x0001

CMD_LINK_SYNC = 0x0C
CMD_PING = 0x01
RSP_PING = 0x81

REQ = 0x1E00
RSP = 0x1E60

# Bulk acceptance transfer buffer in the selected (bank 1) window, with
# canary regions on both sides.
BULK_BUF = 0x1000
BULK_CANARY_LO = 0x0E00
BULK_CANARY_HI = 0x1200

OK_LINE = b"m8 ioc selftest: OK"
FAIL_LINE = b"m8 ioc selftest: FAIL"
PROMPT = b"Z80> "

M8IOC_NAME = "M8IOCTSTBIN"

# Host sync the smoke image waits for before the banner (kernel/main.c
# kernel_host_sync_wait; tools/serial_smoke.py sends the same sequence).
M8IOC_HOST_SYNC = b"\xaa\x55"
CMD_FS_OPEN = 0x22
RSP_FS_OPEN = 0xA2
CMD_FS_READ = 0x23
RSP_FS_READ = 0xA3
CMD_FS_WRITE = 0x24
RSP_FS_WRITE = 0xA4
CMD_FS_CLOSE = 0x25
RSP_FS_CLOSE = 0xA5
CMD_FS_DELETE = 0x27
RSP_FS_DELETE = 0xA7
CMD_XFER_STATUS = 0x06
RSP_XFER_STATUS = 0x86
FS_MODE_READ = 0
FS_MODE_WRITE = 1

# Frozen bulk-arming request classes and the direction the pinned firmware
# arms for each (drivers/ioc/ioc_proto.h; confirmed against the pinned
# handlers.c / fs_share.c / fs2.c READY constructions).
M8IOC_READY_CLASSES = {
    0x04: 0, 0x05: 0, 0x08: 0, 0x0F: 0, 0x23: 0, 0x36: 0,
    0x07: 1, 0x09: 1, 0x24: 1, 0x3E: 1,
}
CMD_PROFILE = 0x0B
RSP_PROFILE = 0x8B
CMD_HID_INPUT = 0x0E
RSP_HID_INPUT = 0x8E
RSP_UNKNOWN = 0xFE
ST_UNKNOWN_CMD = 0x02
ST_FS_NOT_MOUNTED = 0x33
ST_FS_NO_FILE = 0x34
ST_FS_RANGE = 0x37
ST_FS_ERROR = 0x38
ST_VOL_UNMOUNTED = 0x30
ZOS_M8IOC_FAIL_BULK_OPEN = 0x07
ZOS_M8IOC_FAIL_BULK_DELETE = 0x0D
ZOS_M8IOC_PENDING = 0x0E


def bulk_pat(index):
    """The target fixture's deterministic section pattern."""
    return ((index * 31) & 0xFF) ^ 0x5A


def bulk_pattern(length):
    return bytes(bulk_pat(i) for i in range(length))


def expected_frame(rtype, seq, status, data=b""):
    """The exact common request wire bytes (without markers/trailers)."""
    length = 3 + len(data)
    body = bytes([length & 0xFF, length >> 8, rtype, seq, status]) + data
    crc = ioc_crc16(body)
    return (b"\xa5\x5a" + body +
            bytes([(crc >> 8) & 0xFF, crc & 0xFF]))


class IocSimBase(unittest.TestCase):
    IMAGE_DIR = "build-m8-ioc-selftest"
    IMAGE_NAME = "zephyros-m8-ioc-selftest.bin"

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
        # The smoke image holds the banner for the host sync sequence; a
        # real host sends it once its port is open (mode m8-ioc in
        # tools/serial_smoke.py).
        z.sio_rx.extend(M8IOC_HOST_SYNC)
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

    def command_data(self, z, ops, state, rtype, data=b"", status=0):
        """command() with the full reply tuple returned as well."""
        req = self.write_request(z, rtype, status, len(data), data)
        fn = z.rd16(ops)
        result = self.call(z, fn, state, req, third=RSP)
        return result, self.read_reply(z)

    def bulk_call(self, z, ops, state, buf, length, write):
        """bulk_read()/bulk_write(): state in HL, buffer in DE, length on
        the stack; the length operand is the third parameter."""
        fn = z.rd16(ops + (4 if write else 2))
        return self.call(z, fn, state, buf, third=length)

    # -- /SHARED acceptance helpers -------------------------------------
    def fs_open(self, z, ops, state, name=M8IOC_NAME, mode=FS_MODE_WRITE):
        data = bytes([mode]) + name.encode("ascii")
        return self.command_data(z, ops, state, CMD_FS_OPEN, data)

    def fs_close(self, z, ops, state, handle, size):
        data = bytes([handle]) + size.to_bytes(4, "little")
        return self.command_data(z, ops, state, CMD_FS_CLOSE, data)

    def fs_delete(self, z, ops, state, name=M8IOC_NAME):
        return self.command_data(z, ops, state, CMD_FS_DELETE,
                                 name.encode("ascii"))

    def fs_xfer(self, z, ops, state, rtype, handle, offset, length):
        data = (bytes([handle]) + offset.to_bytes(4, "little") +
                length.to_bytes(2, "little"))
        return self.command_data(z, ops, state, rtype, data)

    def xfer_status(self, z, ops, state):
        return self.command_data(z, ops, state, CMD_XFER_STATUS)

    def write_section(self, z, ops, state, handle, offset, length):
        """One golden-path /SHARED write with its mandatory DONE trip."""
        result, reply = self.fs_xfer(z, ops, state, CMD_FS_WRITE, handle,
                                     offset, length)
        self.assertEqual(result, ZIOC_OK)
        self.assertEqual(reply[:3], (RSP_FS_WRITE, 0, 8))
        window = z._sram(z.latch & 0x07)
        window[BULK_BUF:BULK_BUF + length] = bulk_pattern(length)
        result = self.bulk_call(z, ops, state, BULK_BUF, length, write=True)
        self.assertEqual(result, ZIOC_OK)
        result, reply = self.xfer_status(z, ops, state)
        self.assertEqual(result, ZIOC_OK)
        self.assertEqual(reply[0], RSP_XFER_STATUS)
        self.assertGreaterEqual(reply[2], 2)
        self.assertEqual(reply[4], 0, "DONE status must report success")
        return reply

    def read_section(self, z, ops, state, handle, offset, length):
        """One golden-path /SHARED read with pattern verification."""
        result, reply = self.fs_xfer(z, ops, state, CMD_FS_READ, handle,
                                     offset, length)
        self.assertEqual(result, ZIOC_OK)
        self.assertEqual(reply[:3], (RSP_FS_READ, 0, 8))
        self.assertEqual(reply[5] | (reply[6] << 8), length)
        result = self.bulk_call(z, ops, state, BULK_BUF, length, write=False)
        self.assertEqual(result, ZIOC_OK)
        window = z._sram(z.latch & 0x07)
        self.assertEqual(bytes(window[BULK_BUF:BULK_BUF + length]),
                         bulk_pattern(length))

    def open_write(self, z, ops, state, name=M8IOC_NAME):
        result, reply = self.fs_open(z, ops, state, name, FS_MODE_WRITE)
        self.assertEqual(result, ZIOC_OK)
        self.assertEqual(reply[0], RSP_FS_OPEN)
        self.assertEqual(reply[1], 0)
        return reply[3]

    def open_read(self, z, ops, state, name=M8IOC_NAME):
        result, reply = self.fs_open(z, ops, state, name, FS_MODE_READ)
        self.assertEqual(result, ZIOC_OK)
        self.assertEqual(reply[0], RSP_FS_OPEN)
        self.assertEqual(reply[1], 0)
        return reply[3]


class TestM8IocBoot(IocSimBase):
    def test_host_sync_gate_holds_banner_until_sequence(self):
        # The smoke image prints nothing until the host sync sequence
        # completes (the bench's reset also resets the USB bridge, so the
        # host must reopen its port before the boot log starts).
        z = self.make_cpu()
        z.push16(RET)
        z.pc = self.syms["__zos_crt0_init"]
        for _ in range(STEP_LIMIT):
            z.step()
            if z.pc == RET:
                break
        else:
            raise AssertionError("C runtime init did not return")
        z.push16(RET)
        z.pc = self.syms["_kernel_main"]
        for _ in range(20000):
            z.step()
        self.assertEqual(bytes(z.sio_tx), b"",
                         "output before the host sync sequence")

        # Neither a stray byte, nor a partial sequence, nor a broken
        # sequence releases the banner.
        z.sio_rx.extend(b"\x00\xaa")
        for _ in range(4000):
            z.step()
        self.assertEqual(bytes(z.sio_tx), b"",
                         "partial host sync released the banner")
        z.sio_rx.extend(b"\x00")
        for _ in range(4000):
            z.step()
        self.assertEqual(bytes(z.sio_tx), b"",
                         "broken host sync released the banner")

        # The complete sequence releases it and is fully consumed.
        z.sio_rx.extend(M8IOC_HOST_SYNC)
        for _ in range(STEP_LIMIT):
            z.step()
            if OK_LINE in bytes(z.sio_tx):
                break
        else:
            raise AssertionError("boot did not finish after the host sync")
        tx = bytes(z.sio_tx)
        self.assertIn(b"ZephyrOS 0.0\r\n", tx)
        self.assertIn(OK_LINE, tx)
        self.assertFalse(z.sio_rx, "host sync sequence not consumed")

    def test_clean_boot_target_selftest_and_wire_discipline(self):
        peer = IocCommandPeer()
        z = self.make_cpu(peer)
        tx = self.boot(z)

        # Boot transcript: SIO fallback, host contract level, controller
        # level, repeated PINGs, the complete bulk write/read/DONE
        # acceptance, semantic status, success.  Nothing from the stale
        # historical banner.
        self.assertIn(b"console: dynamic sio0.b OK\r\n", tx)
        self.assertIn(b"m8 ioc: transport level 08\r\n", tx)
        self.assertIn(b"m8 ioc: controller level 4A\r\n", tx)
        self.assertIn(b"m8 ioc: ping x4 OK\r\n", tx)
        self.assertIn(b"m8 ioc: bulk write 1/128/512 OK\r\n", tx)
        self.assertIn(b"m8 ioc: done x3 OK\r\n", tx)
        self.assertIn(b"m8 ioc: bulk read 1/128/512 OK\r\n", tx)
        self.assertIn(b"m8 ioc: cleanup OK 0x34\r\n", tx)
        self.assertIn(b"m8 ioc: hid status 00\r\n", tx)
        self.assertIn(OK_LINE, tx)
        self.assertNotIn(b"FAIL", tx)
        self.assertNotIn(b"PANIC", tx)
        self.assertNotIn(b"ioc: unavailable", tx)
        self.assertEqual(z.bank7[self.syms["_m8_ioc_selftest_result"]], 0)

        # Registration: all four IOC capabilities in this stage.
        device = self.ioc_device(z)
        self.assertIsNotNone(device, "ioc.0 is not registered")
        caps, ops, state = device
        self.assertEqual(caps, ZCAP_IOC_COMMAND | ZCAP_IOC_BULK_READ |
                         ZCAP_IOC_BULK_WRITE | ZCAP_IOC_STATUS)
        self.assertNotEqual(ops, 0)
        self.assertNotEqual(state, 0)

        # One reset per lane (init only), one command-lane Enter Hunt for
        # the whole boot, one bulk-lane Enter Hunt for the first verified
        # read.  Every command exchange asserts and releases RTS once;
        # every bulk phase does the same on its own lane.
        self.assertEqual(peer.channel_resets, 1)
        self.assertEqual(peer.a_channel_resets, 1)
        self.assertEqual(peer.hunt_writes, 1)
        self.assertEqual(peer.a_hunt_writes, 1)
        self.assertEqual(peer.rts_asserts, len(peer.requests))
        self.assertEqual(peer.rts_asserts, peer.rts_releases)
        self.assertEqual(peer.a_rts_asserts, 6)
        self.assertEqual(peer.a_rts_asserts, peer.a_rts_releases)

        # Exact command requests, in order: LINK_SYNC (seq 1), four PINGs
        # (seq 2..5), then the /SHARED acceptance exchanges.
        self.assertEqual([request[0] for request in peer.requests[:5]],
                         [CMD_LINK_SYNC] + [CMD_PING] * 4)
        self.assertEqual([request[1] for request in peer.requests[:5]],
                         [1, 2, 3, 4, 5])
        self.assertEqual(
            [request[0] for request in peer.requests[5:]],
            [CMD_FS_OPEN,
             CMD_FS_WRITE, CMD_XFER_STATUS,
             CMD_FS_WRITE, CMD_XFER_STATUS,
             CMD_FS_WRITE, CMD_XFER_STATUS,
             CMD_FS_CLOSE,
             CMD_FS_OPEN,
             CMD_FS_READ, CMD_FS_READ, CMD_FS_READ,
             CMD_FS_CLOSE, CMD_FS_DELETE,
             CMD_FS_OPEN])

        # Exact command wire capture for every exchange: the frame starts
        # with the preloaded A5 at offset zero, carries the full packet
        # and CRC, and ends with the two FFh trailers.
        self.assertEqual(len(peer.captures), len(peer.requests))
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

        # Bulk wire discipline: three writes and three reads moved the
        # exact deterministic patterns; the write headers bind to the
        # command class, the read headers to the response class; the two
        # trailer bytes are the bypassed zero CRC; the dedicated file was
        # read back and deleted.
        self.assertEqual([req[0] for req in peer.bulk_requests],
                         [CMD_FS_WRITE] * 3)
        self.assertEqual([req[1] for req in peer.bulk_requests], [7, 9, 11])
        self.assertEqual([req[2] for req in peer.bulk_requests], [0, 0, 0])
        self.assertEqual([len(req[3]) for req in peer.bulk_requests],
                         [1, 128, 512])
        for req, length in zip(peer.bulk_requests, (1, 128, 512)):
            self.assertEqual(req[3], bulk_pattern(length))
        self.assertEqual([len(send) for send in peer.bulk_sends],
                         [9 + n for n in (1, 128, 512)])
        for offset, send in enumerate(peer.bulk_sends):
            self.assertEqual(send[0:2], b"\xa5\x5a")
            self.assertEqual(send[2] | (send[3] << 8),
                             3 + (1, 128, 512)[offset])
            self.assertEqual(send[4], RSP_FS_READ)
            self.assertEqual(send[5], 15 + offset)
            self.assertEqual(send[6], 0)
            self.assertEqual(send[7:7 + (1, 128, 512)[offset]],
                             bulk_pattern((1, 128, 512)[offset]))
            self.assertEqual(send[-2:], b"\x00\x00")
        self.assertNotIn(M8IOC_NAME, peer.fs_files)
        self.assertGreater(peer.last_xfer_id, 0)

    def test_fs_open_unknown_command_reports_pending(self):
        # A firmware that does not serve the writable /SHARED service
        # answers with the unknown-command class; that unique wire
        # evidence may produce the explicit PENDING result - never the OK
        # marker and never a failure.
        peer = IocCommandPeer(fs_open_fault="unknown")
        z = self.make_cpu(peer)
        tx = self.boot(z, stop=b"m8 ioc selftest: PENDING 0xFE\r\n")
        self.assertIn(b"m8 ioc: bulk write PENDING 0xFE\r\n", tx)
        self.assertIn(b"m8 ioc selftest: PENDING 0xFE\r\n", tx)
        self.assertNotIn(OK_LINE, tx)
        self.assertNotIn(b"FAIL", tx)
        self.assertNotIn(b"m8 ioc: cleanup", tx)
        self.assertNotIn(b"m8 ioc: hid status", tx)
        self.assertEqual(
            z.bank7[self.syms["_m8_ioc_selftest_result"]],
            ZOS_M8IOC_PENDING)
        # Nothing was created, nothing was read, and no bulk phase was
        # left admitted or touched.
        self.assertEqual(peer.fs_files, {})
        self.assertEqual(peer.a_rts_asserts, 0)

    def test_fs_open_unknown_status_is_a_failure(self):
        # An unknown-class reply carrying any other status proves nothing
        # about the writable service: it must fail, not report PENDING.
        peer = IocCommandPeer(fs_open_fault="unknown_status")
        z = self.make_cpu(peer)
        tx = self.boot(z, stop=b"m8 ioc selftest: FAIL 0x07 detail 0x38\r\n")
        self.assertIn(b"m8 ioc selftest: FAIL 0x07 detail 0x38\r\n", tx)
        self.assertNotIn(b"PENDING", tx)
        self.assertNotIn(OK_LINE, tx)
        self.assertEqual(
            z.bank7[self.syms["_m8_ioc_selftest_result"]],
            ZOS_M8IOC_FAIL_BULK_OPEN)

    def test_fs_open_media_failure_is_a_failure(self):
        # A real volume/media status on the expected response class is a
        # failure, not PENDING (the frozen status does not prove an
        # unsupported feature).
        peer = IocCommandPeer(fs_open_fault=ST_VOL_UNMOUNTED)
        z = self.make_cpu(peer)
        tx = self.boot(z, stop=b"m8 ioc selftest: FAIL 0x07 detail 0x30\r\n")
        self.assertIn(b"m8 ioc selftest: FAIL 0x07 detail 0x30\r\n", tx)
        self.assertNotIn(b"PENDING", tx)
        self.assertNotIn(OK_LINE, tx)
        self.assertEqual(
            z.bank7[self.syms["_m8_ioc_selftest_result"]],
            ZOS_M8IOC_FAIL_BULK_OPEN)

    def test_fs_open_wrong_class_is_a_failure(self):
        peer = IocCommandPeer(fs_open_fault=("class", RSP_FS_DELETE))
        z = self.make_cpu(peer)
        tx = self.boot(z, stop=b"m8 ioc selftest: FAIL 0x07 detail 0x01\r\n")
        self.assertIn(b"m8 ioc selftest: FAIL 0x07 detail 0x01\r\n", tx)
        self.assertNotIn(b"PENDING", tx)
        self.assertNotIn(OK_LINE, tx)

    def test_cleanup_requires_exact_no_file_failure(self):
        # Only the frozen no-file failure with its empty payload shape
        # proves the delete; a media/mount/filesystem status or a failure
        # reply that still carries data must fail before any cleanup or
        # OK marker.
        for fault in (ST_VOL_UNMOUNTED, ST_FS_NOT_MOUNTED, ST_FS_RANGE,
                      ST_FS_ERROR, (ST_FS_NO_FILE, b"\x07")):
            peer = IocCommandPeer(fs_delete_reopen_fault=fault)
            z = self.make_cpu(peer)
            tx = self.boot(
                z, stop=b"m8 ioc selftest: FAIL 0x0D detail 0x17\r\n")
            self.assertIn(b"m8 ioc selftest: FAIL 0x0D detail 0x17\r\n",
                          tx, "fault %r was not rejected" % (fault,))
            self.assertNotIn(b"m8 ioc: cleanup", tx)
            self.assertNotIn(OK_LINE, tx)
            self.assertNotIn(b"PENDING", tx)
            self.assertEqual(
                z.bank7[self.syms["_m8_ioc_selftest_result"]],
                ZOS_M8IOC_FAIL_BULK_DELETE)

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
        asserts = peer.rts_asserts
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
        self.assertEqual(peer.rts_asserts, asserts + 3)
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


class TestM8IocReadyAdmission(IocSimBase):
    """READY admission is gated by the frozen bulk-arming request class:
    no other successful reply, whatever its eight-byte shape, may change
    the bulk phase."""

    def _boot(self, peer):
        z = self.make_cpu(peer)
        self.boot(z)
        device = self.ioc_device(z)
        self.assertIsNotNone(device, "ioc.0 is not registered")
        caps, ops, state = device
        return z, ops, state

    def _assert_idle_after(self, peer, z, ops, state):
        """A following command still works and both bulk calls fail before
        touching either lane."""
        result, reply = self.command_data(z, ops, state, CMD_PING,
                                          bytes(range(16)))
        self.assertEqual(result, ZIOC_OK)
        self.assertEqual(reply[0], RSP_PING)
        lane = (peer.a_channel_resets, peer.a_rts_asserts,
                bytes(peer.a_wr3_writes))
        result = self.bulk_call(z, ops, state, BULK_BUF, 8, write=False)
        self.assertEqual(result, ZIOC_EBUSY)
        result = self.bulk_call(z, ops, state, BULK_BUF, 8, write=True)
        self.assertEqual(result, ZIOC_EBUSY)
        self.assertEqual(
            (peer.a_channel_resets, peer.a_rts_asserts,
             bytes(peer.a_wr3_writes)), lane,
            "a rejected bulk call touched the bulk lane")

    def test_hid_input_collision_does_not_admit(self):
        # Exact frozen collision: request six bytes while seven are
        # queued -> `queued=1, dropped=0`, then 41h 01h.  The payload
        # prefix satisfies every READY field check (id 1, read direction,
        # length 321) but CMD_HID_INPUT cannot arm the bulk lane.
        peer = IocCommandPeer()
        z, ops, state = self._boot(peer)
        payload = bytes([0x01, 0x00, 0x41, 0x01, 0xAA, 0xBB, 0xCC, 0xDD])
        peer.custom_replies[CMD_HID_INPUT] = (RSP_HID_INPUT, payload)
        result, reply = self.command_data(z, ops, state, CMD_HID_INPUT,
                                          bytes([6]))
        self.assertEqual(result, ZIOC_OK)
        self.assertEqual(reply[:3], (RSP_HID_INPUT, 0, 8))
        self.assertEqual(tuple(reply[3:11]), tuple(payload))
        self._assert_idle_after(peer, z, ops, state)

    def test_profile_bulk_tx_page_does_not_admit(self):
        # CMD_PROFILE page IOC_PROFILE_PAGE_BULK_TX also answers with an
        # eight-byte payload whose prefix can satisfy every READY shape
        # check; it cannot arm the bulk lane either.
        peer = IocCommandPeer()
        z, ops, state = self._boot(peer)
        payload = bytes([0x02, 0x00, 0x41, 0x01, 0x00, 0x00, 0x00, 0x00])
        peer.custom_replies[CMD_PROFILE] = (RSP_PROFILE, payload)
        result, reply = self.command_data(z, ops, state, CMD_PROFILE,
                                          bytes([0x00, 0x01]))
        self.assertEqual(result, ZIOC_OK)
        self.assertEqual(reply[:3], (RSP_PROFILE, 0, 8))
        self._assert_idle_after(peer, z, ops, state)

    def test_every_bulk_arming_class_admits_the_bound_phase(self):
        peer = IocCommandPeer()
        z, ops, state = self._boot(peer)
        for rtype in sorted(M8IOC_READY_CLASSES):
            direction = M8IOC_READY_CLASSES[rtype]
            peer.arbitrary_ready = {rtype: (direction, 64)}
            result, reply = self.command_data(z, ops, state, rtype,
                                              b"\x00\x40")
            self.assertEqual(result, ZIOC_OK,
                             "class %02X did not accept its READY" % rtype)
            self.assertEqual(reply[:3], (rtype | 0x80, 0, 8))
            self.assertEqual(reply[4], direction)
            self.assertEqual(reply[5] | (reply[6] << 8), 64)
            if direction == 0:
                result = self.bulk_call(z, ops, state, BULK_BUF, 64,
                                        write=False)
                self.assertEqual(result, ZIOC_OK)
                window = z._sram(z.latch & 0x07)
                self.assertEqual(
                    bytes(window[BULK_BUF:BULK_BUF + 64]),
                    bytes(((i * 7) & 0xFF) for i in range(64)))
            else:
                window = z._sram(z.latch & 0x07)
                window[BULK_BUF:BULK_BUF + 64] = bytes(
                    (i ^ 0x3C) for i in range(64))
                result = self.bulk_call(z, ops, state, BULK_BUF, 64,
                                        write=True)
                self.assertEqual(result, ZIOC_OK)
                result, reply = self.xfer_status(z, ops, state)
                self.assertEqual(result, ZIOC_OK)
                self.assertEqual(reply[:2], (RSP_XFER_STATUS, 0))
                self.assertEqual(reply[4], 0)

    def test_failed_or_malformed_reply_never_admits(self):
        # A bulk-arming class carrying a real failure status must not
        # admit a phase, even with the correct eight-byte shape...
        peer = IocCommandPeer(arbitrary_ready={0x36: (0, 64, 0x37)})
        z, ops, state = self._boot(peer)
        result, reply = self.command_data(z, ops, state, 0x36, b"\x00\x40")
        self.assertEqual(result, ZIOC_OK)
        self.assertEqual(reply[:2], (0xB6, 0x37))
        self._assert_idle_after(peer, z, ops, state)

        # ... and neither may a corrupted reply to a bulk-arming class.
        peer.fault = "crc"
        result = self.command(z, ops, state, 0x36, b"\x00\x40")
        self.assertEqual(result, ZIOC_EBADCRC)
        peer.fault = None
        self._assert_idle_after(peer, z, ops, state)

    def test_fs_read_without_open_never_admits(self):
        # The real failed-reply path: FS_READ with no open handle.
        peer = IocCommandPeer()
        z, ops, state = self._boot(peer)
        data = (bytes([0x01]) + (0).to_bytes(4, "little") +
                (8).to_bytes(2, "little"))
        result, reply = self.command_data(z, ops, state, CMD_FS_READ, data)
        self.assertEqual(result, ZIOC_OK)
        self.assertEqual(reply[:2], (RSP_FS_READ, 0x36))
        self._assert_idle_after(peer, z, ops, state)


class TestM8IocBulk(IocSimBase):
    """Direct-ops bulk tests against the registered device after the full
    production-style boot (which already ran the golden-path /SHARED
    acceptance: write 1/128/512 with DONE, read back, delete)."""

    def boot_ready(self, peer=None):
        peer = peer if peer is not None else IocCommandPeer()
        z = self.make_cpu(peer)
        self.boot(z)
        device = self.ioc_device(z)
        self.assertIsNotNone(device)
        caps, ops, state = device
        return z, peer, ops, state

    def prepared_read_file(self, z, peer, ops, state, length=128):
        """Create, write and close a section file, then open it for read."""
        handle = self.open_write(z, ops, state)
        self.write_section(z, ops, state, handle, 0, length)
        result, reply = self.fs_close(z, ops, state, handle, length)
        self.assertEqual(result, ZIOC_OK)
        self.assertEqual(reply[0], RSP_FS_CLOSE)
        return self.open_read(z, ops, state)

    def test_ready_done_interlock_and_phase_gating(self):
        z, peer, ops, state = self.boot_ready()

        # No READY admitted: every bulk call fails before any lane I/O.
        asserts = peer.rts_asserts
        a_asserts = peer.a_rts_asserts
        self.assertEqual(self.bulk_call(z, ops, state, BULK_BUF, 128, False),
                         ZIOC_EBUSY)
        self.assertEqual(self.bulk_call(z, ops, state, BULK_BUF, 128, True),
                         ZIOC_EBUSY)
        self.assertEqual(peer.rts_asserts, asserts)
        self.assertEqual(peer.a_rts_asserts, a_asserts)

        handle = self.open_write(z, ops, state)

        # Issue the FS_WRITE that admits a bulk phase.  A READY admits
        # exactly one bulk phase: no command may displace it, and wrong
        # direction/length/null/zero fail without I/O.
        result, reply = self.fs_xfer(z, ops, state, CMD_FS_WRITE,
                                     handle, 0, 128)
        self.assertEqual(result, ZIOC_OK)
        self.assertEqual(reply[:3], (RSP_FS_WRITE, 0, 8))
        asserts = peer.rts_asserts
        self.assertEqual(self.command(z, ops, state, CMD_PING), ZIOC_EBUSY)
        self.assertEqual(peer.rts_asserts, asserts)
        self.assertEqual(self.bulk_call(z, ops, state, BULK_BUF, 128, False),
                         ZIOC_EBUSY)                    # wrong direction
        self.assertEqual(self.bulk_call(z, ops, state, BULK_BUF, 127, True),
                         ZIOC_EBUSY)                    # not admitted length
        self.assertEqual(self.bulk_call(z, ops, state, BULK_BUF, 0, True),
                         ZIOC_EINVAL)
        self.assertEqual(self.bulk_call(z, ops, state, 0, 128, True),
                         ZIOC_EINVAL)
        self.assertEqual(self.bulk_call(z, ops, state, BULK_BUF, 513, True),
                         ZIOC_EINVAL)
        self.assertEqual(peer.a_rts_asserts, a_asserts)

        # Correct phase: the write returns after the lane phase, and the
        # instance then accepts only the DONE query.
        window = z._sram(z.latch & 0x07)
        window[BULK_BUF:BULK_BUF + 128] = bulk_pattern(128)
        z.iff1 = z.iff2 = False
        self.assertEqual(self.bulk_call(z, ops, state, BULK_BUF, 128, True),
                         ZIOC_OK)
        self.assertFalse(z.iff1 or z.iff2,
                         "write did not restore a disabled entry IFF")
        self.assertEqual(self.command(z, ops, state, CMD_PING), ZIOC_EBUSY)
        result, reply = self.xfer_status(z, ops, state)
        self.assertEqual(result, ZIOC_OK)
        self.assertEqual(reply[0], RSP_XFER_STATUS)
        self.assertEqual(reply[3], peer.last_xfer_id)
        self.assertEqual(reply[4], 0)

        # DONE success returns to idle: normal traffic resumes.
        result, reply = self.fs_close(z, ops, state, handle, 128)
        self.assertEqual(result, ZIOC_OK)
        result, reply = self.fs_delete(z, ops, state)
        self.assertEqual(result, ZIOC_OK)

        # A completed read is idle too: DONE is optional for reads.
        handle = self.prepared_read_file(z, peer, ops, state, length=1)
        result, reply = self.fs_xfer(z, ops, state, CMD_FS_READ, handle, 0, 1)
        self.assertEqual(result, ZIOC_OK)
        self.assertEqual(self.bulk_call(z, ops, state, BULK_BUF, 1, False),
                         ZIOC_OK)
        self.assertEqual(self.command(z, ops, state, CMD_PING), ZIOC_OK)
        result, reply = self.fs_close(z, ops, state, handle, 1)
        self.assertEqual(result, ZIOC_OK)
        result, reply = self.fs_delete(z, ops, state)
        self.assertEqual(result, ZIOC_OK)

    def test_read_canaries_and_header_faults(self):
        z, peer, ops, state = self.boot_ready()
        handle = self.prepared_read_file(z, peer, ops, state, length=128)

        # Exact canaries: a 128-byte read may touch the announced region
        # and nothing else, on either side.
        window = z._sram(z.latch & 0x07)
        window[BULK_CANARY_LO:BULK_BUF] = b"\x5a" * (BULK_BUF - BULK_CANARY_LO)
        window[BULK_BUF:BULK_CANARY_HI + 0x200] = \
            b"\xa5" * (BULK_CANARY_HI + 0x200 - BULK_BUF)
        self.read_section(z, ops, state, handle, 0, 128)
        self.assertEqual(bytes(window[BULK_CANARY_LO:BULK_BUF]),
                         b"\x5a" * (BULK_BUF - BULK_CANARY_LO))
        self.assertEqual(bytes(window[BULK_BUF + 128:BULK_CANARY_HI + 0x200]),
                         b"\xa5" * (BULK_CANARY_HI + 0x200 - BULK_BUF - 128))

        # Every header-metadata violation is rejected after the stream is
        # captured; admission unwinds, and the next command performs the
        # bounded whole-link recovery before normal traffic.
        for fault in ("header_len", "header_type", "header_seq",
                      "header_status"):
            result, reply = self.fs_xfer(z, ops, state, CMD_FS_READ,
                                         handle, 0, 128)
            self.assertEqual(result, ZIOC_OK)
            self.assertEqual(reply[0], RSP_FS_READ)
            window[BULK_BUF:BULK_BUF + 128] = b"\xa5" * 128
            peer.read_fault = fault
            result = self.bulk_call(z, ops, state, BULK_BUF, 128, False)
            peer.read_fault = None
            self.assertEqual(result, ZIOC_EBADFRAME, fault)
            self.assertEqual(peer.a_rts_asserts, peer.a_rts_releases,
                             "fault %r left RTS asserted" % fault)
            # No stale READY: the next bulk call fails before the lane.
            self.assertEqual(
                self.bulk_call(z, ops, state, BULK_BUF, 128, False),
                ZIOC_EBUSY, fault)
            hunts = peer.hunt_writes
            result = self.command(z, ops, state, CMD_PING)
            self.assertEqual(result, ZIOC_OK, "recovery failed for %r" % fault)
            self.assertEqual(peer.requests[-2][0], CMD_LINK_SYNC)
            self.assertEqual(peer.requests[-1][0], CMD_PING)
            self.assertEqual(peer.hunt_writes, hunts + 1)

        # Recovery cleared the bulk boundary: the next verified read
        # enters Hunt once and succeeds again.
        hunts = peer.a_hunt_writes
        self.read_section(z, ops, state, handle, 0, 128)
        self.assertEqual(peer.a_hunt_writes, hunts + 1)

    def test_read_transport_faults_cleanup_and_recovery(self):
        for fault, expected in (("stall", ZIOC_ETIMEDOUT),
                                ("no_dcd", ZIOC_ETIMEDOUT),
                                ("no_cts_release", ZIOC_EIO)):
            peer = IocCommandPeer()
            z, peer, ops, state = self.boot_ready(peer)
            handle = self.prepared_read_file(z, peer, ops, state,
                                             length=512)
            result, reply = self.fs_xfer(z, ops, state, CMD_FS_READ,
                                         handle, 0, 512)
            self.assertEqual(result, ZIOC_OK)
            z.iff1 = z.iff2 = True
            peer.read_fault = fault
            result = self.bulk_call(z, ops, state, BULK_BUF, 512, False)
            peer.read_fault = None
            self.assertEqual(result, expected, fault)
            self.assertTrue(z.iff1 and z.iff2,
                            "IFF not restored after %r" % fault)
            self.assertEqual(peer.a_rts_asserts, peer.a_rts_releases,
                             "fault %r left RTS asserted" % fault)
            # A transport fault leaves no stale READY.
            self.assertEqual(
                self.bulk_call(z, ops, state, BULK_BUF, 512, False),
                ZIOC_EBUSY, fault)
            # The next command recovers the whole link and then runs.
            hunts = peer.hunt_writes
            result = self.command(z, ops, state, CMD_PING)
            self.assertEqual(result, ZIOC_OK, "recovery failed for %r" % fault)
            self.assertGreaterEqual(peer.hunt_writes, hunts + 1)

    def test_write_done_and_controller_failure_paths(self):
        z, peer, ops, state = self.boot_ready()
        handle = self.open_write(z, ops, state)
        window = z._sram(z.latch & 0x07)

        # Controller DONE status reporting failure: the write is never
        # reported complete, and recovery is required before traffic.
        result, reply = self.fs_xfer(z, ops, state, CMD_FS_WRITE,
                                     handle, 0, 128)
        self.assertEqual(result, ZIOC_OK)
        window[BULK_BUF:BULK_BUF + 128] = bulk_pattern(128)
        z.iff1 = z.iff2 = True
        self.assertEqual(self.bulk_call(z, ops, state, BULK_BUF, 128, True),
                         ZIOC_OK)
        self.assertTrue(z.iff1 and z.iff2,
                        "write did not restore an enabled entry IFF")
        peer.done_fault = "status"
        result, reply = self.xfer_status(z, ops, state)
        self.assertEqual(result, ZIOC_EIO)
        self.assertEqual(reply[4], 0x20)
        result = self.command(z, ops, state, CMD_PING)
        self.assertEqual(result, ZIOC_OK)
        self.assertEqual(peer.requests[-2][0], CMD_LINK_SYNC)

        # DONE transfer id mismatch is a binding violation, not success.
        result, reply = self.fs_xfer(z, ops, state, CMD_FS_WRITE,
                                     handle, 0, 128)
        self.assertEqual(result, ZIOC_OK)
        window[BULK_BUF:BULK_BUF + 128] = bulk_pattern(128)
        self.assertEqual(self.bulk_call(z, ops, state, BULK_BUF, 128, True),
                         ZIOC_OK)
        peer.done_fault = "id"
        result, reply = self.xfer_status(z, ops, state)
        self.assertEqual(result, ZIOC_EBADFRAME)
        result = self.command(z, ops, state, CMD_PING)
        self.assertEqual(result, ZIOC_OK)

        # Controller-side underrun: the bytes reached the MCU, but the
        # DONE record reports the transfer failed.
        result, reply = self.fs_xfer(z, ops, state, CMD_FS_WRITE,
                                     handle, 0, 128)
        self.assertEqual(result, ZIOC_OK)
        window[BULK_BUF:BULK_BUF + 128] = bulk_pattern(128)
        peer.write_fault = "underrun"
        result = self.bulk_call(z, ops, state, BULK_BUF, 128, True)
        self.assertEqual(result, ZIOC_OK)
        result, reply = self.xfer_status(z, ops, state)
        self.assertEqual(result, ZIOC_EIO)
        self.assertEqual(reply[4], 0x20)
        peer.write_fault = None
        result = self.command(z, ops, state, CMD_PING)
        self.assertEqual(result, ZIOC_OK)

        # /CTSA never deasserts after the phase: a hardware handshake
        # fault, unwound with RTS released and recovery armed.
        result, reply = self.fs_xfer(z, ops, state, CMD_FS_WRITE,
                                     handle, 0, 128)
        self.assertEqual(result, ZIOC_OK)
        window[BULK_BUF:BULK_BUF + 128] = bulk_pattern(128)
        z.iff1 = z.iff2 = False
        peer.write_fault = "no_cts_deassert"
        result = self.bulk_call(z, ops, state, BULK_BUF, 128, True)
        peer.write_fault = None
        self.assertEqual(result, ZIOC_EIO)
        self.assertFalse(z.iff1 or z.iff2)
        self.assertEqual(peer.a_rts_asserts, peer.a_rts_releases)
        self.assertEqual(
            self.bulk_call(z, ops, state, BULK_BUF, 128, True), ZIOC_EBUSY)
        result = self.command(z, ops, state, CMD_PING)
        self.assertEqual(result, ZIOC_OK)

        # Cleanup after all that recovery noise still works.
        result, reply = self.fs_close(z, ops, state, handle, 128)
        self.assertEqual(result, ZIOC_OK)
        result, reply = self.fs_delete(z, ops, state)
        self.assertEqual(result, ZIOC_OK)

    def test_persistent_residue_and_bypassed_trailer(self):
        # Arbitrary trailer bytes: the deployed pair bypasses bulk DATA
        # CRC, so a read is validated on marker/header/admission and the
        # trailer is consumed without ever being compared.
        peer = IocCommandPeer(read_trailer=b"\xde\xad")
        z, peer, ops, state = self.boot_ready(peer)
        handle = self.prepared_read_file(z, peer, ops, state, length=128)

        # The first explicit read leaves the configured persistent
        # pipeline residue behind; the next read's bounded drain removes
        # exactly that residue before the marker scan.
        self.read_section(z, ops, state, handle, 0, 128)
        self.assertEqual(len(peer.a_rx), peer.pipeline_residue)
        peer.a_drain_reads = 0
        self.read_section(z, ops, state, handle, 0, 128)
        self.assertEqual(peer.a_drain_reads, peer.pipeline_residue,
                         "the bounded drain did not remove the residue")
        self.assertEqual(peer.bulk_sends[-1][-2:], b"\xde\xad")
        self.assertEqual(peer.a_hunt_writes, 1,
                         "a healthy read re-entered Hunt")


if __name__ == "__main__":
    unittest.main()
