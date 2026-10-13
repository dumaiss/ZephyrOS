#!/usr/bin/env python3
"""Host-side MOD driver-container tests.

The container is one flat plan per driver image: a 16-byte header, the
segment records, the endpoint records, the dependency records, the
publication records and the payload (`tools/modpack.py`,
`formats/include/mod.h`).  There is no module type, no version
field, no relocation table and no granule geometry: the packer resolves
every symbol while it links, and `parse_container` is the format's only
validator (the target loader just bounds what protects the nucleus).

Covered here: the `.rel` reader, the flat container round trip, the host
validation of every field, and the packer CLI.  The host assembly of a
driver image is covered by `tests/test_driver_images.py`, and the real
images by `tests/test_hid.py`, `tests/test_ioc.py` and
`tests/test_v9958.py`.
"""

import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)

import modpack  # noqa: E402

HEADER = modpack.HEADER_SIZE
SEGMENT = modpack.SEGMENT_SIZE
ENDPOINT = modpack.ENDPOINT_SIZE
DEPENDENCY = modpack.DEPENDENCY_SIZE
PUBLICATION = modpack.PUBLICATION_SIZE


def rel_text(module, areas, symbols, chunks):
    """Build an XL4 .rel text file (the sdasz80 subset the packer reads).

    areas:   list of (name, size)
    symbols: list of (name, value, area index); an entry with a `None`
             area is a reference to a symbol another object defines.
    chunks:  list of (area_index, addr, data, groups); each group is a
             (mode, rtp, ref) relocation, `rtp` being the offset of the
             patched word inside the chunk data and `ref` either an index
             or a symbol name.

    Returns (text, {symbol name: index in the object's symbol table}).
    """
    refs = [item for item in symbols if item[2] is None]
    defs = [item for item in symbols if item[2] is not None]
    index = dict((item[0], position) for position, item in
                 enumerate(refs + defs))
    lines = [
        "XL4",
        "H %d areas %X global symbols" % (len(areas), len(refs + defs)),
        "M %s" % module,
        "O -mz80 sdcccall(1)",
    ]
    for name, _value, _area in refs:
        lines.append("S %s Ref00000000" % name)
    for area_index, (name, size) in enumerate(areas):
        lines.append("A %s size %X flags 0 addr 0" % (name, size))
        for sym_name, value, area in defs:
            if area == area_index:
                lines.append("S %s Def%08X" % (sym_name, value))
    for area, addr, data, groups in chunks:
        vals = [addr & 0xFF, (addr >> 8) & 0xFF, (addr >> 16) & 0xFF,
                (addr >> 24) & 0xFF] + list(data)
        lines.append("T " + " ".join("%02X" % v for v in vals))
        rvals = [0x00, 0x00, area & 0xFF, (area >> 8) & 0xFF]
        for mode, rtp, ref in groups:
            if isinstance(ref, str):
                ref = index[ref]
            rvals += [mode, rtp, ref & 0xFF, (ref >> 8) & 0xFF]
        lines.append("R " + " ".join("%02X" % v for v in rvals))
    return "\n".join(lines) + "\n", index


def flat_container(segments, endpoints=(), dependencies=(), publications=(),
                   payload=b"", init=None):
    """Build a flat container from records, canonically laid out.

    segments:      (role, base, image_size, bss_size, entry, payload_offset)
    endpoints:     (class, instance, version, flags, number, dispatch)
    dependencies:  (class, instance, number)
    publications:  (name, type, number, object, flags)
    """
    payload_offset = (HEADER + SEGMENT * len(segments) +
                      ENDPOINT * len(endpoints) +
                      DEPENDENCY * len(dependencies) +
                      PUBLICATION * len(publications))
    total = payload_offset + len(payload)
    body = bytearray(struct.pack(
        "<4sBBBBHHHH", modpack.MAGIC, len(segments), len(endpoints),
        len(dependencies), len(publications),
        modpack.NO_ENTRY if init is None else init, payload_offset, total,
        0))
    for role, base, image, bss, entry, offset in segments:
        body += struct.pack("<BBHHHHH", modpack.ROLE_CODES[role], 0, base,
                            image, bss,
                            modpack.NO_ENTRY if entry is None else entry,
                            offset)
    for record in endpoints:
        body += struct.pack("<BBBBHH", *record)
    for record in dependencies:
        body += struct.pack("<BBH", *record)
    for name, type_code, number, obj, flags in publications:
        raw = name.encode("ascii")
        body += raw + b"\x00" * (modpack.PUBLICATION_NAME - len(raw))
        body += struct.pack("<BBBB", type_code, number, obj, flags)
    return bytes(body) + payload


def load_driver(tmp, name, objects, manifest):
    """Write one synthetic driver's objects and manifest; return the
    loaded manifest dict."""
    for file_name, (areas, symbols, chunks) in objects.items():
        text, _ = rel_text(file_name.replace(".rel", ""), areas, symbols,
                           chunks)
        with open(os.path.join(tmp, file_name), "w") as handle:
            handle.write(text)
    manifest = dict(manifest)
    manifest.setdefault("name", name)
    manifest["objects"] = list(objects)
    path = os.path.join(tmp, name + ".json")
    with open(path, "w") as handle:
        json.dump(manifest, handle)
    return modpack.load_manifest(path, tmp)


def pack_driver(tmp, name, objects, manifest):
    """Host-assemble one single-image set with the real packer."""
    packed = modpack.pack_driver_manifest(
        load_driver(tmp, name, objects, manifest))
    rows, _ledger = modpack.plan_driver_set([packed])
    return modpack.build_driver_container(packed, rows[0])


def simple_image(init="entry"):
    """One hot segment holding `_CODE` (the shape every real image has)."""
    objects = {
        "t.rel": (
            [("_CODE", 4)],
            [("_entry", 0, 0)],
            [(0, 0, bytes([0xC9, 0x00, 0x00, 0x00]), [])],
        ),
    }
    manifest = {
        "init": init,
        "segments": [{"role": "hot", "image_areas": ["_CODE"]}],
    }
    return objects, manifest


class TestRelParsing(unittest.TestCase):
    def test_area_and_symbol_records(self):
        text, _ = rel_text(
            "t",
            [("_CODE", 6), ("_DATA", 2)],
            [("_entry", 0, 0), ("_state", 0, 1)],
            [(0, 0, bytes([0x21, 0x00, 0x00, 0xC9]), [(0x00, 0x04, 0x00)])],
        )
        obj = modpack.parse_rel(text, "t.rel")
        self.assertEqual([a.name for a in obj.areas], ["_CODE", "_DATA"])
        self.assertEqual(obj.areas[0].size, 6)
        self.assertEqual(len(obj.symbols), 2)
        self.assertEqual(obj.symbols[0].kind, "def")
        self.assertEqual(obj.symbols[0].area, 0)
        self.assertEqual(len(obj.chunks), 1)
        chunk = obj.chunks[0]
        self.assertEqual(chunk.area_index, 0)
        self.assertEqual(chunk.addr, 0)
        self.assertEqual(chunk.data, bytes([0x21, 0x00, 0x00, 0xC9]))
        self.assertEqual(chunk.relocs, [(0x00, 0x04, 0x00)])

    def test_bad_radix_rejected(self):
        with self.assertRaises(modpack.ModError):
            modpack.parse_rel("XH4\n", "t.rel")

    def test_t_without_r_rejected(self):
        with self.assertRaises(modpack.ModError):
            modpack.parse_rel("XL4\nT 00 00 00 00 11\n", "t.rel")


class TestPackRoundTrip(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def test_valid_container_fields(self):
        container = pack_driver(self.tmp.name, "svc", *simple_image())
        info = modpack.parse_container(container)
        self.assertEqual(info["total_size"], len(container))
        self.assertEqual(info["payload_offset"], HEADER + SEGMENT)
        self.assertEqual(info["image_size"], 4)
        self.assertEqual(info["bss_size"], 0)
        self.assertEqual(info["payload"], bytes([0xC9, 0x00, 0x00, 0x00]))
        self.assertEqual(len(info["segments"]), 1)
        segment = info["segments"][0]
        self.assertEqual(segment["role"], "hot")
        self.assertGreaterEqual(segment["base"], modpack.HOT_TIER_BASE)
        self.assertLess(segment["base"], modpack.HOT_TIER_END)
        self.assertEqual(info["init_offset"], segment["base"])
        self.assertEqual(info["endpoints"], [])
        self.assertEqual(info["dependencies"], [])
        self.assertEqual(info["publications"], [])

    def test_endpoint_and_publication_records(self):
        objects, manifest = simple_image()
        manifest["endpoints"] = [{"class": 1, "instance": 0, "version": 3,
                                  "dispatch": "_entry", "flags": 1}]
        manifest["publishes"] = [{"path": "/dev/serial/0", "type": "SERIAL",
                                  "endpoint": {"class": 1, "instance": 0}}]
        container = pack_driver(self.tmp.name, "svc", objects, manifest)
        info = modpack.parse_container(container)
        endpoint = info["endpoints"][0]
        self.assertEqual((endpoint["class"], endpoint["instance"]), (1, 0))
        self.assertEqual(endpoint["version"], 3)
        self.assertEqual(endpoint["flags"], modpack.EP_REENTRANT)
        self.assertEqual(endpoint["number"], 1)
        self.assertEqual(endpoint["dispatch"], info["init_offset"])
        publication = info["publications"][0]
        self.assertEqual(publication["name"], "/serial/0")
        self.assertEqual(publication["type"], modpack.TYPES["SERIAL"])
        self.assertEqual(publication["number"], 1)
        self.assertEqual(publication["flags"], 0)

    def test_dependency_resolution_and_optional_tail(self):
        first = {
            "t1.rel": (
                [("_CODE", 4)],
                [("_e1", 0, 0)],
                [(0, 0, bytes([0xC9, 0x00, 0x00, 0x00]), [])],
            ),
        }
        second = {
            "t2.rel": (
                [("_CODE", 4)],
                [("_e2", 0, 0)],
                [(0, 0, bytes([0xC9, 0x00, 0x00, 0x00]), [])],
            ),
        }
        packs = [
            modpack.pack_driver_manifest(load_driver(
                self.tmp.name, "one", first,
                {"init": "e1",
                 "segments": [{"role": "hot", "image_areas": ["_CODE"]}],
                 "endpoints": [{"class": 1, "instance": 0, "version": 1,
                                "dispatch": "_e1"}]})),
            modpack.pack_driver_manifest(load_driver(
                self.tmp.name, "two", second,
                {"init": "e2",
                 "segments": [{"role": "hot", "image_areas": ["_CODE"]}],
                 "endpoints": [{"class": 2, "instance": 0, "version": 1,
                                "dispatch": "_e2"}],
                 "depends": [{"class": 1, "instance": 0},
                             {"class": 9, "instance": 0, "optional": 1}]})),
        ]
        rows, _ledger = modpack.plan_driver_set(packs)
        containers = [modpack.build_driver_container(packed, row)
                      for packed, row in zip(packs, rows)]
        info = modpack.parse_container(containers[1], "two.mod")
        self.assertEqual([d["number"] for d in info["dependencies"]], [1, 0])
        self.assertEqual(info["endpoints"][0]["number"], 2)

    def test_deterministic_output(self):
        first = pack_driver(self.tmp.name, "svc", *simple_image())
        second = pack_driver(self.tmp.name, "svc", *simple_image())
        self.assertEqual(first, second)

    def test_unresolved_external_rejected(self):
        objects = {
            "t.rel": (
                [("_CODE", 4)],
                [("_foreign", 0, None), ("_entry", 0, 0)],
                [(0, 0, bytes([0x21, 0x00, 0x00, 0xC9]),
                  [(0x02, 0x04, "_foreign")])],
            ),
        }
        manifest = {"init": "entry",
                    "segments": [{"role": "hot",
                                  "image_areas": ["_CODE"]}]}
        with self.assertRaisesRegex(modpack.ModError,
                                    "unresolved external symbol"):
            pack_driver(self.tmp.name, "svc", objects, manifest)

    def test_byte_fragmented_mode_rejected(self):
        objects = {
            "t.rel": (
                [("_CODE", 4)],
                [("_entry", 0, 0)],
                [(0, 0, bytes([0x06, 0x00, 0x00, 0xC9]),
                  [(0x09, 0x04, 0x00)])],
            ),
        }
        manifest = {"init": "entry",
                    "segments": [{"role": "hot",
                                  "image_areas": ["_CODE"]}]}
        with self.assertRaisesRegex(modpack.ModError,
                                    "unsupported relocation mode 09"):
            pack_driver(self.tmp.name, "svc", objects, manifest)

    def test_pc_relative_mode_rejected(self):
        objects = {
            "t.rel": (
                [("_CODE", 4)],
                [("_entry", 0, 0)],
                [(0, 0, bytes([0x18, 0x00, 0x00, 0xC9]),
                  [(0x04, 0x04, 0x00)])],
            ),
        }
        manifest = {"init": "entry",
                    "segments": [{"role": "hot",
                                  "image_areas": ["_CODE"]}]}
        with self.assertRaisesRegex(modpack.ModError,
                                    "unsupported relocation mode 04"):
            pack_driver(self.tmp.name, "svc", objects, manifest)

    def test_unlisted_area_rejected(self):
        objects = {
            "t.rel": (
                [("_CODE", 4), ("_GSINIT", 2)],
                [("_entry", 0, 0)],
                [(0, 0, bytes([0xC9, 0x00, 0x00, 0x00]), [])],
            ),
        }
        manifest = {"init": "entry",
                    "segments": [{"role": "hot",
                                  "image_areas": ["_CODE"]}]}
        with self.assertRaisesRegex(modpack.ModError,
                                    "not listed in any segment"):
            pack_driver(self.tmp.name, "svc", objects, manifest)

    def test_bss_with_initialized_data_rejected(self):
        objects = {
            "t.rel": (
                [("_CODE", 4), ("_DATA", 4)],
                [("_entry", 0, 0), ("_state", 0, 1)],
                [(0, 0, bytes([0xC9, 0x00, 0x00, 0x00]), []),
                 (1, 0, bytes([0x01, 0x02]), [])],
            ),
        }
        manifest = {
            "init": "entry",
            "segments": [{"role": "hot", "image_areas": ["_CODE"],
                          "bss_areas": ["_DATA"]}],
        }
        with self.assertRaisesRegex(modpack.ModError,
                                    "BSS area _DATA contains"):
            pack_driver(self.tmp.name, "svc", objects, manifest)

    def test_bad_init_entry_rejected(self):
        objects = {
            "t.rel": (
                [("_CODE", 4)],
                [("_entry", 4, 0)],
                [(0, 0, bytes([0xC9, 0x00, 0x00, 0x00]), [])],
            ),
        }
        manifest = {"init": "entry",
                    "segments": [{"role": "hot",
                                  "image_areas": ["_CODE"]}]}
        with self.assertRaisesRegex(modpack.ModError,
                                    "not defined|outside"):
            pack_driver(self.tmp.name, "svc", objects, manifest)

    def test_empty_image_area_rejected(self):
        objects = {
            "t.rel": (
                [("_CODE", 4)],
                [("_entry", 0, 0)],
                [(0, 0, bytes([0xC9, 0x00, 0x00, 0x00]), [])],
            ),
        }
        manifest = {"init": "entry",
                    "segments": [{"role": "hot", "image_areas": ["_CODE"],
                                  "bss_areas": ["_DATA"]}]}
        with self.assertRaisesRegex(modpack.ModError,
                                    "empty or missing"):
            pack_driver(self.tmp.name, "svc", objects, manifest)


class TestContainerValidation(unittest.TestCase):
    """`parse_container` is the format's only validator: every field is
    checked once, on the host (the loader only bounds what protects the
    nucleus)."""

    def valid(self):
        return flat_container([("hot", 0x6000, 4, 0, None, 0)],
                              payload=b"\xC9\x00\x00\x00")

    def reject(self, data, pattern):
        with self.assertRaisesRegex(modpack.ModError, pattern):
            modpack.parse_container(data, "svc.mod")

    def patch(self, data, offset, value, width=1):
        raw = bytearray(data)
        raw[offset:offset + width] = value.to_bytes(width, "little")
        return bytes(raw)

    def test_short_header(self):
        self.reject(self.valid()[:8], "shorter than")

    def test_bad_magic(self):
        self.reject(self.patch(self.valid(), 0, ord("X")), "bad magic")

    def test_reserved_word_not_zero(self):
        self.reject(self.patch(self.valid(), 0x0E, 1), "reserved word")

    def test_segment_count_bounds(self):
        self.reject(self.patch(self.valid(), 4, 0), "bad segment count")
        self.reject(self.patch(self.valid(), 4, modpack.MAX_SEGMENTS + 1),
                    "bad segment count")

    def test_endpoint_count_bound(self):
        self.reject(self.patch(self.valid(), 5, modpack.MAX_ENDPOINTS + 1),
                    "bad endpoint count")

    def test_dependency_count_bound(self):
        self.reject(self.patch(self.valid(), 6,
                               modpack.MAX_DEPENDENCIES + 1),
                    "bad dependency count")

    def test_publication_count_bound(self):
        self.reject(self.patch(self.valid(), 7,
                               modpack.MAX_PUBLICATIONS + 1),
                    "bad publication count")

    def test_total_size_mismatch(self):
        self.reject(self.patch(self.valid(), 0x0C, 0x4000, 2),
                    "total_size")

    def test_payload_offset_not_canonical(self):
        self.reject(self.patch(self.valid(), 0x0A, HEADER + SEGMENT + 2, 2),
                    "payload_offset")

    def test_unknown_role(self):
        self.reject(self.patch(self.valid(), HEADER, 9), "unknown role")

    def test_duplicate_role(self):
        two = flat_container([("hot", 0x6000, 2, 0, None, 0),
                              ("io", 0x8000, 2, 0, None, 2)],
                             payload=b"\xC9\x00\xC9\x00")
        self.reject(self.patch(two, HEADER + SEGMENT, 1),
                    "duplicate segment role")

    def test_segment_flags_not_zero(self):
        self.reject(self.patch(self.valid(), HEADER + 1, 1), "flags are not")

    def test_empty_segment(self):
        empty = flat_container([("hot", 0x6000, 0, 0, None, 0)])
        self.reject(empty, "is empty")

    def test_segment_overruns_payload(self):
        over = flat_container([("hot", 0x6000, 4, 0, None, 0)],
                              payload=b"\xC9\x00")
        self.reject(over, "overruns the payload")

    def test_segment_outside_its_tier(self):
        self.reject(self.patch(self.valid(), HEADER + 2, 0x5000, 2),
                    "does not fit its tier")

    def test_bad_boot_entry(self):
        boot = flat_container([("hot", 0x6000, 2, 0, None, 0),
                               ("boot", 0x1000, 4, 0, 0x2000, 2)],
                              payload=b"\xC9\x00\xC9\x00\xC9\x00\xC9\x00")
        self.reject(boot, "bad entry")

    def test_hot_segment_required(self):
        boot = flat_container([("boot", 0x1000, 4, 0, None, 0)],
                              payload=b"\xC9\x00\x00\x00")
        self.reject(boot, "needs a hot segment")

    def test_init_outside_the_hot_image(self):
        self.reject(self.patch(self.valid(), 0x08, 0x7000, 2),
                    "outside the hot image")

    def test_endpoint_number_zero(self):
        image = flat_container([("hot", 0x6000, 4, 0, None, 0)],
                               endpoints=[(1, 0, 1, 0, 0, 0x6000)],
                               payload=b"\xC9\x00\x00\x00")
        self.reject(image, "number 0")

    def test_endpoint_flags(self):
        image = flat_container([("hot", 0x6000, 4, 0, None, 0)],
                               endpoints=[(1, 0, 1, 0x80, 1, 0x6000)],
                               payload=b"\xC9\x00\x00\x00")
        self.reject(image, "unknown flags")

    def test_endpoint_dispatch_outside_the_hot_image(self):
        image = flat_container([("hot", 0x6000, 4, 0, None, 0)],
                               endpoints=[(1, 0, 1, 0, 1, 0x7000)],
                               payload=b"\xC9\x00\x00\x00")
        self.reject(image, "outside the hot image")

    def test_dependency_number_above_the_exports(self):
        image = flat_container([("hot", 0x6000, 4, 0, None, 0)],
                               endpoints=[(1, 0, 1, 0, 1, 0x6000)],
                               dependencies=[(1, 0, 2)],
                               payload=b"\xC9\x00\x00\x00")
        self.reject(image, "which no image exports")

    def test_publication_name(self):
        image = flat_container([("hot", 0x6000, 4, 0, None, 0)],
                               endpoints=[(1, 0, 1, 0, 1, 0x6000)],
                               publications=[("", 1, 1, 0, 0)],
                               payload=b"\xC9\x00\x00\x00")
        self.reject(image, "bad name")

    def test_publication_record(self):
        for published in (("/serial/0", 0, 1, 0, 0),
                          ("/serial/0", 1, 0, 0, 0),
                          ("/serial/0", 1, 1, 0, 1)):
            image = flat_container([("hot", 0x6000, 4, 0, None, 0)],
                                   endpoints=[(1, 0, 1, 0, 1, 0x6000)],
                                   publications=[published],
                                   payload=b"\xC9\x00\x00\x00")
            self.reject(image, "bad record")


class TestPackCli(unittest.TestCase):
    """The packer CLI the Makefile drives: pack, validate, dump."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def run_cli(self, *args):
        return subprocess.run(
            [sys.executable, os.path.join(ROOT, "tools", "modpack.py")] +
            list(args), cwd=self.tmp.name, capture_output=True, text=True)

    def test_pack_validate_and_dump(self):
        objects, manifest = simple_image()
        manifest["name"] = "svc"
        manifest["objects"] = list(objects)
        for file_name, (areas, symbols, chunks) in objects.items():
            text, _ = rel_text("t", areas, symbols, chunks)
            with open(os.path.join(self.tmp.name, file_name), "w") as handle:
                handle.write(text)
        manifest_path = os.path.join(self.tmp.name, "svc.json")
        with open(manifest_path, "w") as handle:
            json.dump(manifest, handle)
        out = os.path.join(self.tmp.name, "svc.mod")
        packed = self.run_cli("pack", manifest_path, out,
                              "--objdir", self.tmp.name,
                              "--set", manifest_path)
        self.assertEqual(packed.returncode, 0, packed.stderr)

        validated = self.run_cli("validate", out)
        self.assertEqual(validated.returncode, 0, validated.stderr)

        dumped = self.run_cli("dump", out)
        self.assertEqual(dumped.returncode, 0, dumped.stderr)
        self.assertIn("hot", dumped.stdout)

    def test_validate_rejects_a_corrupt_container(self):
        out = os.path.join(self.tmp.name, "bad.mod")
        with open(out, "wb") as handle:
            handle.write(b"XXXX" + b"\x00" * 12)
        result = self.run_cli("validate", out)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("bad magic", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
