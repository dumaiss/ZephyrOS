# SN-Track UI

SN-Track is the KonaOS graphical tracker for ZTR v1 songs
(`docs/ZTR-Format.md`). It shows a song's metadata, orders, instruments
and pattern events on all 16 SN76489 voices, and follows playback with
reversible XOR highlights and two display pages. It is read-only: no
editing, no saving, no ZTR change.

**Rule:** SN-Track is a viewer of ZTR musical data, not an alternative
interpretation of it. Pattern cells show the fields an event explicitly
stores. Absent fields are blank, never inherited from earlier rows, and
never reconstructed from PSG output. The playback engine (libsnd) is
authoritative for execution; the document reader for the file's contents.

## 1. Display

| Property | Value |
|---|---|
| Mode | `GFX_MODE_512x212_16` (V9958 GRAPHIC 6), non-interlaced |
| Font | `GFX_FONT_4X6`, opaque text |
| Pages | 2: page 0 at y 0-211, page 1 at y 256-467 |
| Highlight | `gfx_fill_rect(..., 8, GFX_OP_XOR)` |

**Hardware rules the layout obeys:**
- **Text x is even.** GRAPHIC 6 packs two pixels per byte and text streams
  whole bytes, so every text x is even (`-KOS_EINVAL` otherwise). Every
  coordinate below where text starts is even. Fills and lines take any x.
- **Glyph spacing.** A 4×6 glyph draws in its left 3 columns; only `#`,
  `&` and `~` use the 4th. The last character of every cell (a hex digit,
  `=` or the middle dot) therefore leaves the cell's rightmost pixel column
  blank, and a 1-pixel rule may be drawn there after the text.
- **Base colours are 0-7.** Everything drawn uses palette entries 0-7, so
  one XOR with 8 highlights any rectangle and a second restores it.
- **Palette scale.** The table's levels are 3-bit; `struct gfx_rgb` is
  8-bit per channel and the backend keeps the top 3 bits, so pass
  `level * 255 / 7`.

Coordinates are logical pixels, inclusive.

### 1.1 Vertical regions

| Region | Y | Height |
|---|---|---:|
| Title bar | 0-8 | 9 |
| Panels | 9-43 | 35 |
| Channel headers | 44-54 | 11 |
| Pattern rows | 55-204 | 150 (25 × 6) |
| Status line | 205-211 | 7 |

### 1.2 Panels

| Panel | X | Width |
|---|---|---:|
| Orders | 0-215 | 216 |
| Instruments | 216-345 | 130 |
| Song | 346-511 | 166 |

## 2. Palette

Entries 8-15 are the XOR twins of 0-7. Levels are R G B, 0-7.

| Entry | RGB | Use | Twin | RGB |
|---|---|---|---|---|
| 0 | 0 0 1 | pattern ground | 8 | 2 1 4 |
| 1 | 1 1 2 | panels, beat rows | 9 | 3 2 5 |
| 2 | 2 2 3 | rules, empty cells | 10 | 4 3 6 |
| 3 | 4 4 5 | labels, muted | 11 | 5 5 6 |
| 4 | 6 6 6 | note, text | 12 | 7 7 7 |
| 5 | 5 5 7 | accent, instrument | 13 | 6 6 7 |
| 6 | 3 5 5 | volume, play | 14 | 5 7 6 |
| 7 | 6 5 3 | effect, mute | 15 | 7 6 4 |

## 3. Data sources

| Class | Meaning |
|---|---|
| ZTR | stored in the file |
| DERIVED | computed deterministically from the file |
| PLAYBACK | engine or transport state |
| UI | SN-Track settings, focus, selection |
| STATIC | fixed text |

## 4. Title bar (y 0-8)

Left to right: `SN-TRACK` (STATIC); the title (ZTR `1Ch`); `4 X SN76489`
(STATIC); the PSG clock as `3.58MHZ` (ZTR `18h`); the tick rate as `60HZ`
(ZTR `08h`); `STOP`/`PLAY` (PLAYBACK); the elapsed time `MM:SS`,
right-aligned (PLAYBACK).

**Elapsed time:** a 32-bit count of playback ticks actually processed,
`seconds = ticks / tick_rate`. It resets on a fresh start, holds while
paused or stopped, and continues across orders and loops. It never comes
from the order/row position or the display refresh.

## 5. Orders panel (x 0-215)

One position column and 16 channel columns, 3 characters (12 px) each,
text at x = 0, 12, 24, ... 192 (204 px):

```text
POS A1 A2 A3 AN B1 B2 B3 BN C1 C2 C3 CN D1 D2 D3 DN
```

Column `n` is logical channel `n - 1`, the order entry's byte `n - 1`.
Values are the stored pattern numbers in hex; an unused channel (`FFh`)
is blank. Never substitute the previous order's number.

Four orders are visible. The current order is highlighted (XOR). The
window holds while the current order stays inside it and scrolls to
include it when it leaves. Read-only.

## 6. Instruments panel (x 216-345, heading `INSTRUMENT`)

Four entries, text at x 218: `II NAME... T`. `II` is the directory's
instrument number in hex, the name its string (clipped, never over the
type), `T` the derived type:

| Type | Meaning |
|---|---|
| `T` | used only on tone channels (`(channel & 3) != 3`) |
| `N` | used only on noise channels |
| `M` | unused; the entry is drawn in colour 7 |

An instrument used on both kinds is invalid input: SN-Track refuses the
song with a message. Use means an explicit instrument field in any
pattern event of the song. Unused instruments stay listed. The list
scrolls to keep the selected instrument visible (UI state).

## 7. Song panel (x 346-511, heading `SONG`), text at x 348

| Label | Example | Source |
|---|---|---|
| `TITLE` | `NIGHT DRIVE` | ZTR `1Ch` |
| `SPEED` | `06` | ZTR `0Ah`, ticks per row, hex |
| `TEMPO` | `150` | DERIVED: `round(60 × rate / (4 × speed))`, decimal |
| `ROWS` | `40` | ZTR `0Ch`, hex (`100` for 256 rows) |
| `HILITE` | `04/10` | UI, hex: minor/major beat interval |
| `POS` | `03/79` | PLAYBACK order / ZTR `0Eh` count, hex |
| `PAT` | blank | reserved |

## 8. Pattern grid

### 8.1 Columns

Each chip group is 124 px: a 2 px gap, three 36 px tone cells, a 2 px gap
and a 12 px noise cell. Chip `k` starts at `O = 12 + 124k`.

| Column | Text x | Text width |
|---|---|---:|
| row number | 0 | 12 (2 hex digits, a space) |
| chip `k` tone 1 | O + 2 | 36 |
| chip `k` tone 2 | O + 38 | 36 |
| chip `k` tone 3 | O + 74 | 36 |
| chip `k` noise | O + 112 | 12 |

| Chip | O | T1 | T2 | T3 | N | ends |
|---|---:|---:|---:|---:|---:|---:|
| A | 12 | 14 | 50 | 86 | 124 | 135 |
| B | 136 | 138 | 174 | 210 | 248 | 259 |
| C | 260 | 262 | 298 | 334 | 372 | 383 |
| D | 384 | 386 | 422 | 458 | 496 | 507 |

x 508-511 is margin. Rules (colour 2, 1 px, full grid height):
- chip separators at x = O (12, 136, 260, 384) and 508;
- before each noise cell at O + 110;
- between tone cells at O + 37 and O + 73, inside the blank last glyph
  column of tone 1 and tone 2.

### 8.2 Channel headers (y 44-54)

`A1 A2 A3 AN`, `B1`... `DN`, at each cell's text x. An `M` in colour 7,
in the cell's last character slot (text x + 32 for tone, + 8 for noise),
marks a channel unused in the current order (`FFh`); later, also an
application-muted one (UI). No activity bars or meters.

### 8.3 Tone cell: `NNNIIVEEE` (9 characters, no spaces)

| Chars | Field | Shown |
|---|---|---|
| 0-2 | note (`80h`) | `C-4`, `D#3`; 0 is C-0, 95 B-7. OFF (`08h`) and release (`04h`) show `===`. Legato (`02h`) shows its note. |
| 3-4 | instrument (`40h`) | 2 hex digits |
| 5 | volume (`20h`) | 1 hex digit |
| 6-8 | effect (`10h`) | ID as 1 hex digit, parameter as 2: `01h,37h` is `137` |

Every character slot without a value shows CP850 `FAh`, the middle dot,
in colour 2: absent fields, empty rows and the cells of unused channels.
Rows past the pattern's end are blank. Notation modifiers (`01h`) are not
shown; the decoded event keeps them. No Furnace names, no mnemonics.

Colours: note 4, instrument 5, volume 6, effect 7, dots 2.

**Drawing:** a row is its row number, one dot run per chip (27 characters
at O + 2, 3 at O + 112), then the present fields over it.

### 8.4 Noise cell: `NIV` (3 characters)

| Char | Field |
|---|---|
| 0 | noise control `N = (white << 2) \| rate` |
| 1 | instrument, 1 hex digit |
| 2 | volume, 1 hex digit |

`N` follows the engine's onset rule (`render_noise`, `effective_note`):
- **rate** = the effective note & 3: the note plus the instrument's
  arpeggio macro value 0 (relative), or that value itself (fixed flag),
  or the note alone without an arpeggio macro;
- **white** = bit 0 of the instrument's noise/duty macro value 0, or 0
  without that macro;
- **blank** unless the event has both a note and an instrument and both
  macros (if present) have delay 0. The raw note is never shown as `N`.

Slots without a value show the middle dot. Effects and modifiers are not
shown; the decoded event keeps them.

### 8.5 Rows and pages

Row `i` (0-24) of the viewport is at `y = 55 + 6i`. The page holding
row `r` starts at `25 × (r / 25)`. Rows past the pattern's end are blank.
A new order starts at page 0. No scrolling, no recentring.

Row backgrounds (`HILITE`): colour 1 on rows that are a multiple of the
minor interval, colour 0 otherwise; the major interval's rows draw their
row number in colour 5. The play cursor is an XOR over a background.

**Page flipping.** The hidden page receives the next viewport while the
shown one plays; at the boundary `gfx_show_page` switches. Static regions
are identical on both pages: anything changed on the shown page is
repeated (or copied with `gfx_copy_rect`, even x) onto the hidden one
before it is shown. Playback never waits for drawing.

## 9. Highlights

The cursor row is one `gfx_fill_rect(0, y, 508, 6, 8, GFX_OP_XOR)`.
Moving it is two such fills: the old row, then the new one. The program
tracks, per page, which rectangles are XORed, and never overlaps two
highlights. The current order and the selected instrument use the same
mechanism inside their panels.

## 10. Status line (y 205-211)

```text
OCT 4  STEP 1  INS 01  ROW 0A/1F  FOCUS CHIP A  F1 HELP
```

`OCT`, `STEP`, `INS`, `FOCUS CHIP`: UI state, never written to ZTR.
`ROW`: the current row / `rows - 1`, hex. `F1 HELP`: STATIC. No `EDIT`
indicator.

## 11. Document access

`ztrdoc` is SN-Track's read-only document layer over a `kos_reader_t`:
- **open:** reads the header, strings, orders and instrument directory,
  checks bounds against `docs/ZTR-Format.md`, and computes instrument
  types (one pass over the pattern streams);
- **pattern:** `(channel, pattern number)` to its directory entry; the
  stream is read on demand;
- **events:** decodes a stream into per-row events keeping every field
  (note, instrument, volume, effect, OFF, release, legato, modifiers);
- **macros:** the arpeggio and noise/duty macros' first value and delay.

Renderers take decoded events, never raw bytes.

## 12. Refresh

| Item | Redrawn when |
|---|---|
| Title, song panel, headers | song load |
| Instrument list | load, scroll |
| Order rows | the current order leaves the window |
| Order highlight | the order changes |
| Pattern viewport | page or order change |
| Cursor | row change |
| Time | the shown second changes |
| `ROW` | row change |

The display catches up to the reported position; it never queues stale
positions.
