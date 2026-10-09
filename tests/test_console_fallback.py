#!/usr/bin/env python3
"""Console fallback matrix (work package 41): replay the two service-set
subsets the store can express.

In the resident-service model the console service *derives* its
composition at boot from the endpoints the store provided, so a fallback
row is a store manifest that lists fewer services - the M8 per-image boot
gates are gone.  Two variants are accepted:

* `nohid`          `sio ioc v9958 console`: no HID endpoint, so the input
  direction falls back to the serial service while the output stays on
  the video service.  The diagnostic's marker must appear on the V9958
  trace and never on the serial link, and the typed byte the replay sends
  on the serial lane must come back through the console.
* `nohid-nov9958`  `sio ioc console`: neither HID nor video, so both
  directions are the serial service (the serial-only row).  The marker
  appears on the serial link and the typed byte is read back from it.

Each image also boots its target diagnostic from the late hook, and the
replay pins the early-SIO transcript, the composition rows, the lane each
direction used and the packed store the image carried.

Skipped when the images have not been built.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from test_console_sim import ConsoleSimBase  # noqa: E402

RET = 0x1234
STEP_LIMIT = 12000000

MARKER = b"F41"
TYPED = b"K7\r"

FALLBACK_OK = b"console fallback: OK\r\n"


class FallbackBase(ConsoleSimBase):
    """Boot one subset image and read the diagnostic's lanes."""

    SERVICES = ()
    EXPECT_INPUT = None
    EXPECT_OUTPUT = None
    MARKER_ON_SERIAL = False

    def setUp(self):
        # The shared harness is not a variant: only the concrete classes
        # below carry a subset store and an expectation pair, and unittest
        # collects every TestCase subclass.
        if type(self) is FallbackBase:
            self.skipTest("abstract fallback harness")
        super().setUp()

    def start(self):
        z = self.make_cpu()
        self.start_kernel(z)
        return z

    def run_to_fallback_end(self, z, limit=STEP_LIMIT):
        # The typed line travels on the serial lane, and the kernel's boot
        # host-sync wait consumes whatever the receiver already holds, so
        # it is queued once the diagnostic has reported its composition.
        queued = False
        for step in range(limit):
            self.skip_delay(z)
            z.step()
            if not queued and b"console fallback: output " in z.sio_tx:
                z.sio_rx.extend(TYPED)
                queued = True
            if b"console fallback: FAIL" in z.sio_tx:
                break
            if FALLBACK_OK in z.sio_tx:
                break
            if step % 0x1FFF == 0:
                self.pump(z)
        else:
            raise AssertionError("the fallback diagnostic never finished "
                                 "(PC=%04X)" % z.pc)
        # Let the monitor launcher take over, then decode the V9958 side.
        for step in range(2000000):
            self.skip_delay(z)
            z.step()
            if step % 0x1FFF == 0:
                self.pump(z)
                if "Z80> " in "".join(z.glyphs):
                    break
        self.pump(z)

    def test_service_store_and_composition_lanes(self):
        z = self.start()
        self.run_to_fallback_end(z)
        sio = bytes(z.sio_tx).decode("ascii", "replace")
        trace = "".join(z.glyphs)

        # The loader started exactly the store's set, in manifest order.
        for name in self.SERVICES:
            self.assertIn("driver %s.zmod: hot " % name, sio)
        self.assertIn("service loader: %d images in bank 6\r\n"
                      % len(self.SERVICES), sio)

        # The composition rows the console derived, and the typed round
        # trip: the input direction really is the serial lane.
        self.assertIn("console fallback: input %s\r\n" % self.EXPECT_INPUT,
                      sio)
        self.assertIn("console fallback: output %s\r\n" % self.EXPECT_OUTPUT,
                      sio)
        self.assertIn("console fallback: echo 4B37", sio)  # "K7"
        self.assertIn(FALLBACK_OK.decode(), sio)
        self.assertNotIn("FAIL", sio)

        # The marker's lane is the output direction's proof.
        marker_serial = MARKER.decode() in sio
        marker_video = MARKER.decode() in trace
        self.assertEqual(marker_serial, self.MARKER_ON_SERIAL)
        self.assertEqual(marker_video, not self.MARKER_ON_SERIAL)
        self.assertIn("console: service OK", trace if not
                      self.MARKER_ON_SERIAL else sio)


class TestNoHidVariant(FallbackBase):
    IMAGE_SUBDIR = "build-console-fb-nohid"
    IMAGE_NAME = "zephyros-console-fb-nohid.bin"
    SERVICES = ("sio", "ioc", "v9958", "console")
    EXPECT_INPUT = "serial"
    EXPECT_OUTPUT = "video"
    MARKER_ON_SERIAL = False


class TestSerialOnlyVariant(FallbackBase):
    IMAGE_SUBDIR = "build-console-fb-nohid-nov9958"
    IMAGE_NAME = "zephyros-console-fb-nohid-nov9958.bin"
    SERVICES = ("sio", "ioc", "console")
    EXPECT_INPUT = "serial"
    EXPECT_OUTPUT = "serial"
    MARKER_ON_SERIAL = True


if __name__ == "__main__":
    unittest.main()
