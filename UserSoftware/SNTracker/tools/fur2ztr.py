#!/usr/bin/env python3
"""Convert Furnace text exports to the Zephyr-80 ZTR tracker format.

The importer intentionally lives on the host.  SNTRACK.COM only sees the
compact, bounded binary structures documented in docs/ZTR-Format.md.
"""

from __future__ import annotations

import argparse
import math
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


MAGIC = b"ZTR1"
VERSION = 1
HEADER_SIZE = 64
CHANNEL_COUNT = 16
MAX_INSTRUMENTS = 16
MAX_PATTERN_LENGTH = 256
PSG_CLOCK = 3_579_545
NOTE_COUNT = 96
# The supplied TI-99 export was authored for Furnace's divided TI PSG clock.
# Afternoon Blend's direct 3.579545 MHz SN76489AN cannot reach its lowest
# nominal notes; +24 semitones reproduces the divisors in Furnace's VGM export
# on the Zephyr card while retaining source note names in sparse events.
DEFAULT_PLAYBACK_TRANSPOSE = 24

MASK_NOTE = 0x80
MASK_INSTRUMENT = 0x40
MASK_VOLUME = 0x20
MASK_EFFECT = 0x10
MASK_OFF = 0x08
MASK_RELEASE = 0x04
MASK_LEGATO = 0x02
MASK_MODIFIERS = 0x01

MOD_RESET = 0x80
MOD_ARP_SET = 0x40
MOD_ARP_CLEAR = 0x20
MOD_HAIRPIN_SET = 0x10
MOD_HAIRPIN_CLEAR = 0x08
MOD_VALID_MASK = MOD_RESET | MOD_ARP_SET | MOD_ARP_CLEAR | MOD_HAIRPIN_SET | MOD_HAIRPIN_CLEAR

MACRO_TYPES = {
    "vol": 1,
    "pitch": 2,
    "arp": 3,
    "duty": 4,
    "phaseReset": 5,
}
MACRO_NAMES = {value: key for key, value in MACRO_TYPES.items()}
MACRO_SEQUENCE = 0
MACRO_ADSR = 1
MACRO_LFO = 2

# ZTR effect identifiers are deliberately independent of the Furnace command
# values.  Only effects with defined player semantics are emitted.
EFFECTS = {
    0x00: (1, "arpeggio"),
    0x0A: (2, "volume slide"),
    0xEA: (3, "legato"),
    0xF9: (4, "one-tick volume decrease"),
    0xFA: (5, "fast volume slide"),
    0xFF: (6, "song stop"),
}
EFFECT_NAMES = {native: name for _, (native, name) in EFFECTS.items()}

INSTRUMENT_ENTRY_SIZE = 16
PATTERN_ENTRY_SIZE = 8


class ConversionError(ValueError):
    """Input cannot be represented safely in ZTR v1."""


@dataclass(eq=True)
class MacroValue:
    value: int
    fixed: bool = False


@dataclass(eq=True)
class Macro:
    kind: str
    mode: int = MACRO_SEQUENCE
    step: int = 1
    delay: int = 0
    loop: int = -1
    release: int = -1
    active_release: bool = False
    values: list[MacroValue] = field(default_factory=list)


@dataclass(eq=True)
class Instrument:
    number: int
    name: str
    source_type: int = 0
    macros: dict[str, Macro] = field(default_factory=dict)


@dataclass(eq=True)
class Event:
    row: int
    note: int | None = None
    instrument: int | None = None
    volume: int | None = None
    effect: tuple[int, int] | None = None
    note_off: bool = False
    release: bool = False
    legato: bool = False
    mode_reset: bool = False
    arpeggio: int | None = None
    hairpin: int | None = None
    hairpin_ceiling: int | None = None


@dataclass
class Song:
    name: str = ""
    author: str = ""
    system: str = ""
    tuning: int = 440
    playback_transpose: int = DEFAULT_PLAYBACK_TRANSPOSE
    tick_rate: int = 60
    speed: int = 6
    pattern_length: int = 64
    source_channels: int = 0
    sound_chips: list[str] = field(default_factory=list)
    orders: list[list[int]] = field(default_factory=list)
    patterns: dict[tuple[int, int], list[Event]] = field(default_factory=dict)
    instruments: list[Instrument] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)


@dataclass
class DecodedZtr:
    song: Song
    file_size: int
    note_table: list[int]


def warning(song: Song, message: str) -> None:
    song.warnings.append(message)


def parse_int(value: str, context: str, base: int = 10) -> int:
    try:
        return int(value, base)
    except ValueError as exc:
        raise ConversionError(f"{context}: invalid number {value!r}") from exc


def parse_macro(song: Song, instrument: int, kind: str, text: str) -> Macro | None:
    if kind not in MACRO_TYPES:
        warning(song, f"instrument {instrument:02X}: unsupported macro {kind}")
        return None

    mode = MACRO_SEQUENCE
    step = 1
    delay = 0
    if "[ADSR]" in text:
        mode = MACRO_ADSR
        text = text.replace("[ADSR]", " ")
    if "[LFO]" in text:
        if mode != MACRO_SEQUENCE:
            raise ConversionError(
                f"instrument {instrument:02X} {kind}: both ADSR and LFO modes"
            )
        mode = MACRO_LFO
        text = text.replace("[LFO]", " ")

    def take_option(match: re.Match[str], option: str) -> str:
        nonlocal step, delay
        value = parse_int(
            match.group(1), f"instrument {instrument:02X} {kind} {option}"
        )
        if not 0 <= value <= 255:
            raise ConversionError(
                f"instrument {instrument:02X} {kind}: {option} out of range"
            )
        if option == "speed":
            step = value
        else:
            delay = value
        return " "

    text = re.sub(r"\[SPEED\s+(\d+)\]", lambda m: take_option(m, "speed"), text)
    text = re.sub(r"\[DELAY\s+(\d+)\]", lambda m: take_option(m, "delay"), text)
    if "[" in text or "]" in text:
        raise ConversionError(
            f"instrument {instrument:02X} {kind}: unsupported macro option in {text!r}"
        )

    values: list[MacroValue] = []
    loop = -1
    release = -1
    for token in text.split():
        markers = ""
        while token and token[0] in "|/":
            markers += token[0]
            token = token[1:]
        if "|" in markers:
            loop = len(values)
        if "/" in markers:
            release = len(values)
        if not token:
            continue
        value = parse_int(token, f"instrument {instrument:02X} {kind}")
        fixed = False
        if kind == "arp":
            unsigned = value & 0xFFFFFFFF
            fixed = bool(unsigned & 0x40000000)
            if fixed:
                value = unsigned & 0x3FFFFFFF
                if value & 0x20000000:
                    value -= 0x40000000
        if not -32768 <= value <= 32767:
            raise ConversionError(
                f"instrument {instrument:02X} {kind}: value {value} is not 16-bit"
            )
        values.append(MacroValue(value, fixed))

    if not values:
        raise ConversionError(f"instrument {instrument:02X} {kind}: empty macro")
    if not 1 <= step <= 255:
        raise ConversionError(
            f"instrument {instrument:02X} {kind}: step length is outside 1..255"
        )
    active_release = release >= 0
    if mode == MACRO_ADSR:
        # Furnace text exports the current 16-slot generator backing array.
        # ZTR retains only the documented musical parameters, in this order:
        # bottom, top, attack, hold, decay, sustain, sustain time,
        # sustain decay, release.
        if len(values) < 9:
            raise ConversionError(
                f"instrument {instrument:02X} {kind}: ADSR needs 9 parameters"
            )
        values = values[:9]
        return expand_adsr(kind, step, delay, active_release, values)
    elif mode == MACRO_LFO:
        # Furnace's LFO generator uses slots 0/1 and 11..15.  Store a clean
        # bottom/top/speed/shape/phase/loop/global tuple in ZTR.
        if len(values) < 16:
            raise ConversionError(
                f"instrument {instrument:02X} {kind}: LFO needs 16 parameters"
            )
        values = [values[index] for index in (0, 1, 11, 12, 13, 14, 15)]
        return expand_lfo(kind, step, delay, values)

    if loop >= len(values) or release >= len(values):
        raise ConversionError(
            f"instrument {instrument:02X} {kind}: loop/release point is outside macro"
        )
    if len(values) > 65535:
        raise ConversionError(f"instrument {instrument:02X} {kind}: macro too long")
    return Macro(kind, mode, step, delay, loop, release, active_release, values)


def scale_generator_value(bottom: int, top: int, accumulator: int, maximum: int) -> int:
    value = bottom + ((top - bottom) * accumulator + maximum // 2) // maximum
    return max(-32768, min(32767, value))


def expand_adsr(
    kind: str,
    step: int,
    delay: int,
    active_release: bool,
    parameters: list[MacroValue],
) -> Macro:
    """Translate Furnace dev233's normalized ADSR into a native sequence.

    dev233 is from the pre-245 generator generation: its envelope accumulator
    is 0..255 and the sustain field is in that normalized range.  Expanding it
    on the host avoids multiplication and generator-specific packed fields on
    the Z80 while preserving the release transition.
    """

    bottom, top, attack, hold, decay, sustain, sustain_time, sustain_decay, release_rate = (
        item.value for item in parameters
    )
    for label, value in (
        ("attack", attack),
        ("hold", hold),
        ("decay", decay),
        ("sustain", sustain),
        ("sustain time", sustain_time),
        ("sustain decay", sustain_decay),
        ("release", release_rate),
    ):
        if not 0 <= value <= 255:
            raise ConversionError(f"{kind} ADSR {label} is outside 0..255")

    output: list[MacroValue] = []
    accumulator = 0
    if attack:
        while accumulator < 255:
            accumulator = min(255, accumulator + attack)
            output.append(MacroValue(scale_generator_value(bottom, top, accumulator, 255)))
    else:
        output.append(MacroValue(bottom))
    output.extend(MacroValue(top) for _ in range(hold))

    if decay:
        while accumulator > sustain:
            accumulator = max(sustain, accumulator - decay)
            output.append(MacroValue(scale_generator_value(bottom, top, accumulator, 255)))
    else:
        accumulator = sustain
        output.append(MacroValue(scale_generator_value(bottom, top, accumulator, 255)))
    output.extend(
        MacroValue(scale_generator_value(bottom, top, accumulator, 255))
        for _ in range(sustain_time)
    )

    if sustain_decay:
        while accumulator > 0:
            accumulator = max(0, accumulator - sustain_decay)
            output.append(MacroValue(scale_generator_value(bottom, top, accumulator, 255)))
    held_value = scale_generator_value(bottom, top, accumulator, 255)
    if not output or output[-1].value != held_value:
        output.append(MacroValue(held_value))
    loop = len(output) - 1

    release_position = len(output)
    # Passive release reaches this tail after the sustain-decay section, so
    # continue from the accumulator actually held at that boundary.
    release_accumulator = accumulator
    if release_rate:
        while release_accumulator > 0:
            release_accumulator = max(0, release_accumulator - release_rate)
            output.append(
                MacroValue(
                    scale_generator_value(bottom, top, release_accumulator, 255)
                )
            )
    else:
        output.append(MacroValue(scale_generator_value(bottom, top, release_accumulator, 255)))
    if release_position == len(output):
        # A zero sustain level has no generated release steps.  Point release
        # at the existing held zero instead of one value past the sequence.
        release_position = len(output) - 1
    return Macro(
        kind,
        MACRO_SEQUENCE,
        step,
        delay,
        loop,
        release_position,
        active_release,
        output,
    )


def expand_lfo(kind: str, step: int, delay: int, parameters: list[MacroValue]) -> Macro:
    """Translate Furnace dev233's 0..1023 LFO phase into one native cycle."""

    bottom, top, speed, shape, phase, _source_loop, global_flag = (
        item.value for item in parameters
    )
    if not 0 <= phase <= 1023 or not 0 <= speed <= 32767:
        raise ConversionError(f"{kind} LFO phase/speed is out of range")
    if shape not in (0, 1, 2):
        raise ConversionError(f"{kind} LFO shape {shape} is unsupported")
    if global_flag:
        raise ConversionError(f"{kind} LFO global mode is unsupported")
    if speed == 0:
        cycle_length = 1
    else:
        cycle_length = 1024 // math.gcd(1024, speed)
    output: list[MacroValue] = []
    accumulator = phase
    for _ in range(cycle_length):
        accumulator = (accumulator + speed) & 1023
        if shape == 0:
            waveform = accumulator * 2 if accumulator <= 512 else (1024 - accumulator) * 2
        elif shape == 1:
            waveform = accumulator
        else:
            waveform = 1023 if accumulator >= 512 else 0
        output.append(MacroValue(scale_generator_value(bottom, top, waveform, 1023)))
    return Macro(kind, MACRO_SEQUENCE, step, delay, 0, -1, False, output)


def parse_note(token: str, context: str) -> tuple[int | None, bool, bool]:
    if token == "...":
        return None, False, False
    if token == "OFF":
        return None, True, False
    if token == "===":
        return None, False, True
    match = re.fullmatch(r"([A-G])([#-])(-?\d+)", token)
    if not match:
        raise ConversionError(f"{context}: invalid note {token!r}")
    semitone = {
        "C": 0,
        "D": 2,
        "E": 4,
        "F": 5,
        "G": 7,
        "A": 9,
        "B": 11,
    }[match.group(1)]
    if match.group(2) == "#":
        semitone += 1
    octave = int(match.group(3))
    note = octave * 12 + semitone
    if not 0 <= note < NOTE_COUNT:
        raise ConversionError(f"{context}: note {token} is outside C-0..B-7")
    return note, False, False


def parse_cell(
    song: Song, cell: str, order: int, pattern: int, row: int, channel: int
) -> Event | None:
    fields = cell.strip().split()
    context = (
        f"order {order:02X} pattern {pattern:02X} "
        f"row {row:02X} channel {channel:02X}"
    )
    if len(fields) != 4:
        raise ConversionError(f"{context}: expected NOTE INSTRUMENT VOLUME EFFECT")
    note_text, instrument_text, volume_text, effect_text = fields
    note, note_off, release = parse_note(note_text, context)

    instrument = None
    if instrument_text != "..":
        instrument = parse_int(instrument_text, context, 16)
        if not 0 <= instrument < MAX_INSTRUMENTS:
            raise ConversionError(f"{context}: instrument exceeds 0F")

    volume = None
    if volume_text != "..":
        volume = parse_int(volume_text, context, 16)
        if not 0 <= volume <= 15:
            raise ConversionError(f"{context}: SN76489 volume exceeds 0F")

    effect = None
    if effect_text != "....":
        if not re.fullmatch(r"[0-9A-Fa-f]{4}", effect_text):
            raise ConversionError(f"{context}: invalid effect {effect_text!r}")
        furnace_effect = int(effect_text[:2], 16)
        parameter = int(effect_text[2:], 16)
        if furnace_effect not in EFFECTS:
            warning(
                song,
                f"{context}: "
                f"unsupported effect {effect_text.upper()}",
            )
        else:
            effect = (EFFECTS[furnace_effect][0], parameter)

    if (
        note is None
        and instrument is None
        and volume is None
        and effect is None
        and not note_off
        and not release
    ):
        return None
    return Event(row, note, instrument, volume, effect, note_off, release)


def parse_furnace(path: Path) -> Song:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise ConversionError(f"cannot read {path}: {exc}") from exc
    if not lines or lines[0].strip() != "# Furnace Text Export":
        raise ConversionError(f"{path}: not a Furnace text export")

    song = Song()
    section = ""
    current_instrument: Instrument | None = None
    order_rows: dict[int, list[int]] = {}
    order_blocks: dict[int, list[str]] = {}
    current_order: int | None = None

    for line_number, line in enumerate(lines, 1):
        stripped = line.strip()
        if stripped.startswith("# ") and not stripped.startswith("## "):
            section = stripped[2:]
            current_instrument = None
            continue

        if section == "Song Information":
            match = re.match(r"- (name|author|system|tuning):\s*(.*)$", stripped)
            if match:
                key, value = match.groups()
                if key == "tuning":
                    song.tuning = parse_int(value, f"line {line_number} tuning")
                else:
                    setattr(song, key, value)
            continue

        if section == "Sound Chips":
            if line.startswith("- "):
                song.sound_chips.append(line[2:].strip())
            continue

        if section == "Instruments":
            heading = re.match(r"## ([0-9A-Fa-f]{2}):\s*(.*)$", stripped)
            if heading:
                number = int(heading.group(1), 16)
                if number >= MAX_INSTRUMENTS:
                    raise ConversionError(
                        f"line {line_number}: instrument {number:02X} exceeds ZTR limit"
                    )
                current_instrument = Instrument(number, heading.group(2))
                song.instruments.append(current_instrument)
                continue
            if current_instrument is not None:
                type_match = re.match(r"- type:\s*(\d+)$", stripped)
                if type_match:
                    current_instrument.source_type = int(type_match.group(1))
                    continue
                macro_match = re.match(r"- ([A-Za-z][A-Za-z0-9]*):\s*(.*)$", stripped)
                if macro_match and macro_match.group(1) not in ("type", "macros"):
                    kind, macro_text = macro_match.groups()
                    macro = parse_macro(song, current_instrument.number, kind, macro_text)
                    if macro is not None:
                        current_instrument.macros[kind] = macro
            continue

        if section != "Subsongs":
            continue

        property_match = re.match(
            r"- (tick rate|speeds|pattern length):\s*(.*)$", stripped
        )
        if property_match:
            key, value = property_match.groups()
            if key == "tick rate":
                song.tick_rate = parse_int(value, f"line {line_number} tick rate")
            elif key == "pattern length":
                song.pattern_length = parse_int(
                    value, f"line {line_number} pattern length"
                )
            else:
                speeds = value.split()
                if not speeds:
                    raise ConversionError(f"line {line_number}: empty speed list")
                song.speed = parse_int(speeds[0], f"line {line_number} speed")
                if len(speeds) > 1:
                    warning(song, "multiple Furnace groove speeds are unsupported; using first")
            continue

        order_match = re.match(r"([0-9A-Fa-f]{2})\s*\|\s*((?:[0-9A-Fa-f]{2}\s*)+)$", stripped)
        if order_match and current_order is None:
            order_number = int(order_match.group(1), 16)
            order_rows[order_number] = [
                int(item, 16) for item in order_match.group(2).split()
            ]
            continue

        block_match = re.match(r"----- ORDER ([0-9A-Fa-f]{2})$", stripped)
        if block_match:
            current_order = int(block_match.group(1), 16)
            if current_order in order_blocks:
                raise ConversionError(
                    f"line {line_number}: duplicate ORDER {current_order:02X} block"
                )
            order_blocks[current_order] = []
            continue
        if current_order is not None and re.match(r"[0-9A-Fa-f]{2}\s*\|", stripped):
            order_blocks[current_order].append(stripped)

    if not song.name:
        raise ConversionError("Song Information has no name")
    if not 1 <= song.tick_rate <= 1000:
        raise ConversionError("tick rate is outside 1..1000 Hz")
    if not 1 <= song.speed <= 255:
        raise ConversionError("speed is outside 1..255")
    if not 1 <= song.pattern_length <= MAX_PATTERN_LENGTH:
        raise ConversionError(
            f"pattern length is outside 1..{MAX_PATTERN_LENGTH}"
        )
    if not order_rows:
        raise ConversionError("no order list found")
    expected_orders = list(range(max(order_rows) + 1))
    if sorted(order_rows) != expected_orders:
        raise ConversionError("order numbers must be contiguous from 00")
    song.source_channels = len(order_rows[0])
    if not 1 <= song.source_channels <= CHANNEL_COUNT:
        raise ConversionError("source channel count is outside 1..16")
    if any(len(row) != song.source_channels for row in order_rows.values()):
        raise ConversionError("order rows have inconsistent channel counts")
    song.orders = [
        order_rows[index] + [0xFF] * (CHANNEL_COUNT - song.source_channels)
        for index in expected_orders
    ]
    sn_chips = [name for name in song.sound_chips if "SN76489" in name.upper()]
    if not song.sound_chips:
        raise ConversionError("no Sound Chips entries found")
    unsupported_chips = [name for name in song.sound_chips if name not in sn_chips]
    if unsupported_chips:
        raise ConversionError(
            "unsupported sound chip(s): " + ", ".join(unsupported_chips)
        )
    if len(sn_chips) * 4 != song.source_channels:
        raise ConversionError(
            f"{len(sn_chips)} SN76489 instance(s) provide {len(sn_chips) * 4} "
            f"channels, but the order list has {song.source_channels}"
        )

    if sorted(order_blocks) != expected_orders:
        missing = sorted(set(expected_orders) - set(order_blocks))
        raise ConversionError(
            "pattern blocks do not match orders"
            + (f"; missing {','.join(f'{item:02X}' for item in missing)}" if missing else "")
        )

    for order_number in expected_orders:
        rows = order_blocks[order_number]
        if len(rows) != song.pattern_length:
            raise ConversionError(
                f"ORDER {order_number:02X}: expected {song.pattern_length} rows, got {len(rows)}"
            )
        seen_rows: set[int] = set()
        per_channel = [[] for _ in range(song.source_channels)]
        for row_line in rows:
            row_text, separator, cell_text = row_line.partition("|")
            if not separator:
                raise ConversionError(f"ORDER {order_number:02X}: malformed row")
            row = int(row_text.strip(), 16)
            if row in seen_rows or row >= song.pattern_length:
                raise ConversionError(
                    f"ORDER {order_number:02X}: duplicate/out-of-range row {row:02X}"
                )
            seen_rows.add(row)
            cells = cell_text.split("|")
            if len(cells) != song.source_channels:
                raise ConversionError(
                    f"order {order_number:02X} row {row:02X}: expected "
                    f"{song.source_channels} cells, got {len(cells)}"
                )
            for channel, cell in enumerate(cells):
                event = parse_cell(
                    song,
                    cell,
                    order_number,
                    song.orders[order_number][channel],
                    row,
                    channel,
                )
                if event is not None:
                    per_channel[channel].append(event)

        for channel in range(song.source_channels):
            pattern_number = song.orders[order_number][channel]
            key = (channel, pattern_number)
            if key in song.patterns and song.patterns[key] != per_channel[channel]:
                raise ConversionError(
                    f"channel {channel:02X} pattern {pattern_number:02X} has conflicting data"
                )
            song.patterns[key] = per_channel[channel]

    song.instruments.sort(key=lambda item: item.number)
    if len({item.number for item in song.instruments}) != len(song.instruments):
        raise ConversionError("duplicate instrument number")
    defined_instruments = {item.number for item in song.instruments}
    for (channel, pattern), events in song.patterns.items():
        for event in events:
            if (
                event.instrument is not None
                and event.instrument not in defined_instruments
            ):
                raise ConversionError(
                    f"pattern {pattern:02X} row {event.row:02X} "
                    f"channel {channel:02X}: undefined instrument "
                    f"{event.instrument:02X}"
                )
    return song


def encode_macro(macro: Macro) -> bytes:
    if not 1 <= macro.step <= 255 or not 0 <= macro.delay <= 255:
        raise ConversionError(f"macro {macro.kind}: timing value out of range")
    if not macro.values or len(macro.values) > 0xFFFF:
        raise ConversionError(f"macro {macro.kind}: value count is outside 1..65535")
    if macro.loop >= len(macro.values) or macro.release >= len(macro.values):
        raise ConversionError(f"macro {macro.kind}: loop/release point is outside macro")
    if macro.active_release and macro.release < 0:
        raise ConversionError(f"macro {macro.kind}: active release has no release point")
    flags = 1 if macro.active_release else 0
    output = bytearray(
        struct.pack(
            "<BBBBBBHHH",
            MACRO_TYPES[macro.kind],
            macro.mode,
            flags,
            macro.step,
            macro.delay,
            0,
            macro.loop if macro.loop >= 0 else 0xFFFF,
            macro.release if macro.release >= 0 else 0xFFFF,
            len(macro.values),
        )
    )
    for value in macro.values:
        if not -32768 <= value.value <= 32767:
            raise ConversionError(f"macro {macro.kind}: value is not signed 16-bit")
        if value.fixed and macro.kind != "arp":
            raise ConversionError(f"macro {macro.kind}: fixed-note flag is only valid for arp")
        output += struct.pack("<Bh", 1 if value.fixed else 0, value.value)
    return bytes(output)


def event_mask(event: Event) -> int:
    mask = 0
    if event.note is not None:
        mask |= MASK_NOTE
    if event.instrument is not None:
        mask |= MASK_INSTRUMENT
    if event.volume is not None:
        mask |= MASK_VOLUME
    if event.effect is not None:
        mask |= MASK_EFFECT
    if event.note_off:
        mask |= MASK_OFF
    if event.release:
        mask |= MASK_RELEASE
    if event.legato:
        mask |= MASK_LEGATO
    if event.mode_reset or event.arpeggio is not None or event.hairpin is not None:
        mask |= MASK_MODIFIERS
    return mask


def modifier_bytes(event: Event) -> bytes:
    flags = MOD_RESET if event.mode_reset else 0
    payload = bytearray()
    if event.arpeggio is not None:
        if event.arpeggio == -1:
            flags |= MOD_ARP_CLEAR
        elif 0 <= event.arpeggio <= 0xFF:
            flags |= MOD_ARP_SET
            payload.append(event.arpeggio)
        else:
            raise ConversionError("event arpeggio must be packed nibbles or -1")
    if event.hairpin is not None:
        if event.hairpin == 0:
            flags |= MOD_HAIRPIN_CLEAR
        elif -15 <= event.hairpin <= 15:
            if event.hairpin_ceiling is None or not 0 <= event.hairpin_ceiling <= 15:
                raise ConversionError("active hairpin requires a level ceiling in 0..15")
            flags |= MOD_HAIRPIN_SET
            payload.append(event.hairpin & 0xFF)
            payload.append(event.hairpin_ceiling)
        else:
            raise ConversionError("event hairpin rate is outside -15..15")
    if flags & MOD_ARP_SET and flags & MOD_ARP_CLEAR:
        raise ConversionError("event cannot set and clear arpeggio together")
    if flags & MOD_HAIRPIN_SET and flags & MOD_HAIRPIN_CLEAR:
        raise ConversionError("event cannot set and clear hairpin together")
    return bytes((flags,)) + bytes(payload)


def encode_pattern(events: Iterable[Event]) -> bytes:
    output = bytearray()
    for event in events:
        mask = event_mask(event)
        if not mask:
            continue
        output += bytes((event.row, mask))
        if event.note is not None:
            output.append(event.note)
        if event.instrument is not None:
            output.append(event.instrument)
        if event.volume is not None:
            output.append(event.volume)
        if event.effect is not None:
            output += bytes(event.effect)
        if event.legato and event.note is None:
            raise ConversionError("legato flag requires a note")
        if event.legato and (event.note_off or event.release):
            raise ConversionError("legato note cannot also be OFF/release")
        if mask & MASK_MODIFIERS:
            output += modifier_bytes(event)
    return bytes(output)


def build_note_table(tuning: int, playback_transpose: int) -> list[int]:
    if not 1 <= tuning <= 2000:
        raise ConversionError("tuning is outside 1..2000 Hz")
    if not -128 <= playback_transpose <= 127:
        raise ConversionError("playback transpose is outside -128..127 semitones")
    result = []
    for note in range(NOTE_COUNT):
        midi_note = note + 12 + playback_transpose  # ZTR C-0 is MIDI C0.
        frequency = tuning * (2.0 ** ((midi_note - 69) / 12.0))
        divisor = int(round(PSG_CLOCK / (32.0 * frequency)))
        result.append(max(1, min(1023, divisor)))
    return result


def encode_ztr(song: Song) -> bytes:
    string_pool = bytearray(b"\0")
    string_offsets: dict[str, int] = {"": 0}

    def add_string(value: str) -> int:
        if value in string_offsets:
            return string_offsets[value]
        encoded = value.encode("utf-8")
        if b"\0" in encoded:
            raise ConversionError("metadata strings may not contain NUL")
        offset = len(string_pool)
        string_pool.extend(encoded + b"\0")
        string_offsets[value] = offset
        return offset

    add_string(song.name)
    add_string(song.author)
    add_string(song.system)
    for instrument in song.instruments:
        add_string(instrument.name)

    strings_offset = HEADER_SIZE
    order_offset = strings_offset + len(string_pool)
    order_data = b"".join(bytes(row) for row in song.orders)
    instrument_dir_offset = order_offset + len(order_data)
    macro_data_offset = instrument_dir_offset + len(song.instruments) * INSTRUMENT_ENTRY_SIZE

    macro_data = bytearray()
    instrument_macro_offsets: dict[tuple[int, str], int] = {}
    for instrument in song.instruments:
        for kind in MACRO_TYPES:
            macro = instrument.macros.get(kind)
            if macro is not None:
                instrument_macro_offsets[(instrument.number, kind)] = (
                    macro_data_offset + len(macro_data)
                )
                macro_data += encode_macro(macro)

    instrument_dir = bytearray()
    for instrument in song.instruments:
        macro_offsets = [
            instrument_macro_offsets.get((instrument.number, kind), 0)
            for kind in MACRO_TYPES
        ]
        name_offset = strings_offset + string_offsets[instrument.name]
        instrument_dir += struct.pack(
            "<BBH5HBB",
            instrument.number,
            instrument.source_type & 0xFF,
            name_offset,
            *macro_offsets,
            0,
            0,
        )

    pattern_keys = sorted(song.patterns)
    pattern_dir_offset = macro_data_offset + len(macro_data)
    pattern_data_offset = pattern_dir_offset + len(pattern_keys) * PATTERN_ENTRY_SIZE
    pattern_data = bytearray()
    pattern_dir = bytearray()
    for channel, pattern_number in pattern_keys:
        events = song.patterns[(channel, pattern_number)]
        encoded = encode_pattern(events)
        pattern_dir += struct.pack(
            "<BBHHH",
            channel,
            pattern_number,
            pattern_data_offset + len(pattern_data),
            len(encoded),
            len(events),
        )
        pattern_data += encoded

    note_table_offset = pattern_data_offset + len(pattern_data)
    note_table = struct.pack(
        "<96H", *build_note_table(song.tuning, song.playback_transpose)
    )
    file_size = note_table_offset + len(note_table)
    if file_size > 0xFFFF:
        raise ConversionError(f"ZTR output is {file_size} bytes; v1 limit is 65535")

    header = bytearray(HEADER_SIZE)
    struct.pack_into("<4sBBBB", header, 0, MAGIC, VERSION, HEADER_SIZE, 0, CHANNEL_COUNT)
    struct.pack_into("<HBB", header, 8, song.tick_rate, song.speed, song.source_channels)
    struct.pack_into(
        "<HHHHHHI",
        header,
        12,
        song.pattern_length,
        len(song.orders),
        len(pattern_keys),
        len(song.instruments),
        song.tuning,
        0,
        PSG_CLOCK,
    )
    struct.pack_into(
        "<11H",
        header,
        28,
        strings_offset + string_offsets[song.name],
        strings_offset + string_offsets[song.author],
        strings_offset + string_offsets[song.system],
        order_offset,
        instrument_dir_offset,
        macro_data_offset,
        pattern_dir_offset,
        pattern_data_offset,
        note_table_offset,
        file_size,
        len(string_pool),
    )
    struct.pack_into(
        "<BBBB", header, 50, CHANNEL_COUNT, INSTRUMENT_ENTRY_SIZE, PATTERN_ENTRY_SIZE, NOTE_COUNT
    )
    struct.pack_into("<b", header, 54, song.playback_transpose)
    return bytes(
        header
        + string_pool
        + order_data
        + instrument_dir
        + macro_data
        + pattern_dir
        + pattern_data
        + note_table
    )


def checked_slice(data: bytes, offset: int, length: int, label: str) -> bytes:
    if offset < 0 or length < 0 or offset + length > len(data):
        raise ConversionError(f"{label} is outside file bounds")
    return data[offset : offset + length]


def read_string(data: bytes, offset: int, limit: int, label: str) -> str:
    if not HEADER_SIZE <= offset < limit:
        raise ConversionError(f"{label} string offset is outside string pool")
    end = data.find(b"\0", offset, limit)
    if end < 0:
        raise ConversionError(f"{label} string is not terminated in string pool")
    try:
        return data[offset:end].decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ConversionError(f"{label} is not valid UTF-8") from exc


def decode_macro(data: bytes, offset: int, limit: int, expected_kind: str) -> Macro:
    header = checked_slice(data, offset, 12, f"{expected_kind} macro")
    kind_number, mode, flags, step, delay, reserved, loop, release, count = struct.unpack(
        "<BBBBBBHHH", header
    )
    if kind_number != MACRO_TYPES[expected_kind]:
        raise ConversionError(f"{expected_kind} macro type mismatch")
    if mode not in (MACRO_SEQUENCE, MACRO_ADSR, MACRO_LFO) or reserved:
        raise ConversionError(f"{expected_kind} macro has unsupported mode/reserved bits")
    if flags & ~1 or not 1 <= step <= 255 or not count:
        raise ConversionError(f"{expected_kind} macro has invalid flags/timing/count")
    if loop != 0xFFFF and loop >= count:
        raise ConversionError(f"{expected_kind} macro loop is outside values")
    if release != 0xFFFF and release >= count:
        raise ConversionError(f"{expected_kind} macro release is outside values")
    if flags & 1 and release == 0xFFFF:
        raise ConversionError(f"{expected_kind} macro active release has no release point")
    if offset + 12 + count * 3 > limit:
        raise ConversionError(f"{expected_kind} macro extends beyond macro section")
    raw_values = checked_slice(data, offset + 12, count * 3, f"{expected_kind} values")
    values = []
    for value_flags, value in struct.iter_unpack("<Bh", raw_values):
        if value_flags & ~1 or (value_flags & 1 and expected_kind != "arp"):
            raise ConversionError(f"{expected_kind} macro value has invalid flags")
        values.append(MacroValue(value, bool(value_flags & 1)))
    return Macro(
        expected_kind,
        mode,
        step,
        delay,
        -1 if loop == 0xFFFF else loop,
        -1 if release == 0xFFFF else release,
        bool(flags & 1),
        values,
    )


def decode_pattern(data: bytes, offset: int, length: int, count: int, pattern_length: int) -> list[Event]:
    stream = checked_slice(data, offset, length, "pattern stream")
    cursor = 0
    events: list[Event] = []
    previous_row = -1
    for _ in range(count):
        if cursor + 2 > len(stream):
            raise ConversionError("truncated pattern event header")
        row, mask = stream[cursor], stream[cursor + 1]
        cursor += 2
        if row >= pattern_length or row <= previous_row:
            raise ConversionError("pattern rows are out of range or not increasing")
        if not mask:
            raise ConversionError("pattern event has invalid presence mask")
        if (mask & MASK_NOTE) and (mask & (MASK_OFF | MASK_RELEASE)):
            raise ConversionError("pattern event combines a note with OFF/release")
        if (mask & MASK_OFF) and (mask & MASK_RELEASE):
            raise ConversionError("pattern event combines OFF and release")
        if (mask & MASK_LEGATO) and not (mask & MASK_NOTE):
            raise ConversionError("pattern legato flag has no note")

        def byte(label: str) -> int:
            nonlocal cursor
            if cursor >= len(stream):
                raise ConversionError(f"truncated pattern {label}")
            result = stream[cursor]
            cursor += 1
            return result

        note = byte("note") if mask & MASK_NOTE else None
        instrument = byte("instrument") if mask & MASK_INSTRUMENT else None
        volume = byte("volume") if mask & MASK_VOLUME else None
        effect = None
        if mask & MASK_EFFECT:
            effect = (byte("effect"), byte("effect parameter"))
            if effect[0] not in EFFECT_NAMES:
                raise ConversionError(f"unsupported ZTR effect id {effect[0]}")
        mode_reset = False
        arpeggio = None
        hairpin = None
        hairpin_ceiling = None
        if mask & MASK_MODIFIERS:
            modifier_flags = byte("modifier flags")
            if modifier_flags & ~MOD_VALID_MASK:
                raise ConversionError("pattern modifier has reserved flags set")
            if modifier_flags & MOD_ARP_SET and modifier_flags & MOD_ARP_CLEAR:
                raise ConversionError("pattern modifier sets and clears arpeggio")
            if modifier_flags & MOD_HAIRPIN_SET and modifier_flags & MOD_HAIRPIN_CLEAR:
                raise ConversionError("pattern modifier sets and clears hairpin")
            mode_reset = bool(modifier_flags & MOD_RESET)
            if modifier_flags & MOD_ARP_SET:
                arpeggio = byte("arpeggio offsets")
            elif modifier_flags & MOD_ARP_CLEAR:
                arpeggio = -1
            if modifier_flags & MOD_HAIRPIN_SET:
                encoded_rate = byte("hairpin rate")
                hairpin = encoded_rate - 256 if encoded_rate >= 128 else encoded_rate
                hairpin_ceiling = byte("hairpin ceiling")
                if hairpin == 0 or not -15 <= hairpin <= 15:
                    raise ConversionError("pattern hairpin rate is outside -15..15")
                if hairpin_ceiling > 15:
                    raise ConversionError("pattern hairpin ceiling exceeds 0F")
            elif modifier_flags & MOD_HAIRPIN_CLEAR:
                hairpin = 0
        if note is not None and note >= NOTE_COUNT:
            raise ConversionError("pattern note is outside table")
        if instrument is not None and instrument >= MAX_INSTRUMENTS:
            raise ConversionError("pattern instrument exceeds 0F")
        if volume is not None and volume > 15:
            raise ConversionError("pattern volume exceeds 0F")
        events.append(Event(
            row=row,
            note=note,
            instrument=instrument,
            volume=volume,
            effect=effect,
            note_off=bool(mask & MASK_OFF),
            release=bool(mask & MASK_RELEASE),
            legato=bool(mask & MASK_LEGATO),
            mode_reset=mode_reset,
            arpeggio=arpeggio,
            hairpin=hairpin,
            hairpin_ceiling=hairpin_ceiling,
        ))
        previous_row = row
    if cursor != len(stream):
        raise ConversionError("pattern stream length does not match event count")
    return events


def decode_ztr(data: bytes) -> DecodedZtr:
    if len(data) < HEADER_SIZE:
        raise ConversionError("ZTR file is shorter than its header")
    magic, version, header_size, flags, channel_count = struct.unpack_from("<4sBBBB", data, 0)
    if magic != MAGIC or version != VERSION or header_size != HEADER_SIZE:
        raise ConversionError("not a supported ZTR v1 file")
    if flags or channel_count != CHANNEL_COUNT:
        raise ConversionError("unsupported ZTR flags/channel count")
    tick_rate, speed, source_channels = struct.unpack_from("<HBB", data, 8)
    (
        pattern_length,
        order_count,
        pattern_count,
        instrument_count,
        tuning,
        reserved,
        psg_clock,
    ) = struct.unpack_from("<HHHHHHI", data, 12)
    (
        title_offset,
        author_offset,
        system_offset,
        order_offset,
        instrument_dir_offset,
        macro_data_offset,
        pattern_dir_offset,
        pattern_data_offset,
        note_table_offset,
        file_size,
        string_pool_size,
    ) = struct.unpack_from("<11H", data, 28)
    order_entry_size, instrument_entry_size, pattern_entry_size, note_count = struct.unpack_from(
        "<BBBB", data, 50
    )
    playback_transpose = struct.unpack_from("<b", data, 54)[0]
    if reserved or any(data[55:64]) or psg_clock != PSG_CLOCK:
        raise ConversionError("unsupported ZTR reserved data/PSG clock")
    if file_size != len(data):
        raise ConversionError(f"header size {file_size} does not match file size {len(data)}")
    if (
        not 1 <= tick_rate <= 1000
        or not 1 <= speed <= 255
        or not 1 <= source_channels <= CHANNEL_COUNT
        or not 1 <= pattern_length <= MAX_PATTERN_LENGTH
        or not order_count
        or instrument_count > MAX_INSTRUMENTS
        or not 1 <= tuning <= 2000
    ):
        raise ConversionError("ZTR header value is out of range")
    if (
        order_entry_size != CHANNEL_COUNT
        or instrument_entry_size != INSTRUMENT_ENTRY_SIZE
        or pattern_entry_size != PATTERN_ENTRY_SIZE
        or note_count != NOTE_COUNT
    ):
        raise ConversionError("ZTR structure sizes are unsupported")
    expected_offsets = [
        HEADER_SIZE,
        order_offset,
        instrument_dir_offset,
        macro_data_offset,
        pattern_dir_offset,
        pattern_data_offset,
        note_table_offset,
        file_size,
    ]
    if expected_offsets != sorted(expected_offsets):
        raise ConversionError("ZTR section offsets are not monotonic")
    checked_slice(data, HEADER_SIZE, string_pool_size, "string pool")
    if HEADER_SIZE + string_pool_size != order_offset:
        raise ConversionError("string pool size does not reach order section")

    song = Song(
        name=read_string(data, title_offset, order_offset, "title"),
        author=read_string(data, author_offset, order_offset, "author"),
        system=read_string(data, system_offset, order_offset, "system"),
        tuning=tuning,
        playback_transpose=playback_transpose,
        tick_rate=tick_rate,
        speed=speed,
        pattern_length=pattern_length,
        source_channels=source_channels,
    )
    order_bytes = checked_slice(
        data, order_offset, order_count * CHANNEL_COUNT, "order list"
    )
    song.orders = [
        list(order_bytes[index : index + CHANNEL_COUNT])
        for index in range(0, len(order_bytes), CHANNEL_COUNT)
    ]
    if order_offset + len(order_bytes) != instrument_dir_offset:
        raise ConversionError("order list does not reach instrument directory")

    instrument_bytes = checked_slice(
        data,
        instrument_dir_offset,
        instrument_count * INSTRUMENT_ENTRY_SIZE,
        "instrument directory",
    )
    seen_instruments: set[int] = set()
    for values in struct.iter_unpack("<BBH5HBB", instrument_bytes):
        number, source_type, name_offset, *tail = values
        macro_offsets = tail[:5]
        flags_value, reserved_value = tail[5:]
        if number in seen_instruments or number >= MAX_INSTRUMENTS:
            raise ConversionError("duplicate/out-of-range instrument")
        if flags_value or reserved_value:
            raise ConversionError("instrument has unsupported flags")
        seen_instruments.add(number)
        instrument = Instrument(
            number,
            read_string(data, name_offset, order_offset, "instrument"),
            source_type,
        )
        for kind, offset in zip(MACRO_TYPES, macro_offsets):
            if offset:
                if not macro_data_offset <= offset < pattern_dir_offset:
                    raise ConversionError("macro offset is outside macro section")
                instrument.macros[kind] = decode_macro(
                    data, offset, pattern_dir_offset, kind
                )
        song.instruments.append(instrument)

    pattern_bytes = checked_slice(
        data, pattern_dir_offset, pattern_count * PATTERN_ENTRY_SIZE, "pattern directory"
    )
    if pattern_dir_offset + len(pattern_bytes) != pattern_data_offset:
        raise ConversionError("pattern directory does not reach pattern data")
    for channel, pattern, offset, length, count in struct.iter_unpack(
        "<BBHHH", pattern_bytes
    ):
        if channel >= CHANNEL_COUNT or (channel, pattern) in song.patterns:
            raise ConversionError("duplicate/out-of-range pattern directory key")
        if not pattern_data_offset <= offset <= note_table_offset:
            raise ConversionError("pattern offset is outside pattern data")
        if offset + length > note_table_offset:
            raise ConversionError("pattern extends beyond pattern data")
        song.patterns[(channel, pattern)] = decode_pattern(
            data, offset, length, count, pattern_length
        )

    note_bytes = checked_slice(data, note_table_offset, NOTE_COUNT * 2, "note table")
    if note_table_offset + len(note_bytes) != file_size:
        raise ConversionError("note table does not end at file size")
    note_table = list(struct.unpack("<96H", note_bytes))
    if any(not 1 <= divisor <= 1023 for divisor in note_table):
        raise ConversionError("note table contains a divisor outside 1..1023")
    return DecodedZtr(song, file_size, note_table)


def note_name(note: int) -> str:
    names = ("C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-")
    return f"{names[note % 12]}{note // 12}"


def format_event(event: Event) -> str:
    if event.note is not None:
        note = note_name(event.note)
    elif event.note_off:
        note = "OFF"
    elif event.release:
        note = "==="
    else:
        note = "..."
    instrument = ".." if event.instrument is None else f"{event.instrument:02X}"
    volume = ".." if event.volume is None else f"{event.volume:02X}"
    if event.effect is None:
        effect = "...."
    else:
        effect = f"Z{event.effect[0]:X}{event.effect[1]:02X}"
    modifiers = []
    if event.legato:
        modifiers.append("legato")
    if event.mode_reset:
        modifiers.append("reset")
    if event.arpeggio == -1:
        modifiers.append("arp=off")
    elif event.arpeggio is not None:
        modifiers.append(f"arp={event.arpeggio >> 4},{event.arpeggio & 15}")
    if event.hairpin == 0:
        modifiers.append("hairpin=hold")
    elif event.hairpin is not None:
        modifiers.append(f"hairpin={event.hairpin:+d}/{event.hairpin_ceiling}")
    suffix = " [" + " ".join(modifiers) + "]" if modifiers else ""
    return f"{note} {instrument} {volume} {effect}{suffix}"


def print_summary(song: Song, size: int | None = None) -> None:
    suffix = f", {size} bytes" if size is not None else ""
    event_count = sum(len(events) for events in song.patterns.values())
    print(f"{song.name} — {song.author}")
    print(f"system: {song.system}")
    print(f"target playback transpose: {song.playback_transpose:+d} semitones")
    print(
        f"tick {song.tick_rate} Hz, speed {song.speed}, rows {song.pattern_length}, "
        f"orders {len(song.orders)}, source channels {song.source_channels}/16{suffix}"
    )
    print(
        f"instruments {len(song.instruments)}, pattern streams {len(song.patterns)}, "
        f"sparse events {event_count}, tuning A4={song.tuning} Hz"
    )
    if song.sound_chips:
        print("sound chips: " + ", ".join(song.sound_chips))
    if song.warnings:
        for item in song.warnings:
            print(f"WARNING: {item}", file=sys.stderr)
    else:
        print("warnings: none")


def dump_ztr(decoded: DecodedZtr) -> None:
    song = decoded.song
    print_summary(song, decoded.file_size)
    print("\nInstruments:")
    for instrument in song.instruments:
        macro_text = ", ".join(
            f"{kind}:{('seq', 'adsr', 'lfo')[macro.mode]}/{len(macro.values)}"
            for kind, macro in instrument.macros.items()
        ) or "none"
        print(f"  {instrument.number:02X} {instrument.name}: {macro_text}")
    print("\nOrders:")
    for number, order in enumerate(song.orders):
        print(f"  {number:02X} | " + " ".join(f"{value:02X}" for value in order))
    print("\nPatterns:")
    for (channel, pattern), events in sorted(song.patterns.items()):
        print(f"  channel {channel:02X} pattern {pattern:02X} ({len(events)} events)")
        for event in events:
            print(f"    {event.row:02X} | {format_event(event)}")


def validate_round_trip(source: Song, decoded: Song) -> None:
    fields = (
        "name",
        "author",
        "system",
        "tuning",
        "playback_transpose",
        "tick_rate",
        "speed",
        "pattern_length",
        "source_channels",
        "orders",
        "patterns",
        "instruments",
    )
    for field_name in fields:
        if getattr(source, field_name) != getattr(decoded, field_name):
            raise ConversionError(f"round-trip mismatch in {field_name}")


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--validate", metavar="FURNACE-TEXT", type=Path)
    mode.add_argument("--dump", metavar="ZTR", type=Path)
    parser.add_argument("source", nargs="?", type=Path)
    parser.add_argument("output", nargs="?", type=Path)
    parser.add_argument(
        "--strict", action="store_true", help="treat importer warnings as errors"
    )
    parser.add_argument(
        "--transpose",
        type=int,
        metavar="SEMITONES",
        help=f"target playback transpose (default: {DEFAULT_PLAYBACK_TRANSPOSE:+d})",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_argument_parser().parse_args(argv)
    try:
        if args.dump:
            if args.source or args.output or args.transpose is not None:
                raise ConversionError(
                    "--dump does not accept source/output/transpose arguments"
                )
            data = args.dump.read_bytes()
            dump_ztr(decode_ztr(data))
            return 0

        source_path = args.validate or args.source
        if source_path is None:
            raise ConversionError("provide a Furnace text export or use --dump")
        if args.validate and (args.source or args.output):
            raise ConversionError("--validate takes exactly one Furnace text path")
        song = parse_furnace(source_path)
        if args.transpose is not None:
            song.playback_transpose = args.transpose
        data = encode_ztr(song)
        decoded = decode_ztr(data)
        validate_round_trip(song, decoded.song)
        if args.strict and song.warnings:
            raise ConversionError(f"conversion produced {len(song.warnings)} warning(s)")
        print_summary(song, len(data))
        if args.validate:
            print("validation: parse, encode, bounds check, and decode passed")
            return 0
        output_path = args.output or source_path.with_suffix(".ztr")
        output_path.write_bytes(data)
        print(f"wrote {output_path}")
        return 0
    except (ConversionError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
