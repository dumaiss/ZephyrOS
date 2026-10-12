#!/usr/bin/env python3
"""Deterministic EXE v2 DATA-segment payloads (package 44a test artifact).

Writes the payload files and the segment declaration list for the
loader-outcome programs the subsystem process selftest runs:

    seg <objdir>    exe2seg-seg1.bin + exe2seg.segments.json
                    one segment: 1024-byte image, 64-byte BSS
    big <objdir>    exe2big-seg1.bin, exe2big-seg2.bin +
                    exe2big.segments.json
                    two segments: 32760-byte images, 8-byte BSS each, so
                    the container exceeds 64 KiB

A program that also builds far code declares its far segments first:
ids 1-4 are the `FAR1=`..`FAR4=` segments (package 49b), in order.

The payload pattern is `(i * MUL + ADD) & 0xFF` with one multiplier/addend
per segment; the C programs (tests/target/process/) compute the same
bytes, and tests/target/include/process_selftest.h pins the sizes both
sides use.

Usage:
    gen_exe2_segments.py seg|big <objdir>
"""

import json
import sys
from pathlib import Path

# (image_size, bss_size, multiplier, addend) per segment of each program.
PROGRAMS = {
    "seg": {
        "name": "exe2seg",
        "segments": [(1024, 64, 7, 0x13)],
    },
    "big": {
        "name": "exe2big",
        "segments": [(32760, 8, 11, 0x29), (32760, 8, 13, 0x5B)],
    },
}


def payload(image_size, multiplier, addend):
    return bytes(((i * multiplier + addend) & 0xFF) for i in range(image_size))


def main(argv):
    if len(argv) != 2 or argv[0] not in PROGRAMS:
        print(__doc__)
        return 2
    program = PROGRAMS[argv[0]]
    outdir = Path(argv[1])
    outdir.mkdir(parents=True, exist_ok=True)
    declared = []
    for index, (image_size, bss_size, multiplier, addend) in enumerate(
            program["segments"]):
        payload_name = "%s-seg%d.bin" % (program["name"], index + 1)
        (outdir / payload_name).write_bytes(
            payload(image_size, multiplier, addend))
        declared.append({
            "id": index + 1,
            "kind": "DATA",
            "input": payload_name,
            "bss_size": bss_size,
        })
        print("gen_exe2_segments: %s: %d bytes (mul %d add 0x%02X, bss %d)"
              % (outdir / payload_name, image_size, multiplier, addend,
                 bss_size))
    manifest = outdir / ("%s.segments.json" % program["name"])
    manifest.write_text(json.dumps(declared, indent=2) + "\n")
    print("gen_exe2_segments: %s: %d segments" % (manifest, len(declared)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
