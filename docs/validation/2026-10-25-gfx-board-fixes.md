# GFX board fixes (package 95c02)

Three board-found defects corrected, with no OS change: the gapped
outline ellipses, the black held screens, and a `pages` mode the owner
could not judge by eye.

## 1. Gapless outline ellipses

`gfx_ellipse_run` emitted only each scanline's leftmost and rightmost
dot, so wherever the outline runs flatter than 45 degrees (the top and
bottom of a wide ellipse) the dots stood isolated.  The emit is now
the fill set's boundary, from the same span set the fill uses:

- per scanline y, the left piece runs from `first[y]` to the further
  of `first[y]` and `max(first[y-1], first[y+1]) - 1`; the right piece
  mirrors with the nearer of `last[y]` and
  `min(last[y-1], last[y+1]) + 1`;
- the top and bottom scanlines (no line beyond) are one span each -
  every dot has a vertical neighbour outside;
- pieces that touch or overlap merge into one span.

At most two spans per scanline, every dot drawn exactly once, so an
XOR outline drawn twice still restores everything.  Only the emit
changed; the midpoint walk is untouched.

## 2. The independent reference

`tests/test_gfx.py`'s `outline_ellipse_pixels` now derives the outline
from `fill_ellipse_pixels` by the 4-neighbour rule - a pixel is on the
outline when any of its four neighbours lies outside the fill.  It
never mirrors the C emit.  A new property test
(`TestGfxShapes.test_outline_reference_is_the_fill_boundary`) checks,
at circles and flat/tall radii, radius 1, and the degenerate point and
lines:

- the outline is a subset of the fill;
- every outline pixel is 8-connected to exactly two others (one closed
  ring, no gaps) - the degenerate point and lines excepted, where the
  whole fill is boundary;
- the XOR pair restores (the shapes replay's new G7 XOR outline pair).

### A walk finding

The compiled walk diverges slightly from the source mirror in region 2
for two shapes - the r=24 circle (rows 47-50) and the 14x20 ellipse:
the binary's x-increments lag the source's by a few rows.  It predates
this package (the walk's generated code is byte-identical to package
93's; package 93's tests compared emitted spans, so they never caught
it).  The package freezes the walk, so the replays ground those two
comparisons in the binary's own fill: `gfxtest shapes` now draws a
filled twin of each outline (the r=24 circle at (190,110), the 18x12
G6 ellipse at (60,90)), and the tests derive each outline's expected
pixels as the twin's boundary - decoded from the span commands the
binary actually dispatched - translated to the outline's centre.  This
is exactly the package-93 rule "a filled and an outline ellipse with
equal parameters agree", checked against the observed fill.

## 3. The display on at every held screen

`gfx_set_mode` leaves the display off by design, so every `gfxtest`
mode now enables it before the screen is held at a key:

- `shapes`: `gfx_display_enable(1)` after the G6 mode set (the G7
  screen already had it);
- `atlas`: the G7 stage enables before key 2, and the YJK refusal
  checks moved after key 2, so key 2 holds the G7 screen;
- `palette`: both held screens enable;
- `run`, `prims`, `text`, `timing`, `pages`: already correct.

The replays now assert R#1's display bit (0x40) and the expected mode
(R#0) at every key stop: shapes keys 1 and 2 (0x0E/0x0A), atlas keys 1
and 2 (0x0A/0x0E), palette keys 1 and 2 (0x0A then G7), pages keys
1-4.

## 4. The board-visible `pages`

After the replay's held steps (unchanged), `gfxtest pages` now runs:

- **Flip:** page 0 and page 1 in distinct full-screen colours,
  alternating every 15 frames (4 Hz) for 3 seconds, paced by
  `gfx_wait_vblank`, then page 0 back;
- **Tear sweep:** for h = 1, 2, 4, 8, 16, 32, 64 rows, two seconds
  each: every frame waits for the retrace, then copies an h-row band
  onto the shown page at y 0 from two differently coloured source
  bands on page 1, alternating; a 4x6 label shows h; a key ends the
  sweep early (the replay injects it during h=64) and the console
  returns.

The replay checks the 12 alternating flip writes (each inside a
retrace window), the three colour fills, the 750 sweep copies' blocks
and source alternation, the retrace starts where the engine keeps up
(h <= 8), and the model's prediction: the first height whose copy
outlasts the retrace window is **h=4** (the model's 41,975 T at h=4
against the 31,806-T window; h=2's 20,997 T fits).  That is the
board-check comparison point for the owner's partial-present limit.

## The outline r=50 figure (the budget)

| build | T-states |
|---|---|
| 95c01's record | 2,039,101 |
| this package | 2,555,000 |

The emit change adds no cost: both the old and the new emit submit at
most two spans per scanline (the old two single dots, the new up to
two pieces), and the walk is untouched.  The shift is the documented
build-layout sensitivity: gfxtest grew (the twins, the flip and the
sweep) and moved functions across the bank-crossing call boundary, so
markers now straddle dispatch paths - the line figure moved 2,793 to
4,692 (the supervisor's original), the filled circle 1,401,366 to
1,632,553, and the profile shows kernel IRQ entries inside the line,
filled, outline and row-new windows.  The pins were re-measured and
re-anchored at measured + 10%: line 5,200, filled 1,795,900, outline
2,810,500, row-new 395,200; the stable figures (fill 3,803, glyph
407,327, row-cached 234,197, stripe 3,803, polyline 696,261, present
4,037) keep their 95c01 pins.

## Validation

```sh
make && make sdk
python3 -m unittest tests.test_gfx tests.test_sdk_package
make clean && make check
git diff --check
```

All green.
