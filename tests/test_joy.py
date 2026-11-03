#!/usr/bin/env python3
"""The game controllers: the JOY driver and the SDK's client.

Three families:

* host: the JOY type, class and operations, the board's latch ports, the
  driver's manifest and the systems that carry it, and the SDK's two
  decoders compiled for the host and judged over every latch byte
  against the IO controller's tables (Zephyr-80 `Code/MCU/IOController`:
  `src/controller_latch.c` for the bits, `src/ioc_hid.c`
  `coleco_keypad_nibble` for the keypad codes; checked against those
  files when the Zephyr-80 tree is beside this one);
* artifacts: the packed container and the production set against the
  nucleus tables;
* replay: the production image with the latches scripted
  (`z80sim.ControllerLatch`), `joy stage` typed at the shell.
  `/system/joy.exe` binds both names, reads INFO, is refused pad 2 and
  an exclusive bind, then at each step (the replay sets both latches at
  `stage_mark`) prints both pads through `joy_read` and `joy_read_call`
  with both readings.  The replay holds the bytes, the refusals, the
  ports read, and reports the T-states of both reads.

The artifacts and the replay skip where the production image has not
been built.
"""

import json
import os
import re
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import modpack  # noqa: E402
from maptools import evaluate, parse_asm_inc, parse_c_header  # noqa: E402
from test_hwtest_sim import HwtestReplay, SHELL_PROMPT_NO_CARD  # noqa: E402

PROTOCOL_H = os.path.join(ROOT, "abi", "kos", "protocol.h")
OBJECT_H = os.path.join(ROOT, "abi", "kos", "object.h")
HARDWARE_H = os.path.join(ROOT, "platform", "zephyr80", "public",
                          "hardware.h")
HARDWARE_INC = os.path.join(ROOT, "platform", "zephyr80", "public",
                            "hardware.inc")
JOY_H = os.path.join(ROOT, "user", "sdk", "include", "kos", "joy.h")
JOY_JSON = os.path.join(ROOT, "drivers", "joy", "joy.json")
JOY_IO = os.path.join(ROOT, "drivers", "joy", "joy_io.asm")
DECODE_C = os.path.join(ROOT, "user", "sdk", "src", "joy_decode.c")
HOST_C = os.path.join(ROOT, "tests", "joy", "joy_host.c")
HOST_TOOL = os.path.join(ROOT, "build", "host", "joy_decode")
SYSTEMS = os.path.join(ROOT, "systems")
VFS_H = os.path.join(ROOT, "kernel", "vfs", "vfs.h")
IPC_C = os.path.join(ROOT, "kernel", "ipc", "ipc.c")
MODULES = os.path.join(ROOT, "build", "modules")
JOY_MOD = os.path.join(MODULES, "joy.mod")
JOY_REPORT = os.path.join(MODULES, "joy.report.json")
PLAN = os.path.join(MODULES, "drivers.plan.json")
JOY_EXE = os.path.join(ROOT, "build", "user", "joy.exe")
JOY_MAP = os.path.join(ROOT, "build", "user", "joy", "joy.map")

FIRMWARE = os.path.normpath(os.path.join(ROOT, "..", "Zephyr-80", "Code",
                                         "MCU", "IOController"))
FIRMWARE_LATCH = os.path.join(FIRMWARE, "src", "controller_latch.c")
FIRMWARE_HID = os.path.join(FIRMWARE, "src", "ioc_hid.c")

# The controller's byte (controller_latch.c CTRL_*, active low) and the
# keypad codes, 0-9 then * and # (ioc_hid.c coleco_keypad_nibble).
CTRL = {"UP": 0x01, "RIGHT": 0x02, "DOWN": 0x04, "LEFT": 0x08, "FIRE": 0x40}
IDLE = 0x7F
KEYPAD = [0x0A, 0x0D, 0x07, 0x0C, 0x02, 0x03, 0x0E, 0x05, 0x01, 0x0B,
          0x09, 0x06]
KEYS = "0123456789*#"
# The F310's substitutes (controller_latch_f310_report): X, Y, Back and
# Start are keys 1, 2, * and #.
PAD_KEYS = {"X": 0x0D, "Y": 0x07, "BACK": 0x09, "START": 0x06}

# The replay's script: both latches at each step, and the line each step
# prints (joy_read, joy_read_call, the joystick and the keypad readings).
STEPS = [
    ((0x7F, 0x7F), "7F 7F call 7F 7F dirs ----- ----- key - -"),
    ((0x3E, 0x7F), "3E 7F call 3E 7F dirs U---F ----- key 6 -"),
    ((0x7F, 0x7D), "7F 7D call 7F 7D dirs ----- -R--- key - 1"),
    ((0x76, 0x39), "76 39 call 76 39 dirs U--L- -RD-F key # *"),
    ((0x7A, 0x75), "7A 75 call 7A 75 dirs U-D-- -R-L- key 0 7"),
    ((0x7F, 0x7F), "7F 7F call 7F 7F dirs ----- ----- key - -"),
]
STAGE_LINES = [
    "joy: bind 0 0 type 12",
    "joy: info 0 count 2 ports FC FF",
    "joy: read pad 2 -2",
    "joy: bind 1 0 type 12",
    "joy: exclusive -7",
    "joy: open 2 -5",
    "joy: stray 7F",
]
BUDGET = 40000000


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def value(path, name):
    env = parse_c_header(path)
    return evaluate(env[name], env)


def map_symbol(path, name):
    for line in read(path).splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[1] == name:
            return int(parts[0], 16)
    raise AssertionError("%s has no %s" % (path, name))


def expected_dirs(raw):
    return sum(mask for mask in CTRL.values() if not raw & mask)


def expected_key(raw):
    nibble = raw & 0x0F
    if nibble in KEYPAD:
        return ord(KEYS[KEYPAD.index(nibble)])
    return -1


class TestJoyContract(unittest.TestCase):
    """The ABI, the ports, the manifest and the systems."""

    def test_the_type_class_and_operations(self):
        self.assertEqual(value(OBJECT_H, "KOS_TYPE_JOY"), 12)
        self.assertEqual(value(PROTOCOL_H, "KOS_CLASS_JOY"), 12)
        self.assertEqual(value(PROTOCOL_H, "KJOY_OP_READ"), 0)
        self.assertEqual(value(PROTOCOL_H, "KJOY_OP_INFO"), 1)
        self.assertEqual(value(PROTOCOL_H, "KJOY_PAD_MAX"), 2)
        self.assertEqual(value(PROTOCOL_H, "KJOY_INFO_COUNT"), 0)
        self.assertEqual(value(PROTOCOL_H, "KJOY_INFO_PORTS"), 1)
        self.assertEqual(modpack.TYPES["JOY"], 12)
        # The nucleus installs rows up to the last type.
        self.assertIn("type > KOS_TYPE_JOY",
                      read(os.path.join(ROOT, "kernel", "vfs", "dev.c")))

    def test_the_latch_ports(self):
        inc = parse_asm_inc(HARDWARE_INC)
        for name, port in (("KOS_JOY0_PORT", 0xFC), ("KOS_JOY1_PORT", 0xFF)):
            self.assertEqual(value(HARDWARE_H, name), port, name)
            self.assertEqual(evaluate(inc[name], inc), port, name)

    def test_the_manifest(self):
        manifest = json.loads(read(JOY_JSON))
        self.assertEqual(manifest["name"], "joy")
        self.assertNotIn("init", manifest)
        self.assertEqual([segment["role"] for segment in manifest["segments"]],
                         ["hot"])
        self.assertEqual(manifest["endpoints"],
                         [{"class": 12, "instance": 0, "version": 1,
                           "dispatch": "joy_driver_dispatch", "flags": 0}])
        self.assertEqual(
            [(item["path"], item["type"], item["endpoint"], item["object"])
             for item in manifest["publishes"]],
            [("/dev/input/joy/0", "JOY", {"class": 12, "instance": 0}, 0),
             ("/dev/input/joy/1", "JOY", {"class": 12, "instance": 0}, 1)])
        self.assertEqual(manifest["depends"], [])

    def test_the_driver_reads_its_two_ports_only(self):
        code = "\n".join(line.split(";")[0]
                         for line in read(JOY_IO).splitlines())
        self.assertEqual(re.findall(r"\bin\s+a,\((\w+)\)", code), ["c"])
        self.assertEqual(re.findall(r"\bout\b", code), [])
        self.assertEqual(re.findall(r"ld\s+c,#(\w+)", code),
                         ["KOS_JOY0_PORT", "KOS_JOY1_PORT"])

    def test_every_system_with_the_latch_consumers_carries_it(self):
        for name in sorted(os.listdir(SYSTEMS)):
            system = json.loads(read(os.path.join(SYSTEMS, name)))
            drivers = system["drivers"]
            # The serial production image and its missing-console
            # variant carry the production set too.
            wanted = ("sound" in drivers or "console-v9958-hid" in drivers or
                      name in ("console-serial.json",
                               "console-serial-missing.json"))
            self.assertEqual("joy" in drivers, wanted, name)
            if "iocfat" in drivers and "joy" in drivers:
                self.assertLess(drivers.index("joy"), drivers.index("iocfat"))
        production = json.loads(read(os.path.join(SYSTEMS,
                                                  "production.json")))
        self.assertIn("joy.exe", production["store"])

    def test_the_sdk_header(self):
        for name, number in (("JOY_UP", 0x01), ("JOY_RIGHT", 0x02),
                             ("JOY_DOWN", 0x04), ("JOY_LEFT", 0x08),
                             ("JOY_FIRE", 0x40), ("JOY_IDLE", 0x7F),
                             ("JOY_NIBBLE", 0x0F), ("JOY_KEY_FIRE", 0x40),
                             ("JOY_KEY_NONE", 0x0F)):
            self.assertEqual(value(JOY_H, name), number, name)
        for name, mask in CTRL.items():
            self.assertEqual(value(JOY_H, "JOY_" + name), mask, name)
        for key, code in zip(KEYS, KEYPAD):
            name = {"*": "STAR", "#": "POUND"}.get(key, key)
            self.assertEqual(value(JOY_H, "JOY_KEY_" + name), code, key)
        self.assertEqual(value(JOY_H, "JOY_IDLE"), IDLE)


class TestJoyDecoders(unittest.TestCase):
    """joy_dirs and joy_key, compiled for the host, over every byte."""

    @classmethod
    def setUpClass(cls):
        os.makedirs(os.path.dirname(HOST_TOOL), exist_ok=True)
        subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                        "-I" + os.path.join(ROOT, "abi"),
                        "-I" + os.path.join(ROOT, "user", "sdk", "include"),
                        HOST_C, DECODE_C, "-o", HOST_TOOL],
                       check=True, cwd=ROOT)
        output = subprocess.run([HOST_TOOL], check=True, cwd=ROOT,
                                stdout=subprocess.PIPE, text=True).stdout
        cls.rows = {}
        for line in output.splitlines():
            raw, dirs, key = line.split()
            cls.rows[int(raw, 16)] = (int(dirs, 16), int(key))

    def test_every_byte(self):
        self.assertEqual(sorted(self.rows), list(range(256)))
        for raw, (dirs, key) in self.rows.items():
            self.assertEqual(dirs, expected_dirs(raw), "%02X" % raw)
            self.assertEqual(key, expected_key(raw), "%02X" % raw)

    def test_the_sixteen_nibbles_against_the_keypad_table(self):
        keys = {}
        for nibble in range(16):
            keys[nibble] = self.rows[0x70 | nibble][1]
        self.assertEqual({nibble: chr(key) for nibble, key in keys.items()
                          if key >= 0},
                         {code: key for code, key in zip(KEYPAD, KEYS)})
        self.assertEqual(sorted(nibble for nibble, key in keys.items()
                                if key < 0), [0x00, 0x04, 0x08, 0x0F])
        # The pad's substitutes read as their keys; idle reads none.
        for button, key in (("X", "1"), ("Y", "2"), ("BACK", "*"),
                            ("START", "#")):
            self.assertEqual(chr(self.rows[0x70 | PAD_KEYS[button]][1]), key)
        self.assertEqual(self.rows[IDLE], (0, -1))

    def test_which_keys_share_a_direction(self):
        # A pad reports one direction or two adjacent ones; a code that
        # reads as anything else is no direction a pad can produce.
        possible = {0x0F}
        for one in (0x01, 0x02, 0x04, 0x08):
            possible.add(0x0F & ~one)
        for pair in (0x03, 0x06, 0x0C, 0x09):
            possible.add(0x0F & ~pair)
        shared = sorted(key for key, code in zip(KEYS, KEYPAD)
                        if code in possible)
        self.assertEqual(shared, sorted("123569*#"))

    def test_fire_in_both_readings(self):
        self.assertEqual(self.rows[0x3F][0], CTRL["FIRE"])
        self.assertEqual(self.rows[0x3F][1], -1)
        # A held keypad code with the second fire: the key still reads.
        self.assertEqual(chr(self.rows[0x39][1]), "*")

    @unittest.skipUnless(os.path.exists(FIRMWARE_HID),
                         "the Zephyr-80 tree is not beside this one")
    def test_the_tables_are_the_firmwares(self):
        hid = read(FIRMWARE_HID)
        match = re.search(r"coleco_keypad_nibble\[12\]\s*=\s*\{([^}]*)\}",
                          hid)
        self.assertEqual([int(item.strip().rstrip("u"), 16)
                          for item in match.group(1).split(",")
                          if item.strip()], KEYPAD)
        latch = read(FIRMWARE_LATCH)
        for name, mask in CTRL.items():
            self.assertRegex(latch, r"#define CTRL_%s\s+0x%02xu"
                             % (name, mask))
        for name, code in (("1", 0x0D), ("2", 0x07), ("STAR", 0x09),
                           ("POUND", 0x06)):
            self.assertRegex(latch, r"#define CTRL_KEYPAD_%s\s+0x%02xu"
                             % (name, code))
        header = read(os.path.join(FIRMWARE, "include",
                                   "controller_latch.h"))
        self.assertRegex(header, r"#define CONTROLLER_LATCH_IDLE\s+0x7fu")


class TestJoyArtifacts(unittest.TestCase):
    """The packed container and the production plan."""

    def setUp(self):
        if not os.path.exists(PLAN) or not os.path.exists(JOY_MOD):
            self.skipTest("the driver images have not been packed; run make")

    def test_the_container(self):
        with open(JOY_MOD, "rb") as handle:
            info = modpack.parse_container(handle.read(), "joy.mod")
        self.assertEqual([(item["name"], item["type"], item["object"])
                          for item in info["publications"]],
                         [("/input/joy/0", 12, 0), ("/input/joy/1", 12, 1)])
        self.assertEqual([(endpoint["class"], endpoint["instance"])
                          for endpoint in info["endpoints"]], [(12, 0)])
        report = json.loads(read(JOY_REPORT))
        segments = {segment["role"]: segment
                    for segment in report["segments"]}
        self.assertEqual(sorted(segments), ["hot"])
        hot = segments["hot"]["image_size"] + segments["hot"]["bss_size"]
        print("joy: driver hot %d bytes" % hot)

    def test_the_production_set_fits_the_nucleus_tables(self):
        plan = json.loads(read(PLAN))
        endpoints = sum(len(item["endpoints"]) for item in plan["drivers"])
        rows = set()
        for item in plan["drivers"]:
            with open(os.path.join(MODULES, item["name"]), "rb") as handle:
                info = modpack.parse_container(handle.read(), item["name"])
            for publication in info["publications"]:
                name = publication["name"]
                if name == modpack.FS_ROOT:
                    continue
                rows.add(name)
                while "/" in name[1:]:
                    name = name.rsplit("/", 1)[0]
                    rows.add(name)
        # Nine drivers' ten endpoints, plus the nucleus FILE provider.
        self.assertEqual(endpoints, 10)
        endpoint_max = int(re.search(r"#define IPC_ENDPOINT_MAX (\d+)u",
                                     read(IPC_C)).group(1))
        self.assertLessEqual(endpoints + 1, endpoint_max)
        self.assertEqual(len(rows), 13)
        self.assertTrue({"/input/joy", "/input/joy/0",
                         "/input/joy/1"} <= rows)
        self.assertLessEqual(len(rows), value(VFS_H, "VFS_NS_PUB_MAX"))


class CallProbe:
    """Every call of one function: T-states from entry to return."""

    def __init__(self, entry):
        self.entry = entry
        self.costs = []
        self.open = None

    def __call__(self, z):
        if self.open is None:
            if z.pc == self.entry and (z.latch & 0x18) == 0x10:
                self.open = (z.rd16(z.sp), z.sp + 2, z.bank_of(z.pc),
                             z.tstates)
            return
        ret, sp, bank, start = self.open
        if z.pc == ret and z.sp == sp and (z.latch & 0x18) == 0x10 and \
                z.bank_of(z.pc) == bank:
            self.costs.append(z.tstates - start)
            self.open = None


class TestJoyReplay(HwtestReplay):
    """`joy stage` at the production shell, the latches scripted."""

    def setUp(self):
        self.load_image()
        if not os.path.exists(JOY_EXE):
            self.skipTest("%s missing; run make" % JOY_EXE)

    def test_the_stage_reads_the_latches(self):
        z = self.start(card=False)
        latch = z.controller_latch
        mark = map_symbol(JOY_MAP, "_stage_mark")
        step = map_symbol(JOY_MAP, "_stage_step")
        direct = CallProbe(map_symbol(JOY_MAP, "_joy_read"))
        viacall = CallProbe(map_symbol(JOY_MAP, "_joy_read_call"))
        since = len(z.glyphs)
        z.hid.hid_queue.extend(b"joy stage\r")
        armed = False
        for index in range(BUDGET):
            self.skip_delay(z)
            self.feed(z)
            if z.pc == mark and (z.latch & 0x18) == 0x10:
                latch.latch[:] = STEPS[z.rd(step) - 1][0]
                if not armed:
                    armed = True
                    latch.reads.clear()
            if armed:
                direct(z)
                viacall(z)
            z.step()
            if index % 0x3FF == 0:
                self.pump(z)
                if "joy: stage done" in "".join(z.glyphs[since:]):
                    break
        trace = self.trace(z, since)
        self.assertIn("joy: stage done", trace)
        self.assertNotIn("failed", trace)
        for line in STAGE_LINES:
            self.assertIn(line, trace)
        lines = re.findall(r"joy: (\d+) read (.*)", trace)
        self.assertEqual([(int(number), text.strip())
                          for number, text in lines],
                         [(number, text) for number, (_bytes, text)
                          in enumerate(STEPS, 1)])

        # Each step read each pad twice (directly, then through the
        # driver): pad 0 at FCh, pad 1 at FFh, and no other port.
        self.assertEqual(latch.reads, [0xFC, 0xFC, 0xFF, 0xFF] * len(STEPS))
        self.assertEqual(len(direct.costs), 2 * len(STEPS))
        self.assertEqual(len(viacall.costs), 2 * len(STEPS))
        print("joy: joy_read %d T-states, joy_read_call %d T-states "
              "(means of %d)"
              % (sum(direct.costs) // len(direct.costs),
                 sum(viacall.costs) // len(viacall.costs),
                 len(direct.costs)))

        # Back at the prompt, and the keyboard still types.
        self.run_to(z, SHELL_PROMPT_NO_CARD, since=since)
        since = len(z.glyphs)
        z.hid.hid_queue.extend(b"echo typed\r")
        self.run_to(z, "typed\n" + SHELL_PROMPT_NO_CARD, since=since)


if __name__ == "__main__":
    unittest.main()
