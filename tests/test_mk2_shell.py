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
counts after a program ran ten times.  TestCacheDelivery pins an IOC-FAT
cache hit's single copy from the cache to the caller and the coherence
around it; TestDriverCopyStack a render the V9958 driver copies from its
private stack, inside the copy windows; TestLoaderCacheHits the EXE loader's cache hits on both
boards.  TestIpcCost runs one null call and
one 512-byte driver read on the Mk1 and Mk2 production images on the same
harness (the T-states are informational; the test checks the results).

Skipped when the image has not been built.
"""

import collections
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from replay import (STEP_LIMIT, ConsoleReplay, Mk2Board,  # noqa: E402
                    v9958_screen)
from z80sim import IocCommandPeer, StopSet  # noqa: E402

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


class Mk2ShellReplay(Mk2Board, ShellReplay):
    """The Mk2 production image: booted from reset (the ROM bootstrap
    runs), so the serial-connect delay runs too."""

    STORE = os.path.join(MK2_DIR, "sys", "modules")  # its programs: Mk2

    def make_cpu(self, card=False, files=None):
        return super().make_cpu(card, files)

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
        # The CWD table is cold set 0's data: map the set for the read (a
        # save area naming entries 3-4).
        save = STOP + 0x10
        at = self.nucleus(save)
        z.phys[at:at + 4] = bytes((3, 2) + SET0_PAGES)
        self.call_fixed(z, "_kos_dat_span_restore", hl=save)
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


class TestIrqTrampoline(Mk2ShellCase):
    """A driver's interrupt taken inside a span write (a driver call's
    kos_dat_span_enter, or the copy windows' restore during the call),
    between the mirror write of a domain entry and its OUT (the NMI
    rule's order): the mirror already names the domain page while the
    hardware entry still shows the previous one.  The IRQ trampoline must run the handler on
    the domain all the same, and the interrupted call must finish with
    the domain installed."""

    CTC2_VECTOR = 0x04          # the timer driver's channel (CTC base 00h)
    ENTRY = 6                   # the domain's last entry (3-6)

    def test_an_irq_between_mirror_and_out_runs_on_the_domain(self):
        z = self.start()
        self.idle(z)
        # The last of the unrolled entry writes that follow
        # kos_dat_span_restore (the entry and its write are shared).
        mapper = self.syms["_kos_dat_span_restore"]
        code = bytes(z.phys[self.nucleus(mapper):self.nucleus(mapper) + 128])
        out = mapper + code.rindex(bytes([0xED, 0x79]))  # OUT (C),A
        at = self.nucleus(self.syms["_kos_driver_domain"])
        domain = bytes(z.phys[at:at + 8])
        mirror = 0xEF10 - 0xE000 + 0x7F * PAGE     # KOS_DAT_SYSTEM
        seen = {}

        def lagging(_step):
            if "taken" in seen:
                # The handler's first instruction (the executive and the
                # trampoline run in the fixed region, below 6000h): the
                # domain in force.
                if 0x5F00 <= z.sp < 0x6000 and z.pc >= 0x6000:
                    seen["entries"] = tuple(z.mapper.entries[3:7])
                    return True
                return None
            if (z.c == self.ENTRY and z.a == domain[self.ENTRY] and
                    z.phys[mirror + self.ENTRY] == domain[self.ENTRY] and
                    z.mapper.entries[self.ENTRY] != domain[self.ENTRY] and
                    z.iff1):
                seen["taken"] = z.mapper.entries[self.ENTRY]
                z.assert_int(self.CTC2_VECTOR)
                return set(range(0x6000, 0x10000))
            return None

        since = len(z.glyphs)
        z.hid.hid_queue.extend(b"ls /system\r")
        self.assertIsNotNone(self.drive(z, STEP_LIMIT, watch={out},
                                        hook=lagging),
                             "no driver call found the entry lagging")
        self.assertEqual(seen["entries"], tuple(domain[3:7]))
        # The interrupted call finished and the shell goes on.
        self.run_to_pattern(z, r"\n/system> $", since=since)
        self.idle(z)
        self.assertIn("\n/system\n", self.type_line(z, "pwd", NO_CARD_PROMPT))


class Mk2CallHarness(Mk2ShellCase):
    """Synchronous calls from the shell's page 0 (interrupts masked unless
    a test enables them), with the driver's side injected: at a driver's
    dispatch entry the test calls a driver_api entry exactly as the
    driver would (its registers and stack arguments), then resumes the
    dispatch with every register as it was."""

    CODE, BLOCK, FRAME, BUFFER = 0x1C00, 0x1D00, 0x1E00, 0x1E80
    SOUND_WRITE = 1             # KSOUND write: register/value pairs in
    EINVAL, EBUSY = -2, -4
    CARD = False

    def prepare(self, z):
        """Shell commands before the first injected syscall (which
        overwrites shell code)."""

    def setUp(self):
        super().setUp()
        self.z = z = self.start(card=self.CARD)
        self.prepare(z)
        self.idle(z)
        self.joy = self.bind(z, "/dev/input/joy/0")
        self.sound = self.bind(z, "/dev/sound")
        self.entries = {}       # dispatch entry -> its endpoint number
        self.actions = []       # one per dispatch entry, in order
        self.dat_writes = 0
        self.moves = collections.Counter()
        self.watch = None
        self.before = self.mapping()

    def mapping(self):
        z = self.z
        mirror = bytes(z.rd(0xEF10 + i) for i in range(16))  # KOS_DAT_*
        return tuple(z.mapper.entries), z.mapper.dat_bank, mirror

    def assert_exact(self):
        entries, bank, mirror = self.mapping()
        self.assertEqual((entries, bank, mirror), self.before)
        self.assertEqual(bytes(entries), mirror)

    def step(self, until):
        """Run to `until` (a predicate), firing the injected actions at
        dispatch entries, counting DAT entry writes and the bytes block
        moves carry from one physical page to another (`moves`), and
        calling `watch` before every instruction."""
        z = self.z
        invoke = {self.syms["_ipc_invoke"], self.syms["_ipc_invoke_here"]}
        pending = None
        for _ in range(STEP_LIMIT):
            if until(z):
                return
            if z.pc in invoke and z.mapper.physical(z.pc) == \
                    self.nucleus(z.pc):
                pending = z.hl
            elif pending is not None and z.pc == pending:
                pending = None
                if self.actions:
                    self.actions.pop(0)(z)
                    continue
            if self.watch is not None:
                self.watch(z)
            op, ext = z.rd(z.pc), z.rd(z.pc + 1)
            if ((op == 0xED and ext & 0xC7 == 0x41 and z.c <= 15) or
                    (op == 0xD3 and ext <= 15)):
                self.dat_writes += 1
            if op == 0xED and ext in (0xA0, 0xA8, 0xB0, 0xB8):
                pages = (z.mapper.physical(z.hl) >> 13,
                         z.mapper.physical(z.de) >> 13)
                bc = z.bc
                z.step()
                self.moves[pages] += (bc - z.bc) & 0xFFFF
                continue
            z.step()
        self.fail("no return (PC=%04X)" % z.pc)

    def raw_call(self, handle, op, buffer=0, length=0,
                 direction=DESC_NONE, payload=b"", enabled=False):
        """One SYS_CALL; returns (status, carry)."""
        z = self.z
        frame = (bytes([op, 0, DESC_NONE, 0]) + struct.pack("<H", length) +
                 payload.ljust(24, b"\0") + b"\0\0")
        self.put(z, self.BLOCK, bytes([handle, op, 0, 32]) +
                 struct.pack("<HHBB", buffer, length, direction, 0) +
                 frame + b"\0\0")
        self.put(z, self.CODE, b"\xF7\0")
        z.halted = False
        z.iff1 = z.iff2 = enabled
        z.pc, z.a, z.hl = self.CODE, SYS_CALL, self.BLOCK
        self.step(lambda m: m.pc == self.CODE + 1)
        status = z.hl - 0x10000 if z.hl & 0x8000 else z.hl
        return status, z.f & 1

    def inject(self, name, a=0, l=0, de=0, hl=None, stack=()):
        """Call a driver_api entry from the dispatch entry the machine is
        stopped at (`stack`: words, or the exact bytes); returns its DE
        with every register put back."""
        z = self.z
        names = ("a", "f", "b", "c", "d", "e", "h", "l", "ix", "iy", "sp")
        saved = {n: getattr(z, n) for n in names}
        resume = z.pc
        if isinstance(stack, bytes):
            z.sp = (z.sp - len(stack)) & 0xFFFF
            self.put(z, z.sp, stack)
        else:
            for word in reversed(stack):
                z.push16(word)
        z.push16(resume)
        z.a, z.l, z.de = a, l, de
        if hl is not None:
            z.hl = hl
        z.pc = self.syms[name]
        self.step(lambda m: m.pc == resume and m.sp == saved["sp"])
        result = z.de
        for n, value in saved.items():
            setattr(z, n, value)
        return result - 0x10000 if result & 0x8000 else result

    def endpoint(self, handle):
        """The endpoint number a bound handle resolves to."""
        z = self.z
        seen = []
        at = self.syms["_ipc_syscall_call_object"]

        def record(_z):
            seen.append(z.rd(z.sp + 2))     # the stacked endpoint byte
            return None

        self.put(z, self.BLOCK, bytes([handle, KJOY_OP_READ, 0, 32]) +
                 struct.pack("<HHBB", 0, 0, DESC_NONE, 0) + bytes(34))
        self.put(z, self.CODE, b"\xF7\0")
        z.halted = False
        z.iff1 = z.iff2 = False
        z.pc, z.a, z.hl = self.CODE, SYS_CALL, self.BLOCK
        self.step(lambda m: (m.pc == at and record(m)) or
                  m.pc == self.CODE + 1)
        return seen[0]

    def sound_payload(self):
        self.put(self.z, self.BUFFER, bytes([4, 0x7F]) * 16)


class TestIpcCalls(Mk2CallHarness):
    """Every call leaves the caller's exact mapping (hardware and mirrors)
    and no descriptor a later call could name; an outer descriptor
    reaches a nested callee, a nested call into the mapped domain remaps
    nothing, and a span a driver leaks is closed at the endpoint return
    (architecture 2.7.6, 4.9)."""

    def test_descriptor_null_and_failed_calls_leave_no_state(self):
        z = self.z
        self.sound_payload()
        self.assertEqual(self.raw_call(self.sound, self.SOUND_WRITE,
                                       self.BUFFER, 32, 0), (32, 0))
        self.assert_exact()
        # The descriptor of the call before (slot 0) is not the null
        # call's: the driver naming it is refused.
        stale = []
        self.actions = [lambda _z: stale.append(self.inject(
            "_api_copy_in", a=0, de=0, stack=(0x6000, 1)))]
        self.assertEqual(self.raw_call(self.joy, KJOY_OP_READ), (0, 0))
        self.assertEqual(stale, [self.EINVAL])
        self.assert_exact()
        # A handle nobody bound: refused by the syscall, carry set.
        _status, carry = self.raw_call(0x7E, KJOY_OP_READ)
        self.assertEqual((carry, z.a), (1, -self.EINVAL))
        self.assert_exact()
        # Direction, length and window refusals: the IPC status.
        for buffer, length, direction in ((self.BUFFER, 32, 2),
                                          (self.BUFFER, 4097, 0),
                                          (0x0080, 32, 0),
                                          (0xFFF0, 32, 0)):
            self.assertEqual(self.raw_call(self.sound, self.SOUND_WRITE,
                                           buffer, length, direction),
                             (self.EINVAL, 0), (buffer, length, direction))
            self.assert_exact()
        self.assertEqual(self.raw_call(self.joy, KJOY_OP_READ), (0, 0))
        self.assert_exact()

    def test_an_outer_descriptor_reaches_a_nested_callee(self):
        """The joystick call carries an OUT descriptor; from its dispatch
        the test calls the sound endpoint (a nested, same-domain call:
        no DAT write), whose dispatch copies domain bytes out through the
        outer descriptor.  The joystick endpoint itself is busy (a
        re-entry is refused)."""
        z = self.z
        sound = self.endpoint(self.sound)
        joy = self.endpoint(self.joy)
        self.before = self.mapping()
        self.put(z, self.FRAME, bytes(32))
        self.put(z, self.BUFFER, bytes(8))
        results = {}

        def outer(_z):
            results["reentry"] = self.inject("_api_call", a=joy, l=0,
                                             stack=(self.FRAME,))
            results["mark"] = self.dat_writes
            self.inject("_api_call", a=sound, l=0x7F, stack=(self.FRAME,))

        def nested(_z):
            # Up to the nested dispatch: the domain is already mapped.
            results["nested_writes"] = self.dat_writes - results["mark"]
            results["domain"] = bytes(z.rd(0x6000 + i) for i in range(4))
            results["copied"] = self.inject("_api_copy_out", a=0, de=2,
                                            stack=(0x6000, 4))

        self.actions = [outer, nested]
        self.assertEqual(self.raw_call(self.joy, KJOY_OP_READ, self.BUFFER,
                                       8, DESC_OUT), (0, 0))
        self.assertEqual(results["reentry"], self.EBUSY)
        self.assertEqual(results["copied"], 4)
        self.assertEqual(results["nested_writes"], 0)
        self.assertEqual(bytes(z.rd(self.BUFFER + i) for i in range(8)),
                         bytes(2) + results["domain"] + bytes(2))
        self.assert_exact()

    def test_a_leaked_bulk_span_is_closed_at_the_endpoint_return(self):
        z = self.z
        faults = self.nucleus(self.syms["_ipc_bulk_faults"])
        before = z.phys[faults] | z.phys[faults + 1] << 8
        domain = self.nucleus(self.syms["_kos_driver_domain"])
        seen = {}

        def leak(_z):
            seen["map"] = self.inject("_api_bulk_map")
            seen["entry0"] = z.mapper.entries[0]
            seen["again"] = self.inject("_api_bulk_map")

        self.actions = [leak]
        self.assertEqual(self.raw_call(self.joy, KJOY_OP_READ), (0, 0))
        self.assertEqual(seen, {"map": 0, "entry0": z.phys[domain],
                                "again": self.EBUSY})
        self.assertEqual(z.phys[faults] | z.phys[faults + 1] << 8,
                         before + 1)
        self.assert_exact()
        self.assertEqual(self.raw_call(self.joy, KJOY_OP_READ), (0, 0))
        self.assertEqual(z.phys[faults] | z.phys[faults + 1] << 8,
                         before + 1)

    def test_an_nmi_inside_a_span_write(self):
        """An NMI taken between a domain entry's mirror write and its OUT
        (the routed vector is idle): the call ends in the caller's exact
        mapping."""
        z = self.z
        restore = self.syms["_kos_dat_span_restore"]
        code = bytes(z.phys[self.nucleus(restore):
                            self.nucleus(restore) + 128])
        out = restore + code.rindex(bytes([0xED, 0x79]))     # OUT (C),A
        taken = []
        count = z.nmi_count

        def lagging(m):
            if (not taken and m.pc == out and
                    m.mapper.physical(m.pc) == self.nucleus(out) and
                    m.mapper.entries[m.c] != m.a):
                taken.append(m.c)
                m.nmi_pending = True
            return False

        self.put(z, self.CODE, b"\xF7\0")
        self.put(z, self.BLOCK, bytes([self.joy, KJOY_OP_READ, 0, 32]) +
                 struct.pack("<HHBB", 0, 0, DESC_NONE, 0) + bytes(34))
        z.halted = False
        z.iff1 = z.iff2 = False
        z.pc, z.a, z.hl = self.CODE, SYS_CALL, self.BLOCK
        self.step(lambda m: lagging(m) or m.pc == self.CODE + 1)
        self.assertEqual(taken, [6])
        self.assertEqual(z.nmi_count, count + 1)
        self.assertEqual((z.hl, z.f & 1), (0, 0))
        self.assert_exact()


class TestDriverCopyStack(Mk2CallHarness):
    """A console render: the V9958 driver runs the transaction on its
    private stack in its io block, which lies in the copy windows, and
    copies each chunk of the stream from there.  The stream lies in the
    caller's C000h page, so each copy remaps the windows away from that
    stack: the copy runs on the nucleus's driver copy stack, and no DAT
    write ever remaps the system entry that holds SP."""

    BUFFER = 0xD34A             # a TUI render buffer's place (BSS)
    RENDER, STDOUT, DESC_IN = 6, 1, 0  # KCONSOLE_OP_RENDER, the console
    ROW = 5

    def stream(self, text):
        record = bytes([1, 6 + len(text), 0, self.ROW, 0, len(text)]) + text
        return bytes([1, 0]) + struct.pack("<H", 4 + len(record)) + record

    def test_a_render_from_a_private_stack(self):
        z = self.z
        text = b"rendered from the C000h page, copied from the driver"
        stream = self.stream(text)
        self.put(z, self.BUFFER, stream)
        entry = self.syms["_kos_driver_copy"]
        low = self.syms["kos_driver_copy_stack"]
        top = self.syms["kos_driver_copy_stack_top"]
        page_copy = self.syms["_kos_page_copy"]
        under_sp, callers, copy_sps = [], [], []

        def watch(m):
            if m.pc in (entry, page_copy) and \
                    m.mapper.physical(m.pc) == self.nucleus(m.pc):
                (callers if m.pc == entry else copy_sps).append(m.sp)
            op, ext = m.rd(m.pc), m.rd(m.pc + 1)
            written = None
            if op == 0xD3 and ext <= 7:
                written = ext                   # OUT (n),A: entry n
            elif op == 0xED and ext & 0xC7 == 0x41 and m.c <= 7 and \
                    m.b < 0x80:
                written = m.c                   # OUT (C),r: entry C
            if written is not None and m.mapper.dat_bank == 0 and \
                    m.sp >> 13 == written:
                under_sp.append("PC=%04X SP=%04X entry %d"
                                % (m.pc, m.sp, written))

        self.watch = watch
        status, carry = self.raw_call(self.STDOUT, self.RENDER, self.BUFFER,
                                      len(stream), self.DESC_IN)
        self.watch = None
        self.assertEqual(carry, 0)
        self.assertGreaterEqual(status, 0)
        self.assertEqual(under_sp, [])
        self.assertTrue(any(0xA000 <= sp < 0xE000 for sp in callers),
                        "no copy from a stack in the windows: %s"
                        % ["%04X" % sp for sp in callers])
        self.assertTrue(copy_sps)
        for sp in copy_sps:
            self.assertTrue(low <= sp < top, "%04X" % sp)
        self.assert_exact()
        row = v9958_screen(z.vdp.vram, z.vdp.regs[23])[self.ROW]
        self.assertEqual("".join(cell[0] for cell in row[:len(text)]),
                         text.decode("ascii"))


class FileCallHarness(Mk2CallHarness):
    """File reads on the card from the shell's page 0 (Mk2CallHarness),
    each followed by the caller's mapping check."""

    CARD = True
    SYS_CLOSE = 0x08
    FS2_READ = 0x36             # the card read command the peer counts

    def open_ro(self, path):
        return self.bind(self.z, path, KFS_O_READ)

    def close(self, handle):
        self.put(self.z, self.BLOCK, bytes([handle]))
        _status, carry, _ = self.syscall(self.z, self.SYS_CLOSE, self.BLOCK)
        self.assertEqual(carry, 0)

    def seek(self, handle, offset):
        self.assertEqual(self.raw_call(
            handle, KFILE_OP_SEEK,
            payload=b"\0" + offset.to_bytes(4, "little")), (0, 0))

    def card_reads(self):
        return self.z.hid.fs2_counts[self.FS2_READ]

    def read(self, handle, offset, buffer, length):
        """Seek and read; returns (status, card reads, block moves of 16
        bytes or more), the caller's mapping checked exact."""
        self.seek(handle, offset)
        reads = self.card_reads()
        self.moves.clear()
        status, carry = self.raw_call(handle, KFILE_OP_READ, buffer,
                                      length, DESC_OUT)
        self.assertEqual(carry, 0)
        self.assert_exact()
        moves = {k: v for k, v in self.moves.items() if v >= 16}
        return status, self.card_reads() - reads, moves

    def caller_pages(self, buffer, length):
        image = self.z.mapper.entries[8:16]     # the working entries
        return {image[a >> 13] for a in (buffer, buffer + length - 1)}

    def bytes_at(self, buffer, length):
        return bytes(self.z.rd(buffer + i) for i in range(length))


class TestCacheDelivery(FileCallHarness):
    """A file block the IOC-FAT cache holds reaches the caller in one
    nucleus copy from the cache's page to the caller's (bulk_copy_out),
    wherever the caller buffer lies; the ring and the cache stay
    coherent around it.  M.EXE (13 blocks, the last one 178 bytes) fits
    the cache; S.EXE does not, so a read of it refills the ring with an
    uncached block, which leaves M's blocks to the cache alone."""

    KFILE_OP_WRITE, KFILE_OP_TRUNCATE = 1, 6
    BULK = 0x1C00               # the cache's bulk base, slot 0
    PATH_RECORDS = 32 * 512     # IOCFAT_CACHE_PATHS_AT
    EXTENT = 32 * 512 + 8 * 128
    WANT = None

    def prepare(self, z):
        self.type_line(z, "cp /system/monitor.exe /fs/M.EXE", CARD_PROMPT)
        self.type_line(z, "cp /system/shell.exe /fs/S.EXE", CARD_PROMPT)

    def setUp(self):
        super().setUp()
        with open(os.path.join(ROOT, self.STORE, "monitor.exe"),
                  "rb") as source:
            self.want = source.read()
        self.assertEqual(len(self.want), 12 * 512 + 178)
        domain = self.nucleus(self.syms["_kos_driver_domain"])
        self.domain = bytes(self.z.phys[domain:domain + 8])
        # The extent 1C00h-5FFFh: domain pages 0-2.
        self.cache_pages = set(self.domain[0:3])
        self.m = self.open_ro("/fs/M.EXE")
        self.s = self.open_ro("/fs/S.EXE")
        self.populate()

    def populate(self):
        """Every block of M through the cache, then the ring on S."""
        self.assertEqual(self.read(self.m, 0, 0x2000, 4096)[0], 4096)
        self.assertEqual(self.read(self.m, 4096, 0x2000, 2226)[0], 2226)
        self.assertEqual(self.read(self.s, 0, 0x2000, 512)[0], 512)

    def test_a_cache_hit_moves_each_payload_byte_once(self):
        """Page 0, across the page 0-1 edge and high across the 5-6 edge:
        512 bytes, one pass from the cache's page to the caller's, no card
        command, the bytes around the buffer untouched; the cached copy
        serves every repeat."""
        for buffer in (0x1000, 0x1F00, 0xBF00):
            self.put(self.z, buffer - 16, b"\xA5" * 16)
            self.put(self.z, buffer + 512, b"\x5A" * 16)
            status, reads, moves = self.read(self.m, 0, buffer, 512)
            self.assertEqual((status, reads), (512, 0), hex(buffer))
            self.assertEqual(self.bytes_at(buffer, 512), self.want[:512])
            self.assertEqual(self.bytes_at(buffer - 16, 16), b"\xA5" * 16)
            self.assertEqual(self.bytes_at(buffer + 512, 16), b"\x5A" * 16)
            self.assertEqual(sum(moves.values()), 512, moves)
            self.assertTrue({src for src, _ in moves} <= self.cache_pages)
            self.assertEqual({dst for _, dst in moves},
                             self.caller_pages(buffer, 512))

    def test_partial_blocks_seeks_and_the_end_of_file(self):
        # Inside block 1 to inside block 3: three slot copies.
        status, reads, moves = self.read(self.m, 700, 0x1000, 1000)
        self.assertEqual((status, reads), (1000, 0))
        self.assertEqual(self.bytes_at(0x1000, 1000), self.want[700:1700])
        self.assertEqual(sum(moves.values()), 1000)
        # Across the short last block to the end of the file.
        status, reads, moves = self.read(self.m, 6100, 0x1000, 512)
        self.assertEqual((status, reads), (222, 0))
        self.assertEqual(self.bytes_at(0x1000, 222), self.want[6100:])
        self.assertEqual(sum(moves.values()), 222)
        # At the end, and past it: nothing.
        self.assertEqual(self.read(self.m, 6322, 0x1000, 16)[:2], (0, 0))
        self.assertEqual(self.read(self.m, 7000, 0x1000, 16)[:2], (0, 0))
        # The last byte alone.
        status, reads, moves = self.read(self.m, 6321, 0x1000, 1)
        self.assertEqual((status, reads), (1, 0))
        self.assertEqual(self.bytes_at(0x1000, 1), self.want[6321:])

    def test_a_mixed_read_fills_only_what_the_cache_lacks(self):
        """With blocks 1-2 cached and block 0 not: block 0 comes from the
        card through the ring (and into the cache), 1-2 straight from
        their slots."""
        for handle in (self.m, self.s):
            self.close(handle)
        self.close(self.open_writer("/fs/M.EXE"))    # drops the cache
        self.m = self.open_ro("/fs/M.EXE")
        # The cache was dropped, so no read-ahead: just blocks 1-2.
        status, reads, _ = self.read(self.m, 512, 0x1000, 1024)
        self.assertEqual((status, reads), (1024, 2))
        ring = self.domain[4], self.domain[5]
        status, reads, moves = self.read(self.m, 0, 0x1000, 1536)
        self.assertEqual((status, reads), (1536, 1))
        self.assertEqual(self.bytes_at(0x1000, 1536), self.want[:1536])
        page0 = self.caller_pages(0x1000, 1536)
        from_ring = sum(v for (src, dst), v in moves.items()
                        if src in ring and dst in page0)
        from_cache = sum(v for (src, dst), v in moves.items()
                         if src in self.cache_pages and dst in page0)
        to_cache = sum(v for (src, dst), v in moves.items()
                       if src in ring and dst in self.cache_pages)
        self.assertEqual((from_ring, from_cache, to_cache),
                         (512, 1024, 512), moves)
        # Now every block is cached: the whole range from the cache.
        status, reads, moves = self.read(self.m, 0, 0x1000, 512)
        self.assertEqual((status, reads), (512, 0))

    def open_writer(self, path):
        return self.bind(self.z, path, KFS_O_READ | 0x02)  # KFS_O_WRITE

    def test_writes_and_truncation_reach_later_hits(self):
        """A write through another handle, then a truncation: the cache
        never serves the bytes they replaced."""
        self.close(self.m)
        writer = self.open_writer("/fs/M.EXE")
        self.put(self.z, 0x1000, b"overwritten!")
        self.assertEqual(self.raw_call(writer, self.KFILE_OP_WRITE, 0x1000,
                                       12, 0), (12, 0))
        self.close(writer)
        want = b"overwritten!" + self.want[12:]
        self.m = self.open_ro("/fs/M.EXE")
        self.assertEqual(self.read(self.m, 0, 0x1000, 4096)[0], 4096)
        self.assertEqual(self.read(self.m, 4096, 0x1000, 2226)[0], 2226)
        self.assertEqual(self.read(self.s, 0, 0x1000, 512)[0], 512)
        status, reads, moves = self.read(self.m, 0, 0x1000, 512)
        self.assertEqual((status, reads), (512, 0))
        self.assertEqual(self.bytes_at(0x1000, 512), want[:512])
        self.close(self.m)
        writer = self.open_writer("/fs/M.EXE")
        self.assertEqual(self.raw_call(
            writer, self.KFILE_OP_TRUNCATE,
            payload=b"\0" + (600).to_bytes(4, "little")), (0, 0))
        self.close(writer)
        self.m = self.open_ro("/fs/M.EXE")
        status, reads, _ = self.read(self.m, 512, 0x1000, 512)
        self.assertEqual(status, 88)
        self.assertEqual(self.bytes_at(0x1000, 88), want[512:600])

    def test_the_nucleus_refuses_what_the_driver_may_not_name(self):
        """bulk_copy_out from the IOC-FAT dispatch of a read with a
        16-byte OUT descriptor: the extent, the descriptor's range, its
        direction and liveness, a zero length and an open span are all
        refused; a good copy lands; the read itself then completes.  The
        joystick driver has no bulk extent at all."""
        z = self.z
        name = "_api_bulk_copy_out"
        results = {}

        def checks(_z):
            for label, desc, offset, bulk, length in (
                    ("good", 0, 4, 0, 8),
                    ("extent_end", 0, 0, self.EXTENT - 4, 8),
                    ("extent_wrap", 0, 0, 0xFFFC, 8),
                    ("desc_end", 0, 12, 0, 8),
                    ("desc_offset", 0, 17, 0, 1),
                    ("zero", 0, 0, 0, 0),
                    ("not_live", 1, 0, 0, 8),
                    ("forged", 9, 0, 0, 8)):
                results[label] = self.inject(name, a=desc, de=offset,
                                             stack=(bulk, length))
            self.assertEqual(self.inject("_api_bulk_map"), 0)
            results["span"] = self.inject(name, a=0, de=0,
                                          stack=(0, 8))
            self.inject("_ipc_bulk_unmap")

        self.put(z, 0x1000, b"\xEE" * 16)
        self.seek(self.m, 0)
        self.actions = [checks]
        status, carry = self.raw_call(self.m, KFILE_OP_READ, 0x1000, 16,
                                      DESC_OUT)
        self.assertEqual((status, carry), (16, 0))
        self.assert_exact()
        self.assertEqual(results, {
            "good": 8, "extent_end": self.EINVAL,
            "extent_wrap": self.EINVAL, "desc_end": self.EINVAL,
            "desc_offset": self.EINVAL, "zero": self.EINVAL,
            "not_live": self.EINVAL, "forged": self.EINVAL,
            "span": self.EBUSY})
        # The read wrote its 16 bytes over the injected copy.
        self.assertEqual(self.bytes_at(0x1000, 16), self.want[:16])
        # Wrong direction: an IN descriptor (the write path's).
        seen = []
        self.actions = [lambda _z: seen.append(self.inject(
            name, a=0, de=0, stack=(0, 8)))]
        self.assertEqual(self.raw_call(self.sound, self.SOUND_WRITE,
                                       self.BUFFER, 2, 0)[1], 0)
        # A stale descriptor (the previous call's slot 0), from a driver
        # without a bulk extent; then a live one there.
        self.actions = [lambda _z: seen.append(self.inject(
            name, a=0, de=0, stack=(0, 8)))]
        self.assertEqual(self.raw_call(self.joy, KJOY_OP_READ), (0, 0))
        self.actions = [lambda _z: seen.append(self.inject(
            name, a=0, de=0, stack=(0, 8)))]
        self.assertEqual(self.raw_call(self.joy, KJOY_OP_READ, 0x1000, 8,
                                       DESC_OUT), (0, 0))
        self.assertEqual(seen, [self.EINVAL] * 3)
        self.assert_exact()

    def test_interrupts_during_the_copy(self):
        """An NMI, then the timer's interrupt, taken in the middle of a
        cache hit's byte move (the copy windows hold the cache's and the
        caller's pages; the interrupt's handler maps the domain over
        them): the bytes, the status and the mapping come out exact.  The
        handler itself may not copy (-KOS_EBUSY)."""
        z = self.z
        state = {}

        def nmi(m):
            if ("nmi" not in state and m.rd(m.pc) == 0xED and
                    m.rd(m.pc + 1) == 0xA0 and
                    m.mapper.physical(m.hl) >> 13 in self.cache_pages and
                    m.bc == 300):
                state["nmi"] = m.nmi_count
                m.nmi_pending = True

        self.watch = nmi
        status, reads, moves = self.read(self.m, 0, 0x1000, 512)
        self.assertEqual((status, reads), (512, 0))
        self.assertEqual(z.nmi_count, state["nmi"] + 1)
        self.assertEqual(self.bytes_at(0x1000, 512), self.want[:512])
        self.assertEqual(sum(moves.values()), 512)

        handler = []

        def irq(m):
            if ("irq" not in state and m.rd(m.pc) == 0xED and
                    m.rd(m.pc + 1) == 0xA0 and
                    m.mapper.physical(m.hl) >> 13 in self.cache_pages and
                    m.bc == 300):
                state["irq"] = tuple(m.mapper.entries[5:7])
                m.assert_int(TestIrqTrampoline.CTC2_VECTOR)
            elif ("irq" in state and not handler and
                  0x5F00 <= m.sp < 0x6000 and
                  m.pc >= 0x6000):
                handler.append(tuple(m.mapper.entries[3:7]))
                handler.append(self.inject("_api_bulk_copy_out", a=0, de=0,
                                           stack=(0, 8)))

        self.watch = irq
        self.seek(self.m, 0)
        self.moves.clear()
        status, carry = self.raw_call(self.m, KFILE_OP_READ, 0x1A00, 512,
                                      DESC_OUT, enabled=True)
        self.assertEqual((status, carry), (512, 0))
        self.assertEqual(handler, [tuple(self.domain[3:7]), self.EBUSY])
        self.assertNotEqual(state["irq"], tuple(self.domain[5:7]))
        self.assertEqual(self.bytes_at(0x1A00, 512), self.want[:512])
        self.assertEqual(sum(v for v in self.moves.values() if v >= 16),
                         512)


class TestPage0DirectRead(FileCallHarness):
    """A read of an uncached file (S.EXE, 20,004 bytes: more than the
    block cache holds) into the caller's page 0 at a block boundary goes
    straight from the card into the caller's buffer: the nucleus lends
    it (borrow, through system entry 0), the IOC driver judges the READY
    before its first store and receives into it, and no payload byte
    moves in RAM.  Every other read goes through the ring as before."""

    EIO, ESTALE = -10, -21
    DESC_IN = 0
    LINK_SYNC = 0x0C
    LEN = 0x1F80                # where an injected borrow writes its length

    def prepare(self, z):
        self.type_line(z, "cp /system/shell.exe /fs/S.EXE", CARD_PROMPT)

    def setUp(self):
        super().setUp()
        with open(os.path.join(ROOT, self.STORE, "shell.exe"),
                  "rb") as source:
            self.want = source.read()
        self.assertEqual(len(self.want), 39 * 512 + 36)
        self.s = self.open_ro("/fs/S.EXE")
        self.stores = collections.Counter()
        self.watch = self.count_stores

    def count_stores(self, z):
        """Count the bulk receive's stores (INI) by physical page."""
        if z.rd(z.pc) == 0xED and z.rd(z.pc + 1) == 0xA2:
            self.stores[z.mapper.physical(z.hl) >> 13] += 1

    def page0(self):
        return self.z.mapper.entries[8]     # the caller's logical page 0

    def into_caller(self, moves):
        """Block-move bytes into the caller's pages (logical 0-1)."""
        caller = set(self.z.mapper.entries[8:10])
        return sum(v for (_src, dst), v in moves.items() if dst in caller)

    def assert_no_payload_moves(self, moves, commands):
        """No payload is copied in RAM: into the caller only the five bytes
        each packet kept below its buffer go back (ioc_io_bulk_read), and
        what else moves is the commands' reply frames (ioc_io_exchange,
        under 64 bytes each)."""
        self.assertLessEqual(self.into_caller(moves), 5 * commands, moves)
        self.assertLess(sum(moves.values()), 64 * max(1, commands), moves)

    def syncs(self):
        return sum(1 for request in self.z.hid.requests
                   if request[0] == self.LINK_SYNC)

    def guarded(self, buffer, length):
        """Fill 16 bytes either side (never below 0100h) with guards."""
        low = max(0x0100, buffer - 16)
        self.put(self.z, low, b"\xA5" * (buffer - low))
        self.put(self.z, buffer + length, b"\x5A" * 16)
        return low

    def assert_guards(self, low, buffer, length):
        self.assertEqual(self.bytes_at(low, buffer - low),
                         b"\xA5" * (buffer - low))
        self.assertEqual(self.bytes_at(buffer + length, 16), b"\x5A" * 16)

    def next_read(self, buffer, length):
        """A read with no seek: from where the last one left the file."""
        status, carry = self.raw_call(self.s, KFILE_OP_READ, buffer,
                                      length, DESC_OUT)
        self.assertEqual(carry, 0)
        return status

    def test_an_eligible_read_lands_in_place(self):
        """One block at 0200h and eight from the lowest lent address,
        0105h (the receive keeps each packet's header in the five caller
        bytes below it for the packet): one card read per block, every
        receive store in the caller's page 0, no RAM copy of 16 bytes or
        more, the guards intact, the position past the bytes."""
        for buffer, length in ((0x0200, 512), (0x0105, 4096)):
            low = self.guarded(buffer, length)
            self.stores.clear()
            status, reads, moves = self.read(self.s, 0, buffer, length)
            self.assertEqual((status, reads), (length, length // 512))
            self.assertEqual(self.bytes_at(buffer, length),
                             self.want[:length])
            self.assert_guards(low, buffer, length)
            self.assert_no_payload_moves(moves, reads)
            self.assertEqual(dict(self.stores),
                             {self.page0(): length + 5 * (length // 512)})
        self.assertEqual(self.next_read(0x0200, 512), 512)
        self.assertEqual(self.bytes_at(0x0200, 512), self.want[4096:4608])

    def test_other_reads_go_through_the_ring(self):
        """Page 1, an offset inside a block, less than a block, and a
        block the ring holds: the bytes come from the ring (one copy),
        never received into the caller."""
        ring = set(self.domain_pages()[4:7])
        for offset, buffer, length in ((0, 0x2000, 512), (100, 0x0200, 512),
                                       (1024, 0x0200, 300)):
            self.stores.clear()
            status, reads, moves = self.read(self.s, offset, buffer, length)
            self.assertEqual(status, length)
            self.assertEqual(self.bytes_at(buffer, length),
                             self.want[offset:offset + length])
            self.assertEqual(self.into_caller(moves), length, moves)
            self.assertNotIn(self.page0(), self.stores)
        # The ring holds blocks 0-3 now: block 1 is a ring hit.
        self.assertEqual(self.read(self.s, 0, 0x2000, 2048)[0], 2048)
        status, reads, moves = self.read(self.s, 512, 0x0200, 512)
        self.assertEqual((status, reads), (512, 0))
        self.assertEqual(self.bytes_at(0x0200, 512), self.want[512:1024])
        self.assertTrue({src for src, _ in moves} <= ring, moves)

    def domain_pages(self):
        domain = self.nucleus(self.syms["_kos_driver_domain"])
        return bytes(self.z.phys[domain:domain + 8])

    def test_the_end_of_the_file(self):
        """The short last block, a read across it and one past the end."""
        last = 39 * 512
        status, reads, moves = self.read(self.s, last, 0x0200, 512)
        self.assertEqual((status, reads), (36, 1))
        self.assert_no_payload_moves(moves, reads)
        self.assertEqual(self.bytes_at(0x0200, 36), self.want[last:])
        self.assertEqual(self.next_read(0x0200, 512), 0)
        # Block 38 in place; block 39, which the read above left in the
        # ring, from the ring.
        status, reads, moves = self.read(self.s, last - 512, 0x0200, 1024)
        self.assertEqual((status, reads), (548, 1))
        self.assertEqual(self.into_caller(moves), 36 + 5, moves)
        self.assertEqual(self.bytes_at(0x0200, 548), self.want[last - 512:])
        self.assertEqual(self.read(self.s, 40 * 512, 0x0200, 512)[0], 0)

    def test_a_page_edge_lends_what_the_page_holds(self):
        """At 1F00h the page holds 256 bytes: those are received in
        place, the rest of the block comes through the ring."""
        low = self.guarded(0x1F00, 512)
        status, reads, moves = self.read(self.s, 0, 0x1F00, 512)
        self.assertEqual((status, reads), (512, 2))
        self.assertEqual(self.bytes_at(0x1F00, 512), self.want[:512])
        self.assert_guards(low, 0x1F00, 512)
        self.assertEqual(self.into_caller(moves), 256, moves)

    def test_a_malformed_ready_is_refused_before_any_store(self):
        """A READY echoing another offset, and one admitting more than the
        lent length (at the page edge): -KOS_EIO with the caller's bytes,
        guards and position untouched and no receive store; the link
        recovers before the next command, which reads normally."""
        peer = self.z.hid
        for fault, buffer in (("offset", 0x0200), ("over", 0x1F00)):
            self.put(self.z, buffer - 16, b"\xC3" * 600)
            peer.fs2_read_fault = fault
            self.stores.clear()
            status, reads, _ = self.read(self.s, 0, buffer, 512)
            self.assertEqual((status, reads), (self.EIO, 1), fault)
            self.assertEqual(self.bytes_at(buffer - 16, 600),
                             b"\xC3" * 600, fault)
            self.assertEqual(sum(self.stores.values()), 0, fault)
            peer.fs2_read_fault = None
            syncs = self.syncs()
            self.assertEqual(self.next_read(0x0200, 512), 512)
            self.assertEqual(self.bytes_at(0x0200, 512), self.want[:512])
            self.assertEqual(self.syncs(), syncs + 1, fault)
        # A READY above a packet is never admitted at all.
        peer.fs2_read_fault = "ready_len"
        self.assertEqual(self.read(self.s, 0, 0x0200, 512)[0], self.EIO)
        peer.fs2_read_fault = None

    def test_a_packet_error_after_the_store(self):
        """A packet whose header fails after its payload landed, and one
        the controller stops sending halfway: -KOS_EIO, the bytes outside
        the requested range (the borrowed five below it included) and the
        position untouched; the requested range is unspecified."""
        peer = self.z.hid
        for fault in ("header_seq", "header_status", "stall"):
            low = self.guarded(0x0200, 512)
            peer.read_fault = fault
            status, reads, _ = self.read(self.s, 512, 0x0200, 512)
            self.assertEqual((status, reads), (self.EIO, 1), fault)
            self.assert_guards(low, 0x0200, 512)
            peer.read_fault = None
            self.assertEqual(self.next_read(0x0200, 512), 512, fault)
            self.assertEqual(self.bytes_at(0x0200, 512),
                             self.want[512:1024], fault)

    def test_stale_media(self):
        self.put(self.z, 0x0200, b"\xC3" * 512)
        self.z.hid.fs2_read_fault = "stale"
        self.assertEqual(self.read(self.s, 0, 0x0200, 512)[0], self.ESTALE)
        self.assertEqual(self.bytes_at(0x0200, 512), b"\xC3" * 512)
        self.z.hid.fs2_read_fault = None

    def test_interrupts_during_the_receive(self):
        """An NMI at the 300th store into page 0 (it is taken inside the
        masked packet window, through the caller page's own entry stub),
        then the timer's interrupt raised there with interrupts enabled
        (taken after the packet): bytes, status and mapping exact."""
        z = self.z
        state = {}

        def nmi(m):
            self.count_stores(m)
            if "nmi" not in state and self.stores[self.page0()] == 300:
                state["nmi"] = m.nmi_count
                m.nmi_pending = True

        self.watch = nmi
        status, reads, moves = self.read(self.s, 0, 0x0200, 512)
        self.assertEqual((status, reads), (512, 1))
        self.assert_no_payload_moves(moves, reads)
        self.assertEqual(z.nmi_count, state["nmi"] + 1)
        self.assertEqual(self.bytes_at(0x0200, 512), self.want[:512])

        self.stores.clear()

        def irq(m):
            self.count_stores(m)
            if "irq" not in state and self.stores[self.page0()] == 300:
                state["irq"] = True
                m.assert_int(TestIrqTrampoline.CTC2_VECTOR)

        self.watch = irq
        self.seek(self.s, 512)
        status, carry = self.raw_call(self.s, KFILE_OP_READ, 0x0200, 512,
                                      DESC_OUT, enabled=True)
        self.assertEqual((status, carry), (512, 0))
        self.assert_exact()
        self.assertEqual(self.bytes_at(0x0200, 512), self.want[512:1024])

    def borrow(self, desc=0, offset=0, length=512, direction=DESC_OUT):
        return self.inject("_api_borrow", a=desc, de=offset,
                           stack=struct.pack("<HBH", length, direction,
                                             self.LEN)) & 0xFFFF

    def borrowed(self, addr, length, direction=DESC_OUT):
        return self.inject("_api_borrowed", hl=addr, de=length,
                           stack=bytes([direction])) & 0xFFFF

    def lent(self):
        return self.z.rd(self.LEN) | (self.z.rd(self.LEN + 1) << 8)

    def borrow_during(self, buffer, length, direction=DESC_OUT,
                      lend=DESC_OUT):
        """borrow(0, 0, length, lend) from the IOC-FAT dispatch of a read
        (`direction` DESC_IN: a write) with that descriptor; the call then
        runs."""
        seen = []

        def take(_z):
            seen.append(self.borrow(length=length, direction=lend))
            self.inject("_api_borrow_release")

        self.seek(self.s, 0)
        self.actions = [take]
        _status, carry = self.raw_call(self.s, KFILE_OP_READ if
                                       direction == DESC_OUT else 1,
                                       buffer, length, direction)
        self.assertEqual(carry, 0)
        self.assert_exact()
        return seen[0]

    def test_the_borrow_rules(self):
        """What the nucleus lends, from the IOC-FAT dispatch of a 600-byte
        page-0 read: the descriptor's own range, capped at its end and the
        page's; one borrow at a time; `borrowed` answers only inside it
        and in its direction; bulk_map refuses while it is held; a span,
        a forged, stale or IN descriptor, an empty or out-of-range request
        and a page-0 mapping that is not the caller's are refused.  A
        borrow the dispatch leaves held ends at its return."""
        z = self.z
        r = {}

        def checks(_z):
            r["good"] = (self.borrow(), self.lent())
            r["second"] = self.borrow(length=16)
            r["inside"] = self.borrowed(0x0300, 16)
            r["past"] = self.borrowed(0x0300, 512)
            r["below"] = self.borrowed(0x01FF, 2)
            r["in_dir"] = self.borrowed(0x0300, 16, self.DESC_IN)
            r["span_held"] = self.inject("_api_bulk_map")
            self.inject("_api_borrow_release")
            r["released"] = self.borrowed(0x0200, 16)
            r["capped"] = (self.borrow(offset=100, length=4096), self.lent())
            self.inject("_api_borrow_release")
            r["zero"] = self.borrow(length=0)
            r["desc_end"] = self.borrow(offset=600, length=1)
            r["wrong_dir"] = self.borrow(direction=self.DESC_IN)
            r["forged"] = self.borrow(desc=9)
            r["not_live"] = self.borrow(desc=1)
            self.assertEqual(self.inject("_api_bulk_map"), 0)
            r["in_span"] = self.borrow()
            self.inject("_ipc_bulk_unmap")
            mirror = z.rd(0xEF10)                # system entry 0's mirror
            z.wr(0xEF10, mirror ^ 1)
            r["not_callers"] = self.borrow()
            z.wr(0xEF10, mirror)
            r["held"] = self.borrow(length=16)   # left for the return

        self.seek(self.s, 0)
        self.actions = [checks]
        self.stores.clear()
        status, carry = self.raw_call(self.s, KFILE_OP_READ, 0x0200, 600,
                                      DESC_OUT)
        self.assertEqual((status, carry), (600, 0))
        self.assert_exact()
        self.assertEqual(r, {
            "good": (0x0200, 512), "second": 0, "inside": 0x0300,
            "past": 0, "below": 0, "in_dir": 0, "span_held": self.EBUSY,
            "released": 0, "capped": (0x0264, 500), "zero": 0,
            "desc_end": 0, "wrong_dir": 0, "forged": 0, "not_live": 0,
            "in_span": 0, "not_callers": 0, "held": 0x0200})
        # The held borrow kept the read on the ring; it ended with the
        # dispatch, so the next read of a block the ring lacks is lent.
        self.assertNotIn(self.page0(), self.stores)
        self.assertEqual(self.bytes_at(0x0200, 600), self.want[:600])
        status, reads, moves = self.read(self.s, 8192, 0x0200, 512)
        self.assertEqual((status, reads), (512, 1))
        self.assert_no_payload_moves(moves, reads)
        # Other buffers: page 1, below 0105h, over the call block (BLOCK,
        # 44 bytes), its last byte under the five below the buffer, and
        # just clear of it; an IN descriptor (a write's), lent only for
        # reading.
        end = self.BLOCK + 44
        self.assertEqual([self.borrow_during(buffer, 16) for buffer in
                          (0x2000, 0x0104, self.BLOCK + 8, end + 4,
                           end + 5)],
                         [0, 0, 0, 0, end + 5])
        self.assertEqual(self.borrow_during(0x0200, 16, self.DESC_IN), 0)
        self.assertEqual(self.borrow_during(0x0200, 16, self.DESC_IN,
                                            self.DESC_IN), 0x0200)

    def test_the_borrow_survives_the_nested_call(self):
        """At the IOC driver's dispatch, inside IOC-FAT's lent read: the
        borrow is live there (`borrowed`), and bulk_map refuses."""
        seen = []

        def nested(_z):
            seen.append(self.borrowed(0x0200, 512))
            seen.append(self.inject("_api_bulk_map"))

        self.seek(self.s, 0)
        self.actions = [lambda _z: None, nested]
        self.assertEqual(self.next_read(0x0200, 512), 512)
        self.assertEqual(seen, [0x0200, self.EBUSY])
        self.assertEqual(self.bytes_at(0x0200, 512), self.want[:512])


class Mk1ShellReplay(ShellReplay):
    """The Mk1 production image on the same harness (TestIpcCost)."""

    STORE = os.path.join("build", "modules")

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
        with open(os.path.join(ROOT, replay.STORE, "shell.exe"),
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


class TestLoaderCacheHits(unittest.TestCase):
    """The EXE loader reads a card program into the nucleus's common
    staging block; the second run of a program the IOC-FAT cache holds is
    delivered from the cache straight into that block (bulk_copy_out):
    on Mk2 through the copy windows, on Mk1 in one aperture pass over
    bank 6, the staging block being the destination itself."""

    def run_counting(self, replay, z, line, expect):
        hits = []
        since = len(z.glyphs)
        z.hid.hid_queue.extend(line.encode("ascii") + b"\r")

        def tick(_step):
            replay.pump(z)
            return expect in "".join(z.glyphs[since:])

        self.assertIsNotNone(replay.drive(
            z, STEP_LIMIT, watch={replay.syms["_api_bulk_copy_out"]},
            hook=hits.append, tick=tick), line)
        return len(hits)

    def runs(self, replay, z):
        replay.type_line(z, "cp /system/monitor.exe /fs/M.EXE", CARD_PROMPT)
        counts = []
        for _ in range(2):
            counts.append(self.run_counting(replay, z, "/M.EXE", "Z80> "))
            replay.type_line(z, "Q", CARD_PROMPT)
        return counts

    def test_a_cached_program_loads_from_the_cache_on_both_boards(self):
        mk1 = os.path.join(ROOT, "build", "konaos.bin")
        mk2 = os.path.join(ROOT, MK2_DIR, MK2_IMAGE)
        if not (os.path.isfile(mk1) and os.path.isfile(mk2)):
            self.skipTest("make and make BOARD=mk2 build the images")
        replay = Mk2ShellReplay.open(MK2_DIR, MK2_IMAGE, MK2_MAP)
        first, second = self.runs(replay, replay.start(card=True))
        self.assertEqual(first, 0)
        self.assertGreaterEqual(second, 8)
        replay = Mk1ShellReplay.open("build", "konaos.bin")
        z = replay.start()
        replay.type_line(z, "cp /system/monitor.exe /fs/M.EXE", CARD_PROMPT)
        self.run_counting(replay, z, "/M.EXE", "Z80> ")
        replay.type_line(z, "Q", CARD_PROMPT)
        before = {name: replay.read16(z, name) for name in
                  ("_ipc_copy_aperture", "_ipc_copy_slow")}
        second = self.run_counting(replay, z, "/M.EXE", "Z80> ")
        after = {name: replay.read16(z, name) - before[name]
                 for name in before}
        self.assertGreaterEqual(second, 8)
        # Every one an aperture pass; none staged.
        self.assertGreaterEqual(after["_ipc_copy_aperture"], second)
        self.assertEqual(after["_ipc_copy_slow"], 0)


if __name__ == "__main__":
    unittest.main()
