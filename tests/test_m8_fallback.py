#!/usr/bin/env python3
"""M8 fallback-matrix tests (work package 20): replay every observable
boot/failure variant in the Z80 simulator.

Rows (all executing real linked bytes, every image from its own build
directory):

* all modules (the integration acceptance image): SIO + IOC + HID +
  V9958 resident, console HID/V9958, no optional-module diagnostic,
  target selftest OK and the unchanged monitor launcher starts;
* no HID in the recovery store (`hid: unavailable 0x05`): SIO input +
  V9958 output; the scripted serial byte is read back through the
  console input path and the console marker is display-only;
* no HID and no V9958 (`hid: unavailable 0x05`, `v9958: unavailable
  0x05`): SIO/SIO; the invited serial byte and the console marker both
  appear in the transcript;
* malformed IOC interface (null command operation rolls HID back,
  `hid: unavailable 0x0B`): SIO/V9958;
* arena exhaustion (the v9958.zmod entry cannot be allocated,
  `v9958: unavailable 0x06`): HID/SIO, console marker serial-visible.

Each failure row asserts the exact bound input/output instance and
class from the live registry, the registered-device set (proving no
already-resident module was lost and no failed module was left
behind), the precise diagnostic codes, and the early-SIO independence
of the control transcript (a V9958-bound marker must NOT leak to the
serial link; it must appear in the decoded V9958 trace instead).

Skipped when the images have not been built (`make m8-fallback-selftests`).
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import Z80, IocCommandPeer, V9958Peer, read_symbols  # noqa: E402
from test_m8_sim import ConsoleSimBase, PROMPT, BANNER  # noqa: E402

ZDEV_SERIAL = 1
ZDEV_VIDEO = 2
ZDEV_HID = 4

STEP_LIMIT = 80000000
INPUT_BYTE = 0x52

CONSOLE_MARKER = b"m8 fallback: console output\r\n"
SELFTEST_OK = b"m8 fallback selftest: OK\r\n"
SELFTEST_FAIL = b"m8 fallback selftest: FAIL"
SEND_INPUT = b"m8 fallback: send input\r\n"
INPUT_OK = b"m8 fallback: input OK 0x52\r\n"

# All-modules row: the integration acceptance image.
ALL_IMAGE_DIR = "build-m8-selftest"
ALL_IMAGE_NAME = "zephyros-m8-selftest.bin"
ALL_OK = b"m8 selftest: OK\r\n"
ALL_INPUT = b"m8 selftest: input hid.keyboard0 class 04\r\n"
ALL_OUTPUT = b"m8 selftest: output v9958.0 class 02\r\n"

# Failure rows: expected instance + class per direction, the registered
# instance names in load order, the exact early-SIO diagnostics and the
# observable direction contract.  Mirrors tools/serial_smoke.py.
FAILURE_ROWS = (
    {
        "name": "nohid",
        "dir": "build-m8-fb-nohid",
        "image": "zephyros-m8-fb-nohid.bin",
        "input": (b"sio0.b", ZDEV_SERIAL),
        "output": (b"v9958.0", ZDEV_VIDEO),
        "devices": [b"sio0.b", b"ioc.0", b"v9958.0"],
        "diagnostics": (b"hid: unavailable 0x05\r\n",),
        "scripted_input": True,
        "serial_marker": False,
    },
    {
        "name": "nohid-nov9958",
        "dir": "build-m8-fb-nohid-nov9958",
        "image": "zephyros-m8-fb-nohid-nov9958.bin",
        "input": (b"sio0.b", ZDEV_SERIAL),
        "output": (b"sio0.b", ZDEV_SERIAL),
        "devices": [b"sio0.b", b"ioc.0"],
        "diagnostics": (b"hid: unavailable 0x05\r\n",
                        b"v9958: unavailable 0x05\r\n"),
        "scripted_input": True,
        "serial_marker": True,
    },
    {
        "name": "badioc",
        "dir": "build-m8-fb-badioc",
        "image": "zephyros-m8-fb-badioc.bin",
        "input": (b"sio0.b", ZDEV_SERIAL),
        "output": (b"v9958.0", ZDEV_VIDEO),
        "devices": [b"sio0.b", b"fake.ioc", b"v9958.0"],
        "diagnostics": (b"hid: unavailable 0x0B\r\n",),
        "scripted_input": True,
        "serial_marker": False,
    },
    {
        "name": "arena",
        "dir": "build-m8-fb-arena",
        "image": "zephyros-m8-fb-arena.bin",
        "input": (b"hid.keyboard0", ZDEV_HID),
        "output": (b"sio0.b", ZDEV_SERIAL),
        "devices": [b"sio0.b", b"ioc.0", b"hid.keyboard0"],
        "diagnostics": (b"v9958: unavailable 0x06\r\n",),
        "scripted_input": False,
        "serial_marker": True,
    },
)


def load_sim(directory, image_name):
    """A ConsoleSimBase instance bound to one image, or None when the
    image has not been built."""
    path = os.path.join(ROOT, directory, image_name)
    if not os.path.exists(path):
        return None
    sim = ConsoleSimBase()
    sim.IMAGE_SUBDIR = directory
    sim.IMAGE_NAME = image_name
    sim.setUp()
    return sim


def boot_image(sim, marker, send_input=False, limit=STEP_LIMIT):
    """Boot and step until `marker` appears in the serial transcript,
    sending the invited byte when the image asks and the row scripts it.
    Returns (cpu, transcript)."""
    z = sim.make_cpu()
    sim.start_kernel(z)
    sent = False
    for i in range(limit):
        sim.skip_delay(z)
        z.step()
        if i % 0x1FFF == 0:
            tx = bytes(z.sio_tx)
            if send_input and not sent and SEND_INPUT in tx:
                z.sio_rx.append(INPUT_BYTE)
                sent = True
            if marker in tx:
                sim.pump(z)
                return z, bytes(z.sio_tx)
    sim.pump(z)
    raise AssertionError(
        "did not reach %r (PC=%04X, tail=%r)"
        % (marker, z.pc, bytes(z.sio_tx)[-200:]))


def registered(sim, z):
    return [sim.bank7_cstr(z, dev["name"]) for dev in sim.devices(z)]


class TestM8FallbackMatrix(unittest.TestCase):
    def test_all_modules_image_binds_hid_v9958(self):
        sim = load_sim(ALL_IMAGE_DIR, ALL_IMAGE_NAME)
        if sim is None:
            self.skipTest("build-m8-selftest missing; build the image first")
        z, tx = boot_image(sim, ALL_OK)
        self.assertEqual(
            z.bank7[sim.syms["_m8_selftest_result"]], 0x00)
        self.assertIn(ALL_INPUT, tx)
        self.assertIn(ALL_OUTPUT, tx)
        self.assertNotIn(b": unavailable 0x", tx)
        self.assertNotIn(b"FAIL", tx)
        devices = sim.devices(z)
        self.assertEqual(registered(sim, z),
                         [b"sio0.b", b"ioc.0", b"hid.keyboard0",
                          b"v9958.0"])
        hid = [d for d in devices if d["class"] == ZDEV_HID][0]
        vdp = [d for d in devices if d["class"] == ZDEV_VIDEO][0]
        self.assertEqual(z.bank7[sim.syms["_console_in_handle"]],
                         hid["handle"])
        self.assertEqual(z.bank7[sim.syms["_console_out_handle"]],
                         vdp["handle"])
        # The unchanged monitor launcher follows the target test.
        sim.run_to(z, PROMPT)
        self.assertIn(BANNER, "".join(z.glyphs))

    def test_failure_variants(self):
        for row in FAILURE_ROWS:
            with self.subTest(row=row["name"]):
                self.run_row(row)

    def run_row(self, row):
        sim = load_sim(row["dir"], row["image"])
        if sim is None:
            self.skipTest("%s missing; build the image first" % row["dir"])
        z, tx = boot_image(sim, SELFTEST_OK,
                           send_input=row["scripted_input"])
        self.assertNotIn(SELFTEST_FAIL, tx)
        self.assertEqual(
            z.bank7[sim.syms["_m8_fallback_selftest_result"]], 0x00)

        # Exact early-SIO diagnostics, exactly once each.
        for needle in row["diagnostics"]:
            self.assertEqual(tx.count(needle), 1,
                             "%s: diagnostic %r not exactly once"
                             % (row["name"], needle))

        # The registered set proves rollback/absence kept earlier
        # residents and left no failed module behind.
        self.assertEqual(registered(sim, z), row["devices"],
                         "%s: registry differs" % row["name"])

        # Exact bound pair, from the live device table.
        devices = sim.devices(z)
        in_name, in_class = row["input"]
        out_name, out_class = row["output"]
        dev_in = [d for d in devices if d["class"] == in_class and
                  sim.bank7_cstr(z, d["name"]) == in_name]
        dev_out = [d for d in devices if d["class"] == out_class and
                   sim.bank7_cstr(z, d["name"]) == out_name]
        self.assertTrue(dev_in and dev_out,
                        "%s: expected devices not registered" % row["name"])
        self.assertEqual(z.bank7[sim.syms["_console_in_handle"]],
                         dev_in[0]["handle"])
        self.assertEqual(z.bank7[sim.syms["_console_out_handle"]],
                         dev_out[0]["handle"])

        # Direction contract: a serial-bound input must have been read
        # back through the console service; the marker must be visible
        # on exactly the claimed output channel.
        if row["scripted_input"]:
            self.assertIn(INPUT_OK, tx, "%s: scripted input lost"
                          % row["name"])
            self.assertNotIn(b"m8 fallback: input none", tx)
        if row["serial_marker"]:
            self.assertIn(CONSOLE_MARKER, tx,
                          "%s: serial marker missing" % row["name"])
        else:
            self.assertNotIn(CONSOLE_MARKER, tx,
                             "%s: V9958-bound marker leaked to serial"
                             % row["name"])
            self.assertIn("m8 fallback: console output",
                          "".join(z.glyphs),
                          "%s: marker not visible in the V9958 trace"
                          % row["name"])

        # Early-SIO independence: the control transcript above exists
        # while the bound output may be the V9958 (display-only rows),
        # and the banner always comes from the direct early path.
        self.assertIn(b"ZephyrOS 0.0", tx)


if __name__ == "__main__":
    unittest.main()
