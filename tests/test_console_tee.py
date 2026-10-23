#!/usr/bin/env python3
"""Console serial rescue-path replays: the production image.

Three 1Bh bytes on the serial line toggle input ownership between the
USB keyboard and the serial line and arm a tee that mirrors console
output to the serial line; three more switch ownership back.  These
replays cover, against the production image:

* the gesture: three ESCs arm the tee and take input ownership, so a
  command typed on the serial line runs and its output appears on the
  serial line and on the V9958 trace;
* input ownership: while the serial line owns input the HID keys are
  ignored, and three more ESCs return the input to the HID with the tee
  still armed;
* the gesture bytes are consumed: a key-decoding program reports the
  byte that follows the gesture, never the ESCs;
* a partial gesture followed by another byte arms nothing and resets
  the count;
* RENDER is refused while the tee is armed, so libtui's ANSI fallback
  reaches both screens, and accepted unarmed.

The TUI examples used for the last two checks are packed into a copy of
the production store; the code image is untouched.

Skipped when the image or a TUI example has not been built.
"""

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import romfspack  # noqa: E402
from replay import ConsoleReplay, SHELL_PROMPT, STEP_LIMIT  # noqa: E402
from test_console_acceptance import ConsoleReplayBase  # noqa: E402
from test_tui_sim import _splice_store  # noqa: E402
from z80sim import IocCommandPeer, read_symbols  # noqa: E402

# `pwd` output at the boot CWD (the IOC peer models a card, so the
# shell starts in `/`).
PWD_LINE = b"/\r\n"

TUI_TEST_EXE = os.path.join(ROOT, "build", "user", "tui_test.exe")
TUI_KEYS_EXE = os.path.join(ROOT, "build", "user", "tui_keys.exe")
PRODUCTION_BIN = os.path.join(ROOT, "build", "konaos.bin")
PRODUCTION_MAP = os.path.join(ROOT, "build", "konaos.map")

# tui_keys reports the decoded key as hex and a name; a delivered ESC
# would end the program with the PASS line instead.
KEYS_BYTE = "0041 BYTE"
KEYS_PASS = b"TUI KEYS PASS\r\n"
BATCH_COMMAND = b"/system/tui_test.exe batch\r"
BATCH_PASS = "tui: batch pass"
GRID_COMMAND = b"/system/tui_test.exe grid\r"


class TestConsoleTee(ConsoleReplayBase):
    """The tee and input ownership, at the production shell prompt."""

    def serial_until(self, z, marker, since=0, limit=STEP_LIMIT):
        """Step until `marker` appears on the serial lane."""
        self.run_until(z, [marker], since=since, limit=limit)

    def arm(self, z):
        """Arm the tee with three ESCs and prove it with a serial `pwd`
        whose output reaches the serial lane.  Returns the serial
        transcript length at arming time."""
        sio_at = len(z.sio_tx)
        z.sio_rx.extend(b"\x1b\x1b\x1bpwd\r")
        self.serial_until(z, PWD_LINE, since=sio_at)
        return sio_at

    def test_three_escs_arm_the_tee(self):
        z = self.start()
        self.boot_to_shell(z)
        trace_at = len(z.glyphs)
        sio_at = self.arm(z)
        sio = self.read_sio(z)
        trace = self.read_trace(z)[trace_at:]
        # The serial command's echo and its output travel to both
        # lanes.
        self.assertIn(b"pwd\r\n" + PWD_LINE, sio[sio_at:])
        self.assertIn("pwd", trace)
        self.assertIn("/", trace)

    def test_serial_owns_input_and_hid_is_ignored(self):
        z = self.start()
        self.boot_to_shell(z)
        self.arm(z)
        trace_at = len(z.glyphs)
        # Keys the shell would echo if it read them, queued while the
        # serial line owns input, then a second serial command.
        z.hid.hid_queue.extend(b"ZZZ\r")
        sio_at = len(z.sio_tx)
        z.sio_rx.extend(b"pwd\r")
        self.serial_until(z, PWD_LINE, since=sio_at)
        self.assertIn(b"pwd\r\n" + PWD_LINE, self.read_sio(z)[sio_at:])
        self.assertNotIn(b"ZZZ", self.read_sio(z))
        self.assertNotIn("ZZZ", self.read_trace(z)[trace_at:])

    def test_three_more_escs_return_input_to_hid_and_the_tee_stays(self):
        z = self.start()
        self.boot_to_shell(z)
        self.arm(z)
        # The switch-back gesture, then a command on the HID.  `hello`
        # (not the prompt) is the marker: the prompt after the armed
        # command is flushed before the read that drains the gesture.
        z.sio_rx.extend(b"\x1b\x1b\x1b")
        sio_at = len(z.sio_tx)
        # `help`'s output marker differs from its echo, so the serial
        # lane proves the command completed.
        self.type_line(z, b"help\r", "built-ins:")
        self.serial_until(z, b"built-ins:", since=sio_at)
        sio = self.read_sio(z)[sio_at:]
        self.assertIn(b"help\r\n", sio)
        self.assertIn(b"built-ins:", sio)

    def test_partial_gestures_do_nothing_and_reset(self):
        z = self.start()
        self.boot_to_shell(z)
        sio_before = self.read_sio(z)
        # Two ESCs then another byte, twice: each partial gesture arms
        # nothing (and the second one proves the first was reset by the
        # other byte), while HID input keeps working.
        for _ in range(2):
            z.sio_rx.extend(b"\x1b\x1bX")
            self.type_line(z, b"pwd\r", SHELL_PROMPT)
        self.assertEqual(self.read_sio(z), sio_before)
        # A full gesture afterwards still arms.
        sio_at = len(z.sio_tx)
        z.sio_rx.extend(b"\x1b\x1b\x1bpwd\r")
        self.serial_until(z, PWD_LINE, since=sio_at)


class _TuiTeeBase(ConsoleReplay, unittest.TestCase):
    """The production code image with a store that also carries the TUI
    examples (`_splice_store` swaps only the ROMFS payload)."""

    IMAGE_SUBDIR = "build"
    IMAGE_NAME = "konaos.bin"

    @classmethod
    def setUpClass(cls):
        for path in (PRODUCTION_BIN, PRODUCTION_MAP, TUI_TEST_EXE,
                     TUI_KEYS_EXE):
            if not os.path.isfile(path):
                raise unittest.SkipTest("%s has not been built" % path)
        cls.tmp = tempfile.mkdtemp(prefix="tee-tui-")
        manifest = json.loads(
            Path(os.path.join(ROOT, "build", "store.json")).read_text())
        extras = (("tui_test.exe", TUI_TEST_EXE),
                  ("tui_keys.exe", TUI_KEYS_EXE))
        for name, path in extras:
            manifest["files"] = [entry for entry in manifest["files"]
                                 if entry["name"].lower() != name.lower()]
            manifest["files"].append({"name": name, "type": "EXE",
                                      "path": path})
        out_json = os.path.join(cls.tmp, "store.json")
        Path(out_json).write_text(json.dumps(manifest))
        stream, _report = romfspack.pack(
            out_json, os.path.join(cls.tmp, "store.romfs"),
            objdir=os.path.join(ROOT, "build", "modules"))
        with open(PRODUCTION_BIN, "rb") as handle:
            cls.image = _splice_store(handle.read(), stream)
        cls.syms = read_symbols(PRODUCTION_MAP)

    @classmethod
    def tearDownClass(cls):
        if getattr(cls, "tmp", None):
            import shutil
            shutil.rmtree(cls.tmp, ignore_errors=True)

    def start(self):
        z = self.make_cpu()
        z.hid = z.ioc_peer = IocCommandPeer()
        self.start_kernel(z)
        return z

    def type_line(self, z, text, expect, limit=STEP_LIMIT):
        self.send_raw(z, text, expect, limit=limit)

    def read_sio(self, z):
        return bytes(z.sio_tx)

    def read_trace(self, z):
        return self.trace(z)


class TestConsoleTeeTui(_TuiTeeBase):
    def test_render_refused_while_armed_and_accepted_unarmed(self):
        z = self.start()
        self.boot_to_shell(z)
        # Unarmed, the batch mode's render stream is accepted: the pass
        # line reaches the V9958 and nothing is teed to the serial line.
        self.type_line(z, BATCH_COMMAND, BATCH_PASS)
        self.assertNotIn(b"tui: batch pass", self.read_sio(z))
        # Armed, the same program falls back to ANSI text, which the
        # tee carries to both screens.
        sio_at = len(z.sio_tx)
        trace_at = len(z.glyphs)
        z.sio_rx.extend(b"\x1b\x1b\x1b" + BATCH_COMMAND)
        self.run_until(z, [b"tui: batch pass\r\n"], since=sio_at)
        self.assertIn(BATCH_PASS, self.read_trace(z)[trace_at:])

    def test_info_withdraws_render_while_armed(self):
        """KCONSOLE_OP_INFO reports RENDER unarmed and withdraws it while
        the tee is armed (the RENDER refusal above), with the same grid."""
        z = self.start()
        self.boot_to_shell(z)
        trace_at = len(z.glyphs)
        self.type_line(z, GRID_COMMAND, "tui: grid pass")
        self.assertIn("tui: grid console 85x26 attr 1 flags 1",
                      self.read_trace(z)[trace_at:])
        sio_at = len(z.sio_tx)
        z.sio_rx.extend(b"\x1b\x1b\x1b" + GRID_COMMAND)
        self.run_until(z, [b"tui: grid pass\r\n"], since=sio_at)
        self.assertIn(b"tui: grid console 85x26 attr 1 flags 0",
                      bytes(z.sio_tx[sio_at:]))

    def test_gesture_bytes_reach_no_program(self):
        z = self.start()
        self.boot_to_shell(z)
        self.type_line(z, b"/system/tui_keys.exe\r", "Press")
        trace_at = len(z.glyphs)
        sio_at = len(z.sio_tx)
        # The gesture plus one key: the decoder reports only the key.
        # A delivered ESC would end the program with the PASS line, so
        # its absence proves the gesture bytes were consumed.
        z.sio_rx.extend(b"\x1b\x1b\x1bA")
        self.run_until(z, [b"0041 BYTE\r\n"], since=sio_at)
        sio = self.read_sio(z)[sio_at:]
        trace = self.read_trace(z)[trace_at:]
        self.assertIn(KEYS_BYTE, trace)
        self.assertNotIn(KEYS_PASS, sio)
        self.assertNotIn("TUI KEYS PASS", trace)

    def test_serial_key_sequences_keep_their_lead_in(self):
        z = self.start()
        self.boot_to_shell(z)
        # Arm the tee and run the key decoder from the serial line.
        z.sio_rx.extend(b"\x1b\x1b\x1b/system/tui_keys.exe\r")
        self.run_until(z, [b"Press"], since=len(z.sio_tx))
        sio_at = len(z.sio_tx)
        trace_at = len(z.glyphs)
        # A key sequence's leading ESC is data, not the gesture: left
        # arrow and F1 arrive as the terminal sends them.
        z.sio_rx.extend(b"\x1b[D\x1bOP")
        self.run_until(z, [b"0121 F1\r\n"], since=sio_at)
        sio = self.read_sio(z)[sio_at:]
        self.assertIn(b"0103 LEFT\r\n", sio)
        self.assertNotIn(KEYS_PASS, sio)
        trace = self.read_trace(z)[trace_at:]
        self.assertIn("LEFT", trace)
        self.assertIn("F1", trace)


if __name__ == "__main__":
    unittest.main()
