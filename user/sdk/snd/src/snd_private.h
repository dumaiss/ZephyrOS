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

/* The active-voice list's "no voice" link (BSS-zero would read as
 * voice 0, so the head must be initialized explicitly). */
#define SND_VOICE_NONE 0xFFu

/* The worst program: period (3), one noise (2), 16 ramp steps of
 * WAIT16+ATTEN (5 each = 80), end (1): 86 bytes. */
#define SND_PROGRAM_BYTES 86u

/* snd_tick advances each voice by at most this many ticks per call;
 * the excess counts as an overrun and is never replayed.  Chosen so a
 * 50-60 Hz application that misses a whole window of events still
 * advances at the song's rate while an unbounded coalesced count can
 * never produce an unbounded write burst. */
#define SND_TICK_CATCHUP_MAX 16u

/* snd_tick's per-tick write batch (BATCH mode): four chips of at most
 * 3 tone pairs + 1 noise + 4 attenuations = 44 writes. */
#define SND_BATCH_MAX 44u

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

/* The library-owned program buffers, one per voice, and the per-voice
 * state the C members share with the engine: the active flag (a note
 * is installed) and the current linear level (what snd_note_off's
 * release starts from).  The engine's own scheduling state - the run
 * and wait lists, the program cursors and the wait/due counters -
 * lives in snd_voice.asm's fixed bytes, not here. */
extern uint8_t snd_programs[SND_VOICE_COUNT][SND_PROGRAM_BYTES];
extern uint8_t snd_voice_active[SND_VOICE_COUNT];
extern uint8_t snd_voice_level[SND_VOICE_COUNT];
extern uint8_t snd_voice_head;           /* SND_VOICE_NONE when idle  */
extern uint8_t snd_wait_head;            /* the wait list's head      */
extern uint16_t snd_min_due;             /* the earliest due time     */

/* The flush's change flag: every writer of desired state raises it and
 * snd_output_flush returns immediately (before any compare) while it
 * is clear, which is what keeps an unchanged tick within budget. */
extern uint8_t snd_dirty;

/* Assembly entry points the C members use. */
void snd_load_ports(const void *info);
void snd_shadow_invalidate(void);
void snd_output_flush(void);
void snd_voice_install(uint8_t voice);
void snd_voice_clear(uint8_t voice);     /* idle + level 0, silent  */
void snd_noise_apply(uint8_t noise_voice, uint8_t control);

/* The per-chip desired state, the hardware shadow and the output
 * machinery, owned by snd_out.asm (the C members read the desired
 * noise control for the tone-2 coupling check). */
extern uint16_t snd_desired_period[SND_CHIP_COUNT][3];
extern uint8_t snd_desired_atten[SND_CHIP_COUNT][4];
extern uint8_t snd_desired_noise[SND_CHIP_COUNT];

/* The tick engine's counters. */
extern unsigned int snd_overruns;

/* The current effective rate: the active ZTR/ZVG's, else the manual
 * rate.  Package 98 implements the manual half. */
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
