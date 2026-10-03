#!/usr/bin/env python3
"""Minimal Z80 simulator to replay the ZephyrOS selftest flows.

Models the banking latch (MEM_DECODER rev 12):
  mode 00: 0000-FFFF reads from the selected ROM page; writes target the
           selected SRAM bank (reset copier and ROM-access stub);
  mode 08: flat view of the selected SRAM bank over the whole 64 KiB;
           no ROM and no forced bank (MEM_MODE_FLAT);
  mode 10: 0000-DFFF = bankN, E000+ = bank0 (application);
  mode 11: 0000-1FFF = bankN, 2000-DFFF = bank7, E000+ = bank0 (OS).

Boot pages 0/1 come from the provided images; the M5 recovery pages 2-7
are supplied separately (rom_pages) when a replay exercises ROMFS boot.

Usage: sim.py <selftest-image> [start-pc] [trace]
"""

import sys

SZ = 0x10000
FILL = 0xFF


class Z80:
    def __init__(self, page0, page1, rom_pages=None):
        self.bank0 = bytearray(page0)
        self.bank7 = bytearray(page1)
        self.banks = {n: bytearray([FILL] * SZ) for n in range(1, 7)}
        # ROM images for the ROM-read modes.  Page 0/1 mirror the boot
        # images; recovery pages are provided by the caller.
        self.rom = {0: bytearray(page0), 1: bytearray(page1)}
        if rom_pages:
            for number, data in rom_pages.items():
                self.rom[number] = bytearray(data)
        self.latch = 0x00
        self.pc = 0
        self.sp = 0
        self.a = self.f = 0
        self.b = self.c = 0
        self.d = self.e = 0
        self.h = self.l = 0
        self.ix = self.iy = 0
        self.alt_a = self.alt_f = 0
        self.alt_b = self.alt_c = 0
        self.alt_d = self.alt_e = 0
        self.alt_h = self.alt_l = 0
        self.cycles = 0
        self.halted = False
        self.interrupts = False
        self.iff1 = False
        self.iff2 = False
        self.ei_delay = 0
        self.i = 0
        self.im = 0
        self.int_pending = False
        self.int_vector = 0
        # SIO0/B emulation (ports 22h data / 23h control).  Bytes written
        # to the data port are captured in sio_tx; bytes queued in sio_rx
        # are returned by data-port reads and make RR0's RX-available bit
        # set on control-port reads.  Without pending input the control
        # port reports TX-empty/CTS but no RX-ready, so polling drivers
        # see "no data" instead of the previous 0xFF alias.
        self.sio_tx = bytearray()
        self.sio_rx = bytearray()
        self.sio_ctrl_writes = bytearray()

    def accept_interrupt(self, vector):
        """Accept one maskable IM2 interrupt and fetch its linked target."""
        if not self.iff1 or self.im != 2:
            raise AssertionError("interrupt not enabled in IM2")
        self.halted = False
        self.iff1 = False
        self.push16(self.pc)
        pointer = ((self.i << 8) | (vector & 0xFF)) & 0xFFFF
        self.pc = self.rd16(pointer)

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

    @property
    def PV(self):
        return (self.f >> 2) & 1

    @PV.setter
    def PV(self, v):
        self._sf(0x04, v)

    # ---- interrupts ----
    def assert_int(self, vector):
        """Assert INT with the given IM2 vector byte; delivered when the
        CPU next accepts maskable interrupts."""
        self.int_vector = vector & 0xFF
        self.int_pending = True

    def _accept_interrupt(self):
        self.int_pending = False
        self.iff1 = False
        self.iff2 = False
        self.push16(self.pc)
        self.pc = self.rd16(((self.i << 8) | self.int_vector) & 0xFFFF)
        self.halted = False

    def _interrupt_check(self):
        if self.int_pending and self.iff1 and not self.ei_delay and self.im == 2:
            self._accept_interrupt()
            return True
        return False

    def _cond(self, cc):
        return {0: not self.ZF, 1: self.ZF, 2: not self.CF, 3: self.CF,
                4: not self.PV, 5: self.PV, 6: not self.SNF, 7: self.SNF}[cc]

    @property
    def PF(self):
        return (self.f >> 2) & 1

    @PF.setter
    def PF(self, v):
        self._sf(0x04, v)

    # ---- memory mapping ----
    def _sram(self, bank):
        if bank == 0:
            return self.bank0
        if bank == 7:
            return self.bank7
        return self.banks[bank]

    def bank_of(self, addr):
        """SRAM bank backing `addr` in the RAM-only modes (10h/18h)."""
        mode = self.latch & 0x18
        bank = self.latch & 0x07
        if addr >= 0xE000:
            return 0
        if mode == 0x10:          # application
            return bank
        if mode == 0x18:          # OS
            return 7 if addr >= 0x2000 else bank
        raise SystemExit("sim: bank_of unsupported mode %02Xh" % mode)

    def rd(self, addr):
        mode = self.latch & 0x18
        if mode == 0x00:
            # ROM access: every read (including instruction fetch) is the
            # selected ROM page; writes still land in the selected bank.
            return self.rom[(self.latch >> 5) & 0x07][addr]
        if mode == 0x08:
            # Flat SRAM: the selected bank over the whole address space.
            return self._sram(self.latch & 0x07)[addr]
        return self._sram(self.bank_of(addr))[addr]

    def rd16(self, addr):
        return self.rd(addr) | (self.rd((addr + 1) & 0xFFFF) << 8)

    def wr(self, addr, v):
        mode = self.latch & 0x18
        if mode in (0x00, 0x08):
            self._sram(self.latch & 0x07)[addr] = v & 0xFF
            return
        self._sram(self.bank_of(addr))[addr] = v & 0xFF

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
        # HALT: only an accepted interrupt wakes the CPU.
        if self.halted:
            if self.int_pending and self.iff1:
                self._accept_interrupt()
            return
        # IM2 acceptance before the next instruction (blocked for exactly
        # one instruction after EI).
        if self._interrupt_check():
            return
        self.cycles += 1
        op = self.fetch()
        x, y, z, p, q = (op >> 6) & 3, (op >> 3) & 7, op & 7, (op >> 4) & 3, (op >> 5) & 1
        if op == 0x00: pass
        elif op == 0x07:  # rlca
            c = (self.a >> 7) & 1
            self.a = ((self.a << 1) | c) & 0xFF
            self.CF = c
        elif op == 0x0F:  # rrca
            c = self.a & 1
            self.a = ((self.a >> 1) | (c << 7)) & 0xFF
            self.CF = c
        elif op == 0x17:  # rla
            c = (self.a >> 7) & 1
            self.a = ((self.a << 1) | (1 if self.CF else 0)) & 0xFF
            self.CF = c
        elif op == 0x1F:  # rra
            c = self.a & 1
            self.a = ((self.a >> 1) | ((1 if self.CF else 0) << 7)) & 0xFF
            self.CF = c
        elif op == 0x76: self.halted = True
        elif op == 0xF3: self.iff1 = self.iff2 = False
        elif op == 0xFB:
            self.iff1 = self.iff2 = True
            # EI admits an interrupt only after the following instruction.
            # The per-step countdown below consumes one tick for EI itself.
            self.ei_delay = 2
        elif op == 0x08:  # ex af,af'
            self.a, self.f, self.alt_a, self.alt_f = \
                self.alt_a, self.alt_f, self.a, self.f
        elif op == 0x10:  # djnz d
            off = self.fetch()
            if off & 0x80: off -= 256
            self.b = (self.b - 1) & 0xFF
            if self.b:
                self.pc = (self.pc + off) & 0xFFFF
        elif op == 0xD9:  # exx
            self.b, self.c, self.alt_b, self.alt_c = \
                self.alt_b, self.alt_c, self.b, self.c
            self.d, self.e, self.alt_d, self.alt_e = \
                self.alt_d, self.alt_e, self.d, self.e
            self.h, self.l, self.alt_h, self.alt_l = \
                self.alt_h, self.alt_l, self.h, self.l
        elif op == 0xF9:  # ld sp,hl
            self.sp = (self.h << 8) | self.l
        elif op == 0xED and False: pass
        elif op == 0xC9:  # ret
            self.pc = self.pop16()
        elif op == 0xC3: self.pc = self.fetch16()
        elif op == 0xE9:  # jp (hl)
            self.pc = (self.h << 8) | self.l
        elif op in (0x18, 0x28, 0x20, 0x38, 0x30):
            off = self.fetch()
            if off & 0x80: off -= 256
            if op == 0x18 or (op == 0x28 and self.ZF) or (op == 0x20 and not self.ZF) \
               or (op == 0x38 and self.CF) or (op == 0x30 and not self.CF):
                self.pc = (self.pc + off) & 0xFFFF
        elif op in (0xC2, 0xCA, 0xD2, 0xDA, 0xE2, 0xEA, 0xF2, 0xFA):
            nn = self.fetch16()
            cond = {
                0xC2: not self.ZF, 0xCA: self.ZF,
                0xD2: not self.CF, 0xDA: self.CF,
                0xE2: not self.PF, 0xEA: self.PF,
                0xF2: not self.SNF, 0xFA: self.SNF,
            }[op]
            if cond:
                self.pc = nn
        elif op in (0xC4, 0xCC, 0xD4, 0xDC, 0xE4, 0xEC, 0xF4, 0xFC):
            nn = self.fetch16()
            if self._cond((op >> 3) & 7):
                self.push16(self.pc)
                self.pc = nn
        elif op in (0xC0, 0xC8, 0xD0, 0xD8, 0xE0, 0xE8, 0xF0, 0xF8):
            if self._cond((op >> 3) & 7):
                self.pc = self.pop16()
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
        elif op == 0xCB:
            self.step_cb()
        elif op in (0xD3, 0xDB):
            port = self.fetch()
            if op == 0xD3:
                if port == 0x00:
                    self.latch = self.a & 0xFF
                elif port == 0x22:
                    self.sio_tx.append(self.a & 0xFF)
                elif port == 0x23:
                    self.sio_ctrl_writes.append(self.a & 0xFF)
            else:
                if port == 0x00:
                    self.a = self.latch
                elif port == 0x22:
                    self.a = self.sio_rx.pop(0) if self.sio_rx else 0xFF
                elif port == 0x23:
                    # RR0: TX-empty + CTS set; RX-available only when a
                    # byte is queued.
                    self.a = 0xFE | (0x01 if self.sio_rx else 0x00)
                else:
                    self.a = 0xFF
        elif op in (0x01, 0x11, 0x21, 0x31):
            self.set_rr((op >> 4) & 3, self.fetch16())
        elif op == 0x09:  # add hl,bc
            self.hl = self._add16(self.get_rr(2), self.get_rr(0))
        elif op == 0x19:  # add hl,de
            self.hl = self._add16(self.get_rr(2), self.get_rr(1))
        elif op == 0x29:  # add hl,hl
            self.hl = self._add16(self.get_rr(2), self.get_rr(2))
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
        elif op == 0x34:  # inc (hl)
            addr = (self.h << 8) | self.l
            v = (self.rd(addr) + 1) & 0xFF
            self.wr(addr, v)
            self.ZF = v == 0
            self.SNF = (v & 0x80) != 0
        elif op == 0x35:  # dec (hl)
            addr = (self.h << 8) | self.l
            v = (self.rd(addr) - 1) & 0xFF
            self.wr(addr, v)
            self.ZF = v == 0
            self.SNF = (v & 0x80) != 0
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
        elif op in (0x46, 0x4E, 0x56, 0x5E, 0x66, 0x6E):  # ld r,(hl)
            regs = {0x46: 'b', 0x4E: 'c', 0x56: 'd', 0x5E: 'e', 0x66: 'h', 0x6E: 'l'}
            setattr(self, regs[op], self.rd((self.h << 8) | self.l))
        elif op in (0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x77):  # ld (hl),r
            regs = {0x70: 'b', 0x71: 'c', 0x72: 'd', 0x73: 'e',
                    0x74: 'h', 0x75: 'l', 0x77: 'a'}
            self.wr((self.h << 8) | self.l, getattr(self, regs[op]))
        elif op == 0x7E: self.a = self.rd((self.h << 8) | self.l)
        elif op == 0x77: self.wr((self.h << 8) | self.l, self.a)
        elif op == 0x02: self.wr((self.b << 8) | self.c, self.a)
        elif op == 0x0A: self.a = self.rd((self.b << 8) | self.c)
        elif op == 0x12: self.wr((self.d << 8) | self.e, self.a)
        elif op == 0x1A: self.a = self.rd((self.d << 8) | self.e)
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
        elif op == 0x87:
            self.alu(op)
        elif 0x80 <= op <= 0xBF:
            self.alu(op)
        elif op == 0xE6: self.a = self._logic(self.a & self.fetch())
        elif op == 0xEE: self.a = self._logic(self.a ^ self.fetch())
        elif op == 0xF6: self.a = self._logic(self.a | self.fetch())
        elif op == 0xFE: self._logic(self.a - self.fetch(), sub=True)
        elif op == 0xD6: self._sub(self.fetch())
        elif op == 0xDE:  # sbc a,n
            self._sub(self.fetch(), 1 if self.CF else 0)
        elif op == 0xC6: self._add(self.fetch())
        elif op == 0xCE:  # adc a,n
            self._add(self.fetch(), 1 if self.CF else 0)
        elif op == 0xEB:  # ex de,hl
            self.d, self.e, self.h, self.l = self.h, self.l, self.d, self.e
        elif op == 0xE3:  # ex (sp),hl
            hl = (self.h << 8) | self.l
            self.hl = self.rd16(self.sp)
            self.wr16(self.sp, hl)
        elif op == 0x0B: self.set_rr(0, (self.get_rr(0) - 1) & 0xFFFF)
        elif op == 0x1B: self.set_rr(1, (self.get_rr(1) - 1) & 0xFFFF)
        elif op == 0x2B: self.hl = (self.get_rr(2) - 1) & 0xFFFF
        elif op in (0x04, 0x0C, 0x14, 0x1C, 0x24, 0x2C, 0x3C):  # inc r
            regs = {0x04: 'b', 0x0C: 'c', 0x14: 'd', 0x1C: 'e',
                    0x24: 'h', 0x2C: 'l', 0x3C: 'a'}
            v = (getattr(self, regs[op]) + 1) & 0xFF
            setattr(self, regs[op], v)
            self.ZF = v == 0
            self.SNF = (v & 0x80) != 0
        elif op in (0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x3D):  # dec r
            regs = {0x05: 'b', 0x0D: 'c', 0x15: 'd', 0x1D: 'e',
                    0x25: 'h', 0x2D: 'l', 0x3D: 'a'}
            v = (getattr(self, regs[op]) - 1) & 0xFF
            setattr(self, regs[op], v)
            self.ZF = v == 0
            self.SNF = (v & 0x80) != 0
        elif op == 0x3B: self.sp = (self.sp - 1) & 0xFFFF
        elif op == 0x03: self.set_rr(0, (self.get_rr(0) + 1) & 0xFFFF)
        elif op == 0x13: self.set_rr(1, (self.get_rr(1) + 1) & 0xFFFF)
        elif op == 0x23: self.hl = (self.get_rr(2) + 1) & 0xFFFF
        elif op == 0x33: self.sp = (self.sp + 1) & 0xFFFF
        elif op == 0xED:
            self.step_ed()
        else:
            raise SystemExit("sim: unsupported opcode %02X at %04X" % (op, (self.pc - 1) & 0xFFFF))
        # The one-instruction EI delay must survive the EI step itself:
        # EI sets the delay, the instruction after EI runs while the check
        # is blocked, and only then does the delay expire.
        if self.ei_delay and op != 0xFB:
            self.ei_delay -= 1

    def _add16(self, hl, rr):
        res = hl + rr
        self._sf(1, (res & 0x10000) != 0)
        return res & 0xFFFF

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

    def _logic(self, res, sub=False, add=False):
        raw = res
        res = raw & 0xFF
        self.ZF = res == 0
        self.SNF = (res & 0x80) != 0
        if sub:
            self._sf(0x10, res & 0x10)  # H (approx)
            self._sf(0x02, 1)           # N = 1
            self.CF = raw < 0
        elif add:
            self._sf(0x10, 0)
            self._sf(0x02, 0)           # N = 0
            self.CF = raw > 0xFF
        else:
            self._sf(0x10, 0)
            self._sf(0x02, 0)           # N = 0
            self.CF = 0
            self.PV = (bin(res).count('1') & 1) == 0
        return res

    def _sub(self, n, carry=0):
        v = self.a - n - carry
        self.a = self._logic(v, sub=True)

    def _add(self, n, carry=0):
        v = self.a + n + carry
        self.a = self._logic(v, add=True)

    def alu(self, op):
        srcs = {0: 'b', 1: 'c', 2: 'd', 3: 'e', 4: 'h', 5: 'l', 7: 'a'}
        r = op & 7
        if r == 6:  # (hl)
            v = self.rd((self.h << 8) | self.l)
        else:
            v = getattr(self, srcs[r])
        grp = op & 0xF8
        if grp == 0x80:
            self._add(v)
        elif grp == 0x88:  # adc
            self._add(v, 1 if self.CF else 0)
        elif grp == 0x90:
            self._sub(v)
        elif grp == 0x98:  # sbc
            self._sub(v, 1 if self.CF else 0)
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
        elif op == 0x73:  # ld (nn),sp
            self.wr16(self.fetch16(), self.sp)
        elif op == 0x5B:  # ld de,(nn)
            self.de = self.rd16(self.fetch16())
        elif op == 0x4B:  # ld bc,(nn)
            self.bc = self.rd16(self.fetch16())
        elif op == 0x53:  # ld (nn),de
            self.wr16(self.fetch16(), (self.d << 8) | self.e)
        elif op == 0x43:  # ld (nn),bc
            self.wr16(self.fetch16(), (self.b << 8) | self.c)
        elif op == 0x47:  # ld i,a
            self.i = self.a
        elif op == 0x57:  # ld a,i (P/V reflects IFF2)
            self.a = self.i
            self._sf(0x04, self.iff2)
        elif op in (0x46, 0x56, 0x5E):
            self.im = {0x46: 0, 0x56: 1, 0x5E: 2}[op]
        elif op == 0x4D:  # reti: IFF1 <- IFF2
            self.pc = self.pop16()
            self.iff1 = self.iff2
        elif op == 0x45:  # retn: IFF1 <- IFF2
            self.pc = self.pop16()
            self.iff1 = self.iff2
        elif op in (0x42, 0x52, 0x62, 0x72,
                    0x4A, 0x5A, 0x6A, 0x7A):  # sbc/adc hl,rr
            rr = (op >> 4) & 3
            carry = 1 if self.CF else 0
            if op & 0x08:  # adc hl,rr
                res = self.hl + self.get_rr(rr) + carry
                self.CF = res > 0xFFFF
            else:          # sbc hl,rr
                res = self.hl - self.get_rr(rr) - carry
                self.CF = res < 0
            self.hl = res & 0xFFFF
            self.ZF = self.hl == 0
            self.SNF = (self.hl & 0x8000) != 0
        else:
            raise SystemExit("sim: unsupported ED %02X at %04X" % (op, (self.pc - 2) & 0xFFFF))

    @property
    def de(self):
        return (self.d << 8) | self.e

    @de.setter
    def de(self, v):
        self.d, self.e = (v >> 8) & 0xFF, v & 0xFF

    @property
    def bc(self):
        return (self.b << 8) | self.c

    @bc.setter
    def bc(self, v):
        self.b, self.c = (v >> 8) & 0xFF, v & 0xFF

    def step_cb(self):
        op = self.fetch()
        regs = {0: 'b', 1: 'c', 2: 'd', 3: 'e', 4: 'h', 5: 'l', 7: 'a'}
        r = op & 7
        if r == 6:
            raise SystemExit("sim: unsupported CB %02X (mem)" % op)
        name = regs[r]
        if op < 0x40:  # rotate/shift (flags approximated; Z/S accurate)
            v = getattr(self, name)
            grp = (op >> 3) & 7
            if grp == 0:    # rlc
                c = (v >> 7) & 1
                v = ((v << 1) | c) & 0xFF
                self.CF = c
            elif grp == 1:  # rrc
                c = v & 1
                v = ((v >> 1) | (c << 7)) & 0xFF
                self.CF = c
            elif grp == 2:  # rl
                c = (v >> 7) & 1
                v = ((v << 1) | self.CF) & 0xFF
                self.CF = c
            elif grp == 3:  # rr
                c = v & 1
                v = ((v >> 1) | (self.CF << 7)) & 0xFF
                self.CF = c
            elif grp == 4:  # sla
                c = (v >> 7) & 1
                v = (v << 1) & 0xFF
                self.CF = c
            elif grp == 5:  # sra
                c = v & 1
                v = (v >> 1) | (v & 0x80)
                self.CF = c
            else:           # srl
                c = v & 1
                v = v >> 1
                self.CF = c
            setattr(self, name, v)
            self.ZF = v == 0
            self.SNF = (v & 0x80) != 0
            self.PV = (bin(v).count('1') & 1) == 0
        elif op < 0x80:  # bit n,r: Z = !bit
            bit = (op >> 3) & 7
            self.ZF = not ((getattr(self, name) >> bit) & 1)
        elif op < 0xC0:  # res n,r
            bit = (op >> 3) & 7
            setattr(self, name, getattr(self, name) & ~(1 << bit) & 0xFF)
        else:            # set n,r
            bit = (op >> 3) & 7
            setattr(self, name, getattr(self, name) | (1 << bit))

    def step_prefix(self, op):
        op2 = self.fetch()
        if op == 0xDD:
            reg = self.ix
        else:
            reg = self.iy
        if op2 == 0xCB:  # DD/FD CB d op: rotate/bit/res/set on (ix+d)
            off = self.fetch()
            if off & 0x80: off -= 256
            addr = (reg + off) & 0xFFFF
            op3 = self.fetch()
            v = self.rd(addr)
            regs = {0: 'b', 1: 'c', 2: 'd', 3: 'e', 4: 'h', 5: 'l', 7: 'a'}
            if op3 < 0x40:  # rotate/shift memory, copy to register
                grp = (op3 >> 3) & 7
                if grp == 0:    # rlc
                    c = (v >> 7) & 1
                    v = ((v << 1) | c) & 0xFF
                    self.CF = c
                elif grp == 1:  # rrc
                    c = v & 1
                    v = ((v >> 1) | (c << 7)) & 0xFF
                    self.CF = c
                elif grp == 2:  # rl
                    c = (v >> 7) & 1
                    v = ((v << 1) | self.CF) & 0xFF
                    self.CF = c
                elif grp == 3:  # rr
                    c = v & 1
                    v = ((v >> 1) | (self.CF << 7)) & 0xFF
                    self.CF = c
                elif grp == 4:  # sla
                    c = (v >> 7) & 1
                    v = (v << 1) & 0xFF
                    self.CF = c
                elif grp == 5:  # sra
                    c = v & 1
                    v = (v >> 1) | (v & 0x80)
                    self.CF = c
                else:           # srl
                    c = v & 1
                    v = v >> 1
                    self.CF = c
                self.wr(addr, v)
                r = op3 & 7
                if r != 6:
                    setattr(self, regs[r], v)
                self.ZF = v == 0
                self.SNF = (v & 0x80) != 0
                self.PV = (bin(v).count('1') & 1) == 0
            elif op3 < 0x80:  # bit n,(ix+d)
                bit = (op3 >> 3) & 7
                self.ZF = not ((v >> bit) & 1)
            elif op3 < 0xC0:  # res n,(ix+d)
                bit = (op3 >> 3) & 7
                self.wr(addr, v & ~(1 << bit) & 0xFF)
            else:             # set n,(ix+d)
                bit = (op3 >> 3) & 7
                self.wr(addr, v | (1 << bit))
        elif op2 == 0x21:
            if op == 0xDD:
                self.ix = self.fetch16()
            else:
                self.iy = self.fetch16()
        elif op2 == 0x2A:  # ld ix/iy,(nn)
            v = self.rd16(self.fetch16())
            if op == 0xDD:
                self.ix = v
            else:
                self.iy = v
        elif op2 == 0x22:  # ld (nn),ix/iy
            self.wr16(self.fetch16(), reg)
        elif op2 == 0x23:  # inc ix/iy
            if op == 0xDD:
                self.ix = (self.ix + 1) & 0xFFFF
            else:
                self.iy = (self.iy + 1) & 0xFFFF
        elif op2 == 0x2B:  # dec ix/iy
            if op == 0xDD:
                self.ix = (self.ix - 1) & 0xFFFF
            else:
                self.iy = (self.iy - 1) & 0xFFFF
        elif op2 == 0xE5:  # push ix/iy
            self.push16(self.ix if op == 0xDD else self.iy)
        elif op2 == 0xE1:  # pop ix/iy
            v = self.pop16()
            if op == 0xDD:
                self.ix = v
            else:
                self.iy = v
        elif op2 == 0xE9:  # jp (ix)/(iy)
            self.pc = reg
        elif op2 == 0xF9:  # ld sp,ix/iy
            self.sp = reg
        elif op2 in (0x09, 0x19, 0x29, 0x39):  # add ix/iy,rr (flags approx.)
            rr = (op2 >> 4) & 3
            res = reg + self.get_rr(rr)
            if op == 0xDD:
                self.ix = res & 0xFFFF
            else:
                self.iy = res & 0xFFFF
            self._sf(1, (res & 0x10000) != 0)
        elif op2 == 0x36:  # ld (ix+d),n
            off = self.fetch()
            if off & 0x80: off -= 256
            self.wr((reg + off) & 0xFFFF, self.fetch())
        elif op2 in (0x46, 0x4E, 0x56, 0x5E, 0x66, 0x6E, 0x7E):  # ld r,(ix+d)
            off = self.fetch()
            if off & 0x80: off -= 256
            v = self.rd((reg + off) & 0xFFFF)
            regs = {0x46: 'b', 0x4E: 'c', 0x56: 'd', 0x5E: 'e',
                    0x66: 'h', 0x6E: 'l', 0x7E: 'a'}
            setattr(self, regs[op2], v)
        elif op2 in (0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x77):  # ld (ix+d),r
            off = self.fetch()
            if off & 0x80: off -= 256
            regs = {0x70: 'b', 0x71: 'c', 0x72: 'd', 0x73: 'e',
                    0x74: 'h', 0x75: 'l', 0x77: 'a'}
            self.wr((reg + off) & 0xFFFF, getattr(self, regs[op2]))
        elif op2 in (0x86, 0x8E, 0x96, 0x9E, 0xA6, 0xAE, 0xB6, 0xBE):  # alu a,(ix+d)
            off = self.fetch()
            if off & 0x80: off -= 256
            v = self.rd((reg + off) & 0xFFFF)
            grp = op2 & 0xF8
            if grp == 0x80:
                self._add(v)
            elif grp == 0x88:
                self._add(v, 1 if self.CF else 0)  # adc (ix+d)
            elif grp == 0x90:
                self._sub(v)
            elif grp == 0x98:
                self._sub(v, 1 if self.CF else 0)  # sbc (ix+d)
            elif grp == 0xA0:
                self.a = self._logic(self.a & v)
            elif grp == 0xA8:
                self.a = self._logic(self.a ^ v)
            elif grp == 0xB0:
                self.a = self._logic(self.a | v)
            else:
                self._logic(self.a - v, sub=True)  # cp
        elif op2 in (0x34, 0x35):  # inc/dec (ix+d)
            off = self.fetch()
            if off & 0x80: off -= 256
            addr = (reg + off) & 0xFFFF
            v = self.rd(addr)
            v = (v + 1) & 0xFF if op2 == 0x34 else (v - 1) & 0xFF
            self.wr(addr, v)
            self.ZF = v == 0
            self.SNF = (v & 0x80) != 0
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
