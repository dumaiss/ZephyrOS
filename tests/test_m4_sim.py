#!/usr/bin/env python3
"""Host-side functional test: replay the M4 loader and dynamic-driver
acceptance in the Z80 simulator.

Loader image (build-m4-loader-selftest), executing the real linked code:

  1. boot C-runtime init, irq_init, device_registry_init,
  2. the full `zos_run_m4_selftest` driver after a production-style M4
     boot (registry, static test drivers, boot catalog load, console
     bind): fixture relocation at two arena addresses, failed-init
     rollback, BSS zeroing, IRQ ownership, handler validation, and
     relative-count arena exhaustion,
  3. independent checks: crafted containers are rejected with the exact
     loader error codes and leave the arena untouched.

Acceptance image (build-m4-selftest): the boot catalog's IRQ self-test
SIO module is replayed with three injected CTC0 interrupts inside its
bounded EI spin; the transcript must show the dynamic console line, the
loader OK line, the reported load address, and the final OK line.

Placement image (build-m4-placement-selftest): the embedded module bytes
must be identical to build/modules/sio.zmod and relocated to the 9E00h
arena base.  Failure image (build-m4-failure): the failing module must be
rolled back exactly and the failure pass must report OK.

Skipped when the images have not been built.
"""

import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import Z80, read_symbols  # noqa: E402
import zmodpack  # noqa: E402

RET = 0x1234
STEP_LIMIT = 30000000
SCRATCH = 0xD000  # bank-7 scratch below the ISR stack, unused by the image

ZMOD_EINVAL = (-0x02) & 0xFFFF
ZMOD_ENOTSUP = (-0x07) & 0xFFFF
ZMOD_EFORMAT = (-0x0A) & 0xFFFF
ZMOD_ENOSPC = (-0x06) & 0xFFFF

FIXTURE_MODULES = os.path.join(ROOT, "build", "modules")


def run_function(z, addr, limit=STEP_LIMIT):
    """Call the function at `addr` and run until it returns."""
    z.push16(RET)
    z.pc = addr
    for _ in range(limit):
        z.step()
        if z.pc == RET:
            return
    raise AssertionError("function at %04X did not return (PC=%04X)"
                         % (addr, z.pc))


class M4SimBase(unittest.TestCase):
    IMAGE_SUBDIR = "build-m4-loader-selftest"
    IMAGE_NAME = "zephyros-m4-loader-selftest.bin"

    def setUp(self):
        self.bin_path = os.path.join(ROOT, self.IMAGE_SUBDIR, self.IMAGE_NAME)
        if not os.path.exists(self.bin_path):
            self.skipTest("%s missing; build the image first" % self.bin_path)
        image = open(self.bin_path, "rb").read()
        self.page0, self.page1 = image[:0x10000], image[0x10000:]
        self.syms = read_symbols(os.path.join(
            ROOT, self.IMAGE_SUBDIR, "zephyros.map"))

    def make_cpu(self):
        z = Z80(self.page0, self.page1)
        z.latch = 0x18          # mode 11 over bank 0 (post-boot state)
        z.sp = 0xFFFE           # common boot stack; not the IRQ stacks
        return z

    def boot_partial(self, z):
        """Boot far enough for the loader: C runtime, IRQ, registry."""
        run_function(z, self.syms["__zos_crt0_init"])
        run_function(z, self.syms["_irq_init"])
        run_function(z, self.syms["_device_registry_init"])

    def boot_production(self, z):
        """Mirror kernel_main's M4 device-layer section: registry, module
        loader, boot catalog load (the SIO module registers its device
        here), and the console bind."""
        self.boot_partial(z)
        run_function(z, self.syms["_module_loader_init"])
        run_function(z, self.syms["_module_catalog_load_all"])
        run_function(z, self.syms["_console_bind_defaults"])

    def read_blob(self, z, symbol):
        """Read a packed fixture container from ROM page 1."""
        size_addr = self.syms[symbol + "_size"]
        size = z.bank7[size_addr] | (z.bank7[size_addr + 1] << 8)
        addr = self.syms[symbol]
        return bytes(z.bank7[addr:addr + size])

    def call_module_load(self, z, blob_addr, size, name=0):
        """int module_load(name, blob, size); sdcccall(1) passes the
        third 16-bit argument on the stack, the result returns in DE."""
        z.push16(size)
        z.push16(RET)
        z.hl = name
        z.de = blob_addr
        z.pc = self.syms["_module_load"]
        for _ in range(STEP_LIMIT):
            z.step()
            if z.pc == RET:
                return z.de
        raise AssertionError("module_load did not return")

    def put_blob(self, z, data, addr=SCRATCH):
        z.bank7[addr:addr + len(data)] = data
        return addr

    def free_granules(self, z):
        run_function(z, self.syms["_module_free_granules"])
        return z.a

    def module_count(self, z):
        run_function(z, self.syms["_module_count"])
        return z.a

    def module_get_ptr(self, z, index):
        """Return the module_get(index) pointer (A=index, result in DE)."""
        z.push16(RET)
        z.a = index
        z.pc = self.syms["_module_get"]
        for _ in range(STEP_LIMIT):
            z.step()
            if z.pc == RET:
                return z.de
        raise AssertionError("module_get did not return")

    def module_geometry(self, z, index):
        """Read (load_base, image_size) from module_get(index) (A=index,
        pointer in DE; zmod_module fields are unpadded: load_base at +7,
        image_size at +9)."""
        z.push16(RET)
        z.a = index
        z.pc = self.syms["_module_get"]
        for _ in range(STEP_LIMIT):
            z.step()
            if z.pc == RET:
                break
        else:
            raise AssertionError("module_get did not return")
        ptr = z.de
        base = z.bank7[ptr + 7] | (z.bank7[ptr + 8] << 8)
        size = z.bank7[ptr + 9] | (z.bank7[ptr + 10] << 8)
        return base, size

    def module_name(self, z, index):
        """Read the resident module name at descriptor `index`."""
        ptr = self.module_get_ptr(z, index)
        if ptr == 0:
            return None
        # zmod_module: state(0) first_granule(1) granule_count(2) name(3).
        nptr = z.bank7[ptr + 3] | (z.bank7[ptr + 4] << 8)
        chars = []
        for i in range(24):
            byte = z.bank7[nptr + i]
            if byte == 0:
                break
            chars.append(byte)
        return bytes(chars).decode("ascii")

    def module_indices(self, z, name):
        """All resident module indices whose diagnostic name is `name`."""
        found = []
        for index in range(16):
            if self.module_name(z, index) == name:
                found.append(index)
        return found


class TestM4SelftestReplay(M4SimBase):
    def test_full_selftest_round_trip(self):
        z = self.make_cpu()
        self.boot_production(z)
        run_function(z, self.syms["_zos_run_m4_selftest"])
        self.assertEqual(z.bank7[self.syms["_m4_selftest_result"]], 0x00,
                         "m4 selftest failed with code 0x%02X"
                         % z.bank7[self.syms["_m4_selftest_result"]])
        tx = bytes(z.sio_tx)
        self.assertIn(b"m4 loader selftest: OK\r\n", tx)
        self.assertIn(b"m4 selftest: OK\r\n", tx)

    def test_arena_contains_relocated_probe_at_two_addresses(self):
        z = self.make_cpu()
        self.boot_production(z)
        run_function(z, self.syms["_zos_run_m4_selftest"])
        self.assertEqual(z.bank7[self.syms["_m4_selftest_result"]], 0x00)

        blob = self.read_blob(z, "_z4_fixture_probe")
        info = zmodpack.parse_container(blob, "probe.zmod")
        sites = {patch for _, patch in info["relocs"]}

        # Locate the two resident probe instances by name: the M8 API
        # fixture owns a descriptor slot between the boot module and the
        # probe loads, so slot indices are not stable.
        probe_slots = self.module_indices(z, "m4.probe")
        self.assertEqual(len(probe_slots), 2,
                         "expected two resident probe modules")
        first, first_size = self.module_geometry(z, probe_slots[0])
        second, second_size = self.module_geometry(z, probe_slots[1])
        self.assertEqual(first_size, info["image_size"])
        self.assertEqual(second_size, info["image_size"])
        self.assertGreater(second, first)
        self.assertEqual(first & 0x1FF, 0)
        self.assertEqual(second & 0x1FF, 0)
        delta = second - first

        # Relocated words differ by exactly the load-base delta; all other
        # image bytes are identical.
        for offset in range(first_size):
            a = z.bank7[first + offset]
            b = z.bank7[second + offset]
            if offset in sites or (offset - 1) in sites:
                continue
            self.assertEqual(a, b,
                             "non-relocation byte %04Xh differs" % offset)
        for patch in sites:
            word_a = (z.bank7[first + patch] |
                      (z.bank7[first + patch + 1] << 8))
            word_b = (z.bank7[second + patch] |
                      (z.bank7[second + patch + 1] << 8))
            self.assertEqual(word_b - word_a, delta,
                             "relocated word %04Xh not shifted by the "
                             "load-base delta" % patch)
        # BSS was zeroed before init; probe init wrote its magic marker.
        for base in (first, second):
            self.assertEqual(z.bank7[base + first_size], 0x31)
            self.assertEqual(z.bank7[base + first_size + 1], 0x4D)


class TestM4LoaderRejections(M4SimBase):
    def check_rejection(self, blob, expected, prepare=None):
        z = self.make_cpu()
        self.boot_partial(z)
        if prepare:
            prepare(z)
        addr = self.put_blob(z, blob)
        result = self.call_module_load(z, addr, len(blob))
        self.assertEqual(result, expected,
                         "expected 0x%04X, got 0x%04X" % (expected, result))
        self.assertEqual(self.free_granules(z), 16,
                         "arena was consumed by a rejected container")
        self.assertEqual(self.module_count(z), 0)
        return z

    def valid_container(self):
        rel = [
            ("_CODE", 4), ("_DATA", 2),
        ]
        # Build via the packer for a fully valid baseline.
        import tempfile
        from test_zmod import rel_text, pack_in_dir, simple_manifest
        text = rel_text(
            "t", rel,
            [(".__.ABS.", "def", 0, None), ("_entry", "def", 0, 0),
             ("_state", "def", 0, 1)],
            [(0, 0, bytes([0x21, 0x00, 0x00, 0xC9]),
              [(0x00, 0x05, 0x00)])])
        with tempfile.TemporaryDirectory() as tmp:
            container, _ = pack_in_dir(
                tmp, {"t.rel": text},
                simple_manifest(["t.rel"], "entry", bss_areas=("_DATA",)))
        return container

    def test_bad_magic(self):
        data = bytearray(self.valid_container())
        data[0] = ord("X")
        self.check_rejection(bytes(data), ZMOD_EINVAL)

    def test_bad_version(self):
        data = bytearray(self.valid_container())
        data[4] = 2
        self.check_rejection(bytes(data), ZMOD_ENOTSUP)

    def test_bad_type(self):
        data = bytearray(self.valid_container())
        data[5] = 2
        self.check_rejection(bytes(data), ZMOD_ENOTSUP)

    def test_bad_abi(self):
        data = bytearray(self.valid_container())
        data[6] = 9
        self.check_rejection(bytes(data), ZMOD_ENOTSUP)

    def test_bad_reserved_and_flags(self):
        data = bytearray(self.valid_container())
        data[7] = 1
        self.check_rejection(bytes(data), ZMOD_EINVAL)
        data = bytearray(self.valid_container())
        data[0x1B] = 1
        self.check_rejection(bytes(data), ZMOD_EINVAL)

    def test_truncated(self):
        data = self.valid_container()[:-1]
        self.check_rejection(data, ZMOD_EFORMAT)

    def test_granule_mismatch(self):
        data = bytearray(self.valid_container())
        data[0x0C] = 2  # header claims 2, image+bss need 1
        self.check_rejection(bytes(data), ZMOD_EFORMAT)

    def test_bad_relocation_type(self):
        data = bytearray(self.valid_container())
        reloc_offset = data[0x12] | (data[0x13] << 8)
        data[reloc_offset] = 2
        self.check_rejection(bytes(data), ZMOD_ENOTSUP)

    def test_relocation_bad_reserved(self):
        data = bytearray(self.valid_container())
        reloc_offset = data[0x12] | (data[0x13] << 8)
        data[reloc_offset + 1] = 1
        self.check_rejection(bytes(data), ZMOD_EINVAL)

    def test_relocation_crosses_image(self):
        data = bytearray(self.valid_container())
        reloc_offset = data[0x12] | (data[0x13] << 8)
        image_size = data[0x08] | (data[0x09] << 8)
        data[reloc_offset + 2] = (image_size - 1) & 0xFF
        data[reloc_offset + 3] = (image_size - 1) >> 8
        self.check_rejection(bytes(data), ZMOD_EFORMAT)

    def test_validation_never_touches_arena(self):
        z = self.make_cpu()
        self.boot_partial(z)
        z.bank7[0xA000] = 0xA5
        z.bank7[0xBFFF] = 0x5A
        bad = bytearray(self.valid_container())
        bad[0] = ord("X")
        addr = self.put_blob(z, bytes(bad))
        self.call_module_load(z, addr, len(bad))
        self.assertEqual(z.bank7[0xA000], 0xA5)
        self.assertEqual(z.bank7[0xBFFF], 0x5A)


class TestM4ArenaAllocation(M4SimBase):
    def test_same_blob_twice_then_exhaustion_rolls_back(self):
        z = self.make_cpu()
        self.boot_partial(z)
        run_function(z, self.syms["_module_loader_init"])

        # Load the real probe fixture twice from the embedded catalog.
        probe = self.read_blob(z, "_z4_fixture_probe")
        addr = self.put_blob(z, probe)
        self.assertEqual(self.call_module_load(z, addr, len(probe)), 0,
                         "first probe load failed")
        first_slot = self.module_count(z)
        self.assertEqual(first_slot, 1)
        self.assertEqual(self.call_module_load(z, addr, len(probe)), 1,
                         "second probe load failed")
        self.assertEqual(self.module_count(z), 2)
        self.assertEqual(self.free_granules(z), 14)

        # A failed init must release its 2-granule allocation, restore its
        # descriptor to EMPTY, and leave earlier modules RESIDENT.
        self.assertEqual(self.module_get_ptr(z, 3), 0)
        fail_blob = self.read_blob(z, "_z4_fixture_fail")
        fail_addr = self.put_blob(z, fail_blob)
        result = self.call_module_load(z, fail_addr, len(fail_blob))
        self.assertEqual(result, (-0x0B) & 0xFFFF)  # ZMOD_EINIT
        self.assertEqual(self.free_granules(z), 14)
        self.assertEqual(self.module_count(z), 2)
        self.assertNotEqual(self.module_get_ptr(z, 0), 0)
        self.assertNotEqual(self.module_get_ptr(z, 1), 0)
        self.assertEqual(self.module_get_ptr(z, 3), 0)  # failed load EMPTY
        run_function(z, self.syms["_module_failed_count"])
        self.assertEqual(z.a, 1)


class TestM4BootAcceptance(M4SimBase):
    """Full dynamic-driver acceptance replay on build-m4-selftest: the
    boot catalog's IRQ self-test SIO module claims CTC0, receives three
    injected interrupts inside its bounded EI spin, releases the source,
    registers sio0.b, and the console binds to it."""

    IMAGE_SUBDIR = "build-m4-selftest"
    IMAGE_NAME = "zephyros-m4-selftest.bin"

    REPORT = os.path.join(ROOT, "build", "modules", "sio-irq.report.json")

    def read_report(self):
        if not os.path.exists(self.REPORT):
            self.skipTest("build/modules/sio-irq.report.json missing; run "
                          "`make m4-selftest`")
        return json.load(open(self.REPORT))

    def symbol_offset(self, report, name):
        for key in (name, "_" + name):
            if key in report["symbols"]:
                return report["symbols"][key]
        raise AssertionError("sio-irq report has no symbol %s" % name)

    def call_uint8(self, z, symbol):
        """Call `symbol` and return its uint8_t result (in A)."""
        run_function(z, self.syms[symbol])
        return z.a

    def boot_with_ctc(self, z):
        """Mirror kernel_main's M4 acceptance section: the kernel starts
        CTC0 with the M2 lock held (a pre-claim tick stays pending instead
        of being dispatched unowned, which would quiesce the source), the
        boot module claims the source inside its relocated init and
        receives deliveries through its relocated handler, then the
        kernel stops the timer and restores the interrupt state."""
        report = self.read_report()
        self.boot_partial(z)
        run_function(z, self.syms["_module_loader_init"])

        self.assertTrue(z.iff1, "interrupts should be enabled at boot")
        enabled = self.call_uint8(z, "_irq_arch_lock")
        self.assertFalse(z.iff1, "M2 lock did not mask interrupts")
        run_function(z, self.syms["_zos_ctc0_test_start"])

        # One tick during the load prologue must stay pending: the source
        # is not claimed yet, and an unowned dispatch would quiesce CTC0
        # (the 2026-10-02 hardware failure).  The module's first EI takes
        # this pending tick through its relocated handler.
        z.assert_int(0x00)

        base = self.syms["__driver_arena_start"]
        hits_addr = base + self.symbol_offset(report, "sio_module_irq_hits")
        image_lo = base
        image_hi = base + report["image_size"]

        z.push16(RET)
        z.pc = self.syms["_module_catalog_load_all"]
        for _ in range(STEP_LIMIT):
            if (z.iff1 and not z.ei_delay and not z.int_pending and
                    image_lo <= z.pc < image_hi):
                hits = z.bank7[hits_addr] | (z.bank7[hits_addr + 1] << 8)
                if hits < 3:
                    z.assert_int(0x00)
            z.step()
            if z.pc == RET:
                break
        else:
            raise AssertionError("module_catalog_load_all did not return")
        load_failures = z.a
        run_function(z, self.syms["_zos_ctc0_test_stop"])
        z.a = enabled
        run_function(z, self.syms["_irq_arch_unlock"])
        self.assertTrue(z.iff1, "interrupt state was not restored")

        self.assertEqual(load_failures, 0, "catalog load reported failures")
        run_function(z, self.syms["_console_bind_defaults"])
        hits = z.bank7[hits_addr] | (z.bank7[hits_addr + 1] << 8)
        self.assertEqual(hits, 3, "module observed %d hits" % hits)
        run_function(z, self.syms["_irq_unclaimed_count"])
        self.assertEqual(z.de, 0, "an unowned CTC0 tick was dispatched")

    def test_full_acceptance_boot(self):
        z = self.make_cpu()
        self.boot_with_ctc(z)
        # The console must be bound to the dynamic SIO device (the
        # kernel_main transcript line itself is asserted from the image
        # in tests/test_m4.py).
        run_function(z, self.syms["_console_output_handle"])
        self.assertNotEqual(z.a, 0, "console output unbound")
        run_function(z, self.syms["_console_input_handle"])
        self.assertNotEqual(z.a, 0, "console input unbound")
        run_function(z, self.syms["_zos_run_m4_selftest"])
        self.assertEqual(z.bank7[self.syms["_m4_selftest_result"]], 0x00,
                         "m4 selftest failed with code 0x%02X"
                         % z.bank7[self.syms["_m4_selftest_result"]])
        tx = bytes(z.sio_tx)
        for line in (b"m4 loader selftest: OK\r\n",
                     b"m4 selftest: sio at ",
                     b"m4 selftest: OK\r\n"):
            self.assertIn(line, tx,
                          "acceptance transcript missing %r" % line)

    def test_boot_module_is_bound_in_the_arena(self):
        z = self.make_cpu()
        self.boot_with_ctc(z)
        run_function(z, self.syms["_module_count"])
        self.assertGreaterEqual(z.a, 1, "boot module not resident")
        base, size = self.module_geometry(z, 0)
        self.assertEqual(base, self.syms["__driver_arena_start"])
        report = self.read_report()
        self.assertEqual(size, report["image_size"])

    def test_unowned_ctc0_tick_quiesces_the_timer(self):
        """Executive contract and the 2026-10-02 hardware failure: a
        CTC0 tick dispatched while the source has no owner is answered
        with platform_irq_quiesce(), which resets and stops CTC0.  That
        is why kernel_main starts the timer only with the M2 lock held.
        The simulator does not model the CTC timer itself, so not
        injecting further ticks models the quiesced source."""
        z = self.make_cpu()
        self.boot_partial(z)
        run_function(z, self.syms["_module_loader_init"])
        self.assertTrue(z.iff1)
        run_function(z, self.syms["_zos_ctc0_test_start"])
        z.assert_int(0x00)  # first tick arrives before the module claims
        run_function(z, self.syms["_module_catalog_load_all"])
        load_failures = z.a
        run_function(z, self.syms["_irq_unclaimed_count"])
        self.assertEqual(z.de, 1, "unowned tick was not counted")
        self.assertNotEqual(load_failures, 0,
                            "module must fail with a quiesced timer")
        self.assertEqual(self.module_count(z), 0)
        run_function(z, self.syms["_module_failed_count"])
        self.assertEqual(z.a, 1)


class TestM4FailureAcceptance(M4SimBase):
    IMAGE_SUBDIR = "build-m4-failure"
    IMAGE_NAME = "zephyros-m4-failure.bin"

    def test_failure_boot_rolls_back_and_reports(self):
        z = self.make_cpu()
        self.boot_partial(z)
        run_function(z, self.syms["_module_loader_init"])
        run_function(z, self.syms["_module_catalog_load_all"])
        self.assertNotEqual(z.a, 0, "failing module was not reported")
        run_function(z, self.syms["_zos_run_m4_failure_selftest"])
        self.assertEqual(z.bank7[self.syms["_m4_selftest_result"]], 0x00,
                         "failure selftest failed with code 0x%02X"
                         % z.bank7[self.syms["_m4_selftest_result"]])
        self.assertIn(b"m4 failure selftest: OK\r\n", bytes(z.sio_tx))


class TestM4PlacementIdentity(M4SimBase):
    """The placement image must embed the unmodified sio.zmod bytes and
    relocate them to the shifted 9E00h arena base."""

    IMAGE_SUBDIR = "build-m4-placement-selftest"
    IMAGE_NAME = "zephyros-m4-placement-selftest.bin"

    def test_embedded_bytes_identical_and_relocated(self):
        z = self.make_cpu()
        self.boot_partial(z)
        run_function(z, self.syms["_module_loader_init"])
        run_function(z, self.syms["_module_catalog_load_all"])
        self.assertEqual(z.a, 0, "catalog load reported failures")
        base, size = self.module_geometry(z, 0)
        self.assertEqual(base, 0x9E00)

        blob = self.read_blob(z, "_z4_module_boot")
        packed_path = os.path.join(ROOT, "build", "modules", "sio.zmod")
        if not os.path.exists(packed_path):
            self.skipTest("build/modules/sio.zmod missing; run make")
        self.assertEqual(blob, open(packed_path, "rb").read(),
                         "placement image embeds modified module bytes")
        info = zmodpack.parse_container(blob, "sio.zmod")
        sites = sorted({patch for _, patch in info["relocs"]})
        self.assertGreater(len(sites), 0)
        patch = sites[0]
        word = z.bank7[base + patch] | (z.bank7[base + patch + 1] << 8)
        raw = info["payload"][patch] | (info["payload"][patch + 1] << 8)
        self.assertEqual(word, (raw + base) & 0xFFFF,
                         "relocation not applied to the shifted base")


if __name__ == "__main__":
    unittest.main()
