#!/usr/bin/env python3
"""Deterministic malformed-ZEX-v2 fixture generator (subsystem test artifact).

Builds one fixture per ZEX v2 validation boundary in
include/zephyros/zex.h plus two well-formed control files:

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
    zex-seg-count.zex          ZEX_EXEC_ERR_SEG_SLOTS   (9 rows)
    zex-seg-slots.zex          ZEX_EXEC_ERR_SEG_SLOTS   (21 slots)
    zex-seg-total.zex          ZEX_EXEC_ERR_SEG_SLOTS   (header total wrong)
    zex-seg-table.zex          ZEX_EXEC_ERR_SEG_TABLE   (table outside file)
    zex-seg-kind.zex           ZEX_EXEC_ERR_SEG_KIND    (FAR_CODE, 44b)
    zex-seg-payload.zex        ZEX_EXEC_ERR_SEGMENT     (payload outside file)
    zex-seg-id.zex             ZEX_EXEC_ERR_SEGMENT     (ids not 1..N)
    zex-seg-reserved.zex       ZEX_EXEC_ERR_SEGMENT     (row reserved set)
    zex-wrong-type.dat         (valid bytes, packed as ROMFS DATA)
    zex-return.zex             (valid; entry executes a raw RET)

The host tests import build_fixtures() and assert that zexpack rejects
every malformed file with the boundary code listed here; the subsystem
target test loads the same files from ROMFS through process_exec() and
asserts the kernel's codes match.

Usage:
    gen_fixtures.py <outdir>
"""

import sys
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent.parent.parent / "tools"
sys.path.insert(0, str(TOOLS_DIR))
import zexpack  # noqa: E402

# One genuine segment payload, so the malformed table variants have a real
# row and payload to break.
SEG_PAYLOAD = bytes([0x5A, 0x69, 0xA5, 0xC3])


def _image(size=4, fill=0x00):
    """Deterministic image: a single RET followed by NOPs.  The malformed
    fixtures never execute; the control fixtures may."""
    return bytes([0xC9] + [fill] * (size - 1))


def _container(fields, image, segments=(), payloads=(), table_bias=0):
    """Lay out header + image + table + payloads like the packer.

    `fields` are raw header values except the derived offsets and
    file_size, which this helper computes.  `table_bias` shifts
    segment_table_offset (the out-of-file table fixture).
    """
    fields = dict(fields)
    fields["segment_count"] = len(segments)
    if fields.get("total_segment_slots") is None:
        fields["total_segment_slots"] = sum(
            zexpack.segment_slots(row["image_size"], row["bss_size"])
            for row in segments)
    fields["segment_table_offset"] = (
        zexpack.HEADER_SIZE + len(image) + table_bias)
    fields["primary_payload_offset"] = zexpack.HEADER_SIZE
    cursor = (zexpack.HEADER_SIZE + len(image) +
              len(segments) * zexpack.SEGMENT_ENTRY_SIZE)
    rows = []
    for row, payload in zip(segments, payloads):
        row = dict(row)
        cursor = zexpack.align(cursor, zexpack.PAYLOAD_ALIGN)
        row["payload_offset"] = cursor
        cursor += len(payload)
        rows.append((row, payload))
    body = bytearray(zexpack.build_header(fields))
    body += image
    for row, _payload in rows:
        body += zexpack.build_segment_row(row)
    for row, payload in rows:
        while len(body) < row["payload_offset"]:
            body += b"\x00"
        body += payload
    fields["file_size"] = len(body)
    header = zexpack.build_header(fields)
    return bytes(header + body[len(header):])


def _fields(**overrides):
    fields = zexpack.base_fields()
    fields["file_size"] = 0
    fields["total_segment_slots"] = None
    fields.update(overrides)
    return fields


def _row(**overrides):
    row = zexpack.base_segment_row()
    row["reserved0"] = 0
    row["reserved1"] = 0
    row.update(overrides)
    return row


def _valid_segment(image_size=None, **overrides):
    if image_size is None:
        image_size = len(SEG_PAYLOAD)
    return _row(image_size=image_size, **overrides)


def build_fixtures():
    """Return [{'name','data','tool_code','kernel_code'}] (deterministic).

    tool_code is the zexpack rejection reason (None for the well-formed
    control files); kernel_code is what process_exec() must return when
    the file is packed into ROMFS (None for the raw-return control).
    """
    big_image = _image(0xDF00)  # 57,088 deterministic bytes

    def simple():
        fields = _fields()
        return _container(fields, _image(fields["image_size"]))

    truncated = simple()[:-1]
    padded = simple() + b"\x00"

    entries = [
        ("zex-bad-magic.zex",
         _container(_fields(magic=b"ZEXX"), _image(4)), "ZEX_EXEC_ERR_MAGIC"),
        ("zex-bad-format.zex",
         _container(_fields(format_version=3), _image(4)),
         "ZEX_EXEC_ERR_FORMAT"),
        ("zex-bad-abi.zex",
         _container(_fields(abi_version=2), _image(4)), "ZEX_EXEC_ERR_ABI"),
        ("zex-bad-flags.zex",
         _container(_fields(flags=1), _image(4)), "ZEX_EXEC_ERR_FLAGS"),
        ("zex-bad-reserved0.zex",
         _container(_fields(reserved0=1), _image(4)),
         "ZEX_EXEC_ERR_RESERVED0"),
        ("zex-bad-header-size.zex",
         _container(_fields(header_size=34), _image(4)),
         "ZEX_EXEC_ERR_HEADER"),
        ("zex-bad-reserved1.zex",
         _container(_fields(reserved1=1), _image(4)),
         "ZEX_EXEC_ERR_RESERVED1"),
        ("zex-zero-image.zex",
         _container(_fields(image_size=0), b""), "ZEX_EXEC_ERR_IMAGE"),
        ("zex-truncated.zex", truncated, "ZEX_EXEC_ERR_FILESIZE"),
        ("zex-padded.zex", padded, "ZEX_EXEC_ERR_FILESIZE"),
        ("zex-bad-entry.zex",
         _container(_fields(entry_offset=4), _image(4)),
         "ZEX_EXEC_ERR_ENTRY"),
        ("zex-small-stack.zex",
         _container(_fields(stack_size=1), _image(4)),
         "ZEX_EXEC_ERR_STACK"),
        ("zex-image-end.zex",
         _container(_fields(image_size=0xDF01), big_image + b"\x00"),
         "ZEX_EXEC_ERR_IMAGE_END"),
        ("zex-bss-end.zex",
         _container(_fields(image_size=0xDF00, bss_size=2, stack_size=2),
                    big_image),
         "ZEX_EXEC_ERR_BSS_END"),
        ("zex-stack-collision.zex",
         _container(_fields(image_size=16, bss_size=16, stack_size=0xDEF0),
                    _image(16)),
         "ZEX_EXEC_ERR_COLLISION"),
        # Segment-table boundaries.  The base container is valid; each
        # variant breaks exactly one property.
        ("zex-seg-count.zex",
         _container(_fields(segment_count=9), _image(4),
                    [_valid_segment(id=i + 1) for i in range(9)],
                    [SEG_PAYLOAD] * 9),
         "ZEX_EXEC_ERR_SEG_SLOTS"),
        ("zex-seg-slots.zex",
         _container(_fields(total_segment_slots=21), _image(4),
                    [_valid_segment()], [SEG_PAYLOAD]),
         "ZEX_EXEC_ERR_SEG_SLOTS"),
        ("zex-seg-total.zex",
         _container(_fields(total_segment_slots=3), _image(4),
                    [_valid_segment()], [SEG_PAYLOAD]),
         "ZEX_EXEC_ERR_SEG_SLOTS"),
        ("zex-seg-table.zex",
         _container(_fields(), _image(4), [_valid_segment()],
                    [SEG_PAYLOAD], table_bias=8),
         "ZEX_EXEC_ERR_SEG_TABLE"),
        ("zex-seg-kind.zex",
         _container(_fields(), _image(4),
                    [_valid_segment(kind=zexpack.KIND_FAR_CODE)],
                    [SEG_PAYLOAD]),
         "ZEX_EXEC_ERR_SEG_KIND"),
        ("zex-seg-payload.zex",
         _container(_fields(), _image(4),
                    [_valid_segment(image_size=64)], [SEG_PAYLOAD]),
         "ZEX_EXEC_ERR_SEGMENT"),
        ("zex-seg-id.zex",
         _container(_fields(), _image(4), [_valid_segment(id=2)],
                    [SEG_PAYLOAD]),
         "ZEX_EXEC_ERR_SEGMENT"),
        ("zex-seg-reserved.zex",
         _container(_fields(), _image(4),
                    [_valid_segment(reserved0=1)], [SEG_PAYLOAD]),
         "ZEX_EXEC_ERR_SEGMENT"),
        ("zex-wrong-type.dat", _container(_fields(), _image(4)), None),
        ("zex-return.zex", _container(_fields(), _image(4)), None),
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
