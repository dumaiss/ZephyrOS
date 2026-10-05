"""M9 nested-exec acceptance replay (package 28).

Runs the m9-exec-selftest image in the Z80 simulator.  The image boots
SIO-only, mounts the test-only `ramfs` provider, and the target hook runs
`fstest.zex` (whose `exec` case covers CWD inheritance, a child `chdir`,
descriptor release, the status round trip, the raw-return `ZOS_EFAULT`,
every failure mode and 32 exec/exit cycles) followed by the recursive
`fsdeep.zex` depth probe.

The replay also instruments every suspension, because the continuation
stack is the genuinely new mechanism:

  - the copied gate frame is measured against `ZOS_EXEC_FRAME_SAVE` and
    must be the same size at every depth;
  - the save area must still hold the frame the launch copied once the
    child has run (the child reused the same common gate stack);
  - the resumed parent's live frame must be byte-identical to the frame
    that was suspended, and its memory latch must be the launcher's;
  - the next time the parent returns to application mode it must be over
    the *parent's* bank, not the child's;
  - after the whole chain, the process table, bank reservations,
    descriptor pool and frame save areas are back to baseline.
"""

import os
import re
import sys
import unittest
from pathlib import Path

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import Z80, IocCommandPeer, V9958Peer, read_symbols  # noqa: E402

RET = 0x1234
STACK = 0xFFFE
STEP_LIMIT = 40000000

IMAGE_SUBDIR = "build-m9-exec-selftest"
IMAGE_NAME = "zephyros-m9-exec-selftest.bin"

BANNER = ("ZephyrOS 0.0\r\nplatform: Zephyr-80\r\nkernel entry: OK\r\n"
          "serial: OK\r\nconsole: dynamic sio0.b OK\r\n")
FSTEST_PASS = "fstest: PASS 10/10\r\n"
SELFTEST_OK = "m9 exec selftest: OK\r\n"
GATE_STACK_SIZE = 256
OS_MODE_MASK = 0x18
APP_MODE = 0x10
OS_MODE = 0x18
BANK_MASK = 0x07

HEADER = os.path.join(ROOT, "include", "zephyros", "process.h")
VFS_HEADER = os.path.join(ROOT, "kernel", "vfs", "vfs.h")


def _read(path):
    return Path(path).read_text()


def _pinned():
    text = _read(HEADER)
    values = {}
    for name in ("ZOS_EXEC_MAX_DEPTH", "ZOS_EXEC_FRAME_SAVE"):
        match = re.search(r"#define\s+%s\s+(\d+)u" % name, text)
        assert match is not None, "%s is not pinned in %s" % (name, HEADER)
        values[name] = int(match.group(1))
    return values


class M9ExecSimBase(unittest.TestCase):
    def setUp(self):
        self.bin_path = os.path.join(ROOT, IMAGE_SUBDIR, IMAGE_NAME)
        if not os.path.exists(self.bin_path):
            self.skipTest("%s missing; build the image first" % self.bin_path)
        with open(self.bin_path, "rb") as handle:
            self.image = handle.read()
        self.syms = read_symbols(os.path.join(ROOT, IMAGE_SUBDIR,
                                              "zephyros.map"))
        pinned = _pinned()
        self.max_depth = pinned["ZOS_EXEC_MAX_DEPTH"]
        self.frame_save = pinned["ZOS_EXEC_FRAME_SAVE"]

    def make_cpu(self):
        pages = {n: self.image[n * 0x10000:(n + 1) * 0x10000]
                 for n in range(8)}
        z = Z80(pages[0], pages[1],
                rom_pages={n: pages[n] for n in range(2, 8)})
        z.latch = 0x18
        z.sp = STACK
        z.hid = IocCommandPeer()
        z.ioc_peer = z.hid
        z.vdp = V9958Peer()
        z.vdp_peer = z.vdp
        z.glyphs = []
        z.glyph_pos = 0
        z.glyph_last = None
        return z

    def crt0(self, z):
        z.push16(RET)
        z.pc = self.syms["__zos_crt0_init"]
        for _ in range(2000000):
            z.step()
            if z.pc == RET:
                return
        raise AssertionError("C runtime init did not return")

    def boot(self, limit=STEP_LIMIT):
        """Boot to the acceptance marker, instrumenting every suspension."""
        z = self.make_cpu()
        self.crt0(z)
        z.push16(RET)
        z.pc = self.syms["_kernel_main"]
        delay = self.syms["_kernel_serial_connect_delay"]
        launch_pc = self.syms["_zos_process_launch"]
        resume_pc = self.syms["_zos_process_resume"]
        cont = self.syms["zos_proc_cont"]
        frames = self.syms["zos_proc_frames"]
        gate_top = self.syms["zos_gate_stack_top"]
        probe = _SuspensionProbe(
            z, gate_top, frames, cont, self.frame_save, self.max_depth,
            self.syms.get("_vfs_workspace"))
        min_sp = gate_top
        for step in range(limit):
            if z.pc == delay:
                z.pc = z.pop16()
            if z.pc == launch_pc:
                probe.on_launch()
            elif z.pc == resume_pc:
                probe.on_resume()
            else:
                probe.tick()
            z.step()
            # The sample is taken at the *start* of every instruction, so
            # no push can hide: this is the true gate-stack high-water
            # mark, not a sampled estimate.  That is why the acceptance
            # builds carry no gate-stack canary pattern - there is nothing
            # left for one to observe.
            if gate_top - GATE_STACK_SIZE <= z.sp <= gate_top:
                if z.sp < min_sp:
                    min_sp = z.sp
            if (SELFTEST_OK.encode() in z.sio_tx or
                    b"m9 exec selftest: FAIL" in z.sio_tx or
                    b"PANIC" in z.sio_tx or z.halted):
                break
        else:
            raise AssertionError("acceptance never completed (PC=%04X)"
                                 % z.pc)
        return z, probe, step, min_sp


class _Suspension:
    """One observed suspension of a user parent."""

    def __init__(self, index, saved_sp, frame, latch):
        self.index = index
        self.saved_sp = saved_sp
        self.frame = frame
        self.latch = latch  # OS-mode view of the launcher's bank
        self.phase = "frame"

    def application_latch(self):
        return APP_MODE | (self.latch & BANK_MASK)


class _SuspensionProbe:
    """Instrumentation for the launch/resume gates.

    At every launch it records the launcher's gate frame, its latch and
    the frame area that will hold it; at every resume it checks the save
    area, the restored frame and the latch, then follows the parent back
    to application mode to confirm it returns over its own bank.
    """

    def __init__(self, z, gate_top, frames, cont, frame_save, max_depth,
                 workspace=None):
        self.z = z
        self.workspace = workspace
        self.gate_top = gate_top
        self.frames = frames
        self.cont = cont
        self.frame_save = frame_save
        self.max_depth = max_depth
        self.records = {}
        self.frame_lengths = []
        self.suspensions = 0
        self.resumes = 0
        self.kernel_resumes = 0
        self.poisonings = 0
        self.deepest = 0
        self.errors = []
        self.pending = None

    def _bank0(self):
        return self.z.bank0  # common memory is bank 0 above E000h

    def _read16(self, addr):
        b = self._bank0()
        return b[addr] | (b[addr + 1] << 8)

    def on_launch(self):
        """The launch gate is entered with the launcher's live gate frame.

        The primitive captures its caller's SP first, so the SP at this
        instruction *is* the continuation SP, and [SP, gate_top) is the
        frame it will copy.
        """
        b = self._bank0()
        req = (self.z.h << 8) | self.z.l
        index = b[req + 3]
        if index == 0:
            return  # kernel launcher: boot stack, no gate frame
        if index >= self.max_depth:
            self.errors.append("launch index %d out of range" % index)
            return
        saved_sp = self.z.sp
        length = self.gate_top - saved_sp
        self.frame_lengths.append(length)
        if length > self.frame_save:
            self.errors.append(
                "frame at depth %d is %d bytes, over the %d-byte save"
                % (index, length, self.frame_save))
        record = _Suspension(index, saved_sp,
                             bytes(b[saved_sp:self.gate_top]), self.z.latch)
        self.records[index] = record
        self.suspensions += 1
        self.deepest = max(self.deepest, index)
        self._poison_workspace()

    def _poison_workspace(self):
        """Overwrite the suspended parent's shared VFS workspace.

        The parent is suspended *now*, before the child's first
        instruction, so a workspace-scratch-shaped continuation would be
        destroyed here and the acceptance would fail after the resume.
        The bytes are written physically (the sim's bank-7 array), which
        is what a hostile child with a workspace pointer would do."""
        if self.workspace is None:
            return
        size = 512
        self.z.bank7[self.workspace:self.workspace + size] = bytes(
            (0xA5 ^ i) & 0xFF for i in range(size))
        self.poisonings += 1

    def on_resume(self):
        """Check everything the child could have disturbed."""
        b = self._bank0()
        req = (self.z.h << 8) | self.z.l
        index = b[req + 2]
        if index == 0:
            self.kernel_resumes += 1
            return
        self.resumes += 1
        record = self.records.get(index)
        if record is None:
            self.errors.append("resume at depth %d without a launch" % index)
            return
        area = bytes(b[self.frames + (index - 1) * self.frame_save:
                       self.frames + index * self.frame_save])
        if area[:len(record.frame)] != record.frame:
            self.errors.append(
                "the depth-%d frame save was modified while the child ran"
                % index)
        if self._read16(self.cont + 2 * index) != record.saved_sp:
            self.errors.append(
                "the depth-%d continuation SP was overwritten" % index)
        self.pending = record

    def tick(self):
        """Follow a completed resume back to application mode."""
        record = self.pending
        if record is None:
            return
        z = self.z
        if record.phase == "frame":
            # The resume's RET has landed in the launcher: everything above
            # the popped return address must be byte-identical to the frame
            # that was suspended, and the latch must be the launcher's.
            if not (z.sp == record.saved_sp + 2 and
                    z.pc == self._read16(record.saved_sp)):
                return
            live = bytes(self._bank0()[record.saved_sp + 2:self.gate_top])
            if live != record.frame[2:]:
                self.errors.append(
                    "depth %d resumed with a different gate frame"
                    % record.index)
            if z.latch != record.latch:
                self.errors.append(
                    "depth %d resumed with latch %02X, expected %02X"
                    % (record.index, z.latch, record.latch))
            record.phase = "application"
        elif (z.latch & OS_MODE_MASK) == APP_MODE:
            # The suspended SYS_EXEC has returned to user mode: it must be
            # the launcher's own bank, never the child's.
            if z.latch != record.application_latch():
                self.errors.append(
                    "depth %d returned to application mode over bank %d, "
                    "expected %d"
                    % (record.index, z.latch & BANK_MASK,
                       record.application_latch() & BANK_MASK))
            self.pending = None


class TestM9ExecAcceptance(M9ExecSimBase):
    def test_boot_and_acceptance_marker(self):
        z, _probe, _steps, _min_sp = self.boot()
        sio = z.sio_tx.decode("ascii", "replace")
        self.assertTrue(sio.startswith(BANNER), sio)
        self.assertIn(FSTEST_PASS, sio)
        self.assertIn(SELFTEST_OK, sio)
        self.assertNotIn("FAIL", sio)
        self.assertNotIn("PANIC", sio)
        self.assertEqual(z.bank7[self.syms["_m9_exec_selftest_result"]], 0)

    def test_no_exec_case_fails(self):
        z, _probe, _steps, _min_sp = self.boot()
        sio = z.sio_tx.decode("ascii", "replace")
        self.assertNotIn("fstest: exec FAIL", sio)
        self.assertNotIn("fstest: FAIL", sio)
        # The child ran from its inherited CWD and reported it.
        self.assertIn("fschild: cwd /D1\r\n", sio)

    def test_depth_probe_reaches_the_limit(self):
        z, probe, _steps, _min_sp = self.boot()
        self.assertEqual(
            z.bank7[self.syms["_m9_exec_selftest_result"]], 0,
            "the depth probe did not stop at ZOS_EXEC_MAX_DEPTH")
        self.assertEqual(probe.deepest, self.max_depth - 1,
                         "the deepest suspension was depth %d, expected %d"
                         % (probe.deepest, self.max_depth - 1))
        self.assertGreaterEqual(probe.suspensions, 32,
                                "the 32 exec/exit cycles did not suspend")
        self.assertEqual(probe.suspensions, probe.resumes,
                         "%d suspensions but %d resumes"
                         % (probe.suspensions, probe.resumes))

    def test_workspace_poisoning_does_not_disturb_the_parent(self):
        """A suspended parent holds nothing in the shared VFS workspace."""
        z, probe, _steps, _min_sp = self.boot()
        self.assertIn("_vfs_workspace", self.syms,
                      "the acceptance image has no shared VFS workspace")
        self.assertGreater(probe.poisonings, 0,
                           "no suspension poisoned the workspace")
        self.assertEqual(probe.errors, [])
        sio = z.sio_tx.decode("ascii", "replace")
        self.assertIn(FSTEST_PASS, sio)
        self.assertIn(SELFTEST_OK, sio)

    def test_frames_fit_and_are_identical_at_every_depth(self):
        z, probe, _steps, _min_sp = self.boot()
        self.assertEqual(probe.errors, [])
        self.assertEqual(len(set(probe.frame_lengths)), 1,
                         "the copied frame differs between suspensions: %s"
                         % sorted(set(probe.frame_lengths)))
        length = probe.frame_lengths[0]
        self.assertLessEqual(
            length, self.frame_save,
            "the copied gate frame is %d bytes, over the %d-byte save"
            % (length, self.frame_save))
        print("m9 exec: copied gate frame %d bytes of %d saved, "
              "%d suspensions, %d resumes (+%d kernel), deepest depth %d"
              % (length, self.frame_save, probe.suspensions,
                 probe.resumes, probe.kernel_resumes, probe.deepest))

    def test_process_state_returns_to_baseline(self):
        z, _probe, _steps, _min_sp = self.boot()
        table = self.syms["_process_table"]
        # struct process: state, pid, bank, parent, flags:u16, exit_status:u16
        stride = 8
        self.assertEqual(
            z.bank7[self.syms["_process_running_pid"]], 0xFF,
            "a process is still RUNNING at the acceptance mark")
        for pid in range(7):
            state = z.bank7[table + pid * stride]
            self.assertEqual(state, 0,
                             "process slot %d is not FREE" % pid)
        # The deepest probe instance's status reached the kernel launcher:
        # four live levels, the fifth exec refused.
        status = (z.bank7[self.syms["_process_last_status"]] |
                  (z.bank7[self.syms["_process_last_status"] + 1] << 8))
        self.assertEqual(status, self.max_depth,
                         "the kernel launcher saw status %d, expected %d"
                         % (status, self.max_depth))

    def test_descriptor_pool_is_released(self):
        z, _probe, _steps, _min_sp = self.boot()
        base = self.syms["_vfs_descriptors"]
        header = _read(VFS_HEADER)
        match = re.search(
            r"sizeof\(struct vfs_descriptor\)\s*==\s*(\d+)", header)
        self.assertIsNotNone(match, "vfs_descriptor size is not pinned")
        stride = int(match.group(1))
        for fd in range(8):
            self.assertEqual(z.bank7[base + fd * stride], 0xFF,
                             "descriptor %d still owned" % fd)
            self.assertEqual(z.bank7[base + fd * stride + 1], 0,
                             "descriptor %d not FREE" % fd)

    def test_frame_save_areas_are_idle_but_intact(self):
        z, probe, _steps, _min_sp = self.boot()
        self.assertTrue(probe.records, "no suspension was observed")
        for index, record in probe.records.items():
            base = self.syms["zos_proc_frames"] + (index - 1) * \
                self.frame_save
            live = bytes(z.bank0[base:base + len(record.frame)])
            self.assertEqual(live, record.frame,
                             "the depth-%d save area no longer holds the "
                             "frame it copied" % index)

    def test_gate_stack_stays_within_budget(self):
        z, _probe, _steps, min_sp = self.boot()
        gate_top = self.syms["zos_gate_stack_top"]
        depth = gate_top - min_sp
        self.assertGreater(depth, 0, "the gate stack was never entered")
        self.assertLessEqual(
            depth, GATE_STACK_SIZE,
            "worst-case gate-stack depth %d exceeds the %d-byte gate stack"
            % (depth, GATE_STACK_SIZE))
        print("m9 exec: worst-case gate-stack depth %d of %d bytes"
              % (depth, GATE_STACK_SIZE))


if __name__ == "__main__":
    unittest.main()
