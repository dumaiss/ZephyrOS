#!/usr/bin/env python3
"""Create the M5 corrupt-metadata recovery image (test artifact).

Copies a packed recovery stream and sets the header `reserved` field
(offset 10, little-endian u16) to 1.  kernel/romfs/romfs.c must reject the
image during romfs_init(), before any module execution, and print the
precise early-console panic marker.  The build intentionally skips
romfspack validation for this image: the corruption is the point.
"""

import sys
from pathlib import Path

MAGIC = b"ZRFS"
RESERVED_OFFSET = 10


def corrupt(data):
    if len(data) < 16 or data[0:4] != MAGIC:
        raise ValueError("input is not a ROMFS stream")
    if data[RESERVED_OFFSET] != 0:
        raise ValueError("header reserved field is already nonzero")
    data = bytearray(data)
    data[RESERVED_OFFSET] = 1
    return bytes(data)


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    try:
        data = corrupt(Path(argv[1]).read_bytes())
    except ValueError as exc:
        print("corrupt_romfs: %s" % exc, file=sys.stderr)
        return 1
    Path(argv[2]).write_bytes(data)
    print("corrupt_romfs: %s -> %s (header reserved := 1)"
          % (argv[1], argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
