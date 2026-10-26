# ZTR v1 file format

ZTR is the native, little-endian song format used by Zephyr-80 SNTracker. It
stores tracker rows and instrument macros, not a pre-rendered stream of
SN76489 writes. All offsets are absolute from byte zero of the file. Version 1
files are limited to 65,535 bytes so every offset can be handled directly by a
Z80.

Multi-byte integers are unsigned unless explicitly described as signed.
Reserved bytes and bits must be zero. A reader must reject a file whose bounds,
counts, or reserved fields are invalid.

## Header

The header is exactly 64 bytes.

| Offset | Size | Field |
| ---: | ---: | --- |
| `00h` | 4 | ASCII signature `ZTR1` |
| `04h` | 1 | format version (`01h`) |
| `05h` | 1 | header size (`40h`) |
| `06h` | 1 | flags; zero in v1 |
| `07h` | 1 | logical channel count; always 16 |
| `08h` | 2 | playback tick rate in Hz |
| `0Ah` | 1 | tracker speed in ticks per row |
| `0Bh` | 1 | source channel count, 1 through 16 |
| `0Ch` | 2 | rows per pattern, 1 through 256 |
| `0Eh` | 2 | order count |
| `10h` | 2 | channel-pattern directory entry count |
| `12h` | 2 | instrument count, 0 through 16 |
| `14h` | 2 | tuning frequency for A4 in Hz |
| `16h` | 2 | reserved |
| `18h` | 4 | PSG clock in Hz; `00369E99h` (3,579,545) |
| `1Ch` | 2 | title string offset |
| `1Eh` | 2 | author string offset |
| `20h` | 2 | source-system string offset |
| `22h` | 2 | order-list offset |
| `24h` | 2 | instrument-directory offset |
| `26h` | 2 | macro-data offset |
| `28h` | 2 | pattern-directory offset |
| `2Ah` | 2 | pattern-data offset |
| `2Ch` | 2 | note-divisor-table offset |
| `2Eh` | 2 | exact file size |
| `30h` | 2 | string-pool size |
| `32h` | 1 | bytes per order entry (`10h`) |
| `33h` | 1 | instrument-directory entry size (`10h`) |
| `34h` | 1 | pattern-directory entry size (`08h`) |
| `35h` | 1 | note-table entry count (`60h`, 96) |
| `36h` | 1 | signed target playback transpose in semitones |
| `37h` | 9 | reserved |

The sections occur in this order and do not overlap:

```text
header, strings, orders, instrument directory, macro data,
pattern directory, pattern data, note divisor table
```

## Strings

Strings are NUL-terminated UTF-8. Offset zero within the pool is a single NUL,
so an absent string may point to the first pool byte. Instrument directory
entries contain absolute offsets into the same pool.

## Orders and fixed channel mapping

Each order is exactly 16 bytes, one pattern number per logical channel. `FFh`
means the channel is unused in that order. The logical-to-physical mapping is
fixed:

| Channels | Physical voices |
| --- | --- |
| `00h`-`03h` | SN0 Tone 0, Tone 1, Tone 2, Noise |
| `04h`-`07h` | SN1 Tone 0, Tone 1, Tone 2, Noise |
| `08h`-`0Bh` | SN2 Tone 0, Tone 1, Tone 2, Noise |
| `0Ch`-`0Fh` | SN3 Tone 0, Tone 1, Tone 2, Noise |

No dynamic voice allocation is implied by the format. A one-chip Furnace song
uses channels `00h`-`03h`; the remaining order bytes are `FFh`.

## Instruments

The instrument directory has one 16-byte entry per instrument:

| Offset | Size | Field |
| ---: | ---: | --- |
| `00h` | 1 | instrument number, `00h`-`0Fh` |
| `01h` | 1 | source instrument type (informational) |
| `02h` | 2 | instrument name string offset |
| `04h` | 2 | volume macro offset, or zero |
| `06h` | 2 | pitch macro offset, or zero |
| `08h` | 2 | arpeggio macro offset, or zero |
| `0Ah` | 2 | noise/duty macro offset, or zero |
| `0Ch` | 2 | phase-reset macro offset, or zero |
| `0Eh` | 1 | instrument flags; zero in v1 |
| `0Fh` | 1 | reserved |

Macro offsets point into the macro-data section. Each referenced macro begins
with this 12-byte header:

| Offset | Size | Field |
| ---: | ---: | --- |
| `00h` | 1 | type: 1 volume, 2 pitch, 3 arpeggio, 4 noise/duty, 5 phase reset |
| `01h` | 1 | mode: 0 sequence, 1 ADSR, 2 LFO |
| `02h` | 1 | flags: bit 0 requests active release |
| `03h` | 1 | step length in playback ticks |
| `04h` | 1 | initial delay in playback ticks |
| `05h` | 1 | reserved |
| `06h` | 2 | loop value index, or `FFFFh` |
| `08h` | 2 | release value index, or `FFFFh` |
| `0Ah` | 2 | value count |

Every value is three bytes: a value-flags byte followed by a signed 16-bit
value. Value flag bit 0 means a fixed note in an arpeggio macro; clear means a
relative semitone offset. Other value flags are reserved.

Sequence mode advances one value after `step length` ticks, after first waiting
`delay` ticks. At the end it loops to the loop index or holds the last value.
Before release it does not cross a release index. An active release jumps to
the release index when a `===` event arrives; passive release permits normal
progression to resume.

ADSR and LFO modes reserve the same value container for future native
generators. `fur2ztr.py` currently expands Furnace dev233 ADSR/LFO generators
into ordinary ZTR sequences. This translates Furnace's packed, normalized
generator slots on the host and keeps the v1 Z80 player deterministic.

Volume values use Furnace loudness notation: 0 is silent and 15 is loudest.
Pitch values are signed 1/128-semitone offsets. Noise/duty bit 0 selects white
noise when set and periodic noise when clear. On a noise channel, a fixed
arpeggio value supplies the two-bit SN noise rate. Rate 3 explicitly selects
Tone 2 as the noise clock, retaining the hardware coupling. A nonzero
phase-reset macro value causes the noise control byte to be rewritten; the
SN76489 has no equivalent tone phase-reset command.

## Sparse channel-patterns

Pattern identity is the pair `(logical channel, pattern number)`. Each
eight-byte directory entry is:

| Offset | Size | Field |
| ---: | ---: | --- |
| `00h` | 1 | logical channel, `00h`-`0Fh` |
| `01h` | 1 | pattern number |
| `02h` | 2 | absolute stream offset |
| `04h` | 2 | encoded stream length |
| `06h` | 2 | event count |

A stream contains only non-empty cells, sorted by row. No terminator is stored;
the directory length and event count must agree. Each event starts with:

```text
row:u8, presence_mask:u8, payload...
```

The row is absolute within the pattern. For a 256-row pattern it spans
`00h`-`FFh`. Payload fields occur in descending mask-bit order:

| Mask | Payload | Meaning |
| ---: | --- | --- |
| `80h` | `note:u8` | normal note; 0 is C-0, 95 is B-7 |
| `40h` | `instrument:u8` | instrument number |
| `20h` | `volume:u8` | volume 0-15 |
| `10h` | `effect:u8, parameter:u8` | native effect and parameter |
| `08h` | none | note OFF: silence and end macros |
| `04h` | none | note release (`===`): enter macro release stage |
| `02h` | none | legato note: update pitch without restarting macros; requires `80h` |
| `01h` | variable | persistent notation-modifier payload described below |

Normal note, OFF, and release are mutually exclusive. A legato event is a note
event and therefore cannot also be OFF or release. An empty pattern needs only
its directory entry; empty rows consume no bytes.

### Persistent notation modifiers

When mask bit `01h` is present, the payload begins with one flags byte. Any
additional values follow in the order shown here:

| Flag | Following payload | Meaning |
| ---: | --- | --- |
| `80h` | none | clear all persistent notation state before applying this event |
| `40h` | `offsets:u8` | set arpeggio; high/low nibbles are `x`/`y` semitone offsets |
| `20h` | none | clear arpeggio |
| `10h` | `rate:i8, ceiling:u8` | set hairpin; rate is -15..-1 or 1..15, ceiling is 0..15 |
| `08h` | none | stop the hairpin and retain its current accumulated level offset |

Flags `04h`-`01h` are reserved. Set and clear flags for the same modifier
family are mutually exclusive. Arpeggio offsets are unsigned nibbles. The
hairpin is evaluated once per playback tick after the instrument volume macro,
then clamped between silence and the supplied instrument ceiling.

Arpeggio and hairpin modes persist across rows and ordinary note onsets. Note
OFF clears both. The compiler uses the reset flag when an allocated physical
voice changes musical-layer ownership while persistent state remains active.

Native effects in v1 are:

| ID | Furnace import | Meaning |
| ---: | ---: | --- |
| 1 | `00xy` | three-step arpeggio: 0, +x, +y semitones |
| 2 | `0Axy` | volume slide (`x0` up, `0y` down) |
| 3 | `EAxx` | legato mode, zero off/nonzero on |
| 4 | `F9xx` | one-tick volume decrease |
| 5 | `FAxy` | fast volume slide (four times the `0Axy` rate) |
| 6 | `FFxx` | stop song |

The importer reports any other Furnace effect with order, row, and channel. It
does not put an undefined command in the ZTR stream; `--strict` turns such a
warning into conversion failure.

## Note divisor table

The file ends with 96 little-endian 16-bit SN76489 tone divisors indexed by the
source notes C-0 through B-7. The converter applies the signed target playback
transpose, then computes and clamps each entry to 1-1023 using the song's A4
tuning and:

```text
period = 3579545 / (32 * note_frequency)
```

This table preserves non-A440 songs without floating-point work on the Z80.
The supplied TI-99/4A Furnace export uses a divided PSG clock whose register
periods play two octaves higher on Afternoon Blend's direct 3.579545 MHz
SN76489AN. Its ZTR therefore records a target transpose of +24 semitones. Note
events keep their original Furnace names; the generated divisors carry the
hardware adaptation.
