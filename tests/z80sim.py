#!/usr/bin/env python3
"""Minimal Z80 simulator to replay the ZephyrOS syscall selftest flow.

Models the banking latch (mode 10: 0000-DFFF=bankN, E000+=bank0;
mode 11: 0000-1FFF=bankN, 2000-DFFF=bank7, E000+=bank0) and executes the
real instructions from the selftest image.

Usage: sim.py <selftest-image> [start-pc] [trace]
"""

import sys

SZ = 0x10000
FILL = 0xFF


class Z80:
    def __init__(self, page0, page1):
        self.bank0 = bytearray(page0)
        self.bank7 = bytearray(page1)
        self.banks = {n: bytearray([FILL] * SZ) for n in range(1, 7)}
        self.latch = 0x00
        self.pc = 0
        self.sp = 0
        self.a = self.f = 0
        self.b = self.c = 0
        self.d = self.e = 0
        self.h = self.l = 0
        self.ix = self.iy = 0
        self.cycles = 0
        self.halted = False
        self.interrupts = False
        self.iff1 = False

    # ---- flags ----
    def _sf(self, bit, on):
        self.f = (self.f | bit) if on else (self.f & ~bit)

    @property
    def CF(self):
        return (self.f >> 0) & 1

    @CF.setter
    def CF(self, v):
        self._sf(1, v)

    @property
    def ZF(self):
        return (self.f >> 6) & 1

    @ZF.setter
    def ZF(self, v):
        self._sf(0x40, v)

    @property
    def SNF(self):
        return (self.f >> 7) & 1

    @SNF.setter
    def SNF(self, v):
        self._sf(0x80, v)

    # ---- memory mapping ----
    def bank_of(self, addr):
        mode = self.latch & 0x18
        bank = self.latch & 0x07
        if addr >= 0xE000:
            return 0
        if mode == 0x10:          # application
            return bank
        if mode == 0x18:          # OS
            return 7 if addr >= 0x2000 else bank
        raise SystemExit("sim: unsupported mode %02Xh" % mode)

    def rd(self, addr):
        bank = self.bank_of(addr)
        if bank == 0:
            return self.bank0[addr]
        if bank == 7:
            return self.bank7[addr]
        return self.banks[bank][addr]

    def rd16(self, addr):
        return self.rd(addr) | (self.rd((addr + 1) & 0xFFFF) << 8)

    def wr(self, addr, v):
        bank = self.bank_of(addr)
        if bank == 0:
            self.bank0[addr] = v & 0xFF
        elif bank == 7:
            self.bank7[addr] = v & 0xFF
        else:
            self.banks[bank][addr] = v & 0xFF

    def wr16(self, addr, v):
        self.wr(addr, v & 0xFF)
        self.wr((addr + 1) & 0xFFFF, (v >> 8) & 0xFF)

    def push16(self, v):
        self.sp = (self.sp - 2) & 0xFFFF
        self.wr16(self.sp, v)

    def pop16(self):
        v = self.rd16(self.sp)
        self.sp = (self.sp + 2) & 0xFFFF
        return v

    def fetch(self):
        op = self.rd(self.pc)
        self.pc = (self.pc + 1) & 0xFFFF
        return op

    def fetch16(self):
        lo = self.fetch()
        hi = self.fetch()
        return lo | (hi << 8)

    def set_rr(self, code, v):
        if code == 0: self.b, self.c = (v >> 8) & 0xFF, v & 0xFF
        elif code == 1: self.d, self.e = (v >> 8) & 0xFF, v & 0xFF
        elif code == 2: self.h, self.l = (v >> 8) & 0xFF, v & 0xFF
        else: self.sp = v

    def get_rr(self, code):
        if code == 0: return (self.b << 8) | self.c
        if code == 1: return (self.d << 8) | self.e
        if code == 2: return (self.h << 8) | self.l
        return self.sp

    def step(self):
        self.cycles += 1
        op = self.fetch()
        x, y, z, p, q = (op >> 6) & 3, (op >> 3) & 7, op & 7, (op >> 4) & 3, (op >> 5) & 1
        if op == 0x00: pass
        elif op == 0x76: self.halted = True
        elif op == 0xF3: self.iff1 = False
        elif op == 0xFB: self.iff1 = True
        elif op == 0xF9:  # ld sp,hl
            self.sp = (self.h << 8) | self.l
        elif op == 0xED and False: pass
        elif op == 0xC9:  # ret
            self.pc = self.pop16()
        elif op == 0xC3: self.pc = self.fetch16()
        elif op in (0x18, 0x28, 0x20, 0x38, 0x30):
            off = self.fetch()
            if off & 0x80: off -= 256
            if op == 0x18 or (op == 0x28 and self.ZF) or (op == 0x20 and not self.ZF) \
               or (op == 0x38 and self.CF) or (op == 0x30 and not self.CF):
                self.pc = (self.pc + off) & 0xFFFF
        elif op in (0xC2, 0xCA, 0xD2, 0xDA, 0xE2, 0xEA, 0xF2, 0xFA):
            nn = self.fetch16()
            cond = {(0xC2, 0xD2, 0xE2, 0xF2): not self.ZF,
                    (0xCA, 0xDA, 0xEA, 0xFA): self.ZF}.get(tuple(sorted([op])))
            if op in (0xD2, 0xDA, 0xF2, 0xFA) and not self.CF:
                self.pc = nn
            elif op in (0xE2, 0xEA) and (self.pc == self.pc):
                pass
        elif op == 0xCD:  # call
            target = self.fetch16()
            self.push16(self.pc)
            self.pc = target
        elif op == 0xC7 or (0xC7 <= op <= 0xFF and (op & 0xC7) == 0xC7):
            self.push16(self.pc)
            self.pc = op & 0x38
        elif op in (0xC5, 0xD5, 0xE5, 0xF5):
            vals = {0xC5: (self.b << 8) | self.c, 0xD5: (self.d << 8) | self.e,
                    0xE5: (self.h << 8) | self.l, 0xF5: (self.a << 8) | self.f}
            self.push16(vals[op])
        elif op in (0xC1, 0xD1, 0xE1, 0xF1):
            v = self.pop16()
            if op == 0xC1: self.b, self.c = v >> 8, v & 0xFF
            elif op == 0xD1: self.d, self.e = v >> 8, v & 0xFF
            elif op == 0xE1: self.h, self.l = v >> 8, v & 0xFF
            else: self.a, self.f = v >> 8, v & 0xFF
        elif op in (0xDD, 0xFD):
            self.step_prefix(op)
        elif op in (0xD3, 0xDB):
            port = self.fetch()
            if op == 0xD3:
                if port == 0x00:
                    self.latch = self.a & 0xFF
            else:
                if port == 0x00:
                    self.a = self.latch
                else:
                    self.a = 0xFF
        elif op in (0x01, 0x11, 0x21, 0x31):
            self.set_rr((op >> 4) & 3, self.fetch16())
        elif op == 0x22:  # ld (nn),hl
            self.wr16(self.fetch16(), (self.h << 8) | self.l)
        elif op == 0x2A:  # ld hl,(nn)
            self.hl = self.rd16(self.fetch16())
        elif op == 0x32:  # ld (nn),a
            self.wr(self.fetch16(), self.a)
        elif op == 0x3A:  # ld a,(nn)
            self.a = self.rd(self.fetch16())
        elif op == 0x36:  # ld (hl),n
            self.wr((self.h << 8) | self.l, self.fetch())
        elif op == 0x39:  # add hl,sp
            hl = (self.h << 8) | self.l
            res = hl + self.sp
            self.hl = res
            self._sf(1, (res & 0x10000) != 0)
        elif op in (0x47, 0x4F, 0x57, 0x5F, 0x67, 0x6F, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D,
                    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D,
                    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D,
                    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D):
            self.ld_reg(op)
        elif op == 0x7E: self.a = self.rd((self.h << 8) | self.l)
        elif op == 0x77: self.wr((self.h << 8) | self.l, self.a)
        elif op == 0x3E: self.a = self.fetch()
        elif op in (0x06, 0x0E, 0x16, 0x1E, 0x26, 0x2E):
            regs = {0x06: 'b', 0x0E: 'c', 0x16: 'd', 0x1E: 'e', 0x26: 'h', 0x2E: 'l'}
            setattr(self, regs[op], self.fetch())
        elif op == 0x17:  # rla
            oldc = self.CF
            self.CF = (self.a >> 7) & 1
            self.a = ((self.a << 1) | oldc) & 0xFF
        elif op == 0x1F:  # rra
            oldc = self.CF
            self.CF = self.a & 1
            self.a = ((self.a >> 1) | (oldc << 7)) & 0xFF
        elif op == 0x2F: self.a ^= 0xFF
        elif op == 0x37: self.CF = 1
        elif op == 0x3F: self.CF ^= 1
        elif op == 0xAF: self.a = 0
        elif op == 0xA7: self._logic(self.a & self.a)
        elif op == 0xB7: self._logic(self.a | self.a)
        elif op in (0x87, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4,
                    0xA5, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC,
                    0xBD):
            self.alu(op)
        elif op == 0xE6: self.a = self._logic(self.a & self.fetch())
        elif op == 0xEE: self.a = self._logic(self.a ^ self.fetch())
        elif op == 0xF6: self.a = self._logic(self.a | self.fetch())
        elif op == 0xFE: self._logic(self.a - self.fetch(), sub=True)
        elif op == 0xD6: self._sub(self.fetch())
        elif op == 0xC6: self._add(self.fetch())
        elif op == 0xEB:  # ex de,hl
            self.d, self.e, self.h, self.l = self.h, self.l, self.d, self.e
        elif op == 0xE3:  # ex (sp),hl
            hl = (self.h << 8) | self.l
            self.hl = self.rd16(self.sp)
            self.wr16(self.sp, hl)
        elif op == 0x0B: self.set_rr(0, (self.get_rr(0) - 1) & 0xFFFF)
        elif op == 0x1B: self.set_rr(1, (self.get_rr(1) - 1) & 0xFFFF)
        elif op == 0x2B: self.hl = (self.get_rr(2) - 1) & 0xFFFF
        elif op == 0x3B: self.sp = (self.sp - 1) & 0xFFFF
        elif op == 0x23: self.hl = (self.get_rr(2) + 1) & 0xFFFF
        elif op == 0xED:
            self.step_ed()
        else:
            raise SystemExit("sim: unsupported opcode %02X at %04X" % (op, (self.pc - 1) & 0xFFFF))

    @property
    def hl(self):
        return (self.h << 8) | self.l

    @hl.setter
    def hl(self, v):
        self.h, self.l = (v >> 8) & 0xFF, v & 0xFF

    def ld_reg(self, op):
        dst = {0x47: 'b', 0x4F: 'c', 0x57: 'd', 0x5F: 'e', 0x67: 'h', 0x6F: 'l',
               0x78: 'a', 0x79: 'a', 0x7A: 'a', 0x7B: 'a', 0x7C: 'a', 0x7D: 'a'}.get(op)
        src = {0x47: 'a', 0x4F: 'a', 0x57: 'a', 0x5F: 'a', 0x67: 'a', 0x6F: 'a',
               0x78: 'b', 0x79: 'c', 0x7A: 'd', 0x7B: 'e', 0x7C: 'h', 0x7D: 'l'}.get(op)
        if op & 0xF8 == 0x40:  # 40h-47h: ld b,*
            dst = 'b'
            src = ['b', 'c', 'd', 'e', 'h', 'l', None, 'a'][op & 7]
            if src is None:
                return
        elif op & 0xF8 == 0x48:  # 48h-4Fh: ld c,*
            dst = 'c'
            src = ['b', 'c', 'd', 'e', 'h', 'l', None, 'a'][op & 7]
            if src is None:
                return
        elif op & 0xF8 == 0x50:  # 50h-57h: ld d,*
            dst = 'd'
            src = ['b', 'c', 'd', 'e', 'h', 'l', None, 'a'][op & 7]
            if src is None:
                return
        elif op & 0xF8 == 0x58:  # 58h-5Fh: ld e,*
            dst = 'e'
            src = ['b', 'c', 'd', 'e', 'h', 'l', None, 'a'][op & 7]
            if src is None:
                return
        elif op & 0xF8 == 0x60:  # 60h-67h: ld h,*
            dst = 'h'
            src = ['b', 'c', 'd', 'e', 'h', 'l', None, 'a'][op & 7]
            if src is None:
                return
        elif op & 0xF8 == 0x68:  # 68h-6Fh: ld l,*
            dst = 'l'
            src = ['b', 'c', 'd', 'e', 'h', 'l', None, 'a'][op & 7]
            if src is None:
                return
        if dst and src:
            setattr(self, dst, getattr(self, src))

    def _logic(self, res, sub=False):
        res &= 0xFF
        self.ZF = res == 0
        self.SNF = (res & 0x80) != 0
        if sub:
            self._sf(0x10, res & 0x10)  # H (approx)
            self.CF = res > 0xFF or res < 0
        else:
            self._sf(0x10, 0)
            self.CF = 0
        return res

    def _sub(self, n):
        v = self.a - n
        self.a = self._logic(v, sub=True)

    def _add(self, n):
        v = self.a + n
        self.a = self._logic(v, sub=False)

    def alu(self, op):
        srcs = {0: 'b', 1: 'c', 2: 'd', 3: 'e', 4: 'h', 5: 'l', 7: 'a'}
        r = op & 7
        if r == 6:
            raise SystemExit("sim: unsupported (hl) alu op %02X" % op)
        v = getattr(self, srcs[r])
        grp = op & 0xF8
        if grp == 0x80:
            self._add(v)
        elif grp == 0x90:
            self._sub(v)
        elif grp == 0xA0:
            self.a = self._logic(self.a & v)
        elif grp == 0xA8:
            self.a = self._logic(self.a ^ v)
        elif grp == 0xB0:
            self.a = self._logic(self.a | v)
        elif grp == 0xB8:  # cp r: flags only, A unchanged
            self._logic(self.a - v, sub=True)
        else:
            raise SystemExit("sim: unsupported alu op %02X" % op)

    def step_ed(self):
        op = self.fetch()
        if op == 0xB0:  # ldir
            bc = (self.b << 8) | self.c
            while bc:
                self.wr((self.d << 8) | self.e, self.rd((self.h << 8) | self.l))
                self.hl = (self.get_rr(2) + 1) & 0xFFFF
                de = (self.d << 8) | self.e
                self.d, self.e = ((de + 1) >> 8) & 0xFF, (de + 1) & 0xFF
                bc -= 1
                self.b, self.c = (bc >> 8) & 0xFF, bc & 0xFF
                self.cycles += 1
            self.ZF = 0
        elif op == 0x7B:  # ld sp,(nn)
            self.sp = self.rd16(self.fetch16())
        elif op == 0x4D:  # reti
            self.pc = self.pop16()
            self.iff1 = True
        else:
            raise SystemExit("sim: unsupported ED %02X at %04X" % (op, (self.pc - 2) & 0xFFFF))

    def step_prefix(self, op):
        op2 = self.fetch()
        if op == 0xDD:
            reg = self.ix
            push = 0xDD, 0xE5
            pop = 0xDD, 0xE1
        else:
            reg = self.iy
            push = 0xFD, 0xE5
            pop = 0xFD, 0xE1
        if op2 == 0x21:
            if op == 0xDD:
                self.ix = self.fetch16()
            else:
                self.iy = self.fetch16()
        elif op2 == 0xE5:  # push ix/iy
            self.push16(self.ix if op == 0xDD else self.iy)
        elif op2 == 0xE1:  # pop ix/iy
            v = self.pop16()
            if op == 0xDD:
                self.ix = v
            else:
                self.iy = v
        else:
            raise SystemExit("sim: unsupported %02X %02X" % (op, op2))


def read_symbols(map_path):
    import re
    syms = {}
    for line in open(map_path):
        m = re.match(r"\s*([0-9A-Fa-f]{8})\s+(\S+)", line)
        if m:
            syms[m.group(2)] = int(m.group(1), 16)
    return syms


def main():
    img_path = sys.argv[1]
    img = open(img_path, "rb").read()
    page0, page1 = img[:0x10000], img[0x10000:]
    z = Z80(page0, page1)
    syms = read_symbols(img_path.replace("zephyros-selftest.bin", "zephyros.map"))

    # Locate the driver's `call _zos_selftest_run` to find the return site.
    run = syms["_zos_selftest_run"]
    driver = syms["_zos_run_syscall_selftest"]
    code = bytes(page1[a] if a >= 0x2000 and a < 0xE000 else page0[a] for a in range(driver, driver + 0x80))
    call_bytes = bytes([0xCD, run & 0xFF, (run >> 8) & 0xFF])
    ret_site = driver + code.index(call_bytes) + 3

    bank = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    # Boot state: bank 0 populated from page 0, bank 7 from page 1,
    # mode 11 over bank 0, kernel stack in common.
    z.latch = 0x18
    z.sp = 0xFEFE
    z.pc = run
    z.a = bank
    z.push16(ret_site)
    z.sp = 0xFEFC

    trace = len(sys.argv) > 2 and sys.argv[2] == "trace"
    limit = 2000000
    steps = 0
    while steps < limit:
        steps += 1
        if trace and steps < 600:
            print("%6d PC=%04X SP=%04X A=%02X BC=%02X%02X DE=%02X%02X IX=%04X IY=%04X"
                  " latch=%02X" % (steps, z.pc, z.sp, z.a, z.b, z.c, z.d, z.e, z.ix, z.iy, z.latch))
        z.step()
        if z.halted:
            print("sim: HALT at step %d, PC=%04X" % (steps, z.pc))
            break
        if z.pc == ret_site:
            print("sim: returned to driver at step %d" % steps)
            print("sim: result=%02X bank=%02X sp=%04X"
                  % (z.bank0[syms["_zos_selftest_result"]], z.bank0[syms["_zos_selftest_bank"]], z.sp))
            break
        if z.pc > 0x10000 or steps > limit:
            print("sim: runaway at step %d, PC=%04X" % (steps, z.pc))
            break
    else:
        print("sim: step limit reached, PC=%04X" % z.pc)


if __name__ == "__main__":
    main()
