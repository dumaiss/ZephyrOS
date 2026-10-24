#!/usr/bin/env python3
"""The KIMG v1 image format: the shipped header and the converter.

Host tests only: `user/sdk/include/kos/kimg.h` pins the format's
layout and constants; `tools/kimg.py` converts an image and the tests
parse the result byte-for-byte (magic, version, mode, geometry,
palette and the payload in each mode's packing).  The converter needs
Pillow, which is not a build dependency: its tests skip without it.
The committed fixtures `tests/gfx/fixtures/*.kimg` are parsed here
too, so a regeneration that drifts from the format fails the suite.
"""

import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from test_ioc import struct_member_order  # noqa: E402

import kimg  # noqa: E402

KIMG_H = os.path.join(ROOT, "user", "sdk", "include", "kos", "kimg.h")
FIXTURES = os.path.join(ROOT, "tests", "gfx", "fixtures")

try:
    from PIL import Image
    HAVE_PILLOW = True
except ImportError:  # pragma: no cover - the host simply lacks Pillow
    HAVE_PILLOW = False


def read_text(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def parse_header(path):
    with open(path, "rb") as handle:
        data = handle.read()
    magic, version, mode, flags, palette_count = struct.unpack(
        "<4sBBBB", data[:8])
    width, height, payload_size = struct.unpack("<HHI", data[8:16])
    return {"magic": magic, "version": version, "mode": mode,
            "flags": flags, "palette_count": palette_count,
            "width": width, "height": height,
            "payload_size": payload_size,
            "palette": data[16:16 + palette_count * 3],
            "payload": data[16 + palette_count * 3:]}


def synthetic_path(directory, width, height, pixels):
    """A solid-colour RGB source image, deterministic and tiny."""
    image = Image.new("RGB", (width, height), pixels)
    path = os.path.join(directory, "source.png")
    image.save(path)
    return path


class KimgHostTests(unittest.TestCase):
    """The shipped header: layout, constants, static asserts."""

    def test_header_layout_and_constants(self):
        text = read_text(KIMG_H)
        self.assertEqual(struct_member_order(text, "kimg_header"),
                         ["magic", "version", "mode", "flags",
                          "palette_count", "width", "height",
                          "payload_size"])
        self.assertIn('KIMG_MAGIC_0 \'K\'', text)
        self.assertIn('KIMG_MAGIC_3 \'G\'', text)
        self.assertIn("KIMG_VERSION    1u", text)
        self.assertIn("KIMG_HEADER_SIZE 16u", text)
        self.assertIn("sizeof(struct kimg_header) == "
                      "KIMG_HEADER_SIZE", text)
        self.assertIn("offsetof(struct kimg_header, payload_size) == 12",
                      text)

    def test_fixtures_match_the_format(self):
        for name, mode, palette_count, payload_size in (
                ("parrot-64x32-g6.kimg", 0, 16, 1024),
                ("parrot-64x32-g7.kimg", 1, 0, 2048),
                ("parrot-64x32-yjk.kimg", 2, 0, 2048)):
            header = parse_header(os.path.join(FIXTURES, name))
            self.assertEqual(header["magic"], b"KIMG", name)
            self.assertEqual(header["version"], 1, name)
            self.assertEqual(header["mode"], mode, name)
            self.assertEqual(header["flags"], 0, name)
            self.assertEqual(header["palette_count"], palette_count,
                             name)
            self.assertEqual((header["width"], header["height"]),
                             (64, 32), name)
            self.assertEqual(header["payload_size"], payload_size, name)
            self.assertEqual(len(header["payload"]), payload_size, name)
            self.assertEqual(len(header["palette"]),
                             palette_count * 3, name)
            # The g6 payload packs two 4-bit indices per byte.
            if mode == 0:
                for byte in header["payload"]:
                    self.assertLessEqual(byte >> 4, 15, name)
                    self.assertLessEqual(byte & 0x0F, 15, name)


@unittest.skipUnless(HAVE_PILLOW,
                     "Pillow is required (pip install Pillow)")
class KimgConverterTests(unittest.TestCase):
    """`tools/kimg.py`: convert then parse, byte for byte."""

    def convert(self, directory, mode, size, colour):
        source = synthetic_path(directory, 23, 17, colour)
        output = os.path.join(directory, "out-" + mode + ".kimg")
        kimg.convert(source, output, mode, size)
        return parse_header(output)

    def test_g7_rgb332_payload(self):
        with tempfile.TemporaryDirectory() as directory:
            header = self.convert(directory, "g7", (8, 4), (0xA0, 0xC0,
                                                            0xE0))
            self.assertEqual(header["magic"], b"KIMG")
            self.assertEqual(header["version"], 1)
            self.assertEqual(header["mode"], 1)
            self.assertEqual(header["palette_count"], 0)
            self.assertEqual((header["width"], header["height"]), (8, 4))
            self.assertEqual(header["payload_size"], 32)
            # One RGB332 byte per pixel: R 5, G 6, B 3 -> 0xBB.
            self.assertEqual(header["payload"],
                             bytes([0xBB]) * 32)

    def test_yjk_jk_payload(self):
        with tempfile.TemporaryDirectory() as directory:
            # r = 24 + 3, g = 24 + 2: J 3, K 2 -> 0x13.
            header = self.convert(directory, "yjk", (6, 3), (0x1B, 0x1A,
                                                              0x10))
            self.assertEqual(header["mode"], 2)
            self.assertEqual(header["palette_count"], 0)
            self.assertEqual(header["payload_size"], 18)
            self.assertEqual(header["payload"], bytes([0x13]) * 18)

    def test_g6_palette_and_packed_indices(self):
        with tempfile.TemporaryDirectory() as directory:
            header = self.convert(directory, "g6", (8, 4), (0x30, 0x50,
                                                            0x70))
            self.assertEqual(header["mode"], 0)
            # A solid colour quantizes to a single palette entry (the
            # format carries the count, at most 16 for G6).
            self.assertEqual(header["palette_count"], 1)
            self.assertEqual(len(header["palette"]), 3)
            self.assertEqual((header["width"], header["height"]), (8, 4))
            self.assertEqual(header["payload_size"], 16)
            self.assertEqual(tuple(header["palette"][0:3]),
                             (0x30, 0x50, 0x70))
            # Every packed nibble is that index: 0.
            self.assertEqual(header["payload"], bytes(16))

    def test_conversion_is_deterministic(self):
        with tempfile.TemporaryDirectory() as directory:
            source = synthetic_path(directory, 23, 17, (0x30, 0x50,
                                                        0x70))
            outputs = []
            for number in range(2):
                output = os.path.join(directory, "d%d.kimg" % number)
                kimg.convert(source, output, "g6", (16, 8))
                with open(output, "rb") as handle:
                    outputs.append(handle.read())
            self.assertEqual(outputs[0], outputs[1])

    def test_bad_geometry_refuses(self):
        with tempfile.TemporaryDirectory() as directory:
            source = synthetic_path(directory, 8, 8, (0, 0, 0))
            for size in ((0, 4), (4, 0), (5, 4)):  # g6 wants even x
                with self.assertRaises(SystemExit):
                    kimg.convert(source, "out.kimg", "g6", size)

    def test_info_subcommand(self):
        with tempfile.TemporaryDirectory() as directory:
            source = synthetic_path(directory, 23, 17, (0x30, 0x50,
                                                        0x70))
            output = os.path.join(directory, "info.kimg")
            kimg.convert(source, output, "g7", (8, 4))
            run = subprocess.run([sys.executable,
                                  os.path.join(ROOT, "tools", "kimg.py"),
                                  "info", output],
                                 capture_output=True, text=True,
                                 check=True)
            self.assertIn("KIMG v1 mode=g7 flags=0 palette=0 8x4 "
                          "payload=32", run.stdout)


if __name__ == "__main__":
    unittest.main()
