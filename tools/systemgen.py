#!/usr/bin/env python3
"""KonaOS image generator.

One `systems/<name>.json` describes one image: the driver set in load
order, the program the launcher starts, the store's non-driver files and
the image's extra C flags.  This tool turns that description into every
build input derived from it, under the build directory:

    system.mk              KOS_DRIVERS, KOS_BOOT_PROGRAM, the flags and
                           the object/file lists the rules need
    store.json             the ROMFS manifest tools/romfspack.py packs:
                           the driver images in load order, `drivers.man`,
                           then the store's files
    modules/drivers.man    the boot manifest the nucleus loader reads:
                           one 24-byte name record per driver image
    drivers/<id>.json      one resolved copy of each selected driver
                           manifest: the source manifest with `depends`
                           turned back into the `{class, instance}` rows
                           the packer consumes (`optional` where marked)

The resolved copies are what `tools/modpack.py` packs.  It keeps every
host-side decision it made before: the endpoint numbers, the tier plan,
the in-container tables and the plan report.

A driver entry in `drivers` is either a bare id - `drivers/<id>/<id>.json`
or the single `drivers/<dir>/<id>.json` - or a repo-relative path without
the `.json` suffix (the test-only drivers under `tests/`).  The id is the
basename; ids must be unique in a system and the manifest's `name` must
equal its id.

Driver manifests carry `"schema": "kos-driver-1"` and declare their
dependencies symbolically:

    "depends": ["INPUT", "VIDEO:1", "#16", "INPUT?"]

  * `INPUT`      a protocol type name, instance 0;
  * `VIDEO:1`    the same with the endpoint instance;
  * `#16`        a raw endpoint class, for the acceptance drivers whose
                 classes carry no protocol name (decimal);
  * trailing `?` the dependency is optional: it resolves to 0 when no
                 selected endpoint provides it.

A name resolves against the selected driver set, by endpoint class
(`INPUT` is class 4, `VIDEO` class 2, ...; the names and their numbers are
the protocol types of abi/kos/protocol.h, shared with the packer).  No
match, or more than one, is a build error naming the system, the driver
and the entry.

The tool also validates the boot setting: an absolute path to one file
directly under `/system`, at most KOS_PATH_MAX - 1 bytes of printable
ASCII without spaces, naming an EXE the generated store carries.  SDCC
aborts on any argument that contains ".exe", so the setting reaches the
kernel through the generated `boot_program.h` and never as a -D flag; a
`cflags` entry that names a `.exe` path is refused for the same reason.

Usage:
    systemgen.py <system.json> <build-dir> [--omit-image DRIVER ...]

`--omit-image` leaves one selected driver's image out of the store while
its name stays in the boot manifest: the required-driver failure store
.  Exit status: 0 success, 1 rejected input, 2 usage.
"""

import argparse
import json
import os
import re
import sys

if os.path.dirname(os.path.abspath(__file__)) not in sys.path:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import modpack  # the protocol type names and the manifest geometry

SYSTEM_SCHEMA = "kos-system-1"
DRIVER_SCHEMA = "kos-driver-1"

SYSTEM_FIELDS = ("schema", "name", "drivers", "boot", "after_exit",
                 "store", "cflags")

# What the launcher does after the boot program exits normally.  Every
# image states it: `monitor` ends an acceptance image at the monitor after
# one run, `relaunch` is the production contract.
AFTER_EXIT_CHOICES = ("relaunch", "monitor")
MONITOR_NAME = "monitor.exe"

MANIFEST_ENTRY_SIZE = modpack.MANIFEST_ENTRY_SIZE

# abi/kos/fs.h: KOS_PATH_MAX counts the terminating NUL.
KOS_PATH_MAX = 128
BOOT_PATH_MAX = KOS_PATH_MAX - 1  # 127
SYSTEM_ROOT = "/system/"
PRINTABLE_MIN = 0x21  # '!' - no spaces, controls, DEL or non-ASCII
PRINTABLE_MAX = 0x7E  # '~'

# A symbolic dependency: a protocol type name or `#class`, an optional
# instance, and a trailing `?` for an optional declaration.
DEPENDENCY = re.compile(
    r"^(?P<name>[A-Z][A-Z0-9_]*|#[0-9]+)(?::(?P<instance>[0-9]+))?"
    r"(?P<optional>\?)?$")


class SystemError(Exception):
    """A rejected system file, driver manifest or boot setting."""


# --------------------------------------------------------------------------
# input
# --------------------------------------------------------------------------


def read_json(path, what):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return json.load(handle)
    except OSError as error:
        raise SystemError("cannot read %s %s: %s" % (what, path, error))
    except json.JSONDecodeError as error:
        raise SystemError("%s %s is not JSON: %s" % (what, path, error))


def load_system(path):
    """Return the validated system document and its name."""
    document = read_json(path, "system file")
    if not isinstance(document, dict):
        raise SystemError("%s: a system file is a JSON object" % path)
    schema = document.get("schema")
    if schema != SYSTEM_SCHEMA:
        raise SystemError("%s: schema must be %r, not %r"
                          % (path, SYSTEM_SCHEMA, schema))
    extra = [field for field in document if field not in SYSTEM_FIELDS]
    if extra:
        raise SystemError("%s: unknown field(s): %s"
                          % (path, ", ".join(sorted(extra))))
    for field in ("name", "drivers", "boot", "after_exit", "store",
                  "cflags"):
        if field not in document:
            raise SystemError("%s: missing %r" % (path, field))
    name = document["name"]
    if not isinstance(name, str) or not name:
        raise SystemError("%s: name must be a non-empty string" % path)
    stem = os.path.splitext(os.path.basename(path))[0]
    if stem != name:
        raise SystemError("%s: the file name must be systems/<name>.json "
                          "(name is %r)" % (path, name))
    if not isinstance(document["drivers"], list) or not document["drivers"]:
        raise SystemError("%s: drivers must be a non-empty list of ids"
                          % path)
    for entry in document["drivers"]:
        if not isinstance(entry, str) or not entry:
            raise SystemError("%s: drivers entries are non-empty strings"
                              % path)
    if not isinstance(document["store"], list):
        raise SystemError("%s: store must be a list of file names" % path)
    for entry in document["store"]:
        if not isinstance(entry, str) or not entry:
            raise SystemError("%s: store entries are non-empty strings"
                              % path)
    if document["after_exit"] not in AFTER_EXIT_CHOICES:
        raise SystemError("%s: after_exit must be one of %s, not %r"
                          % (path, " or ".join(AFTER_EXIT_CHOICES),
                             document["after_exit"]))
    if not isinstance(document["cflags"], list):
        raise SystemError("%s: cflags must be a list of flags" % path)
    for flag in document["cflags"]:
        if not isinstance(flag, str):
            raise SystemError("%s: cflags entries are strings" % path)
        if ".exe" in flag:
            raise SystemError(
                "%s: cflags entry %r contains a .exe path; SDCC refuses any "
                "argument that does, and the boot setting travels through "
                "the generated header" % (path, flag))
    return document, name


def find_manifest(root, entry):
    """Resolve one `drivers` entry to the manifest file it names."""
    if "/" in entry:
        path = os.path.join(root, entry + ".json")
        if not os.path.isfile(path):
            raise SystemError("driver %r: no manifest at %s" % (entry, path))
        return path
    directory = os.path.join(root, "drivers")
    candidates = []
    for parent, _dirs, names in os.walk(directory):
        if entry + ".json" in names:
            candidates.append(os.path.join(parent, entry + ".json"))
    candidates.sort()
    if not candidates:
        raise SystemError("unknown driver %r: no drivers/*/%s.json"
                          % (entry, entry))
    if len(candidates) > 1:
        raise SystemError("driver %r is ambiguous: %s"
                          % (entry, ", ".join(candidates)))
    return candidates[0]


def load_driver(path, driver_id):
    """Return the validated source manifest of one selected driver."""
    manifest = read_json(path, "driver manifest")
    if not isinstance(manifest, dict):
        raise SystemError("%s: a driver manifest is a JSON object" % path)
    schema = manifest.get("schema")
    if schema != DRIVER_SCHEMA:
        raise SystemError("%s: schema must be %r, not %r"
                          % (path, DRIVER_SCHEMA, schema))
    name = manifest.get("name")
    if not isinstance(name, str) or not name:
        raise SystemError("%s: missing name" % path)
    if name != driver_id:
        raise SystemError("%s: name %r is not the driver id %r"
                          % (path, name, driver_id))
    objects = manifest.get("objects")
    if not isinstance(objects, list) or not objects or \
            not all(isinstance(item, str) and item for item in objects):
        raise SystemError("%s: objects must be a non-empty list of file "
                          "names" % path)
    segments = manifest.get("segments")
    if not isinstance(segments, list) or not segments:
        raise SystemError("%s: segments must be a non-empty list" % path)
    for field in ("endpoints", "publishes", "depends"):
        value = manifest.get(field, [])
        if not isinstance(value, list):
            raise SystemError("%s: %s must be a list" % (path, field))
    for index, entry in enumerate(manifest.get("depends", [])):
        if not isinstance(entry, str):
            raise SystemError("%s: depends[%d] must be a string like "
                              "\"INPUT\" or \"#16\"" % (path, index))
    manifest["depends"] = list(manifest.get("depends", []))
    return manifest


# --------------------------------------------------------------------------
# dependency resolution
# --------------------------------------------------------------------------


def dependency_key(entry, path, index):
    """Parse one symbolic dependency into (class, instance, optional)."""
    where = "%s: depends[%d] %r" % (path, index, entry)
    match = DEPENDENCY.match(entry)
    if match is None:
        raise SystemError("%s is not a protocol type name, a `#class` or "
                          "one of those with an instance and a trailing `?`"
                          % where)
    name = match.group("name")
    if name.startswith("#"):
        class_number = int(name[1:])
        if class_number < 1 or class_number > 0xFF:
            raise SystemError("%s: class out of range" % where)
    else:
        if name not in modpack.TYPES:
            raise SystemError("%s: unknown protocol type %r (one of %s)"
                              % (where, name,
                                 ", ".join(sorted(modpack.TYPES))))
        class_number = modpack.TYPES[name]
    instance = int(match.group("instance") or 0)
    if instance > 0xFF:
        raise SystemError("%s: instance out of range" % where)
    return class_number, instance, bool(match.group("optional"))


def resolve_set(system_name, drivers):
    """Resolve every selected driver's symbolic dependencies.

    `drivers` is the load-order list of (id, manifest) pairs; each
    manifest's `depends` is replaced by the `{class, instance}` rows the
    packer consumes.  A name that no selected endpoint provides, or that
    more than one provides, is an error naming the system, the driver and
    the entry."""
    exports = []
    for driver_id, manifest in drivers:
        for endpoint in manifest.get("endpoints", []):
            exports.append((int(endpoint["class"]),
                            int(endpoint["instance"]), driver_id))
    for driver_id, manifest in drivers:
        depends = []
        for index, entry in enumerate(manifest["depends"]):
            class_number, instance, optional = dependency_key(
                entry, manifest["path"], index)
            matches = [driver for (cls, inst, driver) in exports
                       if cls == class_number and inst == instance]
            if len(matches) > 1:
                raise SystemError(
                    "%s: %s: dependency %r matches more than one selected "
                    "endpoint: %s" % (system_name, driver_id, entry,
                                      ", ".join(sorted(matches))))
            # A marked-optional dependency that no selected endpoint
            # provides keeps its declared endpoint and its row: the packer
            # resolves it to number 0, and the driver's dependency array
            # keeps its positions.
            if not matches and not optional:
                raise SystemError(
                    "%s: %s: dependency %r matches no selected endpoint"
                    % (system_name, driver_id, entry))
            row = {"class": class_number, "instance": instance}
            if optional:
                row["optional"] = 1
            depends.append(row)
        manifest["resolved_depends"] = depends
    return drivers


# --------------------------------------------------------------------------
# boot setting
# --------------------------------------------------------------------------


def validate_boot(program, store_files, system_name, missing=()):
    """Return the validated boot program, or raise SystemError.

    `missing` names store entries the caller deliberately left out
    (`--omit-store`): a boot program naming one is accepted, because the
    image exists to exercise the launcher's failure path."""
    where = "%s: boot %r" % (system_name, program)
    if not program:
        raise SystemError("%s: the boot program is empty" % system_name)
    try:
        raw = program.encode("ascii")
    except UnicodeEncodeError:
        raise SystemError("%s: the boot program must be ASCII" % where)
    if len(raw) > BOOT_PATH_MAX:
        raise SystemError(
            "%s: %d bytes, over the %d-byte limit"
            % (where, len(raw), BOOT_PATH_MAX))
    for byte in raw:
        if not PRINTABLE_MIN <= byte <= PRINTABLE_MAX:
            raise SystemError(
                "%s: printable ASCII without spaces only" % where)
    if not program.startswith(SYSTEM_ROOT):
        raise SystemError("%s: not an absolute path under %s" % (where, SYSTEM_ROOT))
    name = program[len(SYSTEM_ROOT):]
    if name == "" or "/" in name:
        raise SystemError("%s: must name one file directly under %s"
                          % (where, SYSTEM_ROOT))
    if name in missing:
        return program
    for entry in store_files:
        if entry["name"] != name:
            continue
        if entry["type"] != "EXE":
            raise SystemError("%s: %s is a %s in the store, not an EXE"
                              % (where, name, entry["type"]))
        return program
    raise SystemError("%s: %s is not in the system's store"
                      % (where, name))


def validate_after_exit(system, store_files, system_name):
    """Check the after-exit policy against the store.

    `monitor` ends the run at /system/monitor.exe, so the image must carry
    it; `relaunch` needs nothing beyond the boot program itself."""
    if system["after_exit"] != "monitor":
        return
    for entry in store_files:
        if entry["name"] == MONITOR_NAME:
            if entry["type"] != "EXE":
                raise SystemError(
                    "%s: after_exit monitor, but %s is a %s in the store"
                    % (system_name, MONITOR_NAME, entry["type"]))
            return
    raise SystemError(
        "%s: after_exit monitor, but %s is not in the system's store"
        % (system_name, MONITOR_NAME))


# --------------------------------------------------------------------------
# outputs
# --------------------------------------------------------------------------


def store_type(name):
    """The ROMFS media type of one store file entry."""
    if name.endswith(".exe"):
        return "EXE"
    if name.endswith(".mod"):
        return "MOD"
    return "DATA"


def check_romfs_name(where, name):
    """The ROMFS name field holds up to 23 printable ASCII bytes."""
    encoded = name.encode("ascii", "replace")
    if len(encoded) > MANIFEST_ENTRY_SIZE - 1 or \
            not all(0x20 <= byte <= 0x7E for byte in encoded):
        raise SystemError(
            "%s: the ROMFS name %r is not a flat name of at most %d "
            "characters" % (where, name, MANIFEST_ENTRY_SIZE - 1))


def build_store(system_name, drivers, store, omit, omit_store=()):
    """The ROMFS manifest: the driver images, drivers.man, the store."""
    files = []
    seen = {}
    for driver_id, _manifest in drivers:
        if driver_id in omit:
            continue
        image = driver_id + ".mod"
        files.append({"name": image, "type": "MOD", "path": image})
    files.append({"name": "drivers.man", "type": "DATA",
                  "path": "drivers.man"})
    for name in store:
        if name in omit_store:
            continue
        files.append({"name": name, "type": store_type(name), "path": name})
    for entry in files:
        folded = entry["name"].upper()
        if folded in seen:
            raise SystemError(
                "%s: %s appears twice in the store (also as %s)"
                % (system_name, entry["name"], seen[folded]))
        seen[folded] = entry["name"]
        check_romfs_name(system_name, entry["name"])
    return {"format": "romfs-v1", "files": files}


def driver_manifest(build_dir, driver_id):
    return os.path.join(build_dir, "drivers", driver_id + ".json")


def write_resolved(build_dir, drivers):
    for driver_id, manifest in drivers:
        copy = {key: value for key, value in manifest.items()
                if key not in ("path", "resolved_depends")}
        copy["depends"] = manifest["resolved_depends"]
        path = driver_manifest(build_dir, driver_id)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(copy, handle, indent=2)
            handle.write("\n")


def write_system_mk(build_dir, system, drivers, omit_store=()):
    modules = os.path.join(build_dir, "modules")
    objects = []
    for _driver_id, manifest in drivers:
        for name in manifest["objects"]:
            path = os.path.join(modules, name)
            if path not in objects:
                objects.append(path)
    lines = [
        "# Generated by tools/systemgen.py from systems/%s.json."
        % system["name"],
        "# Do not edit: change the system file and rebuild.",
        "KOS_DRIVERS := %s" % " ".join(driver_id for driver_id, _manifest
                                       in drivers),
        "KOS_BOOT_PROGRAM := %s" % system["boot"],
        "KOS_BOOT_AFTER_EXIT := %s" % system["after_exit"],
        "KOS_SYSTEM_CFLAGS := %s" % " ".join(system["cflags"]),
        "KOS_DRIVER_OBJECTS := %s" % " ".join(objects),
        "KOS_STORE_FILES := %s" % " ".join(
            os.path.join(modules, name) for name in system["store"]
            if name not in omit_store),
        "",
    ]
    with open(os.path.join(build_dir, "system.mk"), "w",
              encoding="utf-8") as handle:
        handle.write("\n".join(lines))


def write_boot_manifest(build_dir, drivers):
    modules = os.path.join(build_dir, "modules")
    os.makedirs(modules, exist_ok=True)
    manifest = bytearray()
    for driver_id, _manifest in drivers:
        image = driver_id + ".mod"
        check_romfs_name("driver %r" % driver_id, image)
        encoded = image.encode("ascii")
        manifest += encoded + bytes(MANIFEST_ENTRY_SIZE - len(encoded))
    with open(os.path.join(modules, "drivers.man"), "wb") as handle:
        handle.write(bytes(manifest))


def write_store(build_dir, store):
    with open(os.path.join(build_dir, "store.json"), "w",
              encoding="utf-8") as handle:
        json.dump(store, handle, indent=2)
        handle.write("\n")


# --------------------------------------------------------------------------
# driver entry point
# --------------------------------------------------------------------------


def generate(system_path, build_dir, omit=(), omit_store=()):
    system, system_name = load_system(system_path)
    root = os.path.dirname(os.path.dirname(os.path.abspath(system_path)))
    drivers = []
    ids = []
    for entry in system["drivers"]:
        driver_id = os.path.basename(entry)
        if driver_id in ids:
            raise SystemError("%s: driver %r is listed twice"
                              % (system_name, driver_id))
        ids.append(driver_id)
        path = find_manifest(root, entry)
        manifest = load_driver(path, driver_id)
        manifest["path"] = path
        drivers.append((driver_id, manifest))
    for driver_id in omit:
        if driver_id not in ids:
            raise SystemError("%s: --omit-image %r names no selected driver"
                              % (system_name, driver_id))
    for name in omit_store:
        if name not in system["store"]:
            raise SystemError("%s: --omit-store %r names no store entry"
                              % (system_name, name))
    resolve_set(system_name, drivers)
    store = build_store(system_name, drivers, system["store"], set(omit),
                        set(omit_store))
    validate_boot(system["boot"], store["files"], system_name,
                  missing=set(omit_store))
    validate_after_exit(system, store["files"], system_name)

    os.makedirs(build_dir, exist_ok=True)
    write_system_mk(build_dir, system, drivers, set(omit_store))
    write_store(build_dir, store)
    write_boot_manifest(build_dir, drivers)
    write_resolved(build_dir, drivers)
    print("systemgen: %s: %d drivers, %d store files, boot %s after-exit %s"
          % (system_name, len(drivers), len(store["files"]),
             system["boot"], system["after_exit"]))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="generate an image's build inputs from its system file")
    parser.add_argument("system", help="systems/<name>.json")
    parser.add_argument("build", help="the build directory to write into")
    parser.add_argument("--omit-image", action="append", default=[],
                        metavar="DRIVER",
                        help="leave this selected driver's image out of the "
                             "store while its name stays in the boot "
                             "manifest")
    parser.add_argument("--omit-store", action="append", default=[],
                        metavar="NAME",
                        help="leave this store entry out of the packed "
                             "store; a boot program naming it is accepted, "
                             "so the launcher's failure path has a store")
    args = parser.parse_args(argv)
    try:
        return generate(args.system, args.build, args.omit_image,
                        args.omit_store)
    except SystemError as error:
        print("systemgen: error: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
