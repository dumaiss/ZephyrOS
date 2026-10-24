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


def decode_yjk(payload, width):
    """The V9958's YJK display of `payload`, as 5-bit (r, g, b): each
    byte's bits 7-3 are its pixel's Y; per four pixels, K is bits 2-0 of
    the first byte (low) and the second (high), J of the third and the
    fourth; R = Y + J, G = Y + K, B = (5Y - 2J - K) / 4, clamped."""
    def signed6(value):
        return value - 64 if value & 0x20 else value

    def clamp(value):
        return max(0, min(31, value))

    shown = []
    for run in range(0, len(payload), 4):
        quad = payload[run:run + 4]
        k = signed6((quad[0] & 7) | ((quad[1] & 7) << 3))
        j = signed6((quad[2] & 7) | ((quad[3] & 7) << 3))
        for byte in quad:
            y = byte >> 3
            shown.append((clamp(y + j), clamp(y + k),
                          clamp((5 * y - 2 * j - k) // 4)))
    return shown


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
                ("parrot6.img", 0, 16, 1024),
                ("parrot7.img", 1, 0, 2048),
                ("parroty.img", 2, 0, 2048)):
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

    def test_g7_payload_is_the_chip_grb_byte(self):
        with tempfile.TemporaryDirectory() as directory:
            header = self.convert(directory, "g7", (8, 4), (0xA0, 0xC0,
                                                            0xE0))
            self.assertEqual(header["magic"], b"KIMG")
            self.assertEqual(header["version"], 1)
            self.assertEqual(header["mode"], 1)
            self.assertEqual(header["palette_count"], 0)
            self.assertEqual((header["width"], header["height"]), (8, 4))
            self.assertEqual(header["payload_size"], 32)
            # GRAPHIC 7 is green 7-5, red 4-2, blue 1-0: G 6, R 5, B 3.
            self.assertEqual(header["payload"], bytes([0xD7]) * 32)

    def test_yjk_solid_colours_show_their_colour(self):
        """Each colour, encoded and decoded as the chip decodes it,
        is within one 5-bit step of the source."""
        for colour in ((255, 0, 0), (0, 255, 0), (0, 0, 255),
                       (128, 128, 128), (200, 120, 40), (0, 0, 0),
                       (255, 255, 255)):
            with tempfile.TemporaryDirectory() as directory:
                header = self.convert(directory, "yjk", (8, 2), colour)
                self.assertEqual(header["mode"], 2)
                self.assertEqual(header["palette_count"], 0)
                self.assertEqual(header["payload_size"], 16)
                want = tuple(value >> 3 for value in colour)
                for shown in decode_yjk(header["payload"], 8):
                    for got, source in zip(shown, want):
                        self.assertLessEqual(abs(got - source), 1,
                                             (colour, shown))

    def test_yjk_picture_round_trip(self):
        """A picture whose colour changes inside the four-pixel runs:
        the decoded image stays close to the source (the runs share J
        and K, each pixel keeps its own Y)."""
        width, height = 32, 8
        image = Image.new("RGB", (width, height))
        for y in range(height):
            for x in range(width):
                image.putpixel((x, y), ((x * 8) & 0xFF, (y * 32) & 0xFF,
                                        255 - x * 8))
        with tempfile.TemporaryDirectory() as directory:
            source = os.path.join(directory, "gradient.png")
            image.save(source)
            output = os.path.join(directory, "gradient.kimg")
            kimg.convert(source, output, "yjk", (width, height))
            header = parse_header(output)
        shown = decode_yjk(header["payload"], width)
        error = 0
        for index, pixel in enumerate(shown):
            source = image.getpixel((index % width, index // width))
            error += sum(abs(got - (value >> 3))
                         for got, value in zip(pixel, source))
        self.assertLess(error / (3 * len(shown)), 1.5)

    def test_yjk_width_is_a_multiple_of_four(self):
        with tempfile.TemporaryDirectory() as directory:
            source = synthetic_path(directory, 8, 8, (0, 0, 0))
            with self.assertRaises(SystemExit):
                kimg.convert(source, os.path.join(directory, "o.kimg"),
                             "yjk", (6, 4))

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
