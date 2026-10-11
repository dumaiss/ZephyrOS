#!/usr/bin/env python3
"""Generate a ZEX v2 manifest from an sdldz80 link map (work package 28c).

The generic `zex_program` rule in user/sdk/zex.mk links a program, then
calls this tool to derive `image_size` and `bss_size` from the map.  No
size is ever hand-entered:

    image_size = s__DATA - LOAD_BASE     (the image ends where BSS begins)
    bss_size   = l__DATA                 (the linker's zero-fill length)
    entry_offset = <entry symbol> - LOAD_BASE

The stack size and scratch symbol come from the rule's options or the
SDK defaults.  The output JSON uses the exact key set tools/zexpack.py
accepts.  `--segments FILE` merges a JSON list of auxiliary segment
declarations (id/kind/input/bss_size; the packer resolves the payload
against the same objdir); the package-44b far-code area tooling extends
this when far code lands.

Usage:
    zexmanifest.py MAP OUT --name NAME [--entry SYM] [--stack N]
                   [--scratch SYM:SIZE] [--input FILE] [--segments FILE]

Reads the map, writes the manifest.  Exit 1 on a geometry error.
"""

import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from zexpack import parse_symbols  # noqa: E402

LOAD_BASE = 0x0100
STACK_TOP = 0xE000


class ManifestError(Exception):
    pass


def build_manifest(map_path, name, entry, stack_size, scratch, input_name=None,
                   segments_path=None):
    text = Path(map_path).read_text()
    symbols = parse_symbols(text)
    for key in ("s__DATA", "l__DATA", "l__CODE"):
        if key not in symbols:
            raise ManifestError("%s: map has no %s symbol" % (map_path, key))
    if entry not in symbols:
        raise ManifestError("%s: map has no entry symbol %r" % (map_path, entry))
    s_data = symbols["s__DATA"]
    image_size = s_data - LOAD_BASE
    bss_size = symbols["l__DATA"]
    entry_offset = symbols[entry] - LOAD_BASE
    if image_size <= 0:
        raise ManifestError("image_size must be positive (got %d)" % image_size)
    if symbols["l__CODE"] > image_size:
        raise ManifestError(
            "code %d bytes overflows the image %d bytes"
            % (symbols["l__CODE"], image_size))
    if entry_offset < 0 or entry_offset >= image_size:
        raise ManifestError(
            "entry %s at %04Xh is outside the image" % (entry, symbols[entry]))
    if bss_size < 0:
        raise ManifestError("bss_size must be non-negative")
    if LOAD_BASE + image_size + bss_size > STACK_TOP:
        raise ManifestError("image + BSS collide with the stack top")

    manifest = {
        "format": "zex-v2",
        "name": name,
        "input": input_name or (os.path.splitext(name)[0] + ".ihx"),
        "map": os.path.basename(map_path),
        "entry": entry,
        "entry_offset": entry_offset,
        "image_size": image_size,
        "bss_size": bss_size,
        "stack_size": int(stack_size),
    }
    if scratch is not None:
        if ":" not in scratch:
            raise ManifestError("scratch must be SYMBOL:SIZE")
        sym, size = scratch.rsplit(":", 1)
        try:
            size = int(size, 0)
        except ValueError:
            raise ManifestError("scratch size %r is not an integer" % size)
        if sym not in symbols:
            raise ManifestError("scratch symbol %r is missing from the map" % sym)
        addr = symbols[sym]
        if addr < LOAD_BASE or addr + size > 0x2000:
            raise ManifestError(
                "scratch %s at %04Xh size %d leaves the 0100h-1FFFh window"
                % (sym, addr, size))
        manifest["scratch"] = {"symbol": sym, "size": size}
    if segments_path is not None:
        try:
            raw = json.loads(Path(segments_path).read_text())
        except OSError as exc:
            raise ManifestError("cannot read segments: %s" % exc)
        except ValueError as exc:
            raise ManifestError("segments is not valid JSON: %s" % exc)
        if not isinstance(raw, list):
            raise ManifestError("segments must be a JSON list")
        manifest["segments"] = raw
    return manifest


def main(argv):
    parser = argparse.ArgumentParser(prog="zexmanifest.py")
    parser.add_argument("map")
    parser.add_argument("output")
    parser.add_argument("--name", required=True)
    parser.add_argument("--entry", default="zos_entry")
    parser.add_argument("--stack", type=int, default=256)
    parser.add_argument("--scratch", default=None)
    parser.add_argument("--input", default=None)
    parser.add_argument("--segments", default=None)
    args = parser.parse_args(argv)
    try:
        manifest = build_manifest(args.map, args.name, args.entry,
                                  args.stack, args.scratch, args.input,
                                  args.segments)
    except (ManifestError, OSError, ValueError) as exc:
        print("zexmanifest: %s" % exc, file=sys.stderr)
        return 1
    Path(args.output).write_text(json.dumps(manifest, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
