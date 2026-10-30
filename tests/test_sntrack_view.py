#!/usr/bin/env python3
"""SN-Track's static ZTR view (docs/sntrack-ui.md) on the production image.

A reference renderer draws each expected page from the song's bytes, and
the peer's VRAM must equal it pixel for pixel.
"""

import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from replay import SHELL_PROMPT  # noqa: E402
from test_gfx import (FRAME_TSTATES, GfxReplay, KEEP_CONTENT, KEEP_NAME,
                      OPS_ACQUIRE, RETRACE_TSTATES, load_font4)  # noqa: E402

SNTVIEW = os.path.join(ROOT, "UserSoftware", "SNTracker", "build",
                       "user", "sntview.exe")
SONGS = os.path.join(ROOT, "UserSoftware", "SNTracker", "songs")
LIMIT = 200_000_000

PALETTE = ((0, 0, 1), (1, 1, 2), (2, 2, 3), (4, 4, 5),
           (6, 6, 6), (5, 5, 7), (3, 5, 5), (6, 5, 3),
           (2, 1, 4), (3, 2, 5), (4, 3, 6), (5, 5, 6),
           (7, 7, 7), (6, 6, 7), (5, 7, 6), (7, 6, 4))
TEXT_X = (14, 50, 86, 124, 138, 174, 210, 248,
          262, 298, 334, 372, 386, 422, 458, 496)
DOT = "\xFA"  # CP850 middle dot: an empty character slot
NOTES = ("C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-",
         "A#", "B-")


# -- the document --------------------------------------------------------

def parse_ztr(data):
    """The fields the view shows, read as docs/ZTR-Format.md states."""
    u16 = lambda o: struct.unpack_from("<H", data, o)[0]  # noqa: E731

    def string(o):
        return data[o:data.index(0, o)].decode("latin-1")

    def macro_first(o):
        if o == 0:
            return None
        flags, value = data[o + 12], struct.unpack_from("<h", data, o + 13)[0]
        return {"flags": flags, "value": value, "delay": data[o + 4]}

    song = {"title": string(u16(0x1C)), "rate": u16(0x08),
            "speed": data[0x0A], "rows": u16(0x0C),
            "clock": struct.unpack_from("<I", data, 0x18)[0]}
    orders_at = u16(0x22)
    song["orders"] = [list(data[orders_at + 16 * i:orders_at + 16 * i + 16])
                      for i in range(u16(0x0E))]
    instruments = []
    for i in range(u16(0x12)):
        e = u16(0x24) + 16 * i
        instruments.append({"number": data[e], "name": string(u16(e + 2)),
                            "arp": macro_first(u16(e + 8)),
                            "duty": macro_first(u16(e + 10)),
                            "type": "M"})
    by_number = {ins["number"]: ins for ins in instruments}
    patterns = {}
    for i in range(u16(0x10)):
        channel, number, at, length, _count = struct.unpack_from(
            "<BBHHH", data, u16(0x28) + 8 * i)
        events = {}
        end = at + length
        while at < end:
            row, mask = data[at], data[at + 1]
            at += 2
            event = {"mask": mask}
            for bit, name in ((0x80, "note"), (0x40, "ins"), (0x20, "vol")):
                if mask & bit:
                    event[name] = data[at]
                    at += 1
            if mask & 0x10:
                event["eff"] = (data[at], data[at + 1])
                at += 2
            if mask & 0x01:
                flags = data[at]
                at += 1 + (1 if flags & 0x40 else 0) + \
                    (2 if flags & 0x10 else 0)
            events[row] = event
            if "ins" in event:
                by_number[event["ins"]]["type"] = \
                    "N" if channel & 3 == 3 else "T"
        patterns[(channel, number)] = events
    song["instruments"] = instruments
    song["by_number"] = by_number
    song["patterns"] = patterns
    return song


def tone_cell(event):
    """`NNNIIVEEE` with each field's colour (section 8.3)."""
    cells = [(DOT, 2)] * 9
    mask = event["mask"]
    if mask & 0x0C:
        cells[0:3] = [("=", 4)] * 3
    elif mask & 0x80:
        note = event["note"]
        text = NOTES[note % 12] + str(note // 12)
        cells[0:3] = [(c, 4) for c in text]
    if "ins" in event:
        cells[3:5] = [(c, 5) for c in "%02X" % event["ins"]]
    if "vol" in event:
        cells[5] = ("%X" % event["vol"], 6)
    if "eff" in event:
        cells[6:9] = [(c, 7) for c in "%X%02X" % event["eff"]]
    return cells


def noise_control(song, event):
    """Section 8.4's N, or None."""
    if "note" not in event or "ins" not in event:
        return None
    ins = song["by_number"][event["ins"]]
    arp, duty = ins["arp"], ins["duty"]
    if (arp and arp["delay"]) or (duty and duty["delay"]):
        return None
    note = event["note"]
    if arp:
        note = arp["value"] if arp["flags"] & 1 else note + arp["value"]
    note = max(0, min(95, note))
    white = duty["value"] & 1 if duty else 0
    return (white << 2) | (note & 3)


def noise_cell(song, event):
    cells = [(DOT, 2)] * 3
    control = noise_control(song, event)
    if control is not None:
        cells[0] = ("%X" % control, 4)
    if "ins" in event:
        cells[1] = ("%X" % event["ins"], 5)
    if "vol" in event:
        cells[2] = ("%X" % event["vol"], 6)
    return cells


# -- the reference renderer ----------------------------------------------

class Page:
    def __init__(self, font):
        self.font = font
        self.px = [bytearray(512) for _ in range(212)]

    def fill(self, x, y, w, h, colour):
        for row in range(y, y + h):
            self.px[row][x:x + w] = bytes([colour]) * w

    def xor(self, x, y, w, h):
        for row in range(y, y + h):
            for col in range(x, x + w):
                self.px[row][col] ^= 8

    def vline(self, x, y0, y1, colour):
        for row in range(y0, y1 + 1):
            self.px[row][x] = colour

    def text(self, x, y, text, fg, bg):
        self.cells(x, y, [(c, fg) for c in text], bg)

    def cells(self, x, y, cells, bg):
        for index, (char, fg) in enumerate(cells):
            glyph = self.font[ord(char) * 6:ord(char) * 6 + 6]
            for row in range(6):
                for bit in range(4):
                    on = glyph[row] & (0x80 >> bit)
                    self.px[y + row][x + index * 4 + bit] = fg if on else bg

    def g6(self, row):
        line = self.px[row]
        return bytes((line[i] << 4) | line[i + 1] for i in range(0, 512, 2))


def expected_page(font, song, order, first_row, order_top):
    """The page as view_draw_page draws it."""
    p = Page(font)
    rows = song["rows"]
    # Title bar.
    p.fill(0, 0, 512, 9, 1)
    p.text(0, 1, "SN-TRACK", 5, 1)
    p.text(38, 1, song["title"], 4, 1)
    p.text(180, 1, "4 X SN76489", 3, 1)
    mhz = (song["clock"] + 5000) // 10000
    p.text(268, 1, "%u.%02uMHZ" % (mhz // 100, mhz % 100), 4, 1)
    p.text(320, 1, "%uHZ" % song["rate"], 4, 1)
    p.text(356, 1, "STOP", 7, 1)
    p.text(492, 1, "00:00", 4, 1)
    # Orders.
    p.fill(0, 9, 216, 35, 1)
    p.text(0, 10, "POS A1 A2 A3 AN B1 B2 B3 BN C1 C2 C3 CN D1 D2 D3 DN",
           3, 1)
    for line in range(4):
        number = order_top + line
        if number >= len(song["orders"]):
            continue
        y = 19 + line * 6
        p.text(0, y, "%02X " % number, 4, 1)
        for ch, ref in enumerate(song["orders"][number]):
            if ref != 0xFF:
                p.text(12 + ch * 12, y, "%02X" % ref, 4, 1)
        if number == order:
            p.xor(0, y, 204, 6)
    # Instruments.
    p.fill(216, 9, 130, 35, 1)
    p.text(218, 10, "INSTRUMENT", 3, 1)
    for line, ins in enumerate(song["instruments"][:4]):
        y = 19 + line * 6
        text = ("%02X %s" % (ins["number"], ins["name"][:15])).ljust(18)
        p.text(218, y, text + ins["type"], 7 if ins["type"] == "M" else 4, 1)
        if ins["number"] == 0:
            p.xor(216, y, 130, 6)
    # Song.
    p.fill(346, 9, 166, 35, 1)
    p.text(348, 10, "SONG", 3, 1)
    p.text(348, 16, "TITLE", 3, 1)
    p.text(372, 16, song["title"], 4, 1)
    p.text(348, 22, "SPEED", 3, 1)
    p.text(372, 22, "%02X" % song["speed"], 4, 1)
    p.text(390, 22, "TEMPO", 3, 1)
    tempo = (15 * song["rate"] + song["speed"] // 2) // song["speed"]
    p.text(414, 22, "%u" % tempo, 4, 1)
    p.text(348, 28, "ROWS", 3, 1)
    p.text(368, 28, "%02X" % rows, 4, 1)
    p.text(390, 28, "HILITE", 3, 1)
    p.text(418, 28, "04/10", 4, 1)
    p.text(348, 34, "POS", 3, 1)
    p.text(364, 34, "%02X/%02X" % (order, len(song["orders"]) & 0xFF), 4, 1)
    p.text(390, 34, "PAT", 3, 1)
    # Channel headers.
    refs = song["orders"][order]
    p.fill(0, 44, 512, 11, 0)
    for ch in range(16):
        name = "ABCD"[ch >> 2] + ("N" if ch & 3 == 3 else str((ch & 3) + 1))
        p.text(TEXT_X[ch], 46, name, 4, 0)
        if refs[ch] == 0xFF:
            p.text(TEXT_X[ch] + (8 if ch & 3 == 3 else 32), 46, "M", 7, 0)
    # Pattern rows.
    for i in range(25):
        row = first_row + i
        y = 55 + i * 6
        bg = 1 if row < rows and row % 4 == 0 else 0
        p.fill(0, y, 512, 6, bg)
        if row >= rows:
            continue
        p.text(0, y, "%02X" % row, 5 if row % 16 == 0 else 3, bg)
        for chip in range(4):
            p.text(TEXT_X[chip * 4], y, DOT * 27, 2, bg)
            p.text(TEXT_X[chip * 4 + 3], y, DOT * 3, 2, bg)
        for ch in range(16):
            if refs[ch] == 0xFF:
                continue
            event = song["patterns"][(ch, refs[ch])].get(row)
            if event is None:
                continue
            cells = noise_cell(song, event) if ch & 3 == 3 \
                else tone_cell(event)
            p.cells(TEXT_X[ch], y, cells, bg)
    # Rules.
    for chip in range(5):
        p.vline(12 + chip * 124, 44, 204, 2)
    for chip in range(4):
        for offset in (37, 73, 110):
            p.vline(12 + chip * 124 + offset, 44, 204, 2)
    # Status line.
    p.fill(0, 205, 512, 7, 1)
    p.text(0, 205, "OCT 4  STEP 1  INS 00  ROW %02X/%02X  FOCUS CHIP A  "
           "F1 HELP" % (first_row, (rows - 1) & 0xFF), 4, 1)
    # The play cursor on the page's first row.
    p.xor(0, 55, 508, 6)
    return p


# -- synthetic songs -----------------------------------------------------

def event_bytes(row, note=None, ins=None, vol=None, eff=None, off=False,
                release=False, legato=False, mods=None):
    mask = ((0x80 if note is not None else 0) |
            (0x40 if ins is not None else 0) |
            (0x20 if vol is not None else 0) |
            (0x10 if eff is not None else 0) |
            (0x08 if off else 0) | (0x04 if release else 0) |
            (0x02 if legato else 0) | (0x01 if mods else 0))
    out = bytearray((row, mask))
    for value in (note, ins, vol):
        if value is not None:
            out.append(value)
    if eff is not None:
        out += bytes(eff)
    if mods:
        out += bytes(mods)
    return bytes(out)


def build_ztr(title, rows, orders, instruments, patterns, rate=60,
              speed=6):
    """A valid ZTR v1 file. instruments: (number, name, arp, duty) with
    each macro None or (flags, value, delay); patterns: {(channel,
    number): [event bytes]}."""
    pool = bytearray(b"\0")

    def intern(text):
        at = 64 + len(pool)
        pool.extend(text.encode("latin-1") + b"\0")
        return at

    title_at = intern(title)
    names = [intern(ins[1]) for ins in instruments]
    orders_at = 64 + len(pool)
    instruments_at = orders_at + 16 * len(orders)
    macros_at = instruments_at + 16 * len(instruments)
    macros = bytearray()
    directory = bytearray()
    for (number, _name, arp, duty), name_at in zip(instruments, names):
        offsets = []
        for kind, macro in ((3, arp), (4, duty)):
            if macro is None:
                offsets.append(0)
                continue
            offsets.append(macros_at + len(macros))
            flags, value, delay = macro
            macros += struct.pack("<BBBBBBHHH", kind, 0, 0, 1, delay, 0,
                                  0xFFFF, 0xFFFF, 1)
            macros += struct.pack("<Bh", flags, value)
        directory += struct.pack("<BBHHHHHHBB", number, 0, name_at, 0, 0,
                                 offsets[0], offsets[1], 0, 0, 0)
    patterns_at = macros_at + len(macros)
    data_at = patterns_at + 8 * len(patterns)
    pattern_dir = bytearray()
    stream_data = bytearray()
    for (channel, number), events in sorted(patterns.items()):
        stream = b"".join(events)
        pattern_dir += struct.pack("<BBHHH", channel, number,
                                   data_at + len(stream_data), len(stream),
                                   len(events))
        stream_data += stream
    notes_at = data_at + len(stream_data)
    size = notes_at + 192
    header = bytearray(64)
    header[0:4] = b"ZTR1"
    header[4:8] = bytes((1, 0x40, 0, 16))
    struct.pack_into("<HBBHHHHH", header, 8, rate, speed, 16, rows,
                     len(orders), len(patterns), len(instruments), 440)
    struct.pack_into("<I", header, 0x18, 3579545)
    struct.pack_into("<HHHHHHHHHHH", header, 0x1C, title_at, 64, 64,
                     orders_at, instruments_at, macros_at, patterns_at,
                     data_at, notes_at, size, len(pool))
    header[0x32:0x36] = bytes((16, 16, 8, 96))
    body = (bytes(header) + bytes(pool) +
            b"".join(bytes(order) for order in orders) + bytes(directory) +
            bytes(macros) + bytes(pattern_dir) + bytes(stream_data) +
            struct.pack("<96H", *([254] * 96)))
    assert len(body) == size
    return body


def fields_song():
    """Every field, in 64-row patterns (pages 00, 19h and 32h)."""
    tone = [
        event_bytes(0x00, note=48, ins=0, vol=15, eff=(1, 0x37)),
        event_bytes(0x01, off=True),
        event_bytes(0x02, release=True),
        event_bytes(0x03, note=39, legato=True),
        event_bytes(0x04, eff=(2, 0xA0)),
        event_bytes(0x05, mods=(0x40, 0x37)),
        event_bytes(0x06, vol=3, mods=(0x10, 3, 12)),
        event_bytes(0x07, eff=(3, 0x01)),
        event_bytes(0x08, eff=(4, 0x02), mods=(0x80,)),
        event_bytes(0x09, eff=(5, 0x12), mods=(0x20,)),
        event_bytes(0x0A, ins=0, eff=(6, 0x00), mods=(0x08,)),
        event_bytes(0x10, note=95, vol=0),
        event_bytes(0x1A, note=0, ins=0),
        event_bytes(0x33, note=60, vol=8),
        event_bytes(0x3F, off=True),
    ]
    noise = [
        event_bytes(0x00, note=48, ins=1, vol=9),     # white, 48+1 -> 5
        event_bytes(0x01, note=50, ins=3),            # fixed 2 -> 2
        event_bytes(0x02, note=5, ins=4, vol=1),      # duty delay: no N
        event_bytes(0x03, note=7),                    # no instrument
        event_bytes(0x04, vol=12),
        event_bytes(0x20, note=11, ins=1),            # white, 12 -> 4
    ]
    second = [event_bytes(0x00, note=24, ins=5, vol=4),
              event_bytes(0x1B, note=25)]
    orders = []
    for index in range(6):
        order = [0xFF] * 16
        order[0] = 0
        order[3] = 0
        order[5] = index & 1
        orders.append(order)
    instruments = [(0, "TONE LEAD", None, None),
                   (1, "WHITE HAT", (0, 1, 0), (0, 1, 0)),
                   (2, "SPARE", None, None),
                   (3, "FIXED KICK", (1, 2, 0), None),
                   (4, "LATE SNARE", None, (0, 1, 2)),
                   (5, "BASS", None, None)]
    patterns = {(0, 0): tone, (3, 0): noise,
                (5, 0): second, (5, 1): second[:1]}
    return build_ztr("FIELD TEST", 64, orders, instruments, patterns)


def mixed_song():
    """Instrument 0 on a tone and a noise channel."""
    order = [0xFF] * 16
    order[0] = order[3] = 0
    return build_ztr("MIXED", 32, [order], [(0, "BOTH", None, None)],
                     {(0, 0): [event_bytes(0, note=48, ins=0)],
                      (3, 0): [event_bytes(0, note=48, ins=0)]})


def wide_song():
    """Three 400-byte streams in one order: over 1,024 bytes."""
    events = [event_bytes(row, note=48, ins=0, vol=15, eff=(2, 0x10))
              for row in range(57)]
    order = [0xFF] * 16
    order[0] = order[1] = order[2] = 0
    return build_ztr("WIDE", 64, [order], [(0, "LEAD", None, None)],
                     {(ch, 0): events for ch in range(3)})


# -- the replay ----------------------------------------------------------

@unittest.skipUnless(os.path.exists(SNTVIEW),
                     "run make -C UserSoftware/SNTracker")
class TestSNTrackView(GfxReplay):
    def setUp(self):
        super().setUp()
        self.font = load_font4()

    def boot(self, songs):
        with open(SNTVIEW, "rb") as handle:
            files = {"/".join(KEEP_NAME): KEEP_CONTENT,
                     "SNTVIEW.EXE": handle.read()}
        files.update(songs)
        z = self.start(files)
        z.vdp.frame_period = FRAME_TSTATES   # the retrace runs
        return z

    def song(self, name):
        with open(os.path.join(SONGS, name + ".ztr"), "rb") as handle:
            return handle.read()

    def launch(self, z, ops, command):
        """Run the command; return once the first page is drawn and the
        display is enabled again."""
        seen_off = []
        z.hid.hid_queue.extend(command)

        def displayed(z):
            if OPS_ACQUIRE not in ops:
                return False
            if z.vdp.regs[1] & 0x40 == 0:
                seen_off.append(True)
            return bool(seen_off) and z.vdp.regs[1] & 0x40 != 0
        self.run_recording(z, ops, displayed, limit=LIMIT)

    def turn(self, z, ops, page):
        z.hid.hid_queue.extend(b" ")
        self.run_recording(z, ops, lambda z: z.vdp.shown_page == page,
                           limit=LIMIT)
        _write_at, _value, clock = z.vdp.page2_writes[-1]
        self.assertGreaterEqual(clock, FRAME_TSTATES - RETRACE_TSTATES,
                                "the page switch landed in the retrace")

    def assert_page(self, z, page, song, order, first_row, order_top):
        expected = expected_page(self.font, song, order, first_row,
                                 order_top)
        base = page * 256 * 256
        for y in range(212):
            got = bytes(z.vdp.vram[base + y * 256:base + y * 256 + 256])
            want = expected.g6(y)
            if got != want:
                x = next(i for i in range(256) if got[i] != want[i]) * 2
                self.fail("page %u order %02X row %02X: y %u x %u: "
                          "VRAM %s, expected %s" %
                          (page, order, first_row, y, x,
                           got[x // 2:x // 2 + 8].hex(),
                           want[x // 2:x // 2 + 8].hex()))

    def quit(self, z, ops, since):
        z.hid.hid_queue.extend(b"q")
        self.run_recording(z, ops, lambda z:
                           SHELL_PROMPT in "".join(z.glyphs[since:]),
                           limit=LIMIT)

    def test_night_market_pages_palette_and_exit(self):
        data = self.song("night-market")
        song = parse_ztr(data)
        z = self.boot({"SONG.ZTR": data})
        before = self.snapshot(z)
        ops = []
        since = len(z.glyphs)
        self.launch(z, ops, b"/fs/sntview.exe /fs/song.ztr\r")
        self.assertEqual([list(p) for p in z.vdp.palette],
                         [[(r << 4) | b, g] for r, g, b in PALETTE])
        self.assertEqual(z.vdp.shown_page, 0)
        self.assert_page(z, 0, song, 0, 0x00, 0)
        self.turn(z, ops, 1)
        self.assert_page(z, 1, song, 0, 0x19, 0)
        self.turn(z, ops, 0)
        self.assert_page(z, 0, song, 1, 0x00, 0)
        self.quit(z, ops, since)
        self.assert_hold_sequence(ops)
        self.assert_restored(z, before)

    def test_runaway_circuit_order_four(self):
        data = self.song("RunawayCircuit")
        song = parse_ztr(data)
        self.assertEqual(sum(ins["type"] == "N"
                             for ins in song["instruments"]), 3)
        z = self.boot({"SONG.ZTR": data})
        ops = []
        self.launch(z, ops, b"/fs/sntview.exe /fs/song.ztr 4\r")
        self.assert_page(z, 0, song, 4, 0x00, 4)

    def test_fields_pages_and_order_window(self):
        data = fields_song()
        song = parse_ztr(data)
        self.assertEqual([ins["type"] for ins in song["instruments"]],
                         ["T", "N", "M", "N", "N", "T"])
        z = self.boot({"SONG.ZTR": data})
        ops = []
        self.launch(z, ops, b"/fs/sntview.exe /fs/song.ztr 3\r")
        self.assert_page(z, 0, song, 3, 0x00, 0)
        self.turn(z, ops, 1)
        self.assert_page(z, 1, song, 3, 0x19, 0)
        self.turn(z, ops, 0)
        self.assert_page(z, 0, song, 3, 0x32, 0)
        # Order 4 leaves the window 0-3: the window starts at 4.
        self.turn(z, ops, 1)
        self.assert_page(z, 1, song, 4, 0x00, 4)

    def test_refusals_print_and_never_open_the_display(self):
        truncated = self.song("night-market")[:5000]
        z = self.boot({"MIXED.ZTR": mixed_song(), "WIDE.ZTR": wide_song(),
                       "SHORT.ZTR": truncated})
        for name, message in (
                (b"mixed", "an instrument plays tone and noise"),
                (b"wide", "an order exceeds 1024 stream bytes"),
                (b"short", "invalid ZTR song")):
            ops = []
            since = len(z.glyphs)
            z.hid.hid_queue.extend(b"/fs/sntview.exe /fs/" + name +
                                   b".ztr\r")

            def refused(z):
                text = "".join(z.glyphs[since:])
                return (message in text and
                        SHELL_PROMPT in text.split(message, 1)[1])
            self.run_recording(z, ops, refused, limit=LIMIT)
            self.assertNotIn(OPS_ACQUIRE, ops, name)


if __name__ == "__main__":
    unittest.main()
