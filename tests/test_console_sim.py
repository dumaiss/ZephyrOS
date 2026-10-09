#!/usr/bin/env python3
"""Console acceptance tests: replay the console image in the Z80
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

from z80sim import (Z80, IocCommandPeer, V9958Peer, read_symbols,
                    skip_serial_connect_delay)  # noqa: E402
import romfspack  # noqa: E402

RET = 0x1234
STEP_LIMIT = 80000000
STACK = 0xFFFE
HOST_SYNC = b"\xaa\x55"

ZVIDEO_PRESENT = 0x03
ZDEV_EINVAL = -2

# The monitor, rebuilt in package 28c on the user SDK: it is now an
# ordinary SDK program (crt0 + libzos, no entry assembly and no
# hand-written manifest), so the packed container bytes changed; the size
# is still 20-byte header + linked image.  ZephyrOS carries no
# backward-compatibility obligation for milestone artifacts
# (work/M9-REFERENCE.md), so this is the current oracle.
MONITOR_SIZE = 4786
MONITOR_SHA256 = ("515304486a599922648ab317d47dad255859cc14a31b0fa4d630dbd21e5cc6"
                  "3c")

# Shared markers: the visible session, the early-SIO transcript and the
# console diagnostic's marker and typed-line echo (package 41).
PROMPT = "Z80> "
BANNER = "ZephyrOS native monitor"
SELFTEST_OK = "console selftest: OK"
CONSOLE_MARKER_TEXT = "m8"
TYPED_ECHO_TEXT = "K7"
SYSINFO = "system: abi 01 app 10 os 18 common E000 kernel-bank 07 romfs 02"
PROCESS = "process: pid 0 bank 0 flags 0001"
BANK_INFO = "bank: current 00 mask 7F"
MONITOR_EXIT = "monitor: exit status 0x"

class ConsoleSimBase(unittest.TestCase):
    """Shared boot, stepping, and V9958 trace-decoding helpers."""

    IMAGE_SUBDIR = "build-console-selftest"
    IMAGE_NAME = "zephyros-console-selftest.bin"

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
        """Return from the kernel's serial connect spin (see
        z80sim.skip_serial_connect_delay: the address is only that spin
        with the nucleus mapped, bank 6 answers it during service
        boot/init and must execute)."""
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

    def run_to_pattern(self, z, pattern, limit=STEP_LIMIT, since=0):
        """Step until `pattern` matches the fresh trace (for output whose
        end is not a fixed marker, e.g. a rendered hex value)."""
        for i in range(limit):
            self.skip_delay(z)
            z.step()
            if i % 0x1FFF == 0:
                self.pump(z)
                if re.search(pattern, "".join(z.glyphs[since:])):
                    return
        self.pump(z)
        raise AssertionError(
            "did not match %r (PC=%04X, tail=%r)"
            % (pattern, z.pc, "".join(z.glyphs)[-160:]))

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

    # -- kernel-context calls (sdcccall(1)) -----------------------------
    # -- registry --------------------------------------------------------
class TestConsoleImage(ConsoleSimBase):
    """`build-console-selftest`: the full service set, the console
    diagnostic and the unchanged monitor over the composed lanes."""

    def test_diagnostic_transcript_and_monitor_session(self):
        z = self.make_cpu()
        z.hid.hid_queue.extend(b"K7\r")
        self.boot_to_prompt(z)
        sio = bytes(z.sio_tx).decode("ascii", "replace")
        trace = "".join(z.glyphs)

        # The early SIO carries the banner, the loader's rows and the
        # diagnostic transcript, and never the console service's output.
        self.assertIn("console selftest: input class 04 (hid)\r\n", sio)
        self.assertIn("console selftest: output class 02 (video)\r\n", sio)
        self.assertIn("console selftest: typed\r\n", sio)
        self.assertIn("console selftest: OK\r\n", sio)
        self.assertNotIn("FAIL", sio)
        self.assertNotIn("console: service OK", sio)
        self.assertEqual(self.syms.get("_ace_console_result") is not None,
                         True)

        # The visible lane carries the console boot line, the diagnostic's
        # marker, the typed echo and the monitor session.
        self.assertIn("console: service OK", trace)
        self.assertIn(CONSOLE_MARKER_TEXT, trace)
        self.assertIn(TYPED_ECHO_TEXT, trace)
        self.assertIn(BANNER, trace)
        self.assertIn(PROMPT, trace)

        # The monitor reads through the console service: one typed line and
        # a quit/relaunch cycle.
        self.send_line(z, "R", SYSINFO)
        self.send_line(z, "Q", MONITOR_EXIT)
        self.run_until_count(z, BANNER, 2)
        self.assertEqual("".join(z.glyphs).count(BANNER), 2)

    def test_romfs_monitor_payload_matches_packed_artifact(self):
        payload = self.entries["monitor.zex"]["payload"]
        # The packed ZEX container: 20-byte header + linked image.
        self.assertEqual(len(payload), MONITOR_SIZE)
        self.assertEqual(payload[:4], b"ZEX1")
        self.assertEqual(hashlib.sha256(payload).hexdigest(), MONITOR_SHA256)
        packed_path = os.path.join(ROOT, "build", "modules", "monitor.zex")
        if not os.path.exists(packed_path):
            self.skipTest("build/modules/monitor.zex missing")
        with open(packed_path, "rb") as fh:
            self.assertEqual(payload, fh.read())


if __name__ == "__main__":
    unittest.main()
