#!/usr/bin/env python3
"""Resident-service spike: per-object two-bank layout report.

Reads the package-35 spike link map, the object files that link used and
the classification file (tests/m10/two-bank-classification.json), then
reports, for the revision-13 6000h split:

  * every linked object with its area sizes and its tier;
  * the fixed core (fixed code + data + the ISR and service stacks, plus
    the M10 estimate rows) against the 2000h-5FFFh fixed region;
  * the cold nucleus against bank 7's window half (6000h-DFFFh);
  * the service bank: the hot window set (drivers split hot/boot from
    their symbol tables, the slimmed VFS service, the console) against
    the 32 KiB window, and the out-of-window tier (boot-only code and
    bulk data) against its own region;
  * the smallest set of relief changes that would bring the fixed core
    under the fixed region when it exceeds it;
  * the gap between the classified fixed core and the linked one.

The build *is* the measurement; this tool never edits anything.  It fails
when the linked objects do not match the classification file, so a stale
classification cannot survive.

Usage:
    rev13_layout.py MAP --objdir DIR --classification FILE
                        [--module-dir DIR] [--out FILE] [--verbose]
"""

import argparse
import json
import os
import re
import sys
from pathlib import Path

BANK7_BASE = 0x2000
WINDOW_SPLIT = 0x6000
WINDOW_END = 0xE000
TWO_BANK_TIERS = ("fixed", "cold", "service", "retired")
AREA_LINE = re.compile(r"^A (\S+) size ([0-9A-F]+) flags (\d+) addr ")


class LayoutError(Exception):
    pass


def parse_rel(path):
    """Area sizes of one sdld object file."""
    areas = {}
    for line in Path(path).read_text().splitlines():
        match = AREA_LINE.match(line)
        if match:
            areas[match.group(1)] = int(match.group(2), 16)
    return areas


def parse_map(text):
    symbols = {}
    for line in text.splitlines():
        match = re.match(r"\s*([0-9A-Fa-f]{8})\s+(\S+)", line)
        if match:
            symbols[match.group(2)] = int(match.group(1), 16)
    return symbols


def object_sizes(objdir):
    sizes = {}
    for name in sorted(os.listdir(objdir)):
        if name.endswith(".rel"):
            sizes[name[:-4]] = parse_rel(os.path.join(objdir, name))
    return sizes


def size_of(sizes, stem, area):
    return sizes.get(stem, {}).get(area, 0)


def object_bytes(sizes, stem):
    """(code, data) of one object, whichever segment it compiled into."""
    code = size_of(sizes, stem, "_CODE") or size_of(sizes, stem, "_WINDOW")
    data = (size_of(sizes, stem, "_DATA")
            or size_of(sizes, stem, "_WINDOW_DATA"))
    return code, data


def parse_sym_sizes(path):
    """Per-symbol sizes from one sdasz80 .sym file (spans within the area).

    The assembler's symbol table carries every symbol with its area and
    offset; the size of a symbol is the distance to the next symbol in the
    same area (the area size for the last one), so the table is measured,
    not estimated."""
    syms = []
    areas = {}
    in_areas = False
    for line in Path(path).read_text(errors="replace").splitlines():
        if line.strip() == "Area Table":
            in_areas = True
            continue
        if in_areas:
            match = re.match(r"\s*(\d+)\s+(\S+)\s+size\s+([0-9A-F]+)",
                             line)
            if match:
                areas[int(match.group(1))] = int(match.group(3), 16)
            continue
        match = re.match(r"\s*(\d+)\s+(\S+)\s+([0-9A-F]{8})(?:\s|$)", line)
        if match:
            syms.append((int(match.group(1)), match.group(2),
                         int(match.group(3), 16)))
    by_area = {}
    for area, name, addr in syms:
        by_area.setdefault(area, []).append((addr, name))
    out = {}
    for area, entries in by_area.items():
        entries.sort()
        for index, (addr, name) in enumerate(entries):
            end = (entries[index + 1][0] if index + 1 < len(entries)
                   else areas.get(area, 0))
            if end > addr:
                out[name] = end - addr
    return out


def report_symbol_sizes(path):
    """Per-symbol sizes from a zmodpack module report (global symbols).

    The report's `symbols` map holds every exported symbol's offset in the
    packed image; the size is the distance to the next exported symbol (or
    the image end).  It is the only measured source for the asm objects,
    which have no .sym file."""
    report = json.loads(Path(path).read_text())
    symbols = report.get("symbols", {})
    offsets = sorted((offset, name) for name, offset in symbols.items())
    out = {}
    for index, (offset, name) in enumerate(offsets):
        end = (offsets[index + 1][0] if index + 1 < len(offsets)
               else report["image_size"])
        if end > offset:
            out[name] = end - offset
    return out, report


def module_split(module_dir, name, entry):
    """Hot and boot-only bytes of one resident module, from symbol sizes.

    C objects contribute their .sym tables; the asm objects (no .sym)
    contribute the module report's exported symbols.  Every symbol the
    classification names as boot-only must resolve, so a renamed or
    removed symbol cannot silently move bytes between the tiers."""
    manifest_path = Path(entry["manifest"])
    manifest = json.loads(manifest_path.read_text())
    object_dir = Path(module_dir)
    table = {}
    for object_name in manifest["objects"]:
        stem = object_name[:-4] if object_name.endswith(".rel") \
            else object_name
        sym_path = object_dir / (stem + ".sym")
        if sym_path.exists():
            for symbol, size in parse_sym_sizes(sym_path).items():
                table[symbol] = size
    report_path = object_dir / (name + ".report.json")
    if not report_path.exists():
        raise LayoutError("%s: module report %s is missing (run the build)"
                          % (name, report_path))
    report_sizes, report = report_symbol_sizes(report_path)
    boot = {}
    for symbol in entry["boot_symbols"]:
        if symbol in table:
            boot[symbol] = table[symbol]
        elif symbol in report_sizes:
            boot[symbol] = report_sizes[symbol]
        else:
            raise LayoutError("%s: boot symbol %s not found in %s or %s"
                              % (name, symbol, object_dir, report_path))
    image_size = report["image_size"]
    bss_size = report["bss_size"]
    total = image_size + bss_size
    boot_bytes = sum(boot.values())
    if boot_bytes > total:
        raise LayoutError("%s: boot symbols total %d bytes, over the %d-byte "
                          "module" % (name, boot_bytes, total))
    return {
        "name": name,
        "image_size": image_size,
        "bss_size": bss_size,
        "total": total,
        "boot": boot_bytes,
        "hot": total - boot_bytes,
        "boot_symbols": boot,
    }


def sum_estimates(rows, tier):
    total = {"code_low": 0, "code_high": 0, "data_low": 0, "data_high": 0,
             "common_low": 0, "common_high": 0}
    for row in rows:
        if row["tier"] == tier:
            for key in total:
                total[key] += row[key]
    return total


def relief_change(sizes, change):
    """Bytes freed in the fixed region by one candidate change.

    `objects` are measured from the build; `fixed_bytes` carries a
    labeled estimate for a change whose detail is not yet implemented
    (reported as an estimate, never as a measurement)."""
    freed = 0
    for stem in change["objects"]:
        code, data = object_bytes(sizes, stem)
        freed += code + data
    return freed + change.get("fixed_bytes", 0)


def two_bank_ledgers(sizes, classification, module_dir, verbose):
    """The classification summary, both ledgers and the service projection."""
    entries = classification["objects"]
    tiers = {}
    for entry in entries:
        tier = entry["tier"]
        if tier not in TWO_BANK_TIERS:
            raise LayoutError("unknown tier %r for %s" % (tier,
                                                          entry["object"]))
        stem = entry["object"]
        if stem not in sizes:
            raise LayoutError("classified object %s is not linked in %s"
                              % (stem, module_dir))
        code, data = object_bytes(sizes, stem)
        tiers.setdefault(tier, []).append({
            "object": stem, "tier": tier, "code": code, "data": data,
            "reason": entry.get("reason", ""),
            "ledger": entry.get("ledger", True),
        })
    for stem in sorted(sizes):
        if stem not in [entry["object"] for entry in entries]:
            raise LayoutError("linked object %s has no classification entry"
                              % stem)
    def totals(tier, ledger_only=True):
        rows = [row for row in tiers.get(tier, [])
                if row["ledger"] or not ledger_only]
        return (sum(row["code"] for row in rows),
                sum(row["data"] for row in rows), rows)

    fixed_code, fixed_data, fixed_rows = totals("fixed")
    cold_code, cold_data, cold_rows = totals("cold")
    service_code, service_data, service_rows = totals("service")
    retired_code, retired_data, _ = totals("retired")

    fixed_stack = sum(item["size"] for item in classification["stacks"]
                      if item["tier"] == "fixed")
    other_stack = sum(item["size"] for item in classification["stacks"]
                      if item["tier"] != "fixed")
    fixed_est = sum_estimates(classification["estimates"], "fixed")
    cold_est = sum_estimates(classification["estimates"], "cold")

    common_objects = classification.get("common_resident", [])
    common_code = sum(size_of(sizes, stem, "_COMMON") for stem in common_objects)
    common_data = sum(size_of(sizes, stem, "_COMMON_DATA")
                      for stem in common_objects)

    fixed_region = WINDOW_SPLIT - BANK7_BASE
    window_half = WINDOW_END - WINDOW_SPLIT
    out_of_window = WINDOW_SPLIT + (0x10000 - WINDOW_END)
    used = fixed_code + fixed_data + fixed_stack
    entry = {
        "fixed_region": fixed_region,
        "fixed_used": used,
        "fixed_room": fixed_region - used,
        "fixed_used_low": used + fixed_est["code_low"] +
                          fixed_est["data_low"],
        "fixed_used_high": used + fixed_est["code_high"] +
                           fixed_est["data_high"],
        "window_half": window_half,
        "cold_used": cold_code + cold_data,
        "cold_room": window_half - (cold_code + cold_data),
        "cold_used_low": cold_code + cold_data + cold_est["code_low"] +
                         cold_est["data_low"],
        "cold_used_high": cold_code + cold_data + cold_est["code_high"] +
                          cold_est["data_high"],
        "out_of_window": out_of_window,
    }
    entry["fixed_room_low"] = fixed_region - entry["fixed_used_high"]
    entry["fixed_room_high"] = fixed_region - entry["fixed_used_low"]
    entry["cold_room_low"] = window_half - entry["cold_used_high"]
    entry["cold_room_high"] = window_half - entry["cold_used_low"]
    splits = {"%04X" % WINDOW_SPLIT: entry}

    modules = {}
    modules_total = {"hot": 0, "boot": 0, "total": 0}
    for name, module in sorted(classification["service_modules"].items()):
        split = module_split(module_dir, name, module)
        modules[name] = split
        modules_total["hot"] += split["hot"]
        modules_total["boot"] += split["boot"]
        modules_total["total"] += split["total"]

    vfs = classification["service_vfs"]
    vfs_members = [entry["object"] for entry in entries
                   if entry["tier"] == "service"
                   and entry["object"] in vfs["members"]]
    vfs_service_code = sum(object_bytes(sizes, stem)[0] for stem in vfs_members)
    vfs_service_data = sum(object_bytes(sizes, stem)[1] for stem in vfs_members)
    primitive = vfs["nucleus_primitive"]
    primitive_code, primitive_data = object_bytes(sizes, primitive)
    retired = {}
    for stem in vfs["retired"]:
        retired[stem] = object_bytes(sizes, stem)
    savings = vfs["idiom_savings"]
    idiom_low = (vfs_service_code + vfs_service_data) * \
        savings["low_percent"] // 100
    idiom_high = (vfs_service_code + vfs_service_data) * \
        savings["high_percent"] // 100

    console = [entry["object"] for entry in entries
               if entry["tier"] == "service"
               and entry["object"] not in vfs["members"]
               and entry.get("ledger", True)]
    console_code = sum(object_bytes(sizes, stem)[0] for stem in console)
    console_data = sum(object_bytes(sizes, stem)[1] for stem in console)

    bulk_data = []
    for item in classification.get("bulk_data", []):
        path = Path(module_dir) / item["file"]
        if not path.exists():
            raise LayoutError("bulk data %s is missing (%s)"
                              % (item["name"], path))
        bulk_data.append({"name": item["name"],
                          "size": path.stat().st_size,
                          "reason": item.get("reason", "")})
    bulk_total = sum(item["size"] for item in bulk_data)

    window_hot_code = (modules_total["hot"] + vfs_service_code +
                       console_code)
    window_hot_data = (vfs_service_data + console_data)
    window_total = window_hot_code + window_hot_data
    out_of_window_total = modules_total["boot"] + bulk_total

    service = {
        "window": {
            "hot_code": modules_total["hot"] + vfs_service_code +
                        console_code,
            "hot_data": vfs_service_data + console_data,
            "service_stacks": 0,
            "total": window_total,
            "window_size": WINDOW_END - WINDOW_SPLIT,
        },
        "out_of_window": {
            "boot_code": modules_total["boot"],
            "bulk_data": bulk_total,
            "bulk_items": bulk_data,
            "total": out_of_window_total,
            "size": splits["%04X" % WINDOW_SPLIT]["out_of_window"],
        },
        "modules": modules,
        "modules_total": modules_total,
        "vfs": {
            "members": vfs_members,
            "code": vfs_service_code,
            "data": vfs_service_data,
            "primitive": primitive,
            "primitive_code": primitive_code,
            "primitive_data": primitive_data,
            "retired": {stem: {"code": code, "data": data}
                        for stem, (code, data) in retired.items()},
            "idiom_savings_low": idiom_low,
            "idiom_savings_high": idiom_high,
        },
        "console_code": console_code,
        "console_data": console_data,
    }

    relief = []
    for change in classification.get("relief", []):
        freed = relief_change(sizes, change)
        relief.append({"change": change["change"],
                       "objects": change["objects"],
                       "freed": freed,
                       "kind": change.get("kind", "design")})

    return {
        "tiers": {
            "fixed": {"code": fixed_code, "data": fixed_data},
            "cold": {"code": cold_code, "data": cold_data},
            "service": {"code": service_code, "data": service_data,
                        "members": len(service_rows)},
            "retired": {"code": retired_code, "data": retired_data},
        },
        "stacks": {"fixed": fixed_stack, "other": other_stack},
        "estimates": {
            "fixed": fixed_est,
            "cold": cold_est,
        },
        "common": {"code": common_code, "data": common_data},
        "splits": splits,
        "service": service,
        "relief": relief,
        "placement": {
            "classified_fixed_but_linked_cold": [
                {"object": row["object"], "tier": row["tier"],
                 "code": row["code"], "data": row["data"]}
                for row in fixed_rows
                if size_of(sizes, row["object"], "_WINDOW") > 0
                or size_of(sizes, row["object"], "_WINDOW_DATA") > 0],
            "classified_elsewhere_but_linked_fixed": [
                {"object": row["object"], "tier": row["tier"],
                 "code": row["code"], "data": row["data"]}
                for tier in TWO_BANK_TIERS if tier != "fixed"
                for row in tiers.get(tier, [])
                if size_of(sizes, row["object"], "_CODE") > 0],
        },
    }


def print_two_bank(ledgers, measured, classification):
    print("two-bank classification: fixed %d code + %d data; cold %d + %d; "
          "service %d + %d; retired %d + %d; stacks fixed %d; common "
          "objects %d code + %d data"
          % (ledgers["tiers"]["fixed"]["code"],
             ledgers["tiers"]["fixed"]["data"],
             ledgers["tiers"]["cold"]["code"],
             ledgers["tiers"]["cold"]["data"],
             ledgers["tiers"]["service"]["code"],
             ledgers["tiers"]["service"]["data"],
             ledgers["tiers"]["retired"]["code"],
             ledgers["tiers"]["retired"]["data"],
             ledgers["stacks"]["fixed"],
             ledgers["common"]["code"], ledgers["common"]["data"]))
    for base, entry in sorted(ledgers["splits"].items()):
        print("two-bank fixed %sh: %d code + %d data + %d stacks = %d / %d "
              "bytes, room %d; M10 estimates +%d..%d -> room %d..%d"
              % (base, ledgers["tiers"]["fixed"]["code"],
                 ledgers["tiers"]["fixed"]["data"],
                 ledgers["stacks"]["fixed"], entry["fixed_used"],
                 entry["fixed_region"], entry["fixed_room"],
                 entry["fixed_used_low"] - entry["fixed_used"],
                 entry["fixed_used_high"] - entry["fixed_used"],
                 entry["fixed_room_low"], entry["fixed_room_high"]))
        print("two-bank cold  %sh: %d code + %d data = %d / %d bytes "
              "(window half), room %d; estimates -> room %d..%d"
              % (base, ledgers["tiers"]["cold"]["code"],
                 ledgers["tiers"]["cold"]["data"], entry["cold_used"],
                 entry["window_half"], entry["cold_room"],
                 entry["cold_room_low"], entry["cold_room_high"]))
    service = ledgers["service"]
    window = service["window"]
    out = service["out_of_window"]
    print("service bank: window hot %d code + %d data + %d service stacks "
          "= %d / %d (32 KiB at 6000h); out-of-window boot %d code + %d "
          "bulk data = %d / %d (32 KiB at 6000h)"
          % (window["hot_code"], window["hot_data"],
             window["service_stacks"], window["total"],
             window["window_size"],
             out["boot_code"], out["bulk_data"], out["total"],
             out["size"]))
    vfs = service["vfs"]
    print("service bank: VFS service %d code + %d data (members: %s); "
          "nucleus reader primitive %d code + %d data; retired fs %d; "
          "idiom savings (estimate, excluded) %d..%d"
          % (vfs["code"], vfs["data"], ", ".join(vfs["members"]),
             vfs["primitive_code"], vfs["primitive_data"],
             vfs["retired"].get("fs", {}).get("code", 0),
             vfs["idiom_savings_low"], vfs["idiom_savings_high"]))
    for name, entry in sorted(service["modules"].items()):
        print("service bank:   %-7s hot %5d + boot %4d = %5d bytes "
              "(%d code + %d bss)"
              % (name, entry["hot"], entry["boot"], entry["total"],
                 entry["image_size"], entry["bss_size"]))
    if ledgers["relief"]:
        print("two-bank relief: %s"
              % "; ".join("%s (frees %d)" % (r["change"], r["freed"])
                          for r in ledgers["relief"]))
    if measured and measured.get("linked_fixed_used") is not None:
        fixed_used = ledgers["splits"]["%04X"
                                       % measured["split"]]["fixed_used"]
        print("two-bank linked: kernel code end %04Xh, data base %04Xh, "
              "ISR stack %04Xh-%04Xh; linked fixed core %d vs classified "
              "%d (linked - classified = %+d)"
              % (measured["kernel_code_end"], measured["data_base"],
                 measured["isr_stack_base"], measured["isr_stack_top"],
                 measured["linked_fixed_used"], fixed_used,
                 measured["linked_fixed_used"] - fixed_used))
        cold = ledgers["placement"]["classified_fixed_but_linked_cold"]
        if cold:
            print("two-bank linked: classified fixed but linked cold: %s"
                  % ", ".join("%s %d" % (row["object"],
                                         row["code"] + row["data"])
                              for row in cold))
        misfiled = ledgers["placement"]["classified_elsewhere_but_linked_fixed"]
        if misfiled:
            print("two-bank linked: classified elsewhere but linked fixed: "
                  "%s" % ", ".join("%s (%s) %d"
                                   % (row["object"], row["tier"],
                                      row["code"] + row["data"])
                                   for row in misfiled))





def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("map")
    parser.add_argument("--objdir", required=True)
    parser.add_argument("--classification", required=True)
    parser.add_argument("--two-bank", action="store_true",
                        help="accepted for compatibility: the two-bank "
                             "ledgers (fixed, cold, service) are the only "
                             "report this tool produces")
    parser.add_argument("--module-dir", default="build/modules",
                        help="directory holding the resident module .sym / "
                             ".report.json artifacts")
    parser.add_argument("--out")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv[1:])

    try:
        symbols = parse_map(Path(args.map).read_text())
        classification = json.loads(Path(args.classification).read_text())
        sizes = object_sizes(args.objdir)
        ledgers = two_bank_ledgers(sizes, classification,
                                   args.module_dir, args.verbose)
        measured = {
            "split": WINDOW_SPLIT,
            "kernel_code_end": symbols.get("__kernel_code_end"),
            "data_base": symbols.get("s__DATA"),
            "isr_stack_base": symbols.get("irq_isr_stack"),
            "isr_stack_top": symbols.get("irq_isr_stack_top"),
        }
        if measured["kernel_code_end"] is not None:
            measured["linked_fixed_used"] = (
                measured["kernel_code_end"] - BANK7_BASE
                + (measured["isr_stack_base"] - measured["data_base"])
                + (measured["isr_stack_top"] -
                   measured["isr_stack_base"]))
        print_two_bank(ledgers, measured, classification)
        if args.verbose:
            for tier in TWO_BANK_TIERS:
                for entry in classification["objects"]:
                    if entry["tier"] == tier:
                        code, data = object_bytes(sizes,
                                                  entry["object"])
                        print("    %-16s %-8s %6d %6d  %s"
                              % (entry["object"], tier, code, data,
                                 entry.get("reason", "")))
        if args.out:
            Path(args.out).write_text(json.dumps(
                {"ledgers": ledgers, "measured": measured,
                 "classification": args.classification},
                indent=2, sort_keys=True) + "\n")
        return 0
    except LayoutError as exc:
        print("rev13_layout: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
