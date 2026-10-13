#!/usr/bin/env python3
"""Deterministic malformed-EXE-v2 fixture generator (subsystem test artifact).

Builds one fixture per EXE v2 validation boundary in
formats/include/exe.h plus two well-formed control files:

    exe-bad-magic.exe          EXE_EXEC_ERR_MAGIC
    exe-bad-format.exe         EXE_EXEC_ERR_FORMAT
    exe-bad-abi.exe            EXE_EXEC_ERR_ABI
    exe-bad-flags.exe          EXE_EXEC_ERR_FLAGS
    exe-bad-reserved0.exe      EXE_EXEC_ERR_RESERVED0
    exe-bad-header-size.exe    EXE_EXEC_ERR_HEADER
    exe-bad-reserved1.exe      EXE_EXEC_ERR_RESERVED1
    exe-zero-image.exe         EXE_EXEC_ERR_IMAGE
    exe-truncated.exe          EXE_EXEC_ERR_FILESIZE
    exe-padded.exe             EXE_EXEC_ERR_FILESIZE
    exe-bad-entry.exe          EXE_EXEC_ERR_ENTRY
    exe-small-stack.exe        EXE_EXEC_ERR_STACK
    exe-image-end.exe          EXE_EXEC_ERR_IMAGE_END
    exe-bss-end.exe            EXE_EXEC_ERR_BSS_END
    exe-stack-collision.exe    EXE_EXEC_ERR_COLLISION
    exe-seg-count.exe          EXE_EXEC_ERR_SEG_SLOTS   (9 rows)
    exe-seg-slots.exe          EXE_EXEC_ERR_SEG_SLOTS   (21 slots)
    exe-seg-total.exe          EXE_EXEC_ERR_SEG_SLOTS   (header total wrong)
    exe-seg-table.exe          EXE_EXEC_ERR_SEG_TABLE   (table outside file)
    exe-seg-kind.exe          EXE_EXEC_ERR_SEG_KIND    (unknown kind)
    exe-far-oversize.exe       EXE_EXEC_ERR_SEGMENT     (FAR_CODE over 16 KiB)
    exe-seg-payload.exe        EXE_EXEC_ERR_SEGMENT     (payload outside file)
    exe-seg-id.exe             EXE_EXEC_ERR_SEGMENT     (ids not 1..N)
    exe-seg-flags.exe          EXE_EXEC_ERR_SEGMENT     (row flag bit outside MAPPABLE)
    exe-wrong-type.dat         (valid bytes, packed as ROMFS DATA)
    exe-return.exe             (valid; entry executes a raw RET)

The host tests import build_fixtures() and assert that exepack rejects
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
import exepack  # noqa: E402

# One genuine segment payload, so the malformed table variants have a real
# row and payload to break.
SEG_PAYLOAD = bytes([0x5A, 0x69, 0xA5, 0xC3])


def _image(size=4, fill=0x00):
    """Deterministic image: a single RET followed by NOPs.  The malformed
    fixtures never execute; the control fixtures may."""
    return bytes([0xC9] + [fill] * (size - 1))


def old_magic_container():
    """A well-formed container carrying the pre-rename magic bytes.  The
    bytes are spelled in hex so the retired token cannot reappear in the
    tree; the format tests assert the bad-magic boundary rejects them."""
    return _container(_fields(magic=bytes([0x5A, 0x45, 0x58, 0x32])),
                      _image(4))


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
            exepack.segment_slots(row["image_size"], row["bss_size"])
            for row in segments)
    fields["segment_table_offset"] = (
        exepack.HEADER_SIZE + len(image) + table_bias)
    fields["primary_payload_offset"] = exepack.HEADER_SIZE
    cursor = (exepack.HEADER_SIZE + len(image) +
              len(segments) * exepack.SEGMENT_ENTRY_SIZE)
    rows = []
    for row, payload in zip(segments, payloads):
        row = dict(row)
        cursor = exepack.align(cursor, exepack.PAYLOAD_ALIGN)
        row["payload_offset"] = cursor
        cursor += len(payload)
        rows.append((row, payload))
    body = bytearray(exepack.build_header(fields))
    body += image
    for row, _payload in rows:
        body += exepack.build_segment_row(row)
    for row, payload in rows:
        while len(body) < row["payload_offset"]:
            body += b"\x00"
        body += payload
    fields["file_size"] = len(body)
    header = exepack.build_header(fields)
    return bytes(header + body[len(header):])


def _fields(**overrides):
    fields = exepack.base_fields()
    fields["file_size"] = 0
    fields["total_segment_slots"] = None
    fields.update(overrides)
    return fields


def _row(**overrides):
    row = exepack.base_segment_row()
    row["flags"] = 0
    row["reserved1"] = 0
    row.update(overrides)
    return row


def _valid_segment(image_size=None, **overrides):
    if image_size is None:
        image_size = len(SEG_PAYLOAD)
    return _row(image_size=image_size, **overrides)


def build_fixtures():
    """Return [{'name','data','tool_code','kernel_code'}] (deterministic).

    tool_code is the exepack rejection reason (None for the well-formed
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
        ("exe-bad-magic.exe",
         _container(_fields(magic=b"EXEX"), _image(4)), "EXE_EXEC_ERR_MAGIC"),
        ("exe-bad-format.exe",
         _container(_fields(format_version=3), _image(4)),
         "EXE_EXEC_ERR_FORMAT"),
        ("exe-bad-abi.exe",
         _container(_fields(abi_version=2), _image(4)), "EXE_EXEC_ERR_ABI"),
        ("exe-bad-flags.exe",
         _container(_fields(flags=1), _image(4)), "EXE_EXEC_ERR_FLAGS"),
        ("exe-bad-reserved0.exe",
         _container(_fields(reserved0=1), _image(4)),
         "EXE_EXEC_ERR_RESERVED0"),
        ("exe-bad-header-size.exe",
         _container(_fields(header_size=34), _image(4)),
         "EXE_EXEC_ERR_HEADER"),
        ("exe-bad-reserved1.exe",
         _container(_fields(reserved1=1), _image(4)),
         "EXE_EXEC_ERR_RESERVED1"),
        ("exe-zero-image.exe",
         _container(_fields(image_size=0), b""), "EXE_EXEC_ERR_IMAGE"),
        ("exe-truncated.exe", truncated, "EXE_EXEC_ERR_FILESIZE"),
        ("exe-padded.exe", padded, "EXE_EXEC_ERR_FILESIZE"),
        ("exe-bad-entry.exe",
         _container(_fields(entry_offset=4), _image(4)),
         "EXE_EXEC_ERR_ENTRY"),
        ("exe-small-stack.exe",
         _container(_fields(stack_size=1), _image(4)),
         "EXE_EXEC_ERR_STACK"),
        ("exe-image-end.exe",
         _container(_fields(image_size=0xDF01), big_image + b"\x00"),
         "EXE_EXEC_ERR_IMAGE_END"),
        ("exe-bss-end.exe",
         _container(_fields(image_size=0xDF00, bss_size=2, stack_size=2),
                    big_image),
         "EXE_EXEC_ERR_BSS_END"),
        ("exe-stack-collision.exe",
         _container(_fields(image_size=16, bss_size=16, stack_size=0xDEF0),
                    _image(16)),
         "EXE_EXEC_ERR_COLLISION"),
        # Segment-table boundaries.  The base container is valid; each
        # variant breaks exactly one property.
        ("exe-seg-count.exe",
         _container(_fields(segment_count=9), _image(4),
                    [_valid_segment(id=i + 1) for i in range(9)],
                    [SEG_PAYLOAD] * 9),
         "EXE_EXEC_ERR_SEG_SLOTS"),
        ("exe-seg-slots.exe",
         _container(_fields(total_segment_slots=21), _image(4),
                    [_valid_segment()], [SEG_PAYLOAD]),
         "EXE_EXEC_ERR_SEG_SLOTS"),
        ("exe-seg-total.exe",
         _container(_fields(total_segment_slots=3), _image(4),
                    [_valid_segment()], [SEG_PAYLOAD]),
         "EXE_EXEC_ERR_SEG_SLOTS"),
        ("exe-seg-table.exe",
         _container(_fields(), _image(4), [_valid_segment()],
                    [SEG_PAYLOAD], table_bias=8),
         "EXE_EXEC_ERR_SEG_TABLE"),
        ("exe-seg-kind.exe",
         _container(_fields(), _image(4),
                    [_valid_segment(kind=3)],
                    [SEG_PAYLOAD]),
         "EXE_EXEC_ERR_SEG_KIND"),
        ("exe-far-oversize.exe",
         _container(_fields(), _image(4),
                    [_valid_segment(kind=exepack.KIND_FAR_CODE,
                                    image_size=exepack.SEG_MAX_FAR_SIZE +
                                    1)],
                    [SEG_PAYLOAD]),
         "EXE_EXEC_ERR_SEGMENT"),
        ("exe-seg-payload.exe",
         _container(_fields(), _image(4),
                    [_valid_segment(image_size=64)], [SEG_PAYLOAD]),
         "EXE_EXEC_ERR_SEGMENT"),
        ("exe-seg-id.exe",
         _container(_fields(), _image(4), [_valid_segment(id=2)],
                    [SEG_PAYLOAD]),
         "EXE_EXEC_ERR_SEGMENT"),
        ("exe-seg-flags.exe",
         _container(_fields(), _image(4),
                    [_valid_segment(flags=2)], [SEG_PAYLOAD]),
         "EXE_EXEC_ERR_SEGMENT"),
        ("exe-wrong-type.dat", _container(_fields(), _image(4)), None),
        ("exe-return.exe", _container(_fields(), _image(4)), None),
    ]
    fixtures = []
    for name, data, code in entries:
        kernel_code = code
        if name == "exe-wrong-type.dat":
            kernel_code = "EXE_EXEC_ERR_TYPE"
        elif name == "exe-return.exe":
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
