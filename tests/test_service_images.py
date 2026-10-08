#!/usr/bin/env python3
"""Host-side tests for the M10 resident service images (work package 40).

Pure tests build synthetic SDCC .rel text, pack service containers with
`tools/zmodpack.py` and check the format, the per-segment relocation
semantics, the load-order validation and the tier plan - including the
"missing dependency" and "oversized hot segment" cases the acceptance
image cannot cover alone.

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


def pack_service(tmp, name, objects, manifest):
    """Pack one service container from synthetic objects.

    objects: {file name: (areas, symbols, chunks)} for rel_text.
    """
    for file_name, (areas, symbols, chunks) in objects.items():
        text, _ = rel_text(file_name.replace(".rel", ""), areas, symbols,
                           chunks)
        with open(os.path.join(tmp, file_name), "w") as handle:
            handle.write(text)
    manifest = dict(manifest)
    manifest.setdefault("name", name)
    manifest.setdefault("type", "service")
    manifest.setdefault("abi", 1)
    manifest.setdefault("flags", 0)
    manifest["objects"] = list(objects)
    path = os.path.join(tmp, name + ".json")
    with open(path, "w") as handle:
        json.dump(manifest, handle)
    loaded = zmodpack.load_manifest(path, tmp)
    return zmodpack.build_container(zmodpack.pack_manifest(loaded))


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
            "depends": [{"class": 5, "instance": 0}],
        })
        info = zmodpack.parse_container(container)
        self.assertEqual(info["module_type"], "service")
        self.assertEqual(info["service_abi"], 1)
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
        self.assertEqual(boot["entry_offset"], 0)
        self.assertEqual(info["init_offset"], 0)
        endpoint = info["endpoints"][0]
        self.assertEqual((endpoint["class"], endpoint["instance"],
                          endpoint["version"]), (3, 0, 1))
        self.assertEqual(endpoint["dispatch_offset"], 4)
        self.assertEqual(info["dependencies"], [{"class": 5, "instance": 0}])

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


class TestServiceRelocations(unittest.TestCase):
    """Each segment relocates against its own base through the records."""

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

    def test_records_name_the_target_segment_and_patch_the_owning_word(self):
        info = zmodpack.parse_container(self.build())
        # The hot segment's records: one into the io segment (target 2,
        # the io-relative offset of `_buffer`) and one into its own BSS.
        self.assertEqual(info["relocs"][0:2], [(2, 0), (0, 2)])
        payload = info["payload"]
        self.assertEqual(payload[0] | (payload[1] << 8), 0)
        self.assertEqual(payload[2] | (payload[3] << 8), 6)
        # The boot segment's record points into the hot segment.
        self.assertEqual(info["relocs"][2], (0, 1))
        boot_at = info["segments"][1]["payload_offset"]
        self.assertEqual(payload[boot_at + 1] | (payload[boot_at + 2] << 8),
                         0)

    def test_loading_adds_the_target_segment_base(self):
        """The loader's rule, applied by hand: the io pointer lands in the
        io block, the BSS pointer in the hot segment's BSS, and the boot
        pointer in the hot image."""
        info = zmodpack.parse_container(self.build())
        bases = [0x7000, 0x0100, 0xBB00]  # hot, boot, io
        image = bytearray(info["payload"])
        for segment in info["segments"]:
            for record in range(segment["reloc_count"]):
                target, patch = info["relocs"][segment["reloc_start"] +
                                                record]
                at = segment["payload_offset"] + patch
                word = (image[at] | (image[at + 1] << 8)) + bases[target]
                self.assertLessEqual(word, 0xFFFF)
                image[at] = word & 0xFF
                image[at + 1] = (word >> 8) & 0xFF
        self.assertEqual(image[0] | (image[1] << 8), 0xBB00)
        self.assertEqual(image[2] | (image[3] << 8), 0x7006)
        boot_at = info["segments"][1]["payload_offset"]
        self.assertEqual(image[boot_at + 1] | (image[boot_at + 2] << 8),
                         0x7000)

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


class TestServiceSet(unittest.TestCase):
    """The `services` command: load order, dependencies and the tier plan
    the loader mirrors."""

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
            "depends": [{"class": dep[0], "instance": dep[1]}
                        for dep in (depends or [])],
        }
        return pack_service(self.tmp.name, name, objects, manifest)

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

    def sizes(self, container, role):
        info = zmodpack.parse_container(container)
        for segment in info["segments"]:
            if segment["role"] == role:
                return segment["image_size"] + segment["bss_size"]
        return 0

    def test_manifest_names_the_images_in_load_order(self):
        manifest, plan = self.services([
            self.write("ipc-a.zmod", self.image("a", export=(1, 0))),
            self.write("ipc-b.zmod",
                       self.image("b", export=(2, 0), depends=[(1, 0)]))])
        self.assertEqual(len(manifest), 48)
        self.assertEqual([manifest[index * 24:(index + 1) * 24].split(
            b"\0")[0] for index in range(2)],
            [b"ipc-a.zmod", b"ipc-b.zmod"])
        self.assertEqual(plan["order"], ["ipc-a.zmod", "ipc-b.zmod"])
        self.assertEqual(plan["format"], "zos-services/1")

    def test_a_missing_dependency_is_refused(self):
        """A dependency no earlier image exports is a boot error: the
        loader refuses the image and reports it; the packer refuses the
        set."""
        first = self.write("ipc-a.zmod", self.image("a", depends=[(5, 0)]))
        second = self.write("ipc-b.zmod", self.image("b", export=(5, 0)))
        with self.assertRaises(zmodpack.ZmodError) as caught:
            self.services([first, second])
        self.assertIn("dependency 5.0", str(caught.exception))
        # The other order is the valid one.
        manifest, plan = self.services([second, first])
        self.assertEqual(len(manifest), 48)
        self.assertEqual(plan["order"], ["ipc-b.zmod", "ipc-a.zmod"])

    def test_a_duplicate_endpoint_is_refused(self):
        with self.assertRaises(zmodpack.ZmodError) as caught:
            self.services([
                self.write("ipc-a.zmod", self.image("a", export=(1, 0))),
                self.write("ipc-b.zmod", self.image("b", export=(1, 0)))])
        self.assertIn("already exported", str(caught.exception))

    def test_an_oversized_hot_segment_is_refused(self):
        """The hot tier is 6000h-DFFFh: a segment that no longer fits
        fails the plan, exactly where the loader would refuse it."""
        big = self.write("ipc-big.zmod",
                         self.image("big", hot_bss=0x7000, io_size=16))
        with self.assertRaises(zmodpack.ZmodError) as caught:
            self.services([big])
        self.assertIn("hot segments need", str(caught.exception))

    def test_the_service_io_block_is_bounded(self):
        """io segments pack from C000h down inside 8000h-BFFFh."""
        first = self.write("ipc-a.zmod", self.image("a", io_size=0x1800))
        second = self.write("ipc-b.zmod", self.image("b", io_size=0x1800))
        _, plan = self.services([first, second])
        self.assertEqual([entry["io_base"] for entry in plan["services"]],
                         [IO_TIER[1] - 0x1800, IO_TIER[1] - 0x3000])
        # A third block would leave the 8000h-BFFFh range.
        third = self.write("ipc-c.zmod", self.image("c", io_size=0x1800))
        with self.assertRaises(zmodpack.ZmodError) as caught:
            self.services([first, second, third])
        self.assertIn("I/O", str(caught.exception))

    def test_the_plan_matches_the_loader_tier_policy(self):
        """io from C000h down, hot largest-first from 6000h with the io
        block skipped - the policy service_loader.c runs."""
        small = self.image("small", hot_bss=2, io_size=16)
        large = self.image("large", hot_bss=64, io_size=32)
        _, plan = self.services([self.write("ipc-small.zmod", small),
                                 self.write("ipc-large.zmod", large)])
        row = dict((entry["name"], entry) for entry in plan["services"])
        self.assertEqual(row["ipc-small.zmod"]["io_base"],
                         IO_TIER[1] - self.sizes(small, "io"))
        self.assertEqual(row["ipc-large.zmod"]["io_base"],
                         IO_TIER[1] - self.sizes(small, "io") -
                         self.sizes(large, "io"))
        # The largest hot segment takes the bottom of the tier.
        self.assertEqual(row["ipc-large.zmod"]["hot_base"], HOT_TIER[0])
        self.assertEqual(row["ipc-small.zmod"]["hot_base"],
                         HOT_TIER[0] + ((self.sizes(large, "hot") + 1) & ~1))
        self.assertLess(row["ipc-small.zmod"]["hot_base"],
                        row["ipc-large.zmod"]["io_base"])
        nucleus = plan["nucleus"]
        self.assertEqual(nucleus["service_hot_bytes"],
                         self.sizes(small, "hot") + self.sizes(small, "io") +
                         self.sizes(large, "hot") +
                         self.sizes(large, "io"))
        self.assertEqual(nucleus["service_boot_bytes"], 0)
        self.assertEqual(nucleus["service_hot_capacity_bytes"], 32768)
        self.assertEqual(nucleus["service_boot_capacity_bytes"], 24576)

    def test_descending_blocks_never_overlap(self):
        """io and bulk blocks are reserved from the cursor down, rounded
        *down* to an even address: a block of an odd size must not reach
        into the block above it (the loader would zero a neighbour's BSS)."""
        sizes = (0x205, 0x205, 0x7, 0x205)
        paths = [self.write("ipc-%d.zmod" % index,
                            self.image("s%d" % index, io_size=size,
                                       bulk=size, boot=True))
                 for index, size in enumerate(sizes)]
        _, plan = self.services(paths)
        for role, tier in (("io_base", IO_TIER), ("bulk_base", BOOT_TIER)):
            spans = []
            for entry in plan["services"]:
                base = entry[role]
                info = zmodpack.parse_service_container(
                    open(entry["container"], "rb").read())
                size = info["segments"][0]["image_size"]
                for segment in info["segments"]:
                    if segment["role"] == ("io" if role == "io_base"
                                           else "bulk"):
                        size = segment["image_size"] + segment["bss_size"]
                spans.append((base, base + size))
            spans.sort()
            for (low, high), (next_low, _next_high) in zip(spans, spans[1:]):
                self.assertLessEqual(high, next_low,
                                     "%s blocks overlap: %04X-%04X then "
                                     "%04X" % (role, low, high - 1, next_low))
                self.assertEqual(low & 1, 0, "unaligned base %04X" % low)

    def test_boot_and_bulk_share_the_lower_tier(self):
        """Boot segments bump up from 0000h, bulk data down from 6000h."""
        first = self.image("one", boot=True, bulk=8, io_size=0)
        second = self.image("two", boot=True, bulk=16, io_size=0)
        _, plan = self.services([self.write("ipc-one.zmod", first),
                                 self.write("ipc-two.zmod", second)])
        one, two = plan["services"]
        self.assertEqual(one["boot_base"], BOOT_TIER[0])
        self.assertEqual(two["boot_base"],
                         BOOT_TIER[0] + ((self.sizes(first, "boot") + 1) &
                                         ~1))
        self.assertEqual(one["bulk_base"],
                         BOOT_TIER[1] - self.sizes(first, "bulk"))
        self.assertEqual(two["bulk_base"],
                         one["bulk_base"] - self.sizes(second, "bulk"))
        nucleus = plan["nucleus"]
        self.assertEqual(nucleus["service_boot_bytes"],
                         self.sizes(first, "boot") + self.sizes(first, "bulk") +
                         self.sizes(second, "boot") +
                         self.sizes(second, "bulk"))
        # The two hot segments still count (their images plus BSS).
        self.assertEqual(nucleus["service_hot_bytes"],
                         self.sizes(first, "hot") + self.sizes(second, "hot"))


if __name__ == "__main__":
    unittest.main()
