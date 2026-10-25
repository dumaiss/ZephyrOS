# GFX shapes (rectangles, lines, points, ellipses; COPY and XOR)

## Result

The shape set of gfx-sdk.md §8.1 is in: `enum gfx_op`
(`GFX_OP_COPY`, `GFX_OP_XOR`) and the six calls in `kos/gfx.h`,
implemented in a new `gfx_shapes.c` member, drawn by the VDP's own
commands through the existing batch.  `gfx_fill_rect` gains the op
and uses HMMV for an aligned COPY and LMMV (the per-dot logical
fill) otherwise, so an odd x in the 16-colour mode is accepted;
`gfx_rect` is four LINE commands with each corner plotted exactly
once; `gfx_line` and `gfx_point` are one LINE or PSET with the
major/minor axes and DIX/DIY direction bits derived from the
endpoints (all eight octants, the horizontal and vertical cases);
`gfx_fill_ellipse` and `gfx_ellipse` run the midpoint ellipse on the
CPU and expand it into one horizontal span (LMMV) per scanline, the
outline as non-overlapping left/right spans so an XOR ellipse drawn
twice restores the background byte for byte.  A zero width/height is
a no-op, a zero radius draws the degenerate line or point; an out-
of-bounds shape (an ellipse's bounding box included) or an unknown
op is `-KOS_EINVAL` and queues nothing; in YJK only an aligned COPY
`gfx_fill_rect` is accepted, the rest is `-KOS_ENOTSUP`.  The
command pipeline's block builders (HMMV, HMMM, LINE, PSET, LMMV)
are all assembly (`v9958_io.asm`): one R#17 set, then the 15-byte
R#32-46 block through OUTI/OTIR.

`gfxtest shapes` draws the full set in G7 and G6 and blocks at three
keys for the replay to hold the screens; `gfxtest timing` marks five
targets.  `tests/test_gfx.py` replays both against the peer's VRAM,
the shapes against a Python midpoint reference and the peer's own
executed commands.

## Checkpoints

1. **The API and refusals.** The header matches §8.1; the replay
   pins the out-of-bounds and unknown-op `-KOS_EINVAL` refusals
   queueing nothing, the YJK `-KOS_ENOTSUP` refusals, and the zero
   sizes' silent no-ops, and every existing call site (gfxtest,
   GfxDemo, PView through `gfx_clear`) builds with the new
   `gfx_fill_rect` signature.
2. **Lines.** All eight octants plus horizontal and vertical, drawn
   by one LINE command each; the replay decodes every pixel from the
   VRAM and matches the Python mirror of the VDP's Maj/Min error
   stepping, endpoints included; the command blocks carry MAJ, DIX
   and DIY from the endpoints.
3. **Points and G6 nibbles.** Single points in both modes; at odd
   and even x in G6 the written nibble is the colour's low nibble
   and the neighbouring pixel is untouched.
4. **Rectangles.** `gfx_rect` plots the outline pixels exactly with
   the corners once; `gfx_fill_rect` runs the HMMV path (aligned
   COPY) and the LMMV path (odd x, and every XOR fill), with the
   command used checked.
5. **Ellipses.** Filled and outline at several radii including
   `rx != ry` and radius 0, in both modes; the pixel sets equal the
   reference midpoint ellipse, and the outline spans never overlap
   (the XOR-redraw restoration proves it).
6. **XOR.** Every shape drawn twice in XOR restores the background
   byte for byte.
7. **The command blocks.** The LINE builder fixes found on the way
   are pinned: ARG carries MAJ|DIX|DIY (the direction bits stashed
   in a spare block byte so restoring |dx| cannot clobber them),
   NX always holds the major length and NY the minor, and the real
   bit values are DIX=0x04, DIY=0x08 (openMSX's `VDPCmdEngine`).
8. **Peer model.** `V9958Peer` executes LINE (the Maj/Min error
   stepping, citing the V9958 Technical Data Book), PSET and LMMV
   with IMP and EOR, in G6 and G7; the docstring lists what is not
   modelled (the other logical ops, the real timing).
9. **Docs.** As listed; `git diff -U0` writing check prints nothing.

## Budgets

`gfxtest timing`, CPU T-states between the marker pairs, measured on
the peer's status-read timestamps (the harness from
`test_tui_sim.py`'s timing test):

| target | measured | budget |
|---|---|---|
| one aligned `gfx_fill_rect`, queue + flush | 308 | <= 1,200 |
| `gfx_line`, any octant | 216 | <= 1,500 |
| filled circle r=50 (one) | 131,166 | <= 150,000 |
| outline circle r=50 (one) | 196,351 | <= 250,000 |
| opaque 6×8 glyph in G6, one cell | 37,509 | record (package 95's baseline) |

No OS change: the OS image and the v9958 driver bytes are untouched
(the new code is libgfx user-side members plus the backend's
assembly builders, paid only by programs that link the shape entry
points).

## Validation

```sh
make && make sdk && make -C UserSoftware/GfxDemo test   # built OK
python3 -m unittest tests.test_gfx tests.test_sdk_package \
    tests.test_source_layout tests.test_sdk   # 21 + 66 + 12 tests, OK
git diff --check
```

Artifacts: `build/user/gfxtest.exe` (23,621 bytes),
`UserSoftware/GfxDemo/build/user/gfxdemo.exe` (12,423 bytes).

## Pins changed

- `tests/test_gfx.py`: `TestGfxShapes` (the span-derived G7/G6
  screens, the sequential key feeding, the XOR restoration) and
  `TestGfxTiming` (the five budgets) added; `TestGfxPrims` gains
  the G6 odd-x LMMV VRAM checks and the program's G6 clear.
- `tests/z80sim.py`: the peer executes LINE, PSET and LMMV IMP/EOR;
  LINE/PSET entries record the full colour byte.
- `tests/gfx/gfxtest.c`: `shapes` and `timing` modes; the prims G6
  clear.
- `tests/test_source_layout.py`: `gfx_shapes.c` in `OWNER_FILES`.
- No expected output changed elsewhere.

## Found outside scope

- `docs/gfx-sdk.md`'s §8.1 (the design this implements) predates
  the package on the tree; preserved.

## Pending

- Hardware: `gfxtest shapes` on the board - the owner checklist
  (rectangles, a fan of lines, filled and outline circles and
  ellipses, an XOR marker drawn and erased, then a key and the
  console returns).
