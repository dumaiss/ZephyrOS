#!/usr/bin/env python3
"""Run a packed program in the Zephyr-80 emulator (`make run`).

Stages the program and its assets into a card folder under valid 8.3
names, starts MAME's zephyr80 machine on the chosen KonaOS image with that
folder as the card and the `konaos` plugin loaded, and hands the plugin a
session: wait for the shell prompt on the console, type
`/fs/<NAME>.EXE [args]`, and report the screen lines the program printed
once the prompt returns.  Breakpoints by symbol (`--break`) need the
debugger (`--mame-args -debug`) and are set in the symbol's own physical
bank.

The emulator is a separate tool: this helper looks for it only when it
runs, and names what is missing (the binary, the image, the plugin).  The
plugin ships with the emulator, in the `plugins/` directory beside the
MAME binary; `--plugins` names another directory holding `konaos/`.
`--mame` may name the emulator bundle's directory instead of a binary: its
`mamepbitz`, `plugins/` and `konaos.bin` are then the defaults.

Usage:
    exerun.py --exe build/user/NAME.exe --card DIR --rundir DIR
              --mame MAME --flash IMAGE [--osmap MAP] [--map MAP]
              [--plugins DIR] [--asset SRC[=NAME]]... [--args TEXT]
              [--break SYMBOL]... [--mame-args TEXT] [--exit]
              [--timeout SECONDS] [--prompt TEXT]

Exit status: 0 when MAME ended normally (with --exit: when the program
returned to the prompt), 2 for a missing or invalid input, otherwise 1.
"""

import argparse
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

# The 8.3 characters a card name may hold (FAT short names, upper case).
NAME_CHARS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'()-@^_`{}~")

PLUGIN = "konaos"


class RunError(Exception):
    pass


def short_name(name):
    """A file name as the valid 8.3 card name it is staged under: upper
    case, invalid characters dropped, the base cut to 8 characters and the
    extension to 3.  RunError when nothing of the base survives."""
    base, dot, ext = name.upper().rpartition(".")
    if not dot:
        base, ext = ext, ""
    base = "".join(c for c in base if c in NAME_CHARS)[:8]
    ext = "".join(c for c in ext if c in NAME_CHARS)[:3]
    if not base:
        raise RunError("%r has no characters a card name can use" % name)
    return base + ("." + ext if ext else "")


def staging(program, assets):
    """(source, card name) for the program and each asset.

    `program` is the packed .exe, staged as <NAME>.EXE; an asset is
    `SRC` or `SRC=NAME`.  Two files staged under one name are refused."""
    plan = [(Path(program), short_name(Path(program).stem + ".exe"))]
    for asset in assets:
        source, sep, name = asset.partition("=")
        source = Path(source)
        name = name if sep else source.name
        card = short_name(name)
        if sep and card != name.upper():
            raise RunError("asset name %r is not a valid 8.3 name (it would be %s)"
                           % (name, card))
        plan.append((source, card))
    seen = {}
    for source, card in plan:
        if not source.is_file():
            raise RunError("%s is missing" % source)
        if card in seen:
            raise RunError("%s and %s would both be %s on the card"
                           % (seen[card], source, card))
        seen[card] = source
    return plan


BUNDLE_MAME = "mamepbitz"
BUNDLE_FLASH = "konaos.bin"


def find_mame(mame):
    """The emulator binary: a path, a name looked up on PATH, or an
    emulator bundle's directory (its `mamepbitz`)."""
    if os.path.isdir(mame):
        mame = os.path.join(mame, BUNDLE_MAME)
    if os.sep in mame or (os.altsep and os.altsep in mame):
        path = Path(mame)
        if path.is_file() and os.access(path, os.X_OK):
            return path
    else:
        found = shutil.which(mame)
        if found:
            return Path(found)
    raise RunError(
        "the emulator %r is not found: set KOS_MAME to the zephyr80 MAME "
        "binary (built in the Zephyr-80 checkout's Code/MODERN/Emulator/mame "
        "with `make SUBTARGET=pbitz`), or to the emulator bundle's directory" % mame)


def find_plugins(mame, plugins):
    """The plugins directory holding konaos/: given, or beside the binary."""
    directory = Path(plugins) if plugins else Path(os.path.realpath(mame)).parent / "plugins"
    if not (directory / PLUGIN / "init.lua").is_file():
        raise RunError("the %s MAME plugin is not in %s: set KOS_MAME_PLUGINS to "
                       "the directory holding %s/" % (PLUGIN, directory, PLUGIN))
    return directory


def write_session(path, conf):
    lines = []
    for key, value in conf:
        if "\n" in value or "\t" in value:
            raise RunError("%s %r holds a tab or a line break" % (key, value))
        lines.append("%s\t%s\n" % (key, value))
    Path(path).write_text("".join(lines))


def main(argv):
    parser = argparse.ArgumentParser(prog="exerun.py", description=__doc__.splitlines()[0])
    parser.add_argument("--exe", required=True, help="the packed program")
    parser.add_argument("--card", required=True, help="the card folder the program is staged into")
    parser.add_argument("--rundir", required=True, help="the session, capture and MAME directories")
    parser.add_argument("--mame", default="", help="the MAME binary (path or name on PATH)")
    parser.add_argument("--flash", default="", help="the KonaOS flash image")
    parser.add_argument("--osmap", default="", help="the image's konaos.map (default: beside the image)")
    parser.add_argument("--map", default="", help="the program's link map (symbols)")
    parser.add_argument("--plugins", default="", help="the directory holding the konaos plugin")
    parser.add_argument("--asset", action="append", default=[], help="SRC or SRC=NAME to stage beside the program")
    parser.add_argument("--args", default="", help="the program's command-line arguments")
    parser.add_argument("--break", dest="breaks", action="append", default=[], help="a symbol to break at")
    parser.add_argument("--mame-args", default="", help="extra MAME options")
    parser.add_argument("--exit", action="store_true", help="end MAME once the program has returned")
    parser.add_argument("--timeout", default="60", help="emulated seconds for the prompt and the program")
    parser.add_argument("--prompt", default="/> ", help="the shell prompt at the card root")
    args = parser.parse_args(argv)

    try:
        plan = staging(args.exe, [a for a in args.asset if a])
        bundle = Path(args.mame) if args.mame and os.path.isdir(args.mame) else None
        flash_name = args.flash or (str(bundle / BUNDLE_FLASH) if bundle else "")
        if not flash_name:
            raise RunError("set KOS_FLASH to the KonaOS flash image to run (konaos.bin)")
        flash = Path(flash_name)
        if not flash.is_file():
            raise RunError("the flash image %s is missing" % flash)
        mame = find_mame(args.mame or "mamepbitz")
        plugins = find_plugins(mame, args.plugins)
        osmap = Path(args.osmap) if args.osmap else flash.parent / "konaos.map"
        if args.osmap and not osmap.is_file():
            raise RunError("the OS map %s is missing" % osmap)
        # A bare symbol is the program's own; image:name names another
        # image's (konaos:<symbol> for the OS).
        program_name = Path(args.exe).stem
        breaks = [b if ":" in b else "%s:%s" % (program_name, b) for b in args.breaks if b]
        if breaks and not osmap.is_file():
            raise RunError("breakpoints need the OS map: set KOS_OSMAP (konaos.map of %s)" % flash)
        if breaks and not args.map:
            raise RunError("breakpoints need the program's link map")
        card = Path(args.card)
        card.mkdir(parents=True, exist_ok=True)
        for source, name in plan:
            shutil.copyfile(source, card / name)
        rundir = Path(args.rundir)
        rundir.mkdir(parents=True, exist_ok=True)
        command = "/fs/" + plan[0][1] + (" " + args.args if args.args else "")
        session = [("command", command), ("prompt", args.prompt), ("timeout", args.timeout),
                   ("capture", str((rundir / "console.txt").resolve())),
                   ("result", str((rundir / "result.txt").resolve())), ("exit", "1" if args.exit else "0")]
        if osmap.is_file():
            session.append(("osmap", str(osmap.resolve())))
        if args.map:
            session.append(("program", str(Path(args.map).resolve())))
        session += [("break", b) for b in breaks]
        write_session(rundir / "session", session)
    except RunError as exc:
        print("exerun: %s" % exc, file=sys.stderr)
        return 2

    result = rundir / "result.txt"
    if result.exists():
        result.unlink()
    cmd = [str(mame), "zephyr80", "-flash", str(flash.resolve()), "-carddir", str(card.resolve()),
           "-pluginspath", str(plugins.resolve()), "-plugin", PLUGIN, "-skip_gameinfo",
           "-cfg_directory", str(rundir / "cfg"), "-nvram_directory", str(rundir / "nvram"),
           "-state_directory", str(rundir / "sta"), "-snapshot_directory", str(rundir / "snap")]
    cmd += shlex.split(args.mame_args)
    print("exerun: staged %s in %s" % (", ".join(name for _, name in plan), card))
    print("exerun: %s" % " ".join(shlex.quote(c) for c in cmd))
    sys.stdout.flush()
    env = dict(os.environ, KONAOS_SESSION=str((rundir / "session").resolve()))
    code = subprocess.run(cmd, env=env).returncode
    status = result.read_text().splitlines() if result.exists() else []
    if status:
        print("exerun: %s: %s" % (status[0], status[1] if len(status) > 1 else ""))
    if code != 0:
        print("exerun: MAME exited with status %d" % code, file=sys.stderr)
        return 1
    if args.exit and (not status or status[0] != "ran"):
        print("exerun: the program did not return to the prompt (%s)"
              % (status[0] if status else "no result"), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
