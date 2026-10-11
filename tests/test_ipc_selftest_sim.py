#!/usr/bin/env python3
"""IPC substrate acceptance replay (work packages 39 and 42d).

Boots build-ipc-selftest/zephyros-ipc-selftest.bin -- the production boot
path with the fixed-core IPC substrate linked in, the six synthetic
resident services placed in bank 6 by the cold loader, and the SDK client
program booting as /SYSTEM/ipc.zex -- once, and asserts the outcomes of
that one run:

* the promoted package-35 conformance suite runs from the client and
  reports PASS (>= 49 checks), and the caller-buffer probe that boots the
  client reports PASS for its twelve-case matrix (correction 01);
* every phase of both programs marks its boundary on the wire in a/b/c
  order: the lower-case codes are the client's measurement phases, the
  upper-case ones the probe's copy cases;
* the copy-path counters the resolver keeps for exactly this purpose each
  moved: the window path, the aperture path and the chunked slow path all
  carried bytes;
* the reserved stacks (service 5D00h-5F00h, ISR 5F00h-6000h, gate stack)
  stay inside their regions: the replay watches SP and records each
  high-water mark;
* the probe's service-side slow case put its 16-byte pattern on the
  console wire;
* the loader's placement report matches the packer's plan, each service
  boot segment ran exactly once, and `con`'s slow buffer sits in the hot
  tier;
* the run ends with the caller's own mapping restored (mode 10, page bits
  clear), so the copy gate's aperture window was closed by the code that
  opened it.

Work package 42d: this replay used to carry a per-instruction tracer
(T-state spans, the latch-write save/restore tree, LDIR and syscall
censuses, injection framing) and ran for about 17 minutes.  Every path it
attributed from the outside is checked from the inside by the two
programs the image runs - the client and the probe each verify the paths
they take and print one verdict - so the replay now asserts those
verdicts, the phase markers, the resolver's own counters and the machine
state the run leaves behind; it boots the image once and finishes in
seconds.  The per-case T-state baselines and the latch/IRQ analyses live
in the package-42b and package-42e records; regenerate them with a tracer
if the copy paths themselves change.

Skipped when the image has not been built.
"""

import json
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from replay import RET, STEP_LIMIT, Replay  # noqa: E402

IMAGE_SUBDIR = "build-ipc-selftest"
IMAGE_NAME = "zephyros-ipc-selftest.bin"

PASS = b"ipc: PASS\r\n"
CLIENT_START = b"ipc selftest: client OK\r\n"
FAIL = b"ipc: FAIL\r\n"
# The caller-buffer probe's own verdict, printed after the client exits.
APERTURE_PASS = b"ipccopy: PASS\r\n"
APERTURE_FAIL = b"ipccopy: FAIL\r\n"
LOADER_OK = b"service loader: 8 images in bank 6\r\n"

MODE_MASK = 0x18
PAGE_MASK = 0xE0
OS_MODE = 0x18
APPLICATION_MODE = 0x10
CTC0_PORT = 0x40
CTC0_CONTROL = 0xA7
CTC0_TIME_CONSTANT = 0x00
CTC0_VECTOR = 0x00
# Correction 01: the irqx copy op arms the same channel on a short time
# constant, so the modelled delivery lands inside the path-B aperture
# window instead of between copies.  The stand-in keys on the constant,
# which is the one writable value that separates the two profiles.
CTC0_APERTURE_TIME_CONSTANT = 0x08

PAGE_ONE = 0x20

# The resident service domain: bank 6 in the mode-11 window, with the hot
# tier (6000h-DFFFh) and the SERVICE_IO blocks (8000h-BFFFh) inside it
# (architecture 36.6, correction 01 to 36.8).
DOMAIN_BASE = 0x6000
DOMAIN_END = 0xE000

# The packed service images, their per-image reports and the plan the
# loader's own report is pinned to (Makefile: IPC_MODDIR).
MODULE_DIR = os.path.join(ROOT, IMAGE_SUBDIR, "modules", "ipc-modules")
SERVICE_DIR = os.path.join(ROOT, IMAGE_SUBDIR, "modules")
SERVICE_ORDER = ["ioc", "fat", "vfs", "echo", "con", "irqx"]
IPC_BOOT_MAGIC = 0xA7

# The client's measurement phases (its one-letter marker codes); 8 warm-up
# calls then 24 measured calls per phase.
PHASES = {
    "n": "null-call",
    "c": "con256-write",
    "o": "console256-direct",
    "r": "vfs-512-direct",
    "s": "vfs-64-offset",
    "t": "vfs-512-staged",
    "u": "vfs-512-repeat",
    "f": "fsread-baseline",
}

# The caller-buffer probe's phases (correction 01): code, case name, the
# caller buffer it names, and the copy path the correction requires.
# "window" is path A (one mode-11 LDIR), "aperture" is path B (one LDIR
# with the mode-10 aperture open), "slow" is path C (the chunked copy
# through the zos_bank_copy gate).
PROBE_PHASES = {
    "A": ("window-read", 0x0100, "window"),
    "P": ("window-single", 0x0100, "window"),
    "B": ("high1-read", 0x3000, "aperture"),
    "Q": ("high1-single", 0x3000, "aperture"),
    "C": ("high2-read", 0x7000, "aperture"),
    "R": ("high2-single", 0x7000, "aperture"),
    "D": ("high3-read", 0xC000, "aperture"),
    "S": ("high3-single", 0xC000, "aperture"),
    "E": ("behind-read", 0x9000, "slow"),
    "T": ("behind-single", 0x9000, "slow"),
    "U": ("service-slow", 0x0100, "slow"),
    "V": ("irq-aperture", 0x3000, "aperture"),
}

# The probe's console pattern (tests/target/ipc/ipccopy.c).
PROBE_PATTERN_BASE = 0x80
PROBE_SLOW_BYTES = 16

# Counter symbols the probe's cases are attributed by (kernel/ipc/ipc.c,
# linked with the compiler's underscore): one per resolver outcome, so the
# attribution survives as an outcome even though the replay no longer
# follows every instruction.
COPY_COUNTERS = ("_ipc_copy_window", "_ipc_copy_aperture", "_ipc_copy_slow")

COUNT_RE = re.compile(rb"ipc: (\d+)/(\d+) checks\r\n")
# The client marks its phases with lower-case codes; the caller-buffer
# probe (correction 01) marks its cases with upper-case ones, so one
# expression reads both transcripts.
MARK_RE = re.compile(rb"ipc: ([A-Za-z])([abc])\r\n")

# The reserved stacks, named (base, top) from the linked map.
STACK_NAMES = ("service", "isr", "gate")


class IpcCtc0:
    """Host stand-in for CTC0 (the simulator models no timer).

    The kernel programs the channel through port 40h; the stand-in
    delivers vector 00h where each profile expects it.  The standard
    period lands while the service domain is resident (the delivery an
    in-progress call must survive); correction 01's short period lands
    inside the next mode-10 aperture window the copy gate opens, which is
    the interrupted-copy case.  The deliveries are inputs - the client
    and the probe assert their effects - so they stay in the replay.
    """

    def __init__(self, syms):
        self.ape_base = syms["ipc_common_begin"]
        self.ape_end = syms["ipc_common_end"]
        self.control = False
        self.armed = False
        self.armed_aperture = False
        self.injections = 0

    def install(self, z):
        saved = z.io_out

        def io_out(port, value):
            if (port & 0xFF) == CTC0_PORT:
                self.write(value & 0xFF)
            return saved(port, value)

        z.io_out = io_out

    def write(self, value):
        """The kernel's CTC0 helper: vector base, control word, constant.

        The channel starts on the write that follows the control word, so
        the stand-in arms on that one write only: the vector base and the
        time constant share a value on this platform, and only the control
        word separates a fresh arming from an address write."""
        if value == CTC0_CONTROL:
            self.control = True
            return
        if not self.control:
            return
        self.control = False
        if value == CTC0_APERTURE_TIME_CONSTANT:
            self.armed_aperture = True
        elif value == CTC0_TIME_CONSTANT:
            self.armed = True

    def __call__(self, z):
        if not z.iff1 or z.int_pending:
            return
        if (self.armed_aperture
                and (z.latch & MODE_MASK) == APPLICATION_MODE
                and (z.latch & PAGE_MASK) == PAGE_ONE
                and self.ape_base <= z.pc < self.ape_end):
            z.assert_int(CTC0_VECTOR)
            self.armed_aperture = False
            self.injections += 1
        elif (self.armed and (z.latch & PAGE_MASK)
                and DOMAIN_BASE <= z.pc < DOMAIN_END):
            z.assert_int(CTC0_VECTOR)
            self.armed = False
            self.injections += 1


class StackMarks:
    """The high-water mark of each reserved stack: one range test per
    step, without following the rest of the machine."""

    def __init__(self, regions):
        self.regions = tuple(regions)
        self.low = {}

    def __call__(self, z):
        sp = z.sp
        for name, base, top in self.regions:
            if base <= sp <= top:
                low = self.low.get(name)
                if low is None or sp < low:
                    self.low[name] = sp


class IpcSim(Replay, unittest.TestCase):
    IMAGE_SUBDIR = IMAGE_SUBDIR
    IMAGE_NAME = IMAGE_NAME

    def setUp(self):
        self.load_image()

    def reserved_stacks(self):
        # Half-open byte ranges: the service stack ends where the ISR
        # stack starts (5F00h), and a push reaching the region's top is
        # still inside it.
        return (
            ("service", self.syms["zos_service_stack"],
             self.syms["zos_service_stack_top"] - 1),
            ("isr", self.syms["irq_isr_stack"] + 1,
             self.syms["irq_isr_stack_top"] - 1),
            ("gate", self.syms["zos_gate_stack"],
             self.syms["zos_gate_stack_top"] - 1),
        )

    def drive(self, z):
        self.ctc0(z)
        self.stacks(z)

    def boot(self):
        """One boot serves every assertion: the image runs the whole
        surface in a single pass."""
        if getattr(self, "_booted", None) is not None:
            return self._booted
        z = self.make_cpu(latch=OS_MODE)
        self.stacks = StackMarks(self.reserved_stacks())
        self.ctc0 = IpcCtc0(self.syms)
        self.crt0(z)
        z.push16(RET)
        z.pc = self.syms["_kernel_main"]
        self.ctc0.install(z)
        self.input_driver = self.drive
        self.run_until(z, (APERTURE_PASS, APERTURE_FAIL, FAIL),
                       limit=STEP_LIMIT)
        tx = bytes(z.sio_tx)
        if APERTURE_PASS in tx:
            # The probe prints its verdict and then closes the aperture;
            # the run has settled once the caller's own mapping is back,
            # which is what the state assertions read.  A failing image
            # skips the wait and is reported by the verdict tests.
            for _ in range(2000000):
                self.skip_delay(z)
                self.feed(z)
                z.step()
                if ((z.latch & MODE_MASK) == APPLICATION_MODE
                        and (z.latch & PAGE_MASK) == 0):
                    break
            else:
                raise AssertionError(
                    "the caller's mapping never came back (PC=%04X)"
                    % z.pc)
        self._booted = z
        return z

    def transcript(self):
        return bytes(self.boot().sio_tx)

    def report(self, name):
        with open(os.path.join(MODULE_DIR, name + ".report.json"),
                  encoding="utf-8") as handle:
            return json.load(handle)

    def plan(self):
        if getattr(self, "_plan", None) is None:
            path = os.path.join(SERVICE_DIR, "services.plan.json")
            if not os.path.exists(path):
                self.skipTest("the service images have not been packed")
            with open(path, encoding="utf-8") as handle:
                self._plan = json.load(handle)
        return self._plan

    def symbol_address(self, name, symbol):
        """The bank-6 address of a service image symbol: the packer's
        segment-relative offset in the segment the report names."""
        entry = [item for item in self.plan()["services"]
                 if item["name"] == "ipc-%s.zmod" % name][0]
        report = self.report(name)
        role = report["symbol_segments"][symbol]
        base = {"hot": entry["hot_base"], "boot": entry["boot_base"],
                "io": entry["io_base"] or 0,
                "bulk": entry["bulk_base"] or 0}[role]
        return base + report["symbols"][symbol]


class TestIpcAcceptance(IpcSim):
    """The promoted conformance suite passes and its evidence holds."""

    def test_client_passes_the_promoted_suite(self):
        tx = self.transcript()
        for line in (b"ZephyrOS 0.0\r\n", b"platform: Zephyr-80\r\n",
                     b"kernel entry: OK\r\n", b"serial: OK\r\n"):
            self.assertIn(line, tx)
        self.assertIn(LOADER_OK, tx)
        self.assertIn(CLIENT_START, tx)
        self.assertNotIn(FAIL, tx, tx[-400:].decode("latin-1"))
        self.assertNotIn(b"PANIC", tx)
        self.assertIn(PASS, tx)
        match = COUNT_RE.search(tx)
        self.assertIsNotNone(match, tx[-200:].decode("latin-1"))
        passed, total = (int(group) for group in match.groups())
        self.assertGreaterEqual(total, 49,
                                "the suite must carry >= 49 checks")
        self.assertEqual(passed, total)
        print("ipc: %d/%d checks, six services in bank 6" % (passed, total))

    def test_every_phase_boundary_is_marked(self):
        """Each phase of both programs marks its boundary a, b, c on the
        wire, in that order and exactly once each.

        The check doubles as the anti-aliasing guard the tracer used to
        carry: the payload bytes are >= 80h, so a printable marker line
        can only come from the phase code itself."""
        marks = {}
        for code, suffix in MARK_RE.findall(self.transcript()):
            marks.setdefault(code.decode(), []).append(suffix.decode())
        for code in sorted(set(PHASES) | set(PROBE_PHASES)):
            self.assertEqual(marks.get(code), ["a", "b", "c"],
                             "phase %s markers out of order or missing"
                             % code)


class TestIpcOutcomes(IpcSim):
    """The evidence the run itself leaves behind."""

    def test_copy_path_counters_cover_every_path(self):
        """The resolver counts one per outcome (kernel/ipc/ipc.c), so a
        path that never carried a byte reports zero."""
        z = self.boot()
        totals = {name: self.read16(z, name) for name in COPY_COUNTERS}
        for name, count in sorted(totals.items()):
            self.assertGreater(count, 0, "%s never moved" % name)
        print("ipc: copy-path counters %s, %d CTC0 deliveries"
              % (totals, self.ctc0.injections))

    def test_probe_passes_the_caller_buffer_matrix(self):
        tx = self.transcript()
        self.assertIn(APERTURE_PASS, tx)
        self.assertNotIn(APERTURE_FAIL, tx)
        print("ipc: probe verdict %r"
              % [line for line in tx.decode("latin1").split("\r\n")
                 if line.startswith("ipccopy: ")][-1:])

    def test_service_io_rule_counts_the_out_of_range_case_slow(self):
        """The probe's service-side slow case (``U``) writes its pattern
        through the console, so the bytes are on the wire sixteen per
        call: the only case whose slow path comes from the service's own
        buffer sitting outside 8000h-BFFFh."""
        marker = b"".join(bytes([PROBE_PATTERN_BASE + (index & 0x3F)])
                          for index in range(PROBE_SLOW_BYTES))
        self.assertGreaterEqual(self.transcript().count(marker), 2,
                                "the slow case's bytes never reached the "
                                "console")

    def test_stack_high_water_marks(self):
        self.boot()
        regions = dict((name, (base, top))
                       for name, base, top in self.reserved_stacks())
        for name in STACK_NAMES:
            base, top = regions[name]
            low = self.stacks.low.get(name)
            self.assertIsNotNone(low, "the %s stack was never used" % name)
            self.assertGreater(low, base,
                               "%s stack underflowed to %04X" % (name, low))
            print("ipc: %s stack high-water %04Xh-%04Xh (%d of %d bytes "
                  "used)" % (name, low, top, top - low + 1, top - base + 1))

    def test_the_run_ends_in_the_callers_mapping(self):
        """The probe's last act is the path-B copy with the aperture
        window open; by the end of the run the gate has put the caller's
        own mapping back (mode 10 over its bank, page bits clear)."""
        z = self.boot()
        self.assertEqual(z.latch & MODE_MASK, APPLICATION_MODE)
        self.assertEqual(z.latch & PAGE_MASK, 0)


class TestIpcServiceImages(IpcSim):
    """The resident service images (work package 40).

    The loader's own report is pinned against the packer's plan: the same
    tier policy, the same bases, the two architecture-36.20 ledgers and
    the boot tier's one-shot evidence, read back out of bank 6.  The
    service images cover their own copy internals; other images never
    load six like this.
    """

    def test_loader_reports_the_packer_plan(self):
        """Every image's placement line and both ledgers match the plan."""
        tx = self.transcript()
        plan = self.plan()
        for entry in plan["services"]:
            line = "driver %s: hot %04Xh" % (entry["name"],
                                             entry["hot_base"])
            if entry["io_base"] is not None:
                line += " io %04Xh" % entry["io_base"]
            if entry["boot_base"] is not None:
                line += " boot %04Xh" % entry["boot_base"]
            if entry["bulk_base"] is not None:
                line += " bulk %04Xh" % entry["bulk_base"]
            self.assertIn(line.encode() + b"\r\n", tx, line)
        nucleus = plan["nucleus"]
        self.assertIn(("driver hot: %d bytes of %d\r\n"
                       % (nucleus["service_hot_bytes"],
                          nucleus["service_hot_capacity_bytes"])).encode(),
                      tx)
        self.assertIn(("driver boot/bulk: %d bytes of %d\r\n"
                       % (nucleus["service_boot_bytes"],
                          nucleus["service_boot_capacity_bytes"])).encode(),
                      tx)
        print("ipc: six service images placed from the manifest, "
              "driver hot %d/%d, driver boot/bulk %d/%d"
              % (nucleus["service_hot_bytes"],
                 nucleus["service_hot_capacity_bytes"],
                 nucleus["service_boot_bytes"],
                 nucleus["service_boot_capacity_bytes"]))

    def test_every_boot_segment_ran_exactly_once(self):
        """Each boot segment recorded one run in its own bank-6 BSS."""
        z = self.boot()
        bank = z.banks[6]
        for name in SERVICE_ORDER:
            runs = self.symbol_address(name, "_ipc_%s_boot_runs" % name)
            magic = self.symbol_address(name, "_ipc_%s_boot_magic" % name)
            self.assertEqual(bank[runs], 1,
                             "%s boot segment ran %d times"
                             % (name, bank[runs]))
            self.assertEqual(bank[magic], IPC_BOOT_MAGIC, name)
        print("ipc: six boot segments, each a single execution")

    def test_con_slow_buffer_is_outside_the_service_io_range(self):
        """`con`'s slow buffer sits in the hot tier, not in 8000h-BFFFh."""
        plan = self.plan()
        entry = [item for item in plan["services"]
                 if item["name"] == "ipc-con.zmod"][0]
        address = entry["hot_base"] + \
            self.report("con")["symbols"]["_ipc_con_slow_buffer"]
        self.assertFalse(0x8000 <= address < 0xC000,
                         "the slow buffer is at %04Xh" % address)
        self.assertTrue(0x6000 <= address < 0xE000,
                        "the slow buffer is at %04Xh" % address)
        print("ipc: con slow buffer at %04Xh (hot tier)" % address)


class TestIpcPlacement(IpcSim):
    """Fixed-core placement: the substrate stays in the mapped core."""

    def test_fixed_core_symbols_stay_below_the_split(self):
        for name in ("ipc_asm_begin", "ipc_asm_end", "_ipc_lookup",
                     "_ipc_syscall_call", "_ipc_copy_run", "_ipc_invoke",
                     "_ipc_invoke_here", "_gw_invoke"):
            address = self.syms.get(name)
            if address is None:
                continue
            self.assertGreaterEqual(address, 0x2000, name)
            self.assertLess(address, 0x6000, name)
        self.assertEqual(self.syms["zos_service_stack"], 0x5D00)
        self.assertEqual(self.syms["zos_service_stack_top"], 0x5F00)
        self.assertEqual(self.syms["irq_isr_stack"], 0x5F00)
        self.assertEqual(self.syms["irq_isr_stack_top"], 0x6000)
        for name in ("zos_syscall_entry", "zos_gate_stack",
                     "zos_gate_stack_top"):
            self.assertGreaterEqual(self.syms[name], 0xE000, name)
        # The cold half starts at the split and the fixed core ends there.
        geometry = self.geometry()
        self.assertEqual(geometry["REV13_WINDOW_BASE"], 0x6000)
        self.assertLessEqual(geometry["FIXED_CODE_END"], 0x6000)
        self.assertEqual(self.syms["s__WINDOW"], 0x6000)
        self.assertEqual(geometry["KERNEL_CODE_END"],
                         self.syms["__kernel_code_end"])
        self.assertLessEqual(geometry["DATA_BASE"], 0x6000)
        print("ipc: substrate %04Xh-%04Xh, fixed code end %04Xh, cold "
              "end %04Xh, service stack %04Xh-%04Xh"
              % (self.syms["ipc_asm_begin"],
                 self.syms["ipc_asm_end"], geometry["FIXED_CODE_END"],
                 self.syms["__kernel_code_end"],
                 self.syms["zos_service_stack"],
                 self.syms["zos_service_stack_top"]))

    def geometry(self):
        """The build's generated memory geometry (ZOS_* constants)."""
        values = {}
        with open(os.path.join(ROOT, IMAGE_SUBDIR, "geometry",
                               "memory.inc")) as handle:
            for line in handle:
                match = re.match(r"^ZOS_([A-Z0-9_]+)\s*=\s*0x([0-9A-Fa-f]+)",
                                 line)
                if match:
                    values[match.group(1)] = int(match.group(2), 16)
        return values


if __name__ == "__main__":
    unittest.main()
