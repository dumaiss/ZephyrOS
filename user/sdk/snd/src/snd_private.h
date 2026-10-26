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

/* snd_zvg.c's one bounded ring refill, also snd_service's ZVG arm
 * (snd_service itself lives in snd_ztr.c). */
int snd_zvg_refill(void);

/* --- ZTR state, shared by snd_ztr.c and snd_ztr_dec.asm ---------------- */

/* Resident capacities, sized for both fixture songs (136 orders, 398
 * patterns, 6,612 macro bytes) with headroom; exceeding one is
 * -KOS_ENOSPC at open. */
#define SND_ZTR_MAX_ORDERS   140u
#define SND_ZTR_MAX_PATTERNS 400u
#define SND_ZTR_INSTRUMENTS  16u
#define SND_ZTR_MACRO_BYTES  6656u
#define SND_ZTR_QUEUE_ROWS   3u   /* rows of lookahead (2 usable)  */

/* The prepared row record, 101's contract with the engine: fixed
 * size, already decoded.  A record holds the row's position and one
 * 10-byte slot per channel; a slot's first byte is the decoded
 * presence mask, 0 when the channel has no event, so the consumer
 * skips idle channels on one test and never parses.
 *
 *   +0 order, +1 row; slot at 2 + channel * 10:
 *     +0 mask (0 = no event)
 *     +1 note (FFh when absent)
 *     +2 instrument (FFh when absent)
 *     +3 volume (FFh when absent)
 *     +4 effect (0 = none)
 *     +5 effect parameter
 *     +6 modifiers
 *     +7 arpeggio (MOD_ARP_SET's payload)
 *     +8 hairpin rate (MOD_HAIRPIN_SET's payload, signed)
 *     +9 hairpin ceiling */
#define SND_ZTR_ROW_BYTES  162u
#define SND_ZTR_SLOT_BYTES 10u
#define SND_ZTR_MACRO_SLOTS 5u

/* The ZTR event and modifier bits and the native effects
 * (docs/ZTR-Format.md), shared by the C producer and the engine. */
#define ZTR_EVENT_NOTE       0x80u
#define ZTR_EVENT_INSTRUMENT 0x40u
#define ZTR_EVENT_VOLUME     0x20u
#define ZTR_EVENT_EFFECT     0x10u
#define ZTR_EVENT_OFF        0x08u
#define ZTR_EVENT_RELEASE    0x04u
#define ZTR_EVENT_LEGATO     0x02u
#define ZTR_EVENT_MODIFIERS  0x01u

#define ZTR_MOD_RESET        0x80u
#define ZTR_MOD_ARP_SET      0x40u
#define ZTR_MOD_ARP_CLEAR    0x20u
#define ZTR_MOD_HAIRPIN_SET  0x10u
#define ZTR_MOD_HAIRPIN_CLEAR 0x08u

#define ZTR_EFFECT_ARPEGGIO  1u
#define ZTR_EFFECT_VOL_SLIDE 2u
#define ZTR_EFFECT_LEGATO    3u
#define ZTR_EFFECT_VOL_DEC   4u
#define ZTR_EFFECT_FAST_VOL  5u
#define ZTR_EFFECT_STOP      6u

extern uint8_t snd_ztr_state;        /* SND_ZTR_* from kos/snd.h    */
extern uint16_t snd_ztr_rate;        /* the header's tick rate      */
extern uint8_t snd_ztr_speed;        /* ticks per row               */
extern uint16_t snd_ztr_pattern_length;
extern uint16_t snd_ztr_order_count;

/* The producer/consumer queue: record indices as single bytes; the
 * producer publishes the tail only after the record is complete, the
 * consumer advances the head only after the row is applied.  Record
 * i lives at snd_ztr_queue_addrs[i] (a 16-bit table, so neither side
 * multiplies by SND_ZTR_ROW_BYTES). */
extern uint8_t snd_ztr_head;
extern volatile uint8_t snd_ztr_tail;
extern uint16_t snd_ztr_queue_addrs[SND_ZTR_QUEUE_ROWS];

/* The engine's position (assembly-owned): the row to consume next and
 * the row's tick. */
extern uint8_t snd_ztr_order;
extern uint8_t snd_ztr_row;
extern uint8_t snd_ztr_tick_in_row;

/* The voices the orders use (open reserves them in snd_reserved_voices)
 * and the playback counters. */
extern uint16_t snd_ztr_mask;
extern uint16_t snd_ztr_underruns;
extern int snd_ztr_last_error;

/* The resident tables, page-aligned.  The divisor table splits into a
 * lo page and the following hi page (`ld l,note / ld e,(hl) /
 * inc h / ld d,(hl)`); the loudness page holds base<<4|macro ->
 * base*macro/15; the instrument page holds one 16-byte entry per
 * number, its macro offsets remapped into the resident macro area. */
extern uint8_t snd_ztr_core_page;    /* 16 x 16-byte channel records */
extern uint8_t snd_ztr_core2_page;   /* the effect/notation half     */
extern uint8_t snd_ztr_macro_page[SND_ZTR_MACRO_SLOTS];
extern uint8_t snd_ztr_div_lo_page;  /* the hi page follows          */
extern uint8_t snd_ztr_loud_page;
extern uint8_t snd_ztr_instr_page;
extern uint16_t snd_ztr_macro_base;  /* the resident macro data     */

/* Assembly entry points the C members use (snd_ztr_dec.asm). */
uint8_t *snd_ztr_row_address(uint8_t index); /* the record at index i */
void snd_ztr_tick(void);             /* one playback tick           */
void snd_ztr_mute(void);             /* the mask's voices to atten 15 */
void snd_ztr_restore(void);          /* re-render from the held state */
void snd_ztr_reset_channels(void);   /* clear the engine's channels  */

/* The engine's channel-state layout (snd_ztr_dec.asm owns it; the
 * channel is page:(c << 4), field offsets below).  sndtest's ztrdump
 * mode reads it for the debug export. */
#define SND_ZTR_CHANNELS 16u

#define ZC_WORK        0u /* bit7 dirty-any, bits2-6 the macro slots */
#define ZC_DIRTY       1u
#define ZC_FLAGS       2u /* bit0 active, bit1 released, bit2 legato,
			   * bit3 notation arpeggio                  */
#define ZC_NOTE        3u
#define ZC_INSTRUMENT  4u
#define ZC_BASE_VOL    5u
#define ZC_MACRO_VOL   6u
#define ZC_EFFECT      7u
#define ZC_EFFECT_PARAM 8u
#define ZC_ARP_LO      9u
#define ZC_ARP_FLAGS   10u
#define ZC_PITCH_LO    11u
#define ZC_PITCH_HI    12u
#define ZC_DUTY        13u
#define ZC_PHASE       14u
#define ZC_ARP_HI      15u

/* The effect/notation half (the core2 page). */
#define ZC2_EFFECT_PHASE 0u
#define ZC2_NOT_ARP    1u
#define ZC2_NOT_PHASE  2u
#define ZC2_HAIR_RATE  3u
#define ZC2_HAIR_OFF   4u
#define ZC2_HAIR_CEIL  5u
/* The oracle's register cache, as channel fields (psg.c). */
#define ZC2_LAST_PER_LO 6u
#define ZC2_LAST_PER_HI 7u
#define ZC2_LAST_ATT   8u
#define ZC2_LAST_NOISE 9u

#define ZM_POS_LO      0u /* macro_position, an item index */
#define ZM_POS_HI      1u
#define ZM_TIMER       2u
#define ZM_DELAY       3u
#define ZM_MACRO_LO    4u
#define ZM_MACRO_HI    5u

#endif /* KOS_SND_PRIVATE_H */
