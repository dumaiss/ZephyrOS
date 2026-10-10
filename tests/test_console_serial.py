#!/usr/bin/env python3
"""Console-serial variant acceptance (work package 42e): replay the two
build-selection images in the Z80 simulator.

The console implementation is selected per build (architecture 36.11,
M10-REFERENCE decision 12).  These two images cover the `console-serial`
half of the selection; the production `console-v9958-hid` half is replayed
by tests/test_console_acceptance.py.

* `build-console-serial-selftest` is the production image set with
  `console-serial` in the CONSOLE slot: the loader places the same six
  driver images, and every dynamic console byte - `console: service OK`,
  the monitor banner and prompt, and the monitor's answer to a typed `r` -
  arrives on the serial lane, while the V9958 trace carries no monitor
  text at all (the console never routes there).
* `build-console-serial-fail-selftest` boots the same set and
  `services.man` from a store that omits `console-serial.zmod`: the
  required driver cannot be loaded, so the loader names it on the early
  console and the nucleus stops the boot there - an explicit boot failure
  (36.11), never a fallback - and the launcher is never reached.

The runtime-fallback images and their composition tests are retired with
the fallback they covered; the serial configuration is a build selection
now, and its outcome is the monitor prompt on the serial lane.

Skipped when the images have not been built.
"""

import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import romfspack  # noqa: E402
from test_console_sim import BANNER, MONITOR_EXIT, PROMPT  # noqa: E402
from test_console_acceptance import ConsoleReplayBase  # noqa: E402

# The production set with the serial console, in manifest (load) order,
# with the endpoint class each image registers.
SERVICE_SET = (("sio.zmod", 1), ("ioc.zmod", 5), ("hid.zmod", 4),
               ("v9958.zmod", 2), ("console-serial.zmod", 6),
               ("iocfat.zmod", 7))

CONSOLE_SERVICE_OK = b"console: service OK\r\n"
SERIAL_SYSINFO = \
    b"system: abi 01 app 10 os 18 common E000 kernel-bank 07 romfs 02\r\n"

# The required driver's failure: the loader names the image it could not
# find, the nucleus reports the manifest failure and stops.
LOADER_NO_IMAGE = b"service loader: console-serial.zmod: no image\r\n"
MANIFEST_UNAVAILABLE = b"services: unavailable 0x05\r\n"
CONSOLE_SERVICE_FAILED = b"console: service failed\r\n"

STEP_LIMIT = 20000000


class SerialBase(ConsoleReplayBase):
    """Boot one `console-serial` image with the emulated peers."""

    def setUp(self):
        if type(self) is SerialBase:
            self.skipTest("abstract serial harness")
        super().setUp()

    def service_plan(self):
        """The nucleus ledger the packer's set report carries for this
        image (tools/zmodpack.py services --report services.plan.json)."""
        path = os.path.join(ROOT, self.IMAGE_SUBDIR, "modules",
                            "services.plan.json")
        with open(path, "r") as handle:
            return json.load(handle)["nucleus"]

    def store_entries(self):
        path = os.path.join(ROOT, self.IMAGE_SUBDIR, "recovery.romfs")
        with open(path, "rb") as handle:
            _header, entries = romfspack.parse_image(handle.read())
        return {entry["name"]: entry for entry in entries}

    def boot_serial_to_prompt(self, z, limit=STEP_LIMIT):
        """Step until the monitor prompt appears on the serial lane."""
        for step in range(limit):
            self.skip_delay(z)
            z.step()
            if step % 0x1FFF == 0:
                self.pump(z)
                if PROMPT.encode("ascii") in bytes(z.sio_tx):
                    return step
        raise AssertionError("no monitor prompt on the serial lane "
                             "(PC=%04X, tail=%r)"
                             % (z.pc, bytes(z.sio_tx)[-160:]))

    def type_serial(self, z, text, expect, limit=STEP_LIMIT):
        """Queue a line on the serial input lane and wait for its output.

        The console-serial variant's input is the SERIAL service, so the
        bytes travel on SIO0/B (z.sio_rx), not the HID queue."""
        start = len(z.sio_tx)
        z.sio_rx.extend(text)
        for step in range(limit):
            self.skip_delay(z)
            z.step()
            if step % 0x1FFF == 0:
                self.pump(z)
                if expect in bytes(z.sio_tx[start:]):
                    return
        raise AssertionError("%r did not follow %r (tail=%r)"
                             % (expect, text,
                                bytes(z.sio_tx[start:])[-160:]))

    def wait_serial_count(self, z, expect, count, limit=STEP_LIMIT):
        for step in range(limit):
            self.skip_delay(z)
            z.step()
            if step % 0x1FFF == 0:
                if bytes(z.sio_tx).count(expect) >= count:
                    return
        raise AssertionError("%r did not appear %d times (tail=%r)"
                             % (expect, count, bytes(z.sio_tx)[-160:]))


class TestSerialVariant(SerialBase):
    """`build-console-serial-selftest`: the other CONSOLE driver."""

    IMAGE_SUBDIR = "build-console-serial-selftest"
    IMAGE_NAME = "zephyros-console-serial-selftest.bin"

    def test_monitor_runs_on_the_serial_lane(self):
        z = self.start()
        self.boot_serial_to_prompt(z)
        sio = bytes(z.sio_tx)
        trace = self.read_trace(z)

        # The loader placed the production image set with the serial
        # console, in manifest order, and the ledger matches the packer's
        # plan.  The evidence rows are linked for the variant image.
        for name in ("sio.zmod", "ioc.zmod", "hid.zmod", "v9958.zmod",
                     "console-serial.zmod", "iocfat.zmod"):
            self.assertIn(("driver %s: hot " % name).encode("ascii"), sio)
        self.assertIn(b"service loader: 6 images in bank 6\r\n", sio)
        plan = self.service_plan()
        self.assertIn(b"driver hot: %d bytes of 32768\r\n"
                      % plan["service_hot_bytes"], sio)
        self.assertIn(b"driver boot/bulk: %d bytes of 24576\r\n"
                      % plan["service_boot_bytes"], sio)
        self.assertNotIn(b": unavailable 0x", sio)

        # The whole dynamic console is this lane: the boot line, the
        # monitor banner and prompt.
        self.assertIn(CONSOLE_SERVICE_OK, sio)
        self.assertIn(BANNER.encode("ascii"), sio)
        self.assertIn(PROMPT.encode("ascii"), sio)
        self.assertNotIn(b"console: unbound", sio)
        self.assertNotIn(CONSOLE_SERVICE_FAILED, sio)

        # The V9958 driver is loaded but the console never routes to it:
        # no monitor text may appear in the glyph trace.
        self.assertNotIn(BANNER, trace)
        self.assertNotIn(PROMPT, trace)

        # Typing on the serial lane reaches the monitor through the
        # console: the system row comes back on the same lane.
        self.type_serial(z, b"r\r", SERIAL_SYSINFO)
        self.assertIn(SERIAL_SYSINFO, bytes(z.sio_tx))

        # `q` exits through the serial console and the launcher relaunches
        # the monitor (the same contract as production).
        self.type_serial(z, b"q\r", MONITOR_EXIT.encode("ascii"))
        self.wait_serial_count(z, BANNER.encode("ascii"), 2)

    def test_store_carries_the_serial_console(self):
        entries = self.store_entries()
        manifest = os.path.join(ROOT, "tests", "console",
                                "manifest-serial.json")
        with open(manifest) as handle:
            names = [entry["name"] for entry in json.load(handle)["files"]]
        self.assertEqual(sorted(entries), sorted(names))
        self.assertIn("console-serial.zmod", entries)
        self.assertIn("monitor.zex", entries)


class TestRequiredConsoleFailure(SerialBase):
    """`build-console-serial-fail-selftest`: the required driver is absent
    from the store, so the boot must fail explicitly on the early console."""

    IMAGE_SUBDIR = "build-console-serial-fail-selftest"
    IMAGE_NAME = "zephyros-console-serial-fail-selftest.bin"

    def run_to_halt(self, z, limit=STEP_LIMIT):
        for step in range(limit):
            self.skip_delay(z)
            z.step()
            if z.halted:
                return
            if step % 0x1FFF == 0:
                self.pump(z)
        raise AssertionError("the kernel never halted (PC=%04X)" % z.pc)

    def test_boot_fails_explicitly_on_the_missing_console(self):
        z = self.start()
        self.run_to_halt(z)
        sio = bytes(z.sio_tx)

        # Every image before the console loaded; the console image the
        # manifest names is not in the store.
        for name in ("sio.zmod", "ioc.zmod", "hid.zmod", "v9958.zmod"):
            self.assertIn(("driver %s: hot " % name).encode("ascii"), sio)
        self.assertNotIn(b"driver console-serial.zmod:", sio)
        self.assertNotIn(b"driver iocfat.zmod:", sio)
        self.assertIn(LOADER_NO_IMAGE, sio)
        self.assertIn(MANIFEST_UNAVAILABLE, sio)
        self.assertIn(CONSOLE_SERVICE_FAILED, sio)

        # No fallback and no launcher: the console is not up, the monitor
        # never runs, and nothing follows the failure line.
        self.assertNotIn(CONSOLE_SERVICE_OK, sio)
        self.assertNotIn(BANNER.encode("ascii"), sio)
        self.assertNotIn(PROMPT.encode("ascii"), sio)
        self.assertNotIn(b"monitor: exit status", sio)

        after = len(sio)
        for _ in range(20000):
            z.step()
        self.assertEqual(len(bytes(z.sio_tx)), after)

    def test_store_omits_only_the_console_image(self):
        entries = self.store_entries()
        manifest = os.path.join(ROOT, "tests", "console",
                                "manifest-serial-missing.json")
        with open(manifest) as handle:
            names = [entry["name"] for entry in json.load(handle)["files"]]
        self.assertEqual(sorted(entries), sorted(names))
        self.assertNotIn("console-serial.zmod", entries)

        # The boot manifest still lists the console: the store is what is
        # incomplete, which is the failure this image exercises.
        with open(os.path.join(ROOT, self.IMAGE_SUBDIR, "modules",
                               "services.man"), "rb") as handle:
            data = handle.read()
        listed = [data[i:i + 24].split(b"\0")[0].decode()
                  for i in range(0, len(data), 24)]
        self.assertEqual(listed, [name for name, _cls in SERVICE_SET])


if __name__ == "__main__":
    unittest.main()
