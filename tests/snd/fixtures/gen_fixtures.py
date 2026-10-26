#!/usr/bin/env python3
"""Generate tests/snd/fixtures for the sound oracles.

Checked-in generator; the fixtures it writes are committed beside it and
the checked-in goldens pin the oracles' output.  Run from the repository
root:

    python3 tests/snd/fixtures/gen_fixtures.py

Sources:
  - the two ZTR songs are copied byte for byte from the Zephyr-80 host
    tree (../Zephyr-80/Code/HOST/SNTracker/songs/);
  - the ZVG fixtures are compiled from small VGM files written here with
    the Zephyr-80 compiler (../Zephyr-80/Code/HOST/VGMPlayer/tools/
    compile_vgm.py);
  - every malformed fixture is a one-byte (or one-cut) mutation of a good
    fixture, one per validation rule of ZTR-Format.md / the ZVGC header
    and opcode checks.  The oracle's outcome for each is pinned in
    tests/snd/golden/malformed.md.
"""

import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
Z80 = os.path.normpath(os.path.join(ROOT, "..", "Zephyr-80"))
SONGS = os.path.join(Z80, "Code", "HOST", "SNTracker", "songs")
COMPILER = os.path.join(Z80, "Code", "HOST", "VGMPlayer", "tools",
                        "compile_vgm.py")

FIXTURES = os.path.join(HERE, "fixtures" if False else "")
MALFORMED = os.path.join(HERE, "malformed")

ZTR_SONGS = ("night-market.ztr", "RunawayCircuit.ztr")
ZVG_SONGS = ("run-shortwaits.zvg", "wait16.zvg")


def u16(data, offset):
    return data[offset] | (data[offset + 1] << 8)


def write(path, data):
    with open(path, "wb") as handle:
        handle.write(bytes(data))


def build_vgm(events, end_sample, rate=60, sn_clock=3579545):
    """A minimal VGM 1.50 file: (sample, value) SN76489 writes, a 66h end."""
    data_offset = 0x40
    body = bytearray()
    position = 0
    for sample, value in events:
        if sample < position:
            raise ValueError("samples not monotonic")
        wait = sample - position
        while wait:
            chunk = min(wait, 0xFFFF)
            body.append(0x61)
            body += struct.pack("<H", chunk)
            wait -= chunk
        body.append(0x50)
        body.append(value & 0xFF)
        position = sample
    wait = end_sample - position
    while wait:
        chunk = min(wait, 0xFFFF)
        body.append(0x61)
        body += struct.pack("<H", chunk)
        wait -= chunk
    body.append(0x66)
    header = bytearray(data_offset)
    header[0:4] = b"Vgm "
    header[0x04:0x08] = struct.pack("<I", data_offset + len(body))
    header[0x08:0x0C] = struct.pack("<I", 0x150)
    header[0x0C:0x10] = struct.pack("<I", sn_clock)
    header[0x24:0x28] = struct.pack("<I", rate)
    header[0x34:0x38] = struct.pack("<I", 0x0C)
    return bytes(header + body)


def compile_zvg(vgm_bytes, output):
    """Run the Zephyr-80 compiler on a staged VGM (it takes file paths)."""
    temp = output + ".vgm"
    write(temp, vgm_bytes)
    subprocess.run([sys.executable, COMPILER, temp, output], check=True)
    os.unlink(temp)


def good_song_events():
    """(sample, value) events for the two good ZVGs (rate 60, 735
    samples per tick)."""
    tick = 735
    short = []
    # A burst of three writes at tick 0 (one write run), short waits,
    # more bursts and single writes, ending around tick 120.
    short.append((0 * tick, 0x9F))
    short.append((0 * tick, 0xBF))
    short.append((0 * tick, 0xDF))
    short.append((5 * tick, 0x80))
    short.append((5 * tick, 0x0E))
    short.append((9 * tick, 0x90))
    short.append((9 * tick, 0x1F))
    short.append((13 * tick, 0x92))
    short.append((21 * tick, 0x93))
    short.append((37 * tick, 0x94))
    short.append((53 * tick, 0x95))
    short.append((60 * tick, 0x9F))
    short.append((60 * tick, 0x96))
    short.append((64 * tick, 0x97))
    short.append((68 * tick, 0x98))
    short.append((85 * tick, 0x99))
    short.append((101 * tick, 0x9A))
    short.append((101 * tick, 0x9B))
    short.append((101 * tick, 0x9C))
    short.append((101 * tick, 0x9D))
    short.append((117 * tick, 0x9E))
    short_end = 130 * tick
    long_ = []
    # A few writes with long gaps (100 ticks each: WAIT16), ending at
    # tick 300.
    long_.append((0 * tick, 0x9F))
    long_.append((0 * tick, 0xBF))
    long_.append((100 * tick, 0x90))
    long_.append((100 * tick, 0x1F))
    long_.append((200 * tick, 0x80))
    long_.append((200 * tick, 0x0E))
    long_.append((200 * tick, 0x8F))
    long_.append((300 * tick, 0x9F))
    long_end = 310 * tick
    return short, short_end, long_, long_end


def ztr_mutations(data):
    """name, mutated for every ZTR validation rule in ztr.c."""
    out = []
    n = len(data)
    string_end = 64 + u16(data, 48)
    note_offset = u16(data, 44)
    pattern_length = u16(data, 12)

    def add(name, blob):
        out.append((name, blob))

    def edit(name, offset, value):
        blob = bytearray(data)
        blob[offset] = value
        add(name, bytes(blob))

    add("header-truncated", data[:32])
    edit("header-magic", 0, ord("X"))
    edit("header-version", 4, 2)
    edit("header-size", 5, 32)
    edit("header-flags", 6, 1)
    edit("header-channels", 7, 8)
    edit("header-reserved", 63, 1)
    edit("values-tick-rate-zero", 8, 0)
    edit("values-tick-rate-high", 8, 181)
    edit("values-rate-high-byte", 9, 1)
    edit("values-speed-zero", 10, 0)
    edit("values-source-channels", 11, 17)
    edit("values-pattern-length-zero", 12, 0)
    blob = bytearray(data)
    blob[13] = 1
    add("values-pattern-length-high", bytes(blob))
    edit("values-order-count-zero", 14, 0)
    edit("values-instrument-count", 18, 17)
    edit("values-reserved", 22, 1)
    edit("values-clock", 24, 1)
    edit("values-file-size", 46, 0)
    edit("values-entry-sizes", 50, 8)
    blob = bytearray(data)
    blob[48:50] = struct.pack("<H", 0xFFFF)
    add("sections-overlap", bytes(blob))
    blob = bytearray(data)
    blob[28:30] = struct.pack("<H", string_end)
    add("strings-title", bytes(blob))
    instrument_offset = u16(data, 36)
    instrument_count = data[18]
    blob = bytearray(data)
    blob[instrument_offset + 16] = blob[instrument_offset]
    add("instruments-dup", bytes(blob))
    first_macro = u16(data, instrument_offset + 4)
    assert first_macro, "first instrument has no volume macro"
    blob = bytearray(data)
    blob[first_macro + 10:first_macro + 12] = b"\x00\x00"
    add("instruments-macro", bytes(blob))
    directory_offset = u16(data, 40)
    pattern_count = u16(data, 16)
    assert pattern_count > 0
    # The mutations below edit a stream, so they target the first
    # directory entry with a non-empty pattern (an empty pattern
    # consumes no event bytes).
    entry = None
    for index in range(pattern_count):
        candidate = directory_offset + index * 8
        if u16(data, candidate + 4) != 0:
            entry = candidate
            break
    assert entry is not None, "no non-empty pattern"
    pattern_offset = u16(data, entry + 2)

    blob = bytearray(data)
    blob[entry] = 16
    add("patterns-channel", bytes(blob))
    blob = bytearray(data)
    blob[entry + 2:entry + 4] = struct.pack("<H", 0)
    add("patterns-offset-low", bytes(blob))
    blob = bytearray(data)
    blob[entry + 2:entry + 4] = struct.pack("<H", note_offset)
    add("patterns-offset-high", bytes(blob))
    blob = bytearray(data)
    blob[entry + 4:entry + 6] = struct.pack("<H", 1)
    add("patterns-truncated", bytes(blob))
    blob = bytearray(data)
    blob[entry + 6:entry + 8] = struct.pack("<H",
                                            u16(data, entry + 6) + 1)
    add("patterns-event-count", bytes(blob))
    blob = bytearray(data)
    blob[pattern_offset] = pattern_length & 0xFF
    add("patterns-row", bytes(blob))
    blob = bytearray(data)
    blob[pattern_offset + 1] = 0x00
    add("patterns-mask", bytes(blob))
    blob = bytearray(data)
    blob[pattern_offset + 1] = 0x80
    blob[pattern_offset + 2] = 96
    add("patterns-note", bytes(blob))
    blob = bytearray(data)
    blob[pattern_offset + 1] = 0x40
    blob[pattern_offset + 2] = 16
    add("patterns-instrument", bytes(blob))
    blob = bytearray(data)
    blob[pattern_offset + 1] = 0x20
    blob[pattern_offset + 2] = 16
    add("patterns-volume", bytes(blob))
    blob = bytearray(data)
    blob[pattern_offset + 1] = 0x10
    blob[pattern_offset + 2] = 0
    add("patterns-effect", bytes(blob))
    blob = bytearray(data)
    blob[pattern_offset + 1] = 0x0C
    add("patterns-off-release", bytes(blob))
    blob = bytearray(data)
    blob[pattern_offset + 1] = 0x02
    add("patterns-legato", bytes(blob))
    blob = bytearray(data)
    blob[pattern_offset + 1] = 0x01
    blob[pattern_offset + 2] = 0x04
    add("patterns-modifiers-reserved", bytes(blob))
    blob = bytearray(data)
    blob[pattern_offset + 1] = 0x01
    blob[pattern_offset + 2] = 0x60
    add("patterns-modifiers-arp-conflict", bytes(blob))
    blob = bytearray(data)
    blob[pattern_offset + 1] = 0x01
    blob[pattern_offset + 2] = 0x18
    add("patterns-modifiers-hairpin-conflict", bytes(blob))
    blob = bytearray(data)
    blob[note_offset + 95 * 2:note_offset + 95 * 2 + 2] = b"\x00\x00"
    add("note-table-divisor", bytes(blob))
    padding = (128 - (n % 128)) % 128
    assert padding > 0, "song size is already record-aligned"
    blob = bytearray(data) + b"\x1a" * (padding - 1) + b"\x00"
    add("padding-not-eof", bytes(blob))
    add("padding-wrong-size", bytes(bytearray(data) + b"\x1a" * 16))
    return out


def zvg_mutations(data):
    """name, mutated for every ZVGC header and opcode check."""
    out = []
    n = len(data)
    stream = data[16:]

    def add(name, blob):
        out.append((name, blob))

    def edit(name, offset, value):
        blob = bytearray(data)
        blob[offset] = value
        add(name, bytes(blob))

    add("header-truncated", data[:8])
    edit("header-magic", 0, ord("X"))
    edit("header-version", 4, 3)
    edit("header-rate-zero", 5, 0)
    edit("header-rate-high", 5, 181)
    edit("header-length", 7, 8)
    blob = bytearray(data)
    blob[8:12] = b"\x00\x00\x00\x00"
    add("header-declared-zero", bytes(blob))
    blob = bytearray(data)
    blob[8:12] = struct.pack("<I", n - 16 + 1)
    add("header-declared-long", bytes(blob))
    blob = bytearray(data)
    blob[16] = 0x03
    add("stream-bad-opcode", bytes(blob))
    # WAIT16 with a zero tick count: a WRITE then 02h 00 00.
    small = bytearray(data[:16])
    small[8:12] = struct.pack("<I", 6)
    small += b"\x01\x9f\x02\x00\x00\x00"
    add("stream-wait16-zero", bytes(small))
    # The tail of a WAIT16 cut after one byte: a complete stream whose
    # last command is missing its second byte.  Derived only from the
    # fixture that contains a WAIT16.
    cut = None
    for offset in range(len(stream) - 2):
        if stream[offset] == 0x02:
            cut = offset
            break
    if cut is not None:
        kept = 16 + cut + 2
        blob = bytearray(data[:kept])
        blob[8:12] = struct.pack("<I", cut + 2)
        add("stream-truncated-command", bytes(blob))
    return out


def wrap_song_events():
    """(sample, value) events for wraps.zvg: 600 ticks at 60 Hz, each a
    tone latch+data pair and a volume write, every 4th a noise pair -
    3,317 stream bytes, so the player's 256-byte ring wraps a dozen
    times with commands straddling its end."""
    tick = 735
    events = []
    for f in range(600):
        ch = f % 3
        period = 200 + (f * 7) % 600
        at = f * tick
        events.append((at, 0x80 | (ch << 5) | (period & 0x0F)))
        events.append((at, (period >> 4) & 0x3F))
        events.append((at, 0x90 | (ch << 5) | (f % 16)))
        if f % 4 == 0:
            events.append((at, 0xE4 | (f // 4) % 3))
            events.append((at, 0xF0 | ((f // 4) % 16)))
    return events, 600 * tick


def main():
    os.makedirs(FIXTURES, exist_ok=True)
    os.makedirs(MALFORMED, exist_ok=True)
    for name in ZTR_SONGS:
        source = os.path.join(SONGS, name)
        shutil.copyfile(source, os.path.join(FIXTURES, name))
        with open(source, "rb") as handle:
            data = handle.read()
        for bad, blob in ztr_mutations(data):
            write(os.path.join(MALFORMED, bad + ".ztr"), blob)

    short, short_end, long_, long_end = good_song_events()
    compile_zvg(build_vgm(short, short_end), os.path.join(FIXTURES,
                                                          ZVG_SONGS[0]))
    compile_zvg(build_vgm(long_, long_end), os.path.join(FIXTURES,
                                                         ZVG_SONGS[1]))
    wrap, wrap_end = wrap_song_events()
    compile_zvg(build_vgm(wrap, wrap_end), os.path.join(FIXTURES,
                                                        "wraps.zvg"))
    for name in ZVG_SONGS:
        with open(os.path.join(FIXTURES, name), "rb") as handle:
            data = handle.read()
        for bad, blob in zvg_mutations(data):
            write(os.path.join(MALFORMED, bad + ".zvg"), blob)
    print("fixtures: %s and %s" % (FIXTURES, MALFORMED))


if __name__ == "__main__":
    main()
