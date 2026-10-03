#!/usr/bin/env python3
"""V9958 CP850 6x8 font asset pipeline (work package 18).

The reviewable source asset is drivers/v9958/assets/font_cp850_6x8.inc:
the exact 2048-byte font imported from the pinned Zephyr-80 reference
(commit 2c6b9650).  This tool parses its `.db` byte rows and emits the
deterministic ROMFS DATA payload `v9958-6x8.font`:

    fontpack.py <source.inc> <output.font>   pack + verify
    fontpack.py verify <source.inc>          parse + verify only

The pinned SHA-256 of the 2048-byte payload is
6db207e0466a05af42894d378261f876da51bb391240e1beda4f1d5f12ec442f;
a mismatch is an error, never a warning.  The payload never enters the
resident ZMOD module: the driver reads it from ROMFS in bounded chunks.
"""

import hashlib
import re
import sys
from pathlib import Path

FONT_SIZE = 2048
GLYPHS = 256
GLYPH_BYTES = 8
PINNED_SHA256 = (
    "6db207e0466a05af42894d378261f876da51bb391240e1beda4f1d5f12ec442f"
)

DB_RE = re.compile(r"^\s*\.db\s+(.+)$")


class FontError(Exception):
    pass


def parse_font(path):
    """Parse the .db rows of the source asset into bytes."""
    values = []
    rows = 0
    with open(path, "r", encoding="utf-8") as handle:
        for lineno, line in enumerate(handle, 1):
            match = DB_RE.match(line)
            if not match:
                continue
            rows += 1
            for field in match.group(1).split(","):
                field = field.strip()
                if not field:
                    continue
                try:
                    value = int(field, 0)
                except ValueError:
                    raise FontError(
                        "%s:%d: bad byte %r" % (path, lineno, field))
                if not 0 <= value <= 0xFF:
                    raise FontError(
                        "%s:%d: byte out of range %r" % (path, lineno, field))
                values.append(value)
    if rows != GLYPHS:
        raise FontError(
            "%s: expected %d .db rows, found %d" % (path, GLYPHS, rows))
    if len(values) != FONT_SIZE:
        raise FontError(
            "%s: expected %d bytes, found %d" % (path, FONT_SIZE, len(values)))
    return bytes(values)


def verify(data, path):
    digest = hashlib.sha256(data).hexdigest()
    if digest != PINNED_SHA256:
        raise FontError(
            "%s: SHA-256 %s does not match the pinned reference %s"
            % (path, digest, PINNED_SHA256))
    return digest


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    command = argv[1]
    if command == "verify":
        if len(argv) != 3:
            print(__doc__)
            return 2
        source = argv[2]
        try:
            data = parse_font(source)
            digest = verify(data, source)
        except FontError as error:
            print("fontpack: FAIL - %s" % error, file=sys.stderr)
            return 1
        print("fontpack: %s OK (%d bytes, sha256 %s)"
              % (source, len(data), digest))
        return 0
    if command.startswith("-") or len(argv) != 3:
        print(__doc__)
        return 2
    source = command
    output = argv[2]
    try:
        data = parse_font(source)
        digest = verify(data, source)
    except FontError as error:
        print("fontpack: FAIL - %s" % error, file=sys.stderr)
        return 1
    Path(output).write_bytes(data)
    written = hashlib.sha256(Path(output).read_bytes()).hexdigest()
    if written != digest:
        print("fontpack: FAIL - written output does not round-trip",
              file=sys.stderr)
        return 1
    print("fontpack: %s -> %s (%d bytes, sha256 %s)"
          % (source, output, len(data), digest))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
