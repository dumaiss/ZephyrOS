#!/usr/bin/env python3
"""ZephyrOS M9 static mount-table generator.

Translates the selected VFS mount manifest into the immutable kernel table
consumed by kernel/vfs at boot (architecture sections 32.5.1, 32.33 and
work/M9-REFERENCE.md "Static mount table").

    mountgen.py <manifest.json> --builtin <names> \
        --out build/generated/vfs_mounts.c \
        --report build/generated/vfs_mounts.json

The generator never rewrites policy: a manifest is either emitted verbatim
(canonicalized into C) or rejected.  Output is deterministic -- no
timestamps, no absolute host paths -- so `make check` reproducibility holds.
"""

import argparse
import json
import re
import sys
from pathlib import Path

FORMAT = "vfs-mounts-v1"
MAX_MOUNTS = 4
PATH_MAX = 128  # includes the terminating NUL

FLAG_SYSTEM = "VFS_MOUNT_SYSTEM"
FLAG_REQUIRED = "VFS_MOUNT_REQUIRED"
FLAG_OPTIONAL = "VFS_MOUNT_OPTIONAL"

_ENTRY_KEYS = {"path", "filesystem", "source", "system", "required"}
_TOP_KEYS = {"format", "mounts"}


class MountError(Exception):
    """The manifest violates the mount-configuration contract."""


def _is_upper_ascii_path(path):
    """Canonical policy path: absolute, upper-case ASCII, no trailing or
    repeated slash (only "/" itself may end in a slash)."""
    if not path.startswith("/"):
        return False
    if len(path) > 1 and path.endswith("/"):
        return False
    if "//" in path:
        return False
    if any(ch.islower() for ch in path):
        return False
    return True


def _components(path):
    return [c for c in path.split("/") if c]


def _nested(a, b):
    """True when mountpoint `a` is a proper component-prefix of `b`."""
    ca, cb = _components(a), _components(b)
    return len(ca) < len(cb) and cb[:len(ca)] == ca


def validate(manifest_path, builtin, max_mounts=MAX_MOUNTS):
    """Validate and canonicalize a manifest; returns the entry list."""
    path = Path(manifest_path)
    try:
        raw = json.loads(path.read_text())
    except OSError as exc:
        raise MountError("cannot read manifest: %s" % exc)
    except ValueError as exc:
        raise MountError("manifest is not valid JSON: %s" % exc)

    where = str(path)
    if not isinstance(raw, dict):
        raise MountError("%s: manifest must be an object" % where)
    if raw.get("format") != FORMAT:
        raise MountError("%s: unsupported format %r (expected %r)"
                         % (where, raw.get("format"), FORMAT))
    unknown = set(raw) - _TOP_KEYS
    if unknown:
        raise MountError("%s: unknown manifest field(s): %s"
                         % (where, ", ".join(sorted(unknown))))
    mounts = raw.get("mounts")
    if not isinstance(mounts, list) or not mounts:
        raise MountError("%s: 'mounts' must be a non-empty list" % where)
    if len(mounts) > max_mounts:
        raise MountError("%s: %d mounts exceed the kernel capacity %d"
                         % (where, len(mounts), max_mounts))

    builtin_lower = {name.lower() for name in builtin}
    entries = []
    seen = set()
    system_count = 0

    for index, entry in enumerate(mounts):
        at = "%s: mounts[%d]" % (where, index)
        if not isinstance(entry, dict):
            raise MountError("%s: entry must be an object" % at)
        unknown = set(entry) - _ENTRY_KEYS
        if unknown:
            raise MountError("%s: unknown field(s): %s"
                             % (at, ", ".join(sorted(unknown))))

        path_value = entry.get("path")
        if not isinstance(path_value, str) or not path_value:
            raise MountError("%s: missing path" % at)
        if len(path_value) >= PATH_MAX:
            raise MountError("%s: path %r exceeds ZOS_PATH_MAX (%d)"
                             % (at, path_value, PATH_MAX))
        if not path_value.startswith("/"):
            raise MountError("%s: mount path %r is relative"
                             % (at, path_value))
        if not _is_upper_ascii_path(path_value):
            raise MountError("%s: mount path %r is not canonical "
                             "(absolute upper-case ASCII, no trailing or "
                             "repeated '/')" % (at, path_value))
        comps = _components(path_value)
        if any(c in (".", "..") for c in comps):
            raise MountError("%s: mount path %r contains '.' or '..'"
                             % (at, path_value))
        if len(comps) > 1:
            raise MountError("%s: nested mountpoint %r is not supported in M9"
                             % (at, path_value))

        filesystem = entry.get("filesystem")
        if not isinstance(filesystem, str) or not filesystem:
            raise MountError("%s: missing/empty filesystem name" % at)
        source = entry.get("source")
        if not isinstance(source, str) or not source:
            raise MountError("%s: missing/empty source name" % at)

        system = entry.get("system", False)
        if not isinstance(system, bool):
            raise MountError("%s: 'system' must be a boolean" % at)
        if "required" in entry:
            required = entry["required"]
            if not isinstance(required, bool):
                raise MountError("%s: 'required' must be a boolean" % at)
            if system:
                raise MountError("%s: the system entry must not carry "
                                 "'required'" % at)
        else:
            required = False

        if system:
            system_count += 1
            if filesystem.lower() not in builtin_lower:
                raise MountError("%s: system filesystem %r is not built into "
                                 "the kernel (%s)"
                                 % (at, filesystem,
                                    ", ".join(sorted(builtin))))
            flags = FLAG_SYSTEM
        elif required:
            flags = FLAG_REQUIRED
        else:
            flags = FLAG_OPTIONAL

        key = path_value.upper()
        if key in seen:
            raise MountError("%s: duplicate mountpoint %r" % (at, path_value))
        seen.add(key)

        entries.append({
            "path": path_value,
            "path_len": len(path_value),
            "filesystem": filesystem,
            "source": source,
            "flags": flags,
            "system": system,
            "required": required,
        })

    if system_count == 0:
        raise MountError("%s: no system=true mount" % where)
    if system_count > 1:
        raise MountError("%s: %d system=true mounts (exactly one required)"
                         % (where, system_count))

    for i, a in enumerate(entries):
        for j, b in enumerate(entries):
            if i != j and _nested(a["path"], b["path"]):
                raise MountError("%s: nested mountpoints %r inside %r are not "
                                 "supported in M9"
                                 % (where, b["path"], a["path"]))
    return entries


def render_c(entries):
    lines = [
        "/* generated by tools/mountgen.py; do not edit */",
        '#include "kernel/vfs/mount_config.h"',
        "",
        "const struct vfs_mount_config vfs_mount_table[] = {",
    ]
    if entries:
        width = max(len(e["path"]) for e in entries)
        for entry in entries:
            path_literal = "\"%s\"," % entry["path"]
            lines.append(
                '\t{ %-*s %du, %-9s %-10s %s },'
                % (width + 3, path_literal, entry["path_len"],
                   '"%s",' % entry["filesystem"],
                   '"%s",' % entry["source"], entry["flags"]))
    else:
        lines.append("\t{ /* empty */ }")
    lines.append("};")
    lines.append("const uint8_t vfs_mount_count = %du;" % len(entries))
    lines.append("")
    return "\n".join(lines)


def build_report(manifest_path, builtin, entries):
    return {
        "format": FORMAT,
        "manifest": Path(manifest_path).name,
        "builtin": sorted(builtin),
        "count": len(entries),
        "mounts": [
            {
                "path": e["path"],
                "path_len": e["path_len"],
                "filesystem": e["filesystem"],
                "source": e["source"],
                "flags": e["flags"],
                "system": e["system"],
                "required": e["required"],
            }
            for e in entries
        ],
    }


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("manifest")
    parser.add_argument("--builtin", default="",
                        help="comma/space separated provider names")
    parser.add_argument("--out", required=True)
    parser.add_argument("--report")
    parser.add_argument("--max-mounts", type=int, default=MAX_MOUNTS)
    args = parser.parse_args(argv)

    builtin = [name for name in re.split(r"[,\s]+", args.builtin) if name]
    try:
        entries = validate(args.manifest, builtin, args.max_mounts)
    except MountError as exc:
        print("mountgen: %s" % exc, file=sys.stderr)
        return 1

    source = render_c(entries)
    out = Path(args.out)
    if out.parent and not out.parent.exists():
        out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(source)

    report = build_report(args.manifest, builtin, entries)
    if args.report:
        report_path = Path(args.report)
        if report_path.parent and not report_path.parent.exists():
            report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n")
    print("mountgen: %s: %d mount(s) -> %s"
          % (args.manifest, len(entries), out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
