#!/usr/bin/env python3
"""Assemble the standalone KonaOS SDK tree.

`make sdk` runs this tool; it lays out

    build/kos-sdk-<version>/
      VERSION          "<version> abi <n>", n = EXE_ABI_VERSION
      include/         kos.h, tui.h, kos/*.h, klibc/*.h, and every abi/kos/
                       header they include (copied with their include
                       paths: "kos/…" and "klibc/…" resolve inside the
                       tree, the ABI copies under include/abi/kos/*.h)
      lib/             libkos.lib (the runtime object folded in), libtui.lib,
                       libtui_128x35.lib, libtui_widgets.lib, crt0.rel
      tools/           exepack.py, exemanifest.py, ihx.py
      mk/kos.mk        the standalone build rules
      template/        Makefile + hello.c
      examples/        hello, fardemo (__banked), files (open/read/list),
                       fardata (DATA1= + FAR1=, far data and a banked call),
                       tui_demo, tui_keys, tui_dialog
      docs/            sdk.md, tui.md, exe-format.md

Nothing in the tree names a path outside it: every copied file is checked
(the docs are the tree's documentation and describe the OS tree too, so
they are exempt), and the header closure is proved complete by resolving
every quoted include the way SDCC will.

Usage:
    sdkpack.py --version V --libkos FILE --libtui FILE --libtui-wide FILE
               --crt0 FILE
               [--root DIR] [--out DIR] [--tar FILE]

With --tar the tree is also written as a .tar.gz with pinned metadata
(member order, mtimes, ownership), so the same tree always packs to the
same bytes.
"""

import argparse
import gzip
import posixpath
import re
import shutil
import sys
import tarfile
from pathlib import Path

TOOL_FILES = ("exepack.py", "exemanifest.py", "ihx.py")
DOC_FILES = ("sdk.md", "tui.md", "exe-format.md")
SDK_INCLUDE = "user/sdk/include"
SDK_TEMPLATE = "user/sdk/template"
SDK_EXAMPLES = "user/sdk/examples"
ABI_ROOT = "abi"

# Tokens a copied file may not name: the SDK must stand alone.
FORBIDDEN = ("abi/", "user/sdk")

INCLUDE_RE = re.compile(r'#\s*include\s+"([^"]+)"')
ABI_RE = re.compile(r"^\s*#define\s+EXE_ABI_VERSION\s+(\d+)\s*u?\s*$", re.M)


class PackError(Exception):
    pass


def abi_version(header_text):
    """EXE_ABI_VERSION from formats/include/exe.h, never copied by hand."""
    match = ABI_RE.search(header_text)
    if match is None:
        raise PackError("formats/include/exe.h defines no EXE_ABI_VERSION")
    return int(match.group(1))


def sdk_headers(root):
    """(dest, source) for the SDK's own headers, copied flat under include/.

    The destination is the include path the header is reached by, so a
    `"kos/…"` or `"klibc/…"` include inside the tree resolves to it.
    """
    base = root / SDK_INCLUDE
    copies = [("include/kos.h", base / "kos.h"),
              ("include/tui.h", base / "tui.h"),
              ("include/tui_widgets.h", base / "tui_widgets.h")]
    for path in sorted((base / "kos").glob("*.h")):
        copies.append(("include/kos/%s" % path.name, path))
    for path in sorted((base / "klibc").glob("*.h")):
        copies.append(("include/klibc/%s" % path.name, path))
    return copies


def abi_headers(root, headers):
    """The abi/kos headers `headers` include, transitively.

    Quoted includes resolve the way SDCC resolves them for an SDK build:
    the including file's own directory first, then the ABI root, then the
    SDK include root (mk/kos.mk passes -I<abi> -I<include>, the same
    order the OS tree uses).  Every include must resolve, or the header
    closure the SDK ships would be incomplete.
    """
    sources = dict(headers)
    for path in sorted((root / ABI_ROOT / "kos").glob("*.h")):
        sources["include/%s/kos/%s" % (ABI_ROOT, path.name)] = path

    def resolve(dest, include):
        candidates = (
            posixpath.normpath(posixpath.join(posixpath.dirname(dest),
                                              include)),
            posixpath.join("include", ABI_ROOT, include),
            posixpath.join("include", include),
        )
        for candidate in candidates:
            if candidate in sources:
                return candidate
        raise PackError('%s: #include "%s" resolves nowhere inside the SDK'
                        % (dest, include))

    abi = {}
    pending = list(headers)
    seen = set()
    while pending:
        dest, source = pending.pop()
        if dest in seen:
            continue
        seen.add(dest)
        for include in INCLUDE_RE.findall(source.read_text()):
            target = resolve(dest, include)
            if target.startswith("include/%s/" % ABI_ROOT):
                abi[target] = sources[target]
            if target not in seen:
                pending.append((target, sources[target]))
    return abi


def tree_files(root, headers, abi, libkos, libtui, libtui_wide,
               libtui_widgets, crt0):
    """The complete (relative destination, source) list of the SDK tree."""
    files = []
    files += headers
    files += sorted(abi.items())
    files += [
        ("lib/libkos.lib", Path(libkos)),
        ("lib/libtui.lib", Path(libtui)),
        ("lib/libtui_128x35.lib", Path(libtui_wide)),
        ("lib/libtui_widgets.lib", Path(libtui_widgets)),
        ("lib/crt0.rel", Path(crt0)),
        ("mk/kos.mk", root / "mk" / "kos.mk"),
    ]
    files += [("tools/%s" % name, root / "tools" / name)
              for name in TOOL_FILES]
    files += [("template/%s" % name, root / SDK_TEMPLATE / name)
              for name in ("Makefile", "hello.c")]
    files += [("examples/%s" % name, root / SDK_EXAMPLES / name)
              for name in ("Makefile", "hello.c", "fardemo.c",
                           "fardemo_far.c", "files.c", "fardata.c",
                           "fardata_far.c", "levels.c", "tui_demo.c",
                           "tui_keys.c", "tui_dialog.c")]
    files += [("docs/%s" % name, root / "docs" / name)
              for name in DOC_FILES]
    return files


def build_tree(root, out, version, libkos, libtui, libtui_wide,
               libtui_widgets, crt0):
    """Lay out the SDK tree under `out`; returns the destination list."""
    root = Path(root)
    out = Path(out)
    for path in (root / "formats/include/exe.h", Path(libkos), Path(libtui),
                 Path(libtui_wide), Path(libtui_widgets), Path(crt0)):
        if not path.is_file():
            raise PackError("%s is missing" % path)
    abi = abi_version((root / "formats/include/exe.h").read_text())
    headers = sdk_headers(root)
    files = tree_files(root, headers, abi_headers(root, headers), libkos,
                       libtui, libtui_wide, libtui_widgets, crt0)
    for _dest, source in files:
        if not Path(source).is_file():
            raise PackError("%s is missing" % source)

    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    (out / "VERSION").write_text("%s abi %d\n" % (version, abi))
    for dest, source in files:
        target = out / dest
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)

    for dest, _source in files:
        if dest.startswith("docs/"):
            continue
        data = (out / dest).read_bytes()
        for token in FORBIDDEN + (str(root),):
            if token.encode("ascii") in data:
                raise PackError("%s names %r; the SDK must stand alone"
                                % (dest, token))
    return ["VERSION"] + [dest for dest, _source in files]


def walk_tree(tree, prefix=""):
    """The archive order: each directory's entries by name, a directory
    before what it holds."""
    for path in sorted(Path(tree).iterdir(), key=lambda entry: entry.name):
        name = posixpath.join(prefix, path.name) if prefix else path.name
        yield path, name
        if path.is_dir():
            yield from walk_tree(path, name)


def pack_tar(tree, tarball):
    """Write `tree` to `tarball` with pinned member order, mtimes and
    ownership, so the same tree packs to the same bytes."""
    with open(tarball, "wb") as raw:
        gz = gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0)
        with gz, tarfile.open(fileobj=gz, mode="w") as tar:
            for path, name in walk_tree(tree, Path(tree).name):
                info = tar.gettarinfo(str(path), arcname=name)
                info.mtime = 0
                info.uid = info.gid = 0
                info.uname = info.gname = ""
                info.mode = 0o755 if info.isdir() else 0o644
                if info.isdir():
                    tar.addfile(info)
                else:
                    with open(path, "rb") as source:
                        tar.addfile(info, source)


def main(argv):
    parser = argparse.ArgumentParser(prog="sdkpack.py")
    parser.add_argument("--version", required=True)
    parser.add_argument("--libkos", required=True)
    parser.add_argument("--libtui", required=True)
    parser.add_argument("--libtui-wide", required=True,
                        help="libtui built for 128 x 35 (TUI_GRID=128x35)")
    parser.add_argument("--libtui-widgets", required=True)
    parser.add_argument("--crt0", required=True)
    parser.add_argument("--root", default=None,
                        help="the repository root (default: this tool's)")
    parser.add_argument("--out", default=None,
                        help="the tree to write (default: "
                             "<root>/build/kos-sdk-<version>)")
    parser.add_argument("--tar", default=None,
                        help="also write the tree to this .tar.gz")
    args = parser.parse_args(argv)
    root = Path(args.root).resolve() if args.root else \
        Path(__file__).resolve().parent.parent
    out = Path(args.out) if args.out else \
        root / "build" / ("kos-sdk-%s" % args.version)
    try:
        dests = build_tree(root, out, args.version, args.libkos,
                           args.libtui, args.libtui_wide,
                           args.libtui_widgets, args.crt0)
        if args.tar:
            pack_tar(out, args.tar)
    except (PackError, OSError) as exc:
        print("sdkpack: %s" % exc, file=sys.stderr)
        return 1
    print("sdkpack: %s (%d files)"
          % (out, len(dests)))
    if args.tar:
        print("sdkpack: %s" % args.tar)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
