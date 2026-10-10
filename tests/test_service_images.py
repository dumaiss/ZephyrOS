#!/usr/bin/env python3
"""Host-side tests for the host-assembled service images (packages 41b-42c).

Pure tests build synthetic SDCC .rel text, pack service containers with
`tools/zmodpack.py` and check the host assembly: the linked segment bases,
the relocation arithmetic the packer applies, the host-assigned endpoint
numbers, the dependency resolution (including the optional and missing
cases), the load-order validation and the tier plan.  The flat container
carries no relocation records or version field (work package 42c), so the
tests that read them are gone with them.

Run with: `python3 -m unittest tests.test_service_images`.
"""

import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)

import zmodpack  # noqa: E402

HOT_TIER = (0x6000, 0xE000)
IO_TIER = (0x8000, 0xC000)
BOOT_TIER = (0x0000, 0x6000)


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


def load_service(tmp, name, objects, manifest):
    """Write one synthetic service's objects and manifest; return the
    loaded manifest dict."""
    for file_name, (areas, symbols, chunks) in objects.items():
        text, _ = rel_text(file_name.replace(".rel", ""), areas, symbols,
                           chunks)
        with open(os.path.join(tmp, file_name), "w") as handle:
            handle.write(text)
    manifest = dict(manifest)
    manifest.setdefault("name", name)
    manifest.setdefault("type", "service")
    manifest.setdefault("abi", 3)
    manifest.setdefault("flags", 0)
    manifest["objects"] = list(objects)
    path = os.path.join(tmp, name + ".json")
    with open(path, "w") as handle:
        json.dump(manifest, handle)
    return zmodpack.load_manifest(path, tmp)


def pack_set(tmp, images):
    """Host-assemble a set of synthetic images, in load order: returns
    (containers, ledger) with the same code path the packer CLI uses."""
    packs = [zmodpack.pack_service_manifest(
        load_service(tmp, name, objects, manifest))
        for name, objects, manifest in images]
    rows, ledger = zmodpack.plan_service_set(packs)
    return [zmodpack.build_service_container(packed, row)
            for packed, row in zip(packs, rows)], ledger


def pack_service(tmp, name, objects, manifest):
    """Host-assemble one single-image set."""
    containers, _ = pack_set(tmp, [(name, objects, manifest)])
    return containers[0]


class TestServiceFormat(unittest.TestCase):
    """The container's service section, round-tripped through the packer."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def test_round_trip_keeps_segments_endpoints_and_dependencies(self):
        container = pack_service(self.tmp.name, "svc", {
            "hot.rel": (
                [("_CODE", 8), ("_HOT", 2)],
                [("_svc_init", 0, 0), ("_svc_dispatch", 4, 0),
                 ("_slow", 6, 0)],
                [(0, 0, [0x00, 0x00, 0xC9, 0x00, 0x00, 0xC9, 0x00, 0x00],
                  [])]),
            "boot.rel": (
                [("_BOOT", 4), ("_BOOT_DATA", 2)],
                [("_svc_boot", 0, 0), ("_boot_runs", 0, 1)],
                [(0, 0, [0x3E, 0x01, 0x32, 0x00], [])]),
            "io.rel": (
                [("_IOBUF", 4), ("_SERVICE_IO", 28)],
                [],
                [(0, 0, [0xAA] * 4, [])]),
        }, {
            "init": "_svc_init",
            "segments": [
                {"role": "hot", "image_areas": ["_CODE"],
                 "bss_areas": ["_HOT"]},
                {"role": "boot", "image_areas": ["_BOOT"],
                 "bss_areas": ["_BOOT_DATA"], "entry": "_svc_boot"},
                {"role": "io", "image_areas": ["_IOBUF"],
                 "bss_areas": ["_SERVICE_IO"]},
            ],
            "endpoints": [{"class": 3, "instance": 0, "version": 1,
                           "dispatch": "_svc_dispatch", "flags": 0}],
            "depends": [{"class": 5, "instance": 0, "optional": 1}],
        })
        info = zmodpack.parse_container(container)
        self.assertEqual([segment["role"] for segment in info["segments"]],
                         ["hot", "boot", "io"])
        hot, boot, io = info["segments"]
        self.assertEqual((hot["image_size"], hot["bss_size"]), (8, 2))
        self.assertEqual((boot["image_size"], boot["bss_size"]), (4, 2))
        self.assertEqual((io["image_size"], io["bss_size"]), (4, 28))
        # The payload is the segment images back to back, in table order.
        self.assertEqual([hot["payload_offset"], boot["payload_offset"],
                          io["payload_offset"]], [0, 8, 12])
        self.assertEqual(info["image_size"], 16)
        self.assertEqual(info["bss_size"], 2 + 2 + 28)
        # Every address the container carries is final and inside the
        # tier of its role (architecture 36.6, 36.11).
        for segment in info["segments"]:
            base, end = {"hot": HOT_TIER, "io": IO_TIER,
                         "boot": BOOT_TIER, "bulk": BOOT_TIER}[
                             segment["role"]]
            self.assertGreaterEqual(segment["base"], base)
            self.assertLessEqual(segment["base"] + segment["image_size"] +
                                 segment["bss_size"], end)
        self.assertEqual(boot["entry"],
                         boot["base"] + 0)
        self.assertEqual(info["init_offset"], hot["base"] + 0)
        endpoint = info["endpoints"][0]
        self.assertEqual((endpoint["class"], endpoint["instance"],
                          endpoint["version"]), (3, 0, 1))
        self.assertEqual(endpoint["number"], 1)
        self.assertEqual(endpoint["dispatch"], hot["base"] + 4)
        # The one declared dependency is optional and nothing exports it.
        self.assertEqual(info["dependencies"],
                         [{"class": 5, "instance": 0, "number": 0}])

    def test_every_segment_role_has_exactly_one_segment(self):
        with self.assertRaises(zmodpack.ZmodError) as caught:
            pack_service(self.tmp.name, "dup", {
                "dup.rel": ([("_CODE", 2)], [("_dup_init", 0, 0)],
                            [(0, 0, [0xC9, 0x00], [])]),
            }, {
                "init": "_dup_init",
                "segments": [
                    {"role": "hot", "image_areas": ["_CODE"]},
                    {"role": "hot", "image_areas": ["_CODE"]},
                ],
            })
        self.assertIn("duplicate role", str(caught.exception))

    def test_a_hot_segment_is_required(self):
        with self.assertRaises(zmodpack.ZmodError) as caught:
            pack_service(self.tmp.name, "noht", {
                "io.rel": ([("_IOBUF", 4)], [], [(0, 0, [0xAA] * 4, [])]),
            }, {
                "segments": [{"role": "io", "image_areas": ["_IOBUF"]}],
            })
        self.assertIn("hot segment", str(caught.exception))

    def test_only_the_boot_role_carries_an_entry(self):
        with self.assertRaises(zmodpack.ZmodError) as caught:
            pack_service(self.tmp.name, "badent", {
                "x.rel": ([("_CODE", 2)], [("_x", 0, 0)],
                          [(0, 0, [0xC9, 0x00], [])]),
            }, {
                "init": "_x",
                "segments": [{"role": "hot", "image_areas": ["_CODE"],
                              "entry": "_x"}],
            })
        self.assertIn("entry", str(caught.exception))

    def test_a_boot_entry_must_live_in_the_boot_segment(self):
        with self.assertRaises(zmodpack.ZmodError) as caught:
            pack_service(self.tmp.name, "bootent", {
                "h.rel": ([("_CODE", 2)], [("_svc_init", 0, 0)],
                          [(0, 0, [0xC9, 0x00], [])]),
                "b.rel": ([("_BOOT", 2)], [("_elsewhere", 4, 0)],
                          [(0, 0, [0xC9, 0x00], [])]),
            }, {
                "init": "_svc_init",
                "segments": [
                    {"role": "hot", "image_areas": ["_CODE"]},
                    {"role": "boot", "image_areas": ["_BOOT"],
                     "entry": "_elsewhere"},
                ],
            })
        self.assertIn("boot", str(caught.exception))

    def test_init_must_live_in_the_hot_segment(self):
        with self.assertRaises(zmodpack.ZmodError) as caught:
            pack_service(self.tmp.name, "badinit", {
                "h.rel": ([("_CODE", 2)], [("_svc_init", 0, 0)],
                          [(0, 0, [0xC9, 0x00], [])]),
                "b.rel": ([("_BOOT", 2)], [("_boot_entry", 0, 0)], []),
            }, {
                "init": "_boot_entry",
                "segments": [
                    {"role": "hot", "image_areas": ["_CODE"]},
                    {"role": "boot", "image_areas": ["_BOOT"],
                     "entry": "_boot_entry"},
                ],
            })
        self.assertIn("init", str(caught.exception))

    def test_endpoint_flags_are_bounded(self):
        with self.assertRaises(zmodpack.ZmodError) as caught:
            pack_service(self.tmp.name, "flags", {
                "fl.rel": ([("_CODE", 4)], [("_fl_init", 0, 0),
                                            ("_fl_dispatch", 0, 0)],
                           [(0, 0, [0xC9, 0x00, 0x00, 0x00], [])]),
            }, {
                "init": "_fl_init",
                "segments": [{"role": "hot", "image_areas": ["_CODE"]}],
                "endpoints": [{"class": 1, "instance": 0, "version": 1,
                               "dispatch": "_fl_dispatch", "flags": 0x80}],
            })
        self.assertIn("flags", str(caught.exception))


class TestHostLinking(unittest.TestCase):
    """The host applies each ZREL record against the final segment bases
    (architecture 36.11): the container carries linked bytes only."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def build(self):
        """A hot image with one pointer into the io segment and one into
        its own BSS, plus a boot segment pointing at hot code."""
        return pack_service(self.tmp.name, "rel", {
            "hot.rel": (
                [("_CODE", 8), ("_HOT", 2)],
                [("_svc_init", 0, 0), ("_hot_data", 6, 0),
                 ("_buffer", 0, None)],
                [(0, 0, [0x00, 0x00, 0x00, 0x00, 0xC9, 0x00, 0x00, 0x00],
                  [(0x02, 4, "_buffer"), (0x02, 6, "_hot_data")])]),
            "boot.rel": (
                [("_BOOT", 6)],
                [("_svc_boot", 0, 0), ("_svc_init", 0, None)],
                [(0, 0, [0x21, 0x00, 0x00, 0xC9, 0x00, 0x00],
                  [(0x02, 5, "_svc_init")])]),
            "io.rel": (
                [("_IOBUF", 16)],
                [("_buffer", 0, 0)],
                [(0, 0, [0x11] * 4, [])]),
        }, {
            "init": "_svc_init",
            "segments": [
                {"role": "hot", "image_areas": ["_CODE"],
                 "bss_areas": ["_HOT"]},
                {"role": "boot", "image_areas": ["_BOOT"],
                 "entry": "_svc_boot"},
                {"role": "io", "image_areas": ["_IOBUF"]},
            ],
        })

    def test_the_io_pointer_lands_in_the_io_blocks_final_base(self):
        info = zmodpack.parse_container(self.build())
        hot = info["segments"][0]
        io = info["segments"][2]
        payload = info["payload"]
        # `_buffer` is at io offset 0: the linked word is the io base.
        self.assertEqual(payload[0] | (payload[1] << 8), io["base"])
        # `_hot_data` is at hot offset 6 (after the 8-byte image start of
        # the BSS area? no: BSS starts at image_size 8; the symbol sits at
        # 6, inside the image).
        self.assertEqual(payload[2] | (payload[3] << 8), hot["base"] + 6)

    def test_the_boot_pointer_lands_in_the_hot_images_final_base(self):
        info = zmodpack.parse_container(self.build())
        hot = info["segments"][0]
        boot = info["segments"][1]
        payload = info["payload"]
        self.assertEqual(payload[boot["payload_offset"] + 1] |
                         (payload[boot["payload_offset"] + 2] << 8),
                         hot["base"])

    def test_a_relocation_overflow_is_refused_at_pack_time(self):
        # A word holding F000h plus the target segment's base (at least
        # 6000h for a hot segment) overflows 16 bits at link time.
        with self.assertRaises(zmodpack.ZmodError) as caught:
            pack_service(self.tmp.name, "ovf", {
                "h.rel": (
                    [("_CODE", 4)],
                    [("_ovf_init", 0, 0), ("_hot_data", 2, 0)],
                    [(0, 0, [0xC9, 0x00, 0x00, 0xF0],
                      [(0x02, 6, "_hot_data")])]),
            }, {
                "init": "_ovf_init",
                "segments": [{"role": "hot", "image_areas": ["_CODE"]}],
            })
        self.assertIn("overflow", str(caught.exception))

    def test_hot_code_reaching_the_boot_tier_is_refused(self):
        with self.assertRaises(zmodpack.ZmodError) as caught:
            pack_service(self.tmp.name, "hotboot", {
                "h.rel": (
                    [("_CODE", 4)],
                    [("_svc_init", 0, 0), ("_boot_symbol", 0, None)],
                    [(0, 0, [0x00, 0x00, 0xC9, 0x00],
                      [(0x02, 4, "_boot_symbol")])]),
                "b.rel": ([("_BOOT", 2)], [("_boot_symbol", 0, 0)], []),
            }, {
                "init": "_svc_init",
                "segments": [
                    {"role": "hot", "image_areas": ["_CODE"]},
                    {"role": "boot", "image_areas": ["_BOOT"]},
                ],
            })
        self.assertIn("invisible", str(caught.exception))

    def test_bulk_data_carries_no_relocations(self):
        with self.assertRaises(zmodpack.ZmodError) as caught:
            pack_service(self.tmp.name, "bulkrel", {
                "h.rel": ([("_CODE", 2)], [("_svc_init", 0, 0)],
                          [(0, 0, [0xC9, 0x00], [])]),
                "bulk.rel": ([("_BULK", 4)], [("_bulk_data", 0, 0)],
                             [(0, 0, [0x00, 0x00, 0x00, 0x00],
                               [(0x02, 4, "_bulk_data")])]),
            }, {
                "init": "_svc_init",
                "segments": [
                    {"role": "hot", "image_areas": ["_CODE"]},
                    {"role": "bulk", "image_areas": ["_BULK"]},
                ],
            })
        self.assertIn("bulk", str(caught.exception))

    def test_a_bulk_segment_can_carry_a_binary_asset(self):
        """A segment may take its image from a binary asset (work package
        41: the V9958 font is bulk data): the file's bytes are the segment
        image, linked verbatim (they carry no records)."""
        payload = bytes((i * 7 + 3) & 0xFF for i in range(2048))
        with open(os.path.join(self.tmp.name, "font.bin"), "wb") as handle:
            handle.write(payload)
        container = pack_service(self.tmp.name, "asset", {
            "h.rel": ([("_CODE", 2)], [("_svc_init", 0, 0)],
                      [(0, 0, [0xC9, 0x00], [])]),
        }, {
            "init": "_svc_init",
            "segments": [
                {"role": "hot", "image_areas": ["_CODE"]},
                {"role": "bulk", "asset": "font.bin"},
            ],
        })
        info = zmodpack.parse_container(container)
        bulk = [s for s in info["segments"] if s["role"] == "bulk"][0]
        self.assertEqual(bulk["image_size"], 2048)
        self.assertEqual(bulk["bss_size"], 0)
        start = info["payload_offset"] + bulk["payload_offset"]
        self.assertEqual(container[start:start + 2048], payload)

    def test_an_asset_segment_takes_no_areas(self):
        with open(os.path.join(self.tmp.name, "font.bin"), "wb") as handle:
            handle.write(b"\x01\x02")
        with self.assertRaises(zmodpack.ZmodError) as caught:
            pack_service(self.tmp.name, "assetbad", {
                "h.rel": ([("_CODE", 2)], [("_svc_init", 0, 0)],
                          [(0, 0, [0xC9, 0x00], [])]),
            }, {
                "init": "_svc_init",
                "segments": [
                    {"role": "hot", "image_areas": ["_CODE"]},
                    {"role": "bulk", "image_areas": ["_BULK"],
                     "asset": "font.bin"},
                ],
            })
        self.assertIn("asset", str(caught.exception))


class TestServiceSet(unittest.TestCase):
    """Host assembly: the load order, the endpoint numbers, the resolved
    dependencies and the tier plan the packer and the loader share."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def image(self, name, export=None, depends=None, io_size=16,
              hot_bss=2, boot=False, bulk=0):
        """One synthetic image: a hot segment (init plus an optional
        dispatch entry), an optional io segment, boot and bulk data."""
        data = [0xC9, 0x00]
        symbols = [("_%s_init" % name, 0, 0)]
        chunks = [(0, 0, data, [])]
        areas = [("_CODE", len(data)), ("_HOT", max(hot_bss, 1))]
        segments = [{"role": "hot", "image_areas": ["_CODE"],
                     "bss_areas": ["_HOT"]}]
        endpoints = []
        objects = {"%s.rel" % name: (areas, symbols, chunks)}
        if export is not None:
            symbols.append(("_%s_dispatch" % name, 0, 0))
            endpoints.append({"class": export[0], "instance": export[1],
                              "version": 1,
                              "dispatch": "_%s_dispatch" % name,
                              "flags": 0})
        if io_size:
            segments.append({"role": "io", "image_areas": ["_IOBUF"],
                             "bss_areas": ["_SERVICE_IO"]})
            objects["%s_io.rel" % name] = (
                [("_IOBUF", 4), ("_SERVICE_IO", max(io_size - 4, 0))], [],
                [(0, 0, [0xAA] * 4, [])])
        if boot:
            segments.append({"role": "boot", "image_areas": ["_BOOT"],
                             "entry": "_%s_boot" % name})
            objects["%s_boot.rel" % name] = (
                [("_BOOT", 4)], [("_%s_boot" % name, 0, 0)], [])
        if bulk:
            segments.append({"role": "bulk", "image_areas": ["_BULK"]})
            objects["%s_bulk.rel" % name] = (
                [("_BULK", bulk)], [], [(0, 0, [0x22] * bulk, [])])
        manifest = {
            "init": "_%s_init" % name,
            "segments": segments,
            "endpoints": endpoints,
            "depends": list(depends or []),
        }
        return (name, objects, manifest)

    def pack(self, *images):
        return pack_set(self.tmp.name, list(images))

    def write(self, name, container):
        path = os.path.join(self.tmp.name, name)
        with open(path, "wb") as handle:
            handle.write(container)
        return path

    def services(self, paths):
        out = os.path.join(self.tmp.name, "services.man")
        report = os.path.join(self.tmp.name, "plan.json")
        zmodpack.cmd_services([out] + paths + ["--report", report])
        with open(out, "rb") as handle:
            manifest = handle.read()
        with open(report, encoding="utf-8") as handle:
            return manifest, json.load(handle)

    def size_of(self, container, role):
        info = zmodpack.parse_container(container)
        for segment in info["segments"]:
            if segment["role"] == role:
                return segment["image_size"] + segment["bss_size"]
        return None

    def test_manifest_names_the_images_in_load_order(self):
        containers, _ = self.pack(self.image("one", export=(10, 0)),
                                  self.image("two", export=(11, 0)))
        manifest, report = self.services([self.write("one.zmod", containers[0]),
                                          self.write("two.zmod", containers[1])])
        entries = [manifest[i:i + 24].split(b"\0")[0]
                   for i in range(0, len(manifest), 24)]
        self.assertEqual(entries, [b"one.zmod", b"two.zmod"])
        self.assertEqual([entry["name"] for entry in report["services"]],
                         ["one.zmod", "two.zmod"])

    def test_endpoint_numbers_are_host_assigned_in_load_order(self):
        containers, _ = self.pack(self.image("one", export=(10, 0)),
                                  self.image("two", export=(11, 0)))
        one = zmodpack.parse_container(containers[0])
        two = zmodpack.parse_container(containers[1])
        self.assertEqual(one["endpoints"][0]["number"], 1)
        self.assertEqual(two["endpoints"][0]["number"], 2)

    def test_a_required_dependency_resolves_to_the_exporter_number(self):
        containers, _ = self.pack(
            self.image("ioc", export=(5, 0)),
            self.image("hid", export=(4, 0),
                       depends=[{"class": 5, "instance": 0}]))
        hid = zmodpack.parse_container(containers[1])
        self.assertEqual(hid["dependencies"],
                         [{"class": 5, "instance": 0, "number": 1}])

    def test_an_optional_dependency_that_is_absent_resolves_to_zero(self):
        containers, _ = self.pack(
            self.image("sio", export=(1, 0)),
            self.image("console", export=(6, 0),
                       depends=[{"class": 4, "instance": 0, "optional": 1},
                                {"class": 1, "instance": 0, "optional": 1}]))
        console = zmodpack.parse_container(containers[1])
        self.assertEqual([dep["number"] for dep in console["dependencies"]],
                         [0, 1])

    def test_a_missing_required_dependency_is_refused(self):
        with self.assertRaises(zmodpack.ZmodError) as caught:
            self.pack(self.image("hid", export=(4, 0),
                                 depends=[{"class": 5, "instance": 0}]))
        self.assertIn("dependency 5.0", str(caught.exception))

    def test_a_dependency_on_a_later_image_is_refused(self):
        with self.assertRaises(zmodpack.ZmodError) as caught:
            self.pack(self.image("hid", export=(4, 0),
                                 depends=[{"class": 5, "instance": 0}]),
                      self.image("ioc", export=(5, 0)))
        self.assertIn("earlier image", str(caught.exception))

    def test_an_image_may_depend_on_its_own_endpoint(self):
        containers, _ = self.pack(
            self.image("echo", export=(16, 0),
                       depends=[{"class": 16, "instance": 0}]))
        echo = zmodpack.parse_container(containers[0])
        self.assertEqual(echo["dependencies"],
                         [{"class": 16, "instance": 0, "number": 1}])

    def test_a_duplicate_endpoint_is_refused(self):
        with self.assertRaises(zmodpack.ZmodError) as caught:
            self.pack(self.image("one", export=(10, 0)),
                      self.image("two", export=(10, 0)))
        self.assertIn("already exported", str(caught.exception))

    def test_a_duplicate_image_name_is_refused(self):
        with self.assertRaises(zmodpack.ZmodError) as caught:
            self.pack(self.image("one", export=(10, 0)),
                      self.image("one", export=(11, 0)))
        self.assertIn("duplicate image name", str(caught.exception))

    def test_an_oversized_hot_segment_is_refused(self):
        with self.assertRaises(zmodpack.ZmodError) as caught:
            self.pack(self.image("big", hot_bss=0x8000))
        self.assertIn("tier", str(caught.exception))

    def test_the_service_io_block_is_bounded(self):
        with self.assertRaises(zmodpack.ZmodError) as caught:
            self.pack(self.image("bigio", io_size=0x4001))
        self.assertIn("range", str(caught.exception))

    def test_the_plan_matches_the_embedded_bases_and_ledgers(self):
        containers, ledger = self.pack(
            self.image("one", export=(10, 0), io_size=32, boot=True),
            self.image("two", export=(11, 0), boot=True, bulk=64))
        rows = {}
        for name, container in zip(("one.zmod", "two.zmod"), containers):
            info = zmodpack.parse_container(container)
            rows[name] = dict((segment["role"], segment["base"])
                              for segment in info["segments"])
        hot_used = sum(self.size_of(c, "hot") + (self.size_of(c, "io") or 0)
                       for c in containers)
        boot_used = sum((self.size_of(c, "boot") or 0) +
                        (self.size_of(c, "bulk") or 0)
                        for c in containers)
        self.assertEqual(ledger["service_hot_bytes"], hot_used)
        self.assertEqual(ledger["service_boot_bytes"], boot_used)
        for name, container in zip(("one.zmod", "two.zmod"), containers):
            info = zmodpack.parse_container(container)
            for segment in info["segments"]:
                self.assertEqual(segment["base"], rows[name][segment["role"]])

    def test_the_services_command_accepts_the_linked_containers(self):
        containers, ledger = self.pack(
            self.image("ioc", export=(5, 0)),
            self.image("hid", export=(4, 0),
                       depends=[{"class": 5, "instance": 0}]))
        _manifest, report = self.services(
            [self.write("ioc.zmod", containers[0]),
             self.write("hid.zmod", containers[1])])
        self.assertEqual(report["nucleus"], ledger)
        self.assertEqual(report["services"][1]["dependencies"],
                         [{"class": 5, "instance": 0, "number": 1}])
        self.assertEqual(report["services"][1]["hot_base"],
                         zmodpack.parse_container(
                             containers[1])["segments"][0]["base"])

    def test_the_services_command_reports_the_driver_ledgers(self):
        containers, _ = self.pack(self.image("one", export=(10, 0)))
        import contextlib
        import io
        out = os.path.join(self.tmp.name, "services.man")
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            zmodpack.cmd_services([out,
                                   self.write("one.zmod", containers[0])])
        text = buffer.getvalue()
        self.assertIn("driver one.zmod: hot ", text)
        self.assertIn("driver hot: ", text)
        self.assertIn("driver boot/bulk: ", text)

    def test_descending_blocks_never_overlap(self):
        containers, _ = self.pack(
            self.image("one", export=(10, 0), bulk=64),
            self.image("two", export=(11, 0), bulk=64))
        spans = []
        for container in containers:
            info = zmodpack.parse_container(container)
            for segment in info["segments"]:
                if segment["role"] in ("bulk", "io"):
                    spans.append((segment["base"],
                                  segment["base"] + segment["image_size"] +
                                  segment["bss_size"]))
        spans.sort()
        for (_, end), (start, _) in zip(spans, spans[1:]):
            self.assertLessEqual(end, start)

    def test_boot_and_bulk_share_the_lower_tier(self):
        containers, _ = self.pack(
            self.image("one", export=(10, 0), boot=True, bulk=64))
        info = zmodpack.parse_container(containers[0])
        boot = [s for s in info["segments"] if s["role"] == "boot"][0]
        bulk = [s for s in info["segments"] if s["role"] == "bulk"][0]
        self.assertEqual(boot["base"], 0x0000)
        self.assertEqual(bulk["base"] + bulk["image_size"] +
                         bulk["bss_size"], 0x6000)
        self.assertLessEqual(boot["base"] + boot["image_size"] +
                             boot["bss_size"], bulk["base"])


if __name__ == "__main__":
    unittest.main()
