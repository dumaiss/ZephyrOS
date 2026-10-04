#!/usr/bin/env python3
"""Host-side ROMFS tests: constant contract, tools/romfspack.py
behavior, map/image placement rules, and the built recovery artifacts.

The executable kernel behavior is replayed from the real linked images by
tests/test_romfs_sim.py; this file covers the host toolchain and the
artifact-level contracts.  Run with: make test / make romfs-test
"""

import json
import os
import re
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)

import mapcheck  # noqa: E402
import romfspack  # noqa: E402
from z80sim import read_symbols  # noqa: E402
from maptools import evaluate, make_map, parse_c_header, parse_asm_inc  # noqa: E402

ROMFS_H = os.path.join(ROOT, "include", "zephyros", "romfs.h")
ERRNO_H = os.path.join(ROOT, "include", "zephyros", "errno.h")
MEMORY_H = os.path.join(ROOT, "platform", "zephyr80", "memory.h")
MEMORY_INC = os.path.join(ROOT, "platform", "zephyr80", "memory.inc")
ROM_ACCESS_ASM = os.path.join(ROOT, "kernel", "romfs", "rom_access.asm")
ROMFS_SELFTEST_H = os.path.join(ROOT, "tests", "target", "include",
                                "romfs_selftest.h")

ROMFS_STAGING = 128
ROM_ACCESS_BASE = 0xEF00
ROM_ACCESS_SIZE = 8


def make_manifest(tmp, files, objdir=None):
    """files: list of (name, type, filename, bytes).  Returns path."""
    base = objdir or os.path.join(tmp, "inputs")
    os.makedirs(base, exist_ok=True)
    entries = []
    for name, media, filename, data in files:
        with open(os.path.join(base, filename), "wb") as fh:
            fh.write(data)
        entries.append({"name": name, "type": media, "path": filename})
    manifest = os.path.join(tmp, "manifest.json")
    with open(manifest, "w") as fh:
        json.dump({"format": "romfs-v1", "files": entries}, fh)
    return manifest, base


class TestRomfsConstants(unittest.TestCase):
    def setUp(self):
        self.romfs = parse_c_header(ROMFS_H)
        self.memory_c = parse_c_header(MEMORY_H)
        self.memory_asm = parse_asm_inc(MEMORY_INC)
        self.errno = parse_c_header(ERRNO_H)

    def test_format_constants(self):
        self.assertEqual(evaluate("ROMFS_VERSION", self.romfs), 1)
        self.assertEqual(evaluate("ROMFS_HEADER_SIZE", self.romfs), 16)
        self.assertEqual(evaluate("ROMFS_ENTRY_SIZE", self.romfs), 36)
        self.assertEqual(evaluate("ROMFS_NAME_SIZE", self.romfs), 24)
        self.assertEqual(evaluate("ROMFS_FLAG_ZMOD", self.romfs), 1)
        self.assertEqual(evaluate("ROMFS_FLAG_ZEX", self.romfs), 2)
        self.assertEqual(evaluate("ROMFS_FLAG_DATA", self.romfs), 3)

    def test_platform_geometry(self):
        layout = mapcheck.parse_memory_inc(MEMORY_INC)
        self.assertEqual(layout["ZOS_ROM_PAGE_SIZE"], 0x10000)
        self.assertEqual(layout["ZOS_ROM_PAGE_COUNT"], 8)
        self.assertEqual(layout["ZOS_ROM_IMAGE_SIZE"], 0x80000)
        self.assertEqual(layout["ZOS_ROMFS_FIRST_PAGE"], 2)
        self.assertEqual(layout["ZOS_ROMFS_PAGE_COUNT"], 6)
        self.assertEqual(layout["ZOS_ROMFS_WINDOW_SIZE"], 0xC000)
        self.assertEqual(layout["ZOS_ROMFS_CAPACITY"], 0x48000)
        self.assertEqual(layout["ZOS_ROMFS_STAGING_SIZE"], ROMFS_STAGING)
        self.assertEqual(layout["ZOS_ROM_ACCESS_BASE"], ROM_ACCESS_BASE)
        self.assertEqual(layout["ZOS_ROM_ACCESS_SIZE"], ROM_ACCESS_SIZE)

    def test_staging_size_is_single_sourced(self):
        self.assertEqual(evaluate("ZOS_ROMFS_STAGING_SIZE", self.memory_c),
                         ROMFS_STAGING)
        self.assertEqual(evaluate("ZOS_ROMFS_STAGING_SIZE",
                                  self.memory_asm), ROMFS_STAGING)

    def test_rom_access_error_codes_match_errno(self):
        """The assembly primitive returns positive ZOS_* codes; the local
        equates must match include/zephyros/errno.h."""
        text = open(ROM_ACCESS_ASM).read()
        for name, errno_name in (("RAX_EINVAL", "ZOS_EINVAL"),
                                 ("RAX_EBUSY", "ZOS_EBUSY")):
            match = re.search(r"^%s\s*=\s*(0[xX][0-9A-Fa-f]+)" % name,
                              text, re.M)
            self.assertIsNotNone(match, "rom_access.asm is missing %s" % name)
            self.assertEqual(int(match.group(1), 16),
                             evaluate(errno_name, self.errno))


class TestRomfspack(unittest.TestCase):
    def test_round_trip_and_layout(self):
        with tempfile.TemporaryDirectory(prefix="romfs-") as tmp:
            manifest, objdir = make_manifest(tmp, [
                ("a.bin", "DATA", "a.bin", bytes(range(256)) * 3),
                ("b.zex", "ZEX", "b.bin", b"hello romfs"),
            ])
            stream, report = romfspack.pack(
                manifest, os.path.join(tmp, "out.romfs"), objdir=objdir)
            header, entries = romfspack.parse_image(stream)
            self.assertEqual(header["version"], 1)
            self.assertEqual(header["entry_size"], 36)
            self.assertEqual(header["file_count"], 2)
            self.assertEqual(header["directory_size"], 72)
            self.assertEqual(header["image_size"], len(stream))
            self.assertEqual(header["payload_start"], 16 + 72)
            self.assertEqual(stream[0:4], b"ZRFS")
            self.assertEqual(entries[0]["name"], "a.bin")
            self.assertEqual(entries[0]["offset"], 16 + 72)
            self.assertEqual(entries[1]["offset"], 16 + 72 + 768)
            self.assertEqual(entries[0]["payload"], bytes(range(256)) * 3)
            self.assertEqual(entries[1]["payload"], b"hello romfs")
            # Entry bytes: name NUL-terminated and zero padded.
            entry_raw = stream[16:16 + 24]
            self.assertEqual(entry_raw[:5], b"a.bin")
            self.assertEqual(entry_raw[5:], b"\x00" * 19)
            # Map text carries the required columns.
            self.assertIn("logical range", report)
            self.assertIn("physical ROM range", report)
            self.assertIn("DATA", report)
            self.assertIn("sha256", report)

    def test_output_is_reproducible(self):
        with tempfile.TemporaryDirectory(prefix="romfs-") as tmp:
            manifest, objdir = make_manifest(tmp, [
                ("a.bin", "DATA", "a.bin", b"payload" * 100),
            ])
            one, _ = romfspack.pack(manifest, os.path.join(tmp, "1.romfs"),
                                    objdir=objdir)
            two, _ = romfspack.pack(manifest, os.path.join(tmp, "2.romfs"),
                                    objdir=objdir)
            self.assertEqual(one, two)

    def test_cross_page_placement_map(self):
        geom = romfspack.load_geometry()
        size = geom["window_size"] + 0x40
        with tempfile.TemporaryDirectory(prefix="romfs-") as tmp:
            manifest, objdir = make_manifest(tmp, [
                ("big.bin", "DATA", "big.bin", b"\xA5" * size),
            ])
            stream, report = romfspack.pack(
                manifest, os.path.join(tmp, "out.romfs"), objdir=objdir)
            _header, entries = romfspack.parse_image(stream)
            self.assertEqual(
                romfspack.format_physical(entries[0]["offset"],
                                          entries[0]["size"], geom),
                "page 2 0034h-BFFFh, page 3 0000h-0073h")
            self.assertIn("page 2", report)
            self.assertIn("page 3", report)

    def test_logical_to_physical_mapping(self):
        geom = romfspack.load_geometry()
        self.assertEqual(romfspack.logical_to_physical(0, geom), (2, 0))
        self.assertEqual(romfspack.logical_to_physical(0xBFFF, geom),
                         (2, 0xBFFF))
        self.assertEqual(romfspack.logical_to_physical(0xC000, geom), (3, 0))
        self.assertEqual(romfspack.logical_to_physical(0x48000 - 1, geom),
                         (7, 0xBFFF))
        with self.assertRaises(romfspack.RomfsError):
            romfspack.logical_to_physical(0x48000, geom)

    def _expect_pack_error(self, files, pattern, manifest_mutate=None):
        with tempfile.TemporaryDirectory(prefix="romfs-") as tmp:
            manifest, objdir = make_manifest(tmp, files)
            if manifest_mutate:
                manifest_mutate(tmp, manifest)
            with self.assertRaisesRegex(romfspack.RomfsError, pattern):
                romfspack.pack(manifest, os.path.join(tmp, "out.romfs"),
                               objdir=objdir)

    def test_duplicate_names_rejected(self):
        self._expect_pack_error(
            [("dup.bin", "DATA", "a.bin", b"x"),
             ("dup.bin", "DATA", "b.bin", b"y")], r"duplicate file name")

    def test_case_insensitive_collision_rejected(self):
        # Native VFS path matching is case-insensitive, so two names that
        # differ only in case would be unreachable.
        self._expect_pack_error(
            [("Readme.txt", "DATA", "a.bin", b"x"),
             ("README.TXT", "DATA", "b.bin", b"y")],
            r"case-insensitive collision")

    def test_slash_in_name_rejected(self):
        self._expect_pack_error(
            [("dir/file.bin", "DATA", "a.bin", b"x")],
            r"must not contain '/'")

    def test_overlong_name_rejected(self):
        self._expect_pack_error(
            [("x" * 24, "DATA", "a.bin", b"x")], r"longer than 23")

    def test_empty_and_nonprintable_names_rejected(self):
        self._expect_pack_error([("", "DATA", "a.bin", b"x")],
                                r"non-empty string")
        self._expect_pack_error([("a\x01b", "DATA", "a.bin", b"x")],
                                r"non-printable")

    def test_unsupported_type_rejected(self):
        self._expect_pack_error([("a.bin", "CPM", "a.bin", b"x")],
                                r"unsupported type")

    def test_missing_input_rejected(self):
        with tempfile.TemporaryDirectory(prefix="romfs-") as tmp:
            manifest = os.path.join(tmp, "manifest.json")
            with open(manifest, "w") as fh:
                json.dump({"files": [{"name": "a.bin", "type": "DATA",
                                      "path": "absent.bin"}]}, fh)
            with self.assertRaisesRegex(romfspack.RomfsError,
                                        r"input not found"):
                romfspack.pack(manifest, os.path.join(tmp, "out.romfs"))

    def test_capacity_excess_rejected(self):
        geom = romfspack.load_geometry()
        self._expect_pack_error(
            [("big.bin", "DATA", "big.bin", b"\x00" * (geom["capacity"] + 1))],
            r"capacity")

    def test_manifest_without_files_rejected(self):
        with tempfile.TemporaryDirectory(prefix="romfs-") as tmp:
            manifest = os.path.join(tmp, "manifest.json")
            with open(manifest, "w") as fh:
                json.dump({"files": []}, fh)
            with self.assertRaisesRegex(romfspack.RomfsError,
                                        r"non-empty list"):
                romfspack.pack(manifest, os.path.join(tmp, "out.romfs"))

    # --- validate-mode rejections on crafted streams --------------------

    def _valid_stream(self):
        files = [(b"a.bin", romfspack.FLAG_DATA, b"abcdef")]
        return romfspack.build_image(files, romfspack.load_geometry())

    def _expect_validate_error(self, mutate, pattern):
        data = bytearray(self._valid_stream())
        mutate(data)
        with self.assertRaisesRegex(romfspack.RomfsError, pattern):
            romfspack.parse_image(bytes(data))

    def test_validate_rejects_bad_magic(self):
        self._expect_validate_error(lambda d: d.__setitem__(0, ord("X")),
                                    r"bad magic")

    def test_validate_rejects_bad_version(self):
        self._expect_validate_error(lambda d: d.__setitem__(4, 2),
                                    r"unsupported version")

    def test_validate_rejects_bad_entry_size(self):
        self._expect_validate_error(lambda d: d.__setitem__(5, 40),
                                    r"entry_size")

    def test_validate_rejects_header_reserved(self):
        self._expect_validate_error(lambda d: d.__setitem__(10, 1),
                                    r"reserved")

    def test_validate_rejects_truncation_and_padding(self):
        self._expect_validate_error(lambda d: d.pop(), r"truncated")
        self._expect_validate_error(lambda d: d.extend(b"\xFF"),
                                    r"image_size")

    def test_validate_rejects_directory_mismatch(self):
        self._expect_validate_error(
            lambda d: d.__setitem__(8, 40), r"directory_size")

    def test_validate_rejects_table_payload_overlap(self):
        def mutate(d):
            d[40:44] = (16).to_bytes(4, "little")
        self._expect_validate_error(mutate, r"overlaps the table")

    def test_validate_rejects_unsupported_flags(self):
        def mutate(d):
            flags_at = 16 + 32
            d[flags_at:flags_at + 2] = (7).to_bytes(2, "little")
        self._expect_validate_error(mutate, r"unsupported flags")

    def test_validate_rejects_entry_reserved(self):
        def mutate(d):
            d[16 + 34:16 + 36] = (1).to_bytes(2, "little")
        self._expect_validate_error(mutate, r"reserved")

    def test_validate_rejects_unterminated_name(self):
        def mutate(d):
            d[16:16 + 24] = b"X" * 24
        self._expect_validate_error(mutate, r"not NUL-terminated")

    def test_validate_rejects_nonzero_padding(self):
        def mutate(d):
            d[16 + 6] = ord("X")
        self._expect_validate_error(mutate, r"padding is not zero")

    def test_validate_rejects_duplicate_names(self):
        with tempfile.TemporaryDirectory(prefix="romfs-") as tmp:
            manifest, objdir = make_manifest(tmp, [
                ("a.bin", "DATA", "a.bin", b"x"),
                ("a.bin2", "DATA", "b.bin", b"y"),
            ])
            stream = bytearray(romfspack.pack(
                manifest, os.path.join(tmp, "out.romfs"), objdir=objdir)[0])
        # Copy entry 1's name over entry 2's name -> duplicate.
        stream[16 + 36:16 + 36 + 24] = stream[16:16 + 24]
        with self.assertRaisesRegex(romfspack.RomfsError, r"duplicate"):
            romfspack.parse_image(bytes(stream))

    def test_validate_rejects_32bit_wrap(self):
        def mutate(d):
            # entry 0: offset = FFFFFFF0h, size = 100h -> sum wraps u32.
            d[40:44] = (0xFFFFFFF0).to_bytes(4, "little")
            d[44:48] = (0x100).to_bytes(4, "little")
        self._expect_validate_error(mutate, r"leaves the image")


class TestRomfsMapcheck(unittest.TestCase):
    def test_romfs_reader_in_common_rejected(self):
        text = make_map(symbols={"_romfs_read": 0xE200})
        with self.assertRaisesRegex(mapcheck.MapError, "_romfs_read"):
            mapcheck.check_map(text)

    def test_rom_access_primitive_in_bank7_rejected(self):
        text = make_map(symbols={"_rom_access_copy": 0x3000})
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "_rom_access_copy"):
            mapcheck.check_map(text)

    def test_rom_access_staging_in_bank7_rejected(self):
        text = make_map(symbols={"_rom_access_staging": 0xC400,
                                 "rom_access_saved_latch": 0xC480})
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "_rom_access_staging"):
            mapcheck.check_map(text)

    def test_rom_access_staging_size_mismatch_rejected(self):
        text = make_map(symbols={"rom_access_saved_latch": 0xE241})
        with self.assertRaisesRegex(mapcheck.MapError, "staging buffer"):
            mapcheck.check_map(text)

    def test_rom_access_sync_extent_mismatch_rejected(self):
        text = make_map(symbols={"rom_access_sync_end": 0xEF09})
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "rom_access_sync_end"):
            mapcheck.check_map(text)

    def test_rom_access_trampoline_placement_mismatch_rejected(self):
        text = make_map(symbols={"rom_access_trampoline": 0xEF00})
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "rom_access_trampoline"):
            mapcheck.check_map(text)

    def test_rom_access_stub_area_size_mismatch_rejected(self):
        text = make_map(areas={"_ROM_ACCESS_STUB0": (0x0000, 0x0B)})
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "_ROM_ACCESS_STUB"):
            mapcheck.check_map(text)

    def test_production_rejects_embedded_catalog(self):
        for symbol in ("_z4_module_boot", "_zmod_catalog",
                       "_module_catalog_load_all"):
            text = make_map(symbols={symbol: 0x2600})
            with self.assertRaisesRegex(mapcheck.MapError,
                                        "production image"):
                mapcheck.check_map(text, production=True)


class TestBuiltArtifacts(unittest.TestCase):
    def _read(self, rel):
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            self.skipTest("%s missing; build it first" % rel)
        return path, open(path, "rb").read()

    def test_production_recovery_matches_packed_sio(self):
        _p, stream = self._read("build/recovery.romfs")
        _p, zmod = self._read("build/modules/sio.zmod")
        header, entries = romfspack.parse_image(stream)
        names = {entry["name"]: entry for entry in entries}
        self.assertIn("sio.zmod", names)
        self.assertEqual(names["sio.zmod"]["flags"], romfspack.FLAG_ZMOD)
        self.assertEqual(names["sio.zmod"]["payload"], zmod,
                         "ROMFS sio.zmod payload differs from the packed "
                         "container (byte identity violated)")

    def test_production_image_places_recovery_stream_only_in_pages_2_7(self):
        _p, image = self._read("build/zephyros.bin")
        _p, stream = self._read("build/recovery.romfs")
        self.assertEqual(len(image), 8 * 0x10000)
        geom = romfspack.load_geometry()
        for i in range(len(stream)):
            page, offset = romfspack.logical_to_physical(i, geom)
            self.assertEqual(image[page * 0x10000 + offset], stream[i],
                             "logical byte %d not placed at page %d %04X"
                             % (i, page, offset))
        # Unused stream capacity stays FF, and the C000h-FFFFh half of
        # every recovery page stays FF except the fixed ROM-access stub
        # mirror, which must equal the copy linked in common (page 0).
        layout = mapcheck.parse_memory_inc(MEMORY_INC)
        base = layout["ZOS_ROM_ACCESS_BASE"]
        size = layout["ZOS_ROM_ACCESS_SIZE"]
        common_stub = image[base:base + size]
        self.assertNotEqual(common_stub, b"\xFF" * size,
                            "ROM-access stub is unlinked in page 0")
        self.assertEqual(image[base - 2:base], b"\xD3\x00",
                         "trampoline OUT (00h),A missing before the stub")
        for page in range(2, 8):
            page_base = page * 0x10000
            tail = image[page_base + 0xC000:page_base + 0x10000]
            expected = bytearray(b"\xFF" * 0x4000)
            expected[base - 0xC000:base - 0xC000 + size] = common_stub
            self.assertEqual(bytes(tail), bytes(expected),
                             "page %d C000h-FFFFh is not FF plus the "
                             "ROM-access stub mirror" % page)
            self.assertEqual(image[page_base + base:page_base + base + size],
                             common_stub,
                             "page %d ROM-access stub mirror differs "
                             "from the common copy" % page)
        first_free = len(stream)
        for i in range(first_free, min(geom["capacity"], first_free + 64)):
            page, offset = romfspack.logical_to_physical(i, geom)
            self.assertEqual(image[page * 0x10000 + offset], 0xFF)
        # Boot pages keep the reset vector.
        self.assertNotEqual(image[0], 0xFF)

    def test_manifest_records_pages_and_romfs_placement(self):
        _p, text = self._read("build/zephyros.manifest.json")
        manifest = json.loads(text)
        self.assertEqual(len(manifest["pages"]), 8)
        self.assertTrue(manifest["romfs"]["present"])
        placement = manifest["romfs"]["placement"]
        self.assertGreater(len(placement), 0)
        self.assertEqual(placement[0]["page"], 2)
        self.assertEqual(placement[0]["logical_offset"], 0)
        _p, image = self._read("build/zephyros.bin")
        for page in manifest["pages"]:
            data = image[page["index"] * 0x10000:(page["index"] + 1) * 0x10000]
            import hashlib
            self.assertEqual(page["sha256"],
                             hashlib.sha256(data).hexdigest(),
                             "manifest page %d hash mismatch" % page["index"])

    def test_acceptance_manifest_matches_constants(self):
        _p, stream = self._read("build-subsys-selftest/recovery.romfs")
        _header, entries = romfspack.parse_image(stream)
        names = {entry["name"]: entry for entry in entries}
        _p, zmod = self._read("build/modules/sio.zmod")
        self.assertEqual(names["sio.zmod"]["payload"], zmod)
        cross = names["crosspage.bin"]
        self.assertEqual(cross["size"], 0xC040)
        for i in range(cross["size"]):
            self.assertEqual(cross["payload"][i],
                             ((i & 0xFF) ^ ((i >> 8) & 0xFF)) & 0xFF,
                             "pattern byte %d differs" % i)
        # The asset must span the 48-KiB window boundary.
        self.assertLess(cross["offset"], 0xC000)
        self.assertGreater(cross["offset"] + cross["size"], 0xC000)
        # romfs_selftest.h mirrors the acceptance-manifest geometry.  The
        # payload-offset constant is target-owned (tests/target/include/);
        # the target uses it to place its boundary reads, so only the
        # size, window and asset name are pinned here.
        env = parse_c_header(ROMFS_SELFTEST_H)
        self.assertEqual(evaluate("ZOS_RFST_CROSSPAGE_SIZE", env),
                         cross["size"])
        self.assertEqual(evaluate("ZOS_RFST_WINDOW", env), 0xC000)
        self.assertIn('"crosspage.bin"', open(ROMFS_SELFTEST_H).read())
        # The acceptance manifest is a test artifact: the production
        # manifest must not carry the pattern asset.
        production = json.loads(
            open(os.path.join(ROOT, "recovery", "manifest.json")).read())
        self.assertNotIn("crosspage.bin",
                         {e["name"] for e in production["files"]})

    def test_image_markers_and_no_embedded_catalog(self):
        _p, image = self._read("build-subsys-selftest/"
                               "zephyros-subsys-selftest.bin")
        for marker in (b"romfs selftest: OK\r\n",
                       b"console: dynamic sio0.b OK\r\n",
                       b"romfs: corrupt recovery image"):
            self.assertIn(marker, image)
        syms = read_symbols(os.path.join(ROOT, "build-subsys-selftest",
                                         "zephyros.map"))
        self.assertNotIn("_z4_module_boot", syms)
        self.assertNotIn("_zmod_catalog", syms)

    def test_corrupt_image_differs_only_in_corrupted_metadata(self):
        # The corrupt build packs the production manifest and then patches
        # the header reserved field, so the clean counterpart is the
        # production recovery stream (not the subsystem acceptance store).
        _p, clean = self._read("build/recovery.romfs")
        _p, corrupt = self._read("build-romfs-corrupt/recovery.romfs")
        self.assertEqual(len(clean), len(corrupt))
        diff = [i for i in range(len(clean)) if clean[i] != corrupt[i]]
        self.assertEqual(diff, [10],
                         "corrupt stream must differ exactly in the header "
                         "reserved field")
        self.assertEqual(corrupt[10], 1)
        # The kernel must reject the corrupt stream; the tool would too.
        with self.assertRaises(romfspack.RomfsError):
            romfspack.parse_image(corrupt)

    def test_corrupt_image_carries_panic_marker(self):
        _p, image = self._read("build-romfs-corrupt/"
                               "zephyros-romfs-corrupt.bin")
        self.assertIn(b"PANIC: ", image)
        self.assertIn(b"romfs: corrupt recovery image", image)
        syms = read_symbols(os.path.join(ROOT, "build-romfs-corrupt",
                                         "zephyros.map"))
        self.assertNotIn("_romfs_selftest_result", syms)

    def test_smoke_constants_match_the_images(self):
        """serial_smoke's ROMFS expectations must exist verbatim in the
        subsystem image (and the corrupt-store image must not contain the
        success marker)."""
        import serial_smoke
        _p, success = self._read("build-subsys-selftest/"
                                 "zephyros-subsys-selftest.bin")
        _p, corrupt = self._read("build-romfs-corrupt/"
                                 "zephyros-romfs-corrupt.bin")
        for const in (serial_smoke.CONSOLE_DYNAMIC_OK,
                      serial_smoke.SUBSYS_ROMFS_OK):
            self.assertIn(const, success)
        # panic() writes "PANIC: " and the message as separate literals;
        # the serial transcript concatenates them.  The corrupt image still
        # carries the unreached success literal in kernel_main's code (the
        # runtime transcript is what proves it is not executed).
        self.assertIn(b"PANIC: ", corrupt)
        self.assertIn(b"romfs: corrupt recovery image", corrupt)
        self.assertNotIn(serial_smoke.SUBSYS_ROMFS_OK, corrupt)


if __name__ == "__main__":
    unittest.main()
