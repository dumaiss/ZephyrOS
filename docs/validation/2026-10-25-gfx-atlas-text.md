# GFX fonts and the glyph atlas (4x6 text, transparent text)

## Result

Two text features, per gfx-sdk.md §12.2-14.1:

- **A second built-in font, 4x6 CP850.** The asset
  (`drivers/v9958/assets/font_cp850_4x6.inc`, imported byte for byte
  from the KonaOS tree with its SHA-256 pinned and verified at build)
  becomes a const table in its own archive member (`gfx_font4.c`),
  with the RAM copy made at the font's first use.  `struct
  gfx_text_style` gains `flags` and `font`; a zeroed style is still
  opaque 6x8, and `enum gfx_font` picks the 4x6 for 128 columns in
  the 16-colour mode.  The glyph calls take the font:
  `gfx_font_set_glyph(font, code, rows)` installs rows whose low bits
  are the pixels (six for the 6x8, four for the 4x6),
  `gfx_font_reset_glyph` restores one glyph and `gfx_font_reset`
  them all; `gfx_text_width(text, font)` measures a string.
- **Transparent text.** `gfx_text_atlas(font, y, lines, first,
  count)` hands libgfx an off-screen band (whole in one page's spare
  lines, 212-255 or 468-511, holding at least one atlas of the
  range) plus a font and a glyph range.  `GFX_TEXT_TRANSPARENT` in
  the style's flags draws each glyph as one batched LMMM with
  transparency (TIMP) from an atlas of its colour, so colour-0
  pixels leave the screen.  An atlas is built on its colour's first
  transparent use with the existing opaque emit (no second
  expansion), and when the band is full the least recently used
  colour's atlas is rebuilt in its place.  A glyph redefined since
  the atlases drew it is redrawn into every built atlas at its next
  use, alone.  Transparent text without an atlas (or from a
  different mode's setup) is `-KOS_EINVAL`; YJK `-KOS_ENOTSUP`;
  glyphs outside the range draw nothing and are counted by the
  return value.  No OS change: the v9958 driver and the kernel image
  are untouched.

Opaque text is now scanline-major assembly: the C side validates
once per call and sets one VRAM address per scanline of the string;
the assembly row loop (`gfx_v9958_io_text_row`) streams every
glyph's bytes - the 6x8's three-page pair tables, the 4x6's 32-byte
pair table (eight pairs cached round-robin), or one byte per pixel
in the 256-colour mode - for both fonts and both modes.  The same
loop serves the atlas builds one cell at a time.

The peer model grew to match: LMMM (code 0x90) executes dot by dot
under IMP/TIMP with the ARG's DIX/DIY directions, and TIMP skips
colour-0 source dots so the destination's background survives
(V9958 Technical Data Book, the logical move commands).  Not
modelled: the transfer's access-slot timing.

## Checkpoints

1. **The asset.** `fontpack.py table4x6` emits the padded const
   table (6 rows plus 2 pads, the same 8-byte stride as the 6x8, so
   the row loop addresses both fonts with one shift), and the
   build-time hash check pins the asset's SHA-256.
2. **Opaque 4x6.** `gfxtest atlas` draws 4x6 text in two pairs in
   both modes plus the right-edge clip: every glyph byte decodes
   against the pinned 4x6 asset, and a full 128-glyph row (512
   dots, every column) is pinned byte for byte.
3. **Transparency.** Over 8-dot stripes, nine colours of transparent
   4x6 text in G6 and four in G7 (plus the two LRU redraws): every
   glyph's foreground pixels carry its colour and every background
   pixel keeps the stripe byte, decoded dot by dot.  The LMMM
   records are pinned whole (SX/SY from the atlas cell, DIX on
   overlap, DIY for a target below the band); the DIY copy onto
   page 1's spare lines lands row-reversed and leaves the boot
   console's cursor-table bytes behind the colour-0 pixels.
4. **The atlas band.** Every cell of every occupied slot decodes
   against the RAM font in its slot's colour: the G6 band's seven
   slots hold colours 3-9 after colours 8 and 9 rebuilt the two
   least recently used slots; the G7 band's three slots hold 4, 1
   and 2 after the redraws rebuilt slots 1 and 2.  A redefined A
   (a full block) redraws exactly the A cells of all seven slots -
   the rest of the band is unchanged - then its reset restores
   them.
5. **Refusals and counts.** Transparent text without an atlas, a
   band over the visible lines, a band too short for one atlas and
   a range past the font are `-KOS_EINVAL`; the YJK setup and draw
   are `-KOS_ENOTSUP`; two of four out-of-range glyphs are counted
   and the other two drawn; a G6 atlas is refused after a mode
   switch.
6. **Restore.** The release puts the console back (the hold
   sequence, the restored registers, palette and cursor tables, the
   exclusive flag clear).

## Budgets

| figure | now | budget |
|---|---|---|
| a full 128-glyph 4x6 row, G6, the whole call | 27,595 instructions (216 per glyph) | 100,000 |
| an opaque 6x8 glyph, G6, cached pair (8-glyph string / 8) | 677 | 1,000 |
| an opaque 4x6 glyph, G7 (6-glyph string / 6) | 529 | 1,000 |
| a transparent glyph, CPU (4-glyph string / 4) | 930 G6, 886 G7 | 1,000 |
| a transparent glyph, VDP time (4x6 cell, LMMM) | 24 dots = 960 T at 3.58 MHz = 0.27 ms | report |
| one 95-glyph 4x6 atlas build, G6 | 196,600 instructions | report |
| a stale glyph's redraw (seven atlases, one cell each) | 18,017 instructions | report |
| OS bytes | unchanged | 0 |
| gfxtest.exe | 32,734 bytes | report |

The transparent glyph's measured CPU includes one engine wait
amortized over the string: a stream of glyphs costs less each.

## Bugs found and fixed

- The atlas build double-counted the glyph's font offset (the emit
  pre-added `code * stride` and the row loop adds it again), so
  every atlas cell held the wrong glyph.
- SDCC's codegen for `while (index >= atlas->per_row)` re-derived
  the field's offset from the register it had just loaded, walking
  the struct by nine bytes an iteration: the 4x6 atlas's second
  glyph row drew in the wrong place (the rebuilds only, where the
  division loop iterated twice).  The divisor is copied to a local
  first, and the other loops were audited for the same pattern.
- An atlas built under one mode was still accepted after a mode
  switch: the setup records the mode and the draw compares it.

## Validation

```sh
make && make sdk
python3 -m unittest tests.test_gfx tests.test_sdk_package \
    tests.test_source_layout
make clean && make check   # once, at the end
git diff --check
```

Artifacts: `build/user/gfxtest.exe` (32,734 bytes).

## Pins changed

- `tests/test_gfx.py`: `TestGfxAtlas` added (the two key stops, the
  transient draw states between them, the band and stripe decodes,
  the LMMM records, the budgets); `GfxHostTests` pins the new
  header and member surface (the fonts, the atlas, the row args).
- `tests/z80sim.py`: the LMMM execution (IMP/TIMP, DIX/DIY) and its
  record.
- `tests/gfx/gfxtest.c`: `atlas` mode.
- `docs/sdk.md`: the fonts and transparent-text paragraphs, the
  measured costs, the member rows.
- `docs/validation/2026-10-25-gfx-pages.md`: unchanged.
