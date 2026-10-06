"""M9 integration acceptance replay (package 31).

Runs the `m9-selftest` image in the Z80 simulator.  The image boots the
production boot path minus HID/V9958 (so the whole acceptance transcript is
on SIO), with `iocfat.zmod` resident and the production `config/mounts.json`
attaching `/` to it as an optional mount on `ioc.0`.  The simulator's
`IocCommandPeer` models the MCU FatFs service (FS2), so the acceptance is
exercised end to end without a card.

The target hook exec's `fstest.zex`, which exec's the deep-matrix program
`fsfat.zex`, then the recursive `fsdeep.zex` depth probe, then drives the two
section-33 error paths that cannot be produced on demand: a stale backend
handle and a failed confirmed write.  The host peer arms those faults on the
`arm` fences, exactly as `tests/test_iocfat_sim.py` does for the cache rules.

One full boot is shared by the tests that inspect the default transcript; it
is the expensive one (the >64 KiB size case moves 128 KiB over the lane).
"""

import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import Z80, IocCommandPeer, V9958Peer, read_symbols  # noqa: E402

RET = 0x1234
STACK = 0xFFFE
STEP_LIMIT = 400000000

IMAGE_SUBDIR = "build-m9-selftest"
IMAGE_NAME = "zephyros-m9-selftest.bin"

SELFTEST_OK = "m9 selftest: OK\r\n"
SELFTEST_ABSENT = "m9 selftest: OK (card absent)\r\n"
SELFTEST_EROFS = "m9 selftest: OK (read-only card)\r\n"
SELFTEST_FAIL = "m9 selftest: FAIL 0x"
FAIL_MARKERS = (SELFTEST_FAIL, "PANIC", SELFTEST_ABSENT, SELFTEST_EROFS)
FSTEST_PASS = "fstest: PASS 5/5\r\n"
FSFAT_PASS = "fsfat: PASS 8/8\r\n"
MONITOR = "Z80> "
GATE_STACK_SIZE = 256

FSTEST_CASES = ("namespace", "romfs", "iocfat", "window", "fat")
FSFAT_CASES = ("namespace", "basic", "sizes", "large", "cache", "pool",
               "exdev", "errors")
STALE_ARM = "m9 selftest: fault:stale:arm\r\n"
STALE_END = "m9 selftest: fault:stale:end\r\n"
WRITE_ARM = "m9 selftest: fault:write:arm\r\n"
WRITE_END = "m9 selftest: fault:write:end\r\n"


class M9IntegrationSimBase(unittest.TestCase):
    image = None
    syms = None
    default_run = None
    bin_path = os.path.join(ROOT, IMAGE_SUBDIR, IMAGE_NAME)

    @classmethod
    def setUpClass(cls):
        if not os.path.exists(cls.bin_path):
            return
        with open(cls.bin_path, "rb") as handle:
            cls.image = handle.read()
        cls.syms = read_symbols(os.path.join(ROOT, IMAGE_SUBDIR,
                                             "zephyros.map"))

    def setUp(self):
        if self.image is None:
            self.skipTest("%s missing; build the image first" % self.bin_path)

    def make_cpu(self, **peer_kwargs):
        pages = {n: self.image[n * 0x10000:(n + 1) * 0x10000]
                 for n in range(8)}
        z = Z80(pages[0], pages[1],
                rom_pages={n: pages[n] for n in range(2, 8)})
        z.latch = 0x18
        z.sp = STACK
        peer = IocCommandPeer(**peer_kwargs)
        z.hid = peer
        z.ioc_peer = peer
        z.peer = peer
        z.vdp = V9958Peer()
        z.vdp_peer = z.vdp
        return z

    def crt0(self, z):
        z.push16(RET)
        z.pc = self.syms["__zos_crt0_init"]
        for _ in range(2000000):
            z.step()
            if z.pc == RET:
                return
        raise AssertionError("C runtime init did not return")

    def run_kernel(self, z, stop, limit=STEP_LIMIT, fences=None,
                   on_fence=None, track_gate=False):
        """Boot to one of `stop`, delivering fence callbacks in order.

        A fence is a transcript marker; when it appears `on_fence(marker)`
        runs, so the host can change the peer's fault state at a known
        point.  Returns (min_gate_sp, steps)."""
        gate_top = self.syms["zos_gate_stack_top"]
        gate_base = gate_top - GATE_STACK_SIZE
        min_sp = gate_top
        buf = bytearray()
        seen_len = 0
        pending = list(fences or [])
        stops = [s.encode() for s in stop]
        self.crt0(z)
        z.push16(RET)
        z.pc = self.syms["_kernel_main"]
        delay = self.syms["_kernel_serial_connect_delay"]
        for step in range(limit):
            if z.pc == delay:
                z.pc = z.pop16()
            z.step()
            if track_gate and gate_base <= z.sp <= gate_top and z.sp < min_sp:
                min_sp = z.sp
            if z.halted:
                break
            if len(z.sio_tx) != seen_len:
                buf += bytes(z.sio_tx[seen_len:])
                seen_len = len(z.sio_tx)
                while pending and pending[0].encode() in buf:
                    marker = pending.pop(0)
                    if on_fence is not None:
                        on_fence(marker)
            if any(s in z.sio_tx for s in stops):
                return min_sp, step
        raise AssertionError("acceptance never completed (PC=%04X)" % z.pc)

    @classmethod
    def full_run(cls):
        """One shared default boot: the whole acceptance plus the monitor."""
        if cls.default_run is None:
            base = cls(methodName="test_boot_and_full_acceptance")
            z = base.make_cpu()
            min_sp, steps = base.run_kernel(
                z, [MONITOR] + list(FAIL_MARKERS), track_gate=True)
            cls.default_run = (z, min_sp, steps)
        return cls.default_run

    def transcript(self, z):
        return z.sio_tx.decode("ascii", "replace")


class TestM9IntegrationAcceptance(M9IntegrationSimBase):
    def test_boot_and_full_acceptance(self):
        z, _min_sp, _steps = self.full_run()
        sio = self.transcript(z)

        for line in ("ZephyrOS 0.0", "platform: Zephyr-80",
                     "kernel entry: OK", "serial: OK",
                     "console: dynamic sio0.b OK"):
            self.assertIn(line + "\r\n", sio)
        self.assertNotIn("unavailable", sio)
        self.assertNotIn("PANIC", sio)
        # The whole native acceptance: fstest and its deep-matrix child.
        for case in FSTEST_CASES:
            self.assertIn("fstest: %s OK" % case, sio)
        for case in FSFAT_CASES:
            self.assertIn("fsfat: %s OK" % case, sio)
        self.assertIn(FSTEST_PASS, sio)
        self.assertIn(FSFAT_PASS, sio)
        self.assertNotIn("fstest: FAIL", sio)
        self.assertNotIn("fsfat: FAIL", sio)
        self.assertIn(SELFTEST_OK, sio)
        self.assertNotIn(SELFTEST_FAIL, sio)
        # The unchanged launcher then runs the monitor on the same SIO link.
        self.assertIn(MONITOR, sio)
        self.assertEqual(
            z.bank7[self.syms["_m9_integration_selftest_result"]], 0)

    def test_large_size_case_is_card_only(self):
        # The 2 KiB test-only RAM provider is not in this image, so the
        # >64 KiB span really is written to and read back from the volume.
        z, _min_sp, _steps = self.full_run()
        sio = self.transcript(z)
        self.assertIn("fsfat: large OK", sio)
        self.assertNotIn("large: skipped", sio)

    def test_fault_windows_are_inert_on_unfaulted_media(self):
        z, _min_sp, _steps = self.full_run()
        sio = self.transcript(z)
        self.assertIn("m9 selftest: fault:stale:fresh\r\n", sio)
        self.assertIn("m9 selftest: fault:write:fresh\r\n", sio)
        self.assertNotIn("fault:stale:observed", sio)
        self.assertNotIn("fault:write:observed", sio)

    def test_gate_stack_stays_within_its_budget(self):
        _z, min_sp, _steps = self.full_run()
        depth = self.syms["zos_gate_stack_top"] - min_sp
        self.assertGreater(depth, 0, "the gate stack was never entered")
        self.assertLessEqual(depth, GATE_STACK_SIZE,
                             "gate-stack depth %d exceeds the gate stack"
                             % depth)
        print("m9: worst-case gate-stack depth %d of %d bytes"
              % (depth, GATE_STACK_SIZE))

    def test_faults_reach_the_provider_when_armed(self):
        def on_fence(marker):
            if marker == STALE_ARM:
                z.peer.fs2_read_fault = "stale"
            elif marker == STALE_END:
                z.peer.fs2_read_fault = None
            elif marker == WRITE_ARM:
                z.peer.fs2_write_fault = "done_fail"
            elif marker == WRITE_END:
                z.peer.fs2_write_fault = None

        z = self.make_cpu()
        fences = [STALE_ARM, "m9 selftest: fault:stale:observed\r\n",
                  STALE_END, WRITE_ARM,
                  "m9 selftest: fault:write:observed\r\n", WRITE_END]
        self.run_kernel(z, [MONITOR] + list(FAIL_MARKERS),
                        fences=fences, on_fence=on_fence)
        sio = self.transcript(z)
        self.assertIn("m9 selftest: fault:stale:observed\r\n", sio)
        self.assertIn("m9 selftest: fault:write:observed\r\n", sio)
        self.assertIn(SELFTEST_OK, sio)
        self.assertNotIn(SELFTEST_FAIL, sio)
        self.assertEqual(
            z.bank7[self.syms["_m9_integration_selftest_result"]], 0)

    def test_absent_card_reports_the_degraded_path(self):
        z = self.make_cpu(fs2_media=False)
        self.run_kernel(z, [MONITOR] + list(FAIL_MARKERS))
        sio = self.transcript(z)
        self.assertIn("vfs: / unavailable 0x", sio)
        self.assertIn(SELFTEST_ABSENT, sio)
        self.assertNotIn(SELFTEST_FAIL, sio)
        self.assertNotIn("PANIC", sio)
        # `/SYSTEM` and the console are unaffected.
        self.assertIn("console: dynamic sio0.b OK", sio)
        self.assertEqual(
            z.bank7[self.syms["_m9_integration_selftest_result"]], 0)

    def test_write_protected_card_takes_the_erofs_path(self):
        z = self.make_cpu(fs2_read_only=True)
        self.run_kernel(z, [MONITOR] + list(FAIL_MARKERS))
        sio = self.transcript(z)
        self.assertIn(SELFTEST_EROFS, sio)
        self.assertNotIn(SELFTEST_FAIL, sio)
        self.assertNotIn("fstest:", sio)
        self.assertEqual(
            z.bank7[self.syms["_m9_integration_selftest_result"]], 0)


if __name__ == "__main__":
    unittest.main()
