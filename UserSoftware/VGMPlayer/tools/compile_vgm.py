#!/usr/bin/env python3
"""Compile an SN76489 VGM/VGZ file into VGMPlayer's compact ZVGC stream."""

from __future__ import annotations

import argparse
import gzip
import struct
from dataclasses import dataclass
from pathlib import Path


VGM_SAMPLE_RATE = 44100
MAGIC = b"ZVGC"
VERSION = 2
HEADER_SIZE = 16

OP_END = 0x00
OP_WRITE = 0x01
OP_WAIT16 = 0x02
OP_SHORT_BASE = 0x3F
OP_WRITE_RUN_BASE = 0x80
OP_WRITE_RUN_MAX = 16


class VgmError(ValueError):
    pass


@dataclass(frozen=True)
class ParsedVgm:
    events: list[tuple[int, int]]
    end_sample: int
    loop_sample: int | None
    loop_event: int | None
    sn_clock: int
    tick_rate: int


def u16(data: bytes, offset: int) -> int:
    if offset + 2 > len(data):
        raise VgmError("truncated 16-bit VGM field")
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    if offset + 4 > len(data):
        raise VgmError("truncated 32-bit VGM field")
    return struct.unpack_from("<I", data, offset)[0]


def need(data: bytes, offset: int, count: int, command: int) -> None:
    if offset + count > len(data):
        raise VgmError(f"truncated VGM command {command:02X}h at {offset - 1:08X}h")


def parse_vgm(data: bytes) -> ParsedVgm:
    if len(data) < 0x40 or data[:4] != b"Vgm ":
        raise VgmError("input is not a VGM file")

    version = u32(data, 0x08)
    sn_clock = u32(data, 0x0C) & 0x3FFFFFFF
    if sn_clock == 0:
        raise VgmError("VGM does not declare an SN76489 clock")

    declared_rate = u32(data, 0x24)
    tick_rate = declared_rate if declared_rate else 60
    if not 1 <= tick_rate <= 180:
        raise VgmError(f"unsupported VGM playback rate {tick_rate} Hz")

    relative_data = u32(data, 0x34) if version >= 0x150 else 0
    pos = 0x34 + relative_data if relative_data else 0x40
    if pos >= len(data):
        raise VgmError("VGM data offset is outside the file")

    relative_loop = u32(data, 0x1C)
    loop_offset = 0x1C + relative_loop if relative_loop else None
    loop_sample: int | None = None
    loop_event: int | None = None

    events: list[tuple[int, int]] = []
    sample = 0
    wait_60 = 735
    wait_50 = 882

    while pos < len(data):
        command_pos = pos
        if loop_offset == command_pos:
            loop_sample = sample
            loop_event = len(events)

        command = data[pos]
        pos += 1

        if command == 0x50:
            need(data, pos, 1, command)
            events.append((sample, data[pos]))
            pos += 1
        elif command == 0x4F:
            need(data, pos, 1, command)
            pos += 1
        elif command == 0x61:
            need(data, pos, 2, command)
            sample += u16(data, pos)
            pos += 2
        elif command == 0x62:
            sample += wait_60
        elif command == 0x63:
            sample += wait_50
        elif command == 0x64:
            need(data, pos, 3, command)
            target = data[pos]
            replacement = u16(data, pos + 1)
            pos += 3
            if target == 0x62:
                wait_60 = replacement
            elif target == 0x63:
                wait_50 = replacement
            else:
                raise VgmError(f"unsupported wait override {target:02X}h")
        elif command == 0x66:
            break
        elif command == 0x67:
            need(data, pos, 6, command)
            if data[pos] != 0x66:
                raise VgmError(f"bad VGM data-block marker at {command_pos:08X}h")
            block_len = u32(data, pos + 2)
            pos += 6
            need(data, pos, block_len, command)
            pos += block_len
        elif 0x70 <= command <= 0x7F:
            sample += (command & 0x0F) + 1
        elif 0x80 <= command <= 0x8F:
            sample += command & 0x0F
        elif command == 0x30:
            raise VgmError("dual-SN command 30h is unsupported; ZVGC targets PSG0")
        elif 0x51 <= command <= 0x5F or 0xA0 <= command <= 0xBF:
            need(data, pos, 2, command)
            pos += 2
        elif 0xC0 <= command <= 0xDF:
            need(data, pos, 3, command)
            pos += 3
        elif command == 0xE0:
            need(data, pos, 4, command)
            pos += 4
        elif command in (0x90, 0x91, 0x95):
            need(data, pos, 4, command)
            pos += 4
        elif command == 0x92:
            need(data, pos, 5, command)
            pos += 5
        elif command == 0x93:
            need(data, pos, 10, command)
            pos += 10
        elif command == 0x94:
            need(data, pos, 1, command)
            pos += 1
        else:
            raise VgmError(f"unsupported VGM command {command:02X}h at {command_pos:08X}h")
    else:
        raise VgmError("VGM command stream has no 66h end command")

    if loop_offset is not None and loop_sample is None:
        raise VgmError("VGM loop offset is not on a parsed command boundary")

    return ParsedVgm(events, sample, loop_sample, loop_event, sn_clock, tick_rate)


def expand_loops(parsed: ParsedVgm, loops: int) -> tuple[list[tuple[int, int]], int]:
    if loops < 1:
        raise VgmError("loop count must be at least one")
    if loops == 1:
        return list(parsed.events), parsed.end_sample

    if parsed.loop_sample is None or parsed.loop_event is None:
        loop_sample = 0
        loop_event = 0
    else:
        loop_sample = parsed.loop_sample
        loop_event = parsed.loop_event

    duration = parsed.end_sample - loop_sample
    if duration <= 0:
        raise VgmError("VGM loop has zero duration")

    result = list(parsed.events)
    loop_events = parsed.events[loop_event:]
    for repetition in range(1, loops):
        shift = duration * repetition
        result.extend((time + shift, value) for time, value in loop_events)
    return result, loop_sample + duration * loops


def emit_wait(stream: bytearray, ticks: int) -> None:
    while ticks:
        if ticks <= 64:
            stream.append(OP_SHORT_BASE + ticks)
            return
        chunk = min(ticks, 0xFFFF)
        stream.append(OP_WAIT16)
        stream.extend(struct.pack("<H", chunk))
        ticks -= chunk


def compile_stream(
    events: list[tuple[int, int]], end_sample: int, tick_rate: int
) -> tuple[bytes, int]:
    stream = bytearray()
    current_tick = 0
    event_index = 0
    while event_index < len(events):
        sample = events[event_index][0]
        event_tick = (sample * tick_rate + VGM_SAMPLE_RATE // 2) // VGM_SAMPLE_RATE
        if event_tick < current_tick:
            raise VgmError("event time moved backwards")
        emit_wait(stream, event_tick - current_tick)
        current_tick = event_tick

        # All PSG writes quantized to this tick are one ordered burst.  V2
        # stores up to sixteen values under one 80h-8Fh run opcode instead of
        # repeating the two-byte V1 WRITE opcode for every value.
        values = bytearray()
        while event_index < len(events):
            next_sample, value = events[event_index]
            next_tick = (
                next_sample * tick_rate + VGM_SAMPLE_RATE // 2
            ) // VGM_SAMPLE_RATE
            if next_tick != event_tick:
                break
            values.append(value)
            event_index += 1
        for run_start in range(0, len(values), OP_WRITE_RUN_MAX):
            run = values[run_start : run_start + OP_WRITE_RUN_MAX]
            stream.append(OP_WRITE_RUN_BASE + len(run) - 1)
            stream.extend(run)

    end_tick = (end_sample * tick_rate + VGM_SAMPLE_RATE // 2) // VGM_SAMPLE_RATE
    emit_wait(stream, max(0, end_tick - current_tick))
    stream.append(OP_END)
    return bytes(stream), end_tick


def read_input(path: Path) -> bytes:
    raw = path.read_bytes()
    if raw[:2] == b"\x1f\x8b" or path.suffix.lower() == ".vgz":
        return gzip.decompress(raw)
    return raw


def main() -> int:
    parser = argparse.ArgumentParser(
        description="compile VGM/VGZ SN76489 data for Zephyr-80 VGMPLAY"
    )
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--loops",
        type=int,
        default=1,
        help="number of times to include the VGM loop section (default: 1)",
    )
    args = parser.parse_args()

    try:
        source = read_input(args.input)
        parsed = parse_vgm(source)
        events, end_sample = expand_loops(parsed, args.loops)
        stream, ticks = compile_stream(events, end_sample, parsed.tick_rate)
    except (OSError, EOFError, VgmError) as exc:
        parser.error(str(exc))

    header = struct.pack(
        "<4sBBBBII",
        MAGIC,
        VERSION,
        parsed.tick_rate,
        0,
        HEADER_SIZE,
        len(stream),
        ticks,
    )
    args.output.write_bytes(header + stream)

    print(
        f"{args.output}: {len(header) + len(stream)} bytes, "
        f"{len(events)} PSG writes, {ticks} ticks "
        f"({ticks / parsed.tick_rate:.2f} s at {parsed.tick_rate} Hz), "
        f"SN clock {parsed.sn_clock} Hz"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
