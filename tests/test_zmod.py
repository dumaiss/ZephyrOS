#!/usr/bin/env python3
"""Host-side M4 ZMOD packer/validator tests.

Pure tests build synthetic SDCC .rel text and exercise the packer,
validator, and container checks without any toolchain dependency.  The
fixture tests (skipped until `make zmod-test` has produced build/modules/)
re-pack the real fixture objects, compare the mini-linker output against
sdldz80 byte-for-byte, and round-trip the generated catalog assembly; the
SIO module variants (sio, sio-irq, sio-fail) are validated from the same
directory.
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)

import zmodpack  # noqa: E402
from maptools import evaluate, parse_c_header  # noqa: E402

MODULE_H = os.path.join(ROOT, "include", "zephyros", "module.h")
FIXTURE_MODULES = os.path.join(ROOT, "build", "modules")
FIXTURE_MANIFESTS = os.path.join(ROOT, "tests", "modules")


def rel_text(module, areas, symbols, chunks):
    """Build an XL4 .rel text file.

    areas:   list of (name, size)
    symbols: list of (name, "def"/"ref", value, area_index)
    chunks:  list of (area_index, addr, data, groups); groups are
             (mode, rtp, ref) tuples for the non-initial relocations.
    """
    lines = [
        "XL4",
        "H %d areas %X global symbols" % (len(areas), len(symbols)),
        "M %s" % module,
        "O -mz80 sdcccall(1)",
    ]
    # Refs and absolute defs precede the area records; each area's defs
    # follow its A line, exactly like sdasz80 output.
    for name, kind, value, area in symbols:
        if kind == "ref" or area is None:
            if kind == "def":
                lines.append("S %s Def%08X" % (name, value))
            else:
                lines.append("S %s Ref00000000" % name)
    for index, (name, size) in enumerate(areas):
        lines.append("A %s size %X flags 0 addr 0" % (name, size))
        for sym_name, kind, value, area in symbols:
            if kind == "def" and area == index:
                lines.append("S %s Def%08X" % (sym_name, value))
    for area_index, addr, data, groups in chunks:
        vals = [addr & 0xFF, (addr >> 8) & 0xFF, (addr >> 16) & 0xFF,
                (addr >> 24) & 0xFF] + list(data)
        lines.append("T " + " ".join("%02X" % v for v in vals))
        rvals = [0x00, 0x00, area_index & 0xFF, (area_index >> 8) & 0xFF]
        for mode, rtp, ref in groups:
            rvals += [mode, rtp, ref & 0xFF, (ref >> 8) & 0xFF]
        lines.append("R " + " ".join("%02X" % v for v in rvals))
    return "\n".join(lines) + "\n"


def write_json(path, obj):
    with open(path, "w") as handle:
        json.dump(obj, handle)


def simple_manifest(objects, init, image_areas=("_CODE",),
                    bss_areas=(), name="t"):
    return {
        "name": name,
        "type": "driver",
        "abi": 3,
        "flags": 0,
        "objects": list(objects),
        "image_areas": list(image_areas),
        "bss_areas": list(bss_areas),
        "init": init,
        "fini": None,
    }


def pack_in_dir(tmp, rel_files, manifest):
    """Write rel files + manifest into tmp and run the packer."""
    for name, text in rel_files.items():
        with open(os.path.join(tmp, name), "w") as handle:
            handle.write(text)
    manifest_path = os.path.join(tmp, "module.json")
    write_json(manifest_path, manifest)
    loaded = zmodpack.load_manifest(manifest_path, tmp)
    packed = zmodpack.pack_manifest(loaded)
    return zmodpack.build_container(packed), packed


def build_valid_container():
    rel = rel_text(
        "t",
        [("_CODE", 4), ("_DATA", 2)],
        [
            (".__.ABS.", "def", 0, None),
            ("_entry", "def", 0, 0),
            ("_state", "def", 0, 1),
        ],
        [(0, 0, bytes([0x21, 0x00, 0x00, 0xC9]), [(0x00, 0x05, 0x00)])],
    )
    with tempfile.TemporaryDirectory() as tmp:
        container, packed = pack_in_dir(
            tmp, {"t.rel": rel},
            simple_manifest(["t.rel"], "entry", bss_areas=("_DATA",)))
    return container, packed


def build_reloc_fixture():
    """Build a valid container with two nonzero base-relative relocations.

    The module mini-links at base zero: _CODE starts at offset 0 and _TEXT
    at offset 8, so the packed 16-bit words hold the module-relative
    targets 8 (the _TEXT area base) and 10 (_helper at _TEXT + 2).  Both
    targets are nonzero, so a load-base shift shows up in the patched
    bytes.
    """
    rel = rel_text(
        "t",
        [("_CODE", 8), ("_TEXT", 4)],
        [
            (".__.ABS.", "def", 0, None),
            ("_entry", "def", 0, 0),
            ("_helper", "def", 2, 1),
        ],
        [(0, 0, bytes([0x21, 0x00, 0x00, 0x11, 0x00, 0x00, 0xC9]),
          [(0x00, 0x05, 0x01), (0x02, 0x08, 0x02)])],
    )
    with tempfile.TemporaryDirectory() as tmp:
        container, packed = pack_in_dir(
            tmp, {"t.rel": rel},
            simple_manifest(["t.rel"], "entry",
                            image_areas=("_CODE", "_TEXT")))
    return container, packed


class TestModuleConstants(unittest.TestCase):
    def setUp(self):
        self.env = parse_c_header(os.path.join(ROOT, "abi", "zos",
                                               "errno.h"))
        self.env.update(parse_c_header(MODULE_H))

    def test_header_geometry(self):
        self.assertEqual(evaluate("ZMOD_HEADER_SIZE", self.env), 28)
        self.assertEqual(evaluate("ZMOD_RELOC_SIZE", self.env), 4)
        self.assertEqual(evaluate("ZMOD_FORMAT_VERSION", self.env), 1)
        self.assertEqual(evaluate("ZMOD_TYPE_DRIVER", self.env), 1)
        self.assertEqual(evaluate("ZREL_ABS16", self.env), 1)
        self.assertEqual(evaluate("ZMOD_NO_ENTRY", self.env), 0xFFFF)

    def test_granule_geometry_matches_platform(self):
        import mapcheck
        layout = mapcheck.parse_memory_inc(
            os.path.join(ROOT, "platform", "zephyr80", "memory.inc"))
        self.assertEqual(evaluate("ZMOD_GRANULE_SIZE", self.env),
                         layout["ZOS_DRIVER_GRANULE_SIZE"])
        # The loader/codec bound is the architectural maximum from
        # 2000h-DF00h (the bank-7 window below the ISR stack), not a
        # compiled arena size.
        self.assertEqual(evaluate("ZMOD_ARCH_BASE", self.env), 0x2000)
        self.assertEqual(evaluate("ZMOD_ARCH_END", self.env), 0xDF00)
        self.assertEqual(
            evaluate("ZMOD_MAX_GRANULES", self.env),
            (evaluate("ZMOD_ARCH_END", self.env) -
             evaluate("ZMOD_ARCH_BASE", self.env)) //
            evaluate("ZMOD_GRANULE_SIZE", self.env))

    def test_loader_error_codes_are_negative_and_distinct(self):
        values = [evaluate(name, self.env) for name in
                  ("ZMOD_EINVAL", "ZMOD_EFORMAT", "ZMOD_ENOTSUP",
                   "ZMOD_ENOSPC", "ZMOD_ENOENT", "ZMOD_EINIT")]
        self.assertEqual(evaluate("ZMOD_OK", self.env), 0)
        self.assertEqual(len(set(values)), len(values))
        for value in values:
            self.assertLess(value, 0, "error code must be negative")


class TestRelParsing(unittest.TestCase):
    def test_area_and_symbol_records(self):
        text = rel_text(
            "t",
            [("_CODE", 6), ("_DATA", 2)],
            [
                (".__.ABS.", "def", 0, None),
                ("_entry", "def", 0, 0),
                ("_state", "def", 0, 1),
            ],
            [(0, 0, bytes([0x21, 0x00, 0x00, 0xC9]), [(0x00, 0x04, 0x00)])],
        )
        obj = zmodpack.parse_rel(text, "t.rel")
        self.assertEqual([a.name for a in obj.areas], ["_CODE", "_DATA"])
        self.assertEqual(obj.areas[0].size, 6)
        self.assertEqual(len(obj.symbols), 3)
        self.assertEqual(obj.symbols[1].kind, "def")
        self.assertEqual(obj.symbols[1].area, 0)
        self.assertEqual(len(obj.chunks), 1)
        chunk = obj.chunks[0]
        self.assertEqual(chunk.area_index, 0)
        self.assertEqual(chunk.addr, 0)
        self.assertEqual(chunk.data, bytes([0x21, 0x00, 0x00, 0xC9]))
        self.assertEqual(chunk.relocs, [(0x00, 0x04, 0x00)])

    def test_bad_radix_rejected(self):
        with self.assertRaises(zmodpack.ZmodError):
            zmodpack.parse_rel("XH4\n", "t.rel")

    def test_t_without_r_rejected(self):
        with self.assertRaises(zmodpack.ZmodError):
            zmodpack.parse_rel("XL4\nT 00 00 00 00 11\n", "t.rel")


class TestPackRoundTrip(unittest.TestCase):
    def test_valid_container_fields(self):
        container, packed = build_valid_container()
        info = zmodpack.parse_container(container)
        self.assertEqual(info["image_size"], 4)
        self.assertEqual(info["bss_size"], 2)
        self.assertEqual(info["required_granules"], 1)
        self.assertEqual(info["init_offset"], 0)
        self.assertEqual(info["fini_offset"], 0xFFFF)
        self.assertEqual(info["payload"], bytes([0x21, 0x00, 0x00, 0xC9]))
        # One ZREL_ABS16 record for the relocated 16-bit immediate.
        self.assertEqual(info["relocs"], [(1, 1)])

    def test_deterministic_output(self):
        first, _ = build_valid_container()
        second, _ = build_valid_container()
        self.assertEqual(first, second)

    def test_granule_rounding_and_bss_layout(self):
        rel = rel_text(
            "t",
            [("_CODE", 4), ("_DATA", 0x205)],
            [
                (".__.ABS.", "def", 0, None),
                ("_entry", "def", 0, 0),
                ("_state", "def", 0, 1),
            ],
            [(0, 0, bytes([0xC9]), [])],
        )
        with tempfile.TemporaryDirectory() as tmp:
            _, packed = pack_in_dir(
                tmp, {"t.rel": rel},
                simple_manifest(["t.rel"], "entry",
                                bss_areas=("_DATA",)))
        # 4 + 517 = 521 -> 2 granules.
        self.assertEqual(packed.required_granules, 2)
        self.assertEqual(packed.bss_size, 0x205)

    def test_symbol_reference_across_objects(self):
        obj_a = rel_text(
            "a", [("_CODE", 4)],
            [("__.ABS.", "def", 0, None), ("_helper", "def", 0, 0)],
            [(0, 0, bytes([0xC3, 0x00, 0x00, 0xC9]), [(0x00, 0x05, 0x00)])],
        )
        obj_b = rel_text(
            "b", [("_CODE", 4)],
            [("__.ABS.", "def", 0, None), ("_helper", "ref", 0, None),
             ("_entry", "def", 0, 0)],
            [(0, 0, bytes([0x21, 0x00, 0x00, 0xC9]),
              [(0x02, 0x05, 0x01)])],
        )
        with tempfile.TemporaryDirectory() as tmp:
            container, packed = pack_in_dir(
                tmp, {"a.rel": obj_a, "b.rel": obj_b},
                simple_manifest(["a.rel", "b.rel"], "entry"))
        # Object b's instance of _CODE starts at 4; its `ld hl,#_helper`
        # resolves to offset 0 (the addend is 0), forming reloc site 4.
        info = zmodpack.parse_container(container)
        self.assertEqual(packed.init_offset, 4)
        # Both objects carry one relocated 16-bit immediate each: obj_a's
        # `jp _helper` at offset 1, obj_b's `ld hl,#_helper` at offset 5.
        self.assertEqual(info["relocs"], [(1, 1), (1, 5)])
        self.assertEqual(info["payload"][5:7], bytes([0x00, 0x00]))

    def test_unresolved_external_rejected(self):
        rel = rel_text(
            "t", [("_CODE", 4)],
            [("__.ABS.", "def", 0, None), ("_entry", "def", 0, 0),
             ("_kernel_thing", "ref", 0, None)],
            [(0, 0, bytes([0x21, 0x00, 0x00, 0xC9]),
              [(0x02, 0x04, 0x01)])],
        )
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(zmodpack.ZmodError,
                                        "unresolved external"):
                pack_in_dir(tmp, {"t.rel": rel},
                            simple_manifest(["t.rel"], "entry"))

    def test_byte_fragmented_mode_rejected(self):
        rel = rel_text(
            "t", [("_CODE", 4)],
            [("__.ABS.", "def", 0, None), ("_entry", "def", 0, 0)],
            [(0, 0, bytes([0x06, 0x00, 0x00, 0xC9]),
              [(0x09, 0x04, 0x00)])],
        )
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(zmodpack.ZmodError,
                                        "unsupported relocation mode 09"):
                pack_in_dir(tmp, {"t.rel": rel},
                            simple_manifest(["t.rel"], "entry"))

    def test_pc_relative_mode_rejected(self):
        rel = rel_text(
            "t", [("_CODE", 4)],
            [("__.ABS.", "def", 0, None), ("_entry", "def", 0, 0)],
            [(0, 0, bytes([0x18, 0x00, 0x00, 0xC9]),
              [(0x04, 0x04, 0x00)])],
        )
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(zmodpack.ZmodError,
                                        "unsupported relocation mode 04"):
                pack_in_dir(tmp, {"t.rel": rel},
                            simple_manifest(["t.rel"], "entry"))

    def test_unlisted_area_rejected(self):
        rel = rel_text(
            "t", [("_CODE", 4), ("_GSINIT", 2)],
            [("__.ABS.", "def", 0, None), ("_entry", "def", 0, 0)],
            [(0, 0, bytes([0xC9, 0x00, 0x00, 0x00]), [])],
        )
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(zmodpack.ZmodError,
                                        "not listed in the manifest"):
                pack_in_dir(tmp, {"t.rel": rel},
                            simple_manifest(["t.rel"], "entry"))

    def test_bss_with_initialized_data_rejected(self):
        rel = rel_text(
            "t", [("_CODE", 4), ("_DATA", 4)],
            [("__.ABS.", "def", 0, None), ("_entry", "def", 0, 0),
             ("_state", "def", 0, 1)],
            [(0, 0, bytes([0xC9, 0x00, 0x00, 0x00]), []),
             (1, 0, bytes([0x01, 0x02]), [])],
        )
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(zmodpack.ZmodError,
                                        "BSS area _DATA contains"):
                pack_in_dir(tmp, {"t.rel": rel},
                            simple_manifest(["t.rel"], "entry",
                                            bss_areas=("_DATA",)))

    def test_bad_init_entry_rejected(self):
        rel = rel_text(
            "t", [("_CODE", 4)],
            [("__.ABS.", "def", 0, None), ("_entry", "def", 4, 0)],
            [(0, 0, bytes([0xC9, 0x00, 0x00, 0x00]), [])],
        )
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(zmodpack.ZmodError,
                                        "not defined|outside"):
                pack_in_dir(tmp, {"t.rel": rel},
                            simple_manifest(["t.rel"], "entry"))

    def test_missing_manifest_area_rejected(self):
        rel = rel_text(
            "t", [("_CODE", 4)],
            [("__.ABS.", "def", 0, None), ("_entry", "def", 0, 0)],
            [(0, 0, bytes([0xC9, 0x00, 0x00, 0x00]), [])],
        )
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(zmodpack.ZmodError,
                                        "empty or missing"):
                pack_in_dir(tmp, {"t.rel": rel},
                            simple_manifest(["t.rel"], "entry",
                                            bss_areas=("_DATA",)))


class TestContainerValidation(unittest.TestCase):
    def setUp(self):
        self.container, _ = build_valid_container()

    def tamper(self, offset, *values):
        data = bytearray(self.container)
        for index, value in enumerate(values):
            data[offset + index] = value
        return bytes(data)

    def expect_error(self, data, pattern):
        with self.assertRaisesRegex(zmodpack.ZmodError, pattern):
            zmodpack.parse_container(data, "test.zmod")

    def test_bad_magic(self):
        self.expect_error(self.tamper(0, ord("X")), "bad magic")

    def test_bad_version(self):
        self.expect_error(self.tamper(4, 2), "unsupported format version")

    def test_bad_type(self):
        # Types 1 (driver) and 2 (service) are the format's module types;
        # anything else is refused.
        self.expect_error(self.tamper(5, 3), "unsupported module type")

    def test_service_type_dispatches_to_the_service_parser(self):
        # A driver container whose type byte says "service" is parsed as a
        # service image and refused by the service rules, never accepted
        # as a driver.
        self.expect_error(self.tamper(5, 2), "service")

    def test_bad_abi(self):
        self.expect_error(self.tamper(6, 9), "unsupported driver ABI")

    def test_bad_flags_and_reserved(self):
        self.expect_error(self.tamper(7, 1), "flags/reserved")
        self.expect_error(self.tamper(0x1A, 1), "flags/reserved")

    def test_truncated_container(self):
        self.expect_error(self.container[:-1], "total_size")

    def test_zero_image_size(self):
        self.expect_error(self.tamper(0x08, 0, 0), "image_size is zero")

    def test_required_granules_mismatch(self):
        self.expect_error(self.tamper(0x0C, 2, 0), "required_granules")

    def test_init_outside_image(self):
        self.expect_error(self.tamper(0x0E, 4, 0), "init_offset")

    def test_payload_inside_header(self):
        self.expect_error(self.tamper(0x16, 4, 0), "payload_offset")

    def test_relocation_table_overlap(self):
        # Move the relocation table to overlap the payload's first bytes.
        payload_offset = self.container[0x16] | (self.container[0x17] << 8)
        data = self.tamper(0x12, payload_offset & 0xFF,
                           (payload_offset >> 8) & 0xFF)
        self.expect_error(data, "overlaps the payload")

    def test_relocation_type_unsupported(self):
        reloc_offset = self.container[0x12] | (self.container[0x13] << 8)
        data = self.tamper(reloc_offset, 2)
        self.expect_error(data, "unsupported relocation type")

    def test_relocation_reserved_byte(self):
        reloc_offset = self.container[0x12] | (self.container[0x13] << 8)
        data = self.tamper(reloc_offset + 1, 1)
        self.expect_error(data, "reserved byte")

    def test_relocation_patch_crosses_image(self):
        reloc_offset = self.container[0x12] | (self.container[0x13] << 8)
        data = self.tamper(reloc_offset + 2, 3, 0)  # image is 4 bytes
        self.expect_error(data, "crosses the image")

    def test_dump_report_lists_relocations(self):
        info = zmodpack.parse_container(self.container)
        text = zmodpack.format_report(info, "test.zmod")
        self.assertIn("ZMOD v1 driver", text)
        self.assertIn("relocations:   1", text)


class TestTwoBaseRelocation(unittest.TestCase):
    """The same container relocates correctly at two distinct load bases.

    The host packer mini-links at base zero, so every packed ZREL_ABS16
    patch site holds a base-relative (module-relative) address; the target
    loader copies the image to an arena granule and applies
    ``word += load_base`` (kernel/module/relocate.c).  This test decodes
    the container's relocation records, proves each recorded target is a
    base-relative address inside the image, and applies the loader's
    arithmetic at two granule-aligned load bases in
    ZMOD_ARCH_BASE..ZMOD_ARCH_END.  It also checks that a misaligned or
    out-of-arena base is rejected by the loader policy mirrored here.
    """

    def setUp(self):
        self.container, _ = build_reloc_fixture()
        self.info = zmodpack.parse_container(self.container)
        self.env = parse_c_header(MODULE_H)
        self.granule = evaluate("ZMOD_GRANULE_SIZE", self.env)
        self.arch_base = evaluate("ZMOD_ARCH_BASE", self.env)
        self.arch_end = evaluate("ZMOD_ARCH_END", self.env)

    @staticmethod
    def _read_u16(data, offset):
        return data[offset] | (data[offset + 1] << 8)

    def relocation_targets(self):
        """Decode each ZREL_ABS16 record's base-relative target word."""
        payload = self.info["payload"]
        return [self._read_u16(payload, patch)
                for _rtype, patch in self.info["relocs"]]

    def apply_relocations(self, image, base):
        """Apply the loader's ``word += load_base`` to a payload copy."""
        patched = bytearray(image)
        for _rtype, patch in self.info["relocs"]:
            value = self._read_u16(patched, patch) + base
            if value > 0xFFFF:
                raise ValueError("relocation overflow at %04Xh" % patch)
            patched[patch] = value & 0xFF
            patched[patch + 1] = (value >> 8) & 0xFF
        return bytes(patched)

    def check_load_base(self, base, allocation=None):
        """Mirror the loader's load-base acceptance rules.

        A base must be granule-aligned and keep the whole allocation inside
        the architectural window (kernel/module/loader.c init check plus the
        module.h bounds).
        """
        if allocation is None:
            allocation = self.info["required_granules"] * self.granule
        if base % self.granule != 0:
            raise ValueError("load base %04Xh is not granule-aligned" % base)
        if base < self.arch_base or base >= self.arch_end:
            raise ValueError("load base %04Xh is outside the arena" % base)
        if base + allocation > self.arch_end:
            raise ValueError("load base %04Xh overruns the arena" % base)
        return base

    def test_relocations_are_base_relative_inside_the_image(self):
        self.assertEqual(self.info["reloc_count"], 2)
        self.assertEqual(self.relocation_targets(), [8, 10])
        for _rtype, patch in self.info["relocs"]:
            self.assertGreaterEqual(patch, 0)
            self.assertLessEqual(patch + 2, self.info["image_size"])
        for target in self.relocation_targets():
            self.assertGreaterEqual(target, 0)
            self.assertLess(target, self.info["image_size"])

    def test_two_bases_shift_patched_words_by_the_delta(self):
        base_a = self.arch_base                  # 2000h, even granule
        base_b = self.arch_base + self.granule   # 2200h, 512 bytes higher
        self.assertEqual(self.check_load_base(base_a), base_a)
        self.assertEqual(self.check_load_base(base_b), base_b)
        delta = base_b - base_a
        self.assertEqual(delta, self.granule)

        image = self.info["payload"]
        patched_a = self.apply_relocations(image, base_a)
        patched_b = self.apply_relocations(image, base_b)
        for _rtype, patch in self.info["relocs"]:
            relative = self._read_u16(image, patch)
            word_a = self._read_u16(patched_a, patch)
            word_b = self._read_u16(patched_b, patch)
            # The patched words are the relative target plus each base, and
            # the relative target is identical at both bases.
            self.assertEqual(word_a, base_a + relative)
            self.assertEqual(word_b, base_b + relative)
            self.assertEqual(word_b - word_a, delta)
            self.assertEqual(word_a - base_a, word_b - base_b)
            self.assertLessEqual(word_b, 0xFFFF)

    def test_invalid_load_bases_rejected(self):
        # Not granule-aligned.
        with self.assertRaisesRegex(ValueError, "granule-aligned"):
            self.check_load_base(self.arch_base + 1)
        # Aligned but below the architectural arena.
        with self.assertRaisesRegex(ValueError, "outside the arena"):
            self.check_load_base(self.arch_base - self.granule)
        # Aligned but at/above the arena end (the architectural window end
        # need not itself be granule-aligned).
        aligned_end = ((self.arch_end + self.granule - 1)
                       // self.granule * self.granule)
        with self.assertRaisesRegex(ValueError, "outside the arena"):
            self.check_load_base(aligned_end)
        # Aligned and inside, but the allocation overruns the arena end.
        with self.assertRaisesRegex(ValueError, "overruns the arena"):
            self.check_load_base(aligned_end - self.granule,
                                 allocation=2 * self.granule)


class TestFixtureArtifacts(unittest.TestCase):
    """Require `make zmod-test`; skipped otherwise."""

    def setUp(self):
        if not os.path.isdir(FIXTURE_MODULES):
            self.skipTest("build/modules/ missing; run `make zmod-test`")
        self.containers = {}
        for name in ("m4_probe", "m4_fail", "m4_filler", "abi_probe"):
            path = os.path.join(FIXTURE_MODULES, name + ".zmod")
            if not os.path.exists(path):
                self.skipTest("%s missing; run `make zmod-test`" % path)
            self.containers[name] = zmodpack.parse_container(
                open(path, "rb").read(), path)

    def test_expected_sizes(self):
        probe = self.containers["m4_probe"]
        filler = self.containers["m4_filler"]
        api = self.containers["abi_probe"]
        self.assertEqual(probe["bss_size"], 2)
        self.assertEqual(probe["required_granules"], 1)
        self.assertGreater(probe["reloc_count"], 0)
        # 2 BSS bytes of magic + 600-byte pad + image -> 2 granules.
        self.assertEqual(filler["required_granules"], 2)
        self.assertGreater(filler["bss_size"], 600)
        # The ABI v2 discovery fixture: three marker words + image.
        self.assertEqual(api["bss_size"], 6)
        self.assertGreater(api["required_granules"], 0)
        self.assertGreater(api["reloc_count"], 0)

    def test_sdldz80_agrees_with_the_mini_linker(self):
        """Link the same objects with sdldz80 at the packer's bases and
        compare every image byte (the strongest correctness check for the
        relocation/emission model)."""
        try:
            subprocess.run(["sdldz80"], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
        except OSError:
            self.skipTest("sdldz80 not available")
        for name in ("m4_probe", "m4_fail", "m4_filler", "abi_probe"):
            manifest_path = os.path.join(FIXTURE_MANIFESTS, name + ".json")
            manifest = zmodpack.load_manifest(manifest_path, FIXTURE_MODULES)
            layout = zmodpack.build_layout(manifest)
            info = self.containers[name]
            with tempfile.TemporaryDirectory() as tmp:
                ihx = os.path.join(tmp, "linked.ihx")
                lk = os.path.join(tmp, "link.lk")
                with open(lk, "w") as handle:
                    handle.write("-n\n-m\n-w\n-i %s\n" % ihx)
                    for area in manifest["image_areas"] + \
                            manifest["bss_areas"]:
                        handle.write("-b %s = 0x%04X\n"
                                     % (area, layout["area_base"][area]))
                    for obj in manifest["object_names"]:
                        handle.write("%s\n"
                                     % os.path.join(FIXTURE_MODULES, obj))
                    handle.write("-e\n")
                result = subprocess.run(["sdldz80", "-f", lk],
                                        capture_output=True, text=True)
                self.assertTrue(os.path.exists(ihx),
                                "sdldz80 failed for %s: %s"
                                % (name, result.stdout + result.stderr))
                memory = {}
                for line in open(ihx):
                    line = line.strip()
                    if not line.startswith(":"):
                        continue
                    raw = bytes.fromhex(line[1:])
                    if raw[3] == 0:
                        addr = (raw[1] << 8) | raw[2]
                        for i, byte in enumerate(raw[4:4 + raw[0]]):
                            memory[addr + i] = byte
                    elif raw[3] == 1:
                        break
                for offset, byte in enumerate(info["payload"]):
                    self.assertEqual(
                        memory.get(offset, 0xFF), byte,
                        "%s image byte %04Xh differs from sdldz80"
                        % (name, offset))

    def test_emit_asm_round_trip(self):
        out_asm = os.path.join(FIXTURE_MODULES, "roundtrip.asm")
        out_inc = os.path.join(FIXTURE_MODULES, "roundtrip.inc")
        argv = [sys.executable, os.path.join(ROOT, "tools", "zmodpack.py"),
                "emit-asm", out_asm,
                "z4_fixture_probe=%s"
                % os.path.join(FIXTURE_MODULES, "m4_probe.zmod"),
                "--header", out_inc]
        result = subprocess.run(argv, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = []
        in_data = False
        for line in open(out_asm):
            line = line.strip()
            if line.startswith("_z4_fixture_probe::"):
                in_data = True
                continue
            if in_data:
                if line.startswith("_z4_fixture_probe_size::"):
                    break
                if line.startswith(".db "):
                    rows += [int(value, 16)
                             for value in line[4:].split(",")]
        expected = open(os.path.join(FIXTURE_MODULES,
                                     "m4_probe.zmod"), "rb").read()
        self.assertEqual(bytes(rows), expected)
        inc = open(out_inc).read()
        self.assertIn("#define Z4_FIXTURE_PROBE_SIZE %d" % len(expected), inc)
        os.unlink(out_asm)
        os.unlink(out_inc)


class TestSioModuleArtifacts(unittest.TestCase):
    """The production SIO module variants, packed by `make zmod-test`.

    The kernel never links these bytes; every image embeds them through
    the generated catalog, so their geometry and reports are part of the
    M4 acceptance surface."""

    EXPECTED = {
        # name: (required_granules, bss_size)
        "sio": (1, 4),
        "sio-irq": (2, 8),
        "sio-fail": (1, 4),
    }

    # Each variant's manifest names its own module entry: production
    # sio_module.c, and the unmistakable test entries under tests/modules/.
    INIT_SYMBOL = {
        "sio": "sio_module_init",
        "sio-irq": "sio_irq_module_init",
        "sio-fail": "sio_fail_module_init",
    }

    def setUp(self):
        if not os.path.isdir(FIXTURE_MODULES):
            self.skipTest("build/modules/ missing; run `make zmod-test`")
        self.containers = {}
        self.reports = {}
        for name in self.EXPECTED:
            path = os.path.join(FIXTURE_MODULES, name + ".zmod")
            if not os.path.exists(path):
                self.skipTest("%s missing; run `make zmod-test`" % path)
            self.containers[name] = zmodpack.parse_container(
                open(path, "rb").read(), path)
            report_path = os.path.join(FIXTURE_MODULES, name + ".report.json")
            if not os.path.exists(report_path):
                self.skipTest("%s missing; run `make zmod-test`" % report_path)
            self.reports[name] = json.load(open(report_path))

    def test_container_geometry(self):
        for name, (granules, bss) in self.EXPECTED.items():
            info = self.containers[name]
            self.assertEqual(info["required_granules"], granules, name)
            self.assertEqual(info["bss_size"], bss, name)
            self.assertGreater(info["image_size"], 0, name)

    def test_init_entry_present(self):
        # The loader must be able to call the module init directly; the
        # packer resolved it to an image offset for every variant.
        for name in self.EXPECTED:
            self.assertGreater(self.containers[name]["init_offset"], 0,
                               name)

    def test_reports_carry_symbol_offsets(self):
        for name in self.EXPECTED:
            report = self.reports[name]
            self.assertEqual(report["name"], "sio")
            self.assertEqual(report["required_granules"],
                             self.EXPECTED[name][0])
            symbols = report["symbols"]
            self.assertIn(self.INIT_SYMBOL[name],
                          {key.lstrip("_") for key in symbols})
            total = report["total_size"]
            for symbol, offset in symbols.items():
                self.assertGreaterEqual(offset, 0)
                self.assertLess(offset, total,
                                "%s symbol %s outside allocation"
                                % (name, symbol))

    def test_irq_variant_exports_selftest_symbols(self):
        symbols = self.reports["sio-irq"]["symbols"]
        for expected in ("_sio_module_irq_hits",
                         "_sio_module_irq_state_addr",
                         "_sio_module_irq_handler"):
            self.assertIn(expected, symbols)
        # The failure variant uses its own clearly named entry; it claims
        # CTC1 then fails so the rollback evidence can be verified.
        fail_symbols = self.reports["sio-fail"]["symbols"]
        self.assertIn("sio_fail_module_init",
                      {key.lstrip("_") for key in fail_symbols})

    def test_sio_image_contains_registered_names(self):
        self.assertIn(b"sio0.b", self.containers["sio"]["payload"])


if __name__ == "__main__":
    unittest.main()
