#!/usr/bin/env python3
"""The mouse: the HID driver's MOUSE endpoint and the SDK's tracker.

Two families:

* host: the controller vocabulary the driver shares with the firmware
  (`drivers/include/ioc_protocol.h`), the MOUSE type and class, the
  protocol operation, the HID manifest's second endpoint and
  publication, the packed container and the production set's `/dev`
  rows and endpoints against the nucleus tables;
* replay: the production image with the emulated controller scripted
  (`IocCommandPeer.mouse_reports`), `mouse stage` typed at the shell.
  `/system/mouse.exe` binds `/dev/input/mouse`, tracks eighteen reads
  over the 510 x 208 text area, moves the pointer to each position and
  prints the state after each; the replay holds the deltas, the clamping
  at both edges, the wheel, the press latch, `present` falling for a
  controller without the command, and the backoff after a transport
  failure: four reads that send no IOC command, then a read that does
  and gets the next report.  Then the pointer (KVIDEO_OP_POINTER): at
  each of the program's checkpoints the replay reads the sprite table
  the V9958 peer holds - slot 0 the cursor, slot 1 the pointer, slot 2
  the D8h terminator, a hidden slot the blank pattern at Y 0, Y D8h
  drawn at D7h - and judges the scroll re-placement, sprites staying on
  through console drawing and a RENDER while the pointer is shown, the
  refusal with no port write while the video is held, and the resets
  that hide it.

Skipped where the production image has not been built.
"""

import json
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import modpack  # noqa: E402
from maptools import evaluate, parse_c_header  # noqa: E402
from test_hwtest_sim import HwtestReplay, SHELL_PROMPT_NO_CARD  # noqa: E402

IOC_PROTOCOL_H = os.path.join(ROOT, "drivers", "include", "ioc_protocol.h")
PROTOCOL_H = os.path.join(ROOT, "abi", "kos", "protocol.h")
OBJECT_H = os.path.join(ROOT, "abi", "kos", "object.h")
VFS_H = os.path.join(ROOT, "kernel", "vfs", "vfs.h")
IPC_C = os.path.join(ROOT, "kernel", "ipc", "ipc.c")
HID_JSON = os.path.join(ROOT, "drivers", "hid", "hid.json")
HID_H = os.path.join(ROOT, "drivers", "hid", "hid_driver.h")
MODULES = os.path.join(ROOT, "build", "modules")
HID_MOD = os.path.join(MODULES, "hid.mod")
PLAN = os.path.join(MODULES, "drivers.plan.json")
MOUSE_EXE = os.path.join(ROOT, "build", "user", "mouse.exe")
MOUSE_MAP = os.path.join(ROOT, "build", "user", "mouse", "mouse.map")

# The script: one entry per CMD_HID_MOUSE the controller answers, a
# (flags, buttons, pressed, dx, dy, wheel) report or a fault.  Flags 3 is
# a mounted interface whose reports decode as the mouse.
SCRIPT = [
    (3, 0, 0, 10, -6, 0),        # 1 motion; the mouse appears
    (3, 0, 0, 0, 0, 0),          # 2 nothing
    (3, 1, 1, 0, 0, 0),          # 3 left goes down
    (3, 1, 0, 0, 0, 0),          # 4 held: the press is still latched
    (3, 0, 0, 1000, 500, 0),     # 5 to the right and bottom edges
    (3, 0, 0, 7, 9, 0),          # 6 held at the edges
    (3, 0, 0, -2000, -300, 0),   # 7 to the left and top edges
    (3, 0, 0, -1, -1, 0),        # 8 held at the edges
    (3, 0, 0, 0, 0, 3),          # 9 wheel
    (3, 0, 0, 0, 0, -1),         # 10 wheel back
    "unknown",                   # 11 a controller without the command
    "transport",                 # 12 the exchange fails
    (3, 0, 0, 20, 0, 0),         # 17 the first read after the penalty
    (3, 0, 0, 0, 0, 0),          # 18 nothing
]

# The lines the stage prints: read, result, x, y, buttons, pressed,
# wheel, present.  Reads 13-16 are the penalty: zeros, no command.
EXPECTED = [
    (1, 1, 265, 98, 0, 0, 0, 1),
    (2, 0, 265, 98, 0, 0, 0, 1),
    (3, 1, 265, 98, 1, 1, 0, 1),
    (4, 0, 265, 98, 1, 1, 0, 1),
    (5, 1, 509, 207, 0, 0, 0, 1),
    (6, 0, 509, 207, 0, 0, 0, 1),
    (7, 1, 0, 0, 0, 0, 0, 1),
    (8, 0, 0, 0, 0, 0, 0, 1),
    (9, 1, 0, 0, 0, 0, 3, 1),
    (10, 1, 0, 0, 0, 0, 2, 1),
    (11, 1, 0, 0, 0, 0, 2, 0),
    (12, 0, 0, 0, 0, 0, 2, 0),
    (13, 0, 0, 0, 0, 0, 2, 0),
    (14, 0, 0, 0, 0, 0, 2, 0),
    (15, 0, 0, 0, 0, 0, 2, 0),
    (16, 0, 0, 0, 0, 0, 2, 0),
    (17, 1, 20, 0, 0, 0, 2, 1),
    (18, 0, 20, 0, 0, 0, 2, 1),
]
# The IOC commands each read sends: none during the penalty.
COMMANDS = [1] * 12 + [0] * 4 + [1] * 2

# The sprite table in the peer's VRAM (the driver's F200h with A16 set):
# three four-byte slots.
SAT = 0x1F200
SAT_END = 0xD8
HIDDEN = (0x00, 0x00, 2, 0)
R8_SPRITES_OFF = 0x02
POINTER_LINES = [
    "mouse: pointer sweep 0",
    "mouse: pointer range -2 -2 -2",
    "mouse: pointer render 2382 hide 0 render 2382",
    "mouse: pointer held 0 busy -4",
    "mouse: pointer reset 0",
]

LINE = re.compile(r"mouse: (\d+) (-?\d+) at (\d+),(\d+) buttons (\d+) "
                  r"pressed (\d+) wheel (-?\d+) present (\d+)")
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


class TestMouseContract(unittest.TestCase):
    """The shared vocabulary, the type and the manifest."""

    def test_the_controller_vocabulary(self):
        for name, number in (("IOC_CMD_HID_MOUSE", 0x12),
                             ("IOC_RSP_HID_MOUSE", 0x92),
                             ("IOC_RSP_UNKNOWN_CMD", 0xFE),
                             ("IOC_OFF_HID_MOUSE_FLAGS", 0),
                             ("IOC_OFF_HID_MOUSE_BUTTONS", 1),
                             ("IOC_OFF_HID_MOUSE_PRESSED", 2),
                             ("IOC_OFF_HID_MOUSE_DX", 3),
                             ("IOC_OFF_HID_MOUSE_DY", 5),
                             ("IOC_OFF_HID_MOUSE_WHEEL", 7),
                             ("IOC_HID_MOUSE_LEN", 9),
                             ("IOC_HID_MOUSE_MOUNTED", 0x01),
                             ("IOC_HID_MOUSE_DECODED", 0x02),
                             ("IOC_FW_LEVEL_PINNED", 75)):
            self.assertEqual(value(IOC_PROTOCOL_H, name), number, name)

    def test_the_type_class_and_operation(self):
        self.assertEqual(value(OBJECT_H, "KOS_TYPE_MOUSE"), 11)
        self.assertEqual(value(PROTOCOL_H, "KOS_CLASS_MOUSE"), 11)
        self.assertEqual(value(PROTOCOL_H, "KMOUSE_OP_READ"), 0)
        self.assertEqual(value(PROTOCOL_H, "KMOUSE_REPORT_LEN"),
                         value(IOC_PROTOCOL_H, "IOC_HID_MOUSE_LEN"))
        self.assertEqual(modpack.TYPES["MOUSE"], 11)

    def test_the_tables_hold_the_production_set(self):
        # The packer's copy of the nucleus row count is the nucleus's.
        self.assertEqual(value(VFS_H, "VFS_NS_PUB_MAX"), 16)
        self.assertEqual(modpack.NS_TABLE_ROWS,
                         value(VFS_H, "VFS_NS_PUB_MAX"))

    def test_the_manifest_adds_the_mouse_endpoint(self):
        manifest = json.loads(read(HID_JSON))
        self.assertEqual(manifest["endpoints"][1],
                         {"class": 11, "instance": 0, "version": 1,
                          "dispatch": "hid_mouse_dispatch", "flags": 0})
        self.assertEqual(manifest["publishes"][1],
                         {"path": "/dev/input/mouse", "type": "MOUSE",
                          "endpoint": {"class": 11, "instance": 0},
                          "object": 0})
        self.assertEqual(manifest["depends"], ["IOC"])

    def test_the_backoff_uses_the_keyboard_constants(self):
        self.assertEqual(value(HID_H, "KHID_STUCK_PENALTY"), 4)
        self.assertEqual(value(HID_H, "KHID_STUCK_PENALTY_MAX"), 128)


class TestMouseArtifacts(unittest.TestCase):
    """The packed container and the production plan."""

    def setUp(self):
        if not os.path.exists(PLAN) or not os.path.exists(HID_MOD):
            self.skipTest("the driver images have not been packed; run make")

    def test_the_container_publishes_both_inputs(self):
        with open(HID_MOD, "rb") as handle:
            info = modpack.parse_container(handle.read(), "hid.mod")
        self.assertEqual([(publication["name"], publication["type"])
                          for publication in info["publications"]],
                         [("/input/keyboard", 4), ("/input/mouse", 11)])
        self.assertEqual([(endpoint["class"], endpoint["instance"])
                          for endpoint in info["endpoints"]],
                         [(4, 0), (11, 0)])

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
                if "/" in name[1:]:
                    rows.add(name.rsplit("/", 1)[0])
        # Eight drivers' endpoints, the mouse's among them, plus the
        # nucleus FILE provider in a 12-entry table.
        self.assertEqual(endpoints, 9)
        self.assertLessEqual(endpoints + 1, 12)
        self.assertEqual(sorted(rows),
                         ["/console", "/input", "/input/keyboard",
                          "/input/mouse", "/ioc", "/serial", "/serial/0",
                          "/sound", "/timer", "/video"])
        self.assertLessEqual(len(rows), value(VFS_H, "VFS_NS_PUB_MAX"))


class TrackProbe:
    """Every `mouse_track` call of the stage: its T-states from entry to
    return, and the IOC mouse commands the controller answered during
    it."""

    def __init__(self, z, entry):
        self.z = z
        self.entry = entry
        self.calls = []
        self.open = None

    def __call__(self, z):
        if self.open is None:
            if z.pc == self.entry and (z.latch & 0x18) == 0x10:
                ret = z.rd16(z.sp)
                self.open = (ret, z.sp + 2, z.bank_of(z.pc), z.tstates,
                             z.ioc_peer.mouse_commands)
            return
        ret, sp, bank, start, commands = self.open
        if z.pc == ret and z.sp == sp and (z.latch & 0x18) == 0x10 and \
                z.bank_of(z.pc) == bank:
            self.calls.append((z.tstates - start,
                               z.ioc_peer.mouse_commands - commands))
            self.open = None


def sprite_y(y, origin):
    """Slot 1's Y for pixel row y: one line up on the R#23 origin, D8h
    drawn one line higher."""
    value = (y + origin - 1) & 0xFF
    return value - 1 if value == SAT_END else value


class MarkProbe:
    """The stage's checkpoints: at each stage_mark entry, the step, the
    T-states, the peer's write counts and the sprite table."""

    def __init__(self, z, entry, step):
        self.z = z
        self.entry = entry
        self.step = step
        self.marks = {}

    def __call__(self, z):
        if z.pc != self.entry or (z.latch & 0x18) != 0x10:
            return
        vdp = z.vdp
        self.marks[z.rd(self.step)] = {
            "tstates": z.tstates,
            "vram": len(vdp.vram_writes),
            "regs": len(vdp.reg_writes),
            "ports": (len(vdp.vram_writes), len(vdp.reg_writes),
                      len(vdp.addr_writes), len(vdp.indirect_writes),
                      len(vdp.palette_writes), len(vdp.latch_writes)),
            "origin": (vdp.regs[23] + 4) & 0xFF,
            "slots": [tuple(vdp.vram[SAT + 4 * slot:SAT + 4 * slot + 4])
                      for slot in range(2)],
            "end": vdp.vram[SAT + 8],
        }


def slot_entries(vram_writes):
    """Every committed slot entry in write order: (write index, slot,
    entry), an entry being complete when its fourth byte is written; the
    terminator slot as (index, 2, value)."""
    sat = bytearray(12)
    entries = []
    for index, (address, value) in enumerate(vram_writes):
        if not SAT <= address < SAT + 12:
            continue
        offset = address - SAT
        sat[offset] = value
        if offset in (3, 7):
            slot = offset // 4
            entries.append((index, slot, tuple(sat[4 * slot:4 * slot + 4])))
        elif offset == 8:
            entries.append((index, 2, value))
    return entries


class TestMouseReplay(HwtestReplay):
    """`mouse stage` at the production shell, the controller scripted."""

    def setUp(self):
        self.load_image()
        if not os.path.exists(MOUSE_EXE):
            self.skipTest("%s missing; run make" % MOUSE_EXE)

    def test_the_stage_tracks_clamps_latches_and_backs_off(self):
        z = self.start(card=False)
        peer = z.ioc_peer
        peer.mouse_reports.extend(SCRIPT)
        probe = TrackProbe(z, map_symbol(MOUSE_MAP, "_mouse_track"))
        marks = MarkProbe(z, map_symbol(MOUSE_MAP, "_stage_mark"),
                          map_symbol(MOUSE_MAP, "_stage_step"))
        since = len(z.glyphs)
        z.hid.hid_queue.extend(b"mouse stage\r")
        armed = False
        for index in range(BUDGET):
            self.skip_delay(z)
            self.feed(z)
            if armed:
                probe(z)
                marks(z)
            z.step()
            if index % 0x3FF == 0:
                self.pump(z)
                trace = "".join(z.glyphs[since:])
                if not armed and "mouse: start" in trace:
                    armed = True
                if "mouse: stage done" in trace:
                    break
        trace = self.trace(z, since)
        self.assertIn("mouse: stage done", trace)
        self.assertNotIn("failed", trace)

        # The name resolves to the MOUSE type, and the tracker starts at
        # the centre with nothing held and no mouse seen.
        self.assertIn("mouse: bind 0 type 11", trace)
        self.assertIn("mouse: start 255,104 buttons 0 pressed 0 wheel 0 "
                      "present 0", trace)
        lines = [tuple(int(field) for field in match)
                 for match in LINE.findall(trace)]
        self.assertEqual(lines, EXPECTED)

        # Every scripted reply was taken, by exactly the reads outside
        # the penalty.
        self.assertEqual(list(peer.mouse_reports), [])
        self.assertEqual(peer.mouse_commands, len(SCRIPT))
        self.assertEqual([commands for _cost, commands in probe.calls],
                         COMMANDS)
        costs = [cost for cost, commands in probe.calls if commands]
        idle = [cost for cost, commands in probe.calls if not commands]
        print("mouse: mouse_track %d T-states with an IOC command "
              "(mean of %d), %d during the penalty"
              % (sum(costs) // len(costs), len(costs),
                 sum(idle) // len(idle)))

        for line in POINTER_LINES:
            self.assertIn(line, trace)
        self.check_pointer(z, marks.marks)

        # Back at the prompt, and the keyboard still types.
        self.run_to(z, SHELL_PROMPT_NO_CARD, since=since)
        since = len(z.glyphs)
        z.hid.hid_queue.extend(b"echo typed\r")
        self.run_to(z, "typed\n" + SHELL_PROMPT_NO_CARD, since=since)

    def check_pointer(self, z, marks):
        self.assertEqual(sorted(marks), list(range(1, 14)))
        vdp = z.vdp
        entries = slot_entries(vdp.vram_writes)

        # The layout, always: slot 0 the cursor (pattern 0) or blank,
        # slot 1 the pointer (pattern 1) or blank, a blank slot at Y 0, a
        # shown one never at D8h, and slot 2 the terminator.
        for _index, slot, entry in entries:
            if slot == 2:
                self.assertEqual(entry, SAT_END)
                continue
            y, _x, pattern, tail = entry
            self.assertIn(pattern, (slot, 2), entry)
            self.assertEqual(tail, 0, entry)
            if pattern == 2:
                self.assertEqual(y, 0, entry)
            else:
                self.assertNotEqual(y, SAT_END, entry)
        for step, mark in marks.items():
            self.assertEqual(mark["end"], SAT_END, step)

        # The last read left the pointer at (20, 0).
        first = marks[1]
        self.assertEqual(first["slots"][1],
                         (sprite_y(0, first["origin"]), 10, 1, 0))

        # The sweep: one slot-1 write per row, no slot-0 write, and the
        # row whose Y would be D8h drawn at D7h.
        origin = first["origin"]
        sweep = [entry for index, slot, entry in entries
                 if first["vram"] <= index < marks[2]["vram"]]
        self.assertEqual(sweep, [(sprite_y(y, origin), y, 1, 0)
                                 for y in range(208)])
        self.assertIn((0xD7, (0xD9 - origin) & 0xFF, 1, 0), sweep,
                      "the sweep must reach the D8h row (origin %d)"
                      % origin)

        # Placed at (100, 50); hiding the cursor leaves slot 1 alone and
        # blanks slot 0.
        placed = (sprite_y(50, marks[3]["origin"]), 50, 1, 0)
        self.assertEqual(marks[3]["slots"][1], placed)
        self.assertEqual(marks[4]["slots"][1], placed)
        self.assertEqual(marks[4]["slots"][0][2:], (2, 0))
        self.assertEqual(marks[4]["slots"][0][0], 0)
        self.assertEqual(marks[3]["slots"][0][2], 0)

        # Scrolling (thirty lines, then the render's SCROLL record) moves
        # the origin; the pointer stays at its screen pixel.
        for step in (5, 6):
            self.assertNotEqual(marks[step]["origin"],
                                marks[step - 2 if step == 5 else 5]
                                ["origin"], step)
            self.assertEqual(marks[step]["slots"][1],
                             (sprite_y(50, marks[step]["origin"]), 50, 1,
                              0), step)

        # Sprites stay on while the pointer is shown: no R#8 write sets
        # SPD from the sweep through the shown render, and that render
        # wrote R#8 (the bracket's restore).  Hidden, the bracket turns
        # them off again around the second render.
        def r8(start, end):
            return [value for value, reg, _now
                    in vdp.reg_writes[marks[start]["regs"]:
                                      marks[end]["regs"]]
                    if reg == 8]

        self.assertTrue(r8(5, 6))
        self.assertEqual([value for value in r8(1, 7)
                          if value & R8_SPRITES_OFF], [])
        self.assertTrue([value for value in r8(7, 8)
                         if value & R8_SPRITES_OFF])

        # Hidden: the blank pattern, the cursor untouched.
        self.assertEqual(marks[7]["slots"][1], HIDDEN)
        self.assertEqual(marks[7]["slots"][0], marks[6]["slots"][0])

        # Held exclusively: the refused move touched no port.
        self.assertEqual(marks[9]["ports"], marks[10]["ports"])
        # The hold's release and KVIDEO_OP_RESET both hide it.
        self.assertEqual(marks[11]["slots"][1], HIDDEN)
        self.assertEqual(marks[12]["slots"][1],
                         (sprite_y(40, marks[12]["origin"]), 15, 1, 0))
        self.assertEqual(marks[13]["slots"][1], HIDDEN)
        self.assertEqual(marks[13]["slots"][0][2], 0)

        moves = (marks[2]["tstates"] - marks[1]["tstates"]) // 208
        shown = marks[6]["tstates"] - marks[5]["tstates"]
        hidden = marks[8]["tstates"] - marks[7]["tstates"]
        print("mouse: pointer_move %d T-states (mean of 208); full-screen "
              "render %d T-states pointer shown, %d hidden"
              % (moves, shown, hidden))


if __name__ == "__main__":
    unittest.main()
