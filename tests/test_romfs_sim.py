#!/usr/bin/env python3
"""Host-side ROMFS functional test: replay the current boot path, the
common ROM-access primitive, and the ROMFS reader in the Z80 simulator.

Coverage (all executing the real linked bytes):

* boot of the subsystem acceptance image: the store is read through the
  common ROM-access primitive (validation-before-mapping, the mirrored
  mode-00 stub), the console binds, and the target selftest prints OK;
* the common ROM-access primitive (kernel/romfs/rom_access.asm):
  validation-before-mapping, exact latch/IFF restoration, a stackless
  mode-00 window that executes only the mirrored stub in the ROM page,
  bounded staging, page selection, window-edge rules, and reentry
  rejection;
* the ROMFS reader (kernel/romfs/romfs.c): header/table validation,
  lookup semantics, and rejection of mutated header/table metadata;
* the corrupt-store boot: the packed store's header reserved field is
  patched in the loaded image (the bytes tests/subsys/corrupt_romfs.py
  used to build a second image), and the boot must panic through the
  early console before any module execution.

Skipped when the images / packed modules have not been built.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from replay import RET, Replay, STACK  # noqa: E402
import modpack  # noqa: E402

STEP_LIMIT = 60000000
SCRATCH_SIZE = 0x200      # host scratch is derived per image in setUp

ROMFS_PAGE_WINDOW = 0xC000
ROMFS_FIRST_PAGE = 2

KOS_EINVAL = 0x02
KOS_ENOENT = 0x05
KOS_EBUSY = 0x04


def pattern_byte(i):
    """Byte i of the deterministic cross-page asset."""
    return ((i & 0xFF) ^ ((i >> 8) & 0xFF)) & 0xFF


def run_function(z, addr, limit=STEP_LIMIT, on_step=None):
    """Call the function at `addr` and run until it returns."""
    z.push16(RET)
    z.pc = addr
    for _ in range(limit):
        z.step()
        if on_step is not None:
            on_step(z)
        if z.pc == RET:
            return
    raise AssertionError("function at %04X did not return (PC=%04X)"
                         % (addr, z.pc))


def call2(z, addr, hl, de, limit=STEP_LIMIT):
    """Call int f(const char *, const char *) -> DE."""
    z.push16(RET)
    z.hl = hl
    z.de = de
    z.pc = addr
    for _ in range(limit):
        z.step()
        if z.pc == RET:
            return z.de
    raise AssertionError("function at %04X did not return" % addr)


class RomfsSimBase(Replay, unittest.TestCase):
    IMAGE_SUBDIR = "build-subsys-selftest"
    IMAGE_NAME = "konaos-subsys-selftest.bin"

    def setUp(self):
        self.load_image()
        self.mod_path = os.path.join(ROOT, "build", "modules", "sio.mod")
        if not os.path.exists(self.mod_path):
            self.skipTest("build/modules/sio.mod missing; run make first")
        with open(self.mod_path, "rb") as fh:
            self.sio_mod = fh.read()
        self.romfs_entries = self.romfs_entries()
        self.sio_entry = self.romfs_entries["sio.mod"]
        self.cross_entry = self.romfs_entries["crosspage.bin"]
        self.scratch = self._derive_scratch()

    def _derive_scratch(self):
        """A free window address above the linked cold data.

        The host scratch cannot use a fixed address: the window is
        derived.  It sits above the cold data run and below the common
        region, inside the mode-11 window the cold code writes."""
        geometry = self.geometry()
        end = (geometry["KOS_WINDOW_DATA_BASE"] +
               geometry["KOS_WINDOW_DATA_SIZE"])
        scratch = (end + 0x1FF) & ~0x1FF
        self.assertLessEqual(scratch + SCRATCH_SIZE, 0xE000,
                             "scratch %04X does not fit the window"
                             % scratch)
        return scratch

    def boot_kernel(self, z, on_step=None, stop_at=None):
        """__kos_crt0_init + kernel_main replay with the connect delay
        skipped (its exported entry symbol is recognized).

        `stop_at` names a byte string in the serial transcript; the
        current boot path ends in the monitor's input loop rather than a
        halt, so acceptance replays stop when their group reports.  The
        interrupt-executive group needs the host to stand in for CTC0
        (the simulator has no timer model), so the injector runs until
        the caller's marker stops the replay."""
        inject = self.injector(z)

        def watch(cpu):
            inject(cpu)
            if on_step is not None:
                on_step(cpu)

        self.start_kernel(z)
        if stop_at is None:
            return self.run_until(z, (), on_step=watch)
        return self.run_until(z, [stop_at], on_step=watch)

    def patch_rom(self, z, logical, data):
        """Overwrite logical ROMFS stream bytes in the simulated ROM."""
        for i, value in enumerate(data):
            at = logical + i
            page = ROMFS_FIRST_PAGE + at // ROMFS_PAGE_WINDOW
            offset = at % ROMFS_PAGE_WINDOW
            z.rom[page][offset] = value

    def rom_byte(self, z, logical):
        page = ROMFS_FIRST_PAGE + logical // ROMFS_PAGE_WINDOW
        return z.rom[page][logical % ROMFS_PAGE_WINDOW]

    def init_romfs(self, z):
        z.push16(RET)
        z.pc = self.syms["_romfs_init"]
        for _ in range(STEP_LIMIT):
            z.step()
            if z.pc == RET:
                return z.de
        raise AssertionError("romfs_init did not return")

    def lookup(self, z, name, out_addr):
        # M9 has exactly one ROMFS lookup and it is ASCII case-folded
        # (architecture section 32.13).
        name_at = self.scratch + 0x100
        z.bank7[name_at:name_at + len(name) + 1] = name + b"\x00"
        return call2(z, self.syms["_romfs_lookup_ci"], name_at, out_addr)

    def call_rom_access(self, z, page, offset, length, req_addr=None,
                        on_step=None):
        req_addr = req_addr or self.scratch
        z.bank7[req_addr] = page
        z.bank7[req_addr + 1] = offset & 0xFF
        z.bank7[req_addr + 2] = (offset >> 8) & 0xFF
        z.bank7[req_addr + 3] = length & 0xFF
        z.bank7[req_addr + 4] = (length >> 8) & 0xFF
        z.push16(RET)
        z.hl = req_addr
        z.pc = self.syms["_rom_access_copy"]
        for _ in range(STEP_LIMIT):
            z.step()
            if on_step is not None:
                on_step(z)
            if z.pc == RET:
                return z.a
        raise AssertionError("rom_access_copy did not return")

    def staging(self, z):
        at = self.syms["_rom_access_staging"]
        return bytes(z.bank0[at:at + 0x80])

    def window_bytes(self, z, page, offset, length):
        return bytes(z.rom[page][offset:offset + length])


class RomWindowWatcher:
    """Assert the ROM-visible window is stackless and runs only the
    mirrored stub, and record the mode-00 latch values used."""

    COMMON_BASE = 0xE000

    def __init__(self, sync_base, sync_end):
        self.sync_base = sync_base
        self.sync_end = sync_end
        self.shadow_steps = 0
        self.shadow_latches = set()
        self._window_sp = None

    def __call__(self, z):
        if (z.latch & 0x18) == 0x00:
            self.shadow_steps += 1
            self.shadow_latches.add(z.latch)
            if not (self.sync_base <= z.pc < self.sync_end):
                raise AssertionError(
                    "PC %04X is outside the mirrored ROM-access stub "
                    "%04X-%04X while ROM is visible"
                    % (z.pc, self.sync_base, self.sync_end - 1))
            if self._window_sp is None:
                self._window_sp = z.sp
            elif z.sp != self._window_sp:
                raise AssertionError(
                    "SP changed during the ROM-visible window (%04X -> "
                    "%04X); the stub must be stackless"
                    % (self._window_sp, z.sp))
            if z.sp < self.COMMON_BASE:
                raise AssertionError(
                    "SP %04X is not on a common stack while ROM is visible"
                    % z.sp)
        else:
            self._window_sp = None


class TestSubsystemRomfsReplay(RomfsSimBase):
    def test_boot_loads_sio_from_romfs_and_selftest_passes(self):
        z = self.make_cpu()
        watcher = RomWindowWatcher(self.syms["rom_access_sync"],
                                   self.syms["rom_access_sync_end"])
        self.boot_kernel(z, on_step=watcher, stop_at=b"process selftest: OK")

        tx = bytes(z.sio_tx)
        self.assertIn(b"KonaOS 0.0\r\n", tx)
        self.assertIn(b"console: driver OK\r\n", tx)
        self.assertIn(b"romfs selftest: OK\r\n", tx)
        self.assertNotIn(b"PANIC", tx)
        self.assertEqual(z.bank7[self.syms["_romfs_selftest_result"]], 0x00,
                         "romfs selftest failed with code 0x%02X"
                         % z.bank7[self.syms["_romfs_selftest_result"]])

        # The transition ran through the mirrored stub in mode 00 with no
        # stack use (asserted per step), on the configured recovery pages.
        self.assertGreater(watcher.shadow_steps, 0,
                           "no ROM-visible steps observed; the replay is "
                           "not exercising the mapping transition")
        # Every mode-00 window must be one of the recovery pages the boot
        # module's payload actually spans.
        first_page = ROMFS_FIRST_PAGE + (self.sio_entry["offset"]
                                         // ROMFS_PAGE_WINDOW)
        last_page = ROMFS_FIRST_PAGE + ((self.sio_entry["offset"]
                                         + self.sio_entry["size"] - 1)
                                        // ROMFS_PAGE_WINDOW)
        # Every observed window must be one of the configured recovery
        # pages, and the boot module's own pages must have been read
        # through the mapping transition (the module really came from
        # ROM).  The acceptance groups deliberately touch further entries
        # (fixtures and EXE programs), which is why the set is a superset.
        configured = {page << 5
                      for page in range(ROMFS_FIRST_PAGE,
                                        ROMFS_FIRST_PAGE + 6)}
        self.assertTrue(watcher.shadow_latches <= configured,
                        "unexpected mode-00 latch values %r"
                        % sorted(watcher.shadow_latches))
        module_pages = {page << 5
                        for page in range(first_page, last_page + 1)}
        self.assertTrue(module_pages <= watcher.shadow_latches,
                        "the boot module's recovery pages %r were never "
                        "read through the mode-00 window (saw %r)"
                        % (sorted(module_pages),
                           sorted(watcher.shadow_latches)))

        # Exact latch restoration: the boot path returns to mode 11 with
        # the OS configuration latch restored.  The bank field is not
        # asserted here because the acceptance groups run the loader and
        # the EXE children -- which select their own banks -- before the
        # marker this replay stops at; the mode field is the invariant.
        # The ROM window is closed (mode 11, never mode 00) and the live
        # configuration bits belong to the bound console module.  The
        # primitive's exact latch/IFF restoration is pinned by the
        # dedicated replay in TestRomAccessPrimitive below.
        self.assertEqual(z.latch & 0x18, 0x18,
                         "boot path left memory mode 11 (latch 0x%02X)"
                         % z.latch)

        # The boot module is resident; no recovery-page byte was written.
        z.halted = False  # leave the halt loop so helpers can run
        for page in range(ROMFS_FIRST_PAGE, 8):
            self.assertEqual(bytes(z.rom[page]),
                             self.image[page * 0x10000:(page + 1) * 0x10000],
                             "recovery ROM page %d was modified" % page)

    def test_mutated_asset_is_detected_by_selftest(self):
        """Negative control: one wrong payload byte makes the target
        selftest fail with the matching code (the checks are real)."""
        z = self.make_cpu()
        at = self.cross_entry["offset"]
        self.patch_rom(z, at, bytes([self.rom_byte(z, at) ^ 0xFF]))
        self.boot_kernel(z, stop_at=b"romfs selftest: FAIL 0x06")
        tx = bytes(z.sio_tx)
        self.assertIn(b"romfs selftest: FAIL 0x06\r\n", tx)
        self.assertNotIn(b"romfs selftest: OK", tx)
        self.assertEqual(z.bank7[self.syms["_romfs_selftest_result"]], 0x06)

    def test_mutated_container_byte_fails_the_checksum(self):
        z = self.make_cpu()
        # Flip one byte inside the container payload in the ROMFS stream
        # (past the relocation table, so the loader still succeeds).
        info = modpack.parse_container(self.sio_mod, "sio.mod")
        at = self.sio_entry["offset"] + info["payload_offset"] + 0x30
        self.patch_rom(z, at, bytes([self.rom_byte(z, at) ^ 0x01]))
        self.boot_kernel(z, stop_at=b"romfs selftest: FAIL")
        tx = bytes(z.sio_tx)
        self.assertIn(b"romfs selftest: FAIL 0x04\r\n", tx)


class TestCorruptStoreReplay(RomfsSimBase):
    """Corrupt-store boot: the header reserved field of the store the
    subsystem image carries is patched in the loaded ROM (the exact
    bytes tests/subsys/corrupt_romfs.py used to write a second image),
    and the kernel must reject the whole store through the early-console
    panic before any module code runs."""

    def test_corrupt_store_panics_before_module_execution(self):
        z = self.make_cpu()
        self.patch_rom(z, 10, b"\x01")  # header reserved := 1
        self.start_kernel(z)
        self.run_until(z, (), halt_ok=True, panic=None)

        tx = bytes(z.sio_tx)
        self.assertIn(b"KonaOS 0.0\r\n", tx)
        header_hex = " ".join(
            "%02X" % byte
            for byte in self.image[2 * 0x10000:2 * 0x10000 + 8])
        self.assertIn(("romfs: init failed, code 0x0A, latch 0x18, "
                       "header %s\r\n" % header_hex).encode("ascii"), tx)
        self.assertIn(b"PANIC: romfs: corrupt recovery image\r\n", tx)
        self.assertNotIn(b"console: driver OK\r\n", tx)
        self.assertNotIn(b"console: driver failed\r\n", tx)


class TestRomAccessPrimitive(RomfsSimBase):
    def setUp(self):
        super().setUp()
        self.expected_page2 = bytes(self.image[2 * 0x10000:
                                               2 * 0x10000 + ROMFS_PAGE_WINDOW])
        self.expected_page3 = bytes(self.image[3 * 0x10000:
                                               3 * 0x10000 + ROMFS_PAGE_WINDOW])

    def test_valid_copy_stages_window_bytes(self):
        z = self.make_cpu()
        watcher = RomWindowWatcher(self.syms["rom_access_sync"],
                                   self.syms["rom_access_sync_end"])
        status = self.call_rom_access(z, 2, 0x0100, 16, on_step=watcher)
        self.assertEqual(status, 0)
        self.assertEqual(self.staging(z)[:16], self.expected_page2[0x100:0x110])
        self.assertEqual(z.latch, 0x18)
        self.assertEqual(z.sp, STACK)
        self.assertGreater(watcher.shadow_steps, 0)
        self.assertEqual(watcher.shadow_latches, {2 << 5})

    def test_page_selection_reads_the_requested_page(self):
        z = self.make_cpu()
        status = self.call_rom_access(z, 3, 0x0000, 32)
        self.assertEqual(status, 0)
        self.assertEqual(self.staging(z)[:32], self.expected_page3[:32])
        self.assertNotEqual(self.expected_page3[:32], self.expected_page2[:32])

    def test_window_edge_is_allowed(self):
        z = self.make_cpu()
        status = self.call_rom_access(z, 2, 0xBF80, 0x80)
        self.assertEqual(status, 0)
        self.assertEqual(self.staging(z)[:0x80],
                         self.expected_page2[0xBF80:0xC000])

    def test_window_overrun_is_rejected_before_mapping(self):
        z = self.make_cpu()
        canary = self.staging(z)
        latch = z.latch
        for page, offset, length in ((2, 0xBF81, 0x80),
                                     (2, 0xC000, 1),
                                     (7, 0xBFFF, 2)):
            status = self.call_rom_access(z, page, offset, length)
            self.assertEqual(status, KOS_EINVAL,
                             "page %d offset %04X len %04X accepted"
                             % (page, offset, length))
        self.assertEqual(z.latch, latch)
        self.assertEqual(self.staging(z), canary)

    def test_invalid_page_length_and_null_are_rejected(self):
        z = self.make_cpu()
        for page, offset, length in ((1, 0, 1), (8, 0, 1), (2, 0, 0),
                                     (2, 0, 0x81), (2, 0xBF80, 0x81)):
            self.assertEqual(
                self.call_rom_access(z, page, offset, length), KOS_EINVAL,
                "accepted page %d offset %04X len %04X"
                % (page, offset, length))
        # Null request pointer.
        z.push16(RET)
        z.hl = 0x0000
        z.pc = self.syms["_rom_access_copy"]
        for _ in range(STEP_LIMIT):
            z.step()
            if z.pc == RET:
                break
        self.assertEqual(z.a, KOS_EINVAL)

    def test_reentry_is_rejected_with_busy(self):
        z = self.make_cpu()
        canary = self.staging(z)
        busy_at = self.syms["rom_access_busy"]
        z.bank0[busy_at] = 1
        try:
            status = self.call_rom_access(z, 2, 0, 16)
        finally:
            z.bank0[busy_at] = 0
        self.assertEqual(status, KOS_EBUSY)
        self.assertEqual(self.staging(z), canary)

    def test_exact_latch_restore_for_a_foreign_bank_field(self):
        """The saved latch byte is restored exactly, not normalized: bank
        bits are preserved across the mode-00 transition."""
        z = self.make_cpu(latch=0x18 | 3)
        status = self.call_rom_access(z, 2, 0, 8)
        self.assertEqual(status, 0)
        self.assertEqual(z.latch, 0x18 | 3)
        self.assertEqual(self.staging(z)[:8], self.expected_page2[:8])

    def test_iff_state_is_saved_and_restored(self):
        for enabled in (0, 1):
            z = self.make_cpu()
            z.iff1 = bool(enabled)
            z.iff2 = bool(enabled)
            status = self.call_rom_access(z, 2, 0x40, 8)
            self.assertEqual(status, 0)
            self.assertEqual(z.iff1, bool(enabled),
                             "IFF1 not restored for enabled=%d" % enabled)
            self.assertEqual(z.iff2, bool(enabled),
                             "IFF2 not restored for enabled=%d" % enabled)

    def test_last_recovery_page_is_usable(self):
        z = self.make_cpu()
        status = self.call_rom_access(z, 7, 0x00, 0x80)
        self.assertEqual(status, 0)
        expected = bytes(self.image[7 * 0x10000:7 * 0x10000 + 0x80])
        self.assertEqual(self.staging(z)[:0x80], expected)


class TestRomfsReaderRejections(RomfsSimBase):
    """Mutated header/table metadata must fail init as a whole."""

    def boot_romfs(self):
        z = self.make_cpu()
        run_function(z, self.syms["__kos_crt0_init"])
        return z

    def assert_init_rejects(self, mutation):
        z = self.boot_romfs()
        mutation(z)
        self.assertNotEqual(self.init_romfs(z), 0,
                            "romfs_init accepted corrupted metadata")
        # Not published: lookups must fail with EINVAL (never ENOENT).
        out = self.scratch + 0x40
        self.assertEqual(self.lookup(z, b"sio.mod", out), (-KOS_EINVAL) & 0xFFFF)

    def test_valid_image_initializes(self):
        z = self.boot_romfs()
        self.assertEqual(self.init_romfs(z), 0)
        # Known entry: name/size/flags from the packed container.
        out = self.scratch + 0x40
        self.assertEqual(self.lookup(z, b"sio.mod", out), 0)
        self.assertEqual(z.bank7[out + 4] | (z.bank7[out + 5] << 8)
                         | (z.bank7[out + 6] << 16)
                         | (z.bank7[out + 7] << 24), len(self.sio_mod))
        self.assertEqual(z.bank7[out + 8] | (z.bank7[out + 9] << 8), 1)
        self.assertEqual(self.lookup(z, b"absent.mod", out),
                         (-KOS_ENOENT) & 0xFFFF)
        # The single lookup folds case: any spelling finds the same entry.
        for spelling in (b"SIO.MOD", b"Sio.Mod", b"sio.MOD"):
            self.assertEqual(self.lookup(z, spelling, out), 0,
                             "%r must resolve" % spelling)
            self.assertEqual(z.bank7[out + 4] | (z.bank7[out + 5] << 8)
                             | (z.bank7[out + 6] << 16)
                             | (z.bank7[out + 7] << 24),
                             len(self.sio_mod))

    def test_bad_magic(self):
        self.assert_init_rejects(
            lambda z: self.patch_rom(z, 0, b"XRFS"))

    def test_bad_version(self):
        self.assert_init_rejects(lambda z: self.patch_rom(z, 4, b"\x02"))

    def test_bad_entry_size(self):
        self.assert_init_rejects(lambda z: self.patch_rom(z, 5, b"\x20"))

    def test_nonzero_header_reserved(self):
        self.assert_init_rejects(lambda z: self.patch_rom(z, 10, b"\x01\x00"))

    def test_mismatched_directory_size(self):
        self.assert_init_rejects(lambda z: self.patch_rom(z, 8, b"\x24\x00"))

    def test_image_size_beyond_capacity(self):
        self.assert_init_rejects(
            lambda z: self.patch_rom(z, 12, b"\x00\x00\x90\x00"))

    def test_unsupported_entry_flags(self):
        # First entry flags field is at header 16 + 32.
        self.assert_init_rejects(lambda z: self.patch_rom(z, 16 + 32, b"\x07\x00"))

    def test_entry_reserved_nonzero(self):
        self.assert_init_rejects(lambda z: self.patch_rom(z, 16 + 34, b"\x01\x00"))

    def test_entry_offset_into_table(self):
        self.assert_init_rejects(
            lambda z: self.patch_rom(z, 16 + 24, b"\x10\x00\x00\x00"))

    def test_entry_size_leaves_image(self):
        self.assert_init_rejects(
            lambda z: self.patch_rom(z, 16 + 28, b"\xFF\xFF\xFF\x00"))

    def test_unterminated_name(self):
        self.assert_init_rejects(
            lambda z: self.patch_rom(z, 16, b"X" * 24))

    def test_empty_name(self):
        self.assert_init_rejects(
            lambda z: self.patch_rom(z, 16, b"\x00"))


if __name__ == "__main__":
    unittest.main()
