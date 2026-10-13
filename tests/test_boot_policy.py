#!/usr/bin/env python3
"""Boot policy: the boot program and what the launcher does when it ends.

The launcher runs one program at boot and then follows the system file's
`after_exit` setting: `relaunch` starts the same program again after a
normal exit, `monitor` hands the machine to `/system/monitor.exe`.  A run
that cannot start at all always reports the failed line and always falls
back to the monitor, whatever the setting; the monitor's own exit returns
to the boot program, and a monitor that cannot start is the controlled
halt.

The host side pins the inputs the launcher is built from: every system
file states a valid setting (the production images boot the shell, every
acceptance image ends at the monitor), the generator rejects a missing,
unknown or store-less setting, and the generated `system.mk` and
`boot_program.h` carry it.  The policy table itself is asserted against
the linked bytes in the replays: production rel launching the shell, both
fallback stores reaching the monitor with the failed line, and an
acceptance image ending at the monitor after one run.

Run with: `python3 -m unittest tests.test_boot_policy`.
"""

import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import romfspack  # noqa: E402
import systemgen  # noqa: E402
from replay import STEP_LIMIT, ConsoleReplay, Replay  # noqa: E402
from z80sim import IocCommandPeer  # noqa: E402

SYSTEMS_DIR = os.path.join(ROOT, "systems")
PRODUCTION_DIR = "build"
PRODUCTION_IMAGE = "konaos.bin"
MISSING_DIR = "build-boot-missing-selftest"
MISSING_IMAGE = "konaos-boot-missing-selftest.bin"
CORRUPT_DIR = "build-boot-corrupt-selftest"
CORRUPT_IMAGE = "konaos-boot-corrupt-selftest.bin"
FS_DIR = "build-fs-selftest"
FS_IMAGE = "konaos-fs-selftest.bin"

# The two production systems boot the shell and relaunch it; every other
# image runs its program once and ends at the monitor.
SHELL_BOOT_SYSTEMS = ("production", "console-serial")
SHELL_BOOT = "/system/shell.exe"

# The shell's displayed prompt on a card (its CWD is /fs) and the state a
# machine without one starts in.
SHELL_PROMPT = "/> "
NO_CARD_PROMPT = "/system> "
NO_FILESYSTEM = "shell: no filesystem, starting in /system"

MONITOR_BANNER = b"KonaOS native monitor"
MONITOR_PROMPT = b"Z80> "

# The failed code each fallback store produces: the missing shell is a
# missing entry (KOS_ENOENT), the malformed container is
# EXE_EXEC_ERR_MAGIC, and both travel negated.
MISSING_FAILED = b"boot: /system/shell.exe failed 34\r\n"
CORRUPT_FAILED = b"boot: /system/exe-bad-magic.exe failed 49\r\n"


def system_files():
    """Every systems/<name>.json, keyed by name."""
    documents = {}
    for name in sorted(os.listdir(SYSTEMS_DIR)):
        if not name.endswith(".json"):
            continue
        with open(os.path.join(SYSTEMS_DIR, name),
                  encoding="utf-8") as handle:
            documents[name[:-5]] = json.load(handle)
    return documents


class TestSystemFiles(unittest.TestCase):
    """The setting is a system-file field with one of two values."""

    def test_every_system_states_one_of_the_two_policies(self):
        for name, document in system_files().items():
            self.assertIn(document["after_exit"], systemgen.AFTER_EXIT_CHOICES,
                          name)

    def test_production_boots_the_shell_and_relaunches_it(self):
        for name, document in system_files().items():
            if name not in SHELL_BOOT_SYSTEMS:
                continue
            self.assertEqual(document["boot"], SHELL_BOOT, name)
            self.assertEqual(document["after_exit"], "relaunch", name)
            self.assertIn("shell.exe", document["store"], name)

    def test_every_acceptance_image_ends_at_the_monitor(self):
        systems = system_files()
        for name, document in systems.items():
            if name in SHELL_BOOT_SYSTEMS:
                continue
            self.assertEqual(document["after_exit"], "monitor", name)
            self.assertIn(systemgen.MONITOR_NAME, document["store"], name)


class TestGeneratorPolicy(unittest.TestCase):
    """systemgen.py validates the setting while it resolves the system."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name
        os.mkdir(os.path.join(self.root, "systems"))

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, **overrides):
        document = {"schema": "kos-system-1", "name": "probe",
                    "drivers": ["probe"], "boot": "/system/monitor.exe",
                    "after_exit": "monitor", "store": ["monitor.exe"],
                    "cflags": []}
        document.update(overrides)
        if document.get("after_exit") is None:
            del document["after_exit"]
        path = os.path.join(self.root, "systems", "probe.json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(document, handle)
        return path

    def validate(self, path):
        """Run the generator's policy gate over one crafted system.

        The driver set is not resolved here: the id list only has to be
        well formed for the gate that reads the policy and the store."""

        system, name = systemgen.load_system(path)
        store = systemgen.build_store(name, [], system["store"], set())
        systemgen.validate_boot(system["boot"], store["files"], name,
                                missing=set())
        systemgen.validate_after_exit(system, store["files"], name)

    def test_rejects_a_system_without_the_setting(self):
        path = self.write(after_exit=None)
        with self.assertRaisesRegex(systemgen.SystemError, "after_exit"):
            self.validate(path)

    def test_rejects_an_unknown_setting(self):
        path = self.write(after_exit="loop")
        with self.assertRaisesRegex(systemgen.SystemError,
                                    "after_exit must be one of"):
            self.validate(path)

    def test_rejects_a_monitor_image_without_the_monitor(self):
        path = self.write(store=[])
        with self.assertRaisesRegex(systemgen.SystemError,
                                    "monitor.exe is not in the system's "
                                    "store"):
            self.validate(path)

    def test_accepts_a_monitor_image_with_it(self):
        self.validate(self.write())

    def test_relaunch_needs_no_monitor_entry(self):
        self.validate(self.write(boot=SHELL_BOOT, after_exit="relaunch",
                                 store=["shell.exe"]))


class TestGeneratedBootSetting(unittest.TestCase):
    """The setting reaches the sources the launcher is built from."""

    def read(self, build_dir, name):
        path = os.path.join(ROOT, build_dir, name)
        if not os.path.exists(path):
            raise unittest.SkipTest("%s has not been generated" % path)
        with open(path, encoding="utf-8") as handle:
            return handle.read()

    def test_production_builds_the_shell_and_a_relaunch(self):
        mk = self.read(PRODUCTION_DIR, "system.mk")
        self.assertIn("KOS_BOOT_PROGRAM := /system/shell.exe\n", mk)
        self.assertIn("KOS_BOOT_AFTER_EXIT := relaunch\n", mk)
        header = self.read(PRODUCTION_DIR, "boot_program.h")
        self.assertIn('#define KOS_BOOT_PROGRAM "/system/shell.exe"\n',
                      header)
        self.assertIn("#define KOS_BOOT_AFTER_MONITOR 0\n", header)

    def test_a_monitor_image_selects_the_monitor_branch(self):
        for build_dir in (MISSING_DIR, CORRUPT_DIR, FS_DIR):
            mk = self.read(build_dir, "system.mk")
            self.assertIn("KOS_BOOT_AFTER_EXIT := monitor\n", mk, build_dir)
            header = self.read(build_dir, "boot_program.h")
            self.assertIn("#define KOS_BOOT_AFTER_MONITOR 1\n", header,
                          build_dir)

    def test_the_fallback_stores_are_what_the_launcher_is_tested_on(self):
        names = self.store_names(MISSING_DIR)
        self.assertNotIn("shell.exe", names)
        self.assertIn(systemgen.MONITOR_NAME, names)
        names = self.store_names(CORRUPT_DIR)
        self.assertIn("exe-bad-magic.exe", names)
        self.assertIn(systemgen.MONITOR_NAME, names)

    def store_names(self, build_dir):
        path = os.path.join(ROOT, build_dir, "recovery.romfs")
        if not os.path.exists(path):
            raise unittest.SkipTest("%s has not been built" % path)
        with open(path, "rb") as handle:
            _header, entries = romfspack.parse_image(handle.read())
        return [entry["name"] for entry in entries]


class ProductionBootBase(ConsoleReplay, unittest.TestCase):
    """The production image and the emulated peers it boots."""

    IMAGE_SUBDIR = PRODUCTION_DIR
    IMAGE_NAME = PRODUCTION_IMAGE

    def setUp(self):
        self.load_image()

    def start(self, card=True):
        z = self.make_cpu()
        z.hid = z.ioc_peer = IocCommandPeer(fs2_media=card)
        self.start_kernel(z)
        self.run_to(z, SHELL_PROMPT if card else NO_CARD_PROMPT,
                    STEP_LIMIT)
        return z

    def type_line(self, z, line, expect, limit=STEP_LIMIT):
        since = len(z.glyphs)
        z.hid.hid_queue.extend(line.encode("ascii") + b"\r")
        self.run_to(z, expect, limit, since=since)
        self.pump(z)
        return "".join(z.glyphs)[since:]


class TestProductionBootPolicy(ProductionBootBase):
    """`build/konaos.bin`: the shell is the boot program."""

    def test_boot_lands_on_the_shell_prompt_without_a_boot_line(self):
        z = self.start()
        trace = self.trace(z)
        self.assertIn("console: driver OK", trace)
        self.assertTrue(trace.rstrip().endswith(SHELL_PROMPT.rstrip()),
                        trace[-80:])
        # A clean boot is not a launcher event: the exit and failed lines
        # are written when the run ends, never before it.
        self.assertNotIn("boot: ", trace)

    def test_exit_reports_the_status_and_the_shell_starts_again(self):
        z = self.start()
        fresh = self.type_line(z, "exit 42", SHELL_PROMPT)
        self.assertIn("exit 42\n", fresh)
        self.assertIn("boot: /system/shell.exe exit 42\n", fresh)
        # The relaunch is a boot like any other: the prompt follows and no
        # second line is written until that run ends too.
        self.assertEqual(fresh.count("boot: "), 1, fresh)
        self.assertTrue(fresh.endswith(SHELL_PROMPT), fresh[-40:])

    def test_a_run_without_a_card_stays_in_the_store(self):
        z = self.start(card=False)
        trace = self.trace(z)
        self.assertIn(NO_FILESYSTEM, trace)
        # The store is the one filesystem a card-less machine has: listing
        # and reading it work, and the display root is not a device.
        listing = self.type_line(z, "ls /system", NO_CARD_PROMPT)
        self.assertIn("shell.exe  ", listing)
        self.assertIn("monitor.exe  ", listing)
        manifest = self.type_line(z, "cat /system/drivers.man",
                                  NO_CARD_PROMPT)
        self.assertIn("sio.mod", manifest)
        self.assertIn("iocfat.mod", manifest)
        self.assertIn("ls: /: no device",
                      self.type_line(z, "ls /", NO_CARD_PROMPT))
        self.assertIn("mkdir: /X: no device",
                      self.type_line(z, "mkdir /X", NO_CARD_PROMPT))

    def test_the_monitor_round_trip_returns_to_the_shell(self):
        z = self.start()
        self.type_line(z, "monitor", "Z80> ")
        fresh = self.type_line(z, "q", SHELL_PROMPT)
        # The monitor was the shell's child, so its exit is the shell's
        # business, not the launcher's.
        self.assertIn("q\n", fresh)
        self.assertNotIn("boot: ", fresh)


class FallbackBootBase(Replay, unittest.TestCase):
    """A fallback store on the serial console: the failed line names the
    program the launcher could not run, and the monitor starts anyway."""

    FAILED = b""

    def setUp(self):
        if type(self) is FallbackBootBase:
            self.skipTest("abstract fallback harness")
        self.load_image()

    def start(self):
        z = self.make_cpu()
        z.hid = z.ioc_peer = IocCommandPeer(fs2_media=True)
        self.start_kernel(z)
        self.run_until(z, [MONITOR_PROMPT], limit=STEP_LIMIT, panic=None)
        return z

    def type_line(self, z, line, expect, limit=STEP_LIMIT):
        start = len(z.sio_tx)
        z.sio_rx.extend(line.encode("ascii") + b"\r")
        self.run_until(z, [expect], limit=limit, since=start, panic=None)
        return bytes(z.sio_tx)[start:]

    def test_the_failed_line_precedes_the_monitor(self):
        z = self.start()
        sio = bytes(z.sio_tx)
        self.assertIn(self.FAILED, sio)
        self.assertLess(sio.index(self.FAILED), sio.index(MONITOR_BANNER))
        self.assertIn(MONITOR_PROMPT, sio)

    def test_the_monitor_exit_returns_to_the_boot_program(self):
        z = self.start()
        self.type_line(z, "q", MONITOR_PROMPT)
        sio = bytes(z.sio_tx)
        # The failed program is the boot program: the launcher tries it
        # again, reports it again and falls back again.
        self.assertEqual(sio.count(self.FAILED), 2, sio[-200:])


class TestMissingBootProgram(FallbackBootBase):
    """`boot-missing`: the boot setting names a file the store omits."""

    IMAGE_SUBDIR = MISSING_DIR
    IMAGE_NAME = MISSING_IMAGE
    FAILED = MISSING_FAILED


class TestCorruptBootProgram(FallbackBootBase):
    """`boot-corrupt`: the boot setting names a malformed container."""

    IMAGE_SUBDIR = CORRUPT_DIR
    IMAGE_NAME = CORRUPT_IMAGE
    FAILED = CORRUPT_FAILED


class TestAcceptanceImageEndsAtTheMonitor(Replay, unittest.TestCase):
    """`build-fs-selftest`: a `monitor` image runs once, then the monitor
    has the machine (the acceptance-image policy).

    The verdict program hands the console to a monitor of its own before
    it returns, so the policy is visible in what follows that session:
    the program's exit line, then a second monitor - the launcher's, not
    the program's - at the machine's own pid."""

    IMAGE_SUBDIR = FS_DIR
    IMAGE_NAME = FS_IMAGE
    EXIT = b"boot: /system/fs_selftest.exe exit 0\r\n"
    VERDICT = b"fs-selftest: PASS\r\n"
    OWN_MONITOR = b"KonaOS native monitor\r\nbank=01 pid=1 "
    LAUNCHER_MONITOR = b"KonaOS native monitor\r\nbank=00 pid=0 "

    def setUp(self):
        self.load_image()

    def run_to_prompt(self, z, since=0, limit=STEP_LIMIT):
        self.run_until(z, [MONITOR_PROMPT], limit=limit, since=since,
                       panic=None)
        return bytes(z.sio_tx)

    def test_one_run_then_the_monitor(self):
        z = self.make_cpu()
        z.hid = z.ioc_peer = IocCommandPeer(fs2_media=True)
        self.start_kernel(z)
        sio = self.run_to_prompt(z)
        self.assertIn(self.VERDICT, sio)
        self.assertIn(self.OWN_MONITOR, sio)
        self.assertNotIn(self.EXIT, sio)

        # `q` ends the program's own monitor session, the program returns
        # its verdict, and the launcher - not the program - runs the
        # monitor that follows.
        start = len(z.sio_tx)
        z.sio_rx.extend(b"q\r")
        sio = self.run_to_prompt(z, since=start)
        self.assertIn(self.EXIT, sio)
        self.assertIn(self.LAUNCHER_MONITOR, sio)


if __name__ == "__main__":
    unittest.main()
