#!/usr/bin/env python3
"""Generate the C header textbench.c includes: the pinned CP850 6x8 font.

The direct-write renderers copy glyph bytes from CPU RAM, so the bench
program embeds the same 2,048-byte asset the V9958 driver uploads as its
atlas (`drivers/v9958/assets/font_cp850_6x8.inc`, packed by
tools/fontpack.py).  The header is generated into the program's object
directory from the pinned asset - one source for the font.

Usage: python3 tests/bench/font2c.py <font.inc> <output.h>
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import fontpack  # noqa: E402


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    font = fontpack.parse_font(argv[1])
    if len(font) != 2048:
        print("font2c.py: %s: expected 2048 bytes, got %d"
              % (argv[1], len(font)), file=sys.stderr)
        return 1
    lines = ["/* Generated from %s - do not edit. */"
             % os.path.basename(argv[1]),
             "#ifndef TEXTBENCH_FONT_H",
             "#define TEXTBENCH_FONT_H",
             "",
             "/* The pinned CP850 6x8 bitmap: 8 bytes per glyph, bits 7..2",
             " * of a row byte are the six pixels. */",
             "const unsigned char textbench_font[2048] = {"]
    for at in range(0, 2048, 16):
        lines.append("\t" + ", ".join("0x%02Xu" % value
                                      for value in font[at:at + 16]) + ",")
    lines += ["};", "", "#endif /* TEXTBENCH_FONT_H */", ""]
    with open(argv[2], "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
