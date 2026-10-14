#!/usr/bin/env python3
"""Console-serial variant acceptance: replay the two
build-selection images in the Z80 simulator.

The console implementation is selected per build (architecture 5.3).  These two images cover the `console-serial`
half of the selection; the production `console-v9958-hid` half is replayed
by tests/test_console_acceptance.py.

* `build-console-serial-selftest` is the production image set with
  `console-serial` in the CONSOLE slot: the loader places the same six
  driver images, and every dynamic console byte - `console: driver OK`,
  the shell's prompt, a typed `pwd`, the store's monitor run as the
  shell's child with its answer to a typed `r`, and the launcher's exit
  line - arrives on the serial lane, while the V9958 trace carries no
  console text at all (the console never routes there).
* `build-console-serial-fail-selftest` boots the same set and
  `drivers.man` from a store that omits `console-serial.mod`: the
  required driver cannot be loaded, so the loader names it on the early
  console and the nucleus stops the boot there - an explicit boot failure
  (5.3), never a fallback - and the launcher is never reached.
* `build-driver-init-fail-selftest` boots the one
  test-only driver image whose init returns non-zero: the loader's
  init-status branch refuses the image, names it on the early console and
  the nucleus stops the boot the same way.  This is the initialization
  half of the required-driver failure row; the missing-image store above
  is the load half.

The serial configuration is a build selection, and its outcome is the
shell's prompt on the serial lane.

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
from replay import SHELL_PROMPT, STEP_LIMIT  # noqa: E402
from test_console_acceptance import (  # noqa: E402
    BANNER, PROMPT, ConsoleReplayBase)

# The launcher's line when the boot program returns: the shell reports
# its status and starts again (the production policy).
SHELL_EXIT = b"boot: /system/shell.exe exit 0\r\n"

# The production set with the serial console, in manifest (load) order,
# with the endpoint class each image registers.
DRIVER_SET = (("sio.mod", 1), ("ioc.mod", 5), ("hid.mod", 4),
               ("v9958.mod", 2), ("console-serial.mod", 6),
               ("timer.mod", 10), ("iocfat.mod", 7))

CONSOLE_DRIVER_OK = b"console: driver OK\r\n"
SERIAL_SYSINFO = \
    b"system: abi 01 app 10 os 18 common E000 kernel-bank 07 romfs 02\r\n"
# The executive's dispatch counters, one line below the platform rows.
# This replay leaves the CTC unmodelled, so no interrupt has been
# dispatched and `last` is the executive's "nothing yet" source.
SERIAL_IRQ_LINE = \
    b"irq: unexpected 0000 unclaimed 0000 last 07\r\n"

# The required driver's failure: the loader names the image it could not
# find, the nucleus reports the manifest failure and stops.
LOADER_NO_IMAGE = b"driver loader: console-serial.mod: no image\r\n"
MANIFEST_UNAVAILABLE = b"drivers: unavailable 0x05\r\n"
CONSOLE_DRIVER_FAILED = b"console: driver failed\r\n"

# The init-status refusal: the image loaded, its init
# returned non-zero, and the loader's failure line names it.
LOADER_INVALID = b"driver loader: init_fail.mod: invalid\r\n"


class SerialBase(ConsoleReplayBase):
    """Boot one `console-serial` image with the emulated peers.

    Both images are smoke images: the kernel holds its banner until the
    host sync sequence arrives (tools/serial_smoke.py sends it on every
    open), so the replay preloads the same two bytes."""

    HOST_SYNC = b"\xaa\x55"

    def setUp(self):
        if type(self) is SerialBase:
            self.skipTest("abstract serial harness")
        super().setUp()

    def driver_plan(self):
        """The nucleus ledger the packer's set report carries for this
        image (tools/modpack.py plan drivers.plan.json)."""
        path = os.path.join(ROOT, self.IMAGE_SUBDIR, "modules",
                            "drivers.plan.json")
        with open(path, "r") as handle:
            return json.load(handle)["nucleus"]

    def manifest_names(self):
        """The store manifest tools/systemgen.py wrote for this image."""
        path = os.path.join(ROOT, self.IMAGE_SUBDIR, "store.json")
        with open(path) as handle:
            return [entry["name"] for entry in json.load(handle)["files"]]

    def store_entries(self):
        path = os.path.join(ROOT, self.IMAGE_SUBDIR, "recovery.romfs")
        with open(path, "rb") as handle:
            _header, entries = romfspack.parse_image(handle.read())
        return {entry["name"]: entry for entry in entries}

    def boot_serial_to_prompt(self, z, limit=STEP_LIMIT,
                              prompt=SHELL_PROMPT):
        """Step until `prompt` appears on the serial lane."""
        for step in range(limit):
            self.skip_delay(z)
            z.step()
            if step % 0x1FFF == 0:
                self.pump(z)
                if prompt.encode("ascii") in bytes(z.sio_tx):
                    return step
        raise AssertionError("no %r prompt on the serial lane "
                             "(PC=%04X, tail=%r)"
                             % (prompt, z.pc, bytes(z.sio_tx)[-160:]))

    def type_serial(self, z, text, expect, limit=STEP_LIMIT):
        """Queue a line on the serial input lane and wait for its output.

        The console-serial variant's input is the SERIAL driver, so the
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
    IMAGE_NAME = "konaos-console-serial-selftest.bin"

    def test_the_shell_runs_on_the_serial_lane(self):
        z = self.start()
        self.boot_serial_to_prompt(z)
        sio = bytes(z.sio_tx)
        trace = self.read_trace(z)

        # The loader placed the production image set with the serial
        # console, in manifest order, and the ledger matches the packer's
        # plan.  The evidence rows are linked for the variant image.
        for name in ("sio.mod", "ioc.mod", "hid.mod", "v9958.mod",
                     "console-serial.mod", "timer.mod", "iocfat.mod"):
            self.assertIn(("driver %s: hot " % name).encode("ascii"), sio)
        self.assertIn(b"driver loader: 7 images in bank 6\r\n", sio)
        plan = self.driver_plan()
        self.assertIn(b"driver hot: %d bytes of 32768\r\n"
                      % plan["driver_hot_bytes"], sio)
        self.assertIn(b"driver boot/bulk: %d bytes of 24576\r\n"
                      % plan["driver_boot_bytes"], sio)
        self.assertNotIn(b": unavailable 0x", sio)

        # The whole dynamic console is this lane: the boot line and the
        # boot program's prompt.  The launcher writes nothing until the
        # run ends.
        self.assertIn(CONSOLE_DRIVER_OK, sio)
        self.assertIn(SHELL_PROMPT.encode("ascii"), sio)
        self.assertNotIn(b"console: unbound", sio)
        self.assertNotIn(CONSOLE_DRIVER_FAILED, sio)
        self.assertNotIn(b"boot: ", sio)

        # The V9958 driver is loaded but the console never routes to it:
        # no console text may appear in the glyph trace.
        self.assertNotIn(SHELL_PROMPT, trace)
        self.assertNotIn(BANNER, trace)
        self.assertNotIn(PROMPT, trace)

        # Typing on the serial lane reaches the shell through the
        # console: the CWD row comes back on the same lane.
        self.type_serial(z, b"pwd\r", b"/\r\n")

        # The store's monitor runs as the shell's child and answers its
        # `r` on the same lane; `q` returns to the shell, not the
        # launcher.  The wait is on the row the `R` block ends with, so
        # the whole block is on the lane when both are asserted.
        self.type_serial(z, b"monitor\r", PROMPT.encode("ascii"))
        self.type_serial(z, b"r\r", SERIAL_IRQ_LINE)
        self.assertIn(SERIAL_SYSINFO, bytes(z.sio_tx))
        self.assertIn(SERIAL_IRQ_LINE, bytes(z.sio_tx))
        self.type_serial(z, b"q\r", SHELL_PROMPT.encode("ascii"))
        self.assertNotIn(b"boot: ", bytes(z.sio_tx))

        # `exit` ends the run: the launcher reports the status and starts
        # the shell again (the production policy).
        self.type_serial(z, b"exit\r", SHELL_EXIT)
        self.wait_serial_count(z, SHELL_PROMPT.encode("ascii"), 2)

    def test_store_carries_the_serial_console(self):
        entries = self.store_entries()
        self.assertEqual(sorted(entries), sorted(self.manifest_names()))
        self.assertIn("console-serial.mod", entries)
        self.assertIn("shell.exe", entries)
        self.assertIn("monitor.exe", entries)


class TestRequiredConsoleFailure(SerialBase):
    """`build-console-serial-fail-selftest`: the required driver is absent
    from the store, so the boot must fail explicitly on the early console."""

    IMAGE_SUBDIR = "build-console-serial-fail-selftest"
    IMAGE_NAME = "konaos-console-serial-fail-selftest.bin"

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
        for name in ("sio.mod", "ioc.mod", "hid.mod", "v9958.mod"):
            self.assertIn(("driver %s: hot " % name).encode("ascii"), sio)
        self.assertNotIn(b"driver console-serial.mod:", sio)
        self.assertNotIn(b"driver iocfat.mod:", sio)
        self.assertIn(LOADER_NO_IMAGE, sio)
        self.assertIn(MANIFEST_UNAVAILABLE, sio)
        self.assertIn(CONSOLE_DRIVER_FAILED, sio)

        # No fallback and no launcher: the console is not up, the monitor
        self.assertNotIn(BANNER.encode("ascii"), sio)
        self.assertNotIn(PROMPT.encode("ascii"), sio)
        self.assertNotIn(b"boot: ", sio)

        after = len(sio)
        for _ in range(20000):
            z.step()
        self.assertEqual(len(bytes(z.sio_tx)), after)

    def test_store_omits_only_the_console_image(self):
        entries = self.store_entries()
        self.assertEqual(sorted(entries), sorted(self.manifest_names()))
        self.assertNotIn("console-serial.mod", entries)

        # The boot manifest still lists the console: the store is what is
        # incomplete, which is the failure this image exercises.
        with open(os.path.join(ROOT, self.IMAGE_SUBDIR, "modules",
                               "drivers.man"), "rb") as handle:
            data = handle.read()
        listed = [data[i:i + 24].split(b"\0")[0].decode()
                  for i in range(0, len(data), 24)]
        self.assertEqual(listed, [name for name, _cls in DRIVER_SET])


class TestRequiredConsoleInitFailure(SerialBase):
    """`build-driver-init-fail-selftest`: the one
    required driver image's init refuses, so the boot must fail explicitly
    on the early console through the loader's init-status branch."""

    IMAGE_SUBDIR = "build-driver-init-fail-selftest"
    IMAGE_NAME = "konaos-driver-init-fail-selftest.bin"

    def run_to_halt(self, z, limit=STEP_LIMIT):
        for step in range(limit):
            self.skip_delay(z)
            z.step()
            if z.halted:
                return
            if step % 0x1FFF == 0:
                self.pump(z)
        raise AssertionError("the kernel never halted (PC=%04X)" % z.pc)

    def test_boot_fails_on_the_refusing_driver(self):
        z = self.start()
        self.run_to_halt(z)
        sio = bytes(z.sio_tx)

        # The image was read (the manifest named it), then its init
        # refused: the loader's failure line names it and the nucleus
        # stops the boot before the launcher (5.3).
        self.assertIn(LOADER_INVALID, sio)
        self.assertIn(CONSOLE_DRIVER_FAILED, sio)

        # No console driver and no launcher: nothing follows the failure.
        self.assertNotIn(CONSOLE_DRIVER_OK, sio)
        self.assertNotIn(b"console: unbound", sio)
        self.assertNotIn(b"driver loader: 1 images", sio)
        self.assertNotIn(BANNER.encode("ascii"), sio)
        self.assertNotIn(b"boot: ", sio)

        after = len(sio)
        for _ in range(20000):
            z.step()
        self.assertEqual(len(bytes(z.sio_tx)), after)

    def test_store_and_manifest_carry_the_refusing_driver(self):
        entries = self.store_entries()
        self.assertEqual(sorted(entries), sorted(self.manifest_names()))
        self.assertIn("init_fail.mod", entries)

        # The boot manifest lists exactly the refusing image, and the
        # packed container refuses through its init symbol.
        with open(os.path.join(ROOT, self.IMAGE_SUBDIR, "modules",
                               "drivers.man"), "rb") as handle:
            data = handle.read()
        listed = [data[i:i + 24].split(b"\0")[0].decode()
                  for i in range(0, len(data), 24)]
        self.assertEqual(listed, ["init_fail.mod"])
        # The packed container carries no endpoint and no publication:
        # the failure is the init status alone.
        report_path = os.path.join(ROOT, self.IMAGE_SUBDIR, "modules",
                                   "init_fail.report.json")
        with open(report_path) as handle:
            report = json.load(handle)
        self.assertEqual(report["endpoints"], [])


if __name__ == "__main__":
    unittest.main()
