#!/usr/bin/env python3
"""Console cost profile: where one console character's T-states go.

A script, not a unittest.  It splices `iobench.exe` into the production
image's store, boots to the shell, runs the bench, and charges every
instruction's T-states (the step delta) to a bucket by the PC and the
active bank latch at the step's start.  The row spans match the bench's
own measurement: from the first entry to the row's key call - after the
row's first clock reading - to the CALL that starts the row's second
clock reading.  The two readings are the bench's instrument (subtracted
from its reported total), so they stay outside the span.  Because
printf itself puts characters, a row's span is only armed once the
previous row's line is in the console trace.

The bucket ranges come from the driver placement plan, the driver
reports and the link maps.  The hand-written v9958_io.asm has no build
listing, so the script generates one with sdasz80 (the build's own
flags) and aligns it on `_v9958_io_wait_command` against the report.
The shared `_v9958_io_stream_indirect` is attributed through the last
CALL site, so a command block streamed for a fill or a scroll is charged
to the code that started it.  `_v9958_out_flush` writes the glyphs
itself (direct VRAM writes inside a bulk-map span); its bucket is the
whole flush, its CE wait the shared `_v9958_io_wait_command`.

The bench subtracts two full clock readings from every batch although
only one falls inside the interval, so its printed totals underreport by
about one reading; the check below adds the calibration loop's measured
reading cost back before the 1% comparison.

Simulator model note: V9958Peer keeps every command busy a fixed 400
instruction counts, so the CE-wait and command figures are CPU-side
only; the board rows answer the engine and WAIT questions.

Usage: python3 tests/console_profile.py
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import pack_image  # noqa: E402
import romfspack  # noqa: E402
from replay import ConsoleReplay, STEP_LIMIT  # noqa: E402
from z80sim import Ctc, read_symbols  # noqa: E402

BUILD = os.path.join(ROOT, "build")
MODULES = os.path.join(BUILD, "modules")
IMAGE = os.path.join(BUILD, "konaos.bin")
KERNEL_MAP = os.path.join(BUILD, "konaos.map")
STORE = os.path.join(BUILD, "store.json")
PROGRAM_MAP = os.path.join(BUILD, "user", "iobench", "iobench.map")
IOBENCH = os.path.join(BUILD, "user", "iobench.exe")

ROWS = (
    ("con.putchar", "_putchar"),
    ("con.printf", "_printf"),
    ("con.write", "_console_write"),
    ("con.mixed", "_console_write"),
    ("con.space", "_console_write"),
    ("con.held", "_console_write"),
)
# The line the bench prints before each row's batch: the previous
# row's report (the ipc row's for the first console row).
PREVIOUS_LINE = (
    "iobench: ipc.null ops",
    "iobench: con.putchar ops",
    "iobench: con.printf ops",
    "iobench: con.write ops",
    "iobench: con.mixed ops",
    "iobench: con.space ops",
)
CHARS = 2000

ROW_RE = re.compile(
    r"iobench:\s+(con\.\w+)\s+ops\s+(\d+)\s+bytes\s+(\d+)\s+min\s+(\d+)"
    r"\s+mean\s+(\d+)\s+max\s+(\d+)\s+per-byte\s+(\d+)")

BUCKETS = [
    "SDK and libc (program)",
    "syscall gate and kernel IPC",
    "console driver",
    "V9958 dispatch and staging",
    "wrapper (private stack, R#8)",
    "parser (term_feed)",
    "fast path (term_write)",
    "glyph drawing (out_flush)",
    "CE wait",
    "SAT writes",
    "scroll",
    "other",
]
OTHER = (
    "other: terminal glue (C)",
    "other: C worker",
    "other: timer driver (interrupt)",
    "other: v9958 remainder",
    "other: unclassified",
)


def die(message):
    print("console_profile.py: %s" % message, file=sys.stderr)
    sys.exit(1)


# --- store splicing (the bench's program into the production store) ------

def splice_store(image, stream):
    data = bytearray(image)
    window = pack_image.ROMFS_WINDOW_SIZE
    for page in range(pack_image.ROMFS_FIRST_PAGE,
                      pack_image.ROMFS_FIRST_PAGE +
                      pack_image.ROMFS_PAGE_COUNT):
        base = page * pack_image.PAGE_SIZE
        data[base:base + window] = bytes([pack_image.FILL]) * window
    logical = 0
    while logical < len(stream):
        page = pack_image.ROMFS_FIRST_PAGE + logical // window
        offset = logical % window
        chunk = min(window - offset, len(stream) - logical)
        base = page * pack_image.PAGE_SIZE + offset
        data[base:base + chunk] = stream[logical:logical + chunk]
        logical += chunk
    return bytes(data)


def pack_store_with_bench():
    tmp = tempfile.mkdtemp(prefix="console-profile-store-")
    manifest = json.loads(Path(STORE).read_text())
    manifest["files"] = [entry for entry in manifest["files"]
                         if entry["name"].lower() != "iobench.exe"]
    manifest["files"].append({"name": "iobench.exe", "type": "EXE",
                              "path": IOBENCH})
    out_json = os.path.join(tmp, "store.json")
    Path(out_json).write_text(json.dumps(manifest))
    stream, _report = romfspack.pack(out_json,
                                     os.path.join(tmp, "store.romfs"),
                                     objdir=MODULES)
    image = splice_store(Path(IMAGE).read_bytes(), stream)
    shutil.rmtree(tmp, ignore_errors=True)
    return image


class ProfileReplay(ConsoleReplay):
    def __init__(self, image):
        self.root = ROOT
        self.subdir = "build"
        self.image_name = "console-profile.bin"
        self.image = image
        self.syms = read_symbols(KERNEL_MAP)


# --- ranges from the build outputs ---------------------------------------

def parse_lst_functions(lst_path):
    """{name: offset} for an SDCC listing's `; Function name` blocks."""
    with open(lst_path) as handle:
        lines = handle.read().splitlines()
    functions = {}
    for index, line in enumerate(lines):
        match = re.match(r"\s*\d+\s+;\s*Function\s+(\w+)", line)
        if not match:
            continue
        for probe in lines[index + 1:index + 6]:
            found = re.match(r"\s*([0-9A-Fa-f]{8})\s+\d+\s+(\S+):", probe)
            if found:
                functions[match.group(1)] = int(found.group(1), 16)
                break
    return functions


def generate_io_listing():
    """Regenerate v9958_io.asm's listing with the build's assembler and
    flags; returns {label: offset} aligned on the report."""
    tmp = tempfile.mkdtemp(prefix="console-profile-asm-")
    result = subprocess.run(
        ["sdasz80", "-Iabi", "-Iplatform/zephyr80/public",
         "-Iplatform/zephyr80/private", "-Itests/target/include", "-l",
         "-o", os.path.join(tmp, "v9958_io.rel"),
         "drivers/v9958/v9958_io.asm"],
        cwd=ROOT, capture_output=True, text=True)
    listing = os.path.join(tmp, "v9958_io.lst")
    if result.returncode != 0 or not os.path.isfile(listing):
        shutil.rmtree(tmp, ignore_errors=True)
        die("sdasz80 listing failed: %s" % result.stderr[-400:])
    labels = {}
    for line in Path(listing).read_text().splitlines():
        found = re.match(r"\s*([0-9A-Fa-f]{8})\s+\d+\s+"
                         r"(?:[0-9A-Fa-f]{2}r)?(\w+)::?", line)
        if found:
            labels[found.group(2).lstrip("_")] = int(found.group(1), 16)
    shutil.rmtree(tmp, ignore_errors=True)
    report = json.loads(Path(os.path.join(
        MODULES, "v9958.report.json")).read_text())
    shift = report["symbols"]["_v9958_io_wait_command"] - labels[
        "v9958_io_wait_command"]
    return {name: offset + shift for name, offset in labels.items()}


def build_ranges():
    """The bucket ranges and the program markers, from the build."""
    plan = json.loads(Path(os.path.join(MODULES,
                                        "drivers.plan.json")).read_text())
    by_name = {driver["name"]: driver for driver in plan["drivers"]}
    for module in ("v9958.mod", "console-v9958-hid.mod", "timer.mod"):
        if module not in by_name:
            die("the driver plan has no %s" % module)

    report = json.loads(Path(os.path.join(MODULES,
                                          "v9958.report.json")).read_text())
    base = by_name["v9958.mod"]["hot_base"]
    hot_size = report["segments"][0]["image_size"]
    symbols = report["symbols"]
    c = parse_lst_functions(os.path.join(MODULES, "v9958.lst"))
    proto = parse_lst_functions(os.path.join(MODULES, "v9958_driver.lst"))
    asm = generate_io_listing()

    for name in ("v9958_cursor_write_sat", "v9958_init_g6",
                 "v9958_fill_cells", "v9958_copy_rows", "v9958_term_sync",
                 "v9958_scroll_up", "v9958_advance_cursor", "v9958_newline",
                 "v9958_put_printable", "v9958_term_cr", "v9958_term_feed",
                 "v9958_overtaken", "v9958_char_putc_worker",
                 "v9958_char_write_worker", "v9958_send"):
        if name not in c:
            die("v9958.lst has no %s" % name)
    for name in ("v9958_sprites_off", "v9958_hold_restore"):
        if name not in asm:
            die("the asm listing has no %s" % name)
    for name in ("_v9958_out_flush", "_v9958_term_write",
                 "_v9958_char_write", "_v9958_render_transaction",
                 "_v9958_io_wait_command", "_v9958_io_stream_ctrl",
                 "_v9958_io_stream_indirect", "_v9958_driver_dispatch",
                 "_v9958_driver_init"):
        if name not in symbols:
            die("the v9958 report has no %s" % name)
    for name in ("v9958_driver_write", "v9958_driver_dispatch",
                 "v9958_driver_init"):
        if name not in proto:
            die("v9958_driver.lst has no %s" % name)

    def at(offset):
        return base + offset

    def region(lo, hi):
        return (at(lo), at(hi))

    # The proto part's shift: the report's dispatch offset minus the
    # listing's.
    proto_shift = (symbols["_v9958_driver_dispatch"] -
                   proto["v9958_driver_dispatch"])

    ranges = {
        "wrapper (private stack, R#8)": [
            region(asm["v9958_sprites_off"], asm["v9958_hold_restore"]),
            (at(symbols["_v9958_char_write"]),
             at(symbols["_v9958_render_transaction"])),
        ],
        "glyph drawing (out_flush)": [
            (at(symbols["_v9958_out_flush"]),
             at(symbols["_v9958_term_write"])),
        ],
        "CE wait": [
            (at(symbols["_v9958_io_wait_command"]),
             at(symbols["_v9958_io_stream_ctrl"])),
        ],
        "fast path (term_write)": [
            (at(symbols["_v9958_term_write"]),
             at(symbols["_v9958_char_write"])),
        ],
        "parser (term_feed)": [
            region(c["v9958_term_feed"], c["v9958_overtaken"]),
        ],
        "SAT writes": [
            region(c["v9958_cursor_write_sat"], c["v9958_init_g6"]),
        ],
        "scroll": [
            region(c["v9958_scroll_up"], c["v9958_advance_cursor"]),
            region(c["v9958_fill_cells"], c["v9958_term_sync"]),
        ],
        "other: terminal glue (C)": [
            region(c["v9958_term_sync"], c["v9958_scroll_up"]),
            region(c["v9958_advance_cursor"], c["v9958_newline"]),
            region(c["v9958_newline"], c["v9958_put_printable"]),
            region(c["v9958_put_printable"], c["v9958_term_cr"]),
            region(c["v9958_term_cr"], c["v9958_term_feed"]),
        ],
        "other: C worker": [
            region(c["v9958_overtaken"], c["v9958_char_putc_worker"]),
            region(c["v9958_char_putc_worker"], c["v9958_char_write_worker"]),
            region(c["v9958_char_write_worker"], c["v9958_send"]),
        ],
        "other: v9958 remainder": [(base, base + hot_size)],
        "V9958 dispatch and staging": [
            (at(proto["v9958_driver_write"] + proto_shift),
             at(proto["v9958_driver_init"] + proto_shift)),
        ],
        "_stream_indirect": [at(symbols["_v9958_io_stream_indirect"])],
        "_staging_kernel": [],
    }

    console = json.loads(Path(os.path.join(
        MODULES, "console-v9958-hid.report.json")).read_text())
    ranges["console driver"] = [
        (by_name["console-v9958-hid.mod"]["hot_base"],
         by_name["console-v9958-hid.mod"]["hot_base"] +
         console["segments"][0]["image_size"]),
    ]
    timer = json.loads(Path(os.path.join(
        MODULES, "timer.report.json")).read_text())
    ranges["other: timer driver (interrupt)"] = [
        (by_name["timer.mod"]["hot_base"],
         by_name["timer.mod"]["hot_base"] +
         timer["segments"][0]["image_size"]),
    ]

    # The nucleus copies the console batch into the driver's staging
    # block: the API trampoline and its copy loop.
    kernel_symbols = sorted(read_symbols(KERNEL_MAP).items(),
                            key=lambda pair: pair[1])
    kernel_bounds = {name: address for name, address in kernel_symbols}
    staging = []
    for name in ("_api_copy_in", "_ipc_nucleus_copy_in"):
        start = kernel_bounds[name]
        end = None
        for other, address in kernel_symbols:
            if address > start and other != name:
                end = address
                break
        if end is None:
            die("no bound after %s in the kernel map" % name)
        staging.append((start, end))
    ranges["_staging_kernel"].extend(staging)

    program = read_symbols(PROGRAM_MAP)
    for name in ("_putchar", "_printf", "_console_write", "_timer_now"):
        if name not in program:
            die("the program map has no %s" % name)
    return ranges, program


def classify(ranges, decoder, pc, latch):
    """The bucket for (pc, latch), preferring the most specific range."""
    bank = decoder.bank_of(pc, latch)
    if bank == 7:
        for lo, hi in ranges["_staging_kernel"]:
            if lo <= pc < hi:
                return "V9958 dispatch and staging"
        return "syscall gate and kernel IPC"
    if bank == 0 and pc >= 0xE000:
        return "syscall gate and kernel IPC"
    if bank in range(0, 6):
        return "SDK and libc (program)"
    if bank != 6:
        return "other: unclassified"
    # The specific ranges first, the whole-module remainder last.
    for name in ("V9958 dispatch and staging", "wrapper (private stack, R#8)",
                 "parser (term_feed)", "fast path (term_write)",
                 "glyph drawing (out_flush)", "CE wait", "SAT writes",
                 "scroll", "other: terminal glue (C)", "other: C worker",
                 "console driver", "other: timer driver (interrupt)",
                 "other: v9958 remainder"):
        for lo, hi in ranges.get(name, ()):
            if lo <= pc < hi:
                return name
    return "other: unclassified"


def run_profile(replay, z, ranges, program):
    """Step through the bench and charge each row span's T-states."""
    timer_now = program["_timer_now"]
    stream_indirect = ranges["_stream_indirect"][0]
    spans = []
    stream_caller = None
    glyph_start = len(z.glyphs)

    print("console_profile.py: booting to the shell", flush=True)
    replay.boot_to_shell(z)
    z.hid.hid_queue.extend(b"/system/iobench.exe\r")

    row_index = 0
    phase = "wait-line"   # the previous row's line, then the reading
    timer_seen = False
    span = None
    # The bench subtracts two full clock readings from every batch even
    # though only one falls inside the interval, so its totals underreport
    # by about one reading.  The calibration loop's back-to-back readings
    # measure that cost here, for the check below.
    last_call = None
    calibration = []

    started = time.time()
    for _step in range(STEP_LIMIT):
        pc = z.pc
        latch = z.latch
        op = z.rd(pc)
        is_timer_call = (op == 0xCD and
                         z.rd16((pc + 1) & 0xFFFF) == timer_now)
        is_stream_call = (op == 0xCD and
                          z.rd16((pc + 1) & 0xFFFF) == stream_indirect)

        if phase == "wait-line":
            if is_timer_call:
                if last_call is not None and row_index == 0 \
                        and not timer_seen:
                    calibration.append(z.tstates - last_call)
                last_call = z.tstates
                if timer_seen:
                    phase = "wait-key"
        elif phase == "wait-key":
            if pc == program[ROWS[row_index][1]]:
                span = {"start": z.tstates, "buckets":
                        {name: 0 for name in BUCKETS + list(OTHER)}}
                phase = "measure"
        elif phase == "measure":
            if is_timer_call:
                span["end"] = z.tstates
                spans.append(span)
                span = None
                if row_index + 1 < len(ROWS):
                    row_index += 1
                    timer_seen = False
                    phase = "wait-line"
                else:
                    phase = "finish"

        if is_stream_call:
            stream_caller = classify(ranges, z.decoder, pc, latch)

        before = z.tstates
        replay.skip_delay(z)
        replay.feed(z)
        z.step()
        spent = z.tstates - before
        if phase == "measure":
            bucket = classify(ranges, z.decoder, pc, latch)
            if pc == stream_indirect and stream_caller is not None:
                bucket = stream_caller
            span["buckets"][bucket] += spent

        if _step % 0x1FFF == 0:
            replay.pump(z)
            transcript = "".join(z.glyphs[glyph_start:])
            if "PANIC" in transcript:
                die("the replay panicked")
            if phase == "wait-line" and not timer_seen:
                if PREVIOUS_LINE[row_index] in transcript:
                    timer_seen = True
            if phase == "finish" and \
                    "iobench: vdp.hmmm" in transcript:
                break
    else:
        replay.pump(z)
        die("the bench did not finish within the step limit "
            "(phase %s, row %d, pc=%04X)"
            % (phase, row_index, z.pc))

    elapsed = time.time() - started
    reading_cost = sorted(calibration)[len(calibration) // 2] \
        if calibration else 0
    print("console_profile.py: profiled %d spans in %.1f s; "
          "clock reading cost %d T-states"
          % (len(spans), elapsed, reading_cost))

    replay.pump(z)
    transcript = "".join(z.glyphs[glyph_start:])
    parsed = {}
    # The 85-column console wraps long row lines mid-token; removing the
    # model's line breaks restores the logical lines for the parser.
    for match in ROW_RE.finditer(transcript.replace("\n", "")):
        parsed[match.group(1)] = {
            "ops": int(match.group(2)),
            "total": int(match.group(2)) * int(match.group(5)),
        }
    for name in ROWS:
        if name[0] not in parsed:
            print("missing row %s; transcript tail: %r"
                  % (name[0], transcript[-500:]))
            die("the bench output has no %s row" % name[0])
    return spans, parsed, reading_cost


def report(spans, parsed, reading_cost):
    print()
    print("T-states per character per bucket (per console row):")
    print("%-32s" % "bucket" +
          "".join("%12s" % row[0] for row in ROWS))
    for name in BUCKETS:
        cells = []
        for index in range(len(ROWS)):
            value = spans[index]["buckets"][name]
            if name == "other":
                value += sum(spans[index]["buckets"].get(sub, 0)
                             for sub in OTHER)
            cells.append("%12d" % (value // CHARS))
        print("%-32s" % name + "".join(cells))

    print()
    print("other, named (T-states per character):")
    print("%-34s" % "sub-bucket" +
          "".join("%12s" % row[0] for row in ROWS))
    for sub in OTHER:
        print("%-34s" % sub +
              "".join("%12d" % (spans[index]["buckets"].get(sub, 0)
                                // CHARS)
                      for index in range(len(ROWS))))

    print()
    print("bucket sums against the bench's own totals (+ one clock "
          "reading, which the bench's subtraction underreports):")
    ok = True
    for index, (row_name, _key) in enumerate(ROWS):
        span = spans[index]
        total = span["end"] - span["start"]
        bench = parsed[row_name]["total"] + reading_cost
        delta = total - bench
        pct = 100.0 * abs(delta) / bench if bench else 0.0
        verdict = "OK" if pct <= 1.0 else "MISMATCH"
        ok = ok and pct <= 1.0
        print("  %-10s profile %8d  bench %8d  delta %+7d (%5.2f%%)  %s"
              % (row_name, total, bench, delta, pct, verdict))
        if pct > 1.0:
            detail = {name: span["buckets"][name] for name in BUCKETS}
            for sub in OTHER:
                detail["other"] += span["buckets"].get(sub, 0)
            print("    buckets: " +
                  "  ".join("%s=%d" % (name, detail[name])
                            for name in BUCKETS))
    return ok


def main():
    if not os.path.isfile(IOBENCH):
        die("build/user/iobench.exe has not been built")
    image = pack_store_with_bench()
    replay = ProfileReplay(image)
    z = replay.make_cpu()
    # The production clock: the bench measures with the TIMER driver,
    # which the CTC drives in the replay.
    Ctc().attach(z)
    ranges, program = build_ranges()
    spans, parsed, reading_cost = run_profile(replay, z, ranges, program)
    return 0 if report(spans, parsed, reading_cost) else 1


if __name__ == "__main__":
    sys.exit(main())
