/* KonaOS libsnd: the userland musical playback SDK (docs/snd-sdk.md).
 *
 * One process-local sound session above `/dev/sound` (kos/sound.h):
 * exclusive card ownership, a per-chip register shadow, a bounded
 * assembly tick engine, and three musical sources - manual notes and
 * instruments in C, pre-rendered ZVGC streams (snd_zvg_*) and ZTR songs
 * (snd_ztr_*).
 *
 * Rendering is assembly: everything that runs per tick - `snd_tick`,
 * the voice programs, the ZVG and ZTR engines, the register shadow and
 * the port writes - is Z80 code.  Setup is C: open/close, validation,
 * note and phrase compilation and the foreground prefetch
 * (`snd_service`).  The tick path never parses, compiles, allocates or
 * reads a source.
 *
 * Return convention: 0 or a non-negative result, or the negated native
 * error code.  Voice numbering: logical physical voice 0..15; 0-2,
 * 4-6, 8-10 and 12-14 are the four chips' tone voices, 3, 7, 11 and 15
 * their noise voices.
 */

#ifndef KOS_SND_H
#define KOS_SND_H

#include "kos/types.h"
#include "kos/reader.h"

#define SND_VOICE_COUNT   16u
#define SND_CHIP_COUNT    4u
#define SND_NOTE_COUNT    96u
#define SND_INSTRUMENTS   8u
#define SND_MAX_TICK_RATE 180u

/* snd_set_output modes. */
#define SND_OUTPUT_DIRECT 0u /* sound_out to the reported port     */
#define SND_OUTPUT_BATCH  1u /* one sound_write per tick           */

/* snd_ztr_prepare's positive code while the start window is not yet
 * resident: the caller repeats it (or snd_service) until 0. */
#define SND_NEED_MORE 1u

/* Notes C-0..B-7 as snd_note_t values (0 = C-0, 95 = B-7). */
#define SND_C0 0u
#define SND_C4 48u
#define SND_C8 96u
#define SND_CS0 1u
#define SND_D0 2u
#define SND_DS0 3u
#define SND_E0 4u
#define SND_F0 5u
#define SND_FS0 6u
#define SND_G0 7u
#define SND_GS0 8u
#define SND_A0 9u
#define SND_AS0 10u
#define SND_B0 11u
#define SND_C1 12u
#define SND_CS1 13u
#define SND_D1 14u
#define SND_DS1 15u
#define SND_E1 16u
#define SND_F1 17u
#define SND_FS1 18u
#define SND_G1 19u
#define SND_GS1 20u
#define SND_A1 21u
#define SND_AS1 22u
#define SND_B1 23u
#define SND_C2 24u
#define SND_CS2 25u
#define SND_D2 26u
#define SND_DS2 27u
#define SND_E2 28u
#define SND_F2 29u
#define SND_FS2 30u
#define SND_G2 31u
#define SND_GS2 32u
#define SND_A2 33u
#define SND_AS2 34u
#define SND_B2 35u
#define SND_C3 36u
#define SND_CS3 37u
#define SND_D3 38u
#define SND_DS3 39u
#define SND_E3 40u
#define SND_F3 41u
#define SND_FS3 42u
#define SND_G3 43u
#define SND_GS3 44u
#define SND_A3 45u
#define SND_AS3 46u
#define SND_B3 47u
#define SND_CS4 49u
#define SND_D4 50u
#define SND_DS4 51u
#define SND_E4 52u
#define SND_F4 53u
#define SND_FS4 54u
#define SND_G4 55u
#define SND_GS4 56u
#define SND_A4 57u
#define SND_AS4 58u
#define SND_B4 59u
#define SND_C5 60u
#define SND_CS5 61u
#define SND_D5 62u
#define SND_DS5 63u
#define SND_E5 64u
#define SND_F5 65u
#define SND_FS5 66u
#define SND_G5 67u
#define SND_GS5 68u
#define SND_A5 69u
#define SND_AS5 70u
#define SND_B5 71u
#define SND_C6 72u
#define SND_CS6 73u
#define SND_D6 74u
#define SND_DS6 75u
#define SND_E6 76u
#define SND_F6 77u
#define SND_FS6 78u
#define SND_G6 79u
#define SND_GS6 80u
#define SND_A6 81u
#define SND_AS6 82u
#define SND_B6 83u
#define SND_C7 84u
#define SND_CS7 85u
#define SND_D7 86u
#define SND_DS7 87u
#define SND_E7 88u
#define SND_F7 89u
#define SND_FS7 90u
#define SND_G7 91u
#define SND_GS7 92u
#define SND_A7 93u
#define SND_AS7 94u
#define SND_B7 95u

/* Instrument kinds. */
#define SND_INSTR_TONE  0u
#define SND_INSTR_NOISE 1u

/* Noise modes (snd_instrument_simple_t.noise_mode / snd_set_noise). */
#define SND_NOISE_PERIODIC 0u
#define SND_NOISE_WHITE    1u

typedef uint8_t snd_voice_t;
typedef uint8_t snd_note_t;

typedef struct snd_instrument_simple {
	uint8_t kind;            /* SND_INSTR_TONE or SND_INSTR_NOISE */
	uint8_t peak_level;      /* linear loudness 0..255             */
	uint16_t gate_ticks;     /* the default duration              */
	uint8_t attack_ticks;
	uint8_t decay_ticks;
	uint8_t sustain_level;   /* linear loudness 0..255            */
	uint8_t release_ticks;
	uint8_t noise_mode;      /* SND_NOISE_PERIODIC or _WHITE      */
	uint8_t noise_rate;      /* 0..3; 3 couples the chip's tone 2 */
} snd_instrument_simple_t;

/* snd_get_info's record: the card and the current session. */
struct snd_info {
	uint8_t version;             /* 1                              */
	uint8_t psg_count;           /* the card's PSGs                */
	uint8_t caps;                /* KSOUND_CAP_* the card answers  */
	uint8_t output_mode;         /* the current SND_OUTPUT_*       */
	uint16_t tick_rate;          /* the active ZTR/ZVG rate, else
				      * the manual rate                */
	uint16_t manual_rate;        /* snd_set_rate's value           */
	uint8_t psg_ports[SND_CHIP_COUNT];
	uint8_t dac_port;            /* the DAC's port, or 0            */
	uint32_t psg_clock_hz;
};

/* --- lifecycle and time ------------------------------------------------ */

/* Acquire `/dev/sound` exclusively, read the card's info and ports,
 * check the PSG clock against the built-in A440 table and set up a
 * muted, invalidated shadow.  A failure holds nothing. */
int snd_open(void);

/* Silence all 16 voices and release the card. */
void snd_close(void);

/* The current session (rates, chip layout, caps). */
int snd_get_info(struct snd_info *out);

/* DIRECT (sound_out per byte) or BATCH (one sound_write per tick);
 * both modes render the identical register trace. */
int snd_set_output(uint8_t mode);

/* The manual music default rate in ticks per second (1-180).  While a
 * ZTR or ZVG plays, snd_tick_rate reports that source's rate. */
int snd_set_rate(uint16_t ticks_per_sec);
uint16_t snd_tick_rate(void);

/* The recommended timer-event period for the current rate:
 * round(39,062.5 / rate) in 25.6 us counts.  Refuses an
 * unrepresentable (zero) rate. */
int snd_timer_period(uint32_t *counts);

/* Advance every active voice program by `elapsed_ticks`, at most the
 * documented catch-up bound (SND_TICK_CATCHUP_MAX); the excess counts
 * as an overrun and is never replayed.  Then one shadow flush.  No
 * file I/O, allocation or compilation happens here. */
int snd_tick(uint16_t elapsed_ticks);

/* The foreground prefetch for the ZVG/ZTR readers.  Never call it from
 * an event handler. */
int snd_service(void);

/* Supply a 96-entry divisor table for a card whose clock the built-in
 * A440 table does not match.  Pass 0 to go back to the built-in table. */
int snd_set_note_table(const uint16_t *divisors);

/* --- manual music ------------------------------------------------------ */

int snd_define_instrument(uint8_t id,
			  const snd_instrument_simple_t *instrument);
int snd_note_on(snd_voice_t voice, snd_note_t note, uint8_t instrument);
int snd_note_for(snd_voice_t voice, snd_note_t note,
		 uint16_t duration_ticks, uint8_t instrument);
int snd_note_off(snd_voice_t voice);    /* begin instrument release */
int snd_voice_silence(snd_voice_t voice);
int snd_set_noise(snd_voice_t noise_voice, uint8_t mode, uint8_t rate);

/* --- ZVG pre-rendered streams ------------------------------------------ */

/* The prefetch ring: 256 bytes, a power of two, so the producer and
 * consumer indices are single atomic bytes. */
#define SND_ZVG_RING 256u

/* snd_zvg_get_status states. */
#define SND_ZVG_IDLE     0u /* no stream (also after stop/close)     */
#define SND_ZVG_PREPARED 1u /* open validated, the ring filling      */
#define SND_ZVG_PLAYING  2u /* the tick decoder is consuming         */
#define SND_ZVG_UNDERRUN 3u /* a command was incomplete; resumable   */
#define SND_ZVG_ERROR    4u /* a malformed opcode; the chip is muted */
#define SND_ZVG_DONE     5u /* the END opcode played; chip muted     */

struct snd_zvg_status {
	uint8_t state;
	uint32_t ticks;         /* decoder ticks since open            */
	uint32_t bytes;         /* stream bytes consumed (header excl.) */
	uint32_t writes;        /* stream bytes written to the chip     */
	uint16_t underruns;     /* incomplete-command ticks             */
	int last_error;         /* the reader error, or 0               */
	uint32_t error_offset;  /* stream offset of the bad opcode      */
};

/* Validate the ZVGC v1/v2 header through the borrowed reader (exactly
 * the reference checks: magic, version, a 1-180 Hz rate, the declared
 * length against the reader's size) and reserve the chip's four
 * voices.  On failure nothing changes.  The source is borrowed; close
 * forgets it and closes nothing. */
int snd_zvg_open(const kos_reader_t *source, uint8_t chip);

/* Fill the ring from the reader before playback (foreground only). */
int snd_zvg_prepare(void);

int snd_zvg_play(void);

/* Stop the stream, mute the chip, invalidate its shadow and release
 * the reservation. */
int snd_zvg_stop(void);

void snd_zvg_close(void);

int snd_zvg_get_status(struct snd_zvg_status *out);

/* --- ZTR songs ---------------------------------------------------------- */

/* snd_ztr_get_status states. */
#define SND_ZTR_EMPTY     0u /* no song (also after stop/close)     */
#define SND_ZTR_PREPARING 1u /* open validated, the queue filling   */
#define SND_ZTR_READY     2u /* the start window is resident        */
#define SND_ZTR_PLAYING   3u /* the tick engine is consuming        */
#define SND_ZTR_PAUSED    4u /* muted, the musical state held       */
#define SND_ZTR_DONE      5u /* the stop effect or the song's end   */
#define SND_ZTR_UNDERRUN  6u /* a due row was not resident; muted   */
#define SND_ZTR_ERROR     7u /* open/reader error; nothing reserved */

struct snd_ztr_status {
	uint8_t state;
	uint8_t order;           /* the row the engine will consume next */
	uint8_t row;
	uint8_t tick_in_row;
	uint16_t rate;           /* the header's tick rate               */
	uint16_t reserved_voices;
	int last_error;          /* the last reader error, or 0          */
	uint16_t overruns;       /* snd_tick catch-up excess             */
	uint16_t underruns;      /* rows that were not resident          */
};

/* Validate the whole song through the borrowed reader (magic, header
 * values, section bounds, strings, instruments and macros, the
 * pattern streams and the note table - docs/ZTR-Format.md), keep the
 * tick's tables resident and reserve the voices the orders use.  On
 * failure nothing changes.  The source is borrowed; close forgets it
 * and closes nothing. */
int snd_ztr_open(const kos_reader_t *source);

/* Fill the prepared-row queue from the reader until it is full (a
 * documented number of rows, the start window snd_ztr_play needs). */
int snd_ztr_prepare(void);

/* Start at (0, 0): needs READY, then sets the ZTR voices' shadow to
 * what the reference player assumes after its mute (attenuation 15,
 * period 0, the noise written), so tick 0's writes equal the
 * golden's. */
int snd_ztr_play(void);

/* Mute the ZTR's voices and hold the musical state. */
int snd_ztr_pause(void);

/* Re-render the ZTR's voices from the held state. */
int snd_ztr_resume(void);

/* Silence the ZTR's voices, forget the song and release the
 * reservation. */
int snd_ztr_stop(void);

void snd_ztr_close(void);

int snd_ztr_get_status(struct snd_ztr_status *out);

#endif /* KOS_SND_H */
