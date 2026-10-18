#!/usr/bin/env python3
"""One replay helper for the Z80 image replays.

Every image replay repeats the same scaffolding: split the 512 KiB image
into its eight pages, build the Z80 with the ROM pages, call the C
runtime entry, start `_kernel_main`, skip the guarded serial-connect
delay, run to a marker, read linked state out of the bank the address
lives in, and capture the transcript.  This module owns all of that
once; a replay keeps only what is specific to its image (peers, step
limits, assertions).

`Replay` is the base machine.  `ConsoleReplay` adds the V9958 visible
lane: the positioned glyph model the monitor sessions are asserted
against.
"""

import json
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
if HERE not in sys.path:
    sys.path.insert(0, HERE)
if os.path.join(ROOT, "tools") not in sys.path:
    sys.path.insert(0, os.path.join(ROOT, "tools"))

import romfspack  # noqa: E402
from z80sim import (Ctc, Ctc0Injector, IocCommandPeer, V9958Peer,  # noqa: E402
                    Z80, read_symbols, skip_serial_connect_delay)

RET = 0x1234
STACK = 0xFFFE          # the common boot stack
COMMON_BASE = 0xE000    # the permanently mapped common region
LATCH_MODE11 = 0x18     # mode 11 (P = 0), OS configuration
CRT0_STEP_LIMIT = 2000000
STEP_LIMIT = 60000000
MARKER_SETTLE_STEPS = 64  # steps of quiet after a matched marker

# The monitor's visible lane, shared by every console replay.  The shell
# is the boot program of every production system, so its prompt is what a
# reboot lands on: the displayed CWD (the card's root reads as `/`) then
# `> `.
BANNER = "KonaOS 0.0"
PROMPT = "Z80>"
MONITOR_EXIT = "monitor: exit"
SHELL_PROMPT = "/> "


def _load_image(root, subdir, image_name, map_name):
    """Read one packed image and its symbol map; a missing image skips
    the test."""
    bin_path = os.path.join(root, subdir, image_name)
    if not os.path.exists(bin_path):
        raise unittest.SkipTest(
            "%s missing; build the image first" % bin_path)
    with open(bin_path, "rb") as handle:
        image = handle.read()
    return image, read_symbols(os.path.join(root, subdir, map_name))


class Replay:
    """An image and its symbol map, reusable across boots.

    Methods only: use it alone (`Replay.open(...)`) or as a mixin
    beside `unittest.TestCase`, where `IMAGE_SUBDIR`/`IMAGE_NAME`
    select the image and `setUp` calls `load_image`.
    """

    IMAGE_SUBDIR = None
    IMAGE_NAME = None
    root = ROOT
    # Optional per-instruction hook the stepping helpers call before
    # every step; None for images that need none.
    input_driver = None

    @classmethod
    def open(cls, subdir, image_name, map_name="konaos.map", root=ROOT):
        """Load `<root>/<subdir>/<image_name>` and its map; a missing
        image skips the test (acceptance images build in their own
        targets only)."""
        instance = cls.__new__(cls)
        instance.root = root
        instance.subdir = subdir
        instance.image, instance.syms = _load_image(
            root, subdir, image_name, map_name)
        return instance

    def load_image(self, subdir=None, image_name=None,
                   map_name="konaos.map"):
        """In-place `open` for a test base that declares IMAGE_SUBDIR
        and IMAGE_NAME.  Returns self."""
        self.subdir = subdir or self.IMAGE_SUBDIR
        self.image_name = image_name or self.IMAGE_NAME
        self.image, self.syms = _load_image(
            self.root, self.subdir, self.image_name, map_name)
        return self

    # -- machine ---------------------------------------------------------
    def make_cpu(self, latch=LATCH_MODE11, sp=STACK):
        """The machine at reset: the image's pages in the ROM map, the
        boot stack in common memory, and the V9958 peer every image that
        boots the video driver needs (its boot tier drives the display
        bring-up through the peer before any diagnostic runs)."""
        pages = {n: self.image[n * 0x10000:(n + 1) * 0x10000]
                 for n in range(8)}
        z = Z80(pages[0], pages[1],
                rom_pages={n: pages[n] for n in range(2, 8)})
        z.latch = latch
        z.sp = sp
        z.vdp_peer = V9958Peer()
        # The board's CTC: the timer driver's tick runs on it, and a
        # program idling between input polls (the SDK's idle) halts until
        # the next tick.  A test that needs its own CTC attaches it after.
        Ctc().attach(z)
        return z

    def skip_delay(self, z):
        """Skip the production serial-connect spin through its exported
        entry (the guard checks the latch, so a stale address cannot
        skip other code)."""
        skip_serial_connect_delay(
            z, self.syms["_kernel_serial_connect_delay"])

    def crt0(self, z, limit=CRT0_STEP_LIMIT):
        """Call `__kos_crt0_init` and run until it returns."""
        z.push16(RET)
        z.pc = self.syms["__kos_crt0_init"]
        for _ in range(limit):
            z.step()
            if z.pc == RET:
                return
        raise AssertionError("C runtime init did not return")

    def start_kernel(self, z, host_sync=b"", ctc0=False):
        """Run the C runtime entry, then start `_kernel_main`.
        `host_sync` preloads the serial input the smoke images wait for;
        `ctc0` arms the host CTC0 injector (see `injector`)."""
        self.crt0(z)
        z.push16(RET)
        if host_sync:
            z.sio_rx.extend(host_sync)
        if ctc0:
            z.wr(self.syms["_irq_active_source"], 0x00)
        z.pc = self.syms["_kernel_main"]
        return z

    def feed(self, z):
        """Call the attached input driver for this step, if any."""
        if self.input_driver is not None:
            self.input_driver(z)

    def injector(self, z):
        """Host stand-in for CTC0 (the simulator models no timer): arm
        the kernel's source cell, then call the returned injector once
        per step."""
        z.wr(self.syms["_irq_active_source"], 0x00)
        return Ctc0Injector(self.syms)

    def run_until(self, z, stops=(), limit=STEP_LIMIT, on_step=None,
                  since=0, halt_ok=False, panic=b"PANIC"):
        """Step with the connect delay skipped until one of the `stops`
        appears in the fresh transcript, the CPU halts (`halt_ok`), or
        the budget ends.  Returns the step count; a `panic` in the
        transcript tail raises.

        A matched marker settles first: the replay returns once its line
        is complete (the transcript ends in a newline) or the transcript
        that carried it has gone quiet, so a caller can assert the whole
        line (not just the prefix that matched)."""
        needles = [s.encode("ascii") if isinstance(s, str) else s
                   for s in stops]
        tx_len = len(z.sio_tx)
        matched = False
        quiet = 0
        for step in range(limit):
            self.skip_delay(z)
            self.feed(z)
            if on_step is not None:
                on_step(z)
            z.step()
            tx = bytes(z.sio_tx)
            grew = len(tx) != tx_len
            if grew:
                tx_len = len(tx)
                if panic is not None and panic in tx[-96:]:
                    raise AssertionError("panic during boot: %r"
                                         % tx[-300:])
                quiet = 0
            if matched:
                if tx.endswith(b"\n"):
                    return step
                quiet = quiet + 1 if not grew else 0
                if quiet >= MARKER_SETTLE_STEPS:
                    return step
                continue
            if any(needle in tx[since:] for needle in needles):
                matched = True
                continue
            if halt_ok and z.halted:
                return step
        raise AssertionError(
            "did not reach %r (PC=%04X, tail=%r)"
            % ([s.decode("ascii", "replace") if isinstance(s, bytes)
                else s for s in stops], z.pc, bytes(z.sio_tx)[-200:]))

    def boot(self, z, stops=(), **kwargs):
        """`start_kernel` then `run_until`."""
        self.start_kernel(z)
        return self.run_until(z, stops, **kwargs)

    def call(self, z, address, limit=STEP_LIMIT, on_step=None):
        """Call the function at `address` and run until it returns."""
        z.push16(RET)
        z.pc = address
        for _ in range(limit):
            if on_step is not None:
                on_step(z)
            z.step()
            if z.pc == RET:
                return
        raise AssertionError("function at %04X did not return (PC=%04X)"
                             % (address, z.pc))

    # -- readers ---------------------------------------------------------
    def transcript(self, z):
        return bytes(z.sio_tx).decode("ascii", "replace")

    def read(self, z, symbol):
        """One byte of linked state from the booted machine: common
        memory lives in bank 0, the nucleus (fixed core and cold half)
        in bank 7."""
        address = self.syms[symbol]
        return (z.bank0[address] if address >= COMMON_BASE
                else z.bank7[address])

    def read16(self, z, symbol):
        address = self.syms[symbol]
        bank = z.bank0 if address >= COMMON_BASE else z.bank7
        return bank[address] | (bank[address + 1] << 8)

    def linked_bank7(self, address, length):
        """Bank-7 bytes of the linked image: page 1 of the ROM image."""
        page1 = self.image[0x10000:0x20000]
        return bytes(page1[address:address + length])

    def geometry(self):
        """The build's generated memory geometry (KOS_* constants)."""
        values = {}
        with open(os.path.join(self.root, self.subdir, "geometry",
                               "memory.inc")) as handle:
            for line in handle:
                match = re.match(
                    r"^(KOS_[A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)", line)
                if match:
                    values[match.group(1)] = int(match.group(2), 0)
        return values

    def reserved_stacks(self):
        """The kernel's reserved stack regions as named half-open byte
        ranges: the driver stack ends where the ISR stack starts."""
        return (
            ("driver", self.syms["kos_driver_stack"],
             self.syms["kos_driver_stack_top"] - 1),
            ("isr", self.syms["irq_isr_stack"] + 1,
             self.syms["irq_isr_stack_top"] - 1),
            ("gate", self.syms["kos_gate_stack"],
             self.syms["kos_gate_stack_top"] - 1),
        )

    def romfs_entries(self):
        """The image's packed recovery store, by name."""
        with open(os.path.join(self.root, self.subdir,
                               "recovery.romfs"), "rb") as handle:
            _header, entries = romfspack.parse_image(handle.read())
        return {entry["name"]: entry for entry in entries}

    def report(self, path):
        """One packer report (JSON) from the build tree."""
        with open(os.path.join(self.root, path)) as handle:
            return json.load(handle)


class ConsoleReplay(Replay):
    """A replay that reads the visible V9958 lane.

    The console images drive the monitor through the V9958 command
    trace; `pump` decodes the trace into a positioned glyph model and
    the wait helpers assert against it.  The serial transcript (early
    SIO) stays available through `Replay.transcript`.
    """

    def make_cpu(self, latch=LATCH_MODE11, sp=STACK, serial_console=False):
        z = super().make_cpu(latch, sp)
        if not serial_console:
            z.hid = IocCommandPeer()
            z.ioc_peer = z.hid
        z.vdp = V9958Peer()
        z.vdp_peer = z.vdp
        z.glyphs = []
        z.glyph_pos = 0
        z.glyph_last = None
        return z

    def pump(self, z):
        """Decode newly issued glyph copies into a positioned text model.

        A 6x8 HMMM copy is a printable glyph; a glyph that does not
        continue the previous copy starts a new model line.  CR/LF and
        scrolling are handled by the renderer itself, so the model only
        needs the printed characters."""
        commands = z.vdp.commands
        while z.glyph_pos < len(commands):
            cmd = commands[z.glyph_pos]
            z.glyph_pos += 1
            block = cmd.get("block")
            if (cmd.get("code") != 0xD0 or not block or
                    (block[8] | (block[9] << 8)) != 6 or
                    (block[10] | (block[11] << 8)) != 8):
                continue
            sx = block[0] | (block[1] << 8)
            sy = block[2]
            dx = block[4] | (block[5] << 8)
            dy = block[6] | (block[7] << 8)
            glyph = ((sy & 0x3F) >> 3) * 32 + sx // 6
            if 32 <= glyph < 127:
                if z.glyph_last == [dy, dx - 6]:
                    z.glyphs.append(chr(glyph))
                else:
                    z.glyphs.append("\n" + chr(glyph))
                z.glyph_last = [dy, dx]

    def trace(self, z, since=0):
        self.pump(z)
        return "".join(z.glyphs[since:])

    def run_to(self, z, expect, limit=STEP_LIMIT, since=0):
        """Step until `expect` appears in the visible trace."""
        for i in range(limit):
            self.skip_delay(z)
            self.feed(z)
            z.step()
            if i % 0x1FFF == 0:
                self.pump(z)
                if expect in "".join(z.glyphs[since:]):
                    return
        self.pump(z)
        raise AssertionError(
            "did not reach %r (PC=%04X, tail=%r)"
            % (expect, z.pc, "".join(z.glyphs)[-160:]))

    def run_until_count(self, z, marker, count, limit=STEP_LIMIT):
        """Step until `marker` occurs at least `count` times in the
        whole trace (used for relaunch detection)."""
        for i in range(limit):
            self.skip_delay(z)
            self.feed(z)
            z.step()
            if i % 0x1FFF == 0:
                self.pump(z)
                if "".join(z.glyphs).count(marker) >= count:
                    return
        self.pump(z)
        raise AssertionError(
            "%r occurred %d times, wanted %d (PC=%04X)"
            % (marker, "".join(z.glyphs).count(marker), count, z.pc))

    def run_to_pattern(self, z, pattern, limit=STEP_LIMIT, since=0):
        """Step until `pattern` matches the fresh trace (for output whose
        end is not a fixed marker, e.g. a rendered hex value)."""
        for i in range(limit):
            self.skip_delay(z)
            self.feed(z)
            z.step()
            if i % 0x1FFF == 0:
                self.pump(z)
                if re.search(pattern, "".join(z.glyphs[since:])):
                    return
        self.pump(z)
        raise AssertionError(
            "did not match %r (PC=%04X, tail=%r)"
            % (pattern, z.pc, "".join(z.glyphs)[-160:]))

    def send_line(self, z, line, expect, limit=STEP_LIMIT):
        """Type one monitor line through the HID input lane and wait
        for the fresh output it produces."""
        start = len(z.glyphs)
        z.hid.hid_queue.extend(line.encode("ascii") + b"\r")
        self.run_to(z, expect, limit, since=start)

    def send_raw(self, z, data, expect, limit=STEP_LIMIT):
        start = len(z.glyphs)
        z.hid.hid_queue.extend(data)
        self.run_to(z, expect, limit, since=start)

    def boot_to_prompt(self, z, limit=STEP_LIMIT):
        self.start_kernel(z)
        self.run_to(z, PROMPT, limit)
        return self.trace(z)

    def boot_to_shell(self, z, limit=STEP_LIMIT, prompt=SHELL_PROMPT):
        """Boot to the shell's displayed prompt (the boot program of the
        production systems).  `prompt` is the card-less `/system> ` where
        a replay starts without one."""
        self.start_kernel(z)
        self.run_to(z, prompt, limit)
        return self.trace(z)


if __name__ == "__main__":  # pragma: no cover - manual smoke check
    print(__doc__)
