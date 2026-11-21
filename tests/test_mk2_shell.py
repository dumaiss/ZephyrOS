#!/usr/bin/env python3
"""The Zephyr-80 Mk2 production image booting to the shell.

The image (`make BOARD=mk2`, build-mk2/konaos-mk2.bin) runs on the
simulator's Mk2 board with the production peers: the V9958 console, the
IOC (keyboard input, the card behind IOC-FAT, the reset command) and the
CTC.  The resident drivers load into the driver domain's pages, the
console driver binds, and the launcher starts /system/shell.exe from the
store; the tests type at the shell through the keyboard lane and read
the screen back.

What they pin (architecture 2.7.6): the boot to the prompt with and
without a card, the shell's built-in commands, an EXE from the store and
one from the card returning their exit status, the CWD a child inherits,
IPC data through a driver read (a file on the card), SYS_REBOOT through
the IOC back to the pass-mode ROM view and a second boot, and the page
counts after a program ran ten times.  TestIpcCost runs one null call and
one 512-byte driver read on the Mk1 and Mk2 production images on the same
harness (the T-states are informational; the test checks the results).

Skipped when the image has not been built.
"""

import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from replay import STEP_LIMIT, ConsoleReplay  # noqa: E402
from z80sim import (Ctc, IocCommandPeer, StopSet, V9958Peer,  # noqa: E402
                    Z80)

MK2_DIR = "build-mk2"
MK2_IMAGE = "konaos-mk2.bin"
MK2_MAP = "konaos-mk2.map"

PAGE = 0x2000
SET0_PAGES = (0x43, 0x44)       # memory.inc KOS_COLD_FIRST_PAGE, set 0
FIXED_PAGES = (0x41, 0x42)      # system entries 1-2
SRAM_FIRST, SRAM_PAGES = 0x40, 0x40
BOOT_SIO = (b"KonaOS 0.0\r\n"
            b"platform: Zephyr-80 Mk2\r\n"
            b"kernel entry: OK\r\n"
            b"serial: OK\r\n")
NO_CARD_PROMPT = "/system> "
CARD_PROMPT = "/> "
NO_FILESYSTEM = "shell: no filesystem, starting in /system"
STOP = 0x5F00                   # the ISR stack's base: never code

SYS_BIND, SYS_CALL = 0x06, 0x07
KFS_O_READ = 0x01
KFILE_OP_READ, KFILE_OP_SEEK = 0, 2
KJOY_OP_READ = 0
DESC_OUT, DESC_NONE = 1, 0xFF


class ShellReplay(ConsoleReplay):
    """A production image at its shell prompt, on the simulated board."""

    @staticmethod
    def check(condition, message=""):
        if not condition:
            raise AssertionError(message)

    def type_line(self, z, line, expect):
        since = len(z.glyphs)
        z.hid.hid_queue.extend(line.encode("ascii") + b"\r")
        self.run_to(z, expect, since=since)
        return self.trace(z, since)

    def idle(self, z):
        """Run to the shell's idle HALT: the CPU in the program's own code
        and mapping."""
        reason, _ = z.run(STEP_LIMIT, halt=True)
        self.check(reason == "halt", reason)

    def syscall(self, z, number, block, code=0x1C00):
        """One RST 30h from the shell's page 0 (the machine is not resumed
        afterwards: the fragment overwrites shell code).  Interrupts
        masked; returns (HL, carry, T-states from the RST to the next
        instruction)."""
        z.wr(code, 0xF7)
        z.wr(code + 1, 0x00)
        z.halted = False
        z.iff1 = z.iff2 = False
        z.pc, z.a, z.hl = code, number, block
        start = z.tstates
        reason, _ = z.run(STEP_LIMIT, stops=StopSet([code + 1]),
                          check_first=False)
        self.check(reason == "stop", bytes(z.sio_tx)[-120:])
        return z.hl, z.f & 1, z.tstates - start

    def put(self, z, addr, data):
        for index, byte in enumerate(data):
            z.wr(addr + index, byte)

    def bind(self, z, path, flags=0, block=0x1D00, text=0x1D80):
        self.put(z, text, path.encode("ascii") + b"\0")
        self.put(z, block, struct.pack("<HHHBB", text, len(path), flags,
                                       0, 0))
        handle, carry, _ = self.syscall(z, SYS_BIND, block)
        self.check(not carry, "bind %s: error %d" % (path, z.a))
        return handle

    def call(self, z, handle, op, arg1=0, buffer=0, length=0,
             direction=DESC_NONE, payload=b"", block=0x1D00):
        frame = (bytes([op, 0, DESC_NONE, 0]) + struct.pack("<H", arg1) +
                 payload.ljust(24, b"\0") + b"\0\0")
        self.put(z, block, bytes([handle, op, 0, 32]) +
                 struct.pack("<HHBB", buffer, length, direction, 0) +
                 frame + b"\0\0")
        status, carry, tstates = self.syscall(z, SYS_CALL, block)
        self.check(not carry, "call: error %d" % z.a)
        return (status - 0x10000 if status & 0x8000 else status), tstates


class Mk2ShellReplay(ShellReplay):
    """The Mk2 production image: booted from reset (the ROM bootstrap
    runs), so the serial-connect delay runs too."""

    def make_cpu(self, card=False):
        z = Z80(self.image, b"", board="mk2")
        z.vdp = z.vdp_peer = V9958Peer()
        z.hid = z.ioc_peer = IocCommandPeer(fs2_media=card)
        Ctc().attach(z)
        z.glyphs = []
        z.glyph_pos = 0
        z.glyph_last = None
        z.glyph_cells = {}
        z.glyph_next = None
        z.glyph_burst = False
        return z

    def drive(self, z, limit, watch=None, hook=None, every=0x1FFF,
              tick=None, delay=True):
        return super().drive(z, limit, watch, hook, every, tick,
                             delay=False)

    def start(self, card=False):
        z = self.make_cpu(card)
        self.run_to(z, CARD_PROMPT if card else NO_CARD_PROMPT)
        return z

    def nucleus(self, addr):
        """A fixed-region address as a physical one."""
        return FIXED_PAGES[(addr >> 13) - 1] * PAGE | (addr & 0x1FFF)

    def free_pages(self, z):
        owner = self.nucleus(self.syms["_kos_page_owner"])
        return sum(1 for byte in z.phys[owner:owner + SRAM_PAGES]
                   if byte == 0)

    def call_fixed(self, z, name, a=0, hl=0, de=0):
        """Run one fixed-region function from a stopped machine, on the
        ISR stack (not resumed afterwards).  Returns DE."""
        z.halted = False
        z.iff1 = z.iff2 = False
        z.sp = STOP + 0x80
        z.push16(STOP)
        z.a, z.hl, z.de = a, hl, de
        z.pc = self.syms[name]
        reason, _ = z.run(STEP_LIMIT, stops=StopSet([STOP]),
                          check_first=False)
        self.check(reason == "stop", "%s did not return" % name)
        return z.de


@unittest.skipUnless(os.path.isfile(os.path.join(ROOT, MK2_DIR, MK2_IMAGE)),
                     "make BOARD=mk2 builds the image")
class Mk2ShellCase(Mk2ShellReplay, unittest.TestCase):
    def setUp(self):
        self.load_image(MK2_DIR, MK2_IMAGE, MK2_MAP)


class TestBoot(Mk2ShellCase):
    def test_boot_lands_on_the_prompt_without_a_card(self):
        z = self.start()
        self.assertEqual(bytes(z.sio_tx), BOOT_SIO)
        trace = self.trace(z)
        self.assertIn("console: driver OK", trace)
        self.assertIn(NO_FILESYSTEM, trace)
        self.assertTrue(trace.endswith(NO_CARD_PROMPT), trace[-80:])
        self.assertNotIn("boot: ", trace)

    def test_boot_lands_on_the_card_root(self):
        z = self.start(card=True)
        trace = self.trace(z)
        self.assertNotIn(NO_FILESYSTEM, trace)
        self.assertTrue(trace.endswith(CARD_PROMPT), trace[-80:])

    def test_built_in_commands(self):
        z = self.start()
        self.assertIn("\n/system\n",
                      self.type_line(z, "pwd", NO_CARD_PROMPT))
        self.assertIn("\nhi\n", self.type_line(z, "echo hi",
                                                NO_CARD_PROMPT))
        listing = self.type_line(z, "ls /system", NO_CARD_PROMPT)
        for name in ("shell.exe", "monitor.exe", "drivers.man"):
            self.assertIn(name, listing)
        self.assertIn("built-ins: cd pwd",
                      self.type_line(z, "help", NO_CARD_PROMPT))


class TestPrograms(Mk2ShellCase):
    def test_a_store_program_returns_its_status(self):
        """fs_child from the store: its arguments, a file it creates on
        the card through the IOC-FAT driver, and its exit status."""
        z = self.start(card=True)
        fresh = self.type_line(z, "fs_child tail-42", CARD_PROMPT)
        self.assertIn('fs-child: tail "tail-42" argc 1', fresh)
        self.assertIn("fs-child: PASS", fresh)
        self.assertIn("[exit 42]", fresh)

    def test_exit_reaches_the_launcher_and_the_shell_starts_again(self):
        z = self.start()
        fresh = self.type_line(z, "exit 42", NO_CARD_PROMPT)
        self.assertIn("boot: /system/shell.exe exit 42", fresh)
        self.assertTrue(fresh.endswith(NO_CARD_PROMPT), fresh[-40:])

    def test_a_card_program_streams_through_the_driver(self):
        """A copy of the shell on the card runs as the child: its image is
        read through IOC-FAT FILE calls, not from the store's pages."""
        z = self.start(card=True)
        self.type_line(z, "cp /system/shell.exe /fs/S.EXE", CARD_PROMPT)
        fresh = self.type_line(z, "/S.EXE", CARD_PROMPT)
        self.assertIn("KonaOS v1.0", fresh)
        fresh = self.type_line(z, "exit 3", CARD_PROMPT)
        self.assertIn("[exit 3]", fresh)

    def test_the_child_inherits_the_cwd(self):
        z = self.start(card=True)
        self.type_line(z, "cd /system", NO_CARD_PROMPT)
        z.hid.hid_queue.extend(b"fs_child tail-42\r")
        launch = self.syms["_kos_process_launch"]
        self.assertIsNotNone(self.drive(z, STEP_LIMIT, watch={launch},
                                        hook=lambda _step: True))
        pid = z.phys[self.nucleus(self.syms["_process_running_pid"])]
        # The CWD table is cold set 0's data: map the set for the read.
        pages = STOP + 0x10
        z.phys[self.nucleus(pages)] = SET0_PAGES[0]
        z.phys[self.nucleus(pages) + 1] = SET0_PAGES[1]
        self.call_fixed(z, "_kos_dat_map", hl=pages, de=0x0203)
        cwd = self.call_fixed(z, "_vfs_cwd_for", a=pid)
        at = SET0_PAGES[(cwd >> 13) - 3] * PAGE | (cwd & 0x1FFF)
        self.assertEqual(bytes(z.phys[at:at + 8]), b"/system\0")

    def test_page_counts_return_after_ten_runs(self):
        z = self.start()
        self.idle(z)
        before = self.free_pages(z)
        for _ in range(10):
            self.assertIn("[exit 1]",
                          self.type_line(z, "fs_child", NO_CARD_PROMPT))
        self.idle(z)
        self.assertEqual(self.free_pages(z), before)


class TestReboot(Mk2ShellCase):
    def test_reboot_returns_to_the_rom_view_and_boots_again(self):
        z = self.start()
        z.hid.hid_queue.extend(b"reboot\r")
        seen = {}

        def reset(_step):
            seen["map_enable"] = z.mapper.map_enable
            seen["resets"] = z.hid.host_resets
            return True

        self.assertIsNotNone(self.drive(z, STEP_LIMIT, watch={0x0000},
                                        hook=reset))
        self.assertEqual(seen, {"map_enable": 0, "resets": 1})
        since = len(z.glyphs)
        self.run_to(z, NO_CARD_PROMPT, since=since)
        self.assertEqual(bytes(z.sio_tx), BOOT_SIO + BOOT_SIO)
        self.assertIn("console: driver OK", self.trace(z, since))


class Mk1ShellReplay(ShellReplay):
    """The Mk1 production image on the same harness (TestIpcCost)."""

    def start(self):
        z = self.make_cpu()
        z.hid = z.ioc_peer = IocCommandPeer(fs2_media=True)
        self.start_kernel(z)
        self.run_to(z, CARD_PROMPT)
        return z


class TestIpcCost(unittest.TestCase):
    """One null application call (KJOY_OP_READ: the joystick driver reads
    one latch, no descriptor) and one 512-byte read the IOC-FAT driver
    copies to the caller (a file block it has cached), from the shell's
    page 0 with interrupts masked, on both production images.  The
    T-states are informational (work records); the test checks that both
    boards return the file's bytes."""

    def measure(self, replay):
        z = replay.start() if isinstance(replay, Mk1ShellReplay) else \
            replay.start(card=True)
        replay.type_line(z, "cp /system/shell.exe /fs/S.EXE", CARD_PROMPT)
        replay.idle(z)
        joy = replay.bind(z, "/dev/input/joy/0")
        status, null_call = replay.call(z, joy, KJOY_OP_READ)
        self.assertEqual(status, 0)
        handle = replay.bind(z, "/fs/S.EXE", KFS_O_READ)
        buffer = 0x1A00
        reads = []
        for _ in range(2):
            replay.call(z, handle, KFILE_OP_SEEK, payload=bytes(5))
            status, tstates = replay.call(z, handle, KFILE_OP_READ, 512,
                                          buffer, 512, DESC_OUT)
            self.assertEqual(status, 512)
            reads.append(tstates)
        with open(os.path.join(ROOT, "build", "modules", "shell.exe"),
                  "rb") as source:
            want = source.read(512)
        self.assertEqual(bytes(z.rd(buffer + i) for i in range(512)), want)
        return null_call, reads[1]

    def test_both_boards_on_one_harness(self):
        mk1 = os.path.join(ROOT, "build", "konaos.bin")
        mk2 = os.path.join(ROOT, MK2_DIR, MK2_IMAGE)
        if not (os.path.isfile(mk1) and os.path.isfile(mk2)):
            self.skipTest("make and make BOARD=mk2 build the images")
        for replay in (Mk1ShellReplay.open("build", "konaos.bin"),
                       Mk2ShellReplay.open(MK2_DIR, MK2_IMAGE, MK2_MAP)):
            null_call, read = self.measure(replay)
            self.assertGreater(null_call, 0)
            self.assertGreater(read, null_call)


if __name__ == "__main__":
    unittest.main()
