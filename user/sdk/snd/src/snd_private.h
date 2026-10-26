/* libsnd private state and the voice-program format (user/sdk/snd).
 *
 * The tick path and the flush are assembly (snd_tick.asm, snd_voice.asm,
 * snd_out.asm); the C members (snd.c, snd_manual.c) own the setup: the
 * lifecycle, the instrument registry and the note compiler.  Everything
 * the assembly reaches lives in the fixed bytes below, in _DATA.
 */

#ifndef KOS_SND_PRIVATE_H
#define KOS_SND_PRIVATE_H

#include "kos/types.h"
#include "kos/snd.h"

/* The per-voice program buffer and the program opcodes.  A program is a
 * byte sequence the C compiler writes and the assembly engine steps:
 *
 *   SND_P_PERIOD  1, lo, hi    the tone's 10-bit divisor (0-1023)
 *   SND_P_ATTEN   2, level     the linear loudness 0-255; the engine
 *                              maps it through snd_level_table to the
 *                              SN76489 attenuation
 *   SND_P_NOISE   3, control   the noise byte (E0h | rate | mode bit),
 *                              written with the forced-control rule
 *   SND_P_WAIT8   4, n         wait n ticks (1-255)
 *   SND_P_WAIT16  5, lo, hi    wait n ticks (1-65535)
 *
 * A WAIT always ends the voice's current tick: the residual (n minus
 * what this tick already consumed) is waited out, so one envelope
 * WAIT/ATTEN pair advances per tick.
 *   SND_P_END     0            the voice goes idle
 *
 * An envelope ramp is at most one WAIT/ATTEN pair per attenuation
 * level change, so a program is bounded by the 16 hardware levels plus
 * its period and end: SND_PROGRAM_BYTES covers every compile. */
#define SND_P_END     0x00u
#define SND_P_PERIOD  0x01u
#define SND_P_ATTEN   0x02u
#define SND_P_NOISE   0x03u
#define SND_P_WAIT8   0x04u
#define SND_P_WAIT16  0x05u

/* The worst program: period (3), one noise (2), 16 ramp steps of
 * WAIT16+ATTEN (5 each = 80), end (1): 86 bytes. */
#define SND_PROGRAM_BYTES 86u

/* snd_tick advances each voice by at most this many ticks per call;
 * the excess counts as an overrun and is never replayed.  Chosen so a
 * 50-60 Hz application that misses a whole window of events still
 * advances at the song's rate while an unbounded coalesced count can
 * never produce an unbounded write burst. */
#define SND_TICK_CATCHUP_MAX 16u

/* snd_tick's write batch (BATCH mode), in {chip, byte} pairs: one tick's
 * worst manual output is 44 writes (four chips of 3 tone pairs, 1 noise,
 * 4 attenuations); a full batch is sent before the next append, so a
 * catch-up ZVG tick never overflows it. */
#define SND_BATCH_MAX 60u

/* --- C-owned state (snd.c, snd_manual.c) ------------------------------- */

extern int snd_handle;                   /* the exclusive /dev/sound */
extern uint8_t snd_output_mode;
extern uint16_t snd_manual_rate;
extern const uint16_t *snd_custom_table; /* snd_set_note_table, or 0 */
extern uint16_t snd_reserved_voices;     /* the ZTR/ZVG claim mask   */

extern const uint16_t snd_a440_table[SND_NOTE_COUNT];
extern const uint8_t snd_level_table[256];

extern struct snd_instrument_simple snd_instruments[SND_INSTRUMENTS];
extern uint8_t snd_instrument_defined[SND_INSTRUMENTS];

/* The library-owned program buffers, one per voice, and the active flag
 * (a note is installed) the C members share with the engine. */
extern uint8_t snd_programs[SND_VOICE_COUNT][SND_PROGRAM_BYTES];
extern uint8_t snd_voice_active[SND_VOICE_COUNT];

/* Assembly owns sixteen 16-byte descriptors in one 256-byte-aligned page
 * (snd_desc_page; voice v's descriptor is page:(v << 4)), walked in voice
 * order with HL, no IX/IY.  The next page is a copy of snd_level_table.
 *
 *   +0 flags (bit 0 active, bit 1 pending silence, bit 2 forced noise
 *      write); 0 means nothing to do
 *   +1..2 wait, +3..4 program cursor
 *   +5 the current linear level (what snd_note_off's release starts from)
 *   +6 desired attenuation, +7 shadow attenuation, +8 attenuation latch
 *   +9 chip port, +10 register latch (80h|sub<<5; E0h for noise), +11 chip
 *   +12..13 desired register pointer, +14..15 shadow register pointer
 *
 * The attenuation write walks +6..+9 with `inc l`; the register pointers
 * address the chip's period pair or noise byte.  RAM: a 768-byte area
 * holding the two aligned pages. */
extern uint8_t snd_desc_page;

/* The flush's change flag: every writer of desired state raises it and
 * snd_output_flush returns immediately (before any compare) while it
 * is clear, which is what keeps an unchanged tick within budget. */
extern uint8_t snd_dirty;

/* Assembly entry points the C members use. */
void snd_load_ports(const void *info);
void snd_voice_init(void);
void snd_shadow_invalidate(void);
void snd_output_flush(void);
void snd_engine(uint16_t elapsed);
void snd_voice_install(uint8_t voice);
void snd_voice_clear(uint8_t voice);     /* idle + level 0, silent  */
uint8_t snd_voice_level(uint8_t voice); /* the current linear level */
void snd_noise_apply(uint8_t noise_voice, uint8_t control);

/* Fast internal assembly ABI.  HL is the voice's descriptor base (any
 * field of it: the primitives mask L).  PERIOD takes DE, ATTEN takes A,
 * and NOISE takes A plus a nonzero B to force an equal-valued control
 * write.  Each primitive updates the desired state, compares/updates the
 * shadow and emits DIRECT or appends BATCH; each clobbers A, BC, DE and
 * L (H is kept). */
/* snd_put_period(HL, DE), snd_put_atten(HL, A), snd_put_noise(HL, A, B) */

/* The per-chip desired state, the hardware shadow and the output
 * machinery, owned by snd_out.asm (the C members read the desired
 * noise control for the tone-2 coupling check). */
extern uint16_t snd_desired_period[SND_CHIP_COUNT][3];
extern uint8_t snd_desired_noise[SND_CHIP_COUNT];

/* The tick engine's counters. */
extern unsigned int snd_overruns;

/* The current effective rate: the active ZTR/ZVG's, else the manual
 * rate. */
extern uint16_t snd_active_rate;

/* --- ZVG state, shared by snd_zvg.c and snd_zvg.asm -------------------- */

/* The producer/consumer ring and its byte indices (each index is one
 * atomic byte).  snd_service publishes the tail only after the bytes
 * are in; the tick consumes complete commands only. */
extern uint8_t snd_zvg_ring_page;  /* the 256-byte-aligned ring's page */
extern volatile uint8_t snd_zvg_head;
extern volatile uint8_t snd_zvg_tail;

extern uint8_t snd_zvg_chip;
extern uint8_t snd_zvg_state;      /* SND_ZVG_* from kos/snd.h */
extern uint16_t snd_zvg_rate;      /* the file's ticks/second   */
extern uint16_t snd_zvg_wait;      /* the decoder's wait counter */
extern uint32_t snd_zvg_remaining; /* stream bytes not yet produced */
extern uint32_t snd_zvg_consumed;  /* stream bytes consumed (header
				    * excluded) */
extern uint32_t snd_zvg_ticks;     /* decoder ticks since open */
extern uint32_t snd_zvg_writes;    /* stream bytes written */
extern uint16_t snd_zvg_underruns; /* incomplete-command ticks */
extern int snd_zvg_last_error;     /* the reader error, or 0 */
extern uint32_t snd_zvg_error_offset;
extern uint8_t snd_zvg_error_op;   /* the bad opcode, or FFh if none */
extern uint32_t snd_zvg_cursor;    /* the reader's next offset */
extern uint8_t snd_zvg_eof;        /* the reader delivered its size */
extern kos_reader_t snd_zvg_reader; /* the borrowed reader's copy */

/* The ZVG assembly decoder: one playback tick.  The wait counter, then
 * complete commands up to the next wait or the end; WRITE and WRITE
 * RUN bytes go to the chip (DIRECT to its port, BATCH appended to the
 * tick's batch).  The chip bypasses the shadow; the stop/close path
 * invalidates it. */
void snd_zvg_tick(void);

#endif /* KOS_SND_PRIVATE_H */
