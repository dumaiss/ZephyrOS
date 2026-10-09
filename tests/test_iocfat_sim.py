"""M9 IOC-FAT provider acceptance replay.

Runs the `m9-iocfat-selftest` image in the Z80 simulator.  The image boots
the production boot set minus HID/V9958 (SIO fallback, the IOC transport and
the resident `iocfat.zmod`), and the production `config/mounts.json` attaches
`/` to `iocfat` as an optional mount on `ioc.0`.  The simulator's
`IocCommandPeer` models the MCU FatFs service (FS2) behind the command lane,
so the provider is exercised end to end without a card.

The target hook drives the provider through the ordinary kernel VFS and then
exec's the native `fstest.zex`, whose IOC-FAT section repeats the key paths
through the public `FS_*` syscalls.  Because the host peer sees every FS2
command, the cache rules are proven by counting `FS2_READ` between the
`fstest: MARK` / `m9 iocfat: cache:N` fences rather than by a module-side
counter, and the gate-stack high-water mark through IOC-FAT is sampled while
the user-side section runs (the only path that uses the gate stack).
"""

import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import (Z80, IocCommandPeer, V9958Peer, read_symbols,
                    skip_serial_connect_delay)  # noqa: E402

RET = 0x1234
STACK = 0xFFFE
STEP_LIMIT = 60000000

IMAGE_SUBDIR = "build-m9-iocfat-selftest"
IMAGE_NAME = "zephyros-m9-iocfat-selftest.bin"

SELFTEST_OK = "m9 iocfat selftest: OK\r\n"
SELFTEST_ABSENT = "m9 iocfat selftest: OK (card absent)\r\n"
SELFTEST_FAIL = "m9 iocfat selftest: FAIL 0x"
FSTEST_PASS = "fstest: PASS 4/4\r\n"
EROFS_NOTE = "m9 iocfat: read-only card: EROFS enforced\r\n"
FSTEST_NOTE = "m9 iocfat: fstest iocfat section passed\r\n"
ABSENT_NOTE = "m9 iocfat: card absent: / not mounted\r\n"

CMD_FS2_READ = 0x36
GATE_DEPTH_LIMIT = 224
GATE_STACK_SIZE = 256

# FS2_READ counts between consecutive fence markers.  The keys are the
# marker that closes a window; the first entry is measured from `cache:0`.
CACHE_READ_DELTAS = [
    ("cache:1", 1),          # fill block 0
    ("cache:2", 0),          # the identical read reuses the slot
    ("cache:3", 1),          # 64 bytes at 500: only block 1 is fetched
    ("cache:4", 1),          # block 2
    ("cache:5", 1),          # block 0 again
    ("cache:6", 1),          # a second handle owns the slot
    ("cache:7", 0),
    ("cache:8", 1),          # the writing file's own confirmed write
    ("cache:9", 1),          # the other open file's confirmed write
    ("cache:10", 0),
    ("cache:11", 1),         # close released the slot
    ("cache:stale:end", 1),  # the STALE read really hit the lane
    ("cache:12", 1),         # STALE dropped the slot, so the next read fetches
    ("cache:13", 1),         # close released the slot again
]


class M9IocFatSimBase(unittest.TestCase):
    def setUp(self):
        self.bin_path = os.path.join(ROOT, IMAGE_SUBDIR, IMAGE_NAME)
        if not os.path.exists(self.bin_path):
            self.skipTest("%s missing; build the image first" % self.bin_path)
        with open(self.bin_path, "rb") as handle:
            self.image = handle.read()
        self.syms = read_symbols(os.path.join(ROOT, IMAGE_SUBDIR,
                                              "zephyros.map"))

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

    def run_kernel(self, z, stop, limit=STEP_LIMIT, fences=None,
                   on_fence=None, gate_window=None, start_fence=None):
        """Boot to one of `stop`, delivering fence callbacks in order.

        `fences` is a list of marker texts, or of tuples of alternatives for
        a point whose word the run chooses (only one can appear); whenever the
        next fence appears in the serial transcript it is popped and
        `on_fence(text)` is called, so the host can change the peer's fault
        state at a known point in the run.  When `gate_window` is given, the
        gate-stack high-water mark is measured only between `start_fence` and
        that window's marker.  Returns (min_sp, steps)."""
        gate_top = self.syms["zos_gate_stack_top"]
        gate_base = gate_top - GATE_STACK_SIZE
        min_sp = gate_top
        tracking = gate_window is None
        buf = bytearray()
        seen_len = 0
        pending = list(fences or [])
        stops = [s.encode() for s in stop]
        self.crt0(z)
        z.push16(RET)
        z.pc = self.syms["_kernel_main"]
        delay = self.syms["_kernel_serial_connect_delay"]
        for step in range(limit):
            skip_serial_connect_delay(z, delay)
            z.step()
            if z.sp < min_sp and gate_base <= z.sp <= gate_top:
                if tracking:
                    min_sp = z.sp
            if len(z.sio_tx) != seen_len:
                buf += bytes(z.sio_tx[seen_len:])
                seen_len = len(z.sio_tx)
                while pending:
                    entry = pending[0]
                    options = (entry if isinstance(entry, (tuple, list))
                               else (entry,))
                    marker = None
                    for option in options:
                        if option.encode() in buf:
                            marker = option
                            break
                    if marker is None:
                        break
                    pending.pop(0)
                    if gate_window is not None:
                        if marker == start_fence:
                            tracking = True
                        elif marker == gate_window:
                            tracking = False
                    if on_fence is not None:
                        on_fence(marker)
            if z.halted:
                break
            if any(s in z.sio_tx for s in stops):
                return min_sp, step
        raise AssertionError("acceptance never completed (PC=%04X)" % z.pc)

    def boot(self, **kwargs):
        z = self.make_cpu(**kwargs)
        min_sp, steps = self.run_kernel(z, [SELFTEST_OK, SELFTEST_FAIL,
                                            "PANIC"])
        return z, min_sp, steps

    def transcript(self, z):
        return z.sio_tx.decode("ascii", "replace")


class TestM9IocFatAcceptance(M9IocFatSimBase):
    def test_boot_mounts_iocfat_at_root(self):
        z, _min_sp, _steps = self.boot()
        sio = self.transcript(z)

        for line in ("ZephyrOS 0.0", "platform: Zephyr-80",
                     "kernel entry: OK", "serial: OK",
                     "console: service OK"):
            self.assertIn(line + "\r\n", sio)
        self.assertNotIn("unavailable", sio)
        self.assertNotIn("PANIC", sio)
        self.assertIn(SELFTEST_OK, sio)
        self.assertNotIn(SELFTEST_FAIL, sio)
        self.assertEqual(
            z.bank7[self.syms["_m9_iocfat_selftest_result"]], 0)

    def test_fstest_runs_its_iocfat_section(self):
        z, _min_sp, _steps = self.boot()
        sio = self.transcript(z)

        # fstest selects its section from the mount configuration, so the
        # ramfs-only cases are not run against a card.
        for case in ("namespace", "romfs", "iocfat", "window"):
            self.assertIn("fstest: %s OK" % case, sio)
        for case in ("ramfs", "errors", "pool", "stale", "writefail",
                     "exdev", "exec"):
            self.assertNotIn("fstest: %s" % case, sio)
        self.assertIn(FSTEST_PASS, sio)
        self.assertIn(FSTEST_NOTE, sio)
        self.assertNotIn("fstest: FAIL", sio)

    def test_writable_card_cases(self):
        z, _min_sp, _steps = self.boot()
        sio = self.transcript(z)

        self.assertIn("m9 iocfat: names:begin\r\n", sio)
        for note in ("read-only card", "card absent"):
            self.assertNotIn(note, sio)
        self.assertIn("m9 iocfat: fstest iocfat section passed\r\n", sio)

    def test_8_3_rejection_table_sends_no_fs2_command(self):
        # The name table is fenced by notes; the host peer counts every FS2
        # command, so a unit of traffic would be visible.
        counts = {}

        def snapshot(marker):
            counts[marker] = sum(z.peer.fs2_counts.values())

        z = self.make_cpu()
        z.peer.fs2_counts.clear()
        self.run_kernel(z, [SELFTEST_OK, SELFTEST_FAIL, "PANIC"],
                        fences=["names:begin", "names:end"],
                        on_fence=lambda m: counts.__setitem__(
                            m, sum(z.peer.fs2_counts.values())))
        self.assertIn("names:begin", counts)
        self.assertIn("names:end", counts)
        self.assertEqual(counts["names:end"] - counts["names:begin"], 0,
                         "a rejected 8.3 name reached the IOC lane")

    def test_cache_rules_are_exact_fs2_read_counts(self):
        # The host peer counts every FS2_READ, so each fence pair pins one
        # exact expectation: a repeated read must not fetch at all, a
        # cross-block read must fetch only the block that is not resident,
        # and a confirmed write, a STALE or a close must drop the slot.
        baseline = "cache:0"
        outcome = ("cache:stale:observed", "cache:stale:fresh")
        measured = [m for m, _ in CACHE_READ_DELTAS]
        reads = []

        def on_fence(marker):
            if marker == "cache:stale:arm":
                z.peer.fs2_read_fault = "stale"
            elif marker in outcome:
                z.peer.fs2_read_fault = None
            if marker == baseline or marker in measured:
                reads.append((marker, z.peer.fs2_counts[CMD_FS2_READ]))

        z = self.make_cpu()
        fences = [baseline] + measured
        fences.insert(fences.index("cache:stale:end"), outcome)
        fences.insert(fences.index(outcome), "cache:stale:arm")
        self.run_kernel(z, [SELFTEST_OK, SELFTEST_FAIL, "PANIC"],
                        fences=fences, on_fence=on_fence)

        sio = self.transcript(z)
        self.assertIn("m9 iocfat selftest: OK\r\n", sio)
        self.assertIn("cache:stale:observed",
                      sio.replace("\r\n", " "),
                      "the armed STALE fault did not reach the provider")
        self.assertEqual([m for m, _ in reads], [baseline] + measured)
        for i, (marker, expected) in enumerate(CACHE_READ_DELTAS):
            served = reads[i + 1][1] - reads[i][1]
            self.assertEqual(
                served, expected,
                "FS2_READ delta up to %s: expected %d, the peer served %d"
                % (marker, expected, served))

    def test_gate_stack_depth_through_iocfat(self):
        # The kernel-side cases run on the kernel stack; only the user-side
        # fstest section issues FS_* syscalls, so the high-water mark is
        # sampled between fstest's two markers.
        z = self.make_cpu()
        min_sp, _steps = self.run_kernel(
            z, [SELFTEST_OK, SELFTEST_FAIL, "PANIC"],
            fences=["fstest: MARK ioc0", "fstest: MARK io9"],
            gate_window="fstest: MARK io9", start_fence="fstest: MARK ioc0")
        sio = self.transcript(z)
        self.assertIn(FSTEST_PASS, sio)
        gate_top = self.syms["zos_gate_stack_top"]
        depth = gate_top - min_sp
        self.assertGreater(depth, 0, "the gate stack was never entered")
        self.assertLessEqual(
            depth, GATE_DEPTH_LIMIT,
            "gate-stack depth through IOC-FAT %d exceeds the %d-byte budget"
            % (depth, GATE_DEPTH_LIMIT))
        self.assertLessEqual(depth, GATE_STACK_SIZE,
                             "gate-stack depth %d exceeds the gate stack"
                             % depth)
        print("m9 iocfat: gate-stack depth through IOC-FAT %d of %d bytes"
              % (depth, GATE_DEPTH_LIMIT))

    def test_gate_stack_depth_stays_within_the_gate_stack(self):
        z, min_sp, _steps = self.boot()
        depth = self.syms["zos_gate_stack_top"] - min_sp
        self.assertGreater(depth, 0, "the gate stack was never entered")
        self.assertLessEqual(
            depth, GATE_STACK_SIZE,
            "worst-case gate-stack depth %d exceeds the %d-byte gate stack"
            % (depth, GATE_STACK_SIZE))
        print("m9 iocfat: worst-case gate-stack depth %d of %d bytes"
              % (depth, GATE_STACK_SIZE))

    def test_read_only_caps_take_the_erofs_branch(self):
        z, _min_sp, _steps = self.boot(
            fs2_read_only=True,
            fs2_files={"/ZOSTEST/RO.TXT": b"read-only"})
        sio = self.transcript(z)

        self.assertIn(EROFS_NOTE, sio)
        self.assertIn("m9 iocfat selftest: OK\r\n", sio)
        self.assertNotIn(SELFTEST_FAIL, sio)
        # No writable card, so the user-side writable section is not run.
        self.assertNotIn("fstest:", sio)
        # The provider refuses from its CAPS-derived state, before the IOC
        # lane sees a mutation at all.
        for command in (0x40, 0x41, 0x42, 0x43, 0x44):   # TRUNC..RMDIR
            self.assertEqual(
                z.peer.fs2_counts[command], 0,
                "mutation 0x%02X reached a write-protected card" % command)
        self.assertEqual(
            z.bank7[self.syms["_m9_iocfat_selftest_result"]], 0)

    def test_stale_replay_takes_the_stale_branch(self):
        z, _min_sp, _steps = self.boot(fs2_stale_path="/ZOSTEST/ST.TXT")
        sio = self.transcript(z)

        self.assertIn("m9 iocfat: stale observed\r\n", sio)
        self.assertNotIn("m9 iocfat: fresh read", sio)
        self.assertIn("m9 iocfat selftest: OK\r\n", sio)

    def test_absent_card_leaves_system_and_console_usable(self):
        z = self.make_cpu(fs2_media=False)
        min_sp, _steps = self.run_kernel(
            z, [SELFTEST_ABSENT, SELFTEST_FAIL, "PANIC"])
        sio = self.transcript(z)

        self.assertIn("vfs: / unavailable 0x", sio)
        self.assertIn(ABSENT_NOTE, sio)
        self.assertIn(SELFTEST_ABSENT, sio)
        self.assertNotIn(SELFTEST_FAIL, sio)
        self.assertNotIn("PANIC", sio)
        # The recovery store and the console are unaffected: the target
        # selftest could only run from `/SYSTEM`, and it reported through
        # the SIO console.
        self.assertIn("console: service OK", sio)
        self.assertEqual(
            z.bank7[self.syms["_m9_iocfat_selftest_result"]], 0)
        depth = self.syms["zos_gate_stack_top"] - min_sp
        self.assertLessEqual(depth, GATE_STACK_SIZE)

    def test_unsupported_card_reports_unavailable(self):
        # A card whose CAPS carry another protocol version must not mount;
        # the kernel then reports the optional mount as unavailable and the
        # boot continues.
        z = self.make_cpu(fs2_caps_fault="version")
        _min_sp, _steps = self.run_kernel(
            z, [SELFTEST_ABSENT, SELFTEST_FAIL, "PANIC"])
        sio = self.transcript(z)

        self.assertIn("vfs: / unavailable 0x", sio)
        self.assertIn(SELFTEST_ABSENT, sio)
        self.assertNotIn("PANIC", sio)


if __name__ == "__main__":
    unittest.main()
