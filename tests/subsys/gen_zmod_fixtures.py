#!/usr/bin/env python3
"""Deterministic malformed-ZMOD fixture generator (test artifact).

The subsystem acceptance image loads its modules through the same
`vfs_file_source` + `module_load_source` path as production, so the
loader's container validation can only be exercised by packing damaged
containers into the acceptance recovery store.  Both fixtures derive
from the reviewed `m4_probe.zmod` bytes:

    badmagic.zmod    valid length, "ZMOD" magic overwritten
    truncated.zmod   the first header-sized prefix of the container

`tests/target/loader/loader_test.c` loads both and requires the exact
-ZOS_EIO (ZMOD_EFORMAT) rejection with no arena or registry change.

Usage: gen_zmod_fixtures.py <objdir>
"""

import sys
from pathlib import Path

SOURCE = "m4_probe.zmod"
BAD_MAGIC = "badmagic.zmod"
TRUNCATED = "truncated.zmod"
HEADER_SIZE = 0x1C


def build_fixtures(objdir):
    """Return {name: bytes} for the malformed containers."""
    data = Path(objdir, SOURCE).read_bytes()
    bad = bytearray(data)
    bad[0:4] = b"\x00\x00\x00\x00"
    truncated = data[:HEADER_SIZE]
    return {BAD_MAGIC: bytes(bad), TRUNCATED: truncated}


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    objdir = Path(argv[1])
    for name, data in sorted(build_fixtures(objdir).items()):
        Path(objdir, name).write_bytes(data)
        print("gen_zmod_fixtures: %s: %d bytes" % (name, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
