#!/usr/bin/env python3
"""Generate the deterministic cross-page pattern asset (test artifact).

The asset is KOS_RFST_CROSSPAGE_SIZE (0xC040) bytes:

    byte i = ((i & 0xFF) ^ ((i >> 8) & 0xFF)) & 0xFF

Packed into the subsystem acceptance ROMFS it fills the first recovery
window and crosses the 48-KiB window boundary, so a bounded romfs_read()
inside the asset exercises the page transition.
tests/target/romfs/romfs_test.c verifies the same formula on target;
tests/target/include/romfs_selftest.h pins the size.
"""

import sys
from pathlib import Path

SIZE = 0xC040


def pattern(size=SIZE):
    return bytes(((i & 0xFF) ^ ((i >> 8) & 0xFF)) & 0xFF for i in range(size))


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    data = pattern()
    Path(argv[1]).write_bytes(data)
    print("gen_asset: %s (%d bytes)" % (argv[1], len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
