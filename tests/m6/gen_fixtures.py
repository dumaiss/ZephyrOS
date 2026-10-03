#!/usr/bin/env python3
"""Deterministic malformed-ZEX fixture generator (M6 test artifact).

Builds one fixture per validation boundary in include/zephyros/zex.h plus
two well-formed control files:

    zex-bad-magic.zex          ZEX_EXEC_ERR_MAGIC
    zex-bad-format.zex         ZEX_EXEC_ERR_FORMAT
    zex-bad-abi.zex            ZEX_EXEC_ERR_ABI
    zex-bad-flags.zex          ZEX_EXEC_ERR_FLAGS
    zex-bad-reserved0.zex      ZEX_EXEC_ERR_RESERVED0
    zex-bad-header-size.zex    ZEX_EXEC_ERR_HEADER
    zex-bad-reserved1.zex      ZEX_EXEC_ERR_RESERVED1
    zex-zero-image.zex         ZEX_EXEC_ERR_IMAGE
    zex-truncated.zex          ZEX_EXEC_ERR_FILESIZE
    zex-padded.zex             ZEX_EXEC_ERR_FILESIZE
    zex-bad-entry.zex          ZEX_EXEC_ERR_ENTRY
    zex-small-stack.zex        ZEX_EXEC_ERR_STACK
    zex-image-end.zex          ZEX_EXEC_ERR_IMAGE_END
    zex-bss-end.zex            ZEX_EXEC_ERR_BSS_END
    zex-stack-collision.zex    ZEX_EXEC_ERR_COLLISION
    zex-wrong-type.dat         (valid bytes, packed as ROMFS DATA)
    zex-return.zex             (valid; entry executes a raw RET)

The host tests import build_fixtures() and assert that zexpack rejects
every malformed file with the boundary code listed here; the M6 target
test loads the same files from ROMFS through process_exec() and asserts
the kernel's codes match.

Usage:
    gen_fixtures.py <outdir>
"""

import sys
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent.parent.parent / "tools"
sys.path.insert(0, str(TOOLS_DIR))
import zexpack  # noqa: E402

# Deterministic image: a single RET followed by NOPs.  The malformed
# fixtures never execute; the control fixtures may.
def _image(size=4):
    return bytes([0xC9] + [0x00] * (size - 1))


def _fixture(magic=zexpack.MAGIC, format_version=zexpack.FORMAT_VERSION,
             abi_version=zexpack.ABI_VERSION, flags=zexpack.FLAGS_NONE,
             reserved0=0, header_size=zexpack.HEADER_SIZE, image_size=4,
             bss_size=4, entry_offset=0, stack_size=16, reserved1=0,
             image=None, trailing=b"", truncate=0):
    fields = zexpack.base_fields()
    fields.update({
        "magic": magic,
        "format_version": format_version,
        "abi_version": abi_version,
        "flags": flags,
        "reserved0": reserved0,
        "header_size": header_size,
        "image_size": image_size,
        "bss_size": bss_size,
        "entry_offset": entry_offset,
        "stack_size": stack_size,
        "reserved1": reserved1,
    })
    body = zexpack.build_header(fields)
    if image is None:
        image = _image(image_size)
    body += image
    body += trailing
    if truncate:
        body = body[:len(body) - truncate]
    return body


def build_fixtures():
    """Return [{'name','data','tool_code','kernel_code'}] (deterministic).

    tool_code is the zexpack rejection reason (None for the well-formed
    control files); kernel_code is what process_exec() must return when
    the file is packed into ROMFS (None for the raw-return control).
    """
    big_image = _image(0xDF00)  # 57,088 deterministic bytes
    entries = [
        ("zex-bad-magic.zex",
         _fixture(magic=b"ZEXX"), "ZEX_EXEC_ERR_MAGIC"),
        ("zex-bad-format.zex",
         _fixture(format_version=2), "ZEX_EXEC_ERR_FORMAT"),
        ("zex-bad-abi.zex",
         _fixture(abi_version=2), "ZEX_EXEC_ERR_ABI"),
        ("zex-bad-flags.zex",
         _fixture(flags=1), "ZEX_EXEC_ERR_FLAGS"),
        ("zex-bad-reserved0.zex",
         _fixture(reserved0=1), "ZEX_EXEC_ERR_RESERVED0"),
        ("zex-bad-header-size.zex",
         _fixture(header_size=18), "ZEX_EXEC_ERR_HEADER"),
        ("zex-bad-reserved1.zex",
         _fixture(reserved1=1), "ZEX_EXEC_ERR_RESERVED1"),
        ("zex-zero-image.zex",
         _fixture(image_size=0, image=b""), "ZEX_EXEC_ERR_IMAGE"),
        ("zex-truncated.zex",
         _fixture(image=_image(8), truncate=1), "ZEX_EXEC_ERR_FILESIZE"),
        ("zex-padded.zex",
         _fixture(trailing=b"\x00"), "ZEX_EXEC_ERR_FILESIZE"),
        ("zex-bad-entry.zex",
         _fixture(entry_offset=4), "ZEX_EXEC_ERR_ENTRY"),
        ("zex-small-stack.zex",
         _fixture(stack_size=1), "ZEX_EXEC_ERR_STACK"),
        ("zex-image-end.zex",
         _fixture(image_size=0xDF01, image=big_image + b"\x00"),
         "ZEX_EXEC_ERR_IMAGE_END"),
        ("zex-bss-end.zex",
         _fixture(image_size=0xDF00, bss_size=2, stack_size=2,
                  image=big_image),
         "ZEX_EXEC_ERR_BSS_END"),
        ("zex-stack-collision.zex",
         _fixture(image_size=16, bss_size=16, stack_size=0xDEF0,
                  image=_image(16)),
         "ZEX_EXEC_ERR_COLLISION"),
        ("zex-wrong-type.dat", _fixture(), None),
        ("zex-return.zex", _fixture(), None),
    ]
    fixtures = []
    for name, data, code in entries:
        kernel_code = code
        if name == "zex-wrong-type.dat":
            kernel_code = "ZEX_EXEC_ERR_TYPE"
        elif name == "zex-return.zex":
            kernel_code = None
        fixtures.append({"name": name, "data": data,
                         "tool_code": code, "kernel_code": kernel_code})
    return fixtures


def main(argv):
    if len(argv) != 1:
        print(__doc__)
        return 2
    outdir = Path(argv[0])
    outdir.mkdir(parents=True, exist_ok=True)
    fixtures = build_fixtures()
    for fixture in fixtures:
        (outdir / fixture["name"]).write_bytes(fixture["data"])
    print("gen_fixtures: wrote %d fixtures to %s" % (len(fixtures), outdir))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
