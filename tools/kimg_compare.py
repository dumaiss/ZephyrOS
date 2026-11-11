#!/usr/bin/env python3
"""Measure YJK fast/optimized encoding on fixed sources; never rewrite goldens.

Uses the existing parrot PNG and a deterministic gradient/lettering fixture.
The default target is a full 256x212 screen. Results include total conversion
wall time (PNG read through IMG write), SSE on a common rounded RGB555 source,
output size and decoded previews. Pillow is the only external dependency.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import struct
import time

import kimg

ROOT = Path(__file__).resolve().parents[1]


def comparison_sources():
    kimg.require_pillow()
    from PIL import ImageDraw
    parrot = kimg.Image.open(ROOT / 'tests/gfx/fixtures/Parrot_VGA_256_palette.png')
    parrot = parrot.convert('RGB')
    # Fivefold source resolution exercises integer-ratio box reduction.
    width, height = 1280, 1060
    chart = kimg.Image.new('RGB', (width, height))
    chart.putdata([(x * 255 // (width - 1), y * 255 // (height - 1),
                    (x + y) * 255 // (width + height - 2))
                   for y in range(height) for x in range(width)])
    draw = ImageDraw.Draw(chart)
    draw.rectangle((95, 95, 1185, 430), fill=(238, 230, 211))
    label = kimg.Image.new('RGB', (218, 67), (238, 230, 211))
    lettering = ImageDraw.Draw(label)
    lettering.text((6, 7), 'WEST OF HOUSE / YJK 0123456789', fill=(28, 29, 37))
    lettering.text((6, 28), 'Fine lettering / shared chroma', fill=(48, 81, 128))
    chart.paste(label.resize((1090, 335), kimg.Image.Resampling.NEAREST), (95, 95))
    for x in range(100, 1180, 20):
        draw.line((x, 500, x, 950), fill=(255, 70, 20), width=5)
    return {'parrot': parrot, 'gradient-type': chart}


def rgb_pixels(image):
    data = iter(image.tobytes())
    return zip(data, data, data)


def decode_payload(payload):
    pixels = []
    for offset in range(0, len(payload), 4):
        quad = payload[offset:offset + 4]
        k = (quad[0] & 7) | ((quad[1] & 7) << 3)
        j = (quad[2] & 7) | ((quad[3] & 7) << 3)
        k = k - 64 if k >= 32 else k
        j = j - 64 if j >= 32 else j
        pixels.extend(kimg.yjk_decode(byte >> 3, j, k) for byte in quad)
    return pixels


def compare(source, output, size=(256, 212), resample='box', sharpen=False):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    prepared = kimg._fit(source.convert('RGB'), size, resample)
    if sharpen:
        prepared = prepared.filter(kimg.ImageFilter.UnsharpMask(
            radius=0.8, percent=120, threshold=2))
    prepared.save(output / 'reference.png')
    expected = [tuple((c * 31 + 127) // 255 for c in pixel)
                for pixel in rgb_pixels(prepared)]
    legacy = [tuple(c >> 3 for c in pixel) for pixel in rgb_pixels(prepared)]
    source_path = output / 'source.png'
    source.save(source_path)
    results = {}
    for quality in ('fast', 'optimized'):
        kimg._yjk_decoded_ramp.cache_clear()
        kimg._yjk_optimized_group.cache_clear()
        path = output / (quality + '.img')
        start = time.perf_counter()
        kimg.convert(source_path, path, 'yjk', size, resample=resample,
                     sharpen=sharpen, yjk_quality=quality)
        elapsed = time.perf_counter() - start
        data = path.read_bytes()
        payload_size = struct.unpack_from('<I', data, 12)[0]
        if data[7] or payload_size != size[0] * size[1] or len(data) != 16 + payload_size:
            raise ValueError('Unexpected YJK packing')
        decoded = decode_payload(data[16:])
        sse = sum((a - b) ** 2 for p, q in zip(expected, decoded)
                  for a, b in zip(p, q))
        legacy_sse = sum((a - b) ** 2 for p, q in zip(legacy, decoded)
                         for a, b in zip(p, q))
        preview = kimg.Image.new('RGB', size)
        preview.putdata([tuple((c * 255 + 15) // 31 for c in p) for p in decoded])
        preview.save(output / (quality + '.png'))
        results[quality] = {'conversion_seconds': elapsed, 'file_bytes': len(data),
            'payload_bytes': payload_size, 'rgb555_rounded_sse': sse,
            'rgb555_rounded_mse_per_component': sse / (len(expected) * 3),
            'rgb555_truncated_sse': legacy_sse,
            'output_sha256': hashlib.sha256(data).hexdigest()}
    results['sse_reduction_percent'] = (100 * (1 - results['optimized']['rgb555_rounded_sse'] /
        results['fast']['rgb555_rounded_sse'])) if results['fast']['rgb555_rounded_sse'] else 0
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/kimg-quality')
    parser.add_argument('--resample', choices=('nearest', 'box', 'lanczos'), default='box')
    parser.add_argument('--sharpen', action='store_true')
    args = parser.parse_args()
    report = {'python': platform.python_version(), 'platform': platform.platform(),
        'pillow': kimg.Image.__version__ if kimg.Image else None,
        'size': [256, 212], 'resample': args.resample, 'sharpen': args.sharpen,
        'timing': 'One cold-cache wall-time sample per quality; includes source read, '
                  'crop/resize, optional sharpen, encoding and output write. No previews/scoring.',
        'error_reference': 'Identical preprocessed RGB888 scaled to RGB555 by nearest '
                           '(value*31+127)//255, also reporting legacy >>3 reference. '
                           'Measured comparisons, not a general RGB888 non-regression guarantee.',
        'fixtures': {}}
    for name, source in comparison_sources().items():
        report['fixtures'][name] = {'source_size': list(source.size),
            'source_rgb_sha256': hashlib.sha256(source.tobytes()).hexdigest(),
            'results': compare(source, args.output / name, resample=args.resample,
                               sharpen=args.sharpen)}
        print(name, json.dumps(report['fixtures'][name]['results']), flush=True)
    (args.output / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
