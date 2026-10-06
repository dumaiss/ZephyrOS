#!/usr/bin/env python3
"""Console integration acceptance tests (work package 20): replay the real
production bytes and the integration acceptance image in the Z80
simulator.

The production image (`build/zephyros.bin`) is the same source, object
list and ROMFS recovery store the physical acceptance image is built
from minus the target-test objects: SIO + IOC + HID + V9958 load from
ROMFS in the frozen order, one console_bind_defaults() selects
HID-input / V9958-output, and the unchanged M7 `monitor.zex` launcher
runs.  Coverage, all against those linked bytes:

* boot order, module set, default binding and the early-SIO
  independence of the control transcript;
* the scripted visible monitor session over the HID input lane decoded
  from the V9958 command trace, with two Q exit/relaunch cycles and
  stable launcher counts;
* IOC command/status/repeated-PING and the complete /SHARED
  READY/DONE bulk write/read/delete sequence through the registered
  class operations, plus transient CRC/sequence fault recovery with
  exact wire discipline (RTS balanced, one channel reset, one hunt);
* HID queue traffic through the console service: fetch-on-doorbell,
  FIFO order, malformed-reply atomicity, the 64-call stuck-doorbell
  penalty and recovery;
* malformed/null/class/capability calls with exact codes, and the
  invariants that prove they call no invalid pointer, mutate no device
  table entry, leak no arena granule and leave no IOC/VDP handshake
  active; the monitor then still starts and exits (post-fault
  recovery);
* the integration acceptance image (`build-console-integration-selftest`) target
  transcript and its hand-off to the monitor launcher;
* ROMFS payload identity: the packed monitor/module/font artifacts are
  stored byte-for-byte with the correct media types, and the monitor
  is the pinned M7 oracle.

Skipped when the images / packed artifacts have not been built.
"""

import hashlib
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import Z80, IocCommandPeer, V9958Peer, read_symbols  # noqa: E402
import romfspack  # noqa: E402
from test_console_sim import (  # noqa: E402
    ConsoleSimBase, MONITOR_SIZE, MONITOR_SHA256, PROMPT, BANNER,
    SYSINFO, PROCESS, BANK_INFO, MONITOR_EXIT, ZDEV_HID, ZDEV_VIDEO,
    ZDEV_SERIAL, ZVIDEO_PRESENT, ZDEV_EINVAL)
from test_ioc_sim import (  # noqa: E402
    IocSimBase, CMD_PING, RSP_PING, FS_MODE_READ, FS_MODE_WRITE,
    IOC_NAME, ST_FS_NO_FILE, ZIOC_OK, ZIOC_EINVAL, ZIOC_EBUSY,
    ZIOC_EBADCRC, bulk_pattern, REQ, RSP)

RET = 0x1234
STEP_LIMIT = 80000000
SCRATCH = 0x1F00

ZDEV_IOC = 5
ZDEV_EAGAIN = -9
ZDEV_ENOTSUP = -7
ZDEV_ENODEV = -3

IOC_PING_DATA_LEN = 26
IOC_PING_DATA_LEVEL = 16
IOC_PING_DATA_POWER = 17
IOC_PING_LINK_SYNCED = 0x40
IOC_FW_LEVEL_PINNED = 0x4A

# Integration acceptance image.
CONSOLE_INTEGRATION_DIR = "build-console-integration-selftest"
CONSOLE_INTEGRATION_IMAGE = "zephyros-console-integration-selftest.bin"
SELFTEST_MARKERS = (
    b"console integration: input hid.keyboard0 class 04\r\n",
    b"console integration: output v9958.0 class 02\r\n",
    b"console integration: devices 04\r\n",
    b"console integration: ioc level 4A\r\n",
    b"console integration: rejected calls OK\r\n",
    b"console integration: OK\r\n",
)

FONT_SHA256 = ("6db207e0466a05af42894d378261f876da51bb391240e1beda4f1d5f12ec442f")


class ProductionBase(ConsoleSimBase):
    """Production bytes: no host-sync gate, no target-test objects."""

    IMAGE_SUBDIR = "build"
    IMAGE_NAME = "zephyros.bin"

    def start_kernel(self, z):
        self.crt0(z)
        z.push16(RET)
        z.pc = self.syms["_kernel_main"]

    def boot_to_bound(self, z, limit=STEP_LIMIT):
        """Boot into kernel context right after console_bind_defaults()
        set the output handle, before the monitor launcher starts."""
        self.start_kernel(z)
        for _ in range(limit):
            self.skip_delay(z)
            z.step()
            if z.bank7[self.syms["_console_out_handle"]] != 0:
                self.pump(z)
                return
        raise AssertionError("console was never bound (PC=%04X)" % z.pc)

    def devices_equal(self, a, b):
        return a == b

    def call_uint8(self, z, target, limit=STEP_LIMIT):
        """Call a uint8_t-returning kernel function (SDCC returns it in
        A, unlike the 16-bit DE convention)."""
        sp_before = z.sp
        z.push16(RET)
        z.pc = target
        for _ in range(limit):
            self.skip_delay(z)
            z.step()
            if z.pc == RET:
                z.sp = sp_before
                return z.a
        raise AssertionError("uint8 call at %04X did not return" % target)

    def granules(self, z):
        return self.call_uint8(z, self.syms["_module_granule_count"])


class TestProductionBoot(ProductionBase):
    def test_boot_order_binding_and_silent_diagnostics(self):
        z = self.make_cpu()
        self.boot_to_prompt(z)
        sio = bytes(z.sio_tx).decode("ascii", "replace")
        trace = "".join(z.glyphs)

        # Early SIO banner; no optional-module diagnostic at all; no
        # acceptance-test symbol or marker in the production image.
        for line in ("ZephyrOS 0.0", "platform: Zephyr-80", "serial: OK"):
            self.assertIn(line, sio)
        self.assertNotIn(": unavailable 0x", sio)
        self.assertNotIn("FAIL", sio)
        self.assertNotIn("console integration", sio)
        self.assertNotIn("_ace_integration_result", self.syms)
        self.assertNotIn("_ace_fallback_result", self.syms)

        # Load order is the frozen sio -> ioc -> hid -> v9958 sequence,
        # visible in the issued handles and the instance names.
        devices = self.devices(z)
        names = [self.bank7_cstr(z, d["name"]) for d in devices]
        self.assertEqual(names, [b"sio0.b", b"ioc.0", b"hid.keyboard0",
                                 b"v9958.0"])
        handles = [d["handle"] for d in devices]
        self.assertEqual(handles, sorted(handles),
                         "registry handles are not in load order")

        # One final console_bind_defaults(): HID input, V9958 output.
        hid = [d for d in devices if d["class"] == ZDEV_HID][0]
        vdp = [d for d in devices if d["class"] == ZDEV_VIDEO][0]
        self.assertEqual(z.bank7[self.syms["_console_in_handle"]],
                         hid["handle"])
        self.assertEqual(z.bank7[self.syms["_console_out_handle"]],
                         vdp["handle"])

        # The console line went through the bound V9958 output (not the
        # serial link): early SIO and the normal console are disjoint.
        self.assertIn("console: dynamic sio0.b OK", trace)
        self.assertNotIn("console: dynamic", sio)

        # The unchanged monitor runs through the binding.
        self.assertIn(BANNER, trace)
        self.assertIn(PROMPT, trace)

    def test_scripted_monitor_session_two_relaunch_cycles(self):
        z = self.make_cpu()
        self.boot_to_prompt(z)
        trace = "".join(z.glyphs)
        match = re.search(r"scratch=([0-9A-F]{4})", trace)
        self.assertIsNotNone(match, "banner does not print the scratch "
                                    "address")
        scratch = int(match.group(1), 16)
        packed = self.entries["monitor.zex"]["payload"]

        # Help, system/bank/process record, dumps against the loaded ZEX
        # bytes, modify/readback, argument rejections.
        self.send_line(z, "S", "ERR: COMMAND")
        self.send_line(z, "H", "Q              quit")
        self.send_line(z, "R", BANK_INFO)
        self.assertIn(SYSINFO, self.trace(z))
        self.assertIn(PROCESS, self.trace(z))
        row = packed[20:36]
        expected = "0100: " + " ".join("%02X" % b for b in row) + " "
        self.send_line(z, "D 0100 20", "0110: ")
        self.assertIn(expected, self.trace(z))
        self.send_line(z, "DB 7 0100 1", "ERR: ARG")
        self.send_line(z, "M %04X A5" % scratch, "OK")
        self.send_line(z, "D %04X 1" % scratch, "%04X: A5 " % scratch)
        self.send_line(z, "O 00 18", "ERR: ARG")
        self.send_line(z, "I 20", "IN=")

        # Line overflow rings the bell and the next line recovers;
        # backspace editing fixes the invalid digit.
        self.send_raw(z, b"D 0100 10" + b"X" * 100 + b"\r", "ERR: ARG")
        self.send_line(z, "D 0100 10", expected)
        self.send_raw(z, b"D 0100 1X\x080\r", expected)

        # First exit and relaunch; the launcher counts stay stable.
        self.send_line(z, "q", MONITOR_EXIT)
        self.run_until_count(z, BANNER, 2)
        self.send_line(z, "R", BANK_INFO)
        self.send_line(z, "Q", MONITOR_EXIT)
        full = "".join(z.glyphs)
        self.assertEqual(full.count(MONITOR_EXIT), 2)
        self.assertEqual(full.count(BANNER), 2)
        self.assertIn("monitor: exit status 0x0000 active 0 free-banks 7",
                      full)


class TestProductionIoc(IocSimBase):
    """Command/status/bulk sequencing and fault recovery through the
    registered class operations on the production bytes."""

    IMAGE_DIR = "build"
    IMAGE_NAME = "zephyros.bin"

    def boot(self, z, stop=None, limit=STEP_LIMIT):
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
            if z.bank7[self.syms["_console_out_handle"]] != 0:
                return bytes(z.sio_tx)
        raise AssertionError("console was never bound (PC=%04X)" % z.pc)

    def prepare(self):
        z = self.make_cpu()
        self.boot(z)
        device = self.ioc_device(z)
        self.assertIsNotNone(device, "ioc.0 is not registered")
        caps, ops, state = device
        return z, z.ioc_peer, ops, state

    def test_command_status_and_ping_round_trip(self):
        z, peer, ops, state = self.prepare()
        self.assertEqual(peer.channel_resets, 1)
        for _ in range(2):
            pattern = bytes(range(16))
            result, reply = self.command_data(z, ops, state, CMD_PING,
                                              pattern)
            self.assertEqual(result, ZIOC_OK)
            self.assertEqual(reply[0], RSP_PING)
            self.assertEqual(reply[1], 0)
            self.assertEqual(reply[2], IOC_PING_DATA_LEN)
            self.assertEqual(bytes(reply[3:19]), pattern)
            self.assertEqual(reply[3 + IOC_PING_DATA_LEVEL],
                             IOC_FW_LEVEL_PINNED)
            self.assertTrue(reply[3 + IOC_PING_DATA_POWER] &
                            IOC_PING_LINK_SYNCED)
        # Semantic status: only HID_PENDING may appear.
        bits = self.status(z, ops, state)
        self.assertEqual(bits & ~0x0001, 0)
        self.assertEqual(peer.rts_asserts, peer.rts_releases)

    def test_bulk_sequencing_over_shared_service(self):
        z, peer, ops, state = self.prepare()

        # No READY admission yet: both bulk calls must be refused before
        # touching the bulk lane.
        lane = (peer.a_channel_resets, peer.a_rts_asserts,
                bytes(peer.a_wr3_writes))
        self.assertEqual(self.bulk_call(z, ops, state, 0x1000, 8, False),
                         ZIOC_EBUSY)
        self.assertEqual(self.bulk_call(z, ops, state, 0x1000, 8, True),
                         ZIOC_EBUSY)
        self.assertEqual((peer.a_channel_resets, peer.a_rts_asserts,
                          bytes(peer.a_wr3_writes)), lane,
                         "a refused bulk call touched the lane")

        # Oversized command DATA is rejected before the request exists.
        request = self.write_request(z, CMD_PING, 0, 27)
        result = self.call(z, z.rd16(ops), state, request, third=RSP)
        self.assertEqual(result, ZIOC_EINVAL)

        # Full /SHARED write/read/delete lifecycle.
        handle = self.open_write(z, ops, state)
        total = 0
        for offset, length in ((0, 1), (1, 128), (129, 512)):
            self.write_section(z, ops, state, handle, offset, length)
            total += length
        self.fs_close(z, ops, state, handle, total)
        handle = self.open_read(z, ops, state)
        for offset, length in ((0, 1), (1, 128), (129, 512)):
            self.read_section(z, ops, state, handle, offset, length)
        self.fs_close(z, ops, state, handle, total)
        self.fs_delete(z, ops, state)
        result, reply = self.fs_open(z, ops, state, IOC_NAME,
                                     FS_MODE_READ)
        self.assertEqual(result, ZIOC_OK)
        self.assertEqual(reply[1], ST_FS_NO_FILE,
                         "cleanup evidence must be the frozen no-file "
                         "status")
        self.assertEqual(reply[2], 0, "no-file reply must be empty")

        # Lane discipline after the whole sequence.
        self.assertEqual(peer.a_rts_asserts, peer.a_rts_releases)
        self.assertEqual(len(peer.bulk_requests), 3,
                         "exactly three write captures expected")

    def test_transient_frame_faults_recover_without_reinit(self):
        z, peer, ops, state = self.prepare()
        for fault, expected in (("crc", ZIOC_EBADCRC),
                                ("seq", -0x0F)):
            peer.fault = fault
            result = self.command(z, ops, state, CMD_PING)
            self.assertEqual(result, expected, fault)
            self.assertEqual(peer.rts_asserts, peer.rts_releases,
                             "%s left RTS asserted" % fault)
            self.assertEqual(peer.channel_resets, 1,
                             "%s reset the channel" % fault)
            peer.fault = None
            self.assertEqual(self.command(z, ops, state, CMD_PING),
                             ZIOC_OK, "no recovery after %s" % fault)
        self.assertEqual(peer.hunt_writes, 1)


class TestProductionHid(ProductionBase):
    """HID queue traffic through the console service."""

    def test_fetch_on_doorbell_fifo_and_malformed_atomicity(self):
        z = self.make_cpu()
        peer = z.hid
        self.boot_to_bound(z)

        # Nothing queued: no fetch at all, and the service reports empty
        # without touching the transport.
        self.assertEqual(self.call(z, self.syms["_console_poll"]), 0)
        self.assertEqual(peer.hid_requests, [])
        self.assertEqual(self.call(z, self.syms["_console_getc"]),
                         ZDEV_EAGAIN)

        # A queued burst is fetched exactly once, in order.
        peer.hid_queue.extend(b"aB3")
        got = [self.console_getc(z) for _ in range(3)]
        self.assertEqual(got, [0x61, 0x42, 0x33])
        self.assertEqual(peer.hid_requests, [16],
                         "not exactly one fetch for the burst")
        self.assertEqual(list(peer.hid_queue), [])
        self.assertEqual(self.call(z, self.syms["_console_getc"]),
                         ZDEV_EAGAIN)

        # A malformed reply changes nothing and the next good fetch
        # still delivers every byte.
        peer.hid_queue.extend(b"\x01\x02\x03")
        peer.hid_faults.append("short")
        self.assertEqual(self.call(z, self.syms["_console_poll"]), 0)
        self.assertEqual(self.call(z, self.syms["_console_getc"]),
                         ZDEV_EAGAIN, "ring changed after malformed reply")
        self.assertEqual(len(peer.hid_queue), 3,
                         "the peer dequeued on a malformed reply")
        peer.hid_faults.clear()
        peer.hid_queue.clear()
        self.assertEqual(self.call(z, self.syms["_console_poll"]), 0)
        peer.hid_queue.extend(b"\x21\x22")
        self.assertEqual(self.console_getc(z), 0x21)
        self.assertEqual(self.console_getc(z), 0x22)
        self.assertEqual(peer.rts_asserts, peer.rts_releases)

    def test_stuck_doorbell_penalty_and_recovery(self):
        z = self.make_cpu()
        peer = z.hid
        self.boot_to_bound(z)
        peer.doorbell = True
        base = len(peer.hid_requests)
        self.assertEqual(self.call(z, self.syms["_console_poll"]), 0)
        self.assertEqual(len(peer.hid_requests), base + 1)
        for _ in range(64):
            self.assertEqual(self.call(z, self.syms["_console_poll"]), 0)
        self.assertEqual(len(peer.hid_requests), base + 1,
                         "the 64-call stuck-doorbell penalty is gone")
        peer.doorbell = False
        peer.hid_queue.append(0x55)
        self.assertEqual(self.call(z, self.syms["_console_poll"]), 1)
        self.assertEqual(self.console_getc(z), 0x55)


class TestProductionInvariants(ProductionBase):
    """Malformed/null/class/capability calls prove harmless, then the
    monitor still starts and exits (post-fault recovery)."""

    def v9958_send(self, z, target, state, op, payload, length):
        sp_before = z.sp
        z.push16(length)
        z.push16(payload)
        z.push16((op & 0xFF) << 8)
        z.sp = (z.sp + 1) & 0xFFFF
        z.push16(RET)
        z.h = (state >> 8) & 0xFF
        z.l = state & 0xFF
        z.pc = target
        for _ in range(STEP_LIMIT):
            self.skip_delay(z)
            z.step()
            if z.pc == RET:
                z.sp = sp_before
                value = (z.d << 8) | z.e
                return value - 0x10000 if value & 0x8000 else value
        raise AssertionError("send at %04X did not return" % target)

    def test_rejections_preserve_tables_granules_and_lanes(self):
        z = self.make_cpu()
        self.boot_to_bound(z)
        resume_pc = z.pc
        before = self.devices(z)
        granules = self.granules(z)
        peer = z.hid
        vdp = z.vdp

        by_class = {}
        for dev in before:
            by_class.setdefault(dev["class"], []).append(dev)
        hid = by_class[ZDEV_HID][0]
        vdp_dev = by_class[ZDEV_VIDEO][0]
        ioc = by_class[ZDEV_IOC][0]

        # VIDEO malformed/null/unsupported operations: exact codes, no
        # port traffic and no state change.
        send = z.rd16(vdp_dev["class_ops"])
        self.assertNotEqual(send, 0, "v9958 class ops missing")
        self.assertEqual(
            self.v9958_send(z, send, vdp_dev["state"], 0x40, 0, 0),
            ZDEV_ENOTSUP, "unknown operation must be ZDEV_ENOTSUP")
        self.assertEqual(
            self.v9958_send(z, send, 0, ZVIDEO_PRESENT, 0, 0),
            ZDEV_ENODEV, "null state must be ZDEV_ENODEV")
        self.assertEqual(
            self.v9958_send(z, send, vdp_dev["state"], ZVIDEO_PRESENT, 0, 5),
            ZDEV_EINVAL)

        # console_bind with unusable pairs: rejected, binding unchanged.
        self.assertEqual(
            self.call(z, self.syms["_console_bind"], a=ioc["handle"],
                      l=vdp_dev["handle"]), ZDEV_EINVAL)
        self.assertEqual(
            self.call(z, self.syms["_console_bind"], a=hid["handle"],
                      l=ioc["handle"]), ZDEV_EINVAL)
        self.assertEqual(z.bank7[self.syms["_console_in_handle"]],
                         hid["handle"])
        self.assertEqual(z.bank7[self.syms["_console_out_handle"]],
                         vdp_dev["handle"])

        # Invariants: device table and arena allocation unchanged, both
        # console directions still work (output on the V9958 trace,
        # input through the HID lane), no handshake left active.
        self.assertEqual(self.devices(z), before)
        self.assertEqual(self.granules(z), granules)
        marker = b"console integration: still alive\r\n"
        addr = self.put_string(z, marker)
        self.assertEqual(
            self.call(z, self.syms["_console_write"], hl=addr,
                      de=len(marker)), len(marker))
        self.pump(z)
        self.assertIn("console integration: still alive", "".join(z.glyphs))
        peer.hid_queue.append(0x37)
        self.assertEqual(self.console_getc(z), 0x37)
        self.assertEqual(peer.rts_asserts, peer.rts_releases)
        self.assertEqual(peer.a_rts_asserts, peer.a_rts_releases)
        self.assertEqual(vdp.protocol_errors, [])
        self.assertFalse(vdp.busy(), "VDP command engine left busy")

        # Post-fault recovery: resume the interrupted kernel flow; the
        # monitor launcher runs and exits.  Every direct call above
        # restored SP, so the saved PC resumes exactly where the boot
        # was interrupted.
        z.pc = resume_pc
        self.run_to(z, PROMPT)
        self.send_line(z, "Q", MONITOR_EXIT)
        self.run_to(z, "free-banks 7")
        self.assertIn("monitor: exit status 0x0000 active 0 free-banks 7",
                      "".join(z.glyphs))


class TestIntegrationImage(ProductionBase):
    """The integration acceptance image target transcript and hand-off."""

    IMAGE_SUBDIR = CONSOLE_INTEGRATION_DIR
    IMAGE_NAME = CONSOLE_INTEGRATION_IMAGE

    def start_kernel(self, z):
        # The acceptance image waits for the host sync sequence before
        # the banner (the bench reset also resets the USB bridge).
        self.crt0(z)
        z.push16(RET)
        z.sio_rx.extend(b"\xaa\x55")
        z.pc = self.syms["_kernel_main"]

    def test_target_transcript_and_monitor_handoff(self):
        z = self.make_cpu()
        self.start_kernel(z)
        for _ in range(STEP_LIMIT):
            self.skip_delay(z)
            z.step()
            if b"console integration: OK\r\n" in z.sio_tx:
                break
        else:
            raise AssertionError("console integration selftest did not complete")
        self.pump(z)
        sio = bytes(z.sio_tx)
        for marker in SELFTEST_MARKERS:
            self.assertIn(marker, sio, marker)
        self.assertNotIn(b": unavailable 0x", sio)
        self.assertNotIn(b"FAIL", sio)
        self.assertEqual(z.bank7[self.syms["_ace_integration_result"]], 0x00)
        # Binding is HID/V9958, exactly as reported.
        devices = self.devices(z)
        hid = [d for d in devices if d["class"] == ZDEV_HID][0]
        vdp = [d for d in devices if d["class"] == ZDEV_VIDEO][0]
        self.assertEqual(z.bank7[self.syms["_console_in_handle"]],
                         hid["handle"])
        self.assertEqual(z.bank7[self.syms["_console_out_handle"]],
                         vdp["handle"])
        # The unchanged monitor follows on the same binding.
        self.run_to(z, PROMPT)
        self.send_line(z, "Q", MONITOR_EXIT)
        self.run_to(z, "free-banks 7")
        self.assertIn("monitor: exit status 0x0000 active 0 free-banks 7",
                      "".join(z.glyphs))


class TestProductionRomfsIdentity(ProductionBase):
    def test_payloads_match_packed_artifacts_and_pinned_hashes(self):
        entries = self.entries
        self.assertEqual(sorted(entries),
                         ["hid.zmod", "ioc.zmod", "iocfat.zmod",
                          "monitor.zex", "sio.zmod", "v9958-6x8.font",
                          "v9958.zmod"])
        checks = (
            ("monitor.zex", romfspack.FLAG_ZEX, "monitor.zex"),
            ("sio.zmod", romfspack.FLAG_ZMOD, "sio.zmod"),
            ("ioc.zmod", romfspack.FLAG_ZMOD, "ioc.zmod"),
            ("hid.zmod", romfspack.FLAG_ZMOD, "hid.zmod"),
            ("v9958.zmod", romfspack.FLAG_ZMOD, "v9958.zmod"),
            ("iocfat.zmod", romfspack.FLAG_ZMOD, "iocfat.zmod"),
            ("v9958-6x8.font", romfspack.FLAG_DATA, "v9958-6x8.font"),
        )
        for name, flag, artifact in checks:
            self.assertEqual(entries[name]["flags"], flag, name)
            path = os.path.join(ROOT, "build", "modules", artifact)
            if not os.path.exists(path):
                self.skipTest("build/modules/%s missing" % artifact)
            with open(path, "rb") as fh:
                packed = fh.read()
            self.assertEqual(entries[name]["payload"], packed,
                             "%s differs from the packed artifact" % name)

        monitor = entries["monitor.zex"]["payload"]
        self.assertEqual(len(monitor), MONITOR_SIZE)
        self.assertEqual(hashlib.sha256(monitor).hexdigest(), MONITOR_SHA256)
        font = entries["v9958-6x8.font"]["payload"]
        self.assertEqual(len(font), 2048)
        self.assertEqual(hashlib.sha256(font).hexdigest(), FONT_SHA256)


if __name__ == "__main__":
    unittest.main()
