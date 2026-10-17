#!/usr/bin/env python3
"""klibc arithmetic, digit and string routines, run in the host Z80.

The SDK's compiler helpers (division, multiplication), the digit
formatters and the memory/string routines are assembly or lean C; this
module links the real members into one probe program, runs it in the
simulator over a table of inputs, and checks every result against the C
semantics computed here: unsigned 16- and 32-bit arithmetic, signed
division truncating toward zero with the remainder's sign the
dividend's, a zero divisor answering 0, string differences as unsigned
chars.

Run with: `python3 -m unittest tests.test_klibc`.
"""

import os
import random
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from test_stdio import SdkProbe, _source  # noqa: E402

ASM = ["klibc/sdcc_arith16.asm", "klibc/sdcc_arith_entries.asm",
       "klibc/sdcc_arith_signed_entries.asm", "klibc/divmod.asm",
       "klibc/mul.asm", "klibc/memcpy.asm", "klibc/memmove.asm",
       "klibc/memset.asm", "klibc/memcmp.asm", "klibc/strlen.asm",
       "klibc/strcmp.asm", "klibc/strncmp.asm", "klibc/strchr.asm"]
C_MEMBERS = ["klibc/sdcc_arith.c", "klibc/sdcc_arith_signed.c",
             "klibc/utoa.c", "klibc/ultoa.c", "klibc/itoa.c",
             "klibc/ltoa.c"]


def _u16_pairs():
    rng = random.Random(16)
    fixed = [(0, 0), (1, 0), (65535, 0), (0, 1), (65535, 1), (65535, 65535),
             (65534, 65535), (32768, 32767), (32767, 32768), (40000, 3),
             (12345, 10), (9, 10), (10, 10), (65535, 255), (65535, 256),
             (1000, 7), (60000, 40000)]
    return fixed + [(rng.randrange(65536), rng.randrange(1, 65536))
                    for _ in range(24)]


def _s16_pairs():
    rng = random.Random(17)
    fixed = [(7, 2), (-7, 2), (7, -2), (-7, -2), (-32768, 1), (-32768, -1),
             (32767, -1), (0, 5), (5, 0), (-5, 0), (-1, 3), (100, -7)]
    return fixed + [(rng.randrange(-32768, 32768), rng.randrange(-300, 300)
                     or 1) for _ in range(16)]


def _u32_pairs():
    rng = random.Random(32)
    fixed = [(0, 1), (0xFFFFFFFF, 1), (0xFFFFFFFF, 10), (4000000000, 7),
             (123456789, 1000), (5, 0), (0x80000000, 0x7FFFFFFF)]
    return fixed + [(rng.randrange(1 << 32), rng.randrange(1, 1 << 20))
                    for _ in range(10)]


def _trunc_div(a, b):
    if b == 0:
        return 0, 0
    q = abs(a) // abs(b)
    if (a < 0) != (b < 0):
        q = -q
    return q, a - q * b


def _c_table(kind, name, values):
    return "static const %s %s[%d] = {%s};" % (
        kind, name, len(values), ", ".join(str(v) for v in values))


def _fmt(value, base, width, pad):
    digits = "0123456789ABCDEF"
    if value == 0:
        text = "0"
    else:
        text = ""
        magnitude = abs(value)
        while magnitude:
            text = digits[magnitude % base] + text
            magnitude //= base
    sign = "-" if value < 0 else ""
    total = len(text) + len(sign)
    if width > total:
        if pad == "0":
            return sign + "0" * (width - total) + text
        return pad * (width - total) + sign + text
    return sign + text


class TestKlibcRoutines(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.u16 = _u16_pairs()
        cls.s16 = _s16_pairs()
        cls.u32 = _u32_pairs()
        cls.mul = [(a, b) for a, b in cls.u16] + [(255, 255), (256, 256),
                                                 (3, 21845), (65535, 65535)]
        cls.schar = [(-128, -128), (-128, 127), (127, 127), (-1, 1), (0, -5),
                     (-7, 9), (100, -100)]
        cls.fmts = [(0, 10, 0, " "), (65535, 10, 0, " "), (42, 10, 5, " "),
                    (42, 10, 5, "0"), (255, 16, 4, "0"), (51966, 16, 0, " ")]
        cls.sfmts = [(-32768, 10, 0, " "), (-42, 10, 6, " "), (-42, 10, 6, "0"),
                     (123, 10, 2, " ")]
        cls.lfmts = [(0xFFFFFFFF, 10, 0, " "), (1234567890, 10, 12, "0"),
                     (0xDEADBEEF, 16, 0, " ")]
        cls.slfmts = [(-2147483648, 10, 0, " "), (-99, 10, 5, "0")]
        probe = "\n".join([
            '#include "kos/types.h"',
            '#include "klibc/string.h"',
            '#include "klibc/fmt.h"',
            _c_table("unsigned int", "ua", [a for a, _ in cls.u16]),
            _c_table("unsigned int", "ub", [b for _, b in cls.u16]),
            _c_table("int", "sa", [a for a, _ in cls.s16]),
            _c_table("int", "sb", [b for _, b in cls.s16]),
            _c_table("unsigned long", "la", ["%du" % a for a, _ in cls.u32]),
            _c_table("unsigned long", "lb", ["%du" % b for _, b in cls.u32]),
            _c_table("unsigned int", "ma", [a for a, _ in cls.mul]),
            _c_table("unsigned int", "mb", [b for _, b in cls.mul]),
            _c_table("signed char", "ca", [a for a, _ in cls.schar]),
            _c_table("signed char", "cb", [b for _, b in cls.schar]),
            "unsigned int uq[%d], ur[%d];" % (len(cls.u16), len(cls.u16)),
            "int sq[%d], sr[%d];" % (len(cls.s16), len(cls.s16)),
            "unsigned long lq[%d], lr[%d];" % (len(cls.u32), len(cls.u32)),
            "unsigned int mp[%d];" % len(cls.mul),
            "int cp[%d];" % len(cls.schar),
            "char fmt_out[%d][40];" % (len(cls.fmts) + len(cls.sfmts) +
                                      len(cls.lfmts) + len(cls.slfmts)),
            "int str_out[16];",
            "unsigned char mem_out[64];",
            "void probe_main(void)",
            "{",
            "\tunsigned char i;",
            "\tunsigned char k = 0u;",
            "\tfor (i = 0u; i < %du; ++i) { uq[i] = ua[i] / ub[i]; "
            "ur[i] = ua[i] %% ub[i]; }" % len(cls.u16),
            "\tfor (i = 0u; i < %du; ++i) { sq[i] = sa[i] / sb[i]; "
            "sr[i] = sa[i] %% sb[i]; }" % len(cls.s16),
            "\tfor (i = 0u; i < %du; ++i) { lq[i] = la[i] / lb[i]; "
            "lr[i] = la[i] %% lb[i]; }" % len(cls.u32),
            "\tfor (i = 0u; i < %du; ++i) { mp[i] = ma[i] * mb[i]; }"
            % len(cls.mul),
            "\tfor (i = 0u; i < %du; ++i) { cp[i] = ca[i] * cb[i]; }"
            % len(cls.schar),
        ] + ["\tutoa(fmt_out[k++], %du, %d, %d, '%s');" % f
             for f in cls.fmts]
          + ["\titoa(fmt_out[k++], %d, %d, %d, '%s');" % f
             for f in cls.sfmts]
          + ["\tultoa(fmt_out[k++], %duL, %d, %d, '%s');" % f
             for f in cls.lfmts]
          + ["\tltoa(fmt_out[k++], %dL, %d, %d, '%s');" % f
             for f in cls.slfmts]
          + [
            '\tstr_out[0] = (int)strlen("");',
            '\tstr_out[1] = (int)strlen("kona os");',
            '\tstr_out[2] = strcmp("abc", "abc");',
            '\tstr_out[3] = strcmp("abc", "abd");',
            '\tstr_out[4] = strcmp("abd", "abc");',
            '\tstr_out[5] = strcmp("ab", "abc");',
            '\tstr_out[6] = strcmp("\\xff", "a");',
            '\tstr_out[7] = strncmp("abcx", "abcy", 3u);',
            '\tstr_out[8] = strncmp("abcx", "abcy", 4u);',
            '\tstr_out[9] = strncmp("a", "b", 0u);',
            '\tstr_out[10] = memcmp("abc", "abd", 3u);',
            '\tstr_out[11] = memcmp("abc", "abd", 2u);',
            '\tstr_out[12] = memcmp("\\x01", "\\xf0", 1u);',
            '\tstr_out[13] = (int)(strchr("kona", \'n\') != (char *)0);',
            '\tstr_out[14] = (int)(strchr("kona", \'z\') == (char *)0);',
            '\tstr_out[15] = (int)(*strchr("kona", 0) == 0);',
            "\tfor (i = 0u; i < 64u; ++i) { mem_out[i] = i; }",
            "\tmemmove(&mem_out[4], &mem_out[0], 8u);",     # up, overlap
            "\tmemmove(&mem_out[20], &mem_out[24], 8u);",   # down, overlap
            "\tmemcpy(&mem_out[40], &mem_out[0], 17u);",
            "\tmemset(&mem_out[58], 0xAA, 5u);",
            "\tmemcpy(&mem_out[63], &mem_out[0], 0u);",
            "}",
        ])
        cls.tmp = tempfile.mkdtemp(prefix="koslibc-")
        cls.probe = SdkProbe(cls.tmp, C_MEMBERS, probe,
                             probe_asm=[_source(name) for name in ASM])
        cls.probe.build().run(limit=6000000)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def words(self, name, count, signed=False):
        bank = self.probe.ram(name)
        base = self.probe.syms[name]
        out = []
        for i in range(count):
            value = bank[base + 2 * i] | (bank[base + 2 * i + 1] << 8)
            if signed and value >= 0x8000:
                value -= 0x10000
            out.append(value)
        return out

    def longs(self, name, count):
        bank = self.probe.ram(name)
        base = self.probe.syms[name]
        return [int.from_bytes(bytes(bank[base + 4 * i:base + 4 * i + 4]),
                               "little") for i in range(count)]

    def test_unsigned_16_bit_division(self):
        quotients = self.words("_uq", len(self.u16))
        remainders = self.words("_ur", len(self.u16))
        for (a, b), q, r in zip(self.u16, quotients, remainders):
            self.assertEqual((q, r), (a // b, a % b) if b else (0, 0),
                             (a, b))

    def test_signed_16_bit_division(self):
        quotients = self.words("_sq", len(self.s16), signed=True)
        remainders = self.words("_sr", len(self.s16), signed=True)
        for (a, b), q, r in zip(self.s16, quotients, remainders):
            want_q, want_r = _trunc_div(a, b)
            # -32768 / -1 overflows int: the low 16 bits wrap.
            want_q = ((want_q + 0x8000) & 0xFFFF) - 0x8000
            self.assertEqual((q, r), (want_q, want_r), (a, b))

    def test_unsigned_32_bit_division(self):
        quotients = self.longs("_lq", len(self.u32))
        remainders = self.longs("_lr", len(self.u32))
        for (a, b), q, r in zip(self.u32, quotients, remainders):
            self.assertEqual((q, r), (a // b, a % b) if b else (0, 0),
                             (a, b))

    def test_multiplication(self):
        products = self.words("_mp", len(self.mul))
        for (a, b), p in zip(self.mul, products):
            self.assertEqual(p, (a * b) & 0xFFFF, (a, b))
        chars = self.words("_cp", len(self.schar), signed=True)
        for (a, b), p in zip(self.schar, chars):
            self.assertEqual(p, a * b, (a, b))

    def test_digit_formatting(self):
        bank = self.probe.ram("_fmt_out")
        base = self.probe.syms["_fmt_out"]
        cases = self.fmts + self.sfmts + self.lfmts + self.slfmts
        for index, (value, radix, width, pad) in enumerate(cases):
            raw = bytes(bank[base + 40 * index:base + 40 * index + 40])
            text = raw.split(b"\0")[0].decode("ascii")
            self.assertEqual(text, _fmt(value, radix, width, pad),
                             (value, radix, width, pad))

    def test_string_routines(self):
        results = self.words("_str_out", 16, signed=True)
        self.assertEqual(results[0:2], [0, 7])
        self.assertEqual(results[2], 0)
        self.assertEqual(results[3], ord("c") - ord("d"))
        self.assertEqual(results[4], ord("d") - ord("c"))
        self.assertEqual(results[5], 0 - ord("c"))
        self.assertEqual(results[6], 0xFF - ord("a"))
        self.assertEqual(results[7:10], [0, ord("x") - ord("y"), 0])
        self.assertEqual(results[10:13], [ord("c") - ord("d"), 0, 1 - 0xF0])
        self.assertEqual(results[13:16], [1, 1, 1])

    def test_memory_routines(self):
        bank = self.probe.ram("_mem_out")
        base = self.probe.syms["_mem_out"]
        got = list(bank[base:base + 64])
        want = list(range(64))
        want[4:12] = list(range(0, 8))         # memmove up over itself
        want[20:28] = list(range(24, 32))      # memmove down over itself
        want[40:57] = want[0:17]               # memcpy, 17 bytes
        want[58:63] = [0xAA] * 5               # memset
        self.assertEqual(got, want)


if __name__ == "__main__":
    unittest.main()
