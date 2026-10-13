"""Console integration acceptance tests: replay the real production bytes
and the integration acceptance image in the Z80 simulator.

Both images boot the resident-driver model: the recovery store lists the
SIO, IOC, HID, V9958 and CONSOLE drivers in the manifest order, the
nucleus driver loader places them in bank 6 and runs their boot tiers
and inits, and the build-selected console driver - not a kernel binding
and with no runtime fallback - forwards the console protocol to the
input/output drivers its manifest names.  The production image is the
same source and store the physical acceptance image is built from minus
the target-test objects; the integration image adds the target
diagnostics - the integration pass plus the IOC, HID and V9958 device
diagnostics - behind the early-SIO driver transcript, and the unchanged
`monitor.exe` launcher.

Coverage, all against those linked bytes:

* production boot: the early console prints its banner and nothing else
  (the driver transcript and the boot line `console: driver OK` travel
  over the console driver, i.e. on the V9958 lane), the store's manifest
  lists the five drivers in load order and every image is a driver
  container exporting exactly one endpoint at version 1;
* the scripted visible monitor session: the nucleus writes its boot line
  through the console driver, the monitor runs its banner and prompt on
  the V9958, a typed `r` returns the sysinfo/process/bank rows through
  the HID-input path the build selected, and `q` exits and relaunches;
* the IOC lane the store's filesystem driver (IOC-FAT) drives through the
  IOC endpoint: link bring-up plus the FS2 command exchange, seen from
  the emulated MCU peer;
* the integration image: the loader's per-driver evidence rows and
  ledger, the target diagnostics' endpoint/composition/marker/typed-line
  /PING/rejection transcript (and the IOC/HID/V9958 diagnostics the same
  image carries), the same bytes on the visible lane, the ESC c display
  reset the boot program runs, and the monitor session
  that follows it;
* ROMFS payload identity: the packed monitor artifact is stored
  byte-for-byte and the store's directory matches the image's manifest.

Skipped when the images have not been built.
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

from replay import (STEP_LIMIT, ConsoleReplay,  # noqa: E402
                    IntegrationInputs, integration_peer)
from z80sim import IocCommandPeer  # noqa: E402
import modpack  # noqa: E402
import romfspack  # noqa: E402
from test_v9958_sim import bringup_starts  # noqa: E402

SMOKE_HOST_SYNC = b"\xaa\x55"

# The packed monitor artifact (pinned by user/monitor/ and the tooling
# tests; the store must deliver these exact bytes).
MONITOR_SIZE = 6066
MONITOR_SHA256 = (
    "4d27328bd1ca820f156594ef47b5691e0c69369cc33f7da06cbf1772ccca7d18")

# The monitor's visible rows, decoded from the V9958 trace.
PROMPT = "Z80> "
BANNER = "KonaOS native monitor"
SYSINFO = "system: abi 01 app 10 os 18 common E000 kernel-bank 07 romfs 02"
PROCESS = "process: pid 0 bank 0 flags 0001"
BANK_INFO = "bank: current 00 mask 7F"
MONITOR_EXIT = "monitor: exit status 0x"
PRODUCTION_DIR = "build"
PRODUCTION_IMAGE = "konaos.bin"

INTEGRATION_DIR = "build-console-integration-selftest"
INTEGRATION_IMAGE = "konaos-console-integration-selftest.bin"

# The five driver images the production store carries, in manifest order,
# with the endpoint class each registers.  The production CONSOLE driver
# is `console-v9958-hid`; the serial variant selects
# `console-serial` from the same shared source.
DRIVER_SET = (("sio.mod", 1), ("ioc.mod", 5), ("hid.mod", 4),
               ("v9958.mod", 2), ("console-v9958-hid.mod", 6),
               ("iocfat.mod", 7))

# The console image's required dependencies (class, instance), in
# manifest order.  `console-serial` is exercised by test_console_serial.
CONSOLE_DEPS = {"console-v9958-hid.mod": ((4, 0), (2, 0))}

# The characters the integration diagnostic writes through the console
# driver and the line the replay queues for its input row.
INTEGRATION_MARKER = "C41"
INTEGRATION_LINES = (
    b"console integration: drivers sio=01 ioc=01 hid=01 video=01 "
    b"console=01\r\n",
    b"console integration: console v01\r\n",
    b"console integration: output video\r\n",
    b"console integration: input hid\r\n",
    b"console integration: ioc level 4A\r\n",
    b"console integration: rejected calls OK\r\n",
    b"console integration: OK\r\n",
)

# IOC peer traffic: the lane's one bounded bring-up, then the IOC-FAT
# driver's FS2 exchange through its resolved IOC endpoint.
LINK_SYNC_REQUEST = 0x0C
PING_REQUEST = 0x01
PING_RESPONSE = 0x81


class ConsoleReplayBase(ConsoleReplay, unittest.TestCase):
    """Boot one image with the emulated peers the drivers talk to."""

    IMAGE_SUBDIR = PRODUCTION_DIR
    IMAGE_NAME = PRODUCTION_IMAGE
    HOST_SYNC = b""
    # The merged image's four diagnostics need their scripted inputs;
    # production carries none of them and reads the monitor's typed
    # input instead.
    INPUT_STREAMS = False

    def setUp(self):
        self.load_image()

    def make_peer(self):
        return IocCommandPeer()

    def start(self):
        """Boot with the emulated peers; the integration image's input
        streams are attached to the stepping helpers."""
        z = self.make_cpu()
        z.hid = z.ioc_peer = self.make_peer()
        if self.INPUT_STREAMS:
            self.input_driver = IntegrationInputs(z.ioc_peer)
        self.start_kernel(z, host_sync=self.HOST_SYNC)
        return z

    def read_sio(self, z):
        return bytes(z.sio_tx)

    def read_trace(self, z):
        return self.trace(z)

    def type_line(self, z, text, expect, limit=STEP_LIMIT):
        self.send_raw(z, text, expect, limit=limit)

    def wait_for_count(self, z, expect, count, limit=STEP_LIMIT):
        self.run_until_count(z, expect, count, limit=limit)


class TestProductionImage(ConsoleReplayBase):
    """`build/konaos.bin`: the store's five drivers and the monitor."""

    def test_boot_lanes_and_driver_store(self):
        z = self.start()
        self.boot_to_prompt(z)
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
        self.assertNotIn(b"console integration", sio)

        # The boot line the nucleus prints through the console driver.
        self.assertIn("console: driver OK", trace)
        self.assertNotIn("console: driver OK", sio.decode("ascii",
                                                            "replace"))
        self.assertIn(BANNER, trace)
        self.assertIn(PROMPT, trace)

        # The production image links no acceptance symbol at all.
        for name in ("_kos_run_console_integration_selftest",
                     "_ace_integration_result", "_kos_target_test_late"):
            self.assertNotIn(name, self.syms)

        self.assert_driver_store()

    def test_monitor_session_reads_through_the_console(self):
        z = self.start()
        self.boot_to_prompt(z)
        self.type_line(z, b"r\r", SYSINFO)
        # `X` is the launcher's own exec path in the monitor: a missing
        # argument reports the monitor's ARG error and an
        # absent program the native code, both back at the prompt.
        self.type_line(z, b"X\r", "ERR: ARG")
        self.type_line(z, b"X /system/nosuch.exe\r", "ERR: 05")
        self.type_line(z, b"q\r", MONITOR_EXIT)
        self.wait_for_count(z, BANNER, 2)  # launcher relaunch
        trace = self.read_trace(z)
        for line in (SYSINFO, PROCESS, BANK_INFO):
            self.assertIn(line, trace)
        self.assertGreaterEqual(trace.count(BANNER), 2)

    def test_ioc_lane_serves_the_filesystem_driver(self):
        z = self.start()
        self.boot_to_prompt(z)
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
            "console-v9958-hid": ["INPUT", "VIDEO"],
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


class TestIntegrationImage(ConsoleReplayBase):
    """`build-console-integration-selftest`: the production driver set
    plus the target diagnostic and the same monitor launcher."""

    IMAGE_SUBDIR = INTEGRATION_DIR
    IMAGE_NAME = INTEGRATION_IMAGE
    HOST_SYNC = SMOKE_HOST_SYNC
    INPUT_STREAMS = True

    def make_peer(self):
        return integration_peer()

    def driver_plan(self):
        """The nucleus ledger the packer's set report carries for this
        image (tools/modpack.py plan drivers.plan.json)."""
        path = os.path.join(ROOT, INTEGRATION_DIR, "modules",
                            "drivers.plan.json")
        with open(path, "r") as handle:
            return json.load(handle)["nucleus"]

    def test_diagnostic_transcript_and_visible_lanes(self):
        z = self.start()
        self.boot_to_prompt(z)
        sio = self.read_sio(z)
        trace = self.read_trace(z)

        # Loader evidence: one row per image, in manifest order, plus the
        # ledger the packer's report pins.
        for name in ("sio.mod", "ioc.mod", "hid.mod", "v9958.mod",
                     "console-v9958-hid.mod"):
            self.assertIn(("driver %s: hot " % name).encode(), sio)
        self.assertIn(b"driver loader: 5 images in bank 6\r\n", sio)
        # The loader's two ledgers must equal what the packer's plan
        # (drivers.plan.json) reports for the same set.
        plan = self.driver_plan()
        self.assertIn(b"driver hot: %d bytes of 32768\r\n"
                      % plan["driver_hot_bytes"], sio)
        self.assertIn(b"driver boot/bulk: %d bytes of 24576\r\n"
                      % plan["driver_boot_bytes"], sio)
        for line in INTEGRATION_LINES:
            self.assertIn(line, sio)
        self.assertNotIn(b"console integration: FAIL", sio)
        self.assertNotIn(b"iocfat", sio)

        # The early SIO lane never carries the console-driver output: the
        # boot line, the diagnostic's marker and its typed echo are all on
        # the visible lane.
        self.assertNotIn(b"console: driver OK", sio)
        self.assertIn("console: driver OK", trace)
        self.assertIn(INTEGRATION_MARKER, trace)
        self.assertIn("K41", trace)          # the typed echo
        self.assertIn("integration done", trace)

        # The image boot program is the ESC c check:
        # it reset the display and printed its line before handing the
        # console to the monitor.  The V9958 bring-up sequence runs three
        # times because the video diagnostic lives in this image: the
        # driver's boot segment, the diagnostic's
        # KVIDEO_RESET, and the ESC c display reset.
        self.assertIn("escc: display reset", trace)
        self.assertEqual(len(bringup_starts(z.vdp)), 3)
        self.assertIn(BANNER, trace)
        self.assertIn(PROMPT, trace)

        # The IOC traffic the diagnostic drove: the driver's link
        # bring-up, then the HID fetch its console read drove and its
        # PING.
        requests = [entry for entry in z.ioc_peer.requests]
        kinds = [entry[0] for entry in requests]
        self.assertEqual(kinds[0], LINK_SYNC_REQUEST)
        self.assertIn(0x0E, kinds)           # CMD_HID_INPUT
        self.assertIn(PING_REQUEST, kinds)
        # Two PINGs hit the lane: the IOC diagnostic's own echo and the
        # integration diagnostic's cross-driver row, whose payload is
        # the pinned ASCII pattern.
        pings = [entry for entry in requests if entry[0] == PING_REQUEST
                 and entry[3] == b"@ABC"]
        self.assertEqual(len(pings), 1)
        self.assertEqual(pings[0][2], 0)
        # Every stream the merged diagnostics read came from the MCU:
        # the HID typed pass and fault pair, the console pair and the
        # integration diagnostic's typed line.
        self.assertEqual(z.ioc_peer.hid_delivered,
                         len(IntegrationInputs.TYPED) +
                         len(IntegrationInputs.FAULT_PAIR) +
                         len(IntegrationInputs.CONSOLE_PAIR) +
                         len(IntegrationInputs.TYPED_LINE))
        self.assertEqual(z.ioc_peer.channel_resets, 1)

    def test_monitor_runs_after_the_diagnostic(self):
        z = self.start()
        self.boot_to_prompt(z)
        self.type_line(z, b"q\r", MONITOR_EXIT)
        self.wait_for_count(z, BANNER, 2)
        trace = self.read_trace(z)
        self.assertGreaterEqual(trace.count(BANNER), 2)


if __name__ == "__main__":
    unittest.main()
