#!/usr/bin/env python3
"""Banked-call trampoline tests.

The spike measured what SDCC 4.5.0
emits for a `__banked` call and what the far definition expects, and these
tests pin both against the real tools:

* the call sequence: `ld e,#b_<fn>` (the segment id the definition's bank
  emitted), `ld hl,#_<fn>` (the entry the linker resolved) and
  `call ___sdcc_bcall_ehl`;
* the frame: every argument on the stack, the callee reading the first at
  `SP+5`, so the trampoline spends exactly three bytes between entry and
  transfer; results in A (8 bits), DE (16) and DEHL (32);
* the link: two far areas linked at `8000h` in one link are two IHX record
  streams, which is what `tools/exepack.py` splits by area.

`user/sdk/src/bcall.asm` is then assembled as it is in the SDK and run in
the host Z80 with the other SDK members stubbed: a program calls far
functions as plain C, the trampoline maps the segment (counting the
`segmap` calls, so the token cache is visible), transfers and restores the
caller's stack, and a call naming no far segment reports
`KOS_RT_EFARMAP` without running anything.

Run with: `python3 -m unittest tests.test_bcall`.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import ihx  # noqa: E402
import pack_image  # noqa: E402
from z80sim import Z80  # noqa: E402

SDCC = "sdcc"
SDASZ80 = "sdasz80"
SDLDZ80 = "sdldz80"

SDK_SRC = os.path.join(ROOT, "user", "sdk", "src")
BCALL_ASM = os.path.join(SDK_SRC, "bcall.asm")

CFLAGS = ["-mz80", "-c", "--nostdinc", "--Werror",
          "-I", os.path.join(ROOT, "abi"),
          "-I", os.path.join(ROOT, "user", "sdk", "include")]

# The far half: the SDK rule's flags for FAR1= are exactly these two extra
# options, bank first, so the module lands in `_FAR1` and every `__banked`
# definition emits `b_<fn> = 1`.
FAR_FLAGS = ["-bo", "1", "--codeseg", "FAR1"]

FAR_C = """\
unsigned char far_c(void) __banked
{
	return 0xC5u;
}

unsigned int far_add(unsigned int a) __banked
{
	return (unsigned int)(a + 2u);
}

unsigned long far_l(unsigned long a, unsigned char b) __banked
{
	return a + b;
}
"""

CALLER_C = """\
extern unsigned char far_c(void) __banked;
extern unsigned int far_add(unsigned int a) __banked;
extern unsigned long far_l(unsigned long a, unsigned char b) __banked;
extern unsigned int primary_banked(void) __banked;

unsigned char r8a, r8b;
unsigned int r16;
unsigned long r32;
unsigned int bad;

int probe(void)
{
	r8a = far_c();
	r8b = far_c();
	r16 = far_add(0x0102u);
	r32 = far_l(0x11223344ul, 0x11u);
	bad = primary_banked();
	return 0;
}

unsigned int primary_banked(void) __banked
{
	return 0x5Au;
}
"""

# The SDK members the trampoline calls, as SDK-shaped stubs: `segmap`
# returns a token per segment id and counts its calls, the aperture
# primitives are no-ops (the far code sits in the bank the machine's
# window shows), and the runtime-error mapper follows runtime.asm.
STUBS_ASM = """\
	.module bcall_probe_stubs

	.globl _segmap
	.globl _aperture_open
	.globl _aperture_close
	.globl _exe_runtime_error
	.globl _segmap_calls
	.globl _segmap_last
	.globl _probe
	.globl _start

	.area _CODE

; int segmap(unsigned int segment);  id in HL, token in DE.
_segmap::
	ld	a, l
	ld	(_segmap_last), a
	ld	hl, #_segmap_calls
	inc	(hl)
	ld	de, #0x0050
	ret

; unsigned char aperture_open(unsigned char token);  no-op, previous = 0.
_aperture_open::
	xor	a, a
	ret

_aperture_close::
	ret

; int exe_runtime_error(void);  A = the code -> DE = -code.
_exe_runtime_error::
	ld	e, a
	ld	d, #0
	xor	a, a
	sub	e
	ld	e, a
	ld	a, #0
	sbc	a, d
	ld	d, a
	ret

_start::
	ld	sp, #0xE000
	call	_probe
	halt

	.area _DATA
_segmap_calls::
	.db	0
_segmap_last::
	.db	0
"""

# The far module of the link-order test: two areas, one per segment.
FAR2_ASM = """\
	.module bcall_probe_far2

	.globl _far2_c

	.area _FAR2

_far2_c::
	ld	a, #0x3C
	ret
"""


def _run(argv, cwd=None, **kwargs):
    """Run a tool in the probe's own directory; a failure is loud (the
    tools write their diagnostics to the text streams)."""
    cwd = cwd or ROOT
    text = kwargs.pop("text", True)
    result = subprocess.run(argv, cwd=cwd, capture_output=True, text=text,
                            **kwargs)
    if result.returncode != 0:
        raise AssertionError("%s failed (%d):\n%s\n%s"
                             % (" ".join(argv), result.returncode,
                                result.stdout, result.stderr))
    return result


class BankedCallProbe:
    """Compiles, links and runs one banked-call probe program.

    The program is the SDK rule's shape in miniature: the primary half is
    an ordinary link, the far half is `FAR1=` (bank 1, `--codeseg FAR1`),
    the trampoline is `user/sdk/src/bcall.asm`, and the SDK members it
    calls are stubs.  The far area is linked at `8000h` like the rule's
    `-b _FAR<n> = 0x8000`, and the host machine keeps its bytes in the
    bank the mode-11 window shows, so the trampoline's transfer lands on
    the far code without a real auxiliary bank.
    """

    def __init__(self, tmp):
        self.tmp = tmp
        self.syms = None

    def path(self, name):
        return os.path.join(self.tmp, name)

    def write(self, name, text):
        with open(self.path(name), "w") as fh:
            fh.write(text)

    def build(self):
        self.write("far.c", FAR_C)
        self.write("caller.c", CALLER_C)
        self.write("stubs.asm", STUBS_ASM)
        _run([SDCC] + CFLAGS + FAR_FLAGS + ["-o", "far.rel", "far.c"],
             cwd=self.tmp)
        _run([SDCC] + CFLAGS + ["-o", "caller.rel", "caller.c"],
             cwd=self.tmp)
        _run([SDASZ80, "-o", "stubs.rel", "stubs.asm"], cwd=self.tmp)
        _run([SDASZ80, "-o", "bcall.rel", BCALL_ASM], cwd=self.tmp)
        self.write("probe.lk", "\n".join([
            "-n", "-m", "-w",
            "-i " + self.path("probe.ihx"),
            "-b _CODE = 0x015D",
            "-b _DATA = 0x0400",
            "-b _FAR1 = 0x8000",
            self.path("caller.rel"),
            self.path("far.rel"),
            self.path("bcall.rel"),
            self.path("stubs.rel"),
            "-e",
            "",
        ]))
        _run([SDLDZ80, "-f", "probe.lk"], cwd=self.tmp)
        with open(self.path("probe.map")) as fh:
            self.syms = ihx.parse_map_symbols(fh.read())
        return self

    def run(self):
        """The machine after the probe ran: registers and the result
        words, read back from the bank the program's writes landed in."""
        with open(self.path("probe.ihx")) as fh:
            image = ihx.parse_ihx(fh.read())
        mem = bytearray(0x10000)
        for addr, byte in image.items():
            mem[addr] = byte
        z = Z80(bytes(mem), bytes(0x10000))
        z.latch = 0x18  # mode 11
        z.sp = 0xE000
        bank = z.bank_of(0x8000)
        sram = z._sram(bank)
        base = self.syms["s__FAR1"]
        for i in range(self.syms["l__FAR1"]):
            sram[base + i] = mem[base + i]
        start = self.syms["_start"]
        z.pc = start
        for _ in range(20000):
            z.step()
            if z.rd(z.pc) == 0x76:  # the halt
                break
        self.machine = z
        self.ram = z._sram(z.bank_of(self.syms["_r8a"]))
        return self

    def byte(self, name):
        return self.ram[self.syms[name]]

    def word(self, name):
        base = self.syms[name]
        return self.ram[base] | (self.ram[base + 1] << 8)

    def long(self, name):
        base = self.syms[name]
        return (self.ram[base] | (self.ram[base + 1] << 8) |
                (self.ram[base + 2] << 16) | (self.ram[base + 3] << 24))

class BcallTestBase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="kosbcall-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)


class TestBankedCallConvention(BcallTestBase):
    """The spike's findings, asserted on a real SDCC build."""

    def setUp(self):
        super().setUp()
        self.probe = BankedCallProbe(self.tmp).build()

    def compiler(self, name):
        """Compile one probe source to assembly, exactly as the SDK rule
        does (`-S` keeps the text for the assertions)."""
        return _run([SDCC, "-mz80", "-S", "--nostdinc"] + CFLAGS[3:] +
                    (FAR_FLAGS if name == "far.c" else []) +
                    ["-o", name + ".s", name], cwd=self.probe.tmp)

    def test_banked_call_is_the_trampoline_sequence(self):
        """A call to a `__banked` function loads E with the bank symbol and
        HL with the linked entry, then calls `___sdcc_bcall_ehl`."""
        self.compiler("caller.c")
        with open(self.probe.path("caller.c.s")) as fh:
            asm = fh.read()
        self.assertIn("___sdcc_bcall_ehl", asm)
        self.assertRegex(
            asm, r"ld\s+e, #b_far_c\b")
        self.assertRegex(
            asm, r"ld\s+hl, #_far_c\b")
        # The b_ symbol comes from the definition's bank, and the primary
        # bank's `__banked` function emits b_ = 0 (no far segment).
        self.assertRegex(asm, r"b_primary_banked\s*=\s*0")

    def test_the_far_module_emits_its_bank_and_area(self):
        """`-bo 1 --codeseg FAR1` (bank first, as the SDK rule passes it)
        puts the module in `_FAR1` and emits `b_<fn> = 1`."""
        self.compiler("far.c")
        with open(self.probe.path("far.c.s")) as fh:
            asm = fh.read()
        self.assertRegex(asm, r"b_far_c\s*=\s*1")
        self.assertRegex(asm, r"b_far_add\s*=\s*1")
        self.assertIn(".area _FAR1", asm)
        # The first argument is read at SP+5: three bytes are reserved
        # between the entry stack pointer and the argument list, so the
        # trampoline must spend exactly three bytes on the way in.
        self.assertRegex(asm, r"_far_add::")
        match = re.search(r"_far_add::\n(.*?)\n\tret", asm, re.S)
        self.assertIsNotNone(match)
        self.assertIn("ld\thl, #5", match.group(1))
        self.assertIn("add\thl, sp", match.group(1))

    def test_two_far_areas_link_at_8000h_in_one_link(self):
        """The linker places several far areas at `8000h` and emits one
        record stream per area, in link order: what the packer splits."""
        self.probe.write("far2.asm", FAR2_ASM)
        _run([SDASZ80, "-o", "far2.rel", "far2.asm"], cwd=self.probe.tmp)
        self.probe.write("two.lk", "\n".join([
            "-n", "-m", "-w",
            "-i " + self.probe.path("two.ihx"),
            "-b _CODE = 0x0000",
            "-b _FAR1 = 0x8000",
            "-b _FAR2 = 0x8000",
            self.probe.path("far.rel"),
            self.probe.path("far2.rel"),
            "-e",
            "",
        ]))
        _run([SDLDZ80, "-f", "two.lk"], cwd=self.probe.tmp)
        with open(self.probe.path("two.map")) as fh:
            syms = ihx.parse_map_symbols(fh.read())
        self.assertEqual(syms["s__FAR1"], 0x8000)
        self.assertEqual(syms["s__FAR2"], 0x8000)
        self.assertGreater(syms["l__FAR1"], 0)
        self.assertGreater(syms["l__FAR2"], 0)
        with open(self.probe.path("two.ihx")) as fh:
            mem = ihx.parse_ihx(fh.read(), multi=True)
        # Both areas wrote 8000h: the first value is `_FAR1`'s entry
        # (`far_c`: LD A,0C5h = 3E C5) and the second `_FAR2`'s
        # (`far2_c`: LD A,3Ch = 3E 3C).
        self.assertEqual(mem[0x8000], [0x3E, 0x3E])
        self.assertEqual(mem[0x8001], [0xC5, 0x3C])


class TestBankedCallBehaviour(BcallTestBase):
    """The trampoline runs: results, the token cache and the error path."""

    def setUp(self):
        super().setUp()
        self.probe = BankedCallProbe(self.tmp).build().run()

    def test_results_come_back_in_a_de_and_dehl(self):
        self.assertEqual(self.probe.byte("_r8a"), 0xC5)
        self.assertEqual(self.probe.byte("_r8b"), 0xC5)
        self.assertEqual(self.probe.word("_r16"), 0x0104)
        self.assertEqual(self.probe.long("_r32"), 0x11223355)

    def test_the_frame_is_restored_exactly(self):
        """The far code's RET lands in the trampoline, which drops the
        one-byte slot and returns to the caller with SP back at the halt:
        the `call _probe` frame is all that is left."""
        # `_start` set SP to E000h, the call to the probe balanced, and
        # the halt is the only thing left running.
        self.assertEqual(self.probe.machine.sp, 0xE000)

    def test_the_token_cache_spends_one_segmap_per_segment(self):
        """Three banked calls of segment 1 - and the refused call of the
        primary bank's function - spend exactly one SYS_SEG_MAP: the token
        is cached, and an id that is not 1..4 is refused before the
        syscall."""
        self.assertEqual(self.probe.byte("_segmap_calls"), 1)
        self.assertEqual(self.probe.byte("_segmap_last"), 1)

    def test_a_primary_banked_function_reports_efarmap(self):
        """A `__banked` definition in the primary bank (b_ = 0) names no
        far segment: the call takes the runtime-error path - DE = -0x30,
        A = the low byte - and nothing runs."""
        self.assertEqual(self.probe.word("_bad"), 0xFFD0)


if __name__ == "__main__":
    unittest.main()
