# CSM language guide

CSM is a text language for writing a musical score independently of the sound
chip that will play it. A score describes musical roles, reusable patterns,
instruments, orchestration, and song form. A target block then maps that score
onto a particular machine.

The current compiler targets the four SN76489 chips on the Zephyr-80
Afternoon Blend card and writes a ZTR file directly:

```text
CSM source -> parser and semantic checks -> musical timeline
           -> voice allocation -> SN76489 backend -> ZTR
```

There are no JSON or Furnace intermediary files in this path.

## Getting started

From the `SNTracker` directory, compile a score with:

```sh
python3 tools/csmc.py songs/RunawayCircuit.csm
```

With no output argument, the compiler writes a `.ztr` file beside the source.
An explicit output path may be supplied:

```sh
python3 tools/csmc.py input.csm output.ztr
```

Useful options are:

- `--allocation-report` lists the physical voice selected for every layer.
- `--dump` decodes and prints the completed ZTR after compiling it.

Every build includes an encode/decode round-trip check. A command that reports
an error does not produce a usable song.

## A complete small score

This is a complete CSM file, not a fragment:

```csm
song FirstSong {
    tick_rate        = 60;
    ticks_per_row    = 5;
    rows_per_quarter = 6;
    tuning           = 440;
    meter            = meter(4, 4);

    roles = [
        role(melody, 4)
    ];
    play = main;

    pattern phrase = | c4 d4 e4 ~ |;

    section verse {
        melody = phrase;
    }

    instrument PlainSquare {
        priority = required;
        level    = 181;
        gate     = 25;
        envelope = adsr(0, 5, 180, 5);
    }

    orchestra Basic {
        layer melody.main {
            source     = melody;
            instrument = PlainSquare;
            route      = C;
        }
    }

    scene normal {
        enables = [melody.main];
    }

    form main {
        using    = Basic;
        sequence = [play(verse, normal)];
        loop     = 0;
    }
}

target Zephyr {
    backend   = sn76489x4;
    clock     = 3579545;
    transpose = 0;

    extension sn76489 {
        max_ztr_bytes = 20480;
    }

    routes {
        C = psg0;
    }

    realize PlainSquare {
        waveform = square;
    }
}
```

The declarations form a dependency chain:

```text
role -> pattern/section -> layer/instrument -> scene -> form -> target
```

`play = main` selects the form to compile. The form selects an orchestra and
pairs each section with a scene. The scene chooses which layers are audible,
and each layer turns one role into a realized instrument on an eligible route.

## Source notation

CSM is case-sensitive. Language words and note names are lowercase. Declared
names such as `PlainSquare` retain their case.

- `//` starts a comment that continues to the end of the line.
- Assignments end with `;`.
- Blocks use `{` and `}`.
- Values are integers, names, arrays, or calls such as `meter(4, 4)`.
- Numeric literals are decimal integers. Negative integers use a leading `-`.
- Names may be qualified with dots, for example `lead.echo`.
- Commas separate array entries, function arguments, and pattern sequences.
- A trailing comma is accepted in arrays and function calls.

CSM intentionally has no floating-point literals and no unit suffixes. The
property being assigned determines the unit.

## Song timing and roles

Every file contains exactly one named `song` block. The current compiler also
requires exactly one target block.

The song properties are:

| Property | Meaning |
| --- | --- |
| `tick_rate` | Playback ticks per second, 1–65535. |
| `ticks_per_row` | Playback ticks in one ZTR event row, 1–255. |
| `rows_per_quarter` | Event rows in one quarter note, 1–64. |
| `tuning` | A4 frequency in integer hertz, 1–2000. |
| `meter` | `meter(numerator, denominator)`. |
| `roles` | Array of `role(name, slots_per_bar)` calls. |
| `play` | Name of the form to compile. |

The tempo is implied by the tick grid:

```text
BPM = tick_rate * 60 / (ticks_per_row * rows_per_quarter)
```

For example, 60 ticks/second, 5 ticks/row, and 6 rows/quarter is 120 BPM. The
number of rows in a bar is:

```text
rows_per_bar = rows_per_quarter * numerator * 4 / denominator
```

That result must be an integer and must fit the ZTR pattern-length limit. Each
role declares its number of top-level slots per bar:

```csm
roles = [
    role(bass, 8),
    role(lead, 16),
    role(hat.left, 8)
];
```

Dots are ordinary parts of a qualified name; `hat.left` is one role, not a
field access.

## Patterns

A pattern is one or more bars of pitched events:

```csm
pattern bass.C = | c3 ~ ~ c3 ~ c3 eb3 ~ |;
```

The vertical bars delimit a musical bar. Each item between them occupies one
top-level slot. Every expanded bar used for a role must have exactly the slot
count declared by that role.

### Events

| Event | Meaning |
| --- | --- |
| `c4` | Start a pitched note. Sharps and flats use `c#4` and `db4`. |
| `c4@20` | Start a note with an explicit duration of 20 ticks. |
| `-d4` | Change the sounding pitch legato, without another attack. |
| `-d4@10` | Legato pitch segment with an explicit duration. |
| `c4^4^7` | Start a note and cycle root, +4, +7 semitones each tick. |
| `^2` / `^` | Set a root/+2 trill, or clear arpeggiation. |
| `<2` / `>2` / `=` | Crescendo, diminuendo, or stop the hairpin at its current level. |
| `~` | Require silence at this slot. |
| `_` | Tie: continue the current pitch without emitting an event. |
| `x` | Trigger an unpitched rhythm instrument. |

`~` does not mean “leave the channel unchanged.” It silences the layer at that
point and clears its persistent arpeggio and hairpin state. It is an error for
`~` to occur inside an explicitly timed note or legato segment.

`_` is only valid while the preceding note or legato segment is still active.
It emits no new note onset, so it does not advance round-robin allocation or
extend the segment's duration.

A note without `@duration` uses the layer's `gate`, or the instrument's
`gate` when the layer has no override. An explicit duration overrides both
gates. Durations are positive integer playback ticks.

#### Legato

`-pitch` is a slur to a different pitch. It is valid only while a preceding
note or legato segment is sounding. It changes pitch without restarting the
instrument envelope and without advancing round-robin distribution. A
legato chain therefore remains on one allocated physical voice.

Like an ordinary note, a legato segment uses its explicit `@duration` when
present and otherwise uses the layer or instrument gate. Release is measured
from the end of the final segment in the chain; starting a legato segment
cancels the release or note-off previously scheduled for the preceding
segment.

#### Arpeggios and trills

`^x^y` cycles the sounding pitch once per playback tick through the root,
`+x`, and `+y` semitones. `^x` is shorthand for `^x^x`, giving a root/offset
trill. Each offset is an integer from 0 through 15. A bare `^` clears the
arpeggio.

The modifier may be attached directly to a note or legato event, such as
`c5@12^4^7`, or occupy its own slot, such as `^2`. It changes pitch only: it
does not attack the note or advance round-robin distribution. The setting
persists across later onsets on the same layer until `^` or `~` clears it.

#### Hairpins

`<r` raises the output level by `r` units per playback tick and `>r` lowers
it. The rate is an integer from 1 through 15. `=` stops the change and holds
the level reached so far. A new `<r` or `>r` changes direction/rate without
discarding that accumulated level offset.

Hairpins are applied after the instrument envelope, so ADSR and table macros
continue normally. The result is clamped at silence and at the instrument's
level ceiling. A hairpin may be attached to a note or legato event, for
example `c5@12>2`, or placed in its own slot. It persists until another
hairpin command or `~`; a new note by itself does not clear it.

Suffix modifiers must touch the event they modify: `c5@12>2` is one event,
while `c5@12 >2` occupies two slots. Arpeggio and hairpin modifiers from the
two different families may be combined on one event. A standalone modifier
slot is valid only while a note is sounding.

The pattern-event grammar is:

```text
event     := note | legato | tie | rest | trigger | modifier+
note      := pitch [ "@" ticks ] modifier*
legato    := "-" pitch [ "@" ticks ] modifier*
tie       := "_"
rest      := "~"
trigger   := "x"
modifier  := "^" [ offset [ "^" offset ] ]
           | ("<" | ">") rate
           | "="
```

Here `ticks` is a positive integer and `offset` and `rate` are integers from
0–15 and 1–15 respectively. The `-` token is unambiguous in event position
because it must be followed by a pitch; negative integers remain available
in property values elsewhere in the language.

### Reuse, concatenation, and repetition

Patterns may reference other patterns. `*N` repeats the entire referenced
pattern, while commas concatenate pattern terms:

```csm
pattern empty = | ~ ~ ~ ~ ~ ~ ~ ~ |;
pattern riff  = | c3 ~ g3 ~ bb3 ~ g3 ~ |;

section intro {
    bass = empty*2, riff*4;
}
```

References may point forward in the file. Recursive pattern references are an
error.

### Subdivisions

Square brackets divide one top-level slot evenly:

```csm
pattern fast =
    | [c5 d5] [eb5 f5] [g5 f5] [eb5 d5]
      [c5 bb4] [g4 bb4] [c5 ~]  [d5 c5] |;
```

The bracketed events still count as one role slot. The row grid must divide
evenly by the role slots and by every subdivision. Subdivisions cannot nest.

ZTR v1 places note events on rows, so `gate`, `delay`, and explicit note
durations must be exact multiples of `ticks_per_row`. The compiler rejects
unrepresentable timing instead of rounding it.

## Sections and rhythms

A section supplies a pattern stream for each pitched role used in that part of
the song:

```csm
section chorus [key=Cmin, function=climax] {
    bass = bass.C*4;
    lead = lead.chorus, lead.answer;
}
```

All role streams present in a section must expand to the same number of bars.
A role omitted from a section is silent for that entire section. Tags in
brackets are preserved as descriptive metadata; the current backend does not
change playback from `key` or `function` tags.

A rhythm defines a one-bar unpitched source for a role of the same name:

```csm
rhythm kick  = | x ~ ~ x ~ ~ ~ ~ |;
rhythm snare = | ~ ~ x ~ ~ ~ x ~ |;
```

The current backend requires each rhythm to expand to exactly one bar. It
repeats that bar across the form whenever a layer sourced from the rhythm is
enabled by the active scene. Rhythm references may reference other rhythms;
pitched patterns and rhythms are separate namespaces.

## Instruments

An instrument describes musical behavior without selecting a physical voice:

```csm
instrument Lead {
    priority = required;
    level    = 143;
    gate     = 15;
    envelope = adsr(0, 3, 191, 6);
    effects  = [vibrato(32, 9)];
}
```

| Property | Meaning |
| --- | --- |
| `priority` | `required` or `optional`; used by voice allocation. |
| `level` | Linear peak amplitude, 0–255. |
| `gate` | Default note duration in playback ticks. |
| `envelope` | One of the envelope forms below. |
| `effects` | Optional array; currently supports one `vibrato`. |

Supported envelopes are:

```csm
envelope = adsr(attack_ticks, decay_ticks, sustain_level, release_ticks);
envelope = one_shot(attack_ticks, decay_ticks);
envelope = table([values], step_ticks, loop_index, release_index);
```

- ADSR attack, decay, and release are non-negative tick counts. Sustain is
  0–255.
- A one-shot attack is non-negative and its decay is positive.
- Table values are amplitudes from 0–255. `step_ticks` is 1–255. Loop and
  release indexes are zero-based; use `-1` when an index is absent.

The SN76489 backend combines `level` with the envelope using integer
arithmetic and maps it to the chip's 16 attenuation steps. Instrument macros
advance every playback tick even though note onsets are restricted to rows.

Vibrato is written as:

```csm
effects = [vibrato(period_ticks, depth)];
```

The period is 4–255 ticks. Depth is a non-negative magnitude in
1/128-semitone units; for example, `depth = 64` is half a semitone.

## Orchestras, layers, and groups

An orchestra turns roles into audible production layers:

```csm
orchestra Studio {
    layer lead.main {
        source     = lead;
        instrument = Lead;
        route      = C;
    }

    layer lead.echo {
        source     = lead;
        instrument = Echo;
        delay      = 20;
        route      = R;
    }

    group lead.full {
        members = [lead.main, lead.echo];
    }
}
```

A layer requires `source`, `instrument`, and either `route` or
`distribute`. Optional layer properties are:

| Property | Meaning |
| --- | --- |
| `transpose` | Musical transposition in semitones, -128 to 127. |
| `detune` | Pitch offset in signed 1/128-semitone units. |
| `delay` | Delay from the source event in playback ticks. |
| `gate` | Layer-specific default gate in playback ticks. |

Delays carry across section boundaries. A layer delay therefore behaves like a
real echo rather than being clipped when a new section begins.

`distribute = roundrobin([L, R])` creates one allocation unit for each named
route. Successive note onsets alternate through the units in declaration
order:

```csm
layer background.alternate {
    source     = background;
    instrument = Pad;
    distribute = roundrobin([L, R]);
}
```

Groups may contain layers or other groups. Group cycles are rejected.

## Scenes and forms

A scene enables layers or groups from an orchestra:

```csm
scene wide {
    enables = [bass.main, lead.full, percussion];
    transforms = [transpose(lead, 12)];
}
```

The supported scene transform is `transpose(target, semitones)`. Its target
may be a role, affecting every enabled layer derived from that role, or one
specific layer. Transpositions from the layer and scene are added once.

A form pairs sections with scenes and chooses the orchestra:

```csm
form main {
    using = Studio;
    sequence = [
        play(intro, sparse),
        play(verse, base),
        play(chorus, wide)
    ];
    loop = 0;
}
```

The current ZTR backend supports play-once forms only, so `loop` must be `0`.
After the final section, the compiler retains delayed events and finite
release tails, adds enough rows for them to finish, and then emits the native
song-stop effect.

## Targets and realizations

Musical declarations do not mention a sound chip. The separate target block
provides hardware-specific choices:

```csm
target AfternoonBlend {
    backend   = sn76489x4;
    clock     = 3579545;
    transpose = 24;

    routes {
        L = psg0;
        R = psg1;
        C = [psg2, psg3];
    }

    realize Lead {
        waveform = square;
    }

    realize Snare {
        waveform = noise;
        mode     = white;
        rate     = medium;
    }
}
```

The current target properties are:

| Property | Meaning |
| --- | --- |
| `backend` | Must be `sn76489x4`. |
| `clock` | Must be `3579545` for the current ZTR backend. |
| `transpose` | Playback-table compensation in semitones, -128 to 127. |

Target `transpose` is hardware compensation, not a compositional transpose.
Use `0` for music authored for the Afternoon Blend clock. The supplied
Runaway Circuit target uses `24` because its source pitch convention was two
octaves below the direct-clock hardware.

Every instrument used by the selected form needs a `realize` block. A square
realization only needs:

```csm
realize Bass {
    waveform = square;
}
```

A noise realization requires a mode and rate:

```csm
realize Hat {
    waveform = noise;
    mode     = white;
    rate     = high;
}
```

Noise `mode` is `white` or `periodic`. Rate is `low`, `medium`, `high`,
or `tone3`. A `tone3` noise voice shares its frequency source with Tone 2 on
the same PSG, which the allocator treats as a resource conflict while both
layers are active.

The optional `limits` block accepts positive integer target metadata:

```csm
limits {
    tonal_voices      = 12;
    noise_voices      = 4;
    attenuation_steps = 16;
}
```

These values document a target; the current SN76489 allocator derives its
actual resources from the `sn76489x4` backend and does not override them from
`limits`.

## Routes and voice allocation

A route names one eligible PSG or an ordered array of PSGs. The SN backend
recognizes `psg0` through `psg3`. Each PSG has three tone voices and one noise
voice.

The compiler expands every enabled layer over the entire form before assigning
voices. Allocation includes:

- note gates and explicit durations;
- complete legato chains as one indivisible voice lifetime;
- envelope release tails;
- layer delays across section boundaries;
- separate units created by round-robin distribution; and
- Tone 2 coupling for `tone3` noise.

Two layers may share a physical voice when their sounding rows do not overlap.
Required units are assigned first with deterministic backtracking. Optional
units are assigned afterward and are dropped if no compatible voice remains.
A required unit is never silently dropped. When ownership of a reused voice
changes, the compiler emits a reset if persistent notation state could
otherwise leak from the previous layer.

A `bind` block may force selected units to physical endpoints:

```csm
bind {
    lead.main       = psg2.tone0;
    bg.alternate[L] = psg0.tone1;
}
```

Endpoint names are `psgN.tone0`, `psgN.tone1`, `psgN.tone2`, and
`psgN.noise`. A binding must be allowed by the layer's route. A normal layer
is addressed by its layer name; a round-robin unit adds its route in brackets.
Conflicting hard bindings are errors.

Use the allocation report when designing routes or diagnosing a spill:

```sh
python3 tools/csmc.py --allocation-report song.csm song.ztr
```

## Backend extensions

An extension block reserves a namespace for properties owned by a backend.
This keeps the musical language independent of one chip while permitting
specific hardware behavior:

```csm
extension sn76489 {
    max_ztr_bytes = 20480;
}
```

At target scope, the SN backend supports:

| Property | Meaning |
| --- | --- |
| `max_ztr_bytes` | Reject output larger than this size, 1–65535 bytes. |

The Zephyr CP/M player has a 20 KiB buffer, so its target should not set this
above 20480.

Inside a noise realization, the backend also supports:

```csm
realize Kick {
    waveform = noise;
    mode     = periodic;
    rate     = low;

    extension sn76489 {
        phase_reset = 1;
    }
}
```

`phase_reset` is `0` or `1`. When enabled, a one-shot reset is emitted at
each note attack. It is only valid for noise instruments. Unknown extension
namespaces and properties are errors; another backend may define its own
namespace without changing patterns, sections, layers, scenes, or forms.

## Units and ranges

| Value | Unit or range |
| --- | --- |
| Timing fields, `gate`, `delay`, `note@duration`, `-note@duration` | Playback ticks |
| ADSR and one-shot time arguments | Playback ticks |
| `level`, sustain, envelope table values | Linear amplitude, 0–255 |
| Arpeggio offsets | Integer semitones, 0–15 |
| Hairpin rate | Output-level units per tick, 1–15 |
| Layer and scene `transpose` | Semitones |
| Layer `detune` | Signed 1/128 semitone |
| Vibrato depth | Non-negative 1/128-semitone magnitude |
| `tuning` | Integer A4 frequency in hertz |
| Route | One or more of `psg0`–`psg3` |

## Diagnostics and current restrictions

Diagnostics contain the file, line, column, severity, and a stable code:

```text
song.csm:42:17: error SEM029: scene 'wide' references undeclared layer or group 'lead.echo'
```

The front end checks duplicate declarations, missing references, cycles,
section lengths, role slot counts, target routes, and form relationships. The
backend then checks hardware ranges, timing alignment, realization details,
voice conflicts, instrument count, output size, and ZTR round-trip integrity.

The present backend intentionally has these limits:

- one target, `sn76489x4`, at 3.579545 MHz;
- play-once forms only;
- note events quantized to rows, without silent rounding;
- at most 16 realized instrument variants (layer detune can create variants);
- one-bar repeating rhythm sources;
- square tone and SN76489 noise realizations only; and
- no runtime voice stealing—the allocation is completed at compile time.

Portamento, retrigger commands, notation-level panning, speed changes, and an
additional effect-style vibrato are not part of the present notation. The
instrument `vibrato()` effect remains available as documented above.

For a larger working example, read
[`songs/RunawayCircuit.csm`](../songs/RunawayCircuit.csm) and compile it with
`--allocation-report`.
