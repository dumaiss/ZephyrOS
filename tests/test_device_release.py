#!/usr/bin/env python3
"""Device release and warm reboot acceptance.

The production image, the emulated card and V9958 peers, and the outcome
program `tests/device/devtest.c` on the card, run from the shell (the
boot program).  Two families:

* host: the packed store's release declaration (`"release": true` in a
  driver manifest reaches the container's endpoint flags), the ABI's op,
  flag, syscall and mode numbers, and the SDK gate's reboot number;
* replay: what a program can and cannot leave behind.  The takeover
  program holds `/dev/video` exclusively, reprograms the VDP for GRAPHIC
  4 and scribbles both VRAM pages; the display reset on release must put
  the driver's registers, palette and font atlas back and draw again on a
  clean screen.  A program that writes the VDP ports with no handle at
  all is caught by the launcher's console reset, and a warm reboot in the
  middle of a card transfer must leave a readable card behind.

The V9958 peer is the observable: `regs`, `palette`, `vram`, `commands`,
`vram_writes` and `protocol_errors` record what the driver did to the
device.  The driver's own drop counter and exclusive flag live in its
bank-6 state (`build/modules/v9958.report.json` names the symbol), so the
drop accounting is read from memory, not from the transcript.
"""

import json
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import modpack  # noqa: E402
from replay import SHELL_PROMPT  # noqa: E402
from test_hwtest_sim import HwtestReplay  # noqa: E402
from test_mod import load_driver, pack_driver, simple_image  # noqa: E402

DEVTEST_EXE = os.path.join(ROOT, "build", "user", "devtest.exe")
DEV_NAME = "DEVTEST.EXE"
MODULE_DIR = os.path.join(ROOT, "build", "modules")
V9958_JSON = os.path.join(ROOT, "drivers", "v9958", "v9958.json")
V9958_CONTAINER = os.path.join(MODULE_DIR, "v9958.mod")

# The card program's bare name: the shell's path starts with `.`, which
# is the card.  The launcher's own line names the boot program.
START = "DEVTEST.EXE"
SHELL_EXIT = "boot: /system/shell.exe exit 0"

# The lines the takeover program prints, with the lengths the drop
# counter must grow by: five console copies, all of them while the
# exclusive handle is held.
TAKEN_LINES = (("dev: claim ok", 15), ("dev: hidden", 13),
               ("dev: hold", 11), ("dev: ", 7), ("dev: leaked", 13))
TAKEN_TOTAL = sum(length for _text, length in TAKEN_LINES)
TAKEN_SUMMARY = "dev: took %s\r\n" % " ".join(
    str(length) for _text, length in TAKEN_LINES)
DRAWN_LINE = "dev: drawn"
POKED_LINE = "dev: poked\r\n"
RELEASED_TRACE = "dev: drawn"

# What the takeover program leaves in the V9958: R#0 bit 2 (M4) selects
# GRAPHIC 4, R#7 is the border color, the palette is filled with 77h and
# both VRAM pages carry the scribble byte.
TAKEN_R0 = 0x04
TAKEN_R7 = 0x0E
TAKEN_PALETTE = [0x77, 0x77]
SCRIBBLE = 0x5A
SCRIBBLE_LEN = 16
ATLAS_BASE = 0x10000            # A16=1, address 0: the normal font atlas
VISIBLE_BASE = 0x0000           # A16=0, address 0: the visible bitmap

# The whole display re-init writes both atlases (49152 bytes each), so a
# reset that re-uploaded the font is visible as tens of thousands of VRAM
# writes; a bare screen clear is a few hundred.
RESET_WRITE_FLOOR = 4096

# The bytes the transfer mode writes to the card before the reboot: the
# test holds the same literal, so a partial or duplicated transfer fails.
PAYLOAD = (b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
           b"abcdefghijklmnopqrstuvwxyz,.!?")

# One boot of the production image costs about 2.3 million emulated
# instructions, so the waits below are budgeted in boots rather than in
# STEP_LIMIT (which is a whole acceptance session): a wait that stops
# matching fails in seconds instead of running for a quarter of an hour.
BOOT_BUDGET = 4000000
EXEC_BUDGET = 3 * BOOT_BUDGET

# The VDP register numbers the console's bring-up owns and the snapshot
# compares (R#0..R#11 plus the window and wait registers).
OWNED_REGS = (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 23, 25, 26, 27, 46)


def read_text(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def header_value(path, name):
    """The numeric value of `#define <name> <value>` in one header."""
    for line in read_text(path).splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0] == "#define" and parts[1] == name:
            return int(parts[2].rstrip("uU"), 0)
    raise AssertionError("%s defines no %s" % (path, name))


def container_endpoints(raw):
    """(class, instance, version, flags, number, dispatch) per endpoint."""
    magic, segments, endpoints, dependencies, publications = struct.unpack(
        "<4sBBBB", raw[:8])
    assert magic == modpack.MAGIC, magic
    offset = modpack.HEADER_SIZE + segments * modpack.SEGMENT_SIZE
    out = []
    for index in range(endpoints):
        record = raw[offset + index * modpack.ENDPOINT_SIZE:
                     offset + (index + 1) * modpack.ENDPOINT_SIZE]
        fields = struct.unpack("<BBBBHH", record)
        out.append(fields)
    return out


class DeviceReleaseHost(unittest.TestCase):
    """Constants and packed artifacts, no emulation."""

    def test_abi_numbers(self):
        protocol = os.path.join(ROOT, "abi", "kos", "protocol.h")
        object_h = os.path.join(ROOT, "abi", "kos", "object.h")
        syscall_h = os.path.join(ROOT, "abi", "kos", "syscall.h")
        mod_h = os.path.join(ROOT, "formats", "include", "mod.h")
        errno_h = os.path.join(ROOT, "abi", "kos", "errno.h")
        self.assertEqual(header_value(protocol, "KOS_OP_ACQUIRE"), 0xFE)
        self.assertEqual(header_value(protocol, "KOS_OP_RELEASE"), 0xFF)
        self.assertEqual(header_value(protocol, "KVIDEO_OP_RESET"), 3)
        self.assertEqual(header_value(protocol, "KCONSOLE_OP_RESET"), 5)
        self.assertEqual(header_value(object_h, "KOS_BIND_EXCLUSIVE"),
                         0x8000)
        self.assertEqual(header_value(syscall_h, "KOS_SYSCALL_REBOOT"), 0x0C)
        self.assertEqual(header_value(mod_h, "MOD_EP_RELEASE"), 0x02)
        self.assertEqual(header_value(errno_h, "KOS_EBUSY"), 0x04)
        self.assertEqual(header_value(errno_h, "KOS_ENOTSUP"), 0x07)

    def test_sdk_gate_reboot(self):
        gate = read_text(os.path.join(ROOT, "user", "sdk", "src", "gate.asm"))
        self.assertRegex(gate, r"(?m)^SYS_REBOOT\s*=\s*0x0C")
        self.assertIn("_reboot", gate)
        header = read_text(
            os.path.join(ROOT, "user", "sdk", "include", "kos", "process.h"))
        self.assertIn("void reboot(void);", header)

    def test_manifest_release_declaration(self):
        manifest = json.loads(read_text(V9958_JSON))
        endpoint = manifest["endpoints"][0]
        self.assertIs(endpoint.get("release"), True,
                      "the video driver declares the release notification")
        self.assertEqual(endpoint.get("flags", 0) & modpack.EP_RELEASE, 0,
                         "release is declared by the key, not the flag")

    def test_packed_store_carries_the_release_flag(self):
        if not os.path.exists(V9958_CONTAINER):
            self.skipTest("the driver images have not been packed")
        with open(V9958_CONTAINER, "rb") as handle:
            endpoints = container_endpoints(handle.read())
        flags = [fields[3] for fields in endpoints]
        self.assertEqual(flags, [modpack.EP_RELEASE], endpoints)
        with open(os.path.join(MODULE_DIR, "v9958.report.json"),
                  encoding="utf-8") as handle:
            report = json.load(handle)
        self.assertEqual(report["endpoints"][0]["flags"],
                         modpack.EP_RELEASE)

    def test_release_key_validation_and_packing(self):
        objects, manifest = simple_image()
        manifest["endpoints"] = [{
            "class": 2, "instance": 0, "version": 1,
            "dispatch": "_entry", "flags": 0, "release": True}]
        with tempfile.TemporaryDirectory() as tmp:
            raw = pack_driver(tmp, "t", objects, manifest)
            self.assertEqual([fields[3] for fields in
                              container_endpoints(raw)],
                             [modpack.EP_RELEASE])
        objects, manifest = simple_image()
        manifest["endpoints"] = [{
            "class": 2, "instance": 0, "version": 1,
            "dispatch": "_entry", "release": "yes"}]
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(modpack.ModError):
                load_driver(tmp, "t", objects, manifest)
        objects, manifest = simple_image()
        manifest["endpoints"] = [{
            "class": 2, "instance": 0, "version": 1,
            "dispatch": "_entry"}]
        with tempfile.TemporaryDirectory() as tmp:
            raw = pack_driver(tmp, "t", objects, manifest)
            self.assertEqual([fields[3] for fields in
                              container_endpoints(raw)], [0])


class DeviceReplay(HwtestReplay):
    """The production image with the card, the V9958 peer and the
    outcome program."""

    def setUp(self):
        self.load_image()
        if not os.path.exists(DEVTEST_EXE):
            self.skipTest("%s missing; run make" % DEVTEST_EXE)

    def start(self, card=True, extra_files=None):
        files = dict(extra_files or {})
        with open(DEVTEST_EXE, "rb") as handle:
            files[DEV_NAME] = handle.read()
        return super().start(card=card, extra_files=files)

    # -- bank-6 driver state ------------------------------------------
    def v9958_state(self, z):
        """The driver's state block in bank 6 (the report names the
        symbol; the packer's plan names the tier base)."""
        if getattr(self, "_state_address", None) is None:
            with open(os.path.join(MODULE_DIR, "drivers.plan.json"),
                      encoding="utf-8") as handle:
                plan = json.load(handle)
            entry = [item for item in plan["drivers"]
                     if item["name"] == "v9958.mod"][0]
            with open(os.path.join(MODULE_DIR, "v9958.report.json"),
                      encoding="utf-8") as handle:
                report = json.load(handle)
            role = report["symbol_segments"]["_v9958_state"]
            base = {"hot": entry["hot_base"], "boot": entry["boot_base"],
                    "io": entry["io_base"] or 0,
                    "bulk": entry["bulk_base"] or 0}[role]
            self._state_address = base + report["symbols"]["_v9958_state"]
        return z.banks[6][self._state_address:self._state_address + 64]

    def dropped(self, z):
        """`dropped` sits after the exclusive flag: the struct packs one
        byte per field, so the u16 starts at offset 21."""
        state = self.v9958_state(z)
        return state[21] | (state[22] << 8)

    def exclusive(self, z):
        return self.v9958_state(z)[20]

    # -- lane and console ---------------------------------------------
    def run_lane(self, z, expect, since, limit=EXEC_BUDGET):
        """Step until `expect` appears in the serial lane.  The console
        may be dropping the same text, so the lane is the sink to wait
        on while a takeover is held."""
        needle = expect.encode("ascii")
        for i in range(limit):
            self.skip_delay(z)
            self.feed(z)
            z.step()
            if i % 0x1FF == 0 and needle in bytes(z.sio_tx)[since:]:
                return
        self.pump(z)
        self.fail("the lane did not reach %r in %d steps\nlane: %r"
                  % (expect, limit, bytes(z.sio_tx)[since:][-300:]))

    def lane(self, z, since=0):
        return bytes(z.sio_tx)[since:].decode("latin-1")

    def type_lane(self, z, line, expect, limit=EXEC_BUDGET):
        """Send one line whose output the outcome program puts on the
        lane: the console copy of that output may be dropped, the lane
        copy never is."""
        since = len(z.sio_tx)
        z.hid.hid_queue.extend(line.encode("ascii") + b"\r")
        self.run_lane(z, expect, since, limit=limit)

    def type_console(self, z, line, expect, limit=EXEC_BUDGET):
        """Send one monitor/shell line and wait for what it draws: the
        console is where a program's own output goes."""
        since = len(z.glyphs)
        z.hid.hid_queue.extend(line.encode("ascii") + b"\r")
        self.run_to(z, expect, since=since, limit=limit)

    # -- snapshots -----------------------------------------------------
    def snapshot(self, z):
        peer = z.vdp
        return {
            "regs": list(peer.regs),
            "palette": [list(entry) for entry in peer.palette],
            "atlas": bytes(peer.vram[ATLAS_BASE:ATLAS_BASE + SCRIBBLE_LEN]),
            "visible": bytes(peer.vram[VISIBLE_BASE:VISIBLE_BASE +
                                       SCRIBBLE_LEN]),
            "writes": len(peer.vram_writes),
            "commands": len(peer.commands),
        }

    def assert_restored(self, z, before, state=None):
        """The display reset rewrote every owned register, palette entry,
        VRAM page and both font atlases."""
        peer = z.vdp
        for reg in OWNED_REGS:
            self.assertEqual(peer.regs[reg], before["regs"][reg],
                             "R#%u was not restored" % reg)
        self.assertEqual(peer.palette, before["palette"],
                         "the palette was not restored")
        self.assertEqual(bytes(peer.vram[ATLAS_BASE:ATLAS_BASE +
                                         SCRIBBLE_LEN]), before["atlas"],
                         "the font atlas was not re-uploaded")
        self.assertNotEqual(
            bytes(peer.vram[VISIBLE_BASE:VISIBLE_BASE + SCRIBBLE_LEN]),
            bytes([SCRIBBLE]) * SCRIBBLE_LEN,
            "the takeover scribble is still on screen")
        clear = bytes(peer.vram[VISIBLE_BASE + 0x800:VISIBLE_BASE + 0x810])
        self.assertEqual(len(set(clear)), 1,
                         "the screen below the text is not a cleared field")
        uploaded = len(peer.vram_writes) - before["writes"]
        self.assertGreaterEqual(uploaded, RESET_WRITE_FLOOR,
                                "the reset wrote only %d VRAM bytes"
                                % uploaded)
        self.assertEqual(peer.protocol_errors, [],
                         "the takeover left protocol errors behind")
        if state is not None:
            self.assertEqual(self.exclusive(z), 0,
                             "the exclusive flag survived the release")
            self.assertEqual(self.dropped(z), state["dropped"])
        return uploaded

    def first_glyph_after(self, z, index):
        """The destination of the first 6x8 glyph copy after `index`."""
        for command in z.vdp.commands[index:]:
            block = command.get("block")
            if (command.get("code") != 0xD0 or not block or
                    (block[8] | (block[9] << 8)) != 6 or
                    (block[10] | (block[11] << 8)) != 8):
                continue
            return (block[4] | (block[5] << 8), block[6] | (block[7] << 8))
        self.fail("no glyph copy after command %d" % index)

    def taken_up(self, z):
        """The peer state a held takeover must produce."""
        peer = z.vdp
        self.assertEqual(peer.regs[0], TAKEN_R0, "R#0 is not GRAPHIC 4")
        self.assertEqual(peer.regs[7], TAKEN_R7, "R#7 keeps the border")
        self.assertEqual(peer.palette[0], TAKEN_PALETTE, "palette entry 0")
        self.assertEqual(peer.palette[15], TAKEN_PALETTE, "palette entry 15")
        for base in (VISIBLE_BASE, ATLAS_BASE):
            chunk = bytes(peer.vram[base:base + SCRIBBLE_LEN])
            self.assertEqual(chunk, bytes([SCRIBBLE]) * SCRIBBLE_LEN,
                             "the scribble at %05Xh is missing" % base)


class TestExclusiveTakeover(DeviceReplay):
    """A program that holds the display, and what its release restores."""

    def test_exit_release_restores_the_display(self):
        z = self.start()
        before = self.snapshot(z)
        lane = len(z.sio_tx)
        trace = len(z.glyphs)
        z.hid.hid_queue.extend((START + " claim\r").encode())
        self.run_lane(z, TAKEN_SUMMARY, lane)
        # Nothing is drawn between this point and the release.
        held_commands = len(z.vdp.commands)
        held = self.snapshot(z)
        self.taken_up(z)
        self.assertEqual(self.exclusive(z), 1,
                         "the takeover did not hold the object")
        self.assertEqual(self.dropped(z), TAKEN_TOTAL,
                         "the drop counter does not match the held lines")
        # The program got the whole length back from every dropped write.
        self.assertIn(TAKEN_SUMMARY, self.lane(z, lane))
        # Nothing the program wrote while holding was drawn, and the
        # display really is still in the takeover state.
        drawn = "".join(z.glyphs)[trace:]
        for text, _length in TAKEN_LINES:
            self.assertNotIn(text, drawn, "%r reached the screen" % text)
        self.assertEqual(len(z.vdp.regs), 47)
        self.assertNotEqual(held["regs"], before["regs"],
                            "the takeover changed no register")

        # The exit unbinds the handle: the driver re-runs its whole
        # display init, so the shell's prompt comes back on a clean
        # screen with the driver's registers, palette and atlas.
        self.run_to(z, SHELL_PROMPT, since=trace, limit=EXEC_BUDGET)
        self.assertEqual(self.exclusive(z), 0,
                         "the exit did not release the object")
        self.assert_restored(z, before, state={"dropped": TAKEN_TOTAL})
        # The first output the release lets through is the shell's
        # prompt, and it starts a fresh model line: the screen was cleared
        # and the cursor went home.
        self.assertIn("\n" + SHELL_PROMPT, "".join(z.glyphs)[trace:],
                      "the prompt did not start on a clean row")
        dx, dy = self.first_glyph_after(z, held_commands)
        self.assertEqual((dx, dy % 8), (0, 0),
                         "the first glyph after the reset is not at home")

    def test_explicit_unbind_draws_again(self):
        z = self.start()
        before = self.snapshot(z)
        lane = len(z.sio_tx)
        trace = len(z.glyphs)
        z.hid.hid_queue.extend((START + " release\r").encode())
        self.run_lane(z, TAKEN_SUMMARY, lane)
        self.taken_up(z)
        self.run_lane(z, RELEASED_TRACE, lane, limit=EXEC_BUDGET)
        self.run_to(z, SHELL_PROMPT, since=trace, limit=EXEC_BUDGET)
        self.assertEqual(self.exclusive(z), 0)
        self.assert_restored(z, before, state={"dropped": TAKEN_TOTAL})
        drawn = "".join(z.glyphs)[trace:]
        self.assertIn(RELEASED_TRACE, drawn,
                      "the line after the explicit unbind was not drawn")
        self.assertIn("\n" + RELEASED_TRACE, drawn,
                      "the drawn line does not start a fresh model line")

    def test_busy_and_unsupported_rules(self):
        z = self.start()
        lane = len(z.sio_tx)
        trace = len(z.glyphs)
        z.hid.hid_queue.extend((START + " busy\r").encode())
        self.run_lane(z, "dev: busy end\r\n", lane)
        transcript = self.lane(z, lane)
        # A second exclusive bind is refused with -KOS_EBUSY, release then
        # acquire succeeds, and devices whose driver declares no release
        # answer -KOS_ENOTSUP (the console and the serial lane).
        for line in ("dev: first ok\r\n", "dev: second -4\r\n",
                     "dev: third ok\r\n", "dev: console -7\r\n",
                     "dev: lane -7\r\n", "dev: busy end\r\n"):
            self.assertIn(line, transcript)
        self.assertNotIn("dev: first -", transcript)
        self.run_to(z, SHELL_PROMPT, since=trace, limit=EXEC_BUDGET)
        # The rules left no handle behind and no protocol errors in the
        # trace of the object.
        self.assertEqual(self.exclusive(z), 0)
        self.assertEqual(z.vdp.protocol_errors, [])


class TestLauncherReset(DeviceReplay):
    """What the launcher's console reset catches."""

    def test_poke_without_a_handle_is_reset_before_the_restart(self):
        z = self.start()
        before = self.snapshot(z)
        trace = len(z.glyphs)
        lane = len(z.sio_tx)
        z.hid.hid_queue.extend((START + " poke\r").encode())
        self.run_lane(z, POKED_LINE, lane)
        self.taken_up(z)  # no handle, no release: the display stays broken
        self.run_to(z, SHELL_PROMPT, since=trace, limit=EXEC_BUDGET)
        self.assertEqual(self.exclusive(z), 0)

        # `exit` ends the boot program; the launcher resets the console,
        # so the restarted shell starts on the driver's own display and a
        # line queued behind the exit never reaches the new program.
        trace = len(z.glyphs)
        z.hid.hid_queue.extend(b"exit\rR\r")
        self.run_to(z, SHELL_EXIT, since=trace, limit=2 * EXEC_BUDGET)
        self.run_to(z, SHELL_PROMPT, since=trace, limit=2 * EXEC_BUDGET)
        fresh = "".join(z.glyphs)[trace:]
        self.assertIn(SHELL_EXIT, fresh)
        self.assertIn(SHELL_PROMPT, fresh)
        self.assertNotIn("command not found", fresh,
                         "input typed for the old program reached the new one")
        self.assert_restored(z, before, state={"dropped": 0})

    def test_console_reset_operation_restores_the_display(self):
        z = self.start()
        before = self.snapshot(z)
        lane = len(z.sio_tx)
        trace = len(z.glyphs)
        z.hid.hid_queue.extend((START + " creset\r").encode())
        self.run_lane(z, POKED_LINE, lane)
        self.taken_up(z)
        self.run_lane(z, "dev: creset 0\r\n", lane)
        self.run_to(z, SHELL_PROMPT, since=trace, limit=EXEC_BUDGET)
        self.assert_restored(z, before, state={"dropped": 0})


class TestWarmReboot(DeviceReplay):
    """The warm reboot: the ROM reset vector, the store and the card."""

    def run_steps(self, z, predicate, limit=BOOT_BUDGET):
        """Step in chunks, decoding glyphs between them, until the
        predicate holds.  `run_to` and `run_until_count` inspect the whole
        transcript, which is what a boot-spanning wait must not do."""
        steps = 0
        while steps < limit:
            for _ in range(0x1FFF):
                self.skip_delay(z)
                self.feed(z)
                z.step()
            steps += 0x1FFF
            self.pump(z)
            if predicate():
                return
        self.pump(z)
        self.fail("the predicate stayed false for %d steps (PC=%04X)\n"
                  "trace: %r" % (limit, z.pc, "".join(z.glyphs)[-200:]))

    def boot(self, z, command, count, limit=BOOT_BUDGET):
        """Send one reboot command (empty for a reboot the program under
        test already asked for) and wait for the `count`-th boot-program
        start and for its prompt."""
        starts = "".join(z.glyphs).count(SHELL_PROMPT)
        position = len(z.glyphs)
        if command:
            z.hid.hid_queue.extend(command)

        def landed():
            fresh = "".join(z.glyphs[position:])
            return (starts + fresh.count(SHELL_PROMPT) >= count and
                    SHELL_PROMPT in fresh)

        self.run_steps(z, landed, limit=limit)

    def test_reboot_loop_and_the_card_still_reads(self):
        z = self.start()
        # The shell's own built-in first, then the monitor's `B`: the
        # same syscall from a running program, each back at a fresh boot
        # program.  The loop is twenty reboots, and the other nineteen
        # reach the monitor through the shell's lookup to drive it.
        self.boot(z, b"reboot\r", 2)
        for index in range(1, 20):
            self.boot(z, b"monitor\rB\r", index + 2)
        self.type_console(z, "ls", "DEVTEST.EXE")
        print("device release: 20 reboots reached the prompt, "
              "the card still reads")

    def test_reboot_during_a_file_transfer_clears_the_slots(self):
        z = self.start()
        peer = z.ioc_peer
        lane = len(z.sio_tx)
        z.hid.hid_queue.extend((START + " write\r").encode())
        self.run_lane(z, "dev: writing %d\r\n" % len(PAYLOAD), lane,
                      limit=BOOT_BUDGET)
        self.assertIn("dev: writing %d\r\n" % len(PAYLOAD),
                      self.lane(z, lane))
        open_slots = [entry for entry in peer.fs2_slots if entry is not None]
        self.assertEqual(len(open_slots), 1,
                         "the transfer left no controller slot open")
        resets = peer.fs2_counts[peer.CMD_FS2_RESET]

        self.boot(z, b"", 2)
        # The card still reads, and the driver dropped the slot the dead
        # Z80 side never closed: its init resets the controller's
        # filesystem contexts on every boot.
        self.assertEqual([entry for entry in peer.fs2_slots
                          if entry is not None], [],
                         "a stale controller slot survived the reboot")
        self.assertGreater(peer.fs2_counts[peer.CMD_FS2_RESET], resets,
                           "the driver did not reset the controller")
        self.assertEqual(bytes(peer.fs2_files[("DEVTEST.DAT",)]), PAYLOAD,
                         "the card does not hold what the program wrote")
        self.type_console(z, "cat DEVTEST.DAT", PAYLOAD.decode("ascii"))
        print("device release: a reboot mid-transfer left a readable card")

if __name__ == "__main__":
    unittest.main()
