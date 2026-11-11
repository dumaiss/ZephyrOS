# KIMG YJK quality implementation and acceptance

Recorded 2026-11-10. Host: Python 3.14.4, Pillow 12.1.1, Linux-7.0.0-42-generic-x86_64-with-glibc2.43.

## Implementation

- `tools/kimg.py`: selectable nearest/box/Lanczos resize, a same-size bypass,
  optional configurable unsharp masking and optimized YJK conversion. The
  default fast encoder and decoder equations are preserved. Prior raw `pack`
  work is preserved, with no change to its behavior in this quality revision.
- `tests/test_kimg.py`: default-output hashes, preprocessing, packing, exhaustive
  group-search oracle, signed J/K, determinism, lossless raw packing, and
  genuinely Pillow-free CLI checks using `python3 -S`.
- `tools/kimg_compare.py`: repeatable comparison using the existing parrot PNG
  and a generated gradient/lettering/line fixture; writes IMG files, decoded
  PNG previews, source hashes, error and time measurements.
- `docs/gfx-sdk.md` and `docs/sdk.md`: options, defaults, examples and trade-offs.
- This report and `kimg-quality-data/`: recorded measurements and previews.

There are no kernel, libGFX, PView, KIMG v1, mode, palette, target-side decoder
or golden-fixture changes. Nothing is committed by this work package.

## Algorithm and compatibility

The optimized RGB888 path rounds each component to RGB555 using integer
`(value*31+127)//255`. The legacy fast path retains `value >> 3`. For each
four-pixel group, per-pixel estimates yield an initial shared J/K. Search
within +/-3 of each shared estimate, clipped to -32..31; each candidate checks
all 32 Y values per pixel against the unchanged clamped decoder. A 32x32
squared-difference table, bounded 4,096-entry decoded-ramp and repeated-group
caches, and partial-error pruning reduce Python overhead. No NumPy dependency
is introduced. Ties prefer ascending J, K and Y; cached output is immutable.

For identical RGB555 input the candidate set includes the legacy J/K and all
of its possible Y values, so the optimized squared error cannot exceed fast.
The test suite checks this on primary colours, extremes and 80 seeded random
groups, plus three groups checked against an independent exhaustive scalar
oracle. The guarantee does not cover actual RGB888 paths with different source
quantizers. Both references are included in the measurement JSON to make that
difference visible.

The decoder remains `R=clamp(Y+J)`, `G=clamp(Y+K)`,
`B=clamp((5Y-2J-K)//4)`. The existing independent round-trip decoder tests retain
this floor behavior. Additional vectors and all 131,072 Y/J/K triples pin the
same equations, including negative pre-clamp blue values. No hardware rounding
change was made or physical V9958 validation claimed.

## Validation

```sh
PYTHONPATH=tests python3 -m unittest test_kimg test_source_layout \
    test_sdk_package.TestSdkSourceInputs -v
python3 -S -m unittest discover -s tests -p test_kimg.py -v
```

- **74 tests passed** across converter, source layout and SDK source inputs.
- Without Pillow/site-packages: **8 passed, 13 conversion tests skipped**.
- All three default-mode outputs match the pre-change byte captures on the
  deterministic 23x17 source, cropped/resized to 16x8. Pinned SHA256 captures
  are in the tests; G6's palette hash is pinned to Pillow 12.1.1, with an
  independent legacy preprocessing/packing comparison for other versions.
- Nearest/box/Lanczos all produce the requested geometry in G6/G7/YJK.
- Same-size and already-target-size crops bypass resize calls.
- Configured unsharp masking is checked after resize; default conversion does
  not invoke it. Tuning values do not enable it on their own.
- Native pixel payloads and optional G6 palettes pack unchanged. Wrong lengths,
  geometry, palette formats and convert-only flags on `pack` are rejected.
- Optimized output is deterministic after clearing both caches.
- No existing golden fixtures were regenerated or replaced.

Full SDK packaging and target/MAME tests were not run for this host-only change.

## Full-screen comparison

All outputs are **256x212**, with a **54,272-byte YJK payload** and a
**54,288-byte IMG** (16-byte KIMG v1 header). Timing is one cold-cache host wall
sample per encoder, from source PNG read through IMG write, including resize
and optional sharpening; fixture creation, scoring and preview generation are
outside the timed interval. These timings are machine- and workload-dependent.

Error is summed squared distance over three 0..31 RGB components per pixel,
against an identical preprocessed, nearest-rounded RGB555 reference for both
encoders. Comparisons within each profile isolate the encoder choice. SSEs
across different preprocessing profiles do not share a reference and should
not be interpreted as a ranking of filters.

| Preprocessing | Source | Fast SSE | Optimized SSE | Reduction | Fast seconds | Optimized seconds |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Box, no sharpening | parrot | 154,559 | 109,360 | 29.24% | 0.195 | 1.492 |
| Box, no sharpening | gradient-type | 873,353 | 824,042 | 5.65% | 0.212 | 1.189 |
| Lanczos + default sharpening | parrot | 146,435 | 106,206 | 27.47% | 0.196 | 3.121 |
| Lanczos + default sharpening | gradient-type | 3,234,951 | 3,058,530 | 5.45% | 0.209 | 1.685 |

Original source sizes: parrot 120x160; generated gradient/lettering 1280x1060
(exact 5:1 reduction to 256x212). The parrot is an existing palette-limited
photographic fixture and includes upscaling after the established center crop;
it is not a substitute for evaluating a larger illustration. The generated
fixture exercises downsampling, thin coloured lines, small type and smooth
colour ramps. PNG rendering may vary with Pillow's default font version; the
run records Pillow version and source RGB SHA256.

With the alternate legacy-truncated reference, optimized error on the parrot
is **higher**, whereas it is lower on the generated fixture. This illustrates
why the common-rounded-reference measurements are not advertised as a universal
RGB888 error guarantee. The JSON includes both SSEs and per-component MSE.

Recorded data: [box JSON](kimg-quality-data/box.json),
[Lanczos/sharpen JSON](kimg-quality-data/lanczos-sharpen.json),
[test log](kimg-quality-data/validation.txt),
[without-Pillow log](kimg-quality-data/without-pillow.txt).

Previews (box profile):

| Source | Reference after preprocessing | Fast decoded | Optimized decoded |
| --- | --- | --- | --- |
| Parrot | [reference](kimg-quality-data/box-parrot-reference.png) | [fast](kimg-quality-data/box-parrot-fast.png) | [optimized](kimg-quality-data/box-parrot-optimized.png) |
| Gradient/type | [reference](kimg-quality-data/box-gradient-type-reference.png) | [fast](kimg-quality-data/box-gradient-type-fast.png) | [optimized](kimg-quality-data/box-gradient-type-optimized.png) |

## Reproduce and use

From the repository checkout (the benchmark and input fixtures are development
files, not additional runtime/SDK dependencies):

```sh
python3 tools/kimg_compare.py
python3 tools/kimg_compare.py --resample lanczos --sharpen \
    --output build/kimg-quality-lanczos
python3 tools/kimg.py convert title.png title.img --mode yjk \
    --resample lanczos --sharpen --yjk-quality optimized
python3 tools/kimg.py pack title.yjk title.img --mode yjk
```

Generated sources, IMG outputs and decoded previews are under `build/`; existing
fixture inputs are read-only. Existing `convert title.png title.img --mode yjk`
still uses nearest resizing, no sharpening and fast YJK. `pack` and `info` need
only Python's standard library.

## Remaining limitations

- Four neighboring pixels share chroma, so saturated edges and thin type can
  still bleed horizontally. The +/-3 search is local, not a global optimum.
- Squared RGB555 error is not a perceptual metric; no gamma correction,
  dithering, cross-group optimization or YAE interpretation is added.
- Sharpening and Lanczos may introduce halos/ringing. Compare previews for the
  actual artwork; higher-quality encoding does not remove preprocessing damage.
- Host cache reuse affects performance. The bounded caches retain recent
  groups/ramps between calls; the benchmark clears them for each timed encode.
