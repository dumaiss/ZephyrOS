#!/usr/bin/env python3
"""Host-side HID functional tests: replay the real HID acceptance
image (SIO + IOC + HID loaded from ROMFS, production-derived arena) in
the Z80 simulator against the IOC peer's MCU model.

Coverage (all executing the real linked bytes):

* production-style boot: SIO loads first, the IOC transport comes up,
  the HID module discovers IOC by class/capability/interface, and
  `hid.keyboard0` registers with exactly
  ZCAP_HID_TEXT | ZCAP_CHAR_IN | ZCAP_CONSOLE_IN, an input-only
  zchar_ops table and a `zhid_ops` whose read_gamepad stays null;
* console binding: input resolves to the HID handle, output to the
  SIO fallback, and the scripted USB-keyboard pass (a B 3 Enter
  Backspace Ctrl-X Arrow-Up, the MCU translation) is fetched through
  the real CMD_HID_INPUT exchange, echoed as `hid: rx XX` lines and
  closed by the forced SIO fallback byte;
* fetch-on-doorbell only: an idle console emits no IOC command traffic,
  and a fetch happens only while ZIOC_STATUS_HID_PENDING is asserted;
* exact request sizing: every fetch asks for the ring's full free space
  (16) and the reply bytes are dequeued by the peer;
* ring semantics: FIFO order through the wraparound, exact preservation
  of NUL/control bytes and multi-byte VT100 sequences, nonblocking
  getc/read_text, and no commit from a malformed reply (short DATA,
  nonzero status, wrong response class, unknown class, more bytes than
  requested) with the ring unchanged afterwards;
* transport errors become throttled "no input", never a wedged console;
* the proven 64-call stuck-doorbell penalty, cleared by a released
  doorbell or by successful data;
* the eight-byte HID reply collision: a six-byte fetch must not strand
  the IOC bulk admission state;
* the no-IOC image: HID is rolled back (no hid.keyboard0) and the SIO
  fallback carries the console, including the serial byte;
* the malformed-IOC image: the fixture registers an IOC-class device
  with a null command operation; HID rejects the interface, rolls back,
  and leaves the fixture and the SIO fallback untouched.

Skipped when the matching image has not been built.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import Z80, IocCommandPeer, V9958Peer, read_symbols  # noqa: E402

RET = 0x1234
STACK = 0xFFFE
STEP_LIMIT = 40000000

ZDEV_SERIAL = 1
ZDEV_HID = 4
ZDEV_IOC = 5
ZCAP_CHAR_IN = 0x0001
ZCAP_CONSOLE_IN = 0x0004
ZCAP_HID_TEXT = 0x00010000
ZCAP_IOC_COMMAND = 0x01000000
ZCAP_IOC_STATUS = 0x08000000

ZDEV_OK = 0
ZDEV_EINVAL = -2
ZDEV_ENODEV = -3
ZDEV_EAGAIN = -9
ZIOC_OK = 0

CMD_LINK_SYNC = 0x0C
CMD_PING = 0x01
CMD_HID_INPUT = 0x0E
RSP_PING = 0x81

# The scripted USB-keyboard pass (the pinned MCU's translation):
# a, B, 3, Enter, Backspace, Ctrl-X, Arrow-Up (ESC [ A).
HID_TYPED = bytes((0x61, 0x42, 0x33, 0x0D, 0x08, 0x18, 0x1B, 0x5B, 0x41))
HID_HOST_SYNC = b"\xaa\x55"

# Host-owned scratch in the currently selected RAM bank: read_text
# destination and native IOC request/reply frames.
HID_BUF = 0x1C00
REQ = 0x1E00
RSP = 0x1E60


class HidSimBase(unittest.TestCase):
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
        # The HID image boots the full production stack, including
        # the V9958 video core; the peer absorbs its A0h-A4h bring-up.
        z.vdp_peer = V9958Peer()
        return z

    def skip_delay(self, z):
        if z.pc == self.syms["_kernel_serial_connect_delay"]:
            z.pc = z.pop16()

    def crt0(self, z):
        z.push16(RET)
        z.pc = self.syms["__zos_crt0_init"]
        for _ in range(2000000):
            z.step()
            if z.pc == RET:
                return
        raise AssertionError("C runtime init did not return")

    def start_kernel(self, z):
        self.crt0(z)
        z.push16(RET)
        z.sio_rx.extend(HID_HOST_SYNC)
        z.pc = self.syms["_kernel_main"]

    def step_to(self, z, marker, on_tx=None, limit=STEP_LIMIT):
        for _ in range(limit):
            z.step()
            self.skip_delay(z)
            if on_tx is not None:
                on_tx(z.sio_tx)
            if marker in z.sio_tx:
                return bytes(z.sio_tx)
        raise AssertionError("did not reach %r (PC=%04X, tail=%r)"
                             % (marker, z.pc, bytes(z.sio_tx)[-160:]))

    def boot_ready(self, peer):
        """Boot to the HID input prompt (target then spins waiting)."""
        z = self.make_cpu(peer)
        self.start_kernel(z)
        self.step_to(z, b"hid: ready for input")
        return z

    def boot_fallback(self, marker=b"hid selftest: OK"):
        """Boot an image whose only input phase is the SIO fallback.

        One extra host-sync pair is left in the receiver (the smoke tool
        repeats AAh 55h until the banner is visible); the fallback phase
        must discard that documented residue before testing the invited
        byte."""
        z = self.make_cpu()
        self.start_kernel(z)
        z.sio_rx.extend(b"\xaa\x55")
        done = {"r": False}

        def on_tx(tx):
            if not done["r"] and b"hid: sio fallback, send R" in tx:
                z.sio_rx.append(ord("R"))
                done["r"] = True

        self.step_to(z, marker, on_tx=on_tx)
        return z

    # -- scratch and calling conventions --------------------------------
    def window(self, z):
        return z._sram(z.latch & 0x07)

    def read_buf(self, z, length):
        return bytes(self.window(z)[HID_BUF:HID_BUF + length])

    def call(self, z, target, hl, de, third=None, limit=STEP_LIMIT):
        """sdcccall(1) call: first 16-bit arg HL, second DE, third on the
        stack, 16-bit result in DE (SDCC 4.5 z80)."""
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
        raise AssertionError("call at %04X did not return" % target)

    # -- registry -------------------------------------------------------
    def devices(self, z):
        base = self.syms["_device_table"]
        out = []
        for index in range(16):
            off = base + index * 16
            if z.bank7[off] == 0:
                continue
            out.append({
                "handle": z.bank7[off],
                "class": z.bank7[off + 1],
                "caps": (z.bank7[off + 2] | (z.bank7[off + 3] << 8) |
                         (z.bank7[off + 4] << 16) |
                         (z.bank7[off + 5] << 24)),
                "name": z.bank7[off + 6] | (z.bank7[off + 7] << 8),
                "char_ops": z.bank7[off + 8] | (z.bank7[off + 9] << 8),
                "class_ops": z.bank7[off + 10] | (z.bank7[off + 11] << 8),
                "state": z.bank7[off + 12] | (z.bank7[off + 13] << 8),
            })
        return out

    def device_by_class(self, z, class_id):
        found = [dev for dev in self.devices(z) if dev["class"] == class_id]
        return found[0] if found else None

    def cstr(self, z, addr):
        out = bytearray()
        while z.rd(addr) != 0:
            out.append(z.rd(addr))
            addr = (addr + 1) & 0xFFFF
        return bytes(out)

    # -- HID operation wrappers -----------------------------------------
    def hid_read_text(self, z, dev, max_len):
        return self.call(z, z.rd16(dev["class_ops"]), dev["state"], HID_BUF,
                         third=max_len)

    def hid_poll(self, z, dev):
        return self.call(z, z.rd16(dev["char_ops"] + 4), dev["state"], 0)

    def hid_getc(self, z, dev):
        return self.call(z, z.rd16(dev["char_ops"] + 6), dev["state"], 0)

    # -- IOC operation wrapper ------------------------------------------
    def ioc_command(self, z, dev, rtype, data=b""):
        window = self.window(z)
        window[REQ] = rtype
        window[REQ + 1] = 0
        window[REQ + 2] = len(data)
        for i, byte in enumerate(data[:26]):
            window[REQ + 3 + i] = byte
        return self.call(z, z.rd16(dev["class_ops"]), dev["state"], REQ,
                         third=RSP)


class TestHidBoot(HidSimBase):
    def test_registration_binding_and_transcript(self):
        peer = IocCommandPeer()
        # Stop at the input prompt: the target has verified its binding
        # but has not yet forced the SIO fallback phase.
        z = self.boot_ready(peer)
        tx = bytes(z.sio_tx)

        # Registration: exactly the three text/input capabilities.
        hid = self.device_by_class(z, ZDEV_HID)
        sio = self.device_by_class(z, ZDEV_SERIAL)
        self.assertIsNotNone(hid, "hid.keyboard0 is not registered")
        self.assertIsNotNone(sio, "sio0.b is not registered")
        self.assertEqual(self.cstr(z, hid["name"]), b"hid.keyboard0")
        self.assertEqual(hid["class"], ZDEV_HID)
        self.assertEqual(hid["caps"],
                         ZCAP_HID_TEXT | ZCAP_CHAR_IN | ZCAP_CONSOLE_IN)
        # Class interface shape: read_text present, read_gamepad null.
        self.assertNotEqual(z.rd16(hid["class_ops"]), 0)
        self.assertEqual(z.rd16(hid["class_ops"] + 2), 0,
                         "read_gamepad must stay null for the text-only device")
        # Input-only character table.
        self.assertEqual(z.rd16(hid["char_ops"]), 0, "putc must be null")
        self.assertEqual(z.rd16(hid["char_ops"] + 2), 0, "write must be null")
        self.assertNotEqual(z.rd16(hid["char_ops"] + 4), 0, "poll missing")
        self.assertNotEqual(z.rd16(hid["char_ops"] + 6), 0, "getc missing")

        # Console binding: HID input preferred, SIO output fallback.
        in_handle = z.rd(self.syms["_console_in_handle"])
        out_handle = z.rd(self.syms["_console_out_handle"])
        self.assertEqual(in_handle, hid["handle"],
                         "console input did not prefer the HID device")
        self.assertEqual(out_handle, sio["handle"],
                         "console output did not stay on the SIO fallback")

        # Complete the scripted pass: typing, then the invited fallback
        # byte.  A stale host-sync pair is left in the receiver (as the
        # smoke tool's repeated banner nudge does on hardware); the
        # fallback phase must discard it before the invited byte.
        peer.hid_queue.extend(HID_TYPED)
        z.sio_rx.extend(b"\xaa\x55")
        done = {"r": False}

        def on_tx(tx):
            if not done["r"] and b"hid: sio fallback, send R" in tx:
                z.sio_rx.append(ord("R"))
                done["r"] = True

        tx = bytes(self.step_to(z, b"hid selftest: OK", on_tx=on_tx))
        expected = b"".join(b"hid: rx %02X\r\n" % byte
                            for byte in HID_TYPED)
        self.assertIn(expected, tx)
        self.assertIn(b"hid: typed pass OK\r\n", tx)
        self.assertIn(b"hid: sio fallback OK\r\n", tx)
        self.assertIn(b"hid selftest: OK", tx)
        self.assertNotIn(b"hid selftest: FAIL", tx)
        self.assertFalse(z.sio_rx, "the fallback byte was not consumed")

        # One fetch carried all nine bytes; the peer dequeued them.
        self.assertEqual(peer.hid_requests, [16])
        self.assertEqual(peer.hid_delivered, 9)
        self.assertEqual(list(peer.hid_queue), [])
        hid_reqs = [req for req in peer.requests if req[0] == CMD_HID_INPUT]
        self.assertEqual(len(hid_reqs), 1)
        self.assertEqual(hid_reqs[0][0], CMD_HID_INPUT)
        self.assertEqual(hid_reqs[0][2], 0)
        self.assertEqual(hid_reqs[0][3], bytes([16]))
        other = [req for req in peer.requests if req[0] != CMD_HID_INPUT]
        self.assertEqual([req[0] for req in other], [CMD_LINK_SYNC])
        # No bulk-lane activity at all in the input path.
        self.assertEqual(peer.a_rts_asserts, 0)

    def test_idle_console_polling_emits_no_ioc_command_traffic(self):
        peer = IocCommandPeer()
        z = self.boot_ready(peer)
        self.assertEqual(peer.hid_requests, [],
                         "the boot prompt itself issued a fetch")
        baseline = len(peer.requests)
        for _ in range(200000):
            z.step()
            self.skip_delay(z)
        self.assertEqual(peer.hid_requests, [],
                         "idle console polling fetched from the IOC")
        self.assertEqual(len(peer.requests), baseline,
                         "idle console polling emitted command traffic")
        # Complete the run afterwards to prove the link still works.
        peer.hid_queue.extend(HID_TYPED)
        done = {"r": False}

        def on_tx(tx):
            if not done["r"] and b"hid: sio fallback, send R" in tx:
                z.sio_rx.append(ord("R"))
                done["r"] = True

        tx = self.step_to(z, b"hid selftest: OK", on_tx=on_tx)
        self.assertIn(b"hid: typed pass OK\r\n", tx)


class TestHidFetchBehavior(HidSimBase):
    def test_fetch_on_doorbell_only_and_exact_sizing(self):
        peer = IocCommandPeer()
        z = self.boot_ready(peer)
        hid = self.device_by_class(z, ZDEV_HID)
        self.assertIsNotNone(hid)

        # Buffered data: poll reports it without any IOC command.
        peer.hid_queue.extend(bytes(range(20)))
        self.assertEqual(self.hid_poll(z, hid), 1)
        self.assertEqual(peer.hid_requests, [16])
        self.assertEqual(peer.hid_delivered, 16)
        self.assertEqual(self.hid_poll(z, hid), 1)
        self.assertEqual(peer.hid_requests, [16],
                         "poll fetched while the ring was non-empty")
        # Drain the first 16 bytes in exact FIFO order.
        for expected in range(16):
            self.assertEqual(self.hid_getc(z, hid), expected)
        # getc is nonblocking but still consults the doorbell: the four
        # remaining bytes are fetched (full free space again) and returned.
        self.assertEqual(self.hid_getc(z, hid), 16)
        self.assertEqual(peer.hid_requests, [16, 16])
        for expected in (17, 18, 19):
            self.assertEqual(self.hid_getc(z, hid), expected)
        # Empty queue: the released doorbell means no more commands.
        self.assertEqual(self.hid_getc(z, hid), ZDEV_EAGAIN)
        self.assertEqual(peer.hid_requests, [16, 16])
        # Every request asked for exactly the ring's free space (16) and
        # stayed below the protocol's 24-byte maximum.
        for wanted in peer.hid_requests:
            self.assertEqual(wanted, 16)

    def test_ring_wraparound_order(self):
        peer = IocCommandPeer()
        z = self.boot_ready(peer)
        hid = self.device_by_class(z, ZDEV_HID)
        batch1 = bytes(range(10))
        batch2 = bytes(0xA0 + i for i in range(10))
        peer.hid_queue.extend(batch1)
        self.assertEqual(self.hid_poll(z, hid), 1)
        self.assertEqual(self.hid_read_text(z, hid, 32), 10)
        self.assertEqual(self.read_buf(z, 10), batch1)
        self.assertEqual(self.hid_read_text(z, hid, 32), 0)
        # The second batch wraps the produce index across the ring end.
        peer.hid_queue.extend(batch2)
        self.assertEqual(self.hid_poll(z, hid), 1)
        self.assertEqual(self.hid_read_text(z, hid, 32), 10)
        self.assertEqual(self.read_buf(z, 10), batch2)
        self.assertEqual(peer.hid_requests, [16, 16])
        self.assertEqual(peer.hid_delivered, 20)

    def test_controls_and_escape_sequences_preserved_exactly(self):
        peer = IocCommandPeer()
        z = self.boot_ready(peer)
        hid = self.device_by_class(z, ZDEV_HID)
        script = bytes((0x00, 0x08, 0x0D, 0x18, 0x1B, 0x5B, 0x41,
                        0x7F, 0xFF))
        peer.hid_queue.extend(script)
        self.assertEqual(self.hid_poll(z, hid), 1)
        self.assertEqual(self.hid_read_text(z, hid, 9), 9)
        self.assertEqual(self.read_buf(z, 9), script)
        self.assertEqual(self.hid_getc(z, hid), ZDEV_EAGAIN)
        # No capacity overrun: a max_len smaller than the ring still
        # returns only what was asked for.
        peer.hid_queue.extend(b"ABCDE")
        self.assertEqual(self.hid_poll(z, hid), 1)
        self.assertEqual(self.hid_read_text(z, hid, 2), 2)
        self.assertEqual(self.read_buf(z, 2), b"AB")
        self.assertEqual(self.hid_getc(z, hid), ord("C"))

    def test_idle_results(self):
        peer = IocCommandPeer()
        z = self.boot_ready(peer)
        hid = self.device_by_class(z, ZDEV_HID)
        for _ in range(3):
            self.assertEqual(self.hid_poll(z, hid), 0)
        self.assertEqual(self.hid_getc(z, hid), ZDEV_EAGAIN)
        self.assertEqual(self.hid_read_text(z, hid, 16), 0)
        self.assertEqual(peer.hid_requests, [],
                         "no fetch may happen without the doorbell")
        # Malformed arguments fail predictably.
        self.assertEqual(self.hid_read_text(z, hid, 0), ZDEV_EINVAL)
        self.assertEqual(
            self.call(z, z.rd16(hid["class_ops"]), hid["state"], 0x0000,
                      third=4), ZDEV_EINVAL)


    def test_unexpected_pre_invitation_byte_still_fails(self):
        # Only the documented host-sync residue (AAh 55h) is discarded;
        # any other byte read before the invitation is a failure with
        # that byte as evidence.
        peer = IocCommandPeer()
        z = self.boot_ready(peer)
        peer.hid_queue.extend(HID_TYPED)
        z.sio_rx.append(ord("X"))
        tx = bytes(self.step_to(
            z, b"hid selftest: FAIL 0x07 detail 0x58"))
        self.assertIn(b"hid: typed pass OK\r\n", tx)
        self.assertIn(b"hid selftest: FAIL 0x07 detail 0x58", tx)


class TestHidFailurePolicy(HidSimBase):
    def _clear_penalty(self, z, peer, hid):
        peer.hid_queue.clear()
        self.assertEqual(self.hid_poll(z, hid), 0)

    def test_malformed_replies_leave_the_ring_unchanged(self):
        peer = IocCommandPeer()
        z = self.boot_ready(peer)
        hid = self.device_by_class(z, ZDEV_HID)
        for fault in ("short", "status", "wrong_type", "unknown",
                      "over_count"):
            peer.hid_queue.clear()
            self.assertEqual(self.hid_poll(z, hid), 0)
            peer.hid_queue.extend(b"\x01\x02\x03")
            peer.hid_faults.append(fault)
            before = len(peer.hid_requests)
            self.assertEqual(self.hid_poll(z, hid), 0,
                             "malformed reply accepted (%s)" % fault)
            self.assertEqual(len(peer.hid_requests), before + 1,
                             "no fetch attempted (%s)" % fault)
            self.assertEqual(self.hid_getc(z, hid), ZDEV_EAGAIN,
                             "ring changed after malformed reply (%s)"
                             % fault)
            if fault != "over_count":
                # The peer only dequeues bytes on a well-formed reply.
                self.assertEqual(len(peer.hid_queue), 3, fault)
            self._clear_penalty(z, peer, hid)

    def test_transport_error_is_throttled_then_recovers(self):
        peer = IocCommandPeer()
        z = self.boot_ready(peer)
        hid = self.device_by_class(z, ZDEV_HID)
        peer.hid_queue.extend(b"\x21\x22")
        peer.stall = True
        before = len(peer.requests)
        self.assertEqual(self.hid_poll(z, hid), 0)
        self.assertEqual(len(peer.requests), before + 1)
        self.assertEqual(self.hid_getc(z, hid), ZDEV_EAGAIN)
        peer.stall = False
        self._clear_penalty(z, peer, hid)
        peer.hid_queue.extend(b"\x21\x22")
        self.assertEqual(self.hid_poll(z, hid), 1)
        self.assertEqual(self.hid_getc(z, hid), 0x21)
        self.assertEqual(self.hid_getc(z, hid), 0x22)
        self.assertEqual(self.hid_getc(z, hid), ZDEV_EAGAIN)

    def test_stuck_doorbell_penalty_is_sixty_four_calls(self):
        peer = IocCommandPeer()
        z = self.boot_ready(peer)
        hid = self.device_by_class(z, ZDEV_HID)
        # Asserted doorbell with nothing delivered: one fetch, penalty.
        peer.doorbell = True
        base = len(peer.hid_requests)
        self.assertEqual(self.hid_poll(z, hid), 0)
        self.assertEqual(len(peer.hid_requests), base + 1)
        for _ in range(64):
            self.assertEqual(self.hid_poll(z, hid), 0)
        self.assertEqual(len(peer.hid_requests), base + 1,
                         "the 64-call penalty did not suppress the fetch")
        self.assertEqual(self.hid_poll(z, hid), 0)
        self.assertEqual(len(peer.hid_requests), base + 2,
                         "the fetch was not retried after the penalty")
        # A released doorbell clears the penalty without a command.
        peer.doorbell = False
        self.assertEqual(self.hid_poll(z, hid), 0)
        self.assertEqual(len(peer.hid_requests), base + 2)
        # Data is fetched immediately: the release cleared the penalty.
        peer.doorbell = True
        peer.hid_queue.append(0x55)
        self.assertEqual(self.hid_poll(z, hid), 1)
        self.assertEqual(len(peer.hid_requests), base + 3)
        self.assertEqual(self.hid_getc(z, hid), 0x55)

    def test_six_byte_reply_does_not_admit_bulk(self):
        peer = IocCommandPeer()
        z = self.boot_ready(peer)
        hid = self.device_by_class(z, ZDEV_HID)
        ioc = self.device_by_class(z, ZDEV_IOC)
        # Six translated bytes make the reply DATA exactly eight bytes --
        # the READY shape.  CMD_HID_INPUT cannot arm the bulk lane.
        peer.hid_queue.extend(b"\x01\x02\x03\x04\x05\x06")
        self.assertEqual(self.hid_poll(z, hid), 1)
        self.assertEqual(self.ioc_command(z, ioc, CMD_PING,
                                          bytes(range(16))), ZIOC_OK)
        window = self.window(z)
        self.assertEqual(window[RSP], RSP_PING)
        self.assertEqual(peer.a_rts_asserts, 0)
        for expected in range(1, 7):
            self.assertEqual(self.hid_getc(z, hid), expected)


class TestHidNoIocImage(HidSimBase):
    IMAGE_DIR = "build-hid-noioc-selftest"
    IMAGE_NAME = "zephyros-hid-noioc-selftest.bin"

    def test_rollback_and_sio_fallback(self):
        z = self.boot_fallback()
        tx = bytes(z.sio_tx)
        self.assertIn(b"hid: unavailable 0x0B\r\n", tx)
        self.assertIn(b"hid: hid absent after rollback\r\n", tx)
        self.assertIn(b"hid selftest: OK", tx)
        self.assertNotIn(b"hid selftest: FAIL", tx)

        self.assertIsNone(self.device_by_class(z, ZDEV_HID),
                          "HID registered without an IOC dependency")
        self.assertIsNone(self.device_by_class(z, ZDEV_IOC),
                          "a device was published where IOC is absent")
        sio = self.device_by_class(z, ZDEV_SERIAL)
        self.assertIsNotNone(sio)
        self.assertEqual(z.rd(self.syms["_console_in_handle"]),
                         sio["handle"],
                         "input did not fall back to SIO")
        self.assertEqual(z.rd(self.syms["_console_out_handle"]),
                         sio["handle"],
                         "output did not fall back to SIO")
        self.assertFalse(z.sio_rx, "the fallback byte was not consumed")


class TestHidBadIocImage(HidSimBase):
    IMAGE_DIR = "build-hid-badioc-selftest"
    IMAGE_NAME = "zephyros-hid-badioc-selftest.bin"

    def test_interface_rejection_and_rollback(self):
        z = self.boot_fallback()
        tx = bytes(z.sio_tx)
        self.assertIn(b"hid: unavailable 0x0B\r\n", tx)
        self.assertIn(b"hid: malformed ioc rejected\r\n", tx)
        self.assertIn(b"hid selftest: OK", tx)
        self.assertNotIn(b"hid selftest: FAIL", tx)

        # The malformed dependency was found (discovery worked) and is
        # still registered after HID's rollback.
        ioc = self.device_by_class(z, ZDEV_IOC)
        self.assertIsNotNone(ioc, "the fixture IOC device disappeared")
        self.assertEqual(self.cstr(z, ioc["name"]), b"fake.ioc")
        self.assertEqual(ioc["caps"], ZCAP_IOC_COMMAND | ZCAP_IOC_STATUS)
        self.assertEqual(z.rd16(ioc["class_ops"]), 0,
                         "fixture command must be null")
        self.assertIsNone(self.device_by_class(z, ZDEV_HID),
                          "HID registered on a malformed IOC interface")
        sio = self.device_by_class(z, ZDEV_SERIAL)
        self.assertEqual(z.rd(self.syms["_console_in_handle"]),
                         sio["handle"])
        self.assertEqual(z.rd(self.syms["_console_out_handle"]),
                         sio["handle"])
        self.assertFalse(z.sio_rx, "the fallback byte was not consumed")


if __name__ == "__main__":
    unittest.main()
