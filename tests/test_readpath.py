#!/usr/bin/env python3
"""Read-path measurement acceptance: the phase instrument and its program.

The measurement build (`make phase-image`, `KOS_IOC_PHASES=1`) records the
timer clock at each step of one `/fs` block read into a table in the two
drivers' data, and `tests/perf/readphase.c` reads that table back through
the option-only `KFILE_OP_PHASES` and prints per-phase min/mean/max.  This
module holds the outcome checks:

* the instrument is off in every shipped build: the make variable is empty,
  no system file names the option, the phase source in the kernel and the
  drivers is inside its `#if`, and only the `phase-image` recipe passes the
  flag (the assembly's own switch is the generated `ioc_phase_build.inc`
  the same variable writes);
* the ABI table is the documented shape (`KFILE_OP_PHASES`, the ten phase
  ids in stamp order, a 24-byte packed table);
* the card program's own data stays clear of the aperture address its
  second pass reads into;
* the instrumented image, replayed with a card, runs `readph`, and the
  report has every phase row the semantics require - a row's sample count
  is the number of reads whose previous step was stamped, so the
  cache-hit pass leaves the transport rows at one per block - the sum
  check holds (no mismatch, no torn pair, the worst deviation inside the
  program's four-count allowance), and the nucleus's own copy-path
  counters moved the way the two buffer passes say they must (those
  counters witness the bytes of a read; the test uses them as a
  cross-check of the path claim, not as the measurement);
* the same program in the production image gets KOS_EINVAL from the fetch
  and says so, which is the run-time form of "the op is absent there".

Skipped when the instrumented image or the card program has not been built
(`make phase-image` builds both; the production replay needs `make`).
"""

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from replay import SHELL_PROMPT, STEP_LIMIT, ConsoleReplay  # noqa: E402
from z80sim import Ctc, IocCommandPeer  # noqa: E402

MAKEFILE = os.path.join(ROOT, "Makefile")
PROTOCOL_H = os.path.join(ROOT, "abi", "kos", "protocol.h")
PHASE_HEADER = os.path.join(ROOT, "drivers", "include", "ioc_phases.h")
PHASE_CLOCK_HEADER = os.path.join(ROOT, "drivers", "include",
                                  "ioc_phases_clock.h")
IOC_ASM = os.path.join(ROOT, "drivers", "ioc", "ioc_io.asm")
DRIVER_SOURCES = ("drivers/ioc/ioc.c", "drivers/ioc/ioc_boot.c",
                  "drivers/iocfat/iocfat.c", "kernel/vfs/fs.c")

PHASE_SUBDIR = "build-phase"
PHASE_IMAGE = "konaos.bin"
PRODUCTION_IMAGE = os.path.join(ROOT, "build", PHASE_IMAGE)
PROGRAM = os.path.join(ROOT, "build", "user", "readphase.exe")
PROGRAM_MAP = os.path.join(ROOT, "build", "user", "readphase",
                           "readphase.map")

# The card's names: the medium is FAT 8.3, so the program is truncated to
# what the owner carries, and one data file feeds every pass.
PROGRAM_CARD_NAME = "READPH.EXE"
DATA_NAME = "BIG.BIN"
DATA_BYTES = 8192

# One command run: three passes over two 512-byte blocks, so pass 3's
# cache behaviour is exactly one miss and three hits per block.
BLOCKS = 2
READS = (BLOCKS, BLOCKS, BLOCKS * 4)

# The nucleus's per-outcome copy counters (kernel/ipc/ipc.c), read by
# symbol from the instrumented map: B for the two same-bank passes, C for
# the aperture-range one.  Console traffic lands in the fast counters too,
# so only lower bounds are asserted.
PATH_COUNTERS = {"B": "_ipc_copy_aperture", "C": "_ipc_copy_slow"}

# The phases the bulk receive stamps inline (KFILE_PHASE_* ids, pinned to
# abi/kos/protocol.h below) and the byte offset of the table's second
# elapsed array (KFILE_PHASE_COUNT, the same header).
RECEIVE_PHASES = (4, 5, 6)          # admit, first, last
PHASE_STRIDE = 10

# What one stamp inside the masked bulk receive may cost, in T-states, and
# what the assembly's stamp costs.  The controller clocks the lane at one
# byte per BULK_BYTE_TSTATES (the simulator's model of the firmware's
# pacing) and the SIO holds three bytes, so a stamp that costs a byte
# time is already a third of the cushion; the stamp itself is the ten
# instructions below - two raw counter reads moved through H and L, two
# negations, two absolute stores (in 11, ld r,r 4, neg 8, ld (nn),a 13).
BYTE_T = IocCommandPeer.BULK_BYTE_TSTATES
RECEIVE_STAMP_BUDGET_TSTATES = 2 * BYTE_T

# The whole service gap a receive stamp is allowed, in T-states: the
# receiver's three bytes at the lane's byte time.  The stamp is only part
# of it - the code the driver runs next is the rest - which is why the
# replay measures the gap from the wire rather than trusting the count
# above.
RECEIVE_GAP_BUDGET_TSTATES = 3 * BYTE_T
RECEIVE_STAMP_INSTRUCTION_T = (11, 4, 11, 4, 4, 8, 13, 4, 8, 13)

# The ports the receive's window is found by (platform/zephyr80/public/
# hardware.h): the bulk lane's data and control, the two CTC counter
# ports the stamps read, and the WR5 data the driver writes to raise RTS
# at the head of the masked receive and to drop it again at its end
# (KOS_IOC_WR5_BULK_RTS_ON, then the IDLE value the release helper
# writes).  Those two writes are what separate the receive's own raw
# stamps from the retrying stamp the phases outside the receive use.
SIO1A_DATA = 0x30
SIO1A_CTRL = 0x31
CTC1_PORT = 0x42
CTC2_PORT = 0x41
WR5_BULK_RTS_ON = 0xEB
WR5_BULK_IDLE = 0xE0

PASS_RE = re.compile(
    r"^pass\s+(\d):\s+(\d+)-byte reads into ([0-9a-fA-F]{4})h,\s+path ([ABC]),"
    r"\s+(\d+) reads,\s+(\d+) misses,\s+(\d+) hits$")
SUM_RE = re.compile(
    r"^sum\s+(\d+) of (\d+) fully stamped,\s+(\d+) mismatch,"
    r"\s+(\d+) torn,\s+worst (\d+) counts$")
PHASES = ("entered", "cache", "sent", "reply", "admit",
          "first", "last", "copy", "caller", "return")


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def strip_phase_blocks(text):
    """`text` without its `#if KOS_IOC_PHASES` blocks: what a shipped
    build's compiler sees."""
    out = []
    depth = 0
    for line in text.splitlines():
        stripped = line.strip()
        if stripped == "#if KOS_IOC_PHASES":
            depth += 1
            continue
        if depth and stripped == "#endif":
            depth -= 1
            continue
        if depth == 0:
            out.append(line)
    return "\n".join(out)


def strip_comments(text):
    """`text` without its comment lines: the tokens a check is about are
    the code's, and a file note may name the file it sits next to."""
    return "\n".join(line for line in text.splitlines()
                     if not line.lstrip().startswith(("/*", "*", "*/")))


def parse_report(trace):
    """The program's report as {pass number: {"line": match, "phases":
    {name: (samples, [min, mean, max] or None)}, "sum": match}}."""
    passes = {}
    current = None
    for line in trace.splitlines():
        line = line.strip()
        match = PASS_RE.match(line)
        if match:
            current = {"line": match, "phases": {}, "sum": None}
            passes[int(match.group(1))] = current
            continue
        if current is None or not line:
            continue
        fields = line.split()
        if fields[0] == "sum":
            match = SUM_RE.match(line)
            assert match, "unparsable sum line %r" % line
            current["sum"] = match
        elif fields[0] in PHASES or fields[0] == "timer":
            assert len(fields) in (3, 5), "unparsable row %r" % line
            samples = int(fields[1])
            values = [float(value) for value in fields[2:]] \
                if len(fields) == 5 else None
            current["phases"][fields[0]] = (samples, values)
    return passes


class TestPhaseBuildWiring(unittest.TestCase):
    """The option is off everywhere but the measurement image."""

    def test_the_flag_defaults_to_empty_and_reaches_every_audience(self):
        text = read(MAKEFILE)
        self.assertIn("KOS_PHASE_FLAGS ?=", text)
        self.assertIn("CFLAGS    += $(KOS_PHASE_FLAGS)", text)
        self.assertIn("DRV_CFLAGS += $(KOS_PHASE_FLAGS)", text)

    def test_only_the_phase_image_passes_the_flag(self):
        text = read(MAKEFILE)
        # The variable is set once, in the phase-image recipe, to the
        # option's own define.
        self.assertEqual(text.count("KOS_PHASE_FLAGS="), 1)
        recipe = text.split("phase-image:", 1)[1].split("\n\n", 1)[0]
        self.assertIn("KOS_PHASE_FLAGS=-DKOS_IOC_PHASES=1", recipe)
        for name in sorted(os.listdir(os.path.join(ROOT, "systems"))):
            if name.endswith(".json"):
                self.assertNotIn(
                    "KOS_IOC_PHASES",
                    read(os.path.join(ROOT, "systems", name)), name)

    def test_the_assembly_switch_is_generated_from_the_same_variable(self):
        makefile = read(MAKEFILE)
        self.assertIn("$(BUILD)/ioc_phase_build.inc:", makefile)
        self.assertIn("$(if $(KOS_PHASE_FLAGS),1,0)", makefile)
        asm = read(IOC_ASM)
        self.assertIn('.include "ioc_phase_build.inc"', asm)
        # The declarations block and the three receive stamps.
        self.assertEqual(asm.count(".if ioc_phase_build"), 4)
        self.assertNotIn(".ifdef", asm)
        for phase in RECEIVE_PHASES:
            self.assertIn("ld\t(_ioc_phases + %d),a\n" % phase, asm)
            self.assertIn("ld\t(_ioc_phases + %d + %d),a\n"
                          % (PHASE_STRIDE, phase), asm)

    def test_the_receive_stamps_are_raw_and_inside_the_fifo_budget(self):
        """The board's finding, as a source rule.

        A stamp taken while the bulk receive is masked must be two raw
        counter reads and their stores - no call, no loop, no retry, no
        parity test - because the controller-clocked receiver's three-byte
        FIFO is the whole budget.  The retaking stamp the phases outside
        the receive use costs about 1000 T-states; three of those inside
        one transfer overran the receiver on the board (every bulk
        transfer failed with EIO).
        """
        asm = read(IOC_ASM)
        blocks = []
        lines = asm.splitlines()
        for index, line in enumerate(lines):
            if line.strip() == ".if ioc_phase_build":
                end = index
                while lines[end].strip() != ".endif":
                    end += 1
                blocks.append(lines[index + 1:end])
        # The declarations block plus one per receive phase.
        self.assertEqual(len(blocks), 1 + len(RECEIVE_PHASES))
        self.assertIn(".globl _ioc_phases", "\n".join(blocks[0]))
        for phase, block in zip(RECEIVE_PHASES, blocks[1:]):
            code = [line.strip() for line in block
                    if line.strip() and not line.strip().startswith(";")]
            self.assertEqual(code, [
                "in\ta,(KOS_CTC1_PORT)",
                "ld\th,a",
                "in\ta,(KOS_CTC2_PORT)",
                "ld\tl,a",
                "ld\ta,h",
                "neg",
                "ld\t(_ioc_phases + %d),a" % phase,
                "ld\ta,l",
                "neg",
                "ld\t(_ioc_phases + %d + %d),a" % (PHASE_STRIDE, phase),
            ], block)
            self.assertEqual(len(code), len(RECEIVE_STAMP_INSTRUCTION_T),
                             block)
            cost = sum(RECEIVE_STAMP_INSTRUCTION_T)
            self.assertLessEqual(cost, RECEIVE_STAMP_BUDGET_TSTATES,
                                 "phase %d costs %d T-states" % (phase, cost))
        # Every stamp in the receive is one of those blocks: no call, no
        # branch, nothing the FIFO would have to wait for.
        for block in blocks[1:]:
            for forbidden in ("call", "jr", "jp", "djnz", "loop"):
                self.assertNotIn(forbidden, block)

    def test_the_phase_source_is_option_only(self):
        for name, tokens in (
                ("drivers/ioc/ioc.c", ("ioc_phases_stamp",
                                       "ioc_phases_bulk_done",
                                       "KFILE_PHASE_SENT")),
                ("drivers/ioc/ioc_boot.c", ("ioc_phases_ctc1_init",)),
                ("drivers/iocfat/iocfat.c", ("iocfat_phases",
                                             "iocfat_phase_note",
                                             "iocfat_phase_begin",
                                             "iocfat_phase_peer",
                                             "KFILE_OP_PHASES")),
                ("kernel/vfs/fs.c", ("KFILE_OP_PHASES",))):
            text = read(os.path.join(ROOT, name))
            stripped = strip_phase_blocks(text)
            for token in tokens:
                self.assertIn(token, text, name)
                self.assertNotIn(token, stripped,
                                 "%s keeps %s outside its guard"
                                 % (name, token))
        # The shared headers carry nothing a build without the option
        # would compile: the clock, which the boot segment starts, and the
        # stamps, which the two hot segments take.
        for path in (PHASE_HEADER, PHASE_CLOCK_HEADER):
            header = strip_comments(strip_phase_blocks(read(path)))
            for token in ("__sfr", "static void", "KFILE_PHASE",
                          "ioc_phases_stamp", "ioc_phases_clear",
                          "ioc_phases_ctc1_init", "IOC_PHASES_CTC1"):
                self.assertNotIn(token, header, path)
        # The boot segment wants the clock alone: the stamping half is the
        # hot segments' include, and a boot segment that took it would
        # carry the instrument's reference byte for nothing.
        boot = read(os.path.join(ROOT, "drivers/ioc/ioc_boot.c"))
        self.assertIn('#include "ioc_phases_clock.h"', boot)
        self.assertNotIn('#include "ioc_phases.h"', boot)

    def test_every_writer_of_the_table_is_inside_the_guard(self):
        for name, token in (("drivers/ioc/ioc.c", "ioc_phases_stamp"),
                            ("drivers/ioc/ioc_boot.c", "ioc_phases_ctc1"),
                            ("drivers/iocfat/iocfat.c", "iocfat_phase_note"),
                            ("drivers/ioc/ioc_proto.c", "payload[5]")):
            text = read(os.path.join(ROOT, name))
            self.assertIn(token, text, name)
            self.assertNotIn(token, strip_phase_blocks(text), name)

    def test_the_kernel_forwards_the_op_inside_its_guard(self):
        text = read(os.path.join(ROOT, "kernel", "vfs", "fs.c"))
        guarded = text.split("#if KOS_IOC_PHASES", 1)[1].split("#endif", 1)[0]
        self.assertIn("KFILE_OP_PHASES", guarded)
        stripped = strip_phase_blocks(text)
        self.assertIn("if (args->op <= KFILE_OP_SYNC", stripped)
        self.assertNotIn("KFILE_OP_PHASES", stripped)


class TestPhaseAbi(unittest.TestCase):
    """The table and its operation as the ABI documents them."""

    def test_the_operation_number_and_the_table_shape(self):
        text = read(PROTOCOL_H)
        self.assertIn("#define KFILE_OP_PHASES 5u", text)
        self.assertIn("#define KFILE_PHASE_COUNT   10u", text)
        body = text.split("struct kfile_phases {", 1)[1].split("};", 1)[0]
        self.assertIn("uint8_t elapsed_1[KFILE_PHASE_COUNT];", body)
        self.assertIn("uint8_t elapsed_2[KFILE_PHASE_COUNT];", body)
        self.assertIn("uint16_t valid;", body)
        self.assertIn("uint8_t flags;", body)
        self.assertIn("uint8_t path;", body)
        # Two ten-byte arrays, a mask and two flag bytes: exactly the
        # frame payload the packed table travels in.
        payload = re.search(r"#define KOS_IPC_PAYLOAD_SIZE (\d+)u",
                            read(os.path.join(ROOT, "abi", "kos",
                                              "ipc.h")))
        self.assertEqual(2 * 10 + 2 + 1 + 1, int(payload.group(1)))

    def test_the_phase_ids_are_the_stamp_order(self):
        text = read(PROTOCOL_H)
        for index, name in enumerate(PHASES):
            self.assertTrue(
                re.search(r"#define KFILE_PHASE_%s\s+%du"
                          % (name.upper(), index), text), name)

    def test_the_receive_stamp_offsets_are_the_abis(self):
        """What ioc_io.asm writes by hand is what the header defines.

        The receive's stamps are inline assembly with literal offsets
        (the assembler cannot read a C header), so the header's ids and
        the table's stride are what those literals mean.
        """
        text = read(PROTOCOL_H)
        for name, phase in zip(("admit", "first", "last"), RECEIVE_PHASES):
            self.assertTrue(
                re.search(r"#define KFILE_PHASE_%s\s+%du"
                          % (name.upper(), phase), text), name)
        self.assertTrue(
            re.search(r"#define KFILE_PHASE_COUNT\s+%du" % PHASE_STRIDE,
                      text))
        self.assertIn("uint8_t elapsed_1[KFILE_PHASE_COUNT];", text)
        self.assertIn("uint8_t elapsed_2[KFILE_PHASE_COUNT];", text)


class TestReadphaseProgram(unittest.TestCase):
    """The program's own placement safety."""

    def setUp(self):
        if not os.path.exists(PROGRAM_MAP):
            self.skipTest("build the card program first (make phase-image)")

    def test_the_card_name_is_8_3(self):
        base, _, extension = PROGRAM_CARD_NAME.partition(".")
        self.assertLessEqual(len(base), 8, PROGRAM_CARD_NAME)
        self.assertLessEqual(len(extension), 3, PROGRAM_CARD_NAME)

    def test_the_program_data_stays_clear_of_the_aperture_pass(self):
        areas = re.findall(r"^(\S+)\s+([0-9A-F]{8})\s+([0-9A-F]{8}) =",
                           read(PROGRAM_MAP), re.M)
        self.assertTrue(areas, "the map has no areas")
        for name, base, size in areas:
            end = int(base, 16) + int(size, 16)
            self.assertLessEqual(end, 0x7C00,
                                 "%s ends at %04X, inside the aperture "
                                 "range the second pass reads into"
                                 % (name, end))


class ReadphaseReplay(ConsoleReplay, unittest.TestCase):
    """Both images, with the card carrying the program and its file."""

    def start(self, image_subdir, image_name, extra_files=None):
        self.load_image(image_subdir, image_name)
        z = self.make_cpu()
        Ctc().attach(z)
        # Every port read, so the receive stamps' budget can be measured
        # from the wire's point of view (receive_stamp_gaps), and every
        # port write, so the receive windows those stamps live in are
        # found by the RTS register writes rather than guessed.
        z.port_reads = []
        z.port_writes = []
        inner_read = z.io_in
        inner_write = z.io_out

        def io_in(port):
            value = inner_read(port)
            z.port_reads.append((z.tstates, port & 0xFF))
            return value

        def io_out(port, value):
            z.port_writes.append((z.tstates, port & 0xFF, value & 0xFF))
            inner_write(port, value)

        z.io_in = io_in
        z.io_out = io_out
        with open(PROGRAM, "rb") as handle:
            program = handle.read()
        data = bytes((index * 7 + 3) & 0xFF for index in range(DATA_BYTES))
        files = {PROGRAM_CARD_NAME: program, DATA_NAME: data}
        files.update(extra_files or {})
        z.hid = z.ioc_peer = IocCommandPeer(fs2_files=files)
        self.boot_to_shell(z, prompt=SHELL_PROMPT)
        return z

    def type_line(self, z, line, expect, limit=STEP_LIMIT, since=0):
        z.hid.hid_queue.extend(line.encode("ascii") + b"\r")
        self.run_to(z, expect, limit, since=since)
        self.pump(z)
        return "".join(z.glyphs)[since:]

    def counters(self, z):
        return {letter: self.read16(z, symbol)
                for letter, symbol in PATH_COUNTERS.items()}

    def receive_windows(self, z):
        """The masked bulk receives, as (open, close) T-state pairs.

        A receive is opened by the driver's own write of the bulk-lane
        WR5 data that raises RTS and closed by the write that drops it
        again, so what counts as inside the receive is the driver's
        definition and not a guess from how close two port accesses are
        in time."""
        windows = []
        opened = None
        for when, port, value in z.port_writes:
            if port != SIO1A_CTRL:
                continue
            if value == WR5_BULK_RTS_ON and opened is None:
                opened = when
            elif value == WR5_BULK_IDLE and opened is not None:
                windows.append((opened, when))
                opened = None
        return windows

    def receive_stamp_gaps(self, z):
        """Inside each receive, the raw stamps and the gaps they cost.

        A receive stamp is two counter reads (channel 1 then channel 2)
        with nothing between them, taken while the bulk receive is
        masked; the retaking stamp the phases outside the receive use
        lives outside these windows entirely, so no read of its shows up
        here.  The gap measured is from the stamp's first counter read to
        the next bulk-lane port access - the time the receiver's FIFO has
        to cover on its own."""
        reads = z.port_reads
        windows = []
        for opened, closed in self.receive_windows(z):
            gaps = []
            for index, (started, port) in enumerate(reads):
                if port != CTC1_PORT or not opened <= started <= closed:
                    continue
                if index + 1 >= len(reads) or \
                        reads[index + 1][1] != CTC2_PORT:
                    continue
                if not opened <= reads[index + 1][0] <= closed:
                    continue
                for later in range(index + 2, len(reads)):
                    if reads[later][1] in (SIO1A_DATA, SIO1A_CTRL):
                        gaps.append(reads[later][0] - started)
                        break
            windows.append(gaps)
        return windows


class TestInstrumentedReadPath(ReadphaseReplay):
    """The measurement image answers the fetch and stamps every step."""

    def setUp(self):
        if not os.path.exists(PROGRAM):
            self.skipTest("build the card program first (make phase-image)")
        if not os.path.exists(os.path.join(ROOT, PHASE_SUBDIR, PHASE_IMAGE)):
            self.skipTest("build the instrumented image first "
                          "(make phase-image)")

    def test_the_report_covers_every_phase_step(self):
        z = self.start(PHASE_SUBDIR, PHASE_IMAGE)
        before = self.counters(z)
        since = len(z.glyphs)
        trace = self.type_line(z, "readph %s %d" % (DATA_NAME, BLOCKS),
                               "readphase: end", since=since)
        after = self.counters(z)
        self.assertIn("readphase: begin", trace)
        passes = parse_report(trace)
        self.assertEqual(sorted(passes), [1, 2, 3], trace)

        # Every pass read the file from the start into the buffer its
        # copy-path claim needs.
        for number, size, path, reads in ((1, 512, "B", READS[0]),
                                          (2, 512, "C", READS[1]),
                                          (3, 128, "B", READS[2])):
            line = passes[number]["line"]
            self.assertEqual(int(line.group(2)), size, line.group(0))
            self.assertEqual(line.group(4), path, line.group(0))
            self.assertEqual(int(line.group(5)), reads, line.group(0))

        # The cache answers: 512-byte reads always need the next block,
        # 128-byte reads need one block in four.
        self.assertEqual((int(passes[1]["line"].group(6)),
                          int(passes[1]["line"].group(7))), (READS[0], 0))
        self.assertEqual((int(passes[2]["line"].group(6)),
                          int(passes[2]["line"].group(7))), (READS[1], 0))
        self.assertEqual((int(passes[3]["line"].group(6)),
                          int(passes[3]["line"].group(7))),
                         (BLOCKS, READS[2] - BLOCKS))

        # A row's sample count is the number of reads whose previous step
        # was stamped: the miss-only passes fill every row, and the
        # cache-hit pass keeps the transport rows at one per block.
        miss_rows = {phase: READS[0] for phase in PHASES}
        miss_rows["entered"] = 0
        hit_rows = {"entered": 0, "cache": READS[2],
                    "sent": BLOCKS, "reply": BLOCKS, "admit": BLOCKS,
                    "first": BLOCKS, "last": BLOCKS, "copy": BLOCKS,
                    "caller": READS[2], "return": READS[2]}
        for number, expected in ((1, miss_rows), (2, miss_rows),
                                 (3, hit_rows)):
            torn = int(passes[number]["sum"].group(4))
            samples = {phase: row[0] for phase, row
                       in passes[number]["phases"].items()
                       if phase != "timer"}
            # A torn stamp takes its two differences out of their rows, so
            # a row's count is the stamped reads it expects, less the torn
            # ones that touched it.
            for phase, count in samples.items():
                self.assertLessEqual(count, expected[phase],
                                     "pass %d %s" % (number, phase))
                self.assertGreaterEqual(
                    count, expected[phase] - 2 * torn,
                    "pass %d %s" % (number, phase))
            self.assertEqual(set(samples), set(expected))
            self.assertEqual(passes[number]["phases"]["timer"][0],
                             READS[number - 1], "pass %d timer" % number)

        # min <= mean <= max, real work shows real time, and no row can
        # exceed the resolution window: a torn or aliased stamp would
        # break one of these, and the sum check below proves the rest.
        for number in (1, 2, 3):
            for phase, (samples, values) in passes[number]["phases"].items():
                if samples == 0:
                    self.assertIsNone(values, phase)
                    continue
                self.assertIsNotNone(values, phase)
                low, mean, high = values
                self.assertLessEqual(low, mean, phase)
                self.assertLessEqual(mean, high, phase)
                self.assertLess(high, 819.0, phase)
                if phase in ("last", "copy", "caller"):
                    self.assertGreater(high, 0.0, phase)

        # The sum check: every fully stamped block's ten differences add
        # up to its ENTERED-to-RETURN difference, exactly or to within the
        # allowance the program names.  The receive's raw stamps can catch
        # one channel stepping between their two reads; the program
        # resolves such a pair to its nearest candidate, and this check is
        # what verifies it.  A pair it cannot resolve is reported as torn
        # and its block left out, so a stamped block is either verified or
        # torn and never wrongly answered: mismatch stays zero.
        for number, stamped in ((1, READS[0]), (2, READS[1]),
                                (3, BLOCKS)):
            match = passes[number]["sum"]
            full, total, mismatch, torn, worst = (int(group)
                                                  for group in match.groups())
            self.assertEqual(total, READS[number - 1], match.group(0))
            self.assertEqual(full + torn, stamped, match.group(0))
            self.assertEqual(mismatch, 0, match.group(0))
            self.assertLessEqual(worst, 4, match.group(0))
            self.assertLessEqual(torn, stamped // 2, match.group(0))

        # The nucleus's own counters: the two same-bank passes copied one
        # descriptor per read, and the aperture-range pass one per read
        # through the staged gate.  Nothing else can make these lower
        # bounds hold.
        self.assertGreaterEqual((after["B"] - before["B"]) % 0x10000,
                                5 * BLOCKS)
        self.assertGreaterEqual((after["C"] - before["C"]) % 0x10000, BLOCKS)

        # The receiver's budget: every masked receive in the replay -
        # the boot's file loads as much as the program's own reads -
        # takes three stamps, one per phase the receive measures, and
        # none of them may cost the receiver more than the FIFO covers
        # while it is away from the wire.
        windows = self.receive_stamp_gaps(z)
        misses = READS[0] + READS[1] + BLOCKS
        self.assertGreaterEqual(len(windows), misses, windows)
        self.assertEqual([len(gaps) for gaps in windows],
                         [len(RECEIVE_PHASES)] * len(windows), windows)
        gaps = [gap for window in windows for gap in window]
        self.assertLessEqual(max(gaps), RECEIVE_GAP_BUDGET_TSTATES, gaps)
        print("readpath: %d masked receives, %d stamps, worst service gap "
              "%d T-states (stamp %d), FIFO budget %d"
              % (len(windows), len(gaps), max(gaps),
                 sum(RECEIVE_STAMP_INSTRUCTION_T),
                 RECEIVE_GAP_BUDGET_TSTATES))

        # The wire's own check: the modelled controller-clocked receiver
        # never overran its three-byte FIFO, which is what broke every
        # bulk transfer on the board before this was fixed.
        self.assertEqual(z.ioc_peer.a_overruns, 0,
                         "bulk receive FIFO overruns")

        # The shell came back with no failed-program line: the program
        # exited zero.  The snapshot the report came from is a character
        # string while the replay's stream is a list of glyphs, so the
        # prompt is looked for in the stream - which may already hold it,
        # the program exits as soon as it has printed its last line - and
        # the check is about what follows the report's own end.
        stream = "".join(z.glyphs)[since:]
        if SHELL_PROMPT not in stream:
            self.run_to(z, SHELL_PROMPT, since=len(z.glyphs))
            stream = "".join(z.glyphs)[since:]
        tail = stream.split("readphase: end", 1)[-1]
        self.assertIn(SHELL_PROMPT, tail)
        self.assertNotIn("readphase: pass", tail)
        self.assertNotIn("boot: ", tail)
        print("readphase: report\n%s" % trace)


class TestProductionRefusesTheFetch(ReadphaseReplay):
    """Without the option the operation is absent, at run time."""

    def setUp(self):
        if not os.path.exists(PROGRAM):
            self.skipTest("build the card program first (make phase-image)")
        if not os.path.exists(PRODUCTION_IMAGE):
            self.skipTest("build the production image first (make)")

    def test_the_fetch_answers_nothing_and_the_program_says_so(self):
        z = self.start("build", PHASE_IMAGE)
        trace = self.type_line(z, "readph %s 1" % DATA_NAME,
                               "readphase: end")
        self.assertIn("readphase: no phase table", trace)
        self.assertNotIn("readphase: pass 1 failed", trace)
        # The refused fetch stopped the first pass before it could fold a
        # single read, so the report holds no pass at all.
        self.assertEqual(parse_report(trace), {}, trace)


class TestTheCacheServesAReread(ReadphaseReplay):
    """The block cache, seen from the card: a file read twice reaches the
    card once, and a change made through the driver sends the next read
    back to the card."""

    NOTE = "NOTE.TXT"
    # Three blocks of printable text, the last one short.
    TEXT = "".join("line %04d of the cached note\r\n" % index
                   for index in range(48))

    def setUp(self):
        if not os.path.exists(PRODUCTION_IMAGE):
            self.skipTest("build the production image first (make)")

    def card_reads(self, z):
        return sum(1 for request in z.ioc_peer.requests
                   if request[0] == IocCommandPeer.CMD_FS2_READ)

    def cat(self, z, mark):
        """The note's lines as `cat` printed them, and the card reads it
        cost.  A unique `echo` after it marks the end, so the lines are
        taken from between the two commands in the whole stream."""
        before = self.card_reads(z)
        z.hid.hid_queue.extend(("cat %s\r" % self.NOTE).encode("ascii"))
        self.type_line(z, "echo %s" % mark, mark + "\n")
        reads = self.card_reads(z) - before  # echo never reads the card
        stream = "".join(z.glyphs)
        end = stream.rindex("echo %s" % mark)
        start = stream.rindex("cat %s" % self.NOTE, 0, end)
        lines = re.findall(r"line \d{4} of the cached note",
                           stream[start:end])
        return lines, reads

    def test_a_reread_stays_off_the_card_until_a_write(self):
        z = self.start("build", PHASE_IMAGE,
                       {self.NOTE: self.TEXT.encode("ascii")})
        expected = re.findall(r"line \d{4} of the cached note", self.TEXT)
        first, reads = self.cat(z, "MARK1")
        self.assertEqual(first, expected)
        self.assertGreater(reads, 0)
        second, reads = self.cat(z, "MARK2")
        self.assertEqual(reads, 0, "the second read reached the card")
        self.assertEqual(second, expected)
        # A copy opens its target for writing, which drops the cache.
        self.type_line(z, "cp %s COPY.TXT" % self.NOTE, SHELL_PROMPT)
        third, reads = self.cat(z, "MARK3")
        self.assertGreater(reads, 0, "a read after a write used the cache")
        self.assertEqual(third, expected)


if __name__ == "__main__":  # pragma: no cover - manual smoke check
    unittest.main()
