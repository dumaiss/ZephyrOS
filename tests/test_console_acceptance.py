"""Console acceptance tests: replay the real production bytes in the Z80
simulator.

The image boots the resident-driver model: the recovery store lists the
SIO, IOC, HID, V9958 and CONSOLE drivers in the manifest order, the
nucleus driver loader places them in bank 6 and runs their boot tiers
and inits, and the build-selected console driver - not a kernel binding
and with no runtime fallback - forwards the console protocol to the
input/output drivers its manifest names.  The production image is the
same source and store the physical acceptance image is built from.

Coverage, all against those linked bytes:

* production boot: the early console prints its banner and nothing else
  (the driver transcript and the boot line `console: driver OK` travel
  over the console driver, i.e. on the V9958 lane), the store's manifest
  lists the drivers in load order and every image is a driver
  container exporting exactly one endpoint at version 1;
* the shell boot and the scripted visible monitor session: the nucleus
  writes its boot line through the console driver, the shell is the boot
  program and prompts on the V9958, `monitor` starts the store's monitor
  as the shell's child, a typed `r` returns the sysinfo/process/bank rows
  through the HID-input path the build selected, and `q` returns to the
  shell - the launcher is not involved, so no `boot:` line is written;
* the IOC lane the store's filesystem driver (IOC-FAT) drives through the
  IOC endpoint: link bring-up plus the FS2 command exchange, seen from
  the emulated MCU peer;
* ROMFS payload identity: the packed monitor artifact is stored
  byte-for-byte and the store's directory matches the image's manifest.

Skipped when the image has not been built.
"""

import hashlib
import json
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from replay import (SHELL_PROMPT, STEP_LIMIT, ConsoleReplay)  # noqa: E402
from z80sim import IocCommandPeer  # noqa: E402
import modpack  # noqa: E402
import romfspack  # noqa: E402

# The packed monitor artifact (pinned by user/monitor/ and the tooling
# tests; the store must deliver these exact bytes).  The monitor grew by
# the B (reboot) command, by the `irq:` row's counters and the SDK's
# longer system-info record, by the SDK's buffered console and by the
# idle wait between input polls, so the pinned identity moved with each
# relink.
MONITOR_SIZE = 6790
MONITOR_SHA256 = (
    "3f73f28b46ee928d08917045f68f9152b470339729da3264c6d6ea3f59bbce49")

# The monitor's visible rows, decoded from the V9958 trace.
PROMPT = "Z80> "
BANNER = "KonaOS native monitor"
SYSINFO = "system: abi 01 app 10 os 18 common E000 kernel-bank 07 romfs 02"
# The monitor runs as the shell's child here, so its rows name the child's
# slot and bank (and no kernel-parent flag).
PROCESS = "process: pid 1 bank 1 flags 0000"
BANK_INFO = "bank: current 01 mask 7F"
# The executive's dispatch counters: nothing has been unexpected or
# unclaimed, and the last source dispatched is the timer driver's tick
# (IRQ_CTC2, 2), the one interrupt the production image runs while the
# monitor waits for its input.
IRQ_LINE = "irq: unexpected 0000 unclaimed 0000 last 02"
PRODUCTION_DIR = "build"
PRODUCTION_IMAGE = "konaos.bin"

# The driver images the production store carries, in manifest order, with
# the endpoint class each registers.  The production CONSOLE driver is
# `console-v9958-hid`; the serial variant selects `console-serial` from
# the same shared source.
DRIVER_SET = (("sio.mod", 1), ("ioc.mod", 5), ("hid.mod", 4),
               ("v9958.mod", 2), ("console-v9958-hid.mod", 6),
               ("timer.mod", 10), ("sound.mod", 9), ("iocfat.mod", 7))

# The console image's required dependencies (class, instance), in
# manifest order: HID input, VIDEO output, SERIAL as the rescue path.
# `console-serial` is exercised by test_console_serial.
CONSOLE_DEPS = {"console-v9958-hid.mod": ((4, 0), (2, 0), (1, 0))}

# IOC peer traffic: the lane's one bounded bring-up, then the IOC-FAT
# driver's FS2 exchange through its resolved IOC endpoint.
LINK_SYNC_REQUEST = 0x0C
PING_REQUEST = 0x01
PING_RESPONSE = 0x81


class ConsoleReplayBase(ConsoleReplay, unittest.TestCase):
    """Boot one image with the emulated peers the drivers talk to."""

    IMAGE_SUBDIR = PRODUCTION_DIR
    IMAGE_NAME = PRODUCTION_IMAGE

    def setUp(self):
        self.load_image()

    def make_peer(self):
        return IocCommandPeer()

    def start(self):
        """Boot with the emulated peers."""
        z = self.make_cpu()
        z.hid = z.ioc_peer = self.make_peer()
        # A smoke image (KOS_SMOKE_HOST_SYNC) holds its first byte until
        # the host's sync sequence arrives: preload it.
        self.start_kernel(z, host_sync=getattr(self, "HOST_SYNC", b""))
        return z

    def read_sio(self, z):
        return bytes(z.sio_tx)

    def read_trace(self, z):
        return self.trace(z)

    def type_line(self, z, text, expect, limit=STEP_LIMIT):
        self.send_raw(z, text, expect, limit=limit)


class TestProductionImage(ConsoleReplayBase):
    """`build/konaos.bin`: the store's drivers and the shell."""

    def test_boot_lanes_and_driver_store(self):
        z = self.start()
        self.boot_to_shell(z)
        sio = self.read_sio(z)
        trace = self.read_trace(z)

        # The early console prints its banner and nothing else: every
        # later line is produced through the console driver, so it
        # reaches the V9958 lane instead of the serial link.
        for line in (b"KonaOS 0.0\r\n", b"platform: Zephyr-80\r\n",
                     b"kernel entry: OK\r\n", b"serial: OK\r\n"):
            self.assertIn(line, sio)
        self.assertEqual(sio.count(b"\n"), 4, sio)
        self.assertNotIn(b": unavailable 0x", sio)
        self.assertNotIn(b"FAIL", sio)

        # The boot line the nucleus prints through the console driver,
        # and the shell it starts: the monitor is in the store but the
        # launcher has not run it.
        self.assertIn("console: driver OK", trace)
        self.assertNotIn("console: driver OK", sio.decode("ascii",
                                                            "replace"))
        self.assertIn(SHELL_PROMPT, trace)
        self.assertNotIn(BANNER, trace)
        self.assertNotIn(PROMPT, trace)

        # The production image links no acceptance symbol at all.
        for name in ("_kos_target_test_late", "_kos_target_test_early",
                     "_kos_target_test_syscall"):
            self.assertNotIn(name, self.syms)

        self.assert_driver_store()

    def test_the_shell_runs_the_monitor_session(self):
        z = self.start()
        self.boot_to_shell(z)
        # The shell is the boot program: it prompts on the visible lane
        # and the launcher writes no line until the run ends.
        self.assertIn(SHELL_PROMPT, self.read_trace(z))

        # `monitor` is found in the shell's path and runs as its child;
        # `X` is the monitor's own exec path: a missing argument reports
        # its ARG error and an absent program the native code.
        self.type_line(z, b"monitor\r", PROMPT)
        self.type_line(z, b"r\r", SYSINFO)
        self.type_line(z, b"X\r", "ERR: ARG")
        self.type_line(z, b"X /system/nosuch.exe\r", "ERR: 05")
        self.type_line(z, b"q\r", SHELL_PROMPT)
        trace = self.read_trace(z)
        for line in (SYSINFO, PROCESS, BANK_INFO, IRQ_LINE):
            self.assertIn(line, trace)
        self.assertEqual(trace.count(BANNER), 1)
        # The monitor was the shell's child: its exit is the shell's
        # business, not a launcher event.
        self.assertNotIn("boot: ", trace)
        self.assertNotIn("monitor: exit status", trace)

    def test_ioc_lane_serves_the_filesystem_driver(self):
        z = self.start()
        self.boot_to_shell(z)
        peer = z.ioc_peer
        requests = [entry[0] for entry in peer.requests]
        # The IOC driver's one bounded LINK_SYNC bring-up, then the
        # IOC-FAT driver's FS2 commands (the driver calls its
        # dependency directly; there is no class shim).
        self.assertEqual(requests[0], LINK_SYNC_REQUEST)
        self.assertIn(0x30, requests)  # CMD_FS2_CAPS
        self.assertEqual(peer.channel_resets, 1)
        self.assertEqual(peer.hunt_writes, 1)
        self.assertEqual(peer.rts_asserts, peer.rts_releases)
        self.assertTrue(peer.requests, "no command reached the peer")

    def test_store_payload_identity(self):
        store = os.path.join(ROOT, PRODUCTION_DIR, "recovery.romfs")
        entries = self.load_store(store)
        # The packed payloads are exactly the store manifest
        # tools/systemgen.py generated from systems/production.json.
        manifest = os.path.join(ROOT, PRODUCTION_DIR, "store.json")
        self.assertEqual(sorted(entries), sorted(self.manifest_names(
            manifest)))
        monitor = entries["monitor.exe"]["payload"]
        self.assertEqual(len(monitor), MONITOR_SIZE)
        self.assertEqual(hashlib.sha256(monitor).hexdigest(),
                         MONITOR_SHA256)

    def assert_driver_store(self):
        manifest = self.manifest_names(os.path.join(ROOT, "build",
                                                    "modules",
                                                    "drivers.man"))
        self.assertEqual(manifest, [name for name, _class in DRIVER_SET])
        for name, klass in DRIVER_SET:
            path = os.path.join(ROOT, "build", "modules", name)
            with open(path, "rb") as handle:
                info = modpack.parse_container(handle.read(), name)
            # The flat plan: one endpoint per image, and every
            # production image carries its publication.
            self.assertEqual([(ep["class"], ep["instance"], ep["version"])
                              for ep in info["endpoints"]],
                             [(klass, 0, 1)], name)
            self.assertEqual(len(info["publications"]), 1, name)
            self.assertEqual(info["publications"][0]["number"],
                             info["endpoints"][0]["number"], name)
            # The console image's required dependencies (no optional
            # dependency and no runtime fallback).  The
            # other images' dependencies are pinned by their own tests.
            if name in CONSOLE_DEPS:
                self.assertEqual([(dep["class"], dep["instance"])
                                  for dep in info["dependencies"]],
                                 list(CONSOLE_DEPS[name]), name)
                for dep in info["dependencies"]:
                    self.assertGreater(dep["number"], 0, name)

    def test_console_manifests_select_the_build(self):
        """The two CONSOLE manifests differ only in what the build
        selects, both require it, and the dependency
        is named symbolically: tools/systemgen.py resolves it against the
        selected set (tests/test_systemgen.py)."""
        expected = {
            "console-v9958-hid": ["INPUT", "VIDEO", "SERIAL"],
            "console-serial": ["SERIAL"],
        }
        for name, deps in expected.items():
            path = os.path.join(ROOT, "drivers", "console",
                                "%s.json" % name)
            with open(path) as handle:
                document = json.load(handle)
            self.assertEqual(document["name"], name)
            self.assertEqual(document["depends"], deps, name)
            for dep in document["depends"]:
                self.assertFalse(dep.endswith("?"), name)

    @staticmethod
    def manifest_names(path):
        """Entry names of a packed store or of a driver boot manifest."""
        if path.endswith("drivers.man"):
            with open(path, "rb") as handle:
                data = handle.read()
            return [data[i:i + 24].split(b"\0")[0].decode()
                    for i in range(0, len(data), 24)]
        with open(path) as handle:
            return [entry["name"] for entry in json.load(handle)["files"]]

    @staticmethod
    def load_store(path):
        with open(path, "rb") as handle:
            _header, entries = romfspack.parse_image(handle.read())
        return {entry["name"]: entry for entry in entries}


if __name__ == "__main__":
    unittest.main()
