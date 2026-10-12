#!/usr/bin/env python3
"""Validate the KOS_BOOT_PROGRAM build setting (package 28b).

The kernel launcher (kernel/main.c) starts `KOS_BOOT_PROGRAM` through its
`/system` VFS path.  Because that path is compiled into the kernel as a
constant string and must work without SD, the build rejects a setting that
could not boot:

  - not an absolute path under `/system`;
  - longer than `KOS_PATH_MAX - 1` bytes;
  - naming a file that is not a EXE in the selected recovery manifest.

`check PROGRAM MANIFEST` exits 0 when the setting is valid and 2 with a
one-line diagnostic otherwise.  The rule set is mirrored by host tests;
each rejection has a negative test.
"""

import argparse
import json
import sys

# abi/kos/fs.h: KOS_PATH_MAX counts the terminating NUL.
KOS_PATH_MAX = 128
BOOT_PATH_MAX = KOS_PATH_MAX - 1  # 127
SYSTEM_PREFIX = "/system/"
PRINTABLE_MIN = 0x21  # '!' - no spaces, controls, DEL or non-ASCII
PRINTABLE_MAX = 0x7E  # '~'


class BootProgramError(Exception):
    """A rejected KOS_BOOT_PROGRAM setting."""


def _basename(program):
    name = program[len(SYSTEM_PREFIX):]
    if name == "" or "/" in name:
        raise BootProgramError(
            "boot program must name one file directly under /system: %r"
            % program)
    return name


def validate(program, manifest):
    """Return the validated program string, or raise BootProgramError.

    `manifest` is the parsed recovery manifest (romfs-v1).
    """
    if not program:
        raise BootProgramError("boot program is empty")
    try:
        raw = program.encode("ascii")
    except UnicodeEncodeError:
        raise BootProgramError(
            "boot program must be ASCII: %r" % program)
    if len(raw) > BOOT_PATH_MAX:
        raise BootProgramError(
            "boot program is %d bytes, over the %d-byte limit"
            % (len(raw), BOOT_PATH_MAX))
    for byte in raw:
        if not PRINTABLE_MIN <= byte <= PRINTABLE_MAX:
            raise BootProgramError(
                "boot program must be printable ASCII without spaces: %r"
                % program)
    if not program.startswith(SYSTEM_PREFIX):
        raise BootProgramError(
            "boot program must be an absolute path under /system: %r"
            % program)
    name = _basename(program)

    files = manifest.get("files") if isinstance(manifest, dict) else None
    if not isinstance(files, list):
        raise BootProgramError("recovery manifest has no file list")
    for entry in files:
        if not isinstance(entry, dict) or entry.get("name") != name:
            continue
        if entry.get("type") != "EXE":
            raise BootProgramError(
                "boot program %r is not a EXE in the manifest" % name)
        return program
    raise BootProgramError(
        "boot program %r is not in the selected recovery manifest" % name)


def load_manifest(path):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return json.load(handle)
    except OSError as error:
        raise BootProgramError("cannot read manifest %s: %s" % (path, error))
    except json.JSONDecodeError as error:
        raise BootProgramError("manifest %s is not JSON: %s" % (path, error))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    check = sub.add_parser("check", help="validate a boot-program setting")
    check.add_argument("program", help="the KOS_BOOT_PROGRAM value")
    check.add_argument("manifest", help="the selected recovery manifest")
    args = parser.parse_args(argv)

    try:
        validate(args.program, load_manifest(args.manifest))
    except BootProgramError as error:
        print("boot program: %s" % error, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
