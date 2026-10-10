#!/usr/bin/env python3
"""Console integration acceptance tests (work package 41): replay the real
production bytes and the integration acceptance image in the Z80 simulator.

Both images boot the resident-service model this package delivers: the
recovery store lists the SIO, IOC, HID, V9958 and CONSOLE services in the
manifest order, the nucleus service loader places them in bank 6 and runs
their boot tiers and inits, and the build-selected console driver - not a
kernel binding and with no runtime fallback - forwards the console
protocol to the input/output services its manifest names.  The production
image is the same
source and store the physical acceptance image is built from minus the
target-test objects; the integration image adds the target diagnostic
(tests/target/console/integration_test.c) behind the early-SIO driver
transcript and the unchanged M7 `monitor.zex` launcher.

Coverage, all against those linked bytes:

* production boot: the early console prints its banner and nothing else
  (the service transcript and the boot line `console: service OK` travel
  over the console service, i.e. on the V9958 lane), the store's manifest
  lists the five services in load order and every image is a service
  container exporting exactly one endpoint at version 1;
* the scripted visible monitor session: the nucleus writes its boot line
  through the console service, the monitor runs its banner and prompt on
  the V9958, a typed `r` returns the sysinfo/process/bank rows through
  the HID-input path the build selected, and `q` exits and relaunches;
* the IOC lane the store's filesystem service (IOC-FAT) drives through the
  IOC endpoint: link bring-up plus the FS2 command exchange, seen from
  the emulated MCU peer;
* the integration image: the loader's per-service evidence rows and
  ledger, the target diagnostic's endpoint/composition/marker/typed-line
  /PING/rejection transcript, the same bytes on the visible lane, the
  ESC c display reset the boot program runs (work package 42e), and the
  monitor session that follows it;
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
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import Z80, IocCommandPeer, V9958Peer, read_symbols  # noqa: E402
import zmodpack  # noqa: E402
import romfspack  # noqa: E402
from test_console_sim import (  # noqa: E402
    ConsoleSimBase, MONITOR_SIZE, MONITOR_SHA256, PROMPT, BANNER,
    SYSINFO, PROCESS, BANK_INFO, MONITOR_EXIT)
from test_v9958_sim import bringup_starts  # noqa: E402

RET = 0x1234
STEP_LIMIT = 12000000

PRODUCTION_DIR = "build"
PRODUCTION_IMAGE = "zephyros.bin"

INTEGRATION_DIR = "build-console-integration-selftest"
INTEGRATION_IMAGE = "zephyros-console-integration-selftest.bin"

# The five service images the production store carries, in manifest order,
# with the endpoint class each registers.  The production CONSOLE driver
# is `console-v9958-hid` (work package 42e); the serial variant selects
# `console-serial` from the same shared source.
SERVICE_SET = (("sio.zmod", 1), ("ioc.zmod", 5), ("hid.zmod", 4),
               ("v9958.zmod", 2), ("console-v9958-hid.zmod", 6),
               ("iocfat.zmod", 7))

# The console image's required dependencies (class, instance), in
# manifest order.  `console-serial` is exercised by test_console_serial.
CONSOLE_DEPS = {"console-v9958-hid.zmod": ((4, 0), (2, 0))}

# The characters the integration diagnostic writes through the console
# service and the line the replay queues for its input row.
INTEGRATION_MARKER = "C41"
INTEGRATION_TYPED = b"K41\r"
INTEGRATION_LINES = (
    b"console integration: services sio=01 ioc=01 hid=01 video=01 "
    b"console=01\r\n",
    b"console integration: console v01\r\n",
    b"console integration: output video\r\n",
    b"console integration: input hid\r\n",
    b"console integration: ioc level 4A\r\n",
    b"console integration: rejected calls OK\r\n",
    b"console integration: OK\r\n",
)

# IOC peer traffic: the lane's one bounded bring-up, then the IOC-FAT
# service's FS2 exchange through its resolved IOC endpoint.
LINK_SYNC_REQUEST = 0x0C
PING_REQUEST = 0x01
PING_RESPONSE = 0x81


class ConsoleReplayBase(ConsoleSimBase):
    """Boot one image with the emulated peers the services talk to."""

    IMAGE_SUBDIR = PRODUCTION_DIR
    IMAGE_NAME = PRODUCTION_IMAGE
    HOST_SYNC = False

    def setUp(self):
        super().setUp()
        self.syms = read_symbols(os.path.join(ROOT, self.IMAGE_SUBDIR,
                                              "zephyros.map"))

    def start(self, queue=None):
        z = self.make_cpu()
        if queue:
            z.hid.hid_queue.extend(queue)
        if self.HOST_SYNC:
            self.start_kernel(z)
        else:
            self.start_kernel(z)
        return z

    def boot_to_prompt(self, z, limit=STEP_LIMIT):
        """Step until the monitor prompt appears on the visible lane."""
        for step in range(limit):
            self.skip_delay(z)
            z.step()
            if step % 0x1FFF == 0:
                self.pump(z)
                if PROMPT in "".join(z.glyphs):
                    self.pump(z)
                    return step
        self.pump(z)
        raise AssertionError("no monitor prompt (PC=%04X, trace=%r)"
                             % (z.pc, "".join(z.glyphs)[-160:]))

    def type_line(self, z, text, expect, limit=STEP_LIMIT):
        """Type one line into the HID queue and wait for its output."""
        start = len(z.glyphs)
        z.hid.hid_queue.extend(text if isinstance(text, bytes)
                               else text.encode("ascii"))
        for step in range(limit):
            self.skip_delay(z)
            z.step()
            if step % 0x1FFF == 0:
                self.pump(z)
                if expect in "".join(z.glyphs[start:]):
                    self.pump(z)
                    return
        self.pump(z)
        raise AssertionError("%r did not follow %r (tail=%r)"
                             % (expect, text,
                                "".join(z.glyphs[start:])[-160:]))

    def wait_for_count(self, z, expect, count, limit=STEP_LIMIT):
        """Step until `expect` has appeared `count` times."""
        for step in range(limit):
            self.skip_delay(z)
            z.step()
            if step % 0x1FFF == 0:
                self.pump(z)
                if "".join(z.glyphs).count(expect) >= count:
                    return
        self.pump(z)
        raise AssertionError("%r did not appear %d times (trace=%r)"
                             % (expect, count,
                                "".join(z.glyphs)[-160:]))

    def read_sio(self, z):
        return bytes(z.sio_tx)

    def read_trace(self, z):
        self.pump(z)
        return "".join(z.glyphs)


class TestProductionImage(ConsoleReplayBase):
    """`build/zephyros.bin`: the store's five services and the monitor."""

    def test_boot_lanes_and_service_store(self):
        z = self.start()
        self.boot_to_prompt(z)
        sio = self.read_sio(z)
        trace = self.read_trace(z)

        # The early console prints its banner and nothing else: every
        # later line is produced through the console service, so it
        # reaches the V9958 lane instead of the serial link.
        for line in (b"ZephyrOS 0.0\r\n", b"platform: Zephyr-80\r\n",
                     b"kernel entry: OK\r\n", b"serial: OK\r\n"):
            self.assertIn(line, sio)
        self.assertEqual(sio.count(b"\n"), 4, sio)
        self.assertNotIn(b": unavailable 0x", sio)
        self.assertNotIn(b"FAIL", sio)
        self.assertNotIn(b"console integration", sio)

        # The boot line the nucleus prints through the console service.
        self.assertIn("console: service OK", trace)
        self.assertNotIn("console: service OK", sio.decode("ascii",
                                                            "replace"))
        self.assertIn(BANNER, trace)
        self.assertIn(PROMPT, trace)

        # The production image links no acceptance symbol at all.
        for name in ("_zos_run_console_integration_selftest",
                     "_ace_integration_result", "_zos_target_test_late"):
            self.assertNotIn(name, self.syms)

        self.assert_service_store()

    def test_monitor_session_reads_through_the_console(self):
        z = self.start()
        self.boot_to_prompt(z)
        self.type_line(z, b"r\r", SYSINFO)
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
        # The IOC service's one bounded LINK_SYNC bring-up, then the
        # IOC-FAT service's FS2 commands (the driver calls its
        # dependency directly; there is no class shim since 42b).
        self.assertEqual(requests[0], LINK_SYNC_REQUEST)
        self.assertIn(0x30, requests)  # CMD_FS2_CAPS
        self.assertEqual(peer.channel_resets, 1)
        self.assertEqual(peer.hunt_writes, 1)
        self.assertEqual(peer.rts_asserts, peer.rts_releases)
        self.assertTrue(peer.requests, "no command reached the peer")

    def test_store_payload_identity(self):
        store = os.path.join(ROOT, PRODUCTION_DIR, "recovery.romfs")
        entries = self.load_store(store)
        manifest = os.path.join(ROOT, "recovery", "manifest.json")
        self.assertEqual(sorted(entries), sorted(self.manifest_names(
            manifest)))
        monitor = entries["monitor.zex"]["payload"]
        self.assertEqual(len(monitor), MONITOR_SIZE)
        self.assertEqual(hashlib.sha256(monitor).hexdigest(),
                         MONITOR_SHA256)

    def assert_service_store(self):
        manifest = self.manifest_names(os.path.join(ROOT, "build",
                                                    "modules",
                                                    "services.man"))
        self.assertEqual(manifest, [name for name, _class in SERVICE_SET])
        for name, klass in SERVICE_SET:
            path = os.path.join(ROOT, "build", "modules", name)
            with open(path, "rb") as handle:
                info = zmodpack.parse_container(handle.read(), name)
            # The flat plan: one endpoint per image, and every
            # production image carries its publication (work package 42c).
            self.assertEqual([(ep["class"], ep["instance"], ep["version"])
                              for ep in info["endpoints"]],
                             [(klass, 0, 1)], name)
            self.assertEqual(len(info["publications"]), 1, name)
            self.assertEqual(info["publications"][0]["number"],
                             info["endpoints"][0]["number"], name)
            # The console image's required dependencies (work package
            # 42e: no optional dependency and no runtime fallback).  The
            # other images' dependencies are pinned by their own tests.
            if name in CONSOLE_DEPS:
                self.assertEqual([(dep["class"], dep["instance"])
                                  for dep in info["dependencies"]],
                                 list(CONSOLE_DEPS[name]), name)
                for dep in info["dependencies"]:
                    self.assertGreater(dep["number"], 0, name)

    def test_console_manifests_select_the_build(self):
        """The two CONSOLE manifests differ only in what the build
        selects (work package 42e), and both require it."""
        expected = {
            "console-v9958-hid": [(4, 0), (2, 0)],   # HID, VIDEO
            "console-serial": [(1, 0)],              # SERIAL
        }
        for name, deps in expected.items():
            path = os.path.join(ROOT, "services", "manifests",
                                "%s.json" % name)
            with open(path) as handle:
                document = json.load(handle)
            self.assertEqual(document["name"], name)
            self.assertEqual([(dep["class"], dep["instance"])
                              for dep in document["depends"]], deps, name)
            for dep in document["depends"]:
                self.assertNotIn("optional", dep, name)

    @staticmethod
    def manifest_names(path):
        """Entry names of a packed store or of a service boot manifest."""
        if path.endswith("services.man"):
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
    """`build-console-integration-selftest`: the production service set
    plus the target diagnostic and the same monitor launcher."""

    IMAGE_SUBDIR = INTEGRATION_DIR
    IMAGE_NAME = INTEGRATION_IMAGE

    def service_plan(self):
        """The nucleus ledger the packer's set report carries for this
        image (tools/zmodpack.py services --report services.plan.json)."""
        path = os.path.join(ROOT, INTEGRATION_DIR, "modules",
                            "services.plan.json")
        with open(path, "r") as handle:
            return json.load(handle)["nucleus"]

    def test_diagnostic_transcript_and_visible_lanes(self):
        z = self.start(queue=INTEGRATION_TYPED)
        self.boot_to_prompt(z)
        sio = self.read_sio(z)
        trace = self.read_trace(z)

        # Loader evidence: one row per image, in manifest order, plus the
        # ledger the packer's report pins.
        for name in ("sio.zmod", "ioc.zmod", "hid.zmod", "v9958.zmod",
                     "console-v9958-hid.zmod"):
            self.assertIn(("driver %s: hot " % name).encode(), sio)
        self.assertIn(b"service loader: 5 images in bank 6\r\n", sio)
        # The loader's two ledgers must equal what the packer's plan
        # (services.plan.json) reports for the same set.
        plan = self.service_plan()
        self.assertIn(b"driver hot: %d bytes of 32768\r\n"
                      % plan["service_hot_bytes"], sio)
        self.assertIn(b"driver boot/bulk: %d bytes of 24576\r\n"
                      % plan["service_boot_bytes"], sio)
        for line in INTEGRATION_LINES:
            self.assertIn(line, sio)
        self.assertNotIn(b"console integration: FAIL", sio)
        self.assertNotIn(b"iocfat", sio)

        # The early SIO lane never carries the console-service output: the
        # boot line, the diagnostic's marker and its typed echo are all on
        # the visible lane.
        self.assertNotIn(b"console: service OK", sio)
        self.assertIn("console: service OK", trace)
        self.assertIn(INTEGRATION_MARKER, trace)
        self.assertIn("K41", trace)          # the typed echo
        self.assertIn("integration done", trace)

        # The image boot program is the ESC c check (work package 42e):
        # it reset the display and printed its line before handing the
        # console to the monitor, so the V9958 bring-up sequence ran
        # twice - the driver's boot segment and the ESC c reset.
        self.assertIn("escc: display reset", trace)
        self.assertEqual(len(bringup_starts(z.vdp)), 2)
        self.assertIn(BANNER, trace)
        self.assertIn(PROMPT, trace)

        # The IOC traffic the diagnostic drove: the service's link
        # bring-up, then the HID fetch its console read drove and its
        # PING.
        requests = [entry for entry in z.ioc_peer.requests]
        kinds = [entry[0] for entry in requests]
        self.assertEqual(kinds[0], LINK_SYNC_REQUEST)
        self.assertIn(0x0E, kinds)           # CMD_HID_INPUT
        self.assertIn(PING_REQUEST, kinds)
        ping = [entry for entry in requests if entry[0] == PING_REQUEST][0]
        self.assertEqual(ping[1:], (3, 0, b"@ABC"))
        self.assertEqual(z.ioc_peer.hid_delivered, len(INTEGRATION_TYPED))
        self.assertEqual(z.ioc_peer.channel_resets, 1)

    def test_monitor_runs_after_the_diagnostic(self):
        z = self.start(queue=INTEGRATION_TYPED)
        self.boot_to_prompt(z)
        self.type_line(z, b"q\r", MONITOR_EXIT)
        self.wait_for_count(z, BANNER, 2)
        trace = self.read_trace(z)
        self.assertGreaterEqual(trace.count(BANNER), 2)


if __name__ == "__main__":
    unittest.main()
