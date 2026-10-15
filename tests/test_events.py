#!/usr/bin/env python3
"""Device events acceptance: the NOTIFY/EVENT_RETURN contract and its replay.

Host checks pin what a replay cannot see:

* the ABI numbers (the two syscall numbers, the operation codes, the
  endpoint event flag) and the argument block's static assertions;
* the event table as the nucleus sees it: `kernel/event/event.h`'s shape and
  offsets against the assembler's own constants;
* the 24-byte delivery frame against the unwind that pops it and against
  both frame builders: the IRQ path completes the stub's saved pair at
  +20/+22, and the syscall path drops its own return address first, so the
  frame starts exactly 22 bytes below the caller's SP - the return address
  the caller's own stack already holds is the frame's PC field;
* the delivery rules as a model (owner running, inside its own syscall,
  inside its own handler, a child running, exited) and the post
  classification (pending only for the process the post runs for);
* the timer source's exact-deadline chunking, as a model of the channel
  and of the driver's count arithmetic: the deadline grid holds for every
  period and every rearm lateness, and the carry (the down-counter read)
  is what keeps the post from being late by the lateness twice over;
* the SDK surface, the manifest bits and the image that selects the source.

The replay boots the fs image with the emulated CTC attached (without it no
channel-3 interrupt ever fires) and drives `/system/events.exe` from the
monitor: the delivery count, the handler's clock readings against the
deadline grid, the driver's own post instants, the nucleus's per-delivery
cost, a console-and-card loop that holds its events instead of losing them,
a child run that must drop them, and an exit whose subscription the next
program takes over.

Run with: `python3 -m unittest tests.test_events`.  The replay class skips
unless `make fs-selftest` has built the image.
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

from maptools import evaluate, parse_asm_inc, parse_c_header  # noqa: E402
from replay import Replay  # noqa: E402
from test_fs_selftest_sim import FsSelftestSimBase  # noqa: E402
from z80sim import Ctc  # noqa: E402

PROTOCOL_H = os.path.join(ROOT, "abi", "kos", "protocol.h")
SYSCALL_H = os.path.join(ROOT, "abi", "kos", "syscall.h")
MOD_H = os.path.join(ROOT, "formats", "include", "mod.h")
HARDWARE_H = os.path.join(ROOT, "platform", "zephyr80", "public",
                          "hardware.h")
HARDWARE_INC = os.path.join(ROOT, "platform", "zephyr80", "public",
                            "hardware.inc")
EVENT_H = os.path.join(ROOT, "kernel", "event", "event.h")
EVENT_ASM = os.path.join(ROOT, "kernel", "event", "event_common.asm")
EVENT_C = os.path.join(ROOT, "kernel", "event", "event.c")
SYSCALL_C = os.path.join(ROOT, "kernel", "event", "event_syscall.c")
IRQ_ENTRY = os.path.join(ROOT, "kernel", "irq", "entry.asm")
IRQ_INIT = os.path.join(ROOT, "kernel", "irq", "init.asm")
SYS_ENTRY = os.path.join(ROOT, "kernel", "syscall", "entry.asm")
HANDLES_C = os.path.join(ROOT, "kernel", "vfs", "handles.c")
MAIN_C = os.path.join(ROOT, "kernel", "main.c")
CTC_C = os.path.join(ROOT, "kernel", "irq", "ctc.c")
SYSCALL_KERNEL_C = os.path.join(ROOT, "kernel", "syscall.c")
TIMER_C = os.path.join(ROOT, "drivers", "timer", "timer.c")
TIMER_PROTO = os.path.join(ROOT, "drivers", "timer", "timer_proto.c")
TIMER_JSON = os.path.join(ROOT, "drivers", "timer", "timer.json")
SDK_EVENT_H = os.path.join(ROOT, "user", "sdk", "include", "kos", "event.h")
SDK_EVENT_ASM = os.path.join(ROOT, "user", "sdk", "src", "event_entry.asm")
MODPACK = os.path.join(ROOT, "tools", "modpack.py")
MAKEFILE = os.path.join(ROOT, "Makefile")
FS_JSON = os.path.join(ROOT, "systems", "fs.json")
PRODUCTION_JSON = os.path.join(ROOT, "systems",
                              "production.json")
BEAT_C = os.path.join(ROOT, "tests", "beat", "beat.c")

IMAGE_SUBDIR = "build-fs-selftest"
IMAGE_NAME = "konaos-fs-selftest.bin"
IMAGE_PATH = os.path.join(ROOT, IMAGE_SUBDIR, IMAGE_NAME)
EVENTS_MAP = os.path.join(IMAGE_SUBDIR, "user", "events", "events.map")

PASS = "events: PASS\r\n"
FAIL = "events: FAIL"
PROMPT = "Z80> "

# The replay bound for one typed monitor command.  The measured runs need
# about 2 million steps; the bound only keeps a program that never reaches
# its verdict from running to the replay's own step limit.
REPLAY_LIMIT = 8_000_000

# The simulated CPU's T-state clock: a 25.6 µs count is a prescaler-256
# timer channel's 256 clocks, so the machine runs at 10 MHz.
TSTATES_PER_SECOND = 10000000

# The replay program's period, in the counts channel 3 works in.
PERIOD_COUNTS = 782
PERIOD_TSTATES = PERIOD_COUNTS * 256


def read(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def asm_constants(path, names):
    """The named constants of a `dasm` include, expressions evaluated."""
    env = parse_asm_inc(path)
    return {name: evaluate(env[name], env) for name in names}


def block(text, label, end="\n; ---"):
    """One labelled routine, up to the next banner comment."""
    return text.split(label, 1)[1].split(end, 1)[0]


def mnemonics(body):
    return [line.split(";")[0].strip().split()[0] for line in
            body.splitlines() if line.strip() and
            not line.strip().startswith(";")]


class TestEventContract(unittest.TestCase):
    """The ABI numbers, the table shape and the two frame builders."""

    def test_the_syscall_and_endpoint_numbers(self):
        env = parse_c_header(SYSCALL_H)
        self.assertEqual(evaluate(env["KOS_SYSCALL_NOTIFY"], env), 0x0D)
        self.assertEqual(evaluate(env["KOS_SYSCALL_EVENT_RETURN"], env),
                         0x0E)

        protocol = parse_c_header(PROTOCOL_H)
        # The subscribe reuses the existing op tail: acquisition and
        # release are its neighbours, and the byte the driver reads is
        # arg0, so the op codes are part of the wire format.
        self.assertEqual(evaluate(protocol["KOS_OP_SUBSCRIBE"],
                                  protocol), 0xFD)
        self.assertEqual(evaluate(protocol["KOS_OP_ACQUIRE"], protocol),
                         0xFE)
        self.assertEqual(evaluate(protocol["KOS_OP_RELEASE"], protocol),
                         0xFF)

        self.assertRegex(read(MOD_H), r"#define\s+MOD_EP_EVENTS\s+0x04u")
        # The packer decides what a manifest's "events" key sets.
        self.assertIn("EP_EVENTS = 0x04", read(MODPACK))

        # The argument block is ABI; its layout is asserted at build time.
        source = read(SYSCALL_H).split("struct kos_notify_args", 1)[1]
        for field in ("handle", "reserved", "handler", "trampoline"):
            self.assertIn(field, source.split("};", 1)[0])
        self.assertIn("sizeof(struct kos_notify_args) == 6", source)

    def test_the_event_table_matches_the_nucleus_view(self):
        env = parse_c_header(EVENT_H)
        self.assertEqual(evaluate(env["KOS_EVENT_MAX"], env), 4)
        self.assertEqual(evaluate(env["KOS_EVENT_ROW_SIZE"], env), 12)
        self.assertEqual(evaluate(env["KOS_EVENT_IN_HANDLER_SIZE"], env), 7)
        asm = asm_constants(EVENT_ASM, (
            "EVENT_MAX", "EVENT_ROW", "EVENT_SLOTS", "EVENT_OFF_BANK",
            "EVENT_OFF_PENDING", "EVENT_OFF_TRAMPOLINE",
            "EVENT_OFF_HANDLER", "EVENT_FRAME_SIZE"))
        self.assertEqual(asm["EVENT_MAX"], 4)
        self.assertEqual(asm["EVENT_ROW"], 12)
        self.assertEqual(asm["EVENT_SLOTS"], 7)
        # The row carries the owner's bank at +1, the counters at +2, and
        # the trampoline and handler the nucleus hands over through it.
        self.assertEqual(asm["EVENT_OFF_BANK"], 1)
        self.assertEqual(asm["EVENT_OFF_PENDING"], 2)
        self.assertEqual(asm["EVENT_OFF_TRAMPOLINE"], 6)
        self.assertEqual(asm["EVENT_OFF_HANDLER"], 8)

    def test_the_frame_is_the_layout_the_unwind_pops(self):
        asm = asm_constants(EVENT_ASM, ("EVENT_FRAME_SIZE",))
        self.assertEqual(asm["EVENT_FRAME_SIZE"], 24)

        # Unwind order: the (latch, 0) pair, the alternate set, AF', then
        # the main set from IY down to AF - so the frame reads latch, HL',
        # DE', BC', AF', IY, IX, HL, DE, BC, AF, PC from its base upward.
        self.assertEqual(mnemonics(block(read(EVENT_ASM),
                                         "_kos_event_unwind::")),
                         ["ld", "out", "ld", "pop", "exx", "pop", "pop",
                          "pop", "exx", "ex", "pop", "ex", "pop", "pop",
                          "pop", "pop", "pop", "pop", "ei", "ret"])

        # The syscall path completes that frame with the caller's own
        # return address: the frame is 22 bytes below the caller's SP, so
        # the check drops its own return address first and pushes the 22
        # bytes the layout needs - AF, BC, DE, the result in HL, IX, IY,
        # the alternate set, the (latch, 0) pair - then saves the frame
        # pointer across the row lookup.
        body = read(EVENT_ASM).split("_kos_event_syscall_exit::", 1)[1]
        self.assertEqual(re.findall(r"^\tpush\t(\w+)", body, re.MULTILINE),
                         ["af", "bc", "de", "hl", "ix", "iy",
                          "af", "bc", "de", "hl", "hl", "hl"])
        self.assertIn("\tpop\thl\n", body)

        # The IRQ path instead completes the same layout with the pair the
        # IRQ stub already pushed: frame = irq_saved_sp - 20.
        self.assertIn("#-(EVENT_FRAME_SIZE - 4)", read(EVENT_ASM))
        irq = block(read(EVENT_ASM), "_kos_event_irq_exit::")
        self.assertIn("ld\tbc,#(EVENT_FRAME_SIZE - 6)", irq)
        self.assertIn("ldir", irq)
        self.assertIn("call\t_kos_event_irq_exit", read(IRQ_ENTRY))
        self.assertIn("call\t_kos_event_syscall_exit", read(SYS_ENTRY))

    def test_the_return_frame_sits_above_the_rst_address(self):
        body = block(read(EVENT_ASM), "_kos_event_return_frame::")
        # The trampoline's RST pushed its return address at the caller's
        # SP, so the frame EVENT_RETURN reuses is two bytes above it.
        self.assertIn("ld\thl,(kos_gate_saved_sp)", body)
        self.assertIn("ld\tde,#2", body)
        # sdcccall(1): a 16-bit result is returned in DE.
        self.assertEqual(mnemonics(body), ["ld", "ld", "add", "ex", "ret"])
        # The cold body stores it for the delivery being closed.
        self.assertIn("frame = kos_event_return_frame();", read(SYSCALL_C))

    def test_the_timer_source_and_the_image_are_wired(self):
        env = parse_c_header(HARDWARE_H)
        # Channel 3 loads with the tick's own control word plus the
        # software reset: timer mode, prescaler 256 (the 25.6 µs count the
        # whole OS counts in), restarted from any state at every load.
        self.assertEqual(evaluate(env["KOS_CTC3_PORT"], env), 0x43)
        self.assertEqual(evaluate(env["KOS_CTC3_CONTROL_LOAD"], env),
                         evaluate(env["KOS_CTC2_CONTROL_TIMER"], env) |
                         evaluate(env["KOS_CTC_CTRL_SOFT_RESET"], env))
        self.assertEqual(evaluate(env["KOS_CTC3_REACH"], env), 256)
        inc = asm_constants(HARDWARE_INC, (
            "KOS_CTC3_PORT", "KOS_CTC3_REACH",
            "KOS_CTC3_CONTROL_LOAD", "KOS_CTC2_CONTROL_TIMER"))
        self.assertEqual(inc["KOS_CTC3_PORT"], 0x43)
        self.assertEqual(inc["KOS_CTC3_REACH"], 256)
        self.assertEqual(inc["KOS_CTC3_CONTROL_LOAD"],
                         inc["KOS_CTC2_CONTROL_TIMER"] | 0x02)

        # The vector page has the channel's entry, and the driver claims
        # its source right beside the tick's.
        self.assertIn("irq_ctc3_stub", read(IRQ_INIT))
        self.assertIn("ld\t(KOS_IRQ_VECTOR_BASE + 0x0006),hl", read(IRQ_INIT))
        proto = read(TIMER_PROTO)
        self.assertIn("KOS_IRQ_CTC3", proto)
        self.assertIn("timer_event_irq_handler", proto)

        # The manifest asks for the release hook and the event flag, and
        # the image stores the program and loads the driver.
        timer = read(TIMER_JSON)
        self.assertIn('"events": true', timer)
        self.assertIn('"release": true', timer)
        image = read(FS_JSON)
        self.assertIn('"events.exe"', image)
        self.assertIn('"timer"', image)
        self.assertIn("exe_program,events", read(MAKEFILE))

    def test_the_sdk_surface_is_the_programs_contract(self):
        sdk = read(SDK_EVENT_H)
        self.assertIn("notify(", sdk)
        self.assertIn("timer_event(", sdk)
        self.assertIn("kos_event_handler_t", sdk)
        self.assertIn("count", sdk)
        self.assertIn("missed", sdk)

        # The trampoline calls the handler through the SDK's C convention
        # and re-enters the nucleus with EVENT_RETURN in A.  It is libkos
        # code in the program's primary image, never a far segment.
        tramp = read(SDK_EVENT_ASM)
        self.assertIn("___sdcc_call_iy", tramp)
        self.assertIn("rst\t0x30", tramp)
        self.assertIn("SYS_EVENT_RETURN = 0x0E", tramp)
        self.assertNotIn("FAR", tramp)
        env = parse_c_header(SYSCALL_H)
        self.assertEqual(evaluate(env["KOS_SYSCALL_EVENT_RETURN"], env),
                         0x0E)


class TestEventRules(unittest.TestCase):
    """The delivery rules and the post classification, as a model."""

    RUNNING = 0x10
    OS = 0x18

    def deliverable(self, rows, latch, handler, pending):
        """`kos_event_pick`: the lowest bound row of the process the latch
        names, with pending events and no handler of that process active."""
        if (latch & 0x18) != self.RUNNING:
            return None
        bank = latch & 0x07
        for index, row in enumerate(rows):
            if row["owner"] == 0 or row["bank"] != bank:
                continue
            if handler.get(row["owner"], 0) != 0:
                continue
            if pending.get(row["owner"], 0) == 0:
                continue
            return index
        return None

    def post(self, row, latches):
        """`kos_event_post_bank` + `kos_event_post`: an event is pending
        exactly while the post runs for the subscriber's own process -
        inside its own code (the interrupted latch names it) or inside one
        of its syscalls (the gate's saved latch does)."""
        irq, gate = latches
        if (irq & 0x18) == self.RUNNING:
            bank = irq & 0x07
        else:
            bank = gate & 0x07
        if row["bank"] == bank:
            row["pending"] += 1
        else:
            row["missed"] += 1

    def test_the_delivery_rules(self):
        rows = [{"owner": 1, "bank": 1}]
        handler = {}
        pending = {1: 1}

        # Its own code running: delivered.
        self.assertEqual(self.deliverable(rows, 0x11, handler, pending), 0)
        # The nucleus is running (a syscall of its own, an IPC, a driver
        # body): nothing yet - that is the "held" row.
        self.assertIsNone(self.deliverable(rows, self.OS | 1, handler,
                                           pending))
        self.assertIsNone(self.deliverable(rows, self.OS | 7, handler,
                                           pending))
        # Inside its own handler: never nested.
        handler[1] = 1
        self.assertIsNone(self.deliverable(rows, 0x11, handler, pending))
        handler[1] = 0
        # A child running: its bank is not the subscriber's.
        self.assertIsNone(self.deliverable(rows, 0x12, handler, pending))
        # Nothing pending.
        self.assertIsNone(self.deliverable(rows, 0x11, handler, {}))
        # Exited or unbound: the row is free.
        rows[0]["owner"] = 0
        self.assertIsNone(self.deliverable(rows, 0x11, handler, pending))

    def test_pending_against_missed_is_the_posted_bank(self):
        row = {"owner": 1, "bank": 1, "pending": 0, "missed": 0}
        # The owner's own code interrupted: pending, and counted.
        self.post(row, (0x11, 0x00))
        # A child (bank 2), another process, or the nucleus: missed.
        self.post(row, (0x12, 0x00))
        self.post(row, (self.OS | 7, 0x00))
        # The owner is inside a syscall or an IPC call: the gate's saved
        # latch names it, so the event is pending and waits for the call
        # to return.
        self.post(row, (self.OS | 1, 0x11))
        # Boot, before any process runs: a driver posting by hand is
        # dropped rather than kept for a process that does not exist.
        self.post(row, (0x00, 0x00))
        self.assertEqual(row["pending"], 2)
        self.assertEqual(row["missed"], 3)

        # Isolation on the delivery side is the same comparison: the scan
        # only picks a row whose bank is the one the delivery runs under
        # (`kos_event_pick_core`), so a suspended owner's backlog can never
        # be handed to whatever program happens to hold the machine - the
        # child in `events run` gets its own bank and must not be entered.
        scan = block(read(EVENT_ASM), "_kos_event_pick_core:",
                     "\nevent_pick_next:")
        self.assertIn("EVENT_OFF_BANK(ix)", scan)
        self.assertIn("cp\tc", scan)

    def test_the_nucleus_counts_a_delivery_once(self):
        enter = block(read(EVENT_ASM), "_kos_event_enter::")
        for byte in range(4):
            self.assertIn("ld\tEVENT_OFF_PENDING" +
                          ("" if byte == 0 else "+%d" % byte) + "(ix),a",
                          enter)
        self.assertIn("xor\ta", enter)
        # Both counters saturate instead of wrapping.
        self.assertEqual(read(EVENT_C).count("!= 0xFFFFu"), 2)
        # A released handle and an exiting process both drop the rows.
        teardown = read(SYSCALL_C)
        self.assertIn("kos_event_handle_release", teardown)
        self.assertIn("kos_event_process_exit", teardown)
        self.assertIn("kos_event_handle_release", read(HANDLES_C))
        self.assertIn("kos_event_process_exit", read(HANDLES_C))

    def test_the_table_is_valid_before_interrupts_can_arrive(self):
        """`kernel_main` clears the table before anything can take an
        interrupt: the common region arrives as FFh (the ROM image carries
        no `_COMMON_DATA` bytes) and both exit checks read the table on
        every return, so an interrupt before the clear would scan garbage
        rows."""
        body = read(MAIN_C)
        clear = body.index("\tkos_event_init();")
        init = body.index("\tirq_init();")
        self.assertLess(clear, init)
        self.assertNotIn("\n\tei", body[body.index("void kernel_main(void)"):
                                        clear])
        # `irq_init` itself masks interrupts while it seeds the vector page.
        arch = block(read(IRQ_INIT), "_irq_arch_init::")
        self.assertEqual(mnemonics(arch)[0], "di")

    def test_the_return_is_masked_before_it_reads_the_latch(self):
        """EVENT_RETURN runs as a syscall body, with the caller's interrupt
        state, and every interrupt exit check rewrites kos_event_latch.  An
        interrupt between loading the frame's latch and the unwind (or a
        re-entry) would hand the program the nucleus's mapping and leave
        in_handler set - every later delivery then blocks while the clock
        runs on (the board's stalled `beat`).  The body masks first."""
        body = read(SYSCALL_C)
        body = body[body.index("void kos_event_return(void)"):]
        body = body[:body.index("\n}")]
        lock = body.index("irq_arch_lock()")
        self.assertLess(lock, body.index("kos_event_in_handler[pid] = 0u;"))
        self.assertLess(lock, body.index(
            "kos_event_latch = kos_event_frame_latch[pid];"))
        self.assertLess(lock, body.index("kos_event_enter();"))

    def test_every_iff_read_is_repeated(self):
        """On the board an interrupt accepted right after LD A,I makes that
        read report IFF2 = 0 (the documented LD A,I behaviour; the board's
        Z84C0020 shows it).  A gate that trusted one read handed programs
        back masked - no delivery could reach them - so every place that
        captures the interrupt state reads it up to three times and takes
        any 'on'.  No source may keep a single read."""
        roots = ("kernel", "drivers", os.path.join("user", "sdk", "src"))
        sites = 0
        for root in roots:
            for dirpath, _, names in os.walk(os.path.join(ROOT, root)):
                for name in names:
                    if not name.endswith(".asm"):
                        continue
                    path = os.path.join(dirpath, name)
                    lines = read(path).splitlines()
                    for index, line in enumerate(lines):
                        if not re.match(r"^\tld\ta,i\b", line):
                            continue
                        before = lines[index - 1] if index else ""
                        after = lines[index + 1] if index + 1 < len(lines) else ""
                        if before.startswith("\tjp\tpe,"):
                            continue    # a repeat inside the sequence
                        sites += 1
                        self.assertTrue(
                            after.startswith("\tjp\tpe,"),
                            "%s:%d reads IFF2 once"
                            % (os.path.relpath(path, ROOT), index + 1))
        self.assertGreaterEqual(sites, 10)

    def test_a_warm_reboot_stops_every_ctc_channel(self):
        """The CTC is not reset by the reset vector, so the reboot stops
        every channel the kernel may have left programmed: the tick (*2)
        and the event timer (*3) would otherwise keep requesting into the
        next boot, where the delivery checks read a table that boot has
        not cleared yet."""
        reboot = read(SYSCALL_KERNEL_C)
        reboot = reboot[reboot.index("static void syscall_reboot(void)"):]
        reboot = reboot[:reboot.index("\n}")]
        self.assertIn("di", reboot)
        self.assertIn("platform_ctc_quiesce_all();", reboot)
        self.assertNotIn("platform_irq_quiesce(", reboot)
        quiesce = block(read(CTC_C), "void platform_ctc_quiesce_all(void)")
        for port in ("KOS_CTC0_PORT", "KOS_CTC1_PORT", "KOS_CTC2_PORT",
                     "KOS_CTC3_PORT"):
            self.assertIn("kos_ctc%s = KOS_CTC_RESET;" % port[7], quiesce)


class TestEventTimerDeadlines(unittest.TestCase):
    """The event timer's deadlines, as a count-by-count model.

    The clock (channel 2) is the only time reference: it expires every
    250 counts and its handler reads the exact count (the tick count was
    just incremented, the counter just reloaded).  When the next deadline
    is within the reach (256 counts), that handler loads channel 3 with
    the counts left; channel 3's handler stops the channel, posts and adds
    the period, reading the clock too (a reading below the deadline is
    one tick stale and is lifted).  Each channel holds one request, a
    masked stretch holds both handlers off, and channel 2 wins a tie.
    """

    TICK = 250
    REACH = 256
    GUARD = 400000      # a runaway model fails instead of eating memory

    def simulate(self, period, start=0, masks=(), periods=6):
        """Returns (posts, lost_ticks): (deadline, count posted at) pairs."""
        masked = set()
        for begin, length in masks:
            masked.update(range(begin, begin + length))
        ticks = start // self.TICK
        last2 = ticks * self.TICK
        pend2 = pend3 = False
        lost2 = 0
        loaded = False
        load_counts = 0
        next3 = None

        def clock(t):
            return ticks * self.TICK + (t - last2)
        deadline = start + period
        posts = []

        def service(now, t):
            nonlocal deadline, loaded, load_counts, next3
            while now >= deadline:
                posts.append((deadline, t))
                deadline += period
            left = deadline - now
            if left <= self.REACH:
                loaded, load_counts, next3 = True, left, t + left

        service(start, start)           # the arm, at an exact reading
        t = start
        while len(posts) < periods:
            t += 1
            self.assertLess(t - start, self.GUARD, "the model ran away")
            if t % self.TICK == 0:
                if pend2:
                    lost2 += 1
                pend2 = True
                last2 = t
            if loaded and t == next3:
                pend3 = True            # a second expiry would be lost
                next3 += load_counts
            if t in masked:
                continue
            if pend2:
                pend2 = False
                ticks += 1
                if not loaded:
                    service(clock(t), t)
            elif pend3:
                pend3 = False
                if loaded:
                    now = clock(t)
                    if now < deadline:
                        now += self.TICK
                    loaded = False
                    service(now, t)
        return posts[:periods], lost2

    def check_grid(self, period, posts, start=0, worst_late=0):
        for index, (deadline, at) in enumerate(posts):
            self.assertEqual(deadline, start + period * (index + 1),
                             "period %d: the grid drifted" % period)
            self.assertGreaterEqual(at, deadline,
                                    "period %d: posted early" % period)
            self.assertLessEqual(at - deadline, worst_late,
                                 "period %d: %d counts late" %
                                 (period, at - deadline))

    def test_an_unmasked_channel_posts_on_the_deadline(self):
        for period in (1, 2, 64, 249, 250, 251, 256, 257, 500, 782, 4096):
            for start in (0, 1, 125, 249):
                posts, lost = self.simulate(period, start)
                self.assertEqual(lost, 0)
                # A tie with the tick costs channel 3 one count.
                self.check_grid(period, posts, start, worst_late=1)

    def test_a_masked_stretch_makes_one_post_late_and_the_next_exact(self):
        # The board's console and card paths can mask longer than one
        # load (256 counts): the post inside the mask comes out at its
        # end, and the next deadline is exact again.  (A mask holding two
        # tick expiries loses one tick - the clock's own limit, which every
        # deadline then follows; the board's stopwatch runs show its masks
        # stay short of that.)
        for length in (10, 100, 255, 300):
            for offset in (-length, -length // 2, 0):
                deadline = 2 * 782
                masks = ((deadline + offset, length),)
                posts, lost = self.simulate(782, masks=masks, periods=4)
                self.assertEqual(lost, 0)
                self.check_grid(782, posts, worst_late=length + 1)
                late = [at - d for d, at in posts]
                self.assertEqual(late[2:], [0, 0],
                                 "length %d offset %d: %r" %
                                 (length, offset, late))

    def test_masks_everywhere_never_lose_double_or_advance_a_post(self):
        for period in (100, 256, 782, 1000):
            masks = tuple((begin, 180) for begin in range(300, 8000, 410))
            posts, lost = self.simulate(period, masks=masks, periods=8)
            self.assertEqual(lost, 0)
            self.check_grid(period, posts, worst_late=181)

    def test_the_arm_resolves_the_stale_tick(self):
        # A tick that expires after the arm masks leaves the masked
        # reading one tick low; the open reading just before it is never
        # low, so a masked reading below it is lifted by one tick.
        def resolve(before, masked):
            return masked + self.TICK if masked < before else masked
        for true in (250, 251, 500, 782, 65535):
            self.assertEqual(resolve(true - 3, true), true)
            self.assertEqual(resolve(true - 3, true - self.TICK), true)

    def test_the_source_is_the_model(self):
        source = read(TIMER_C)
        tick = block(source, "void timer_irq_handler(void *context)", "\n}")
        self.assertIn("timer_ticks = timer_ticks + 1u;", tick)
        self.assertIn("timer_event_armed != 0u && timer_event_loaded == 0u",
                      tick)
        self.assertIn("timer_event_service(timer_counts_now_locked());", tick)
        service = block(source,
                        "static void timer_event_service(uint32_t now)\n{",
                        "\n}")
        self.assertIn("timer_event_deadline += timer_event_period;", service)
        self.assertIn("left <= KOS_CTC3_REACH", service)
        handler = block(source, "void timer_event_irq_handler", "\n}")
        self.assertIn("timer_event_stop_channel();", handler)
        self.assertIn("now = timer_counts_now_locked();", handler)
        self.assertIn("((now - timer_event_deadline) & 0x80000000u) != 0u",
                      handler)
        self.assertIn("timer_event_service(now);", handler)
        load = block(source, "static void timer_event_load(uint16_t counts)",
                     "\n}")
        self.assertIn("KOS_CTC3_CONTROL_LOAD", load)
        self.assertIn("(uint8_t)counts", load)
        # Nothing on channel 3 is counted any more.
        for gone in ("timer_event_chunk", "timer_event_anchor",
                     "timer_event_now"):
            self.assertNotIn(gone, source)
        # The boot segment resets channel 3 from any state.
        boot = read(os.path.join(ROOT, "drivers", "timer", "timer_boot.c"))
        self.assertIn("timer_ctc3_port = KOS_CTC_RESET;", boot)

    def test_the_arm_is_atomic_and_the_release_stops_the_source(self):
        arm = block(read(TIMER_C), "int timer_event_arm(uint32_t period)",
                    "\nuint8_t timer_event_subscribed")
        # Interrupts masked across the deadline update and the load, and a
        # period of zero stops the channel.
        self.assertIn("timer_irq_save_disable()", arm)
        self.assertIn("timer_event_stop_channel();", arm)
        self.assertIn("timer_irq_restore(token)", arm)
        self.assertIn("before = timer_counts_now();", arm)
        self.assertIn("now += KOS_CTC2_TIME_CONSTANT;", arm)
        # One subscription at a time, and only with one in place.
        self.assertIn("-KOS_EBUSY", arm)
        self.assertIn("-KOS_EINVAL", arm)
        # A stop is valid whenever it comes: the unsubscribe and the
        # release both drop the subscription before stopping the channel,
        # so the stop path must not sit behind the check for one being in
        # place - the channel would stay armed and the next subscriber
        # would be refused with EBUSY.
        self.assertLess(arm.index("period == 0u"),
                        arm.index("timer_event_sub == 0u"))
        # The release stops the channel with the subscription.
        release = read(TIMER_C).split("void timer_event_subscribe", 1)[1]
        self.assertIn("timer_event_arm(0u)", release)


class TestBeatBoardCheck(unittest.TestCase):
    """`/system/beat.exe`, the event mechanism's board check.

    The check itself is heard, not asserted: the owner runs the program
    on a board and listens for the 500 ms beat with `missed` at zero, and
    the simulator cannot stand in for it (the program's console output
    never appears on the production replay; the validation record carries
    that).  What is left for a host test is what a silent edit would
    break: that the program ships, that it subscribes and arms the 500 ms
    source, that it accumulates what the nucleus reports, and that it
    silences the tone on the way out.
    """

    def test_the_program_is_declared_and_the_store_carries_it(self):
        makefile = read(MAKEFILE)
        self.assertIn("exe_program,beat,tests/beat", makefile)
        # The handler runs on the program's own stack, below the 24-byte
        # delivery frame, so the reservation is the SDK default rather
        # than the 256 the delivery-less programs use.
        self.assertIn("beat.c,STACK=384,STAGE=1", makefile)
        with open(PRODUCTION_JSON, encoding="utf-8") as handle:
            system = json.load(handle)
        self.assertIn("beat.exe", system["store"])

    def test_the_source_is_the_check(self):
        body = read(BEAT_C)
        # 500 ms in the clock's counts, one subscription, one arm.
        self.assertIn("notify(event_handle, beat_on_event)", body)
        self.assertIn("timer_event(event_handle, PERIOD_COUNTS)", body)
        self.assertIn("#define PERIOD_COUNTS 19531u", body)
        # The handler accumulates the nucleus's per-delivery counters (they
        # are backlogs since the last delivery, not totals) and toggles the
        # tone.
        self.assertIn("beat_events += count;", body)
        self.assertIn("beat_missed += missed;", body)
        # PSG0 at E0h through the diagnostic boundary, the board's port and
        # byte order (docs/devices.md section 9).
        self.assertIn("#define PSG0_PORT   0xE0u", body)
        self.assertIn("diag_io_out(&req)", body)
        # Every path out of the program silences the tone.
        self.assertGreaterEqual(body.count("tone_silence();"), 5)
        # `beat reboot` reboots while subscribed and armed: the call sits
        # in the run loop, before any stop or unsubscribe.
        self.assertIn('#define REBOOT_TAIL   "reboot"', body)
        loop = body[body.index("for (second = 1u;"):]
        self.assertLess(loop.index("reboot();"),
                        loop.index("timer_event(event_handle, 0u)"))


@unittest.skipUnless(os.path.exists(IMAGE_PATH),
                     "run make fs-selftest to build the image")
class EventsReplayMixin:
    """The fs image's event program, driven from the monitor.

    Two suites run it on two machines.  The fs-selftest harness carries
    the emulated card peer, whose lane is paced per byte, so a critical
    section in the card path can last whole periods: there the test
    asserts what holds under any masking (no lost or doubled delivery,
    every period accounted for, deadlines monotonic) and measures the
    mechanism's cost.  The harness without the peer is the one where the
    masking is realistic, so the exact-deadline bounds (grid drift, the
    interval sum, the post instants) are asserted there.
    """

    def make_cpu(self, **peer_kwargs):
        z = super().make_cpu(**peer_kwargs)
        # Without the CTC model a channel-3 write goes nowhere and no
        # event ever fires.
        Ctc().attach(z)
        return z

    def command(self, z, text, stops=(PASS, FAIL, PROMPT)):
        """Type one monitor command and return its fresh transcript."""
        start = len(z.sio_tx)
        z.sio_rx.extend(text.encode("ascii") + b"\r")
        self.run_until(z, list(stops), limit=REPLAY_LIMIT, since=start,
                       halt_ok=True, panic=None)
        return bytes(z.sio_tx)[start:].decode("ascii", "replace")

    def watch_fields(self, transcript):
        # The summary line shares its line with the handler's console
        # output (every delivery prints an "E" through a syscall).
        line = [part for part in transcript.splitlines()
                if "events: watch " in part][-1]
        return [int(value) for value in re.findall(r"\d+", line)]

    def app_symbols(self):
        """The program's linked map: {name: address}."""
        table = {}
        with open(os.path.join(ROOT, EVENTS_MAP), "r",
                  encoding="utf-8") as handle:
            for line in handle:
                fields = line.split()
                if len(fields) >= 2 and fields[0] and fields[1]:
                    try:
                        table[fields[1]] = int(fields[0], 16)
                    except ValueError:
                        continue
        return table

    def app_symbol(self, name):
        table = self.app_symbols()
        if name not in table:
            raise AssertionError("%s is not in %s" % (name, EVENTS_MAP))
        return table[name]

    def watch_log(self, z):
        """Run `events watch` and return (transcript, log).

        The log is the ordered trace of the delivery path: `deliver` and
        `check` are the two return-path checks, `post` the driver's own
        deadline post, `trampoline` and `unwind` the SDK trampoline's
        entry and the EVENT_RETURN unwind, and `handler-enter` and
        `handler-exit` the handler's own two ends - its entry, and the
        trampoline's restore: the step it returned at (its return address,
        read off the stack when it was entered).  Common-memory symbols
        are
        unique; the program's own image shares its addresses with the
        nucleus (mode 11 shows bank 7 over 0000h-5FFFh), so those two are
        conditioned on the mapping the delivery itself selects: the
        trampoline runs in the application mode over the program's bank,
        the post in OS mode.
        """
        common = {self.syms["_kos_event_irq_exit"]: "deliver",
                  self.syms["_kos_event_syscall_exit"]: "check",
                  self.syms["_kos_event_unwind"]: "unwind"}
        trampoline = self.app_symbol("_kos_event_entry")
        post = self.syms["_kos_event_post"]
        handler = self.app_symbols()["_events_on_timer"]
        depth = 0
        return_to = 0
        log = []

        def record(step):
            nonlocal depth, return_to
            if depth > 0 and step.pc == return_to:
                # The instruction after the trampoline's call: this
                # delivery's handler is done and the restore is next.  A
                # delivery inside the handler nests, so the depth is what
                # tells the two returns apart.
                depth -= 1
                log.append((step.tstates, "handler-exit"))
            elif step.pc == handler:
                if depth == 0:
                    return_to = step.rd16(step.sp)
                depth += 1
                log.append((step.tstates, "handler-enter"))
            kind = common.get(step.pc)
            if kind is not None:
                log.append((step.tstates, kind))
            elif step.pc == trampoline:
                if (step.latch & 0x18) == 0x10:
                    log.append((step.tstates, "trampoline"))
            elif step.pc == post and (step.latch & 0x18) == 0x18:
                log.append((step.tstates, "post"))

        start = len(z.sio_tx)
        z.sio_rx.extend(b"X /system/events.exe watch\r")
        self.run_until(z, [PASS, FAIL, PROMPT], limit=REPLAY_LIMIT,
                       since=start, halt_ok=True, panic=None,
                       on_step=record)
        transcript = bytes(z.sio_tx)[start:].decode("ascii", "replace")
        self.assertNotIn(FAIL, transcript)
        self.assertIn(PASS, transcript)
        return transcript, log

    def cost_per_delivery(self, log, deliveries):
        """The nucleus's own cost per delivery, in T-states.

        The mechanism's round trip less the handler itself: the frame
        build and the hand-off into the trampoline (measured to the
        handler's entry, so the trampoline's argument-block save is in),
        then the trampoline's restore with the EVENT_RETURN gate and the
        unwind.  The handler runs between those two spans and is not
        counted, so neither is what it does after its last syscall -
        formatting and a buffered console write, since the SDK buffers
        them.  The two return-path checks are entered on every interrupt
        and every syscall, which is what makes them cheap to keep.
        """
        self.assertEqual(sum(1 for _, kind in log
                             if kind == "trampoline"), deliveries)
        self.assertEqual(sum(1 for _, kind in log if kind == "unwind"),
                         deliveries)
        cost = 0.0
        seen = 0
        irq = check = None
        frames = []
        for when, kind in log:
            if kind == "deliver":
                irq = when
            elif kind == "check":
                check = when
            elif kind == "trampoline":
                self.assertIsNotNone(irq)
                # The hand-off up to the trampoline's first instruction.
                cost += when - irq
                irq = None
                frames.append([when, None, None])
                seen += 1
            elif kind == "handler-enter":
                # Everything from the trampoline's entry to the handler's
                # is mechanism: its three pushes and the argument-block
                # save.  The handler's own body runs from here to the
                # restore.
                frames[-1][1] = when - frames[-1][0]
            elif kind == "handler-exit":
                frames[-1][2] = when
            elif kind == "unwind":
                trampoline_when, save, exit_when = frames.pop()
                self.assertIsNotNone(exit_when or check)
                start = exit_when if exit_when is not None else check
                cost += when - start + (save or 0.0)
                check = None
        self.assertEqual(seen, deliveries)
        return cost / seen


@unittest.skipUnless(os.path.exists(IMAGE_PATH),
                     "run make fs-selftest to build the image")
class TestEventDeadlineReplay(EventsReplayMixin, FsSelftestSimBase):
    """The deadline grid, on the machine where masking is realistic.

    The same image and the same program as TestEventDeliveryReplay, but
    without the emulated card peer: its lane is paced per byte, so masked
    stretches last seconds there, and a masked stretch is exactly what
    moves a delivery's own reading without the driver's deadlines moving.
    The bounds here are the ones the record and the owner's board check
    carry.
    """

    def make_cpu(self, **peer_kwargs):
        # The base machine, without the card peer and its SIO lane.
        z = Replay.make_cpu(self)
        Ctc().attach(z)
        return z

    def test_watch_holds_the_deadline_grid(self):
        z = self.boot()[0]
        transcript, log = self.watch_log(z)
        (deliveries, counts, missed, drift, late,
         intervals, elapsed) = self.watch_fields(transcript)
        self.assertEqual(deliveries, 50)
        self.assertEqual(counts, 50)
        self.assertEqual(missed, 0)
        # The program's own readings: each is the handler's clock call,
        # which an interrupt can delay, so each is bounded (drift), and
        # the signed interval errors sum to the last reading's error less
        # the first's - bounded by the same figure, never accumulating.
        # (`elapsed` is that sum with the periods left in.)  The grid
        # itself is the driver's posts, asserted below.
        self.assertLessEqual(drift, 32)
        self.assertLessEqual(abs(intervals), 32)
        self.assertLessEqual(abs(elapsed - (deliveries - 1) * PERIOD_COUNTS),
                             32)

        # The driver's own posts are the deadline grid: one per period,
        # and its integer count arithmetic is what the grid is.
        posts = [when for when, kind in log if kind == "post"]
        self.assertEqual(len(posts), 50)
        errors = [abs(posts[i] - posts[0] - i * PERIOD_TSTATES) / 256.0
                  for i in range(len(posts))]
        self.assertLessEqual(sorted(errors)[len(errors) // 2], 1.0,
                             "the median post is off its deadline")
        self.assertLessEqual(max(errors), 16.0)

        # The mechanism's own cost per delivery, in its three spans: the
        # frame build and the hand-off into the trampoline (1,710), the
        # trampoline's entry - its pushes and the argument-block save
        # (2,568) - and the restore with the EVENT_RETURN gate and the
        # unwind (4,129).  The handler's own body sits between the last
        # two and is not counted: the buffered console made it large
        # (about 8,300 T-states per delivery), and the older reading
        # carried it because it measured the return side from the
        # handler's last syscall.  The bound is the measurement plus a
        # tenth.
        per_delivery = self.cost_per_delivery(log, deliveries)
        share = per_delivery * 50.0 / TSTATES_PER_SECOND
        self.assertLessEqual(per_delivery, 9250.0,
                             "delivery costs %.0f T-states (%.1f%% at 50 Hz)"
                             % (per_delivery, share * 100.0))
        self.assertLessEqual(share, 0.05)
        self.assertGreaterEqual(per_delivery, 1000.0)


@unittest.skipUnless(os.path.exists(IMAGE_PATH),
                     "run make fs-selftest to build the image")
class TestEventDeliveryReplay(EventsReplayMixin, FsSelftestSimBase):
    """The delivery path on the card-peer machine: what any masking keeps.

    The peer's lane is paced per byte, so its masked stretches can outlast
    a period; the exact-deadline bounds are asserted by
    TestEventDeadlineReplay, which runs the same image without it.
    """

    def test_watch_delivers_every_event_once(self):
        z = self.boot()[0]
        transcript, log = self.watch_log(z)
        (deliveries, counts, missed, drift, late,
         intervals, elapsed) = self.watch_fields(transcript)
        self.assertEqual(deliveries, 50)
        self.assertEqual(missed, 0)
        # Every delivered event stands for at least one deadline (`count`
        # would be zero), and none was delivered twice: one post per
        # delivery, in step with the trampoline and the unwind.
        self.assertGreaterEqual(counts, deliveries)
        posts = [when for when, kind in log if kind == "post"]
        self.assertEqual(len(posts), deliveries)
        # Deadlines are monotonic: the posts come in the order the driver
        # reached them, never backwards.
        self.assertEqual(sorted(posts), posts)
        # `count` and `missed` account for every period: each post added
        # one to a row's backlog, and the deliveries carry every one of
        # them - delivered or dropped - so the totals balance to within
        # the one post that may still be waiting when the program stops.
        self.assertLessEqual(counts + missed, len(posts))
        self.assertLessEqual(len(posts), counts + missed + 1)
        # The mechanism's cost, with the same stamps as the deadline
        # test's reading (the peer's masking does not move the spans; the
        # harness without it is the one that pins them against jitter).
        per_delivery = self.cost_per_delivery(log, deliveries)
        self.assertGreaterEqual(per_delivery, 1000.0)
        self.assertLessEqual(per_delivery, 12000.0)

    def test_the_loop_holds_events_instead_of_losing_them(self):
        z = self.boot()[0]
        transcript = self.command(z, "X /system/events.exe loop")
        self.assertNotIn(FAIL, transcript)
        self.assertIn(PASS, transcript)
        line = [part for part in transcript.splitlines()
                if "events: loop " in part][-1]
        # `events: loop <deliveries> deliveries counts <counts> missed
        # <missed> rounds <rounds>` - the last such line is the summary
        # the rounds' own progress lines lead up to.
        deliveries, counts, missed, rounds = \
            [int(value) for value in re.findall(r"\d+", line)]
        self.assertGreaterEqual(deliveries, 25)
        self.assertGreaterEqual(counts, deliveries)
        # Not one event was dropped while the calls were in progress.
        self.assertEqual(missed, 0)
        # The rounds are where console and card calls held an event.
        self.assertGreater(rounds, 0)

    def test_a_child_run_drops_the_parent_events(self):
        z = self.boot()[0]
        transcript = self.command(
            z, "X /system/events.exe run /system/fs_child.exe tail-42")
        self.assertNotIn(FAIL, transcript)
        self.assertIn(PASS, transcript)
        # `events: run <path> status <s> before <n> after <n> counts <n>
        # missed <n>-><n>`.
        summary = [part for part in transcript.splitlines()
                   if "events: run " in part and " status " in part][-1]
        (status, before, after, counts, missed_before,
         missed_after) = [int(value) for value in re.findall(r"-?\d+",
                                                            summary)]
        self.assertGreaterEqual(status, 0)
        # While the parent is suspended its events were dropped, not
        # delivered: the misses accumulated over the child's run, and the
        # parent's own backlog that was waiting stays accounted for.
        self.assertGreater(missed_after, missed_before)
        self.assertGreaterEqual(counts, after)
        # The child writes through its own stdout while the parent is
        # suspended; every delivery to the parent would print an "E"
        # there, so the whole child section must be free of one.  (The
        # deliveries the parent resumes with land after its last line,
        # where the summary's own line carries them.)
        first = transcript.index("fs-child:")
        child = transcript[first:]
        last = child.rindex("fs-child:")
        self.assertGreater(last, 0)
        self.assertNotIn("E", child[:last])
        self.assertIn("fs-child: stdout ok", child)

    def test_exit_releases_the_subscription(self):
        z = self.boot()[0]
        transcript = self.command(z, "X /system/events.exe exit")
        self.assertIn("events: exit armed", transcript)
        # The subscription went away with the process: the source stops
        # and the row is free for the next program.
        again = self.command(z, "X /system/events.exe watch")
        self.assertNotIn(FAIL, again)
        self.assertIn(PASS, again)


if __name__ == "__main__":
    unittest.main()
