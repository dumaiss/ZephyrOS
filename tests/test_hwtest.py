"""Hardware exerciser static contract.

The production store carries a feature exerciser (`/system/hwtest.exe`)
and the outcome programs it runs, and the monitor gains the command that
starts it.  `test_hwtest_sim.py` replays the outcomes in the simulator;
this module pins what a replay cannot see: the runner's source surface,
the `stage` option the reused programs gained, the monitor command, the
production store's new entries and the build/smoke wiring.
"""

import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)

import mapcheck  # noqa: E402

RUNNER = os.path.join(ROOT, "tests", "hwtest", "hwtest.c")
FS_PROGRAM = os.path.join(ROOT, "tests", "fs", "fs_selftest.c")
FS_CHILD = os.path.join(ROOT, "tests", "fs", "fs_child.c")
XMEM_PROGRAM = os.path.join(ROOT, "tests", "xmem", "xmem_selftest.c")
MONITOR = os.path.join(ROOT, "user", "monitor", "monitor.c")
MONITOR_EXEC = os.path.join(ROOT, "user", "monitor", "monitor_exec.c")
MONITOR_HDR = os.path.join(ROOT, "user", "monitor", "monitor.h")
MAKEFILE = os.path.join(ROOT, "Makefile")
SYSTEM = os.path.join(ROOT, "systems", "production.json")
SMOKE = os.path.join(ROOT, "tools", "serial_smoke.py")
MEMORY_INC = os.path.join(ROOT, "platform", "zephyr80", "private",
                          "memory.inc")
STORE = os.path.join(ROOT, "build", "recovery.romfs")

# The store's program set: the launcher plus the exerciser and the six
# images its three stages run (the xmem family, the far program and the
# filesystem family).
STORE_PROGRAMS = ("monitor.exe", "hwtest.exe", "xmem_selftest.exe",
                  "xmem_child.exe", "xmem_far.exe", "fs_selftest.exe",
                  "fs_child.exe")

# The store the exerciser shares with everything else.
STORE_CAPACITY = 0x48000


def read(path):
    with open(path, "r", errors="replace") as handle:
        return handle.read()


class TestExerciserProgram(unittest.TestCase):
    """`tests/hwtest/hwtest.c`: the store's runner."""

    def test_the_runner_removes_the_card_program_it_staged(self):
        text = read(RUNNER)
        self.assertIn('#define HW_PROGRAM "/fs/HWTEST/CHILD.EXE"', text)
        remove = text[text.index("static void hw_scratch_remove"):]
        remove = remove[:remove.index("\n}\n")]
        self.assertIn("unlink(HW_PROGRAM)", remove)

    def test_it_runs_the_three_stage_programs_from_the_store(self):
        text = read(RUNNER)
        for path in ('"/system/xmem_selftest.exe"', '"/system/xmem_far.exe"',
                     '"/system/fs_selftest.exe"'):
            self.assertIn(path, text)
        self.assertIn("run(", text)
        self.assertIn('#include "kos.h"', text)

    def test_the_stage_option_is_a_tail_not_a_copy(self):
        text = read(RUNNER)
        self.assertIn('XMEM_STAGE_TAIL "stage"', text)
        self.assertIn('FS_STAGE_TAIL   "stage"', text)

    def test_every_card_write_stays_inside_the_scratch_directory(self):
        text = read(RUNNER)
        self.assertIn('HW_DIR    "/fs/HWTEST"', text)
        for name in ("HW_FILE", "HW_RENAMED", "HW_SUBDIR", "HW_CHILD"):
            self.assertIn(name, text)
        # The stage removes the whole footprint and proves the directory
        # it made is gone again, so a failing run leaves no trace either.
        self.assertIn("rmdir(HW_DIR)", text)
        self.assertIn("stat(HW_DIR", text)
        self.assertIn("-KOS_ENOENT", text)
        # Every card path the runner names is one of the scratch names;
        # `/fs/nosuch` is the deliberate absent-path row and `/fs` itself
        # the card root the namespace stage enumerates.
        for literal in text.split("/fs/")[1:]:
            name = literal.split('"')[0]
            self.assertTrue(name.startswith("HWTEST") or name == "nosuch",
                            "/fs/%s is outside the scratch directory" % name)

    def test_it_reports_on_the_console_and_the_serial_lane(self):
        text = read(RUNNER)
        self.assertIn('LANE_NAME    "/dev/serial/0"', text)
        self.assertIn("KOS_TYPE_SERIAL", text)
        self.assertIn("KSIO_OP_WRITE", text)
        # The console comes first, so a lane that fails cannot cost the
        # owner the visible transcript.
        self.assertLess(text.index("console_write(text, length)"),
                        text.index("hw_lane_write(text, length)"))

    def test_every_program_runs_with_no_handle_held(self):
        text = read(RUNNER)
        # A nested exec must not start against this runner's lane handle.
        run = text[text.index("static int hw_run"):]
        run = run[:run.index("\n}")]
        self.assertLess(run.index("hw_lane_close()"), run.index("= run("))
        self.assertLess(run.index("= run("), run.index("hw_lane_open()"))

    def test_the_summary_and_the_card_note_are_the_contract(self):
        text = read(RUNNER)
        self.assertIn('hw_name[] = "hwtest"', text)
        self.assertIn(" (card absent)", text)
        self.assertIn(" (card not writable)", text)
        self.assertIn('"PASS" : "FAIL"', text)
        # PASS exits 0 and FAIL exits 1: the status the monitor prints.
        self.assertIn("(passed == STAGE_COUNT) ? 0 : 1", text)


class TestTheExerciserReportsTheCodeItSaw(unittest.TestCase):
    """A failed row carries the answer, not just its name."""

    def test_every_code_row_prints_the_observed_code(self):
        text = read(RUNNER)
        self.assertIn("hw_check_status(", text)
        self.assertIn('"got %02x want %02x"', text)
        # An accepted call prints only what it got, a wrong type says both.
        self.assertIn("hw_check_accepted(", text)
        self.assertIn('"got %02x"', text)
        self.assertIn('"type %02x want %02x"', text)
        self.assertIn("hw_check_enum(", text)
        # The verdict line carries the first failure's detail.
        self.assertIn("ns_fail_detail", text)
        self.assertIn('"%s %s"', text)

    def test_the_namespace_and_card_rows_use_them(self):
        text = read(RUNNER)
        for label in ("bind-container", "bind-card-enodev", "chdir-card-enodev",
                      "table-emfile", "enum-card", "unbind-bad-handle"):
            self.assertIn('"%s"' % label, text)
        # The card-absent row compares the one documented no-media code.
        self.assertIn("chdir-card-enodev", text)
        self.assertIn("-KOS_ENODEV", text)


class TestReusedProgramOptions(unittest.TestCase):
    """`stage` is the one option the reused programs take."""

    def test_the_filesystem_program_confines_its_writes(self):
        text = read(FS_PROGRAM)
        self.assertIn('FS_STAGE_TAIL "stage"', text)
        self.assertIn('HW_DIR    "/fs/HWTEST"', text)
        for name in ("HW_FILE", "HW_RENAMED", "HW_SUBDIR"):
            self.assertIn(name, text)
        # Both the paths and the monitor handover follow the option; the
        # checks themselves are one list.
        self.assertIn("stage ? HW_FILE : FS_FILE", text)
        self.assertIn("stage ? HW_DIR : FS_DIR", text)
        self.assertIn("if (stage == 0) {", text)
        self.assertIn('run(CHILD, stage ? FS_STAGE_TAIL : "tail-42")',
                      text)

    def test_the_filesystem_program_execs_the_child_from_the_card(self):
        # The card-exec row: the same child, copied onto the
        # card, run from `/fs` - the bind path a bank-6 provider has to
        # read.  Both images' paths, one row pair.
        text = read(FS_PROGRAM)
        self.assertIn('#define FS_CHILD_PROGRAM "/fs/CHILD.EXE"', text)
        self.assertIn('#define HW_CHILD_PROGRAM "/fs/HWTEST/CHILD.EXE"', text)
        self.assertIn("copy_child_to_card(", text)
        self.assertIn('check("exec-card-copy", copy_child_to_card(copy))', text)
        self.assertIn('check("exec-card-status", status == CHILD_STATUS)',
                      text)
        self.assertIn("run(copy, stage ? FS_STAGE_TAIL : \"tail-42\")",
                      text)

    def test_the_child_takes_the_same_option(self):
        text = read(FS_CHILD)
        self.assertIn('#define CHILD_STAGE_FILE "/fs/HWTEST/CHILD.TMP"', text)
        self.assertIn('same(argv[0], CHILD_STAGE)', text)

    def test_the_extended_memory_program_states_its_rows_against_the_pool(self):
        text = read(XMEM_PROGRAM)
        self.assertIn('XMEM_STAGE_TAIL "stage"', text)
        # The pool rows scale with the banks this context can open (the
        # production image shares the pool with live processes).
        self.assertIn("banks = fill_until_full(", text)
        self.assertIn('check("exhaustion-16k-count", count == 2u * banks)',
                      text)
        self.assertIn("(void)xfree(big[banks - 1u])", text)
        # In a stage run the runner owns the far program: one exec level
        # shallower, because the chain already sits at the kernel limit.
        self.assertIn("if (stage == 0) {", text)


class TestMonitorExecCommand(unittest.TestCase):
    """`X <path> [tail]` runs a program and returns to the prompt."""

    def test_the_command_runs_a_program_and_prints_its_status(self):
        text = read(MONITOR_EXEC)
        self.assertIn("void mon_cmd_exec(char *cursor)", text)
        self.assertIn("run(path, tail)", text)
        # A failed exec reports the native code, a normal exit the u16
        # status the runner's verdict travels in.
        self.assertIn("zrt_status_err_code(status)", text)
        self.assertIn('console_puts("exec: exit status 0x")', text)
        self.assertIn("mon_put_hex16(", text)
        # The help row and the dispatch are part of the command surface.
        self.assertIn("X path [tail]", read(MONITOR))
        self.assertIn("mon_cmd_exec(cursor)", read(MONITOR))
        self.assertIn("void mon_cmd_exec(char *cursor);", read(MONITOR_HDR))

    def test_the_monitor_reads_the_serial_lane_the_smoke_drives(self):
        text = read(MONITOR)
        self.assertIn('mon_lane_name[] = "/dev/serial/0"', text)
        self.assertIn("KSIO_OP_POLL", text)
        self.assertIn("KSIO_OP_GETC", text)
        # The console is polled first, so the serial-console build keeps
        # one stream and echoes every byte exactly once.
        self.assertLess(text.index("console_read(&ch, 1)"),
                        text.index("mon_lane_getc()"))
        self.assertIn('mon_lane_name[] = "/dev/serial/0"', text)


class TestProductionStore(unittest.TestCase):
    """The exerciser ships in the production store (no special flash)."""

    def store(self):
        return json.loads(read(SYSTEM))["store"]

    def test_the_system_file_carries_the_exerciser_and_its_programs(self):
        # The store's full order is pinned where the store is read (the
        # HID and V9958 artifact tests); here every program the exerciser
        # needs must be present.  The packed entry's media type follows
        # from the name (tools/systemgen.py's store_type: `.exe` is EXE),
        # pinned by tests/test_systemgen.py.
        store = self.store()
        for name in STORE_PROGRAMS:
            self.assertIn(name, store, "%s is not in the store" % name)

    def test_the_store_fits_its_capacity(self):
        if not os.path.exists(STORE):
            raise unittest.SkipTest("%s missing; build the image first"
                                    % STORE)
        capacity = mapcheck.parse_memory_inc(MEMORY_INC)["KOS_ROMFS_CAPACITY"]
        self.assertEqual(capacity, STORE_CAPACITY)
        self.assertLessEqual(os.path.getsize(STORE), capacity,
                             "the recovery store is over its capacity")


class TestBuildWiring(unittest.TestCase):
    def test_the_runner_is_built_as_a_program(self):
        makefile = read(MAKEFILE)
        self.assertIn("exe_program,hwtest,tests/hwtest", makefile)
        self.assertIn("hwtest.c,STACK=256,STAGE=1", makefile)
        # Production packs it and the programs its stages run: each is
        # one exe_program line and one store entry.
        system = json.loads(read(SYSTEM))
        for name in STORE_PROGRAMS:
            self.assertIn(name, system["store"])
        # `X /system/hwtest.exe` needs the EXE on the store, not a boot
        # program: production still boots straight to the monitor.
        system = json.loads(read(SYSTEM))
        self.assertEqual(system["boot"], "/system/monitor.exe")
        self.assertIn("hwtest.exe", system["store"])


class TestSmokeMode(unittest.TestCase):
    """`make smoke-hardware-hwtest` drives the production lane."""

    def test_the_mode_sends_the_command_and_checks_the_transcript(self):
        text = read(SMOKE)
        self.assertIn('"hwtest"', text)
        self.assertIn('HW_COMMAND = b"X /system/hwtest.exe\\r"', text)
        for marker in ("hwtest: begin", "hwtest: xmem+far PASS",
                       "hwtest: namespace+handles PASS", "hwtest: files PASS",
                       "hwtest: files card absent",
                       "hwtest: files card not writable",
                       "hwtest: 3/3 PASS", "hwtest: 3/3 FAIL"):
            self.assertIn(marker, text)
        # The exerciser takes minutes on hardware: the mode carries its
        # own default timeout.
        self.assertIn("HW_TIMEOUT", text)
        self.assertIn("smoke-hardware-hwtest", read(MAKEFILE))


if __name__ == "__main__":
    unittest.main()
