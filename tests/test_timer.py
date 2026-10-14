#!/usr/bin/env python3
"""Timer acceptance: the TIMER driver contract and its production replay.

Host checks pin what a replay cannot see:

* the channel-2 programming constants (port 42h, the 156.25 Hz control
  word decoded bit by bit from its named constants, the time constant,
  the shared IM2 vector base) and the protocol constants (the TIMER
  class and operations, the INFO values);
* the emulated CTC's two modes: timer mode divides the CPU clock by the
  prescaler, counter mode counts the channel's CLK/TRG input at the
  board's rates - the mode bit the board's `E5h` defect turned on;
* the read's interrupt discipline (the driver saves the caller's state,
  opens for the read and restores it) and `timer_wait_us`'s bounded
  poll, which gives up on a clock that stops advancing;
* the vector page's channel-2 entry, without which a CTC2 interrupt
  cannot reach the driver;
* the `now` composition - tick count, counter, tick count, retry - as a
  model with its retry and wrap cases, and the driver source's own read
  order;
* `timer_us`'s 25.6 µs scaling and its documented range;
* the manifest, the resolved endpoint, the publication and the two
  system files that select the driver.

The replay boots the production image with the emulated CTC attached, so
the driver's channel-2 interrupt really fires, and runs
`/system/timer.exe stage` at the shell: the INFO values, 10,000
monotonic readings with ticks arriving between them, and a 1 s advance.
It measures the interrupt's per-delivery T-state cost (gate, dispatch
and trampoline, handler, return) and, in a second session on a card, the
masked windows of a 512-byte bulk read - the longest that carries
IOC-driver code, the longest in the whole operation, and the ticks the
CTC's single request latch swallowed.  The driver's own tick count is
compared with the CTC model's deliveries throughout.

Skipped when the production image has not been built.
"""

import itertools
import json
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from maptools import evaluate, parse_c_header  # noqa: E402
from replay import SHELL_PROMPT  # noqa: E402
from z80sim import Ctc, IocCommandPeer, opcode_cost  # noqa: E402
from test_hwtest_sim import (HwtestReplay, KEEP_CONTENT,  # noqa: E402
                             KEEP_NAME)

HARDWARE_H = os.path.join(ROOT, "platform", "zephyr80", "public",
                          "hardware.h")
PROTOCOL_H = os.path.join(ROOT, "abi", "kos", "protocol.h")
OBJECT_H = os.path.join(ROOT, "abi", "kos", "object.h")
IRQ_INIT = os.path.join(ROOT, "kernel", "irq", "init.asm")
IRQ_ENTRY = os.path.join(ROOT, "kernel", "irq", "entry.asm")
IRQ_H = os.path.join(ROOT, "platform", "zephyr80", "private", "irq.h")
DRIVER_C = os.path.join(ROOT, "drivers", "timer", "timer.c")
TIMER_IO_ASM = os.path.join(ROOT, "drivers", "timer", "timer_io.asm")
TIMER_PROGRAM = os.path.join(ROOT, "tests", "timer", "timer.c")
RUNNER_C = os.path.join(ROOT, "tests", "hwtest", "hwtest.c")
ERRNO_H = os.path.join(ROOT, "abi", "kos", "errno.h")
MANIFEST = os.path.join(ROOT, "drivers", "timer", "timer.json")
SDK_TIMER_H = os.path.join(ROOT, "user", "sdk", "include", "kos",
                           "timer.h")
SDK_TIMER_C = os.path.join(ROOT, "user", "sdk", "src", "timer.c")
PRODUCTION_SYSTEM = os.path.join(ROOT, "systems", "production.json")
CONSOLE_SERIAL_SYSTEM = os.path.join(ROOT, "systems", "console-serial.json")
PLAN = os.path.join(ROOT, "build", "modules", "drivers.plan.json")
TIMER_REPORT = os.path.join(ROOT, "build", "modules", "timer.report.json")
IOC_REPORT = os.path.join(ROOT, "build", "modules", "ioc.report.json")
STORE = os.path.join(ROOT, "build", "store.json")

# One CTC count: the system clock through the prescaler, in T-states; one
# tick is the channel's time constant in counts.
COUNT_TSTATES = 256
TICK_COUNTS = 250
# A tick in T-states: 6.4 ms of the 10 MHz clock.
TICK_TSTATES = COUNT_TSTATES * TICK_COUNTS

# The masked-window budget: the CTC holds one pending interrupt, so a
# masked window longer than two ticks (12.8 ms) loses one.
MASK_BUDGET_TSTATES = 2 * TICK_TSTATES

# `timer card`'s fixture: 64 KiB, the smallest file the correction's
# timing row is specified over.  The emulated card lane serves it in
# 512-byte blocks, so this is the suite's longest replay row (about
# 1.5 minutes of wall clock).
CARD_BYTES = 64 * 1024
CARD_STEP_LIMIT = 40000000000

# The stage transcript's numbers: one second is 39,062.5 counts, and the
# bound allows the poll period's overshoot (±250 counts).
SECOND_COUNTS = 39062
SECOND_SLACK = 250
# The replay stops when the transcript carries the marker, which is a
# couple of console writes after the `now` line it checks: the ticks in
# that tail are what the live count can run ahead by (200 ms).
PRINT_TAIL_TICKS = 32


def read(path):
    with open(path, "r", errors="replace") as handle:
        return handle.read()


def code_only(text):
    """Strip comments and collapse whitespace so source checks see the
    code alone, whatever the assembler's tab layout."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return re.sub(r"[ \t]+", " ", text)


class TestTimerContract(unittest.TestCase):
    """The constants, the source structure and the manifest."""

    def test_the_channel_programming_constants(self):
        env = parse_c_header(HARDWARE_H)
        # The timer is physical channel 2, and this board's crossed
        # select wiring answers channel 2 at 41h (channel 1 at 42h):
        # one port off, the tick vectors 02h and the handler never runs.
        self.assertEqual(evaluate(env["KOS_CTC2_PORT"], env), 0x41)
        self.assertEqual(evaluate(env["KOS_CTC1_PORT"], env), 0x42)
        self.assertEqual(evaluate(env["KOS_CTC0_PORT"], env), 0x40)
        self.assertEqual(evaluate(env["KOS_CTC3_PORT"], env), 0x43)
        self.assertEqual(evaluate(env["KOS_CTC2_TIME_CONSTANT"], env), 250)
        self.assertEqual(evaluate(env["KOS_CTC0_VECTOR_BASE"], env), 0x00)

        # The control word is composed from one named constant per bit,
        # so the decode below is the source's own.  One bit is the
        # board-critical one: D6 selects counter mode, which counts the
        # channel's CLK/TRG input instead of the system clock - channel
        # 2's runs at 7.3728 MHz, so the same time constant interrupts
        # 189x too fast and livelocks the CPU in the handler.
        bits = {"INTERRUPT_ENABLE": 0x80, "MODE_COUNTER": 0x40,
                "PRESCALER_256": 0x20, "EDGE_RISING": 0x10,
                "TRIGGER_EXTERNAL": 0x08, "TIME_CONSTANT": 0x04,
                "SOFT_RESET": 0x02, "CONTROL_WORD": 0x01}
        for name, value in sorted(bits.items()):
            self.assertEqual(evaluate(env["KOS_CTC_CTRL_" + name], env),
                             value, name)

        control = evaluate(env["KOS_CTC2_CONTROL_TIMER"], env)
        self.assertEqual(control, 0xA5, "the programmed control word")
        self.assertEqual(control & bits["INTERRUPT_ENABLE"],
                         bits["INTERRUPT_ENABLE"], "interrupt enable")
        self.assertEqual(control & bits["MODE_COUNTER"], 0x00,
                         "timer mode: D6 clear")
        self.assertEqual(control & bits["PRESCALER_256"],
                         bits["PRESCALER_256"], "prescaler 256")
        self.assertEqual(control & bits["EDGE_RISING"], 0x00,
                         "falling edge")
        self.assertEqual(control & bits["TRIGGER_EXTERNAL"], 0x00,
                         "automatic trigger (the prescaler's own clock)")
        self.assertEqual(control & bits["TIME_CONSTANT"],
                         bits["TIME_CONSTANT"], "time constant follows")
        self.assertEqual(control & bits["SOFT_RESET"], 0x00,
                         "no software reset")
        self.assertEqual(control & bits["CONTROL_WORD"],
                         bits["CONTROL_WORD"], "control word, not a vector")

        # The reset and the CTC0 test word are composed the same way.
        self.assertEqual(evaluate(env["KOS_CTC_RESET"], env),
                         bits["SOFT_RESET"] | bits["CONTROL_WORD"])
        self.assertEqual(evaluate(env["KOS_CTC0_CONTROL_TEST"], env),
                         bits["INTERRUPT_ENABLE"] | bits["PRESCALER_256"] |
                         bits["TIME_CONSTANT"] | bits["SOFT_RESET"] |
                         bits["CONTROL_WORD"])
        # The model's D6-only difference: the two rates.
        self.assertEqual(10000000 // COUNT_TSTATES // TICK_COUNTS, 156)
        self.assertEqual(7372800 // TICK_COUNTS, 29491)

    def test_the_protocol_constants(self):
        env = parse_c_header(PROTOCOL_H)
        object_env = parse_c_header(OBJECT_H)
        self.assertEqual(evaluate(env["KTIMER_OP_NOW"], env), 0)
        self.assertEqual(evaluate(env["KTIMER_OP_INFO"], env), 1)
        timer_class = evaluate(env["KOS_CLASS_TIMER"], env)
        self.assertEqual(timer_class,
                         evaluate(object_env["KOS_TYPE_TIMER"], object_env))
        self.assertEqual(timer_class, 10)
        # The published constants are the clock's: the count rate is the
        # 10 MHz clock through the 256 prescaler for 10 s, and one tick
        # is the programmed time constant.
        hardware = parse_c_header(HARDWARE_H)
        self.assertEqual(evaluate(env["KOS_TIMER_COUNTS_PER_TICK"], env),
                         evaluate(hardware["KOS_CTC2_TIME_CONSTANT"],
                                  hardware))
        self.assertEqual(evaluate(env["KOS_TIMER_COUNTS_PER_10S"], env),
                         10 * 10000000 // COUNT_TSTATES)

    def test_the_im2_page_installs_the_channel_two_vector(self):
        # Base 00h: channel 2 requests vector byte 04h, so the page must
        # hold the stub there or the interrupt would be treated as
        # unexpected (and the channel quiesced).
        text = code_only(read(IRQ_INIT))
        self.assertIn("irq_ctc2_stub", text)
        self.assertIn("ld (KOS_IRQ_VECTOR_BASE + 0x0004),hl", text)
        # And accepting an interrupt cleared IFF1/IFF2, so the exit must
        # re-enable interrupts before RETI or the stream would stop after
        # the first tick.
        entry = code_only(read(IRQ_ENTRY))
        exit_path = entry[entry.rindex("pop af"):]
        self.assertLess(exit_path.index("ei"), exit_path.index("reti"))

    def test_the_driver_source_reads_ticks_counter_ticks(self):
        body = code_only(read(DRIVER_C))
        first = body.index("before = timer_ticks")
        counter = body.index("down = (uint16_t)timer_ctc2_port")
        second = body.index("after = timer_ticks")
        self.assertLess(first, counter)
        self.assertLess(counter, second)
        self.assertIn("if (before == after)", body)
        # The composition scales the tick count without a multiply
        # runtime: 250 * t = 256 * t - 4 * t - 2 * t, plus the
        # counter's remaining counts.
        self.assertIn("(ticks << 8) - (ticks << 2) - (ticks << 1)", body)
        self.assertIn("KOS_CTC2_TIME_CONSTANT - down", body)
        # The read keeps the caller's interrupt state (correction 02):
        # the token is saved before the loop and put back after it, and
        # the function carries no inline `ei`/`di` of its own.
        read_now = body[body.index("uint32_t timer_counts_now"):]
        read_now = read_now[:read_now.index("\n}")]
        self.assertLess(read_now.index("state = timer_irq_save_enable()"),
                        read_now.index("before = timer_ticks"))
        self.assertLess(read_now.index("after = timer_ticks"),
                        read_now.index("timer_irq_restore(state)"))
        self.assertNotIn("__asm", read_now)

        # The handler's whole body is the tick.
        handler = body[body.index("void timer_irq_handler"):]
        handler = handler[:handler.index("\n}")]
        self.assertIn("timer_ticks = timer_ticks + 1u", handler)

    def test_the_now_model_retries_and_wraps(self):
        """The read sequence as a model: a torn attempt is retried, the
        readings are monotonic across a tick, and elapsed values survive
        the 32-bit wrap."""
        def compose(ticks, down):
            return (ticks * TICK_COUNTS + (TICK_COUNTS - down)) & 0xFFFFFFFF

        def read_now(samples):
            # ticks, counter, ticks: the driver's loop over a machine
            # that yields one observation per access.
            while True:
                before = next(samples)
                down = next(samples)
                after = next(samples)
                if before == after:
                    return compose(before, down)

        # Straight readings inside one tick, then the retry when a tick
        # lands between the two tick reads.
        self.assertEqual(read_now(iter([5, 250, 5])), 5 * 250)
        self.assertEqual(read_now(iter([5, 1, 5])), 5 * 250 + 249)
        self.assertEqual(read_now(iter([5, 1, 6, 6, 250, 6])), 6 * 250)
        # Monotonic across a tick boundary and across the wrap: the
        # counter's 250 values then the next tick's zero.
        sequence = [compose(5, down) for down in range(250, 0, -1)]
        sequence.append(compose(6, 250))
        self.assertTrue(all(b > a for a, b in zip(sequence, sequence[1:])))
        # The wrap: the last counts of the final tick, then tick 0.  The
        # readings stay one count apart in unsigned arithmetic - the whole
        # point of composing now without a sign.
        wrap = [compose(0xFFFFFFFF, down) for down in range(250, 0, -1)]
        wrap.append(compose(0, 250))
        self.assertEqual(len(wrap), 251)
        self.assertTrue(all((b - a) & 0xFFFFFFFF == 1
                            for a, b in zip(wrap, wrap[1:])))

    def test_timer_us_scales_at_25_6_us(self):
        def timer_us(counts):
            return counts * 25 + counts * 3 // 5

        for counts in (1, 250, 39062, 390625, 1000000):
            exact = counts * 25.6
            self.assertLessEqual(timer_us(counts), exact)
            self.assertLess(exact - timer_us(counts), 1)
        # The documented range: 71 minutes (166,406,250 counts) keeps
        # every partial product inside 32 bits (2**32 / 25).
        counts = 71 * 60 * 390625 // 10  # 71 minutes in counts
        self.assertLessEqual(counts * 25, 0xFFFFFFFF)
        self.assertLessEqual(counts * 3, 0xFFFFFFFF)
        # The SDK implements the scaling with shifts and one division,
        # because the freestanding library has no multiply helper.
        body = code_only(read(SDK_TIMER_C))
        self.assertIn("(counts << 4) + (counts << 3) + counts", body)
        self.assertIn("(((counts << 1) + counts) / 5u)", body)
        self.assertNotIn("counts *", body)
        header = read(SDK_TIMER_H)
        for name in ("timer_open", "timer_now", "timer_us", "timer_wait_us"):
            self.assertIn(name, header)

    def test_the_interrupt_token_helpers_keep_the_callers_state(self):
        """`drivers/timer/timer_io.asm`: the read's save-enable/restore
        pair.  A syscall body runs with the caller's interrupt state
        (correction 01), so the driver opens for its critical read and
        puts that state back instead of forcing one."""
        text = re.sub(r"[ \t]+", " ", read(TIMER_IO_ASM))
        save = text[text.index("_timer_irq_save_enable::"):
                    text.index("_timer_irq_restore::")]
        self.assertIn("ld a,i", save)      # P/V = IFF2: the saved state
        self.assertIn("ei", save)          # open for the read
        restore = text[text.index("_timer_irq_restore::"):]
        self.assertIn("di", restore)       # token 0 puts the mask back
        self.assertIn("ei", restore)
        # The build carries it: the module's objects and the one rule
        # that assembles it.
        makefile = read(os.path.join(ROOT, "Makefile"))
        self.assertIn("$(MODULES)/timer_io.rel: drivers/timer/timer_io.asm",
                      makefile)
        manifest = json.loads(read(MANIFEST))
        self.assertIn("timer_io.rel", manifest["objects"])

    def test_wait_us_gives_up_on_a_frozen_clock(self):
        """`timer_wait_us` bounds its poll: one tick (250 counts) has to
        arrive within 100,000 consecutive readings, so a dead or frozen
        clock returns -KOS_ETIMEDOUT instead of hanging the caller - and
        both the stopwatch run and the exerciser report that."""
        sdk = code_only(read(SDK_TIMER_C))
        self.assertIn("TIMER_STALL_READS", sdk)
        self.assertIn("KOS_TIMER_COUNTS_PER_TICK", sdk)
        self.assertIn("-KOS_ETIMEDOUT", sdk)
        errno = parse_c_header(ERRNO_H)
        timeout = evaluate(errno["KOS_ETIMEDOUT"], errno)
        header = read(SDK_TIMER_H)
        self.assertIn("int timer_wait_us(int handle, uint32_t us);", header)

        def wait_us(readings, us):
            # The poll loop as a model: us * 5 / 128 counts, the stall
            # counter reset by every reading that moved a whole tick.
            start = next(readings)
            counts = (us * 5) // 128
            last = start
            stalled = 0
            for now in readings:
                if now - start >= counts:
                    return 0
                if now - last >= TICK_COUNTS:
                    last = now
                    stalled = 0
                else:
                    stalled += 1
                    if stalled >= 100000:
                        return -timeout
            raise AssertionError("the wait never returned")

        # 6.4 ms is exactly one tick in counts.
        self.assertEqual((6400 * 5) // 128, TICK_COUNTS)
        # Frozen: the first 100,000 readings that saw no tick give up.
        frozen = itertools.chain([1000], itertools.repeat(1000))
        self.assertEqual(wait_us(frozen, 6400), -timeout)
        # Alive but slow (a tick every 50,000 readings): the counter
        # resets on each tick, so the wait is served - the bound is a
        # dead clock's, not a slow one's.
        def slow():
            value = 0
            for index in itertools.count():
                yield value
                if index % 50000 == 49999:
                    value += TICK_COUNTS
        self.assertEqual(wait_us(slow(), 6400), 0)
        # The callers report the give-up rather than hanging.
        program = code_only(read(TIMER_PROGRAM))
        self.assertIn("timer_wait_us(handle, SECOND_US)", program)
        self.assertIn('"timer: FAIL wait %d', program)
        runner = code_only(read(RUNNER_C))
        self.assertIn("timer_wait_us(handle, TIMER_WAIT_US)", runner)
        self.assertIn('"wait %d"', runner)

    def test_the_manifest_publication_and_composition(self):
        manifest = json.loads(read(MANIFEST))
        self.assertEqual(manifest["schema"], "kos-driver-1")
        self.assertEqual(manifest["name"], "timer")
        self.assertEqual(manifest["init"], "timer_driver_init")
        self.assertEqual(manifest["objects"],
                         ["timer.rel", "timer_io.rel", "timer_driver.rel",
                          "timer_boot.rel", "sdcc_indirect_calls.rel"])
        segments = {segment["role"]: segment
                    for segment in manifest["segments"]}
        self.assertEqual(sorted(segments), ["boot", "hot"])
        self.assertEqual(segments["boot"]["entry"], "timer_boot_entry")
        self.assertEqual(segments["boot"]["image_areas"], ["_BOOT"])
        self.assertEqual(segments["hot"]["bss_areas"], ["_DRIVER_IO"])
        self.assertEqual(manifest["endpoints"],
                         [{"class": 10, "instance": 0, "version": 1,
                           "dispatch": "timer_driver_dispatch",
                           "flags": 0}])
        self.assertEqual(manifest["publishes"],
                         [{"path": "/dev/timer", "type": "TIMER",
                           "endpoint": {"class": 10, "instance": 0},
                           "object": 0}])
        self.assertEqual(manifest["depends"], [])

        # The two console builds select it (production is the V9958/HID
        # console; the serial build selects the same driver set); it
        # loads after the drivers it shares the bank with.
        for path in (PRODUCTION_SYSTEM, CONSOLE_SERIAL_SYSTEM):
            system = json.loads(read(path))
            self.assertIn("timer", system["drivers"], path)
            self.assertIn("timer.exe", system["store"], path)
            # Before the filesystem provider: nothing but data and
            # programs loads after `iocfat` (tests/test_iocfat.py).
            self.assertLess(system["drivers"].index("timer"),
                            system["drivers"].index("iocfat"))

    def test_the_resolved_endpoint_and_store(self):
        for path in (PLAN, TIMER_REPORT, STORE):
            if not os.path.exists(path):
                raise unittest.SkipTest("%s has not been built" % path)
        plan = json.loads(read(PLAN))
        self.assertIn("timer.mod", plan["order"])
        self.assertEqual(plan["order"][-1], "iocfat.mod")
        entry = [item for item in plan["drivers"]
                 if item["name"] == "timer.mod"][0]
        self.assertEqual(entry["dependencies"], [])
        # The manifest numbers an endpoint by its driver's place in the
        # load order; the TIMER class (10) is the one it registers.
        number = plan["order"].index("timer.mod") + 1
        self.assertEqual([(endpoint["class"], endpoint["number"])
                          for endpoint in entry["endpoints"]], [(10, number)])
        report = json.loads(read(TIMER_REPORT))
        self.assertEqual([(endpoint["class"], endpoint["number"])
                          for endpoint in report["endpoints"]], [(10, number)])
        self.assertEqual([(publication["name"], publication["type"])
                          for publication in report["publications"]],
                         [("/timer", 10)])
        # The tick count is the image's only run-time data, so the hot
        # segment's BSS is exactly the u32 a replay reads.
        hot = [segment for segment in report["segments"]
               if segment["role"] == "hot"][0]
        self.assertEqual(hot["bss_size"], 4)
        self.assertIn("_timer_ticks", report["symbol_segments"])
        store = json.loads(read(STORE))
        names = [item["name"] for item in store["files"]]
        self.assertIn("timer.mod", names)
        self.assertIn("timer.exe", names)


class TestTheCtcModelHonoursTheMode(unittest.TestCase):
    """The emulated CTC's counting (`tests/z80sim.py`, class `Ctc`).

    The model drives every IRQ replay, and its mode bit is what the
    board's defect turned on: a channel in counter mode counts its
    CLK/TRG input, not the CPU clock, so the control word that is a
    156.25 Hz timer becomes a ~29.5 kHz interrupt storm.  A model that
    ignores D6 lets that defect pass every replay; these cases fail on
    it."""

    # The system clock the T-state base runs at, and the channel trigger
    # inputs the board's 14.7456 MHz oscillator divider feeds (Z80
    # Peripherals Controller, the CTC trigger table).
    CLOCK_HZ = 10000000
    TRIGGER_HZ = (1843200, 3686400, 7372800, 7372800)

    def expiries_per_second(self, control, constant, channel=2):
        """One second of the model's clock, counting expiries."""
        ctc = Ctc()
        ctc.write(Ctc.PORTS[0], 0x00)              # the IM2 vector base
        ctc.write(Ctc.PORTS[channel], control)     # the control word
        ctc.write(Ctc.PORTS[channel], constant)    # the constant: starts
        clock = 0
        while clock < self.CLOCK_HZ:
            ctc.advance(1000)
            clock += 1000
        return ctc.expiries[channel]

    def test_timer_mode_counts_the_cpu_clock_through_the_prescaler(self):
        # The driver's own word: 10 MHz / 256 / 250 = 156.25 Hz.
        self.assertEqual(self.expiries_per_second(0xA5, 250), 156)
        # Prescaler 16 (D5 clear): 10 MHz / 16 / 250 = 2500 Hz.
        self.assertEqual(self.expiries_per_second(0x85, 250), 2500)
        # A time constant of 0 means 256 counts: 152.6 Hz.
        self.assertEqual(self.expiries_per_second(0xA5, 0),
                         self.CLOCK_HZ // 256 // 256)

    def test_counter_mode_counts_the_channels_trigger_input(self):
        # 0xE5 - the board's defect word: D6 set, so channel 2 counts
        # CLK/TRG, which the divider drives at 7.3728 MHz.  7.3728 MHz /
        # 250 = 29,491 interrupts/s, 189x the tick: the CPU never leaves
        # the handler.
        self.assertEqual(self.expiries_per_second(0xE5, 250),
                         self.TRIGGER_HZ[2] // 250)
        self.assertEqual(self.TRIGGER_HZ[2] // 250, 29491)
        self.assertEqual(self.TRIGGER_HZ[2] // 250 //
                         (self.CLOCK_HZ // 256 // 250), 189)
        # Every channel counts its own input, and the prescaler divides
        # the CPU clock only, so it changes nothing in counter mode.
        self.assertEqual(self.expiries_per_second(0xC5, 250, channel=0),
                         self.TRIGGER_HZ[0] // 250)
        self.assertEqual(self.expiries_per_second(0xE5, 250, channel=1),
                         self.TRIGGER_HZ[1] // 250)
        self.assertEqual(self.expiries_per_second(0xE5, 250),
                         self.expiries_per_second(0xC5, 250))

    def test_the_model_wires_the_selects_like_the_board(self):
        # The model's map and the header's named ports are the same
        # wiring: a replay only catches a wrong port if they agree.
        env = parse_c_header(HARDWARE_H)
        ports = tuple(evaluate(env["KOS_CTC%d_PORT" % channel], env)
                      for channel in range(4))
        self.assertEqual(Ctc.PORTS, ports)
        self.assertEqual(Ctc.PORTS, (0x40, 0x42, 0x41, 0x43))
        self.assertEqual(Ctc().channel_of_port(0x41), 2)
        self.assertEqual(Ctc().channel_of_port(0x42), 1)

    def test_the_wrong_port_leaves_the_timer_at_zero(self):
        """The board's defect word on the port that looks like channel 2:
        the channel 1 counter runs and the timer's own channel - the one
        the stub at vector 04h answers - never expires, so the driver's
        tick count stays 0 while a port read still moves."""
        ctc = Ctc()
        ctc.write(Ctc.PORTS[0], 0x00)
        ctc.write(0x42, 0xA5)          # the timer's word, channel 1's port
        ctc.write(0x42, 250)
        for _ in range(10000):
            ctc.advance(1000)          # one second of the model's clock
        self.assertEqual(ctc.expiries[2], 0, "the timer's channel ticked")
        self.assertEqual(ctc.expiries[1], 156, "channel 1 did not tick")
        # The vectors are what makes it invisible: the executive holds a
        # stub for the timer's channel only.
        self.assertEqual(ctc.vector(1), 0x02)
        self.assertEqual(ctc.vector(2), 0x04)
        self.assertNotEqual(ctc.vector(1), ctc.vector(2))

    def test_the_model_reads_the_bit_the_way_the_board_does(self):
        # The same channel, one bit apart: D6 clear is the clocked timer,
        # D6 set the input counter.  Neither may be idle - a replay of a
        # counter-mode program has to see its interrupts.
        timer = self.expiries_per_second(0xA5, 250)
        counter = self.expiries_per_second(0xE5, 250)
        self.assertGreater(timer, 0)
        self.assertGreater(counter, 0)
        self.assertGreater(counter, timer * 100)


class IrqCostProbe:
    """Per-delivery T-state spans of the timer interrupt.

    The step hook runs before each instruction, so the instruction's
    whole cost lands in the phase its PC belongs to:
      gate      the vector stub and the common entry up to the dispatch;
      dispatch  the C dispatcher and the driver trampoline up to the
                handler's first instruction;
      handler   the driver handler itself (its window is mapped);
      return    the nucleus exit, up to and including the RETI.
    The CPU's 19-T-state interrupt acceptance is not part of a span.
    """

    def __init__(self, syms, handler):
        self.stub = syms["irq_ctc2_stub"]
        self.dispatch = syms["_irq_dispatch"]
        self.handler = handler
        self.deliveries = []
        self.current = None
        self.phase = None

    def __call__(self, z):
        if self.current is None:
            if z.pc != self.stub:
                return
            self.current = {"gate": 0, "dispatch": 0, "handler": 0,
                            "return": 0}
            self.phase = "gate"
        cost = opcode_cost(z)
        self.current[self.phase] += cost
        if self.phase == "gate" and z.pc == self.dispatch:
            self.phase = "dispatch"
        elif self.phase == "dispatch" and z.pc == self.handler:
            self.phase = "handler"
        elif self.phase == "handler" and z.pc != self.handler and \
                (z.latch & 0x18) == 0x18 and (z.latch & 0xE0) == 0x00:
            self.phase = "return"
        if z.rd(z.pc) == 0xED and z.rd((z.pc + 1) & 0xFFFF) == 0x4D:
            self.deliveries.append(self.current)
            self.current = None

    def mean(self):
        assert self.deliveries, "no delivery was traced"
        totals = [sum(delivery.values()) for delivery in self.deliveries]
        return sum(totals) / len(totals)

    def mean_phase(self, phase):
        return sum(delivery[phase] for delivery in self.deliveries) / \
            len(self.deliveries)


class CountsProbe:
    """The T-state of every `_timer_counts_now` entry: a program's own
    readings bracket the work between them, so two entries give the
    model's clock for exactly the span the program timed."""

    def __init__(self, entry):
        self.entry = entry
        self.calls = []

    def __call__(self, z):
        if z.pc == self.entry:
            self.calls.append(z.tstates)


class MaskWindowProbe:
    """The intervals with maskable interrupts disabled, each described by
    where it started, whether it carried IOC-driver code and the driver
    address range it touched.

    The IOC driver's hot image only maps while the window shows the
    driver page (mode 11, P = 1, latch 38h); the same addresses are the
    nucleus' cold half in the OS window, so the driver-page latch is part
    of the test.  With the correction the gate gives a syscall body the
    caller's interrupt state, so a window that runs long is the work of
    an explicit masking section (the IOC frame exchange and bulk
    receive), and its driver address range names it."""

    DRIVER_WINDOW = 0x38

    def __init__(self, base, end):
        self.base = base
        self.end = end
        self.start = None
        self.spans = []

    def __call__(self, z):
        if not z.iff1:
            if self.start is None:
                self.start = {"start": z.tstates, "start_pc": z.pc,
                              "driver": False, "driver_lo": None,
                              "driver_hi": None,
                              "duration": 0, "end_pc": None}
            if (self.base <= z.pc < self.end and
                    (z.latch & 0xF8) == self.DRIVER_WINDOW):
                self.start["driver"] = True
                low = self.start["driver_lo"]
                self.start["driver_lo"] = z.pc if low is None \
                    else min(low, z.pc)
                high = self.start["driver_hi"]
                self.start["driver_hi"] = z.pc if high is None \
                    else max(high, z.pc)
        elif self.start is not None:
            self._close(z)

    def _close(self, z):
        self.start["duration"] = z.tstates - self.start["start"]
        self.start["end_pc"] = z.pc
        self.spans.append(self.start)
        self.start = None

    def close(self, z):
        if self.start is not None:
            self._close(z)

    def duration(self, span):
        return span["duration"]

    def longest(self, ioc=None):
        """The longest window, optionally only those that carried IOC
        driver code (`ioc=True`) or those that did not (`ioc=False`)."""
        spans = [span["duration"] for span in self.spans
                 if ioc is None or span["driver"] == ioc]
        return max(spans) if spans else 0

    def longest_span(self, ioc=None):
        """The longest window itself (a span dict), or None."""
        spans = [span for span in self.spans
                 if ioc is None or span["driver"] == ioc]
        return max(spans, key=self.duration) if spans else None


class TimerSession(HwtestReplay):
    """The production image's timer sessions: the packed driver's own
    addresses plus the emulated CTC the base boots with."""

    def setUp(self):
        self.load_image()

    # The common-memory stacks a delivery may legitimately find in use
    # (symbols of each stack's bottom and top, from the map).  The IRQ
    # gate saves 18 bytes at [KOS_IRQ_STACK_TOP-18, KOS_IRQ_STACK_TOP):
    # the boot/common stack's base is that top, so a chain on it (SP at
    # or above the base) is safe by construction, and these three sit
    # entirely below the reservation - the gate stack, where syscall
    # bodies run now that the gate gives them the caller's interrupt
    # state, and the two copy primitives, whose stacks are declared and
    # sized for exactly this push.
    STACK_SYMBOLS = (("kos_gate_stack", "kos_gate_stack_top"),
                     ("_ipc_ap_stack", "_ipc_ap_stack_top"),
                     ("kos_copy_stack", "kos_copy_stack_top"))

    def make_cpu(self, *args, **kwargs):
        """The base machine, with every delivered interrupt checked
        against the IRQ transition reservation.

        A delivery whose SP is in common memory below the reservation
        and outside every declared common stack would have the gate's
        save land on frames no stack owns - the shape of the boot-stack
        overlap the package found (`docs/validation/2026-10-13-m12-timer.md`),
        where a chain's frames and the reservation were the same bytes.

        Deliveries taken with SP on the gate stack are counted
        (`body_deliveries`): they are interrupts inside a syscall body,
        which the correction makes possible and the replays assert."""
        z = super().make_cpu(*args, **kwargs)
        irq_header = parse_c_header(IRQ_H)
        top = evaluate(irq_header["KOS_IRQ_STACK_TOP"], irq_header)
        regions = [(self.syms[low], self.syms[high])
                   for low, high in self.STACK_SYMBOLS]
        gate = (self.syms["kos_gate_stack"], self.syms["kos_gate_stack_top"])
        self.body_deliveries = 0
        accept = z._accept_interrupt

        def guarded():
            sp = z.sp
            if gate[0] <= sp <= gate[1] and \
                    (z.latch & 0xF8) != MaskWindowProbe.DRIVER_WINDOW:
                # SP on the gate stack: a syscall body was running, and
                # not inside a driver's own enable window.
                self.body_deliveries += 1
            if 0xE000 <= sp < top and not any(
                    low <= sp <= high for low, high in regions):
                raise AssertionError(
                    "IRQ delivered at SP %04X: below the common stack's "
                    "base %04X and outside every declared common stack "
                    "%s, so the gate's save would land on frames no stack "
                    "owns" % (sp, top, ", ".join(
                        "%04X-%04X" % region for region in regions)))
            accept()

        z._accept_interrupt = guarded
        return z

    def plan_entry(self):
        plan = json.loads(read(PLAN))
        return [item for item in plan["drivers"]
                if item["name"] == "timer.mod"][0]

    def plan_timer_endpoint(self):
        plan = json.loads(read(PLAN))
        return [endpoint for endpoint in
                [item for item in plan["drivers"]
                 if item["name"] == "timer.mod"][0]["endpoints"]
                if endpoint["class"] == 10][0]["number"]

    def driver_address(self, symbol):
        report = json.loads(read(TIMER_REPORT))
        entry = self.plan_entry()
        role = report["symbol_segments"][symbol]
        bases = {"hot": entry["hot_base"], "boot": entry["boot_base"],
                 "io": entry["io_base"] or 0,
                 "bulk": entry["bulk_base"] or 0}
        return bases[role] + report["symbols"][symbol]

    def live_ticks(self, z):
        address = self.driver_address("_timer_ticks")
        return int.from_bytes(bytes(z.banks[6][address:address + 4]),
                              "little")

    def ioc_entry(self):
        plan = json.loads(read(PLAN))
        return [item for item in plan["drivers"]
                if item["name"] == "ioc.mod"][0]

    def ioc_report(self):
        return json.loads(read(IOC_REPORT))

    def ioc_hot_range(self):
        entry = self.ioc_entry()
        report = self.ioc_report()
        hot = [segment for segment in report["segments"]
               if segment["role"] == "hot"][0]
        self.assertEqual(hot["base"], entry["hot_base"])
        return hot["base"], hot["base"] + hot["image_size"]

    def ioc_symbols(self):
        """The IOC driver's hot-image symbols as (address, name),
        address-sorted: what names the code a masked window touched."""
        base = self.ioc_entry()["hot_base"]
        report = self.ioc_report()
        return sorted((base + report["symbols"][name], name)
                      for name, role in report["symbol_segments"].items()
                      if role == "hot")

    def ioc_symbol_at(self, address):
        """`name+offset` for the driver symbol at or before `address`."""
        best = None
        for start, name in self.ioc_symbols():
            if start <= address:
                best = (start, name)
            else:
                break
        if best is None:
            return "%04X" % address
        return "%s+%d" % (best[1].lstrip("_"), address - best[0])

    def describe_span(self, span):
        """Where a masked window sat: its first PC, and the driver code
        range it touched when it touched any."""
        if span is None:
            return "none"
        where = "%04X" % span["start_pc"]
        if span["driver_lo"] is not None:
            where += " (%s .. %s)" % (self.ioc_symbol_at(span["driver_lo"]),
                                      self.ioc_symbol_at(span["driver_hi"]))
        return where

    def assert_interruptible_body(self):
        """The correction's own signature: an interrupt taken with SP in
        the gate stack is one taken *inside* a syscall body.  Before the
        correction every body ran masked, so the only acceptances on the
        gate stack were inside the timer driver's own enable window (the
        driver-page latch); one outside it is a body the executive now
        lets through."""
        print("timer: %d deliveries interrupted a syscall body"
              % self.body_deliveries)
        self.assertGreater(self.body_deliveries, 0)

    def run_with(self, z, line, expect, since, probe=None, limit=None):
        """Type `line` at the shell and run to `expect`, driving `probe`
        before every step."""
        z.hid.hid_queue.extend(line.encode("ascii") + b"\r")
        for index in range(limit or 60000000):
            self.skip_delay(z)
            self.feed(z)
            if probe is not None:
                probe(z)
            z.step()
            if index % 0x1FFF == 0:
                self.pump(z)
                if expect in "".join(z.glyphs[since:]):
                    return index
        self.pump(z)
        raise AssertionError(
            "did not reach %r (PC=%04X, tail=%r)"
            % (expect, z.pc, "".join(z.glyphs)[since:][-160:]))


class TestTimerReplay(TimerSession):
    """`timer stage` in the production image, with the CTC ticking."""

    def test_the_stage_checks_the_clock_and_the_latches_match(self):
        z = self.start(card=False)
        ctc = z.ctc
        probe = IrqCostProbe(self.syms,
                             self.driver_address("_timer_irq_handler"))
        delivered_before = ctc.delivered[2]
        ticks_before = self.live_ticks(z)
        since = len(z.glyphs)
        self.run_with(z, "timer stage", "timer: stage PASS", since,
                      probe=probe)
        trace = "".join(z.glyphs[since:])

        self.assertIn("timer: info 390625 250 OK", trace)
        self.assertIn("timer: mono 10000 OK", trace)
        self.assertNotIn("FAIL", trace)
        advance = int(re.search(r"timer: advance (\d+)", trace).group(1))
        print("timer: stage %d deliveries, advance %d counts over the "
              "1 s wait (%d..%d bound)"
              % (len(probe.deliveries), advance, SECOND_COUNTS,
                 SECOND_COUNTS + SECOND_SLACK))
        self.assertGreaterEqual(advance, SECOND_COUNTS)
        self.assertLessEqual(advance, SECOND_COUNTS + SECOND_SLACK)

        # The CTC's delivered channel-2 requests and the driver's own tick
        # count agree (at most one latched request is in flight), and the
        # handler ran for every delivery the model made.  The trace counts
        # a delivery at the handler's RETI and the model at the CPU's
        # acceptance, so it can miss the one the transcript's marker
        # catches mid-flight.
        ticks = self.live_ticks(z)
        handled = ctc.delivered[2] - delivered_before
        self.assertGreaterEqual(handled - (ticks - ticks_before), 0)
        self.assertLessEqual(handled - (ticks - ticks_before), 1)
        self.assertLessEqual(len(probe.deliveries), handled)
        self.assertLessEqual(handled - len(probe.deliveries), 1)

        # The printed count is the model's own T-state clock: it must sit
        # in the live count's window, and the ticks that pass between the
        # program's line and the replay's read are the lower bound's
        # slack.  (The rate itself is what `advance` bounds above.)
        now = int(re.search(r"timer: now (\d+)", trace).group(1))
        self.assertLess(now, (ticks + 1) * TICK_COUNTS)
        self.assertGreaterEqual(now,
                                (ticks - PRINT_TAIL_TICKS) * TICK_COUNTS)

        # Interrupt cost: the four phases, and the CPU share at 156.25 Hz.
        self.assert_interruptible_body()
        print("timer: interrupt %d deliveries, mean %.1f T "
              "(gate %.1f, dispatch+trampoline %.1f, handler %.1f, "
              "return %.1f), share %.2f %%"
              % (len(probe.deliveries), probe.mean(),
                 probe.mean_phase("gate"),
                 probe.mean_phase("dispatch"),
                 probe.mean_phase("handler"),
                 probe.mean_phase("return"),
                 100.0 * probe.mean() / TICK_TSTATES))
        self.assertLess(probe.mean(), TICK_TSTATES)

    def test_a_512_byte_bulk_read_is_exact_and_its_window_measured(self):
        big = b"0123456789abcdef" * 64
        z = self.start(card=True, extra_files={"BIG.BIN": big})
        peer = z.ioc_peer
        ctc = z.ctc
        probe = MaskWindowProbe(*self.ioc_hot_range())
        since = len(z.glyphs)
        delivered_before = ctc.delivered[2]
        expired_before = ctc.expiries[2]
        ticks_before = self.live_ticks(z)
        self.run_with(z, "cat BIG.BIN", SHELL_PROMPT, since, probe=probe)
        probe.close(z)
        ticks_after = self.live_ticks(z) - ticks_before

        # The 512-byte chunk really crossed the IOC lane, byte-exact:
        # requirement 7's "with the timer running, every IOC transfer
        # must stay correct".
        self.assertGreaterEqual(peer.a_data_reads, 512)
        self.assertIn(big[:512], b"".join(peer.bulk_sends))

        # The masked windows, measured (requirements 3 and 7 want the
        # number and its place): the longest one that carried IOC-driver
        # code is the transfer's own masking section, and every tick that
        # expired while the mask was up beyond the first one the CTC's
        # single request holds is a tick the count never sees.  Both are
        # reported for the hardware verdict; the budget itself is the
        # lane's rate, which only a board settles.
        ioc_window = probe.longest(ioc=True)
        operation_window = probe.longest()
        span = probe.longest_span()
        lost = (ctc.expiries[2] - expired_before) - \
            (ctc.delivered[2] - delivered_before)
        print("timer: %d masked windows, longest with the IOC driver %d T "
              "(%.2f ms of the %.2f ms budget) at %s, longest in the "
              "operation %d T (%.2f ms), %d ticks above the single request "
              "latch"
              % (len(probe.spans), ioc_window, ioc_window * 0.1 / 1000.0,
                 MASK_BUDGET_TSTATES * 0.1 / 1000.0,
                 self.describe_span(probe.longest_span(ioc=True)),
                 operation_window, operation_window * 0.1 / 1000.0, lost))
        self.assertGreater(ioc_window, 0)
        # And the gate really does let a body be interrupted (the
        # correction's own evidence).
        self.assert_interruptible_body()

        # What is exact under any window length: the transfer above, and
        # the driver's count following the delivered requests one for one
        # (the swallowed expiries are the CTC's single latch, not a
        # missed handler).
        handled = ctc.delivered[2] - delivered_before
        self.assertGreaterEqual(handled - ticks_after, 0)
        self.assertLessEqual(handled - ticks_after, 1)

    def test_a_card_copy_keeps_the_clock_the_model_settled(self):
        """`timer card`: the correction's timing row.  A 64 KiB card
        file is copied in 512-byte calls and deleted; the clock the
        program printed must be the model's own clock for the transfer,
        less exactly the ticks the CTC's one-deep latch swallowed while
        the card lane masked.  The card lane's model pacing is what
        decides whether the strict two-tick form holds, and that is
        reported rather than asserted (see the validation record)."""
        big = b"0123456789abcdef" * (CARD_BYTES // 16)
        z = self.start(card=True, extra_files={"BIG.BIN": big})
        peer = z.ioc_peer
        ctc = z.ctc
        windows = MaskWindowProbe(*self.ioc_hot_range())
        bracket = CountsProbe(self.driver_address("_timer_counts_now"))
        since = len(z.glyphs)
        delivered_before = ctc.delivered[2]
        expired_before = ctc.expiries[2]

        def hooks(z):
            windows(z)
            bracket(z)

        # The card line's seconds suffix is the end of the transcript's
        # last line: the run stops after the whole line is out.
        self.run_with(z, "timer card BIG.BIN", " s\n", since,
                      probe=hooks, limit=CARD_STEP_LIMIT)
        windows.close(z)
        trace = "".join(z.glyphs[since:])
        self.assertNotIn("FAIL", trace)
        match = re.search(r"timer: card (\d+) counts (\d+)\.(\d) s", trace)
        self.assertIsNotNone(match, repr(trace[-200:]))
        printed = int(match.group(1))
        tenths = int(match.group(2)) * 10 + int(match.group(3))

        # What crossed: every byte was read from, and written to, the card
        # through the IOC lanes, the written payloads are the file in
        # order, and the temporary is gone - the card is as the run found
        # it (requirement 7's "every IOC transfer stays correct").
        self.assertGreaterEqual(peer.a_data_reads, len(big))
        self.assertIn(big[:512], b"".join(peer.bulk_sends))
        written = b"".join(payload for rtype, _seq, _status, payload
                           in peer.bulk_requests
                           if rtype == IocCommandPeer.CMD_FS2_WRITE)
        self.assertEqual(written, big)
        self.assertEqual({parts: bytes(content)
                          for parts, content in peer.fs2_files.items()},
                         {KEEP_NAME: KEEP_CONTENT, ("BIG.BIN",): big})
        self.assertEqual(peer.fs2_dirs, {()})

        # The clock: the program's reading against the model's, with the
        # swallowed expiries (250 counts each) taken out.
        lost = ((ctc.expiries[2] - expired_before) -
                (ctc.delivered[2] - delivered_before))
        span = bracket.calls[-1] - bracket.calls[0]
        model_counts = span / COUNT_TSTATES
        ioc_window = windows.longest(ioc=True)
        print("timer: card %d bytes, printed %d counts (%.1f s), the "
              "model's clock %d counts, %d expiries lost to the latch, %d "
              "masked windows (longest with the IOC driver %d T)"
              % (len(big), printed, tenths / 10.0, model_counts, lost,
                 len(windows.spans), ioc_window))
        self.assertLessEqual(
            abs(printed - (model_counts - TICK_COUNTS * lost)),
            2 * TICK_COUNTS)

        # The strict form holds while the lanes' own masked sections are
        # inside the two-tick budget.  The emulated card lane is paced far
        # slower than the wire it stands in for, so with it over budget
        # the row reports and leaves the strict check to the board.
        if ioc_window <= MASK_BUDGET_TSTATES:
            self.assertLessEqual(abs(printed - model_counts),
                                 2 * TICK_COUNTS)
        else:
            print("timer: card masked window %d T (%.1f ms) is over the "
                  "%.1f ms model budget at %s: the strict two-tick row is "
                  "the board's to hold"
                  % (ioc_window, ioc_window * 0.1 / 1000.0,
                     MASK_BUDGET_TSTATES * 0.1 / 1000.0,
                     self.describe_span(windows.longest_span(ioc=True))))
        self.assert_interruptible_body()


if __name__ == "__main__":
    unittest.main()
