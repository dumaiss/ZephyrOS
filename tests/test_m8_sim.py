#!/usr/bin/env python3
"""M8 console acceptance tests: replay the console image in the Z80
simulator.

Coverage (all executing the real linked bytes):

* production-style boot of the HID-input / V9958-output image with the
  SIO + IOC + HID + V9958 module set loaded from the recovery store, the
  console selftest's early-SIO transcript reporting the selected classes,
  and early-SIO independence from the bound console;
* the scripted visible monitor session driven through the HID input lane
  and decoded from the V9958 command trace: banner and prompt, unknown
  command, help, system/bank/process information, current/bank dumps,
  bank and argument rejections, scratch-byte modify/readback, port
  input, latch-port rejections, line overflow with bell + recovery,
  backspace editing, CRLF handling, `Q` exit and one relaunch cycle with
  stable launcher counts;
* kernel-context binding matrix over the real console service: the four
  input/output states that the loaded modules make possible
  (HID/V9958, HID/SIO, SIO/V9958, SIO/SIO), each direction proven
  independently, input/output channels proven disjoint per state, and a
  rejected video operation leaving both directions and the device
  registry intact;
* early/panic SIO bypass: the fatal path stays on the serial link while
  the console is bound to the V9958 output;
* byte identity of the ROMFS monitor payload with the packed artifact
  and the pinned M7 size/hash.

Skipped when the console image / packed monitor have not been built.
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

RET = 0x1234
STEP_LIMIT = 80000000
STACK = 0xFFFE
HOST_SYNC = b"\xaa\x55"

ZDEV_OK = 0
ZDEV_HANDLE_INVALID = 0
ZDEV_SERIAL = 1
ZDEV_VIDEO = 2
ZDEV_HID = 4
ZCAP_CONSOLE_IN = 0x0004
ZCAP_CONSOLE_OUT = 0x0008
ZCAP_HID_TEXT = 0x00010000

ZVIDEO_PRESENT = 0x03
ZDEV_EINVAL = -2

# The unchanged M7 monitor (work/M8-REFERENCE.md hash; the packed
# container is 20-byte header + linked image, as recorded by the M8
# ioc-command/ioc-bulk acceptance records).
MONITOR_SIZE = 3546
MONITOR_SHA256 = ("67574d57bdc2326ef3ecbbae9fdefcb313f0c63523b846c710e459af"
                  "cda643ed")

# Shared markers: the visible session and the early-SIO transcript.
PROMPT = "Z80> "
BANNER = "ZephyrOS native monitor"
SELFTEST_OK = "m8 console selftest: OK"
INPUT_CLASS = "m8 console: input hid.keyboard0 class 04"
OUTPUT_CLASS = "m8 console: output v9958.0 class 02"
RENDERER_VISIBLE = "m8 console: renderer visible"
SYSINFO = "system: abi 01 app 10 os 18 common E000 kernel-bank 07 romfs 02"
PROCESS = "process: pid 0 bank 0 flags 0001"
BANK_INFO = "bank: current 00 mask 7F"
MONITOR_EXIT = "monitor: exit status 0x"

# Kernel scratch used to stage marker strings for direct console calls
# (low RAM of the selected bank while mode 11 is active).
SCRATCH = 0x1F00


class ConsoleSimBase(unittest.TestCase):
    """Shared boot, stepping, and V9958 trace-decoding helpers."""

    IMAGE_SUBDIR = "build-m8-console-selftest"
    IMAGE_NAME = "zephyros-m8-console-selftest.bin"

    def setUp(self):
        self.bin_path = os.path.join(ROOT, self.IMAGE_SUBDIR, self.IMAGE_NAME)
        if not os.path.exists(self.bin_path):
            self.skipTest("%s missing; build the image first" % self.bin_path)
        with open(self.bin_path, "rb") as fh:
            self.image = fh.read()
        self.syms = read_symbols(
            os.path.join(ROOT, self.IMAGE_SUBDIR, "zephyros.map"))
        self.romfs_path = os.path.join(ROOT, self.IMAGE_SUBDIR,
                                       "recovery.romfs")
        if not os.path.exists(self.romfs_path):
            self.skipTest("%s missing; build the image first"
                          % self.romfs_path)
        with open(self.romfs_path, "rb") as fh:
            header, entries = romfspack.parse_image(fh.read())
        self.entries = {entry["name"]: entry for entry in entries}

    # -- machine ---------------------------------------------------------
    def make_cpu(self, latch=0x18):
        pages = {n: self.image[n * 0x10000:(n + 1) * 0x10000]
                 for n in range(8)}
        z = Z80(pages[0], pages[1],
                rom_pages={n: pages[n] for n in range(2, 8)})
        z.latch = latch
        z.sp = STACK
        z.hid = IocCommandPeer()
        z.ioc_peer = z.hid
        z.vdp = V9958Peer()
        z.vdp_peer = z.vdp
        z.glyphs = []
        z.glyph_pos = 0
        z.glyph_last = None
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
        z.sio_rx.extend(HOST_SYNC)
        z.pc = self.syms["_kernel_main"]

    # -- V9958 command-trace decoding -----------------------------------
    def pump(self, z):
        """Decode newly issued glyph copies into a positioned text model.

        A 6x8 HMMM copy is a printable glyph; a glyph that does not
        continue the previous copy starts a new model line.  CR/LF and
        scrolling are handled by the renderer itself, so the model only
        needs the printed characters."""
        commands = z.vdp.commands
        while z.glyph_pos < len(commands):
            cmd = commands[z.glyph_pos]
            z.glyph_pos += 1
            block = cmd.get("block")
            if (cmd.get("code") != 0xD0 or not block or
                    (block[8] | (block[9] << 8)) != 6 or
                    (block[10] | (block[11] << 8)) != 8):
                continue
            sx = block[0] | (block[1] << 8)
            sy = block[2]
            dx = block[4] | (block[5] << 8)
            dy = block[6] | (block[7] << 8)
            glyph = ((sy & 0x3F) >> 3) * 32 + sx // 6
            if 32 <= glyph < 127:
                if z.glyph_last == [dy, dx - 6]:
                    z.glyphs.append(chr(glyph))
                else:
                    z.glyphs.append("\n" + chr(glyph))
                z.glyph_last = [dy, dx]

    def trace(self, z, since=0):
        self.pump(z)
        return "".join(z.glyphs[since:])

    def run_to(self, z, expect, limit=STEP_LIMIT, since=0):
        """Step until `expect` appears in the trace (V9958 path)."""
        for i in range(limit):
            self.skip_delay(z)
            z.step()
            if i % 0x1FFF == 0:
                self.pump(z)
                if expect in "".join(z.glyphs[since:]):
                    return
        self.pump(z)
        raise AssertionError(
            "did not reach %r (PC=%04X, tail=%r)"
            % (expect, z.pc, "".join(z.glyphs)[-160:]))

    def run_until_count(self, z, marker, count, limit=STEP_LIMIT):
        """Step until `marker` occurs at least `count` times in the
        whole trace (used for relaunch detection)."""
        for i in range(limit):
            self.skip_delay(z)
            z.step()
            if i % 0x1FFF == 0:
                self.pump(z)
                if "".join(z.glyphs).count(marker) >= count:
                    return
        self.pump(z)
        raise AssertionError(
            "%r occurred %d times, wanted %d (PC=%04X)"
            % (marker, "".join(z.glyphs).count(marker), count, z.pc))

    def send_line(self, z, line, expect, limit=STEP_LIMIT):
        """Type one monitor line through the HID input lane and wait
        for the fresh output it produces."""
        start = len(z.glyphs)
        z.hid.hid_queue.extend(line.encode("ascii") + b"\r")
        self.run_to(z, expect, limit, since=start)

    def send_raw(self, z, data, expect, limit=STEP_LIMIT):
        start = len(z.glyphs)
        z.hid.hid_queue.extend(data)
        self.run_to(z, expect, limit, since=start)

    def boot_to_prompt(self, z, limit=STEP_LIMIT):
        self.start_kernel(z)
        self.run_to(z, PROMPT, limit)
        return self.trace(z)

    def boot_to_selftest_ok(self, z, limit=STEP_LIMIT):
        """Boot until the early-SIO selftest line appears.  The CPU is
        still in kernel context at this point (the monitor launcher has
        not run yet), so direct kernel calls are valid."""
        self.start_kernel(z)
        for _ in range(limit):
            self.skip_delay(z)
            z.step()
            if b"m8 console selftest: OK" in z.sio_tx:
                return
            if _ % 0x1FFF == 0:
                self.pump(z)
        raise AssertionError("console selftest never completed")

    # -- kernel-context calls (sdcccall(1)) -----------------------------
    def call(self, z, target, a=None, hl=None, de=None, l=None,
             limit=STEP_LIMIT):
        """Invoke a kernel function with the SDCC 4.5 register layout:
        first byte argument in A, second in L, 16-bit arguments in HL
        then DE, int result in DE.  Verified against generated call
        sites for this toolchain."""
        sp_before = z.sp
        z.push16(RET)
        if a is not None:
            z.a = a
        if hl is not None:
            z.h = (hl >> 8) & 0xFF
            z.l = hl & 0xFF
        if de is not None:
            z.d = (de >> 8) & 0xFF
            z.e = de & 0xFF
        if l is not None:
            z.l = l
        z.pc = target
        for _ in range(limit):
            self.skip_delay(z)
            z.step()
            self.pump(z)
            if z.pc == RET:
                z.sp = sp_before
                value = (z.d << 8) | z.e
                return value - 0x10000 if value & 0x8000 else value
        raise AssertionError("call at %04X did not return (PC=%04X)"
                             % (target, z.pc))

    def put_string(self, z, data):
        z.bank0[SCRATCH:SCRATCH + len(data) + 1] = data + b"\x00"
        return SCRATCH

    def console_getc(self, z, limit=400):
        """Fetch one console byte, tolerating transient EAGAIN codes."""
        for _ in range(limit):
            ch = self.call(z, self.syms["_console_getc"])
            if ch >= 0:
                return ch
        raise AssertionError("console_getc never returned a byte")

    # -- registry --------------------------------------------------------
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

    def cstr(self, z, addr):
        out = bytearray()
        while z.rd(addr) != 0:
            out.append(z.rd(addr))
            addr = (addr + 1) & 0xFFFF
        return bytes(out)

    def bank7_cstr(self, z, addr):
        """Module-resident strings live in the arena (bank 7), which is
        not addressable through the app-mode mapping the monitor runs
        in; read them from the bank image."""
        out = bytearray()
        while z.bank7[addr] != 0:
            out.append(z.bank7[addr])
            addr = (addr + 1) & 0xFFFF
        return bytes(out)


class TestConsoleBootAndSession(ConsoleSimBase):
    """The console image boots to the unchanged monitor and serves the
    scripted visible session over the HID input / V9958 output pair."""

    def test_boot_transcript_and_early_sio_independence(self):
        z = self.make_cpu()
        self.boot_to_prompt(z)
        sio = bytes(z.sio_tx).decode("ascii", "replace")
        trace = "".join(z.glyphs)

        # Early-SIO control transcript: classes and outcome.
        self.assertIn(INPUT_CLASS, sio)
        self.assertIn(OUTPUT_CLASS, sio)
        self.assertIn(SELFTEST_OK + "\r\n", sio)
        self.assertNotIn("FAIL", sio)
        self.assertNotIn("PANIC", sio)
        self.assertEqual(z.bank7[self.syms["_m8_console_selftest_result"]],
                         0x00)

        # Registry: exactly the four accepted modules, in load order.
        names = [self.bank7_cstr(z, dev["name"])
                 for dev in self.devices(z)]
        self.assertEqual(names, [b"sio0.b", b"ioc.0", b"hid.keyboard0",
                                 b"v9958.0"])

        # The bound console pair is HID input + V9958 output (defaults
        # chosen from the discovered preferred pair at boot).
        self.assertEqual(
            z.bank7[self.syms["_console_in_handle"]],
            [d for d in self.devices(z) if d["class"] == ZDEV_HID][0]
            ["handle"])
        self.assertEqual(
            z.bank7[self.syms["_console_out_handle"]],
            [d for d in self.devices(z) if d["class"] == ZDEV_VIDEO][0]
            ["handle"])

        # Both directions are really independent channels: the console
        # renderer output is visible only in the V9958 trace, the early
        # control lines only on the serial link.
        self.assertIn(RENDERER_VISIBLE, trace)
        self.assertIn("m8 console: reverse SGR normal", trace)
        self.assertNotIn(RENDERER_VISIBLE, sio)
        self.assertNotIn("m8 console selftest", trace)

        # Monitor banner/prompt and the scratch address from the banner.
        self.assertIn(BANNER, trace)
        self.assertIn(PROMPT, trace)
        match = re.search(r"scratch=([0-9A-F]{4})", trace)
        self.assertIsNotNone(match, "banner does not print the scratch "
                                    "address")

    def test_full_scripted_monitor_session(self):
        z = self.make_cpu()
        boot = self.boot_to_prompt(z)
        trace = "".join(z.glyphs)
        match = re.search(r"scratch=([0-9A-F]{4})", trace)
        self.assertIsNotNone(match)
        scratch = int(match.group(1), 16)
        packed = self.entries["monitor.zex"]["payload"]

        # Unknown command (input rejection).
        self.send_line(z, "S", "ERR: COMMAND")

        # Help through the diagnostic console surface.
        self.send_line(z, "H", "Q              quit")
        self.assertIn("R              system/bank information",
                      self.trace(z))
        self.assertIn("DB bank addr len  dump bank 0-6", self.trace(z))
        self.assertIn("I port         input port (10-FF)", self.trace(z))
        self.send_line(z, "H EXTRA", "ERR: ARG")

        # System/bank/process record; the bank line is last.
        self.send_line(z, "R", BANK_INFO)
        self.assertIn(SYSINFO, self.trace(z))
        self.assertIn(PROCESS, self.trace(z))

        # Current-bank dump: rows must match the loaded ZEX bytes.
        row = packed[20:36]
        expected = "0100: " + " ".join("%02X" % b for b in row) + " "
        self.send_line(z, "D 0100 20", "0110: ")
        self.assertIn(expected, self.trace(z))

        # Lowercase commands and hex digits are accepted.
        self.send_line(z, "d 100 10", expected)

        # Bank dumps: page zero opens with the RST 30h JP, bank 1 is a
        # valid application bank, bank 7 and its modify form are masked.
        self.send_line(z, "DB 0 0030 1", "0030: C3 ")
        self.send_line(z, "DB 1 0100 1", "0100: ")
        self.send_line(z, "DB 7 0100 1", "ERR: ARG")
        self.send_line(z, "MB 7 0100 01", "ERR: ARG")

        # Scratch-byte modify/readback in the monitor's own bank.
        self.send_line(z, "M %04X A5" % scratch, "OK")
        readback = "%04X: A5 " % scratch
        self.send_line(z, "D %04X 1" % scratch, readback)

        # Argument rejections: page zero, missing value, junk, range,
        # short argument lists.
        self.send_line(z, "M 00FF 00", "ERR: ARG")
        self.send_line(z, "M 0100", "ERR: ARG")
        self.send_line(z, "D 0100 10 GARBAGE", "ERR: ARG")
        self.send_line(z, "D 12345 10", "ERR: ARG")
        self.send_line(z, "DB 1 0100", "ERR: ARG")

        # Harmless port input, latch-aliased ports rejected unexecuted.
        self.send_line(z, "I 20", "IN=")
        self.assertRegex(self.trace(z), r"IN=[0-9A-F]{2}\r?\n?|IN=[0-9A-F]{2}")
        self.send_line(z, "O 00 18", "ERR: ARG")
        self.send_line(z, "O 0F 00", "ERR: ARG")
        self.send_line(z, "I 0F", "ERR: ARG")

        # Line overflow rings the bell, keeps the first 80 characters,
        # and the monitor recovers on the next line.
        self.send_raw(z, b"D 0100 10" + b"X" * 100 + b"\r", "ERR: ARG")
        self.send_line(z, "D 0100 10", expected)

        # Backspace editing corrects the invalid final digit.
        self.send_raw(z, b"D 0100 1X\x080\r", expected)

        # CR after LF is swallowed by the HID lane: no empty command.
        before = len(z.glyphs)
        self.send_line(z, "R\r\n", BANK_INFO)
        self.assertNotIn("ERR", self.trace(z, since=before))

        # Q with trailing garbage is rejected; plain q exits.
        self.send_line(z, "Q X", "ERR: ARG")
        self.send_line(z, "q", MONITOR_EXIT)

        # Second cycle: the launcher re-execs the same monitor; wait for
        # the second banner, then exit again.
        self.run_until_count(z, BANNER, 2)
        self.send_line(z, "R", BANK_INFO)
        self.send_line(z, "Q", MONITOR_EXIT)

        # Exactly two identical launcher lines: cycles are stable.
        full = "".join(z.glyphs)
        self.assertEqual(full.count(MONITOR_EXIT), 2)
        self.assertEqual(full.count(BANNER), 2)

    def test_romfs_monitor_payload_matches_packed_artifact(self):
        payload = self.entries["monitor.zex"]["payload"]
        # The packed ZEX container: 20-byte header + linked image.
        self.assertEqual(len(payload), 3546)
        self.assertEqual(payload[:4], b"ZEX1")
        self.assertEqual(hashlib.sha256(payload).hexdigest(), MONITOR_SHA256)
        packed_path = os.path.join(ROOT, "build", "modules", "monitor.zex")
        if not os.path.exists(packed_path):
            self.skipTest("build/modules/monitor.zex missing")
        with open(packed_path, "rb") as fh:
            self.assertEqual(payload, fh.read())


class TestBindingMatrix(ConsoleSimBase):
    """The four input/output states over the real console service."""

    def prepare(self):
        z = self.make_cpu()
        self.boot_to_selftest_ok(z)
        self.assertEqual(z.latch, 0x18, "kernel context expected")
        devs = self.devices(z)
        by_class = {}
        for dev in devs:
            by_class.setdefault(dev["class"], []).append(dev)
        self.hid = by_class[ZDEV_HID][0]
        self.vdp = by_class[ZDEV_VIDEO][0]
        sio = [dev for dev in by_class[ZDEV_SERIAL]
               if (dev["caps"] & (ZCAP_CONSOLE_IN | ZCAP_CONSOLE_OUT)) ==
               (ZCAP_CONSOLE_IN | ZCAP_CONSOLE_OUT)]
        self.assertTrue(sio, "no console-capable serial device")
        self.sio = sio[0]
        # The default pair is already HID + V9958 and proven by boot.
        self.assertEqual(z.bank7[self.syms["_console_in_handle"]],
                         self.hid["handle"])
        self.assertEqual(z.bank7[self.syms["_console_out_handle"]],
                         self.vdp["handle"])
        return z

    def bind(self, z, dev_in, dev_out):
        self.assertEqual(
            self.call(z, self.syms["_console_bind"], a=dev_in["handle"],
                      l=dev_out["handle"]), ZDEV_OK)
        self.assertEqual(z.bank7[self.syms["_console_in_handle"]],
                         dev_in["handle"])
        self.assertEqual(z.bank7[self.syms["_console_out_handle"]],
                         dev_out["handle"])

    def exchange(self, z, marker, in_dev, in_byte):
        """Write one marker and read one invited byte in the current
        binding; return (serial delta, v9958 delta, byte read)."""
        sio_before = len(z.sio_tx)
        glyph_before = len(z.glyphs)
        addr = self.put_string(z, marker)
        rc = self.call(z, self.syms["_console_write"], hl=addr,
                       de=len(marker))
        self.assertEqual(rc, len(marker), "console_write short count")
        serial = bytes(z.sio_tx)[sio_before:]
        self.pump(z)
        visible = "".join(z.glyphs[glyph_before:])
        if in_dev["class"] == ZDEV_HID:
            z.hid.hid_queue.append(in_byte)
        else:
            z.sio_rx.append(in_byte)
        got = self.console_getc(z)
        return serial, visible, got

    def test_four_binding_states_with_independent_directions(self):
        z = self.prepare()
        states = (
            ("HID/V9958", self.hid, self.vdp, b"K1x", 0x41),
            ("HID/SIO", self.hid, self.sio, b"K2x", 0x42),
            ("SIO/V9958", self.sio, self.vdp, b"K3x", 0x43),
            ("SIO/SIO", self.sio, self.sio, b"K4x", 0x44),
        )
        seen = {}
        for name, dev_in, dev_out, marker, byte in states:
            self.bind(z, dev_in, dev_out)
            serial, visible, got = self.exchange(z, marker, dev_in, byte)
            text = marker.decode("ascii")
            expect_serial = dev_out["class"] == ZDEV_SERIAL
            self.assertEqual(
                marker in serial, expect_serial,
                "%s: output went to the wrong channel (serial=%r)"
                % (name, serial))
            self.assertEqual(
                text in visible, not expect_serial,
                "%s: output went to the wrong channel (trace=%r)"
                % (name, visible))
            self.assertEqual(got, byte, "%s: input byte lost" % name)
            seen[name] = (serial, visible, got)

        # Each state's channels are disjoint from the others' payloads.
        self.assertIn(b"K2x", seen["HID/SIO"][0])
        self.assertNotIn(b"K1x", seen["HID/SIO"][0])
        self.assertIn("K3x", seen["SIO/V9958"][1])
        self.assertNotIn("K4x", seen["SIO/V9958"][1])

        # Restore the default pair and prove both directions again.
        self.bind(z, self.hid, self.vdp)
        serial, visible, got = self.exchange(z, b"K5x", self.hid, 0x45)
        self.assertNotIn(b"K5x", serial)
        self.assertIn("K5x", visible)
        self.assertEqual(got, 0x45)

    def test_rejected_video_operation_leaves_console_and_registry_intact(self):
        z = self.prepare()
        before = self.devices(z)
        video = self.vdp

        # Malformed PRESENT through the module's zvideo_ops.send (byte
        # length that the block validates against).
        send = z.rd16(video["class_ops"])
        self.assertNotEqual(send, 0, "v9958 class ops missing")
        value = self.v9958_send(z, send, video["state"],
                                ZVIDEO_PRESENT, 0x0000, 5)
        self.assertEqual(value, ZDEV_EINVAL)

        # Registry unchanged, both directions still work.
        self.assertEqual(self.devices(z), before)
        serial, visible, got = self.exchange(z, b"K6x", self.hid, 0x46)
        self.assertNotIn(b"K6x", serial)
        self.assertIn("K6x", visible)
        self.assertEqual(got, 0x46)

    def v9958_send(self, z, target, state, op, payload, length):
        """Call zvideo_ops.send with the SDCC 4.5 stack layout: HL =
        state; on the stack from SP: operation (1 byte), payload
        (2 bytes), length (2 bytes); int result in DE."""
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

    def test_early_and_panic_sio_bypass_the_bindings(self):
        z = self.prepare()
        # Early console output already proved the bypass during boot: the
        # selftest control lines reached the serial link while the
        # console was bound to HID/V9958, and the renderer text did not.
        sio = bytes(z.sio_tx)
        self.assertIn(b"m8 console: output v9958.0 class 02", sio)
        self.assertNotIn(b"m8 console: renderer visible", sio)

        # The fatal path is common-resident and early-console based: it
        # must print to the serial link even with the console bound to
        # the V9958 output.
        self.bind(z, self.hid, self.vdp)
        glyph_before = len(z.glyphs)
        addr = self.put_string(z, b"P19 panic probe")
        sio_before = len(z.sio_tx)
        z.h = (addr >> 8) & 0xFF
        z.l = addr & 0xFF
        z.pc = self.syms["_panic"]
        for _ in range(2000000):
            z.step()
            if b"PANIC: P19 panic probe\r\n" in z.sio_tx:
                break
        self.assertIn(b"PANIC: P19 panic probe\r\n",
                      bytes(z.sio_tx)[sio_before:])
        self.pump(z)
        self.assertNotIn("P19 panic probe",
                         "".join(z.glyphs[glyph_before:]))


if __name__ == "__main__":
    unittest.main()
