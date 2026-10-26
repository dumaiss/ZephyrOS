#!/usr/bin/env python3
"""Compile a CSM score directly to Zephyr-80 ZTR v1."""

from __future__ import annotations

import argparse
import re
import sys
from collections import OrderedDict
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import csm_language as language
import fur2ztr


NOTE_NAMES = {"c": 0, "d": 2, "e": 4, "f": 5, "g": 7, "a": 9, "b": 11}
NOISE_RATES = {"low": 2, "medium": 1, "high": 0, "tone3": 3}
# Minimum 0..255 linear amplitude for each SN/Furnace loudness code 1..15.
# These fixed thresholds are the rounded half-step boundaries of the SN's
# two-decibel attenuator. The compiler therefore needs no floating point.
SN_LOUDNESS_THRESHOLDS = (9, 11, 14, 18, 23, 29, 36, 45, 57, 72, 90, 114, 143, 181, 227)
ZEPHYR_PLAYER_LIMIT = 20 * 1024


class CsmCompileError(ValueError):
    """A valid CSM syntax tree cannot be represented by this backend."""


@dataclass
class CompiledInstrument:
    number: int
    release_ticks: int
    active_release: bool
    level_ceiling: int


@dataclass
class AllocationUnit:
    key: str
    layer: str
    route: str
    candidates: tuple[int, ...]
    required: bool
    activity: frozenset[int]
    hard_channel: int | None = None
    tone3_noise: bool = False


def value(node: Any) -> Any:
    """Unwrap a language-model expression node into plain Python values."""
    if not isinstance(node, dict):
        return node
    kind = node.get("kind")
    if kind in ("integer", "float", "string", "boolean"):
        return node["value"]
    if kind == "reference":
        return node["name"]
    if kind == "array":
        return [value(item) for item in node["items"]]
    if kind == "call":
        return {"call": node["name"], "args": [value(arg) for arg in node["arguments"]]}
    return node


def properties(node: dict[str, Any]) -> dict[str, Any]:
    return {name: value(item) for name, item in node.get("properties", {}).items()}


def require_integer(props: dict[str, Any], name: str, context: str, minimum: int | None = None,
                    maximum: int | None = None, default: int | None = None) -> int:
    item = props.get(name, default)
    if not isinstance(item, int):
        raise CsmCompileError(f"{context}: {name} must be an integer")
    if minimum is not None and item < minimum:
        raise CsmCompileError(f"{context}: {name} must be at least {minimum}")
    if maximum is not None and item > maximum:
        raise CsmCompileError(f"{context}: {name} must be at most {maximum}")
    return item


def expand_expression(expression: dict[str, Any], patterns: dict[str, Any]) -> list[list[list[dict[str, Any]]]]:
    bars: list[list[list[dict[str, Any]]]] = []
    for term in expression["terms"]:
        repeat = term.get("repeat", 1)
        if term["kind"] == "bar":
            bar = [[dict(event) for event in slot["events"]] for slot in term["slots"]]
            for _ in range(repeat):
                bars.append(bar)
        elif term["kind"] == "reference":
            expanded = expand_expression(patterns[term["reference"]]["expression"], patterns)
            for _ in range(repeat):
                bars.extend(expanded)
        else:
            raise CsmCompileError(f"unknown pattern term {term['kind']!r}")
    return bars


def note_number(token: str, transpose: int, playback_transpose: int) -> int:
    match = re.fullmatch(r"([a-g])(b|#)?([0-9]+)", token)
    if not match:
        raise CsmCompileError(f"invalid note {token!r}")
    accidental = -1 if match.group(2) == "b" else 1 if match.group(2) == "#" else 0
    midi = (int(match.group(3)) + 1) * 12 + NOTE_NAMES[match.group(1)] + accidental + transpose
    result = midi - (12 + playback_transpose)
    if not 0 <= result < fur2ztr.NOTE_COUNT:
        raise CsmCompileError(f"note {token} with transpose {transpose:+d} is outside C-0..B-7")
    return result


def sn_loudness(amplitude: int) -> int:
    if amplitude <= 0:
        return 0
    result = 1
    for threshold in SN_LOUDNESS_THRESHOLDS[1:]:
        if amplitude < threshold:
            break
        result += 1
    return min(result, 15)


def ramp(start: int, end: int, ticks: int) -> list[int]:
    if ticks <= 0:
        return []
    return [start + ((end - start) * step + ticks // 2) // ticks for step in range(1, ticks + 1)]


def compile_volume_macro(name: str, props: dict[str, Any]) -> tuple[fur2ztr.Macro, int, bool]:
    level = require_integer(props, "level", f"instrument {name}", 0, 255)
    envelope = props.get("envelope")
    if not isinstance(envelope, dict) or "call" not in envelope:
        raise CsmCompileError(f"instrument {name}: envelope must be adsr(), one_shot(), or table()")

    call = envelope["call"]
    args = envelope["args"]
    amplitudes: list[int]
    loop = -1
    release_index = -1
    release_ticks = 0
    active_release = False

    if call == "adsr":
        if len(args) != 4 or any(not isinstance(item, int) for item in args):
            raise CsmCompileError(f"instrument {name}: adsr requires four integer arguments")
        attack, decay, sustain, release_ticks = args
        if attack < 0 or decay < 0 or release_ticks < 0 or not 0 <= sustain <= 255:
            raise CsmCompileError(f"instrument {name}: invalid adsr range")
        amplitudes = ramp(0, 255, attack) if attack else [255]
        amplitudes.extend(ramp(amplitudes[-1], sustain, decay))
        if amplitudes[-1] != sustain:
            amplitudes.append(sustain)
        loop = len(amplitudes) - 1
        if release_ticks:
            release_index = len(amplitudes)
            amplitudes.extend(ramp(sustain, 0, release_ticks))
            active_release = True
    elif call == "one_shot":
        if len(args) != 2 or any(not isinstance(item, int) for item in args):
            raise CsmCompileError(f"instrument {name}: one_shot requires two integer arguments")
        attack, decay = args
        if attack < 0 or decay <= 0:
            raise CsmCompileError(f"instrument {name}: invalid one_shot range")
        amplitudes = ramp(0, 255, attack) if attack else [255]
        amplitudes.extend(ramp(amplitudes[-1], 0, decay))
        loop = len(amplitudes) - 1
    elif call == "table":
        if len(args) != 4 or not isinstance(args[0], list) or any(not isinstance(item, int) for item in args[1:]):
            raise CsmCompileError(f"instrument {name}: table requires ([values], step, loop, release)")
        amplitudes = args[0]
        step, loop, release_index = args[1:]
        if not amplitudes or any(not isinstance(item, int) or not 0 <= item <= 255 for item in amplitudes):
            raise CsmCompileError(f"instrument {name}: table values must be integers in 0..255")
        if not 1 <= step <= 255:
            raise CsmCompileError(f"instrument {name}: table step must be in 1..255 ticks")
        if loop < -1 or release_index < -1 or loop >= len(amplitudes) or release_index >= len(amplitudes):
            raise CsmCompileError(f"instrument {name}: table loop/release index is outside the table")
        active_release = release_index >= 0
        values = [fur2ztr.MacroValue(sn_loudness((level * item + 127) // 255)) for item in amplitudes]
        release_ticks = (len(amplitudes) - release_index) * step if active_release else 0
        return fur2ztr.Macro("vol", step=step, loop=loop, release=release_index,
                             active_release=active_release, values=values), release_ticks, active_release
    else:
        raise CsmCompileError(f"instrument {name}: unsupported envelope {call}()")

    values = [fur2ztr.MacroValue(sn_loudness((level * item + 127) // 255)) for item in amplitudes]
    return fur2ztr.Macro("vol", step=1, loop=loop, release=release_index,
                         active_release=active_release, values=values), release_ticks, active_release


def compile_pitch_macro(name: str, props: dict[str, Any], detune: int) -> fur2ztr.Macro | None:
    effects = props.get("effects", [])
    vibrato: dict[str, Any] | None = None
    if not isinstance(effects, list):
        raise CsmCompileError(f"instrument {name}: effects must be an array")
    for effect in effects:
        if not isinstance(effect, dict) or effect.get("call") != "vibrato":
            raise CsmCompileError(f"instrument {name}: unsupported instrument effect")
        if vibrato is not None:
            raise CsmCompileError(f"instrument {name}: only one vibrato is supported")
        vibrato = effect
    if vibrato is None:
        if detune == 0:
            return None
        return fur2ztr.Macro("pitch", loop=0, values=[fur2ztr.MacroValue(detune)])
    args = vibrato["args"]
    if len(args) != 2 or any(not isinstance(item, int) for item in args):
        raise CsmCompileError(f"instrument {name}: vibrato requires (period_ticks, depth)")
    period, depth = args
    if period < 4 or period > 255 or depth < 0 or depth > 32767:
        raise CsmCompileError(f"instrument {name}: invalid vibrato period/depth")
    half = max(1, period // 2)
    values = []
    for tick in range(period):
        phase = tick if tick <= half else period - tick
        offset = -depth + (2 * depth * phase + half // 2) // half
        values.append(fur2ztr.MacroValue(detune + offset))
    return fur2ztr.Macro("pitch", loop=0, values=values)


def endpoint_channel(endpoint: str) -> int:
    match = re.fullmatch(r"psg([0-3])\.(tone([0-2])|noise)", endpoint)
    if not match:
        raise CsmCompileError(f"unsupported Afternoon Blend endpoint {endpoint!r}")
    return int(match.group(1)) * 4 + (3 if match.group(2) == "noise" else int(match.group(3)))


def compile_event_modifiers(
    event: dict[str, Any],
    level_ceiling: int,
) -> tuple[int | None, int | None, int | None]:
    arpeggio = None
    hairpin = None
    hairpin_ceiling = None
    for modifier in event.get("modifiers", []):
        kind = modifier.get("kind")
        modifier_value = modifier.get("value")
        if kind == "arpeggio":
            if modifier_value is None:
                arpeggio = -1
            elif (
                isinstance(modifier_value, list)
                and len(modifier_value) == 2
                and all(isinstance(item, int) and 0 <= item <= 15 for item in modifier_value)
            ):
                arpeggio = (modifier_value[0] << 4) | modifier_value[1]
            else:
                raise CsmCompileError("invalid arpeggio modifier")
        elif kind == "hairpin":
            if not isinstance(modifier_value, int) or not -15 <= modifier_value <= 15:
                raise CsmCompileError("invalid hairpin modifier")
            hairpin = modifier_value
            if hairpin:
                hairpin_ceiling = level_ceiling
        else:
            raise CsmCompileError(f"unsupported event modifier {kind!r}")
    return arpeggio, hairpin, hairpin_ceiling


class Compiler:
    def __init__(self, ir: dict[str, Any]):
        self.ir = ir
        self.song_ir = ir["song"]
        if len(ir.get("targets", [])) != 1:
            raise CsmCompileError("the direct compiler currently requires exactly one target")
        self.target_ir = ir["targets"][0]
        self.song_props = properties(self.song_ir)
        self.target_props = properties(self.target_ir)

        self.tick_rate = require_integer(self.song_props, "tick_rate", "song", 1, 65535)
        self.ticks_per_row = require_integer(self.song_props, "ticks_per_row", "song", 1, 255)
        self.rows_per_quarter = require_integer(self.song_props, "rows_per_quarter", "song", 1, 64)
        self.tuning = require_integer(self.song_props, "tuning", "song", 1, 2000)
        meter = self.song_props.get("meter")
        if not isinstance(meter, dict) or meter.get("call") != "meter" or len(meter["args"]) != 2:
            raise CsmCompileError("song: meter must be meter(numerator, denominator)")
        numerator, denominator = meter["args"]
        row_numerator = self.rows_per_quarter * numerator * 4
        if not isinstance(numerator, int) or not isinstance(denominator, int) or denominator <= 0 or row_numerator % denominator:
            raise CsmCompileError("song: meter cannot be represented by the row grid")
        self.rows_per_bar = row_numerator // denominator
        if not 1 <= self.rows_per_bar <= fur2ztr.MAX_PATTERN_LENGTH:
            raise CsmCompileError("song: rows per bar is outside the ZTR v1 range")

        self.playback_transpose = require_integer(self.target_props, "transpose", "target", -128, 127)
        clock = require_integer(self.target_props, "clock", "target", 1)
        if clock != fur2ztr.PSG_CLOCK:
            raise CsmCompileError(f"target: current ZTR backend requires clock {fur2ztr.PSG_CLOCK}")
        if self.target_props.get("backend") != "sn76489x4":
            raise CsmCompileError("target: only backend sn76489x4 is implemented")

        self.patterns = self.song_ir["patterns"]
        self.scenes = {name: properties(item) for name, item in self.song_ir["scenes"].items()}
        self.instruments = {name: properties(item) for name, item in self.song_ir["instruments"].items()}
        self.realization_nodes = self.target_ir["realizations"]
        self.realizations = {name: properties(item) for name, item in self.realization_nodes.items()}
        self.bindings = {name: value(item) for name, item in self.target_ir["bindings"].items()}
        self.routes = {name: value(item) for name, item in self.target_ir["routes"].items()}
        self.max_ztr_bytes = ZEPHYR_PLAYER_LIMIT
        self.validate_extensions()

        play_name = self.song_props["play"]
        form = self.song_ir["forms"].get(play_name)
        if form is None:
            raise CsmCompileError(f"song: selected form {play_name!r} does not exist")
        form_props = properties(form)
        if require_integer(form_props, "loop", f"form {play_name}", 0, 0, 0) != 0:
            raise CsmCompileError("the current ZTR backend supports play-once forms only")
        self.orchestra_name = form_props["using"]
        self.sequence = [item["args"] for item in form_props["sequence"]]
        self.orchestra = self.song_ir["orchestras"][self.orchestra_name]
        self.layers = {name: properties(item) for name, item in self.orchestra["layers"].items()}
        self.groups = {name: properties(item).get("members", []) for name, item in self.orchestra.get("groups", {}).items()}

        self.plan: list[tuple[str, str]] = []
        for section, scene in self.sequence:
            self.plan.extend((section, scene) for _ in range(self.song_ir["sections"][section]["bar_count"]))
        self.form_bars = len(self.plan)
        self.events: list[dict[int, fur2ztr.Event]] = [dict() for _ in range(fur2ztr.CHANNEL_COUNT)]
        self.event_owners: list[dict[int, str]] = [dict() for _ in range(fur2ztr.CHANNEL_COUNT)]
        self.layer_instruments: dict[str, CompiledInstrument] = {}
        self.ztr_instruments: list[fur2ztr.Instrument] = []
        self.allocations: dict[str, int | None] = {}
        self.allocation_units: list[AllocationUnit] = []
        self.allocation_messages: list[str] = []

    def extension_properties(
        self,
        node: dict[str, Any],
        context: str,
        allowed: set[str],
    ) -> dict[str, Any]:
        extensions = node.get("extensions", {})
        unknown_backends = sorted(set(extensions) - {"sn76489"})
        if unknown_backends:
            raise CsmCompileError(f"{context}: unsupported extension {unknown_backends[0]!r}")
        if "sn76489" not in extensions:
            return {}
        result = properties(extensions["sn76489"])
        unknown_properties = sorted(set(result) - allowed)
        if unknown_properties:
            raise CsmCompileError(
                f"{context}: unsupported sn76489 extension property {unknown_properties[0]!r}"
            )
        return result

    def validate_extensions(self) -> None:
        target_extension = self.extension_properties(
            self.target_ir,
            "target",
            {"max_ztr_bytes"},
        )
        if target_extension:
            self.max_ztr_bytes = require_integer(
                target_extension, "max_ztr_bytes", "target extension sn76489", 1, 65535
            )
        for name, node in self.realization_nodes.items():
            extension = self.extension_properties(node, f"realize {name}", {"phase_reset"})
            if extension:
                require_integer(extension, "phase_reset", f"realize {name} extension sn76489", 0, 1)

    def expand_group(self, name: str, stack: tuple[str, ...] = ()) -> set[str]:
        if name in stack:
            raise CsmCompileError(f"cyclic group {' -> '.join(stack + (name,))}")
        result: set[str] = set()
        for member in self.groups.get(name, []):
            if member in self.layers:
                result.add(member)
            elif member in self.groups:
                result.update(self.expand_group(member, stack + (name,)))
        return result

    def enabled_layers(self, scene: str) -> set[str]:
        result: set[str] = set()
        for item in self.scenes[scene].get("enables", []):
            if item in self.layers:
                result.add(item)
            elif item in self.groups:
                result.update(self.expand_group(item))
        return result

    def scene_transpose(self, scene: str, role: str, layer: str) -> int:
        result = 0
        for transform in self.scenes[scene].get("transforms", []):
            if isinstance(transform, dict) and transform.get("call") == "transpose":
                target, amount = transform["args"]
                if target in (role, layer):
                    result += amount
        return result

    def role_bars(self, role: str) -> list[list[list[dict[str, Any]]] | None]:
        output: list[list[list[dict[str, Any]]] | None] = []
        for section, _scene in self.sequence:
            section_ir = self.song_ir["sections"][section]
            stream = section_ir["streams"].get(role)
            count = section_ir["bar_count"]
            if stream is None:
                output.extend([None] * count)
                continue
            bars = expand_expression(stream["expression"], self.patterns)
            if len(bars) != count:
                raise CsmCompileError(f"section {section}.{role}: expanded bar count changed after validation")
            output.extend(bars)
        return output

    def compile_instruments(self) -> None:
        variants: OrderedDict[tuple[str, int], int] = OrderedDict()
        for layer_name, layer in self.layers.items():
            instrument_name = layer["instrument"]
            detune = require_integer(layer, "detune", f"layer {layer_name}", -32768, 32767, 0)
            key = (instrument_name, detune)
            if key not in variants:
                variants[key] = len(variants)
            number = variants[key]
            if number >= fur2ztr.MAX_INSTRUMENTS:
                raise CsmCompileError("SN backend needs more than 16 realized instrument variants")
            if layer_name in self.layer_instruments:
                continue
            inst_props = self.instruments[instrument_name]
            volume, release_ticks, active_release = compile_volume_macro(instrument_name, inst_props)
            macros = {"vol": volume}
            pitch = compile_pitch_macro(instrument_name, inst_props, detune)
            if pitch is not None:
                macros["pitch"] = pitch
            realization = self.realizations[instrument_name]
            realization_extension = self.extension_properties(
                self.realization_nodes[instrument_name],
                f"realize {instrument_name}",
                {"phase_reset"},
            )
            waveform = realization.get("waveform")
            if waveform == "noise":
                mode = realization.get("mode")
                if mode not in ("white", "periodic"):
                    raise CsmCompileError(f"realize {instrument_name}: noise mode must be white or periodic")
                rate = realization.get("rate")
                if rate not in NOISE_RATES:
                    raise CsmCompileError(f"realize {instrument_name}: invalid noise rate")
                macros["duty"] = fur2ztr.Macro(
                    "duty", loop=0, values=[fur2ztr.MacroValue(1 if mode == "white" else 0)]
                )
            elif waveform != "square":
                raise CsmCompileError(f"realize {instrument_name}: waveform must be square or noise")
            if realization_extension.get("phase_reset"):
                if waveform != "noise":
                    raise CsmCompileError(
                        f"realize {instrument_name}: phase_reset is only meaningful for noise"
                    )
                macros["phaseReset"] = fur2ztr.Macro(
                    "phaseReset",
                    loop=1,
                    values=[fur2ztr.MacroValue(1), fur2ztr.MacroValue(0)],
                )
            suffix = f" ({detune:+d}/128)" if detune else ""
            if not any(item.number == number for item in self.ztr_instruments):
                self.ztr_instruments.append(fur2ztr.Instrument(number, instrument_name + suffix, macros=macros))
            level_ceiling = sn_loudness(require_integer(inst_props, "level", f"instrument {instrument_name}", 0, 255))
            self.layer_instruments[layer_name] = CompiledInstrument(
                number, release_ticks, active_release, level_ceiling
            )

    def layer_units(self, layer_name: str, layer: dict[str, Any]) -> list[tuple[str, str]]:
        distribute = layer.get("distribute")
        if distribute is not None:
            if not isinstance(distribute, dict) or distribute.get("call") != "roundrobin":
                raise CsmCompileError(f"layer {layer_name}: unsupported distribution")
            routes = distribute["args"][0]
            if not isinstance(routes, list) or not routes:
                raise CsmCompileError(f"layer {layer_name}: roundrobin requires at least one route")
            return [(f"{layer_name}[{route}]", route) for route in routes]
        route = layer.get("route")
        if not isinstance(route, str):
            raise CsmCompileError(f"layer {layer_name}: route must name a target route")
        return [(layer_name, route)]

    def route_candidates(self, route: str, noise: bool) -> tuple[int, ...]:
        route_value = self.routes.get(route)
        psgs = route_value if isinstance(route_value, list) else [route_value]
        chips: list[int] = []
        for psg in psgs:
            match = re.fullmatch(r"psg([0-3])", psg) if isinstance(psg, str) else None
            if match is None:
                raise CsmCompileError(f"route {route}: expected psg0..psg3 or an array of them")
            chip = int(match.group(1))
            if chip not in chips:
                chips.append(chip)
        if noise:
            return tuple(chip * 4 + 3 for chip in chips)
        return tuple(chip * 4 + voice for chip in chips for voice in range(3))

    def compile_layer_events(
        self,
        layer_name: str,
        layer: dict[str, Any],
        channels: list[int | None],
        validate_physical: bool,
    ) -> list[dict[int, fur2ztr.Event]]:
        saved_events = self.events
        self.events = [dict() for _ in range(fur2ztr.CHANNEL_COUNT)]
        try:
            self.compile_layer(layer_name, layer, channels, validate_physical)
            self.remove_redundant_state_events()
            return self.events
        finally:
            self.events = saved_events

    def activity_for_events(self, events: dict[int, fur2ztr.Event]) -> frozenset[int]:
        active_start: int | None = None
        activity: set[int] = set()
        for row, event in sorted(events.items()):
            if event.note is not None and active_start is None:
                active_start = row
            if event.note_off and active_start is not None:
                activity.update(range(active_start, row))
                active_start = None
        if active_start is not None:
            raise CsmCompileError("internal allocation timeline has an unterminated note")
        return frozenset(activity)

    @staticmethod
    def shares_resource(
        unit: AllocationUnit,
        channel: int,
        other: AllocationUnit,
        other_channel: int,
    ) -> bool:
        if channel == other_channel:
            return True
        same_chip = channel // 4 == other_channel // 4
        if not same_chip:
            return False
        if unit.tone3_noise and (other_channel & 3) == 2:
            return True
        if other.tone3_noise and (channel & 3) == 2:
            return True
        return False

    def allocation_conflicts(
        self,
        unit: AllocationUnit,
        channel: int,
        assigned: dict[str, int],
        units: dict[str, AllocationUnit],
    ) -> list[AllocationUnit]:
        conflicts = []
        for key, other_channel in assigned.items():
            other = units[key]
            if unit.activity.isdisjoint(other.activity):
                continue
            if self.shares_resource(unit, channel, other, other_channel):
                conflicts.append(other)
        return conflicts

    def allocate_channels(self) -> None:
        units: list[AllocationUnit] = []
        valid_binding_keys: set[str] = set()
        for layer_name, layer in self.layers.items():
            specs = self.layer_units(layer_name, layer)
            pseudo_channels = list(range(len(specs)))
            local_events = self.compile_layer_events(layer_name, layer, pseudo_channels, False)
            instrument_name = layer["instrument"]
            instrument_props = self.instruments[instrument_name]
            priority = instrument_props.get("priority")
            if priority not in ("required", "optional"):
                raise CsmCompileError(f"instrument {instrument_name}: priority must be required or optional")
            realization = self.realizations[instrument_name]
            noise = realization.get("waveform") == "noise"
            tone3_noise = noise and realization.get("rate") == "tone3"
            for index, (key, route) in enumerate(specs):
                valid_binding_keys.add(key)
                candidates = self.route_candidates(route, noise)
                hard_channel = None
                if key in self.bindings:
                    hard_channel = endpoint_channel(self.bindings[key])
                    if hard_channel not in candidates:
                        raise CsmCompileError(
                            f"binding {key}={self.bindings[key]} is outside route {route}"
                        )
                units.append(
                    AllocationUnit(
                        key,
                        layer_name,
                        route,
                        candidates,
                        priority == "required",
                        self.activity_for_events(local_events[index]),
                        hard_channel,
                        tone3_noise,
                    )
                )
        unused_bindings = sorted(set(self.bindings) - valid_binding_keys)
        if unused_bindings:
            raise CsmCompileError(f"binding {unused_bindings[0]} does not name an allocation unit")

        by_key = {unit.key: unit for unit in units}
        assigned: dict[str, int] = {}
        for unit in sorted((item for item in units if item.hard_channel is not None), key=lambda item: item.key):
            assert unit.hard_channel is not None
            conflicts = self.allocation_conflicts(unit, unit.hard_channel, assigned, by_key)
            if conflicts:
                row = min(unit.activity & conflicts[0].activity)
                raise CsmCompileError(
                    f"hard binding conflict at row {row}: {unit.key} and {conflicts[0].key}"
                )
            assigned[unit.key] = unit.hard_channel

        required = sorted(
            (item for item in units if item.required and item.key not in assigned),
            key=lambda item: (len(item.candidates), -len(item.activity), item.key),
        )

        def assign_required(index: int) -> bool:
            if index == len(required):
                return True
            unit = required[index]
            for channel in unit.candidates:
                if self.allocation_conflicts(unit, channel, assigned, by_key):
                    continue
                assigned[unit.key] = channel
                if assign_required(index + 1):
                    return True
                del assigned[unit.key]
            return False

        if not assign_required(0):
            unit = next(item for item in required if item.key not in assigned)
            conflict_rows = []
            for channel in unit.candidates:
                for other in self.allocation_conflicts(unit, channel, assigned, by_key):
                    conflict_rows.extend(unit.activity & other.activity)
            detail = f" at row {min(conflict_rows)}" if conflict_rows else ""
            raise CsmCompileError(f"cannot allocate required layer unit {unit.key}{detail}")

        optional = sorted(
            (item for item in units if not item.required and item.key not in assigned),
            key=lambda item: (-len(item.activity), item.key),
        )
        for unit in optional:
            channel = next(
                (
                    candidate
                    for candidate in unit.candidates
                    if not self.allocation_conflicts(unit, candidate, assigned, by_key)
                ),
                None,
            )
            if channel is not None:
                assigned[unit.key] = channel

        self.allocation_units = units
        self.allocations = {unit.key: assigned.get(unit.key) for unit in units}
        for unit in units:
            channel = self.allocations[unit.key]
            if channel is None:
                self.allocation_messages.append(f"dropped optional {unit.key}")
            else:
                chip, voice = divmod(channel, 4)
                voice_name = "noise" if voice == 3 else f"tone{voice}"
                binding = "hard" if unit.hard_channel is not None else "allocated"
                self.allocation_messages.append(
                    f"{unit.key} -> psg{chip}.{voice_name} ({binding}, route {unit.route})"
                )

    def allocated_layer_channels(self, layer_name: str, layer: dict[str, Any]) -> list[int | None]:
        return [self.allocations[key] for key, _route in self.layer_units(layer_name, layer)]

    def put_note(
        self,
        channel: int,
        row: int,
        note: int,
        instrument: CompiledInstrument,
        legato: bool,
        source_event: dict[str, Any],
    ) -> None:
        self.cancel_future_terminations(channel, row)
        arpeggio, hairpin, ceiling = compile_event_modifiers(source_event, instrument.level_ceiling)
        self.events[channel][row] = fur2ztr.Event(
            row=row % self.rows_per_bar,
            note=note,
            instrument=instrument.number,
            volume=15,
            legato=legato,
            arpeggio=arpeggio,
            hairpin=hairpin,
            hairpin_ceiling=ceiling,
        )

    def put_modifiers(
        self,
        channel: int,
        row: int,
        source_event: dict[str, Any],
        level_ceiling: int,
    ) -> None:
        arpeggio, hairpin, ceiling = compile_event_modifiers(source_event, level_ceiling)
        current = self.events[channel].get(row)
        if current is None:
            current = fur2ztr.Event(row % self.rows_per_bar)
            self.events[channel][row] = current
        if arpeggio is not None:
            current.arpeggio = arpeggio
        if hairpin is not None:
            current.hairpin = hairpin
            current.hairpin_ceiling = ceiling

    def put_release(self, channel: int, row: int) -> None:
        current = self.events[channel].get(row)
        if current is None:
            self.events[channel][row] = fur2ztr.Event(row % self.rows_per_bar, release=True)

    def put_off(self, channel: int, row: int) -> None:
        self.cancel_future_terminations(channel, row)
        current = self.events[channel].get(row)
        if current is None or current.note is None:
            self.events[channel][row] = fur2ztr.Event(row % self.rows_per_bar, note_off=True)

    def cancel_future_terminations(self, channel: int, row: int) -> None:
        for future_row, event in list(self.events[channel].items()):
            if future_row >= row and (event.note_off or event.release):
                del self.events[channel][future_row]

    def compile_layer(
        self,
        layer_name: str,
        layer: dict[str, Any],
        channels: list[int | None],
        validate_physical: bool = True,
    ) -> None:
        role = layer["source"]
        instrument_name = layer["instrument"]
        instrument = self.layer_instruments[layer_name]
        noise_layer = self.realizations[instrument_name].get("waveform") == "noise"
        for channel in (item for item in channels if item is not None):
            if not validate_physical:
                continue
            if (channel & 3) == 3 and not noise_layer:
                raise CsmCompileError(f"layer {layer_name}: square instrument is bound to a noise voice")
            if (channel & 3) != 3 and noise_layer:
                raise CsmCompileError(f"layer {layer_name}: noise instrument is bound to a tone voice")
        transpose = require_integer(layer, "transpose", f"layer {layer_name}", -128, 127, 0)
        delay_ticks = require_integer(layer, "delay", f"layer {layer_name}", 0, default=0)
        if delay_ticks % self.ticks_per_row:
            raise CsmCompileError(f"layer {layer_name}: delay must align to {self.ticks_per_row}-tick rows")
        delay_rows = delay_ticks // self.ticks_per_row
        instrument_gate = require_integer(self.instruments[instrument_name], "gate",
                                          f"instrument {instrument_name}", 1, default=None)
        gate_ticks = require_integer(layer, "gate", f"layer {layer_name}", 1, default=instrument_gate)
        if gate_ticks % self.ticks_per_row:
            raise CsmCompileError(f"layer {layer_name}: gate must align to {self.ticks_per_row}-tick rows")

        if role in self.song_ir["rhythms"]:
            rhythm_bars = expand_expression(self.song_ir["rhythms"][role]["expression"], self.patterns)
            if len(rhythm_bars) != 1:
                raise CsmCompileError(f"rhythm {role}: expected exactly one bar")
            bars: list[list[list[dict[str, Any]]] | None] = [rhythm_bars[0]] * self.form_bars
            realization = self.realizations[instrument_name]
            rate = NOISE_RATES[realization["rate"]]
            event_note = rate
        else:
            bars = self.role_bars(role)
            event_note = -1

        rr_index = 0
        chain_slot: int | None = None
        chain_until = -1
        audible_until = -1
        protected_until = -1
        release_rows = (
            (instrument.release_ticks + self.ticks_per_row - 1) // self.ticks_per_row
            if instrument.active_release else 0
        )
        for bar_index, bar in enumerate(bars):
            section, scene = self.plan[bar_index]
            if bar is None or layer_name not in self.enabled_layers(scene):
                continue
            slot_count = len(bar)
            if slot_count <= 0 or self.rows_per_bar % slot_count:
                raise CsmCompileError(f"{section}.{role}: slots do not divide the row grid")
            rows_per_slot = self.rows_per_bar // slot_count
            for slot_index, slot in enumerate(bar):
                if not slot:
                    continue
                if rows_per_slot % len(slot):
                    raise CsmCompileError(f"{section}.{role}: subdivision does not divide the row grid")
                rows_per_event = rows_per_slot // len(slot)
                for sub_index, event in enumerate(slot):
                    row = bar_index * self.rows_per_bar + slot_index * rows_per_slot + sub_index * rows_per_event
                    row += delay_rows
                    kind = event["kind"]
                    if kind == "continuation":
                        if chain_slot is None or row >= chain_until:
                            raise CsmCompileError(f"{section}.{role}: continuation has no sustained note")
                        continue
                    if kind == "modifier":
                        if chain_slot is None or row >= audible_until:
                            raise CsmCompileError(f"{section}.{role}: modifier has no sounding note")
                        channel = channels[chain_slot]
                        if channel is not None:
                            self.put_modifiers(channel, row, event, instrument.level_ceiling)
                        continue
                    if kind == "rest":
                        if row < protected_until:
                            raise CsmCompileError(f"{section}.{role}: silent rest overlaps a timed or legato note")
                        for channel in channels:
                            if channel is not None:
                                self.put_off(channel, row)
                        chain_slot = None
                        chain_until = row
                        audible_until = row
                        protected_until = row
                        continue
                    if kind not in ("note", "legato", "hit"):
                        raise CsmCompileError(f"{section}.{role}: unsupported event {kind}")
                    is_legato = kind == "legato"
                    if is_legato:
                        if role in self.song_ir["rhythms"]:
                            raise CsmCompileError(f"{section}.{role}: rhythm trigger cannot use pitched legato")
                        if chain_slot is None or row >= chain_until:
                            raise CsmCompileError(f"{section}.{role}: legato pitch has no sustained note")
                        selected_slot = chain_slot
                    else:
                        selected_slot = rr_index % len(channels)
                        rr_index += 1
                    channel = channels[selected_slot]
                    scene_transpose = self.scene_transpose(scene, role, layer_name)
                    note = event_note if kind == "hit" else note_number(
                        event["value"], transpose + scene_transpose, self.playback_transpose
                    )
                    duration = event.get("duration_ticks", gate_ticks)
                    if not isinstance(duration, int) or duration <= 0:
                        raise CsmCompileError(f"{section}.{role}: note duration must be a positive integer")
                    if duration % self.ticks_per_row:
                        raise CsmCompileError(
                            f"{section}.{role}: duration {duration} does not align to {self.ticks_per_row}-tick rows"
                        )
                    end_row = row + duration // self.ticks_per_row
                    chain_slot = selected_slot
                    chain_until = end_row
                    audible_until = end_row + release_rows
                    protected_until = end_row if is_legato or event.get("duration_ticks") is not None else row
                    if channel is None:
                        continue
                    self.put_note(channel, row, note, instrument, is_legato, event)
                    if instrument.active_release:
                        self.put_release(channel, end_row)
                        self.put_off(channel, end_row + release_rows)
                    else:
                        self.put_off(channel, end_row)

    def remove_redundant_state_events(self) -> None:
        """Keep OFF/release only when it changes the channel's note state."""
        for channel in range(fur2ztr.CHANNEL_COUNT):
            active = False
            released = False
            normalized: dict[int, fur2ztr.Event] = {}
            for row, event in sorted(self.events[channel].items()):
                if event.note is not None:
                    active = True
                    released = False
                    normalized[row] = event
                elif event.note_off:
                    if active:
                        normalized[row] = event
                        active = False
                        released = False
                elif event.release:
                    if active and not released:
                        normalized[row] = event
                        released = True
                else:
                    normalized[row] = event
            self.events[channel] = normalized

    def merge_layer_events(self, layer_name: str, layer_events: list[dict[int, fur2ztr.Event]]) -> None:
        for channel, events in enumerate(layer_events):
            for row, event in events.items():
                current = self.events[channel].get(row)
                if current is None:
                    self.events[channel][row] = event
                    if event.note is not None:
                        self.event_owners[channel][row] = layer_name
                    continue
                if current.note is not None and event.note is not None:
                    raise CsmCompileError(
                        f"allocator overlap at row {row}, channel {channel:02X}, layer {layer_name}"
                    )
                if event.note is not None:
                    self.events[channel][row] = event
                    self.event_owners[channel][row] = layer_name
                elif current.note is not None:
                    continue
                elif event.note_off or current.note_off:
                    self.events[channel][row] = fur2ztr.Event(row % self.rows_per_bar, note_off=True)
                elif event.release and not current.release:
                    self.events[channel][row] = event

    def mark_voice_handovers(self) -> None:
        """Reset only notation state that could leak to a new layer owner."""
        for channel, events in enumerate(self.events):
            active_owner: str | None = None
            arpeggio_active = False
            hairpin_active = False
            for row, event in sorted(events.items()):
                if event.note is not None:
                    owner = self.event_owners[channel].get(row)
                    if (
                        active_owner is not None
                        and owner is not None
                        and owner != active_owner
                        and (arpeggio_active or hairpin_active)
                    ):
                        event.mode_reset = True
                        arpeggio_active = False
                        hairpin_active = False
                    active_owner = owner

                if event.mode_reset:
                    arpeggio_active = False
                    hairpin_active = False
                if event.arpeggio is not None:
                    arpeggio_active = event.arpeggio >= 0
                if event.hairpin is not None and event.hairpin != 0:
                    hairpin_active = True

                if event.note_off:
                    active_owner = None
                    arpeggio_active = False
                    hairpin_active = False

    def build_song(self) -> fur2ztr.Song:
        self.compile_instruments()
        self.allocate_channels()
        for layer_name, layer in self.layers.items():
            channels = self.allocated_layer_channels(layer_name, layer)
            layer_events = self.compile_layer_events(layer_name, layer, channels, True)
            self.merge_layer_events(layer_name, layer_events)
        self.remove_redundant_state_events()
        self.mark_voice_handovers()

        form_rows = self.form_bars * self.rows_per_bar
        last_event_row = max((max(events, default=-1) for events in self.events), default=-1)
        if last_event_row >= form_rows:
            stop_row = last_event_row + 1
            self.events[0][stop_row] = fur2ztr.Event(
                stop_row % self.rows_per_bar,
                effect=(fur2ztr.EFFECTS[0xFF][0], 0),
            )
        final_row = max((max(events, default=-1) for events in self.events), default=-1) + 1
        total_rows = max(form_rows, final_row)
        total_bars = (total_rows + self.rows_per_bar - 1) // self.rows_per_bar

        orders_by_channel: list[list[int]] = []
        patterns: dict[tuple[int, int], list[fur2ztr.Event]] = {}
        for channel in range(fur2ztr.CHANNEL_COUNT):
            seen: dict[tuple[tuple[Any, ...], ...], int] = {}
            order: list[int] = []
            for bar in range(total_bars):
                start = bar * self.rows_per_bar
                bar_events = []
                for global_row in sorted(row for row in self.events[channel] if start <= row < start + self.rows_per_bar):
                    item = self.events[channel][global_row]
                    bar_events.append(item)
                key = tuple(
                    (
                        item.row,
                        item.note,
                        item.instrument,
                        item.volume,
                        item.effect,
                        item.note_off,
                        item.release,
                        item.legato,
                        item.mode_reset,
                        item.arpeggio,
                        item.hairpin,
                        item.hairpin_ceiling,
                    )
                    for item in bar_events
                )
                pattern = seen.get(key)
                if pattern is None:
                    pattern = len(seen)
                    if pattern > 254:
                        raise CsmCompileError(f"channel {channel}: more than 255 patterns")
                    seen[key] = pattern
                    patterns[(channel, pattern)] = bar_events
                order.append(pattern)
            orders_by_channel.append(order)
        orders = [[orders_by_channel[channel][bar] for channel in range(fur2ztr.CHANNEL_COUNT)]
                  for bar in range(total_bars)]

        return fur2ztr.Song(
            name=self.song_ir.get("name", "Untitled"),
            system="sn76489x4",
            tuning=self.tuning,
            playback_transpose=self.playback_transpose,
            tick_rate=self.tick_rate,
            speed=self.ticks_per_row,
            pattern_length=self.rows_per_bar,
            source_channels=fur2ztr.CHANNEL_COUNT,
            sound_chips=["TI SN76489"] * 4,
            orders=orders,
            patterns=patterns,
            instruments=sorted(self.ztr_instruments, key=lambda item: item.number),
        )


def compile_path(source: Path) -> tuple[fur2ztr.Song, bytes]:
    ir, diagnostics = language.compile_source(source)
    language.print_diagnostics(source, diagnostics)
    if diagnostics.error_count:
        raise CsmCompileError(f"CSM validation failed with {diagnostics.error_count} error(s)")
    compiler = Compiler(ir)
    song = compiler.build_song()
    data = fur2ztr.encode_ztr(song)
    if len(data) > compiler.max_ztr_bytes:
        raise CsmCompileError(
            f"ZTR output is {len(data)} bytes; target limit is {compiler.max_ztr_bytes}"
        )
    decoded = fur2ztr.decode_ztr(data)
    fur2ztr.validate_round_trip(song, decoded.song)
    return song, data


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="CSM source file")
    parser.add_argument("output", nargs="?", type=Path, help="ZTR output (default: SOURCE.ztr)")
    parser.add_argument("--dump", action="store_true", help="dump the generated ZTR after compilation")
    parser.add_argument(
        "--allocation-report",
        action="store_true",
        help="show logical layer-unit to physical voice assignments",
    )
    args = parser.parse_args(argv)
    output = args.output or args.source.with_suffix(".ztr")
    try:
        ir, diagnostics = language.compile_source(args.source)
        language.print_diagnostics(args.source, diagnostics)
        if diagnostics.error_count:
            raise CsmCompileError(f"CSM validation failed with {diagnostics.error_count} error(s)")
        compiler = Compiler(ir)
        song = compiler.build_song()
        data = fur2ztr.encode_ztr(song)
        if len(data) > compiler.max_ztr_bytes:
            raise CsmCompileError(
                f"ZTR output is {len(data)} bytes; target limit is {compiler.max_ztr_bytes}"
            )
        fur2ztr.validate_round_trip(song, fur2ztr.decode_ztr(data).song)
        output.write_bytes(data)
        fur2ztr.print_summary(song, len(data))
        dropped = [item for item in compiler.allocation_messages if item.startswith("dropped")]
        print(f"allocation: {len(compiler.allocation_units) - len(dropped)} assigned, {len(dropped)} dropped")
        if args.allocation_report:
            for item in compiler.allocation_messages:
                print(f"  {item}")
        print(f"wrote {output}")
        if args.dump:
            fur2ztr.dump_ztr(fur2ztr.decode_ztr(data))
        return 0
    except (CsmCompileError, fur2ztr.ConversionError, OSError, KeyError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
