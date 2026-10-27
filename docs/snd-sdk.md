# KonaOS Sound SDK Architecture — `libsnd` v1

**Status:** Implementation design (decided; implemented by work packages 97-103)  
**Target:** KonaOS / Zephyr-80 / Afternoon Blend sound card  
**Library:** `libsnd` (`LIBS=snd`, shipped in the SDK package)  
**Public header:** `kos/snd.h` (shipped)  
**Dependencies:** existing `kos/sound.h`, `kos/timer.h`, `kos/event.h`; existing KRES and `klibc` I/O through the shared `kos/reader.h` view  
**First client:** the current shell tracker's ZTR player, extracted into the SDK; then the native tracker and other programs

> **Sources of truth:** `docs/architecture.md`, `docs/sdk.md`, `docs/devices.md`, `docs/abi.md`, `docs/gfx-sdk.md`, `docs/kres-sdk-design.md`, `docs/CSM-Language.md` and `docs/ZTR-Format.md`. This document defines *new* `libsnd` policy and illustrative implementation APIs. The actual ZTR v1 binary format remains defined solely by `ZTR-Format.md`; do **not** silently modify that format or reinterpret it here. Review the existing shell-tracker player source before moving or rewriting any of its code.

---

## 1. Purpose

`libsnd` is a **small, userland musical playback SDK** above the existing KonaOS SOUND protocol. It has exactly three high-level uses:

1. **Play an existing ZTR song** from a file, buffered stream, KRES resource, or memory source. Extract and reuse the player already in the native SNTracker (`UserSoftware/SNTracker`, the C port of the assembly SNTracker). ZTR is the fully featured music representation.
2. **Play pre-rendered music**: a ZVG stream (ZVGC v1/v2, compiled from VGM by `Code/HOST/VGMPlayer/tools/compile_vgm.py`; the native `UserSoftware/VGMPlayer` is the reference decoder) - SN76489 register bytes and waits, streamed from the same sources as ZTR.
3. **Make simple music in C**: play notes, rests, short phrases, and a few basic instrument/envelope shapes. Reuse *note notation* inspired by CSM, **not** the CSM programming language.

The SDK does not compose, compile, edit, or serialize ZTR. It is not a C tracker, a CSM interpreter, a mixer, or a general music-language runtime. More sophisticated music is authored in the tracker or in CSM on the PC, produces ZTR, and is handed to `libsnd` for playback.

The sound card has four SN76489 PSGs (three tone and one noise voice each) and a DAC. ZTR v1 has **16 fixed logical channels**, mapped consecutively to those four chips. The SN76489 clock reported by the documented ZTR backend is **3,579,545 Hz**. ZTR includes its own 96-entry note-divisor table; playback must use that table instead of silently assuming A440.

**Architectural rule:** musical policy, ZTR parsing, buffers, and simple envelopes belong in userland.

**Rendering is assembly.** Sound is time-sensitive like graphics (gfx-sdk.md 2.6): everything that runs per tick - `snd_tick`, the voice-program, ZVG and ZTR engines, the register shadow and the port writes - is Z80 assembly. Setup is C: open/close, validation, parsing, compiling notes and phrases, the readers and the foreground prefetch (`snd_service`). The tick path never parses, compiles, allocates or reads a source. The bank-6 SOUND driver continues to own card access, lifecycle, and register-level transport; no additional nucleus or driver functionality is required.

### 1.1 Goals

- Preserve *existing shell-tracker playback behavior*, including instrument macros, note releases, effects, noise behavior, and stop semantics.
- Make a working ZTR player usable by arbitrary KonaOS programs without copying tracker code.
- Read **borrowed** sources via existing libraries: KRES logical resources, OS file handles, `klibc` buffered `FILE *`, and memory.
- Avoid another filesystem, stream implementation, per-resource file handle, or duplicate 512-byte read cache.
- Never perform filesystem/KRES I/O, allocation, parsing of source notation, or complex blocking work in the musical tick path.
- Support one exclusive card owner per active sound session; batch writes or use direct port I/O.
- Make a small manual note/instrument API usable by games and ordinary programs, without loading the ZTR decoder when unused.
- Allow the native tracker to reuse the **same authoritative ZTR player**, rather than maintaining a second implementation.
- Work in the existing single-tasking, banked-memory environment with bounded static or caller-provided storage.
- Supply host-side verification of decode and register-write traces before testing hardware.

### 1.2 Explicit non-goals (v1)

- On-target CSM parsing or compiling; CSM's pattern expansion, orchestration, timeline analysis, and voice allocation remain PC-side tools.
- Implementing the tracker/editor, ZTR writer, conversion from VGM/Furnace, or general song construction in C.
- Automatic SFX/voice stealing over a song; automatic song mixing or multi-song playback.
- Dynamic voice allocation for ZTR. Channels are specified by ZTR and map directly to hardware.
- PCM streaming, DAC samples, background audio daemon, kernel scheduler, or IRQ-rate DAC playback.
- A second generic file/stream API hidden within `libsnd`.
- A separate `libztr` package. All ZTR **playback** belongs inside `libsnd`; editor serialization stays application-owned unless separately justified.

---

## 2. Layers and ownership

```text
      C programs, games       Native tracker        Music player
            |                      |                     |
            | simple music         |  ZTR playback        | ZTR playback
            +--------------+-------+---------------------+
                           |
                libsnd (userland, optional)
                +--------------------------+
                | manual notes / envelopes |
                | ZTR player / macros      |
                | shared PSG shadow/output |
                | tick engine              |
                +-------------+------------+
                              |
                  common read-only data adapter     (foreground ONLY)
                        /         |        \
               KRES resource   FILE *     OS handle / memory
                        \         |        /
                      existing SDK storage facilities

                libsnd audio output / ownership
                              |
                  kos/sound.h (libkos)
                    +---------+----------+
                    |                    |
            sound_write(batch)     sound_out(port,value)
                    |              exclusive access only
                    |                    |
            /dev/sound driver      direct sound card I/O
                    |                    |
                    +---------+----------+
                              |
                     4 x SN76489 + DAC

     kos/timer.h + kos/event.h -> application's pacing -> snd_tick()
```

- **Application:** source lifetime, choosing a song, timer subscription, polling/foreground loop, buffer budgets, and UI.
- **`libsnd`:** ZTR validation/interpretation, per-channel musical state, basic note/envelope state, register shadow, direct/batched hardware writes, the ZTR order refill and the ZVG ring.
- **KRES / `klibc` / `libkos`:** source access, existing file buffering and physical I/O. A KRES resource is a **logical descriptor** and not automatically a `FILE *`.
- **SOUND driver:** `/dev/sound` publication, exclusive acquire/reset/release, `sound_info`, `sound_write`, card recovery.
- **Timer:** event notification and timestamps. There is no tempo clock in the SOUND protocol.

The userland libraries are optional static archives: a program that doesn't link `libsnd` pays no musical API cost, and manual-only programs should not pull in the ZTR reader/parser.

---

## 3. Existing interfaces: reuse, do not replace

### 3.1 SOUND interface (`kos/sound.h`)

Current SDK surface (existing, **not** new `libsnd` functions):

```c
int  sound_open(unsigned char exclusive);
int  sound_close(int handle);
int  sound_info(int handle, struct ksound_info *info);
int  sound_write(int handle, const struct ksound_write *writes,
                 unsigned int count);
int  sound_reset(int handle);
void sound_out(unsigned char port, unsigned char value);
```

`sound_info` reports the physical PSG/DAC ports, PSG clock, and capabilities. `struct ksound_write` consists of `{target, value}` pairs, with targets 0–3 for PSGs and 4 for the DAC; use the official `KSOUND_TARGET_*` constants. The driver batches a write sequence in one protocol call. The documentation quotes roughly **15,500 T-states per call** (not per byte). `sound_out` emits a chosen port value **without a per-byte syscall**; it is legal only under an exclusive card hold, with the ports returned by `sound_info`.

`libsnd` calls `sound_open(1)` for exclusive operation and does not assume the console or another application has already initialized the card. `sound_close` and process teardown reset/release it. Never hard-code hardware ports into the public SDK or make application authors issue PSG latch bytes.

### 3.2 Timing interface (existing)

```c
int notify(int handle, kos_event_handler_t handler);
int timer_event(int handle, uint32_t period_counts);
```

`kos_event_handler_t` receives `(count, missed)`. The timer unit is **25.6 µs** per count. Events are delivered on the program's stack, can coalesce, and may arrive between ordinary application instructions. The handler may call SDK routines but must remain bounded and return normally. There is no event queue or preemptive process scheduler.

The application owns the timer handle and `notify` subscription. In v1, `libsnd` **does not open a timer secretly**, define new timer protocols, or claim exclusive ownership of it. The ZTR tick rate comes from the song header. A helper may compute a timer period from the song's `tick_rate`, but registration remains under application control.

### 3.3 Existing file, buffered-stream and resource access

- `kos/fs.h`: `open/read/seek/close` on a KonaOS file handle.
- `klibc/stdio.h`: `FILE *`, `fread/fseek/ftell/fclose`; existing 512-byte stream buffering and `kos/stream.h` planning/measurement.
- `kos/kres.h`: `kres_get` gives a logical `kres_resource`; `kres_read/kres_seek/kres_rclose` use the resource's own cursor and share the container's **one** underlying handle.
- Memory: a caller-owned byte array, for short songs or prepared fixtures.

Do **not** convert a KRES descriptor to a faux OS handle or create a second set of 512-byte buffers merely for uniformity. Source data may be backed by a `.KPR` resource named `music/theme.ztr`, but that name is a flat KRES key; `/` does not create a directory.

### 3.4 One common reader adapter (shared SDK utility)

The current SDK documents these separate readers; the shared interface
they converge on is the reusable *read-at* view shipped in
`kos/reader.h` (package 99), alongside general SDK I/O, **not** inside
`libsnd`:

```c
typedef int (*kos_read_at_fn)(void *context, uint32_t offset,
                             void *dst, uint16_t requested);

typedef struct kos_reader {
    void *context;              /* borrowed backing object */
    kos_read_at_fn read_at;     /* count >= 0, or negative error */
    uint32_t size;              /* exact logical byte length */
} kos_reader_t;
```

`kos_reader_read_full(reader, offset, dst, length)` retries short reads
until the range is complete: it returns the length, a negative error,
or -KOS_EIO at an early end.  The handle and memory constructors take a
small caller-owned descriptor (`kos_handle_source_t`,
`kos_memory_source_t`); the stdio and KRES constructors take the
borrowed `FILE *` and `kres_resource *`.

Only the *three fields above* cross into `libsnd`. `read_at` implements absolute, resource-relative offsets from byte zero of the ZTR, and must return no more than `requested`; EOF returns zero. Short reads are legal and the consumer retries until the requested range is complete or detects EOF/error.

Provide **small separate adapter archive members** for:

| Existing source | Adapter behavior | Allocation / ownership |
|---|---|---|
| OS file handle | `seek` to absolute offset then `read` | borrowed handle; no new open |
| `FILE *` | `fseek` and `fread` | borrowed buffered stream; no extra cache |
| `kres_resource *` | `kres_seek` and `kres_read` | borrowed logical resource; no extra OS handle |
| RAM buffer | bounds-check and `memcpy` | borrowed memory |

Adapter constructors capture the source's size by its existing metadata/size mechanisms, without decoding ZTR. Each adapter's state, including a stable reference to the underlying handle/descriptor, remains alive as long as the song is open. No adapter closes the caller's source. Consumers must not destroy or replace a KRES resource during active playback. The application may seek its own source: every `read_at` is positioned absolutely.

**Avoid accidental dependencies:** including `kos/reader.h` must not force every program to link KRES, `klibc` streams, or their separate adapters. Do not impose an extra audio-only layer between source and existing SDK I/O. This helper can also serve future `libgfx` image loading.

If inspection of the implementation reveals an *existing* equivalent reader abstraction, adapt to that instead of adding this one. The contract is absolute reads and known length, not the proposed symbol names.

---

## 4. Public `libsnd` API (proposed)

The names below define an implementation target and can be refined together with the SDK headers. They are **not** claims that these symbols already exist. Use one process-local sound session (the same straightforward lifecycle model as `libgfx`) and **no heap allocation**.

### 4.1 Lifecycle and time

```c
/* kos/snd.h — proposed */
int  snd_open(void);                      /* exclusive SOUND acquire */
void snd_close(void);                     /* stop, silence, release */
int  snd_get_info(struct snd_info *out);  /* rates, chip layout, caps */
int  snd_set_output(uint8_t mode);        /* DIRECT or BATCH */
int  snd_set_rate(uint16_t ticks_per_sec);/* manual music default */
uint16_t snd_tick_rate(void);             /* active ZTR rate, else manual */
int  snd_timer_period(uint32_t *counts);  /* recommended event period */
int  snd_tick(uint16_t elapsed_ticks);   /* bounded musical advance */
int  snd_service(void);                   /* foreground refill */
```

- `snd_open` acquires once, calls `sound_info`, stores exact PSG ports/clock, and creates a clean muted shadow state. Failure leaves no acquired handle.
- Only one active ZTR and one process-local manual engine are supported; manual voices can coexist only on channels **not reserved** by that ZTR.
- `snd_set_output` selects `SND_OUTPUT_BATCH` (`sound_write`) or `SND_OUTPUT_DIRECT` (`sound_out` under exclusive hold). Direct mode is the preferred low-overhead target once validated. Both must render identical logical register traces.
- `snd_timer_period` rounds `39,062.5 / snd_tick_rate()` into timer counts and refuses unrepresentable periods. A fixed integer count may cause small long-term rate error; preserve the song's rate and document measured drift, rather than pretending the timer has sub-count resolution.
- `snd_tick(n)` never performs file I/O or heap operations and never compiles a phrase. It advances at most a fixed, documented catch-up budget; excess ticks are reported/recorded rather than creating an unbounded burst of writes.
- Never invoke `snd_service` from an event handler. It may call existing SDK storage functions and therefore belongs in the foreground.
- `snd_close` stops playback and releases hardware, but does not close any borrowed song source.

### 4.2 ZTR player

```c
int  snd_ztr_open(const kos_reader_t *source);  /* validate, initialize */
int  snd_ztr_prepare(void);                     /* load the first order */
int  snd_ztr_play(void);
int  snd_ztr_pause(void);
int  snd_ztr_resume(void);
int  snd_ztr_stop(void);
void snd_ztr_close(void);                       /* forget source only */
int  snd_ztr_get_status(struct snd_ztr_status *out);
```

The status reports state (`EMPTY`, `PREPARING`, `READY`, `PLAYING`, `PAUSED`, `DONE`, `UNDERRUN`, `ERROR`), current order, row, tick-in-row, header tick rate, reserved channel mask, last reader error, tick overrun count and order-underrun count. `snd_ztr_close` does **not** call `kres_rclose`, `fclose`, or `close`; the borrowed source remains the caller's property.

Loading a song is two distinct steps:

1. **Open/validate:** one forward pass over the file (section 6), keeping everything but the pattern data resident. Malformed source fails *before* playing registers.
2. **Prepare:** load order 0's pattern streams into an order buffer. `snd_ztr_play` requires it and never blocks on storage.

`pause` holds a documented muted state rather than leaving an uncontrolled note stuck; `resume` restores the current musical state from the engine shadow. `stop` silences ZTR-owned channels. The ZTR `stop song` effect (ID 6) terminates playback deterministically. Looping is **not** implied by ZTR v1; an application may explicitly replay a finished song.

There is no public ZTR encoder/editor API in `libsnd` v1. The tracker may continue to have private ZTR editing and serialization routines.

### 4.3 Simple/manual music

```c
/* Logical physical voice 0..15; 0-2,4-6,8-10,12-14 are tones;
 * 3,7,11,15 are the matching PSG noise voices. */

typedef uint8_t snd_voice_t;
typedef uint8_t snd_note_t;            /* 0=C-0 ... 95=B-7 */

typedef struct snd_instrument_simple {
    uint8_t kind;                     /* TONE or NOISE */
    uint8_t peak_level;               /* linear 0..255 */
    uint16_t gate_ticks;              /* default duration */
    uint8_t attack_ticks;
    uint8_t decay_ticks;
    uint8_t sustain_level;            /* 0..255 */
    uint8_t release_ticks;
    uint8_t noise_mode;               /* periodic/white */
    uint8_t noise_rate;               /* 0..3; 3 = tone2 */
} snd_instrument_simple_t;

int snd_define_instrument(uint8_t id,
                          const snd_instrument_simple_t *instrument);
int snd_note_on(snd_voice_t voice, snd_note_t note, uint8_t instrument);
int snd_note_for(snd_voice_t voice, snd_note_t note,
                 uint16_t duration_ticks, uint8_t instrument);
int snd_note_off(snd_voice_t voice);   /* begin instrument release */
int snd_voice_silence(snd_voice_t voice);
int snd_set_noise(snd_voice_t noise_voice, uint8_t mode, uint8_t rate);
```

- **Compiled, not interpreted.** `snd_note_on`/`snd_note_for`/`snd_note_off` compile, in C at the call, the note and its instrument's envelope into a **voice program**: a byte sequence of pre-computed register values and waits.  The opcodes are `PERIOD p` (3 bytes, the 10-bit divisor), `ATTEN level` (2 bytes, a linear 0-255 level the engine maps through the level table to the SN76489 attenuation), `NOISE control` (2 bytes, written with the forced-control rule), `WAIT8 n` / `WAIT16 n`, and `END`.  A **WAIT ends one logical voice tick**. A coalesced call consumes further waits, up to the 16-tick cap, but commits only the last state reached for each register. An envelope ramp becomes at most one `WAIT`/`ATTEN` pair per level change, so a program is bounded by the 16 hardware levels, not by its length in ticks; the worst program fits the documented 86-byte buffer. The assembly engine walks sixteen 16-byte descriptors in voice order, in one 256-byte-aligned page reached with `L` arithmetic (no IX/IY); the next page is a copy of the level table, so a level's attenuation is one indexed load. Each descriptor holds the flags (zero for a voice with nothing to do), cursor, wait, the current level, the attenuation desired/shadow/latch and port bytes in write order, and precomputed desired/shadow pointers for the period pair or noise byte, so no run/wait list or per-opcode voice-index arithmetic is needed. The walk records the earliest wait; ticks before it only accumulate. `snd_tick` advances each voice by at most `SND_TICK_CATCHUP_MAX` (16) ticks per call; excess counts in `snd_overruns` and is never replayed.
- Manual instruments are **basic**: level, gate, optional ADSR or one-shot-like release, tone/noise selection. No arbitrary macro engine, scene system, or orchestrator in this path.
- Limit the registry to a small fixed number of instruments (e.g. 8), controlled by compile-time configuration and documented RAM usage. `snd_define_instrument` copies one small description into local state.
- For manual pitches, use a **precomputed A440/3,579,545-Hz 96-entry divider table** (no runtime floating point). Check the actual `sound_info` clock against supported tables; reject unknown clocks unless the application supplies a correct table. ZTR always uses its *embedded* divider table.
- Distinguish user-facing linear level `0..255` from ZTR's macro volume `0..15` (0 silent, 15 loudest) and the SN76489 attenuation register (inverted). One output stage converts both paths to SN values, keeping ZTR's existing loudness mapping authoritative.
- A noise channel is not a pitched voice: manual `snd_note_on` targets tone voices only. `snd_set_noise` configures the appropriate noise voice; the SDK may provide a separate `snd_noise_trigger` with duration and instrument in a later incremental step.
- Noise rate 3 uses the corresponding tone2 period. Reject channel conflicts instead of silently altering an occupied tone2 voice.
- **No automatic voice allocation or stealing.** Calls naming a voice reserved by the active ZTR return a clear busy/conflict result. A ZTR reserves every channel referenced by its song orders (including currently silent orders) for the duration of playback.

### 4.4 Short manual phrases (optional but within v1 scope)

A tiny optional phrase helper makes short melodies possible without becoming CSM-in-C:

```c
/* Caller-owned event array; no heap. Names/signatures provisional. */
typedef struct snd_phrase_event {
    uint8_t  kind;                   /* NOTE or REST */
    uint8_t  note;                   /* C-0..B-7 index */
    uint16_t duration_ticks;
} snd_phrase_event_t;

int snd_phrase_parse(const char *text, uint16_t default_ticks,
                     snd_phrase_event_t *dst, uint16_t capacity,
                     uint16_t *written);
int snd_phrase_start(snd_voice_t voice, uint8_t instrument,
                     const snd_phrase_event_t *events, uint16_t count);
int snd_phrase_stop(snd_voice_t voice);
```

`snd_phrase_parse` produces the event array once; `snd_phrase_start` compiles it with the instrument into the voice program in a caller-owned buffer (`snd_phrase_compile` reports the size needed), so the tick engine plays a phrase exactly as it plays one note.

Accept only space-separated `pitch`, `pitch@ticks`, and `~` / `~@ticks`; optional `|` is a visually convenient separator with **no bar or meter semantics**. Pitch names are lowercase `c d e f g a b`, an optional `#` or `b`, and octave `0..7`, where `c4` indexes note 48. Example: `"| c4 d4 e4 ~ | g4@12 e4 c4 |"` (with explicit default ticks passed by caller). Strictly reject unknown tokens, duration overflow and out-of-range pitches. Parse **once** in the foreground into a caller-owned array; ticks consume only the prepared events.

Do **not** support CSM declarations, nested subdivisions, role slots, patterns, references, tie/legato syntax, modifiers (`^`, `<`, `>`, `=`), scenes, orchestras, forms, or arbitrary instrument macros here. If a song needs those, author it as CSM/ZTR.

Manual phrase parsing should link as an optional archive member; simple `snd_note_for` programs must not pay for it.

### 4.5 ZVG pre-rendered streams

```c
int  snd_zvg_open(const kos_reader_t *source, uint8_t chip); /* validate */
int  snd_zvg_prepare(void);                                  /* preload  */
int  snd_zvg_play(void);
int  snd_zvg_stop(void);
void snd_zvg_close(void);                     /* stop, then forget source */
int  snd_zvg_get_status(struct snd_zvg_status *out);
```

- The format is ZVGC v1/v2 exactly as `compile_vgm.py` writes it: a 16-byte header (`ZVGC`, version, a 1-180 Hz rate, the stream length and tick count), then `END`, `WRITE b`, `WAIT16 n`, short waits `40h..7Fh` and write runs `80h..8Fh`. The bytes are one SN76489's; `chip` (0-3) selects the PSG, and the song reserves that chip's four voices.
- The stream is prefetched by `snd_service` into a bounded 256-byte ring through the reader, with single-byte publication (section 6); the assembly decoder consumes complete commands only. A write run goes straight from the ring to the chip's port (loaded once per command tick), and the head and the 32-bit `consumed`/`writes` counters move once per command. The ring is 256-byte aligned, so a command that straddles its end wraps to its start with an 8-bit index. A wait command ends the tick (a wait reaching zero continues into the command loop in the same tick); `WAIT16` with a zero value is a malformed opcode.
- `snd_tick(n)` runs the decoder for each of the `n` elapsed ticks (bounded by the catch-up cap), so coalesced timer events never slow the song. `snd_zvg_play` sends any pending card rewrite first, so the stream's first tick is never silenced.
- An incomplete command at the ring's head is an underrun (section 6.3), never a partial write: the chip is muted (`9F BF DF`), the underrun is counted, and the next refill resumes exactly where the command started. If the reader has already delivered everything the header declared, the incomplete command is instead a truncated stream: an error state with the head opcode (or `FF` when the ring is empty) and its stream offset.
- A malformed opcode is an error state, muted, with the opcode and its stream offset in the status. `END` writes nothing and finishes playback (the `DONE` state); the stream's own final bytes mute the chip, and `snd_zvg_stop`/`close` invalidate the shadow and rewrite it muted regardless, releasing the reservation.
- `snd_tick_rate` is the file's rate while a ZVG is active. One ZVG, one ZTR or neither is active at a time; manual voices can use the chips a ZVG leaves free.
- A dual- or multi-chip ZVG is a later format version; v1/v2 refuse nothing about it because it cannot be expressed.

---

## 5. ZTR v1 playback contract

**Do not invent a revised ZTR format.** `ZTR-Format.md` remains normative. The following is the minimum parser/engine behavior to preserve during the tracker extraction.

### 5.1 Container geometry and tables

- Little-endian `ZTR1`, version 1; **64-byte header**; exact file size no greater than 65,535 bytes; offsets absolute from *ZTR byte zero*, not underlying KRES-file byte zero.
- 16 logical channel slots, with 1–16 source channels; fixed mapping `chip = voice / 4`, `subvoice = voice % 4`; indices 3,7,11,15 represent noise.
- Verify section ordering and bounds: header, strings, orders, instrument directory, macro data, pattern directory, pattern data, note-divisor table.
- 16 bytes per order, 16 bytes per instrument-directory entry, 8 bytes per channel-pattern-directory entry, 96 note-table entries of 16 bits.
- Header's tick rate, speed/ticks per row, pattern rows, order count, instrument count, PSG clock, tuning, and transpose metadata must pass the format's validation rules. Match the hardware clock reported by `sound_info` to the format's PSG clock requirement.
- Each order's 16 channel bytes selects a channel-pattern number; `FFh` means no pattern for that channel.
- A sparse pattern is keyed by `(logical channel, pattern number)`. Decode sorted events as `(row, presence-mask, typed payload)` with exact stream length and event count. An empty pattern consumes no event bytes.

### 5.2 Musical semantics

The existing player should remain the semantic oracle, with the format as the validation oracle:

- Note 0–95, instrument 0–15, explicit volume 0–15, OFF, release (`===`), legato pitch updates.
- Volume, pitch, arpeggio, noise/duty, and noise phase-reset macro sequences. Preserve delay/step, loop and release-index behavior; use signed 16-bit values where specified.
- ZTR's v1 ADSR/LFO macro *containers* are reserved for future generators; existing converters expand those into sequences. Do not silently interpret reserved macro modes as if they were supported.
- Persistent arpeggio/hairpin notation modifiers and their reset/clear semantics; note OFF clears persistent notation state.
- Defined native effects **1..6** only: arpeggio, volume slide, legato toggle, one-tick decrease, fast slide, and song stop.
- Note divisors are read from the embedded 96-entry table; do not retune an existing ZTR when loading.
- Noise rate 3 obtains its clock from Tone 2 of the *same* chip; preserve coupling and phase-reset behavior.

A record generated from CSM or Furnace is still a normal ZTR. `libsnd` does not know or need the authoring source format.

### 5.3 Validation and error behavior

- Reject unknown magic/version, size mismatch, nonzero reserved bits, bad section bounds/overlap, out-of-range counts, malformed macro payloads, incorrect stream lengths, non-monotonic pattern rows, invalid event combinations, dangling instrument/pattern references, and unsupported effects.
- No unchecked pointer/offset arithmetic; use 32-bit calculations for `offset + length` even though ZTR v1 offsets are 16-bit.
- Never partially initialize hardware based on unvalidated source data; a failure preserves the previous muted/stopped state.
- I/O errors retain a reproducible error code and do not become a bogus EOF or a spurious ZTR stop event.
- Existing valid ZTR fixtures must play with **byte-identical musical register traces** relative to the extracted shell player, after excluding redundant writes that do not affect sound if the optimization is explicitly enabled and independently tested.

---

## 6. Streaming / prefetch model

ZVG streams use a byte ring: raw stream bytes, consumed whole commands at a time (section 4.5).

ZTR is **not a raw PSG-register recording**: it has order lists, sparse channel-pattern streams, macro tables and note divisors. The pattern data streams; everything else is resident.

### 6.1 Two phases

**Open: one forward pass.** The header gives every section's offset and count, and the sections come in a fixed order without overlap, so `snd_ztr_open` validates in a single forward read through sequential windows, never seeking back:

- the header; the strings; the orders, the instrument directory, the macro data and the pattern directory, each kept resident as it is checked; the pattern streams, each checked (rows, masks, payloads, references, length and count); the divisor table, kept resident;
- references are checked against what is already resident, or when their target arrives: instrument numbers against the header's count, macro offsets against the instrument directory, stream offsets against the pattern directory, and each order's pattern numbers against the directory once it is read.

**Play: the order double buffer** (the ZVG model: the foreground copies raw bytes, the tick decodes). Rows never cross an order, so the unit of streaming is the order: its 16 channel-pattern streams, each read whole at the offset and length its directory entry gives.

```text
Normal/foreground application execution       Timer-delivered tick
---------------------------------------       --------------------
snd_service(): when the back buffer is free,   snd_tick(elapsed)
  read order n+1's streams into it, one         at each row's start, decode the
  read_at per stream, then publish it           row from the channel cursors
  (one byte)                                    into the current buffer
                                                (the reference decode_row)
                                              advance effects and macros
                                              render -> shadowed PSG writes
(NO card writes needed during the refill)     at the order's end: swap to the
                                                published buffer
                                              (NO I/O / KRES / malloc)
```

The tick decodes raw stream bytes exactly as the reference player does over its whole-file buffer (`player.asm`, `player.c`); nothing decodes ahead. `snd_service` does at most one order refill per call and otherwise returns at once; the caller repeats it from its main loop. The handshake is two bytes (the published order, the consumed order), each written after the data it covers; the tick never follows a partially written buffer.

### 6.2 Work-space and memory policy

- **Resident:** the header fields, the orders, the instrument directory, the macro data, the pattern directory (compacted to 6 bytes per entry: channel, number, offset, length) and the 96 divisors, in library storage sized by compile-time capacities (`SND_ZTR_*`); exceeding one refuses the song at open (`-KOS_ENOSPC`).
- **Order buffers:** two, each `SND_ZTR_ORDER_BYTES` (1,024: a 64-byte table of per-channel start/end cursors, then the raw streams); the validation pass records the largest order's total stream bytes, and a larger order refuses the song at open.
- The capacities: 136 orders × 16 selections, 400 pattern entries × 6 bytes, 6,640 macro bytes; night-market's largest order is 871 stream bytes, RunawayCircuit's 370. The pattern data never sits in RAM whole.
- Data the tick reads is directly visible during the tick: no far-mapped pointers retained across callbacks.

### 6.3 Underrun behavior

An underrun can only occur at an order change: the next order is not yet published. The refill has the whole current order's duration (rows × speed ticks, seconds of music) to complete.

- The tick never blocks for I/O and runs only while `PLAYING`. At the order change it mutes the ZTR's voices, enters `UNDERRUN`, counts it, and holds; `snd_service` resumes playback at that order's first row once the buffer is published.
- A reader error during a refill is kept as the last error; playback stays in `UNDERRUN` (never a bogus song end).
- Test a reader stalled across an order change and a reader that fails.

### 6.4 Interaction with the timer/event system

At about 50–60 Hz, the documented 15,500 T-state batched SOUND call alone is material (around 1.55 ms at 10 MHz). Device delivery and musical processing add further cost. Implement both batch and exclusive direct output and **measure end-to-end ticks**; do not predeclare one choice always faster for every workload.

When an event callback receives `(count, missed)`:

- `count` is coalesced elapsed notifications, not an instruction to emit `count` bursts of outdated register writes.
- Musical state may advance up to a **bounded** number of elapsed ticks, with a single final output flush. Define what happens if the bound is exceeded (`overrun` and a safe pause/mute); never loop forever.
- `missed` is reported distinctly from coalesced count. Parent/child `run()` suspends the parent: audio is **not** guaranteed to continue while another process runs. No OS scheduler or audio service is part of v1.
- Document required program stack reserve for event delivery plus the deepest `snd_tick` stack usage. Prefer direct port writes for the most time-sensitive route.

The external app may alternatively call `snd_tick()` from a synchronous loop for testing. A test must be able to feed exact tick counts to the engine with no timer or physical card.

---

## 7. PSG output and voice ownership

### 7.1 Shared output engine

All musical sources produce desired *hardware state*, not unstructured writes:

```text
ZTR channel state ----+
                      +-- per-chip desired state / register shadow
Manual voice state ---+                |
                                      +-- emit changed latch/data writes
                                      +-- batch via sound_write OR direct sound_out
```

The shadow must correctly model SN76489's latch/data write protocol, including two-byte tone divisor updates, 4-bit attenuation, and noise control/phase reset (where the control write must be forced even when a prior value matches). Manual voices compare and update the shadow as they run and emit directly (or append to the bounded BATCH array); there is no changed-slot queue or per-tick flush pass. The ZTR engine takes the DIRECT/valid test once per tick and compares/writes period, noise and attenuation inline. BATCH or an invalid shadow uses the write-through primitives (`snd_put_period`, `snd_put_atten`, `snd_put_noise`; HL = the descriptor). An invalid shadow is restored by one card-order walk over all 44 register bytes; DIRECT copies the desired state into the shadow in one block and writes each chip's eleven bytes to its port. On acquire/reset, invalidate/reinitialize the shadow so the first frame does not assume hardware state inherited from a prior process.

The DAC exists on the card and in `kos/sound.h`, but **sample playback is outside v1**; don't introduce an 8-kHz event consumer or reserve DAC work-space in the player.

### 7.2 Ownership and conflicts

- While ZTR playback is active, its logical channel mask is reserved exclusively for ZTR. Compute it from the song's referenced order/channel entries; do not assume all 16 are always in use.
- A manual note may use an unreserved tone voice; a manual noise effect may use an unreserved noise voice. A conflicting attempt returns busy without perturbing the song.
- The four noise voices each share their respective chip's tone2 period when noise rate=3. Treat that coupling as a real ownership conflict.
- `snd_ztr_stop` releases ZTR's logical channel claims and silences its outputs. `snd_close` silences **all** channels and releases `/dev/sound`.
- No automatic priority arbitration or stealing in v1; a later optional SFX layer can be added without changing ZTR interpretation.

### 7.3 Output ordering and failure

Manual writes are ordered by logical voice 0 through 15. Within one voice, its period pair precedes noise control and attenuation. DIRECT and BATCH retain exactly that byte order; a coalesced tick emits only the last state reached by each voice register. The invalid-shadow rewrite alone uses card order (chip 0 through 3; tones, noise, attenuations). Use the returned `psg_ports[]`; check `sound_write`'s returned count. If a batch write fails, mark the shadow uncertain so the next flush rewrites known desired state.

---

## 8. Example application workflows

These patterns use the implemented public API.

### 8.1 Play a ZTR in a KRES resource

```c
/* Assume container and resource are opened with the existing KRES API. */
kres_resource song_res;
kos_reader_t song_reader;

kres_get(&container, "music/theme.ztr", &song_res, 0);
/* Adapter creates a borrowed reader over song_res. */
kos_reader_from_kres(&song_reader, &song_res);

snd_open();
snd_ztr_open(&song_reader);
snd_ztr_prepare();                    /* the first order */
snd_ztr_play();

/* Application configures notify()/timer_event() and calls snd_tick()
 * on delivered events. Main loop keeps calling snd_service(). */

snd_ztr_stop();
snd_ztr_close();
snd_close();
kres_rclose(&song_res);
```

Ordering of cleanup matters: stop the player before closing the borrowed resource. If KRES is mutable, the caller must not grow/delete/replace this song resource during playback.

### 8.2 Play the same ZTR from an ordinary stream

```c
FILE *file = fopen("/fs/THEME.ZTR", "rb");
kos_reader_t song_reader;

kos_reader_from_stdio(&song_reader, file);
snd_open();
snd_ztr_open(&song_reader);
/* same prepare / play / timer / service sequence as above */
snd_ztr_close();
snd_close();
fclose(file);
```

The identical reader can be backed by a raw KonaOS file handle or memory, without any ZTR-specific read path. **Do not** allocate a second 512-byte buffer around `FILE *`.

### 8.3 Short manual song in C

```c
snd_instrument_simple_t bell = {
    .kind = SND_INSTR_TONE,
    .peak_level = 190,
    .gate_ticks = 8,
    .attack_ticks = 0,
    .decay_ticks = 2,
    .sustain_level = 90,
    .release_ticks = 3
};

snd_open();
snd_set_rate(50);
snd_define_instrument(0, &bell);
snd_note_for(0, SND_C4, 8, 0);
snd_note_for(1, SND_E4, 8, 0);
snd_note_for(2, SND_G4, 8, 0);
/* app supplies timer events; call snd_tick() until voices finish */
snd_close();
```

For an actual phrase, parse a small notation string once into a caller-owned array with `snd_phrase_parse`; do not interpret source-language tokens in the tick callback.

---

## 9. Packaging, headers, and dependencies

Implemented source layout (**do not introduce `libztr`**):

```text
user/sdk/
  include/kos/snd.h             public musical API
  snd/
    src/snd.c                   lifecycle / card ownership (C)
    src/snd_manual.c            note/instrument compile to voice programs (C)
    src/snd_phrase.c            optional tokenizer + phrase compile (C)
    src/snd_zvg.c               ZVG validate / prefetch (C)
    src/snd_ztr.c               ZTR validate, resident tables, order refill (C)
    src/snd_private.h           private shared state
    src/snd_tick.asm            snd_tick dispatch, catch-up bound
    src/snd_out.asm             register shadow, latch encoding, direct/batch flush
    src/snd_voice.asm           voice-program engine
    src/snd_zvg_dec.asm         ZVG command decoder
    src/snd_ztr_dec.asm         ZTR rows, macros, effects and render

  include/kos/reader.h          shared reader contract (only if absent)
  src/reader_*.c                separate tiny adapters for file/stdio/KRES/RAM
```

Link semantics:

- `LIBS=snd` links `libsnd` and normal `libkos` dependencies.
- A manual-only program must not pull in ZTR indexing, macro parsing, KRES, stdio, or phrase parsing unless referenced.
- ZTR playback must not automatically pull in KRES; an adapter is linked only when requested.
- The reader adapters use **existing** filesystem, buffered stream, and KRES implementations rather than recoding them.
- No program includes driver-private or kernel headers. Public `kos/snd.h` depends only on public SDK/ABI headers.
- Far-code and data use existing EXE and SDK facilities; if an archive member is placed in far code, keep event-path callable entry points and buffers visible and safe under KonaOS mapping rules.

Expected user-facing docs:

- `docs/snd-sdk.md` (this implementation contract)
- `docs/sdk.md` addition for `LIBS=snd`, `kos/snd.h`, sample usage, memory and timing cost
- short application examples `snd-notes`, `snd-ztr-file`, `snd-ztr-kres`
- tests and a host trace tool; no on-target CSM compiler

Host authoring tools ship in the SDK's `tools/snd/`: `csmc.py` (CSM score to ZTR, with `csm_language.py`, its parser and model) and `fur2ztr.py` (a Furnace text export to ZTR). Their source is `UserSoftware/SNTracker/tools/`, with `docs/CSM-Language.md` and `docs/ZTR-Format.md` beside `sdk.md` in the SDK's `docs/`. libsnd streams patterns, so a song's size is bounded by the format, not by a player's RAM: the compiler's `max_ztr_bytes` target option defaults to the format's limit.

---

## 10. Implementation work packages and acceptance

The work packages: 97 oracles (SND-0), 98 core and manual notes (SND-1), 99 readers (SND-2), 100 ZVG, 101 ZTR validation, resident tables and order refill (SND-3/4, the C side), 102 ZTR engine (SND-3/4, the assembly tick side), 103 phrases, clients and examples (SND-5).

### SND-0 — Inventory and freeze behavioral oracles

Inspect the existing **shell tracker** source to locate: ZTR loader, order/pattern iteration, macro runtime, register writer, event/timer handling and current buffering. Record an executable baseline for existing valid ZTR fixtures. Use `ZTR-Format.md` as the validation authority. Do not copy unverified code paths or assume the complete shell-tracker source has been provided with this design.

**Acceptance:** identical per-tick channel states and ordered PSG register traces from the existing player for representative songs (or documented audible-equivalent traces where output deduplication is enabled). Capture malformed fixture behavior separately.

### SND-1 — Card lifecycle, shadow, and manual notes

Add `libsnd` optional archive, `kos/snd.h`, exclusive `sound_open/info/close`, proper shadow/reset, direct vs batched write backends, static A440 note table, simple instruments, manual note/release, noise configuration, timer-paced tick. No ZTR or file dependency.

**Acceptance:** 12 tone voices/4 noise endpoints map correctly; tone divisor and attenuation outputs correct; all sound silenced and hardware released on close; noise tone2 coupling protected; register trace identical in direct/batched modes; no per-byte SOUND syscall.

### SND-2 — Shared reader and adapters

First look for an existing compatible reader contract in the SDK. If absent, add the minimal `kos_reader_t` and independently linkable adapters over OS handles, `FILE *`, `kres_resource`, and RAM. The read contract uses absolute offsets and exact logical source lengths.

**Acceptance:** identical byte fixtures, short-read handling, random reads and bounds via all adapters; KRES uses only the container's existing physical handle; stdio reuses its existing buffer; neither adapter implicitly closes its source; no kernel/driver change.

### SND-3 — Extract ZTR decoder, validator and player

Move the working shell-tracker ZTR playback into `libsnd`. Implement strict v1 validation; orders, patterns, macros, per-channel state, note-divisor table and effects. Preserve all documented behavior. Provide `snd_ztr_open/prepare/play/stop/close/status` and use the shared PSG writer.

**Acceptance:** valid CSM- and Furnace-generated ZTR fixtures play with reference traces; invalid headers, lengths, offsets, rows, events, macros, references and unsupported effects fail without hardware side effects. Song completion and release behavior agree with the reference.

### SND-4 — Foreground order refill and bounded event execution

Support ZTRs from borrowed readers without a full-song RAM buffer: the forward validation pass, the resident tables, the order double buffer refilled by `snd_service`, single-byte publication and safe underrun at an order change. Integrate timer event `count/missed` and measured processing budget.

**Acceptance:** instrumented tick path has zero source reads/seeks/allocations; a reader stalled across an order change causes a safe underrun and exact resumption; same song plays from RAM, raw file, `FILE *`, and KRES; resident, buffer and stack sizes recorded; no unbounded tick replay.

### SND-5 — Tiny notation helper and integration

Optional `snd_phrase_parse` and `snd_phrase_start`; replace the shell tracker's **playback** dependency with `libsnd` and make the native tracker another client. Keep tracker editing and ZTR generation outside `libsnd`. Add SDK examples and build rules.

**Acceptance:** parse legal notes/sharps/flats/rests/durations and reject all unsupported CSM syntax; a manual-only binary does not link ZTR decoder; tracker/shell uses exactly one ZTR playback implementation; no regression in the serial/graphics console path.

### 10.1 Acceptance matrix

| Area | Must demonstrate |
|---|---|
| Behavior | Per-tick musical state and chip writes match shell tracker |
| Hardware | 4 PSGs + noise voices, correct target and port mapping |
| Manual music | Notes/chords, duration, release, simple ADSR, rests/phrases |
| ZTR | Header, orders, sparse patterns, all documented v1 macros/effects |
| File abstraction | Byte-identical playback from file, `FILE *`, KRES, RAM |
| File discipline | KRES one backing handle; no redundant 512-byte cache |
| Streaming | No source I/O on tick path; order double buffer and underrun |
| Performance | Timer, event, processing, and write T-states measured |
| Lifecycle | Exclusive open/reset/release; safe close on errors/exit |
| Isolation | No libztr, no CSM runtime, no new bank-6/7 code |
| ZVG | ZVGC v1/v2 trace-identical to VGMPlayer's decoder, streamed |
| Assembly | Every per-tick routine is assembly; setup only in C |
| Link size | Manual-only, ZTR-only, and KRES-backed sizes recorded |
| Robustness | Truncation, bad offsets, invalid macro, short read and I/O faults |

Host tests should use an output-capture backend with **deterministic per-tick traces**, and a fake source implementation that intentionally produces partial reads and transient failures. Target tests should run under `z80sim.py` acceptance and on real Afternoon Blend hardware. Include a timing benchmark for 50 and 60 Hz tick pacing, both register-output modes, simultaneous graphics, and disk prefetch under load.

---

## 11. Decisions intentionally deferred to implementation evidence

1. **Actual shell-player extraction seams:** inspect existing code, preserve behavior, and avoid reimplementing a correct parser to suit the proposed file structure.
2. **Reader API names:** use an already present SDK abstraction if one exists; only the read-at + size *contract* matters.
3. **Resident capacities and order-buffer size:** derive from real ZTR sizes and available application RAM. Do not hard-code a 20 KiB song limit from a legacy player into the SDK.
4. **Exact API typedefs and error constants:** assign consistently with KonaOS SDK conventions after inspecting the public headers; examples above are proposed, not ABI-frozen.
5. **Catch-up and underrun limits:** choose measurable bounded values via simulator fault tests and an audible hardware trial.
6. **Audibly equivalent write deduplication:** preserve reference traces first; optimize only with regression evidence.

**Final rule:** `libsnd` is the single userland sound SDK. **ZTR is the sophisticated path; manual notes are the intentionally small path.** Both reuse one SOUND card interface and existing storage libraries, and neither introduces higher-level musical behavior into KonaOS banks 6/7.
