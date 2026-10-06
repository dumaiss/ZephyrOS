#!/usr/bin/env python3
"""Host-side V9958 video functional tests: replay the real V9958
acceptance image (SIO + IOC + HID + the V9958 video core loaded from
ROMFS, production-derived arena) in the Z80 simulator against the
V9958 peer's register/VRAM model.

Coverage (all executing the real linked bytes):

* production-style boot: the V9958 module validates its ROMFS font
  resource, registers exactly `v9958.0` (ZDEV_VIDEO, the seven VIDEO
  capability bits, no character interface) and leaves the console
  binding on the SIO output fallback;
* the exact ported bootstrap: porch off with D0 preserved, paced
  R#25/R#8/stop/R#0..R#11/R#14..R#18/R#23/R#26/R#27, R#25 WTE on, porch
  on, then the palette ONLY under native WAIT, with the proven software
  pacing visible in the instruction-timestamped trace;
* the font atlas: both precolored 6x8 atlases are streamed from the
  ROMFS font in bounded chunks and match the independently computed
  expansion of the pinned asset, at the 128-KiB addresses 10000h and
  14000h;
* the command engine: clear-screen HMMV fills page zero with 44h and CE
  busy was observed before completion, exactly as the driver's waits
  require; the sprite cursor pattern/colors/SAT land at 1F800h/1F000h/
  1F200h while the 64-KiB-alias address F800h stays cleared;
* the target pass: RESET re-establishes every owned register and asset
  (two complete init cycles), PALETTE_WRITE changes entry 12, the HMMV
  fill presents a white rectangle, and DATA_WRITE + a 200-byte
  DATA_BLOCK draw the deterministic band;
* malformed requests (null/zero payloads, bad PRESENT/RESET lengths,
  unknown operation, null state) return the exact error and cause no
  port writes or reads at all;
* a direct 4096-byte DATA_BLOCK writes the full 17-bit address space
  (18000h) without aliasing the 64-KiB view (8000h) -- the historical
  16-byte non-block ceiling is gone;
* the font-failure image: the module publishes no device and performs
  no VDP access at all, while HID, SIO and the console binding remain
  untouched, and `v9958: unavailable` is reported.

Skipped when the matching image has not been built.
"""

import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import Z80, IocCommandPeer, V9958Peer, read_symbols  # noqa: E402
import fontpack  # noqa: E402

RET = 0x1234
STACK = 0xFFFE
STEP_LIMIT = 80000000

ZDEV_SERIAL = 1
ZDEV_VIDEO = 2
ZDEV_HID = 4

ZCAP_CONSOLE_IN = 0x0004
ZCAP_CONSOLE_OUT = 0x0008
ZCAP_VIDEO_CTRL_WRITE = 0x00000100
ZCAP_VIDEO_DATA_WRITE = 0x00000200
ZCAP_VIDEO_DATA_BLOCK = 0x00000400
ZCAP_VIDEO_PALETTE = 0x00000800
ZCAP_VIDEO_INDIRECT = 0x00001000
ZCAP_VIDEO_PRESENT = 0x00002000
ZCAP_VIDEO_RESET = 0x00004000
ZCAP_HID_TEXT = 0x00010000

VIDEO_CAPS = (ZCAP_VIDEO_CTRL_WRITE | ZCAP_VIDEO_DATA_WRITE |
              ZCAP_VIDEO_DATA_BLOCK | ZCAP_VIDEO_PALETTE |
              ZCAP_VIDEO_INDIRECT | ZCAP_VIDEO_PRESENT |
              ZCAP_VIDEO_RESET)
ZCAP_CHAR_OUT = 0x0002
CONSOLE_CAPS = VIDEO_CAPS | ZCAP_CHAR_OUT | ZCAP_CONSOLE_OUT

ZVIDEO_CTRL_WRITE = 1
ZVIDEO_DATA_WRITE = 2
ZVIDEO_DATA_BLOCK = 3
ZVIDEO_PALETTE_WRITE = 4
ZVIDEO_INDIRECT_WRITE = 5
ZVIDEO_PRESENT = 6
ZVIDEO_RESET = 7

ZDEV_OK = 0
ZDEV_EINVAL = -2
ZDEV_ENODEV = -3
ZDEV_ENOTSUP = -7

HOST_SYNC = b"\xaa\x55"

# Host-owned scratch for the payloads handed to the module's send().
# The bank-7 window is fully derived, so the buffer is picked per image in
# setUp as the first free arena granule above the packed boot set -- never a
# fixed address that the derived data base or a loaded module could reclaim.
SCRATCH_SIZE = 0x200

# The proven GRAPHIC 6 bootstrap, in order (value, register).
G6_TABLE = [
    (0x0A, 0), (0x00, 1), (0x1F, 2), (0x00, 3), (0x00, 4), (0xE4, 5),
    (0x3F, 6), (0x04, 7), (0x08, 8), (0x88, 9), (0x00, 10), (0x03, 11),
]
BOOTSTRAP = ([(0x00, 25), (0x08, 8), (0x00, 46)] + G6_TABLE +
             [(0x00, 14), (0x02, 15), (0x00, 16), (0x00, 17), (0x00, 18),
              (0xFC, 23), (0x00, 26), (0x00, 27), (0x04, 25)])
PALETTE = bytes([
    0x00, 0x00, 0x11, 0x01, 0x00, 0x06, 0x00, 0x07,
    0x05, 0x00, 0x07, 0x03, 0x50, 0x00, 0x06, 0x06,
    0x70, 0x00, 0x73, 0x03, 0x70, 0x07, 0x74, 0x07,
    0x00, 0x05, 0x67, 0x00, 0x55, 0x05, 0x77, 0x07,
])
PAIR_NORMAL = (0x44, 0x4F, 0xF4, 0xFF)
PAIR_REVERSE = (0xFF, 0xF4, 0x4F, 0x44)
PACING_MIN = 12  # instructions of the bounded djnz delay loop


def expand_row(bits, reverse):
    table = PAIR_REVERSE if reverse else PAIR_NORMAL
    return bytes((table[(bits >> 6) & 3], table[(bits >> 4) & 3],
                  table[(bits >> 2) & 3]))


def atlas_scanline(font, scanline, reverse):
    group, within = scanline >> 3, scanline & 7
    out = bytearray()
    for glyph in range(32):
        out += expand_row(font[group * 256 + glyph * 8 + within], reverse)
    return bytes(out)


def engine_commands(peer):
    return [cmd for cmd in peer.commands if cmd["busy_until"] is not None]


class V9958SimBase(unittest.TestCase):
    IMAGE_DIR = "build-video-selftest"
    IMAGE_NAME = "zephyros-video-selftest.bin"

    def setUp(self):
        self.bin_path = os.path.join(ROOT, self.IMAGE_DIR, self.IMAGE_NAME)
        if not os.path.exists(self.bin_path):
            self.skipTest("%s missing; build the image first" % self.bin_path)
        with open(self.bin_path, "rb") as handle:
            self.image = handle.read()
        self.syms = read_symbols(
            os.path.join(ROOT, self.IMAGE_DIR, "zephyros.map"))
        font_path = os.path.join(ROOT, "drivers", "v9958", "assets",
                                 "font_cp850_6x8.inc")
        self.font = fontpack.parse_font(font_path)
        self.scratch = self._derive_scratch()

    def _derive_scratch(self):
        """First free arena granule above the packed boot set."""
        plan_path = os.path.join(ROOT, self.IMAGE_DIR, "arena.plan.json")
        self.assertTrue(os.path.exists(plan_path),
                        "arena.plan.json missing; build the image first")
        with open(plan_path) as fh:
            arena = json.load(fh)["arena"]
        scratch = arena["start"] + arena["used_granules"] * arena["granule_size"]
        self.assertLessEqual(scratch + SCRATCH_SIZE, arena["end"],
                             "scratch %04X is not in the free arena space"
                             % scratch)
        return scratch

    # -- machine setup --------------------------------------------------
    def make_cpu(self, peer=None):
        pages = {n: self.image[n * 0x10000:(n + 1) * 0x10000]
                 for n in range(8)}
        z = Z80(pages[0], pages[1],
                rom_pages={n: pages[n] for n in range(2, 8)})
        z.latch = 0x18
        z.sp = STACK
        z.ioc_peer = IocCommandPeer()
        z.vdp_peer = peer if peer is not None else V9958Peer()
        return z

    def skip_delay(self, z):
        if z.pc == self.syms["_kernel_serial_connect_delay"]:
            z.pc = z.pop16()

    def crt0(self, z):
        z.push16(RET)
        z.pc = self.syms["__zos_crt0_init"]
        for _ in range(2000000):
            z.step()
            if z.pc == RET:
                return
        raise AssertionError("C runtime init did not return")

    def start_kernel(self, z):
        self.crt0(z)
        z.push16(RET)
        z.sio_rx.extend(HOST_SYNC)
        z.pc = self.syms["_kernel_main"]

    def step_to(self, z, marker, limit=STEP_LIMIT):
        for _ in range(limit):
            z.step()
            self.skip_delay(z)
            if marker in z.sio_tx:
                return bytes(z.sio_tx)
        raise AssertionError("did not reach %r (PC=%04X, tail=%r)"
                             % (marker, z.pc, bytes(z.sio_tx)[-160:]))

    def boot_to(self, peer, marker):
        z = self.make_cpu(peer)
        self.start_kernel(z)
        self.step_to(z, marker)
        return z

    # -- registry helpers -----------------------------------------------
    def devices(self, z):
        base = self.syms["_device_table"]
        out = []
        for index in range(16):
            off = base + index * 16
            if z.bank7[off] == 0:
                continue
            out.append({
                "handle": z.bank7[off],
                "class": z.bank7[off + 1],
                "caps": (z.bank7[off + 2] | (z.bank7[off + 3] << 8) |
                         (z.bank7[off + 4] << 16) |
                         (z.bank7[off + 5] << 24)),
                "name": z.bank7[off + 6] | (z.bank7[off + 7] << 8),
                "char_ops": z.bank7[off + 8] | (z.bank7[off + 9] << 8),
                "class_ops": z.bank7[off + 10] |
                             (z.bank7[off + 11] << 8),
                "state": z.bank7[off + 12] | (z.bank7[off + 13] << 8),
            })
        return out

    def device_by_class(self, z, class_id):
        found = [dev for dev in self.devices(z) if dev["class"] == class_id]
        return found[0] if found else None

    def cstr(self, z, addr):
        out = bytearray()
        while z.rd(addr) != 0:
            out.append(z.rd(addr))
            addr = (addr + 1) & 0xFFFF
        return bytes(out)

    def result(self, z, name):
        return z.rd(self.syms[name])

    # -- module calls ----------------------------------------------------
    def put_scratch(self, z, data):
        z.bank7[self.scratch:self.scratch + len(data)] = data
        return self.scratch

    def v9958_send(self, z, target, state, op, payload, length):
        """Call the module's zvideo_ops.send with the SDCC 4.5 layout:
        HL = state; DE is unused; on the stack, from SP: operation
        (1 byte), payload (2 bytes), length (2 bytes); int result in DE.
        Derived from generated call sites for this exact prototype."""
        sp_before = z.sp
        z.push16(length)
        z.push16(payload)
        z.push16((op & 0xFF) << 8)
        z.sp = (z.sp + 1) & 0xFFFF
        z.push16(RET)          # return address on top, as CALL leaves it
        z.h = (state >> 8) & 0xFF
        z.l = state & 0xFF
        z.pc = target
        for _ in range(STEP_LIMIT):
            z.step()
            if z.pc == RET:
                z.sp = sp_before
                value = (z.d << 8) | z.e
                return value - 0x10000 if value & 0x8000 else value
        raise AssertionError("send at %04X did not return" % target)

    def ops_for(self, z):
        dev = self.device_by_class(z, ZDEV_VIDEO)
        self.assertIsNotNone(dev, "v9958.0 is not registered")
        return dev, z.rd16(dev["class_ops"]), dev["state"]

    # -- shared assertions ----------------------------------------------
    def check_bootstrap(self, peer):
        regs = [(value, reg) for value, reg, _ in peer.reg_writes]
        r25_on = regs.index((0x04, 25))
        self.assertEqual(regs[:r25_on + 1], BOOTSTRAP,
                         "bootstrap register sequence diverged")
        self.assertEqual(regs[r25_on + 1:r25_on + 17],
                         [(index, 16) for index in range(16)],
                         "palette selects must follow R#25 WTE=1")

        # Paced bootstrap: every access before (and including) R#25=04
        # is spaced by the bounded delay loop.
        times = [now for _, _, now in peer.reg_writes[:r25_on + 1]]
        gaps = [b - a for a, b in zip(times, times[1:])]
        self.assertTrue(gaps, "no paced bootstrap writes recorded")
        self.assertGreaterEqual(min(gaps), PACING_MIN,
                                "bootstrap write pacing lost: %r" % gaps)

        # Latch: porch off first, porch on after WTE=1 and before the
        # palette; D0 (interrupt route) is preserved in every write.
        values = [value for _, value in peer.latch_writes]
        self.assertEqual(values[:2], [0x02, 0x00])
        self.assertTrue(all(value & 0x01 == 0 for value in values),
                        "latch write disturbed D0")
        off_now = peer.latch_writes[0][0]
        on_now = peer.latch_writes[1][0]
        self.assertLess(off_now, times[1])
        self.assertLess(times[r25_on], on_now)
        self.assertLess(on_now, peer.palette_writes[0][0])

        # Palette: exactly the proven table, programmed after WTE=1 and
        # the porch (real hardware WAIT), entry by entry.
        self.assertGreaterEqual(len(peer.palette_writes), 32)
        first_pass = peer.palette_writes[:32]
        self.assertEqual(bytes(value for _, value, _, _ in first_pass),
                         PALETTE)
        for now, _, _, _ in first_pass:
            self.assertGreater(now, times[r25_on])
            self.assertGreater(now, on_now)
            self.assertGreater(peer.regs[25] & 0x04, 0)

        # R#15=2 is established before the first status read.
        r15 = [(now, value) for value, reg, now in peer.reg_writes
               if reg == 15]
        self.assertTrue(r15 and r15[0][1] == 2)
        self.assertTrue(peer.status_reads)
        self.assertLess(r15[0][0], peer.status_reads[0][0])

        # Every command engine start shows CE held and released: the
        # driver only continues after a CE=0 status read.
        engines = engine_commands(peer)
        self.assertTrue(engines, "no command engine starts observed")
        for cmd in engines:
            start, until = cmd["start"], cmd["busy_until"]
            during = [(now, ce) for now, ce in peer.status_reads
                      if start <= now < until]
            self.assertTrue(during, "no status read during command")
            self.assertEqual(during[0][1], 1, "CE not asserted")
            self.assertTrue(any(now >= until and ce == 0
                                for now, ce in peer.status_reads),
                            "command completion never observed")

    def check_atlases(self, peer):
        addresses = [addr for _, addr in peer.addr_writes]
        self.assertIn(0x10000, addresses)
        self.assertIn(0x14000, addresses)
        for scanline in range(64):
            base = 0x10000 + scanline * 256
            self.assertEqual(
                bytes(peer.vram[base:base + 96]),
                atlas_scanline(self.font, scanline, False),
                "normal atlas scanline %d mismatch" % scanline)
            self.assertEqual(peer.vram[base + 96], 0x00,
                             "atlas pitch overrun at scanline %d" % scanline)
            base = 0x14000 + scanline * 256
            self.assertEqual(
                bytes(peer.vram[base:base + 96]),
                atlas_scanline(self.font, scanline, True),
                "reverse atlas scanline %d mismatch" % scanline)

    def check_cursor(self, peer, z):
        """Pattern stores, then the sprite cursor SAT derived from the
        module's live terminal state (the renderer keeps it current)."""
        self.assertEqual(bytes(peer.vram[0x1F800:0x1F808]),
                         b"\xe0" * 8)
        self.assertEqual(bytes(peer.vram[0x1F000:0x1F010]),
                         b"\x0b" * 16)
        dev = self.device_by_class(z, ZDEV_VIDEO)
        state = dev["state"]
        col = z.rd(state + 11)
        row = z.rd(state + 12)
        visible = z.rd(state + 13)
        origin = z.rd(state + 3)
        sat = bytes(peer.vram[0x1F200:0x1F208])
        if visible:
            self.assertEqual(sat[0], (((row * 8) + origin) - 1) & 0xFF,
                             "visible cursor Y")
        else:
            self.assertEqual(sat[0], 0xFF, "hidden cursor terminator")
        self.assertEqual(sat[1], (col * 3) & 0xFF, "cursor X")
        self.assertEqual(sat[2:4], b"\x00\x00")
        self.assertEqual(sat[4:8], bytes((0xD8, 0x00, 0x00, 0x00)))
        # 128-KiB addressing: the pattern is NOT aliased into the first
        # 64 KiB (F800h was cleared to the background and stays there).
        self.assertEqual(peer.vram[0xF800], 0x44)


class TestV9958Boot(V9958SimBase):
    def test_registration_bootstrap_and_assets(self):
        peer = V9958Peer()
        z = self.boot_to(peer, b"video: video0 OK\r\n")
        tx = bytes(z.sio_tx)
        self.assertNotIn(b"v9958: unavailable", tx)

        dev = self.device_by_class(z, ZDEV_VIDEO)
        self.assertIsNotNone(dev, "v9958.0 is not registered")
        self.assertEqual(self.cstr(z, dev["name"]), b"v9958.0")
        self.assertEqual(dev["class"], ZDEV_VIDEO)
        self.assertEqual(dev["caps"], CONSOLE_CAPS)
        self.assertNotEqual(dev["char_ops"], 0,
                            "the console renderer table must be published")
        putc = z.rd16(dev["char_ops"])
        write = z.rd16(dev["char_ops"] + 2)
        poll = z.rd16(dev["char_ops"] + 4)
        getc = z.rd16(dev["char_ops"] + 6)
        self.assertNotEqual(putc, 0)
        self.assertNotEqual(write, 0)
        self.assertEqual((poll, getc), (0, 0),
                         "the renderer is output only")
        self.assertNotEqual(dev["class_ops"], 0)
        self.assertNotEqual(dev["state"], 0)

        # The console defaults select this console-capable output and
        # keep HID as the preferred input (the normal HID/V9958 pair).
        hid = self.device_by_class(z, ZDEV_HID)
        sio = self.device_by_class(z, ZDEV_SERIAL)
        self.assertIsNotNone(hid)
        self.assertIsNotNone(sio)
        self.assertEqual(z.rd(self.syms["_console_in_handle"]),
                         hid["handle"])
        self.assertEqual(z.rd(self.syms["_console_out_handle"]),
                         dev["handle"])

        self.check_bootstrap(peer)
        self.check_atlases(peer)
        self.check_cursor(peer, z)

        # Clear screen: HMMV over the full 256-line page zero in 44h.
        engines = engine_commands(peer)
        self.assertEqual(engines[0]["fill"], (0, 0, 0x200, 0x100, 0x44))
        self.assertEqual(bytes(peer.vram[0:0x10000]),
                         b"\x44" * 0x10000)
        self.assertEqual(peer.protocol_errors, [])

    def test_malformed_requests_perform_no_io(self):
        peer = V9958Peer()
        z = self.boot_to(peer, b"video: video0 OK\r\n")
        dev, target, state = self.ops_for(z)
        payload = self.put_scratch(z, b"\xcc" * 64)

        before = (len(peer.latch_writes), len(peer.reg_writes),
                  len(peer.addr_writes), len(peer.palette_writes),
                  len(peer.indirect_writes), len(peer.vram_writes),
                  len(peer.status_reads))

        cases = (
            (ZVIDEO_PRESENT, 0, 5, ZDEV_EINVAL),
            (ZVIDEO_RESET, 0, 1, ZDEV_EINVAL),
            (ZVIDEO_DATA_BLOCK, 0, 64, ZDEV_EINVAL),
            (ZVIDEO_DATA_BLOCK, payload, 0, ZDEV_EINVAL),
            (ZVIDEO_CTRL_WRITE, 0, 2, ZDEV_EINVAL),
            (ZVIDEO_PALETTE_WRITE, 0, 1, ZDEV_EINVAL),
            (0x40, 0, 0, ZDEV_ENOTSUP),
            (0x09, payload, 1, ZDEV_ENOTSUP),
        )
        for op, addr, length, want in cases:
            got = self.v9958_send(z, target, state, op, addr, length)
            self.assertEqual(got, want,
                             "op %02X len %d: got %d want %d"
                             % (op, length, got, want))
        self.assertEqual(
            self.v9958_send(z, target, 0, ZVIDEO_PRESENT, 0, 0),
            ZDEV_ENODEV)

        after = (len(peer.latch_writes), len(peer.reg_writes),
                 len(peer.addr_writes), len(peer.palette_writes),
                 len(peer.indirect_writes), len(peer.vram_writes),
                 len(peer.status_reads))
        self.assertEqual(before, after,
                         "malformed requests touched the VDP ports")

    def test_direct_operations_long_block_and_128k(self):
        peer = V9958Peer()
        z = self.boot_to(peer, b"video: video0 OK\r\n")
        dev, target, state = self.ops_for(z)

        # CTRL_WRITE streams register selects; DATA_BLOCK carries 4096
        # bytes to 18000h (R#14=6) -- a 16-bit payload far above the
        # historical 16-byte ceiling, in the second 64 KiB of VRAM.
        pair = self.put_scratch(z, bytes((0x06, 0x8E)))  # R#14 = 6
        self.assertEqual(self.v9958_send(z, target, state,
                                         ZVIDEO_CTRL_WRITE, pair, 2),
                         ZDEV_OK)
        pair = self.put_scratch(z, bytes((0x00, 0x40)))
        self.assertEqual(self.v9958_send(z, target, state,
                                         ZVIDEO_CTRL_WRITE, pair, 2),
                         ZDEV_OK)
        pattern = bytes(range(256)) * 16
        payload = self.put_scratch(z, pattern)
        self.assertEqual(self.v9958_send(z, target, state,
                                         ZVIDEO_DATA_BLOCK, payload,
                                         4096), ZDEV_OK)
        self.assertEqual(bytes(peer.vram[0x18000:0x19000]), pattern)
        # The 64-KiB view of the same offset was cleared and stays clear:
        # R#14 addressing distinguishes both halves of the 128 KiB.
        self.assertEqual(bytes(peer.vram[0x8000:0x8100]), b"\x44" * 256)

        # Single-byte DATA_WRITE continues at the auto-incremented
        # address.
        one = self.put_scratch(z, b"\x5a")
        self.assertEqual(self.v9958_send(z, target, state,
                                         ZVIDEO_DATA_WRITE, one, 1),
                         ZDEV_OK)
        self.assertEqual(peer.vram[0x19000], 0x5A)

        # PALETTE_WRITE streams one entry (two bytes).
        pair = self.put_scratch(z, bytes((0x03, 0x90)))  # R#16 = 3
        self.assertEqual(self.v9958_send(z, target, state,
                                         ZVIDEO_CTRL_WRITE, pair, 2),
                         ZDEV_OK)
        entry = self.put_scratch(z, bytes((0x11, 0x22)))
        self.assertEqual(self.v9958_send(z, target, state,
                                         ZVIDEO_PALETTE_WRITE, entry, 2),
                         ZDEV_OK)
        self.assertEqual(peer.palette[3], [0x11, 0x22])

        # INDIRECT_WRITE starts a HMMV fill; PRESENT waits for CE.
        pair = self.put_scratch(z, bytes((32, 0x91)))  # R#17 = 32
        self.assertEqual(self.v9958_send(z, target, state,
                                         ZVIDEO_CTRL_WRITE, pair, 2),
                         ZDEV_OK)
        cmd = bytearray(15)
        cmd[4] = 0x10
        cmd[6] = 0x10
        cmd[8] = 0x08
        cmd[10] = 0x08
        cmd[12] = 0xFF
        cmd[14] = 0xC0
        block = self.put_scratch(z, bytes(cmd))
        self.assertEqual(self.v9958_send(z, target, state,
                                         ZVIDEO_INDIRECT_WRITE, block, 15),
                         ZDEV_OK)
        self.assertEqual(self.v9958_send(z, target, state, ZVIDEO_PRESENT,
                                         0, 0), ZDEV_OK)
        self.assertTrue(any(cmd.get("fill") == (0x10, 0x10, 8, 8, 0xFF)
                            for cmd in engine_commands(peer)))
        for row in range(0x10, 0x18):
            self.assertEqual(bytes(peer.vram[row * 256 + 8:row * 256 + 12]),
                             b"\xff" * 4)
        self.assertGreaterEqual(
            sum(1 for _, ce in peer.status_reads if ce == 1), 1)


class TestV9958AcceptancePass(V9958SimBase):
    def test_reset_and_visual_pattern(self):
        peer = V9958Peer()
        z = self.boot_to(peer, b"video selftest: OK\r\n")
        tx = bytes(z.sio_tx)
        for marker in (b"video: video0 OK\r\n",
                       b"video: malformed rejected\r\n",
                       b"video: reset OK\r\n",
                       b"video: console v9958.0 OK\r\n",
                       b"video: fill presented\r\n",
                       b"video: pattern drawn\r\n",
                       b"video selftest: OK\r\n"):
            self.assertIn(marker, tx)
        self.assertEqual(self.result(z, "_ace_video_selftest_result"), 0,
                         "target recorded a failure: %s"
                         % tx[-160:])
        self.assertNotIn(b"FAIL", tx)

        # Boot init + RESET init: two porch cycles, two full clears.
        values = [value for _, value in peer.latch_writes]
        self.assertEqual(values, [0x02, 0x00, 0x02, 0x00])
        engines = engine_commands(peer)
        fills = [cmd["fill"] for cmd in engines if "fill" in cmd]
        self.assertEqual(fills,
                         [(0, 0, 0x200, 0x100, 0x44),
                          (0, 0, 0x200, 0x100, 0x44),
                          (0x40, 0x20, 0x40, 0x20, 0xFF)])
        # The boot console text is rendered through the module: 6x8
        # glyph copies between the two clears.
        glyphs = [cmd for cmd in engines if "fill" not in cmd]
        self.assertTrue(glyphs, "no glyph copies from the boot console")
        for cmd in glyphs:
            block = cmd["block"]
            self.assertEqual(cmd["code"], 0xD0)
            self.assertEqual(block[8] | (block[9] << 8), 6)
            self.assertEqual(block[10] | (block[11] << 8), 8)

        # White rectangle: 64x64 pixels at (64,32) -> bytes 32..63 of
        # scanlines 32..63.
        for row in range(32, 64):
            base = row * 256
            self.assertEqual(
                bytes(peer.vram[base + 32:base + 64]), b"\xff" * 32,
                "white fill row %d mismatch" % row)
        self.assertEqual(peer.vram[32 * 256 + 31], 0x44)
        self.assertEqual(peer.vram[64 * 256 + 32], 0x44)

        # Deterministic band at scanline 96: DATA_WRITE then DATA_BLOCK.
        self.assertEqual(peer.vram[0x6000], 0xFF)
        self.assertEqual(bytes(peer.vram[0x6001:0x6001 + 200]),
                         b"\xcc" * 200)
        self.assertEqual(peer.vram[0x60C9], 0x44)

        # Palette entry 12 changed to magenta; neighbours untouched.
        self.assertEqual(peer.palette[12], [0x77, 0x00])
        self.assertEqual(peer.palette[11], [0x74, 0x07])
        self.assertEqual(peer.palette[13], [0x67, 0x00])

        # Both atlases and the cursor were re-established by RESET.
        self.check_atlases(peer)
        self.check_cursor(peer, z)
        self.assertEqual(peer.protocol_errors, [])


class TestV9958FontFailure(V9958SimBase):
    IMAGE_DIR = "build-video-fontfail-selftest"
    IMAGE_NAME = "zephyros-video-fontfail-selftest.bin"

    def test_absent_device_and_isolation(self):
        peer = V9958Peer()
        z = self.boot_to(peer, b"video fontfail selftest: OK\r\n")
        tx = bytes(z.sio_tx)
        self.assertIn(b"v9958: unavailable 0x0B\r\n", tx)
        self.assertIn(b"video: absent after font failure\r\n", tx)
        self.assertNotIn(b"video: video0 OK", tx)

        self.assertIsNone(self.device_by_class(z, ZDEV_VIDEO),
                          "v9958.0 registered despite the font failure")
        hid = self.device_by_class(z, ZDEV_HID)
        sio = self.device_by_class(z, ZDEV_SERIAL)
        self.assertIsNotNone(hid, "HID disturbed by the V9958 failure")
        self.assertIsNotNone(sio, "SIO disturbed by the V9958 failure")
        self.assertEqual(z.rd(self.syms["_console_in_handle"]),
                         hid["handle"])
        self.assertEqual(z.rd(self.syms["_console_out_handle"]),
                         sio["handle"])
        # No console-output capability: the SIO fallback stays bound.
        self.assertIsNone(self.device_by_class(z, ZDEV_VIDEO))
        self.assertEqual(self.result(z, "_ace_video_selftest_result"), 0)

        # Fail-fast resource validation: no VDP port was touched at all.
        self.assertEqual(peer.latch_writes, [])
        self.assertEqual(peer.reg_writes, [])
        self.assertEqual(peer.addr_writes, [])
        self.assertEqual(peer.palette_writes, [])
        self.assertEqual(peer.indirect_writes, [])
        self.assertEqual(peer.vram_writes, [])
        self.assertEqual(peer.status_reads, [])
        self.assertEqual(peer.commands, [])


if __name__ == "__main__":
    unittest.main()
