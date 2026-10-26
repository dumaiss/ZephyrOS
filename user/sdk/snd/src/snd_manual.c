/* KonaOS libsnd: manual notes and instruments (user/sdk/snd).
 *
 * The C compiler half: every snd_note_on / snd_note_for / snd_note_off
 * call compiles the note and its instrument's envelope into the voice's
 * program buffer - a byte program the assembly engine steps - and hands
 * it over atomically.  No interpretation happens on the tick; a
 * program is bounded by the 16 hardware attenuation levels, not by an
 * envelope's length in ticks.
 *
 * Levels are linear loudness 0..255 and map to the SN76489 attenuation
 * through snd_level_table (255 loudest -> attenuation 0).  The engine
 * keeps each voice's current level, which is what a release starts
 * from.
 */

#include "kos.h"

#include "snd_private.h"

struct snd_instrument_simple snd_instruments[SND_INSTRUMENTS];
uint8_t snd_instrument_defined[SND_INSTRUMENTS];
uint8_t snd_programs[SND_VOICE_COUNT][SND_PROGRAM_BYTES];
uint8_t snd_voice_active[SND_VOICE_COUNT];
uint8_t snd_voice_level[SND_VOICE_COUNT];
uint8_t snd_releasing[SND_VOICE_COUNT];
uint8_t snd_voice_instrument[SND_VOICE_COUNT];

/* --- the compiler ------------------------------------------------------ */

struct compiler {
	uint8_t *cursor;
	uint8_t *limit;
	uint8_t last_atten;
	unsigned int run;       /* ticks at last_atten, not yet emitted */
	unsigned char full;
};

static void emit(struct compiler *compiler, uint8_t byte)
{
	if (compiler->cursor >= compiler->limit) {
		compiler->full = 1u;
		return;
	}
	*compiler->cursor++ = byte;
}

static void wait(struct compiler *compiler, unsigned int ticks)
{
	if (ticks == 0u) {
		return;
	}
	if (ticks <= 255u) {
		emit(compiler, SND_P_WAIT8);
		emit(compiler, (uint8_t)ticks);
	} else {
		emit(compiler, SND_P_WAIT16);
		emit(compiler, (uint8_t)ticks);
		emit(compiler, (uint8_t)(ticks >> 8));
	}
}

/* One envelope tick at linear `level`: emits WAIT/ATTEN only when the
 * mapped attenuation changes (at most one pair per level change). */
static void step(struct compiler *compiler, uint8_t level)
{
	uint8_t atten = snd_level_table[level];

	if (atten != compiler->last_atten) {
		wait(compiler, compiler->run);
		emit(compiler, SND_P_ATTEN);
		emit(compiler, level);
		compiler->last_atten = atten;
		compiler->run = 0u;
	}
	++compiler->run;
}

static uint8_t ramp(uint16_t start, uint16_t end, uint16_t step_no,
		    uint16_t steps)
{
	unsigned int span;
	unsigned int delta;

	if (start >= end) {
		span = (unsigned int)(start - end);
		delta = (span * (unsigned int)step_no) /
			(unsigned int)steps;
		return (uint8_t)(start - delta);
	}
	span = (unsigned int)(end - start);
	delta = (span * (unsigned int)step_no) / (unsigned int)steps;
	return (uint8_t)(start + delta);
}

/* The envelope the sndtest adsr reference reproduces:
 *   attack: 0 -> peak over attack_ticks (0 ticks: peak at once);
 *   decay:  peak -> sustain over decay_ticks;
 *   gate:   held for gate_ticks;
 *   release: sustain -> 0 over release_ticks (0 ticks: silent at once).
 * The final attenuation-15 step always ends the program. */
static int compile_note(uint8_t voice, snd_note_t note,
			uint16_t gate_ticks,
			const struct snd_instrument_simple *instrument)
{
	const uint16_t *table = snd_custom_table != (const uint16_t *)0 ?
		snd_custom_table : snd_a440_table;
	struct compiler compiler;
	uint8_t *program = snd_programs[voice];
	unsigned int i;

	compiler.cursor = program;
	compiler.limit = program + SND_PROGRAM_BYTES;
	compiler.last_atten = 0xFFu;
	compiler.run = 0u;
	compiler.full = 0u;

	emit(&compiler, SND_P_PERIOD);
	emit(&compiler, (uint8_t)table[note]);
	emit(&compiler, (uint8_t)(table[note] >> 8));
	for (i = 0u; i < instrument->attack_ticks; ++i) {
		step(&compiler, ramp(0u, instrument->peak_level,
				     (uint16_t)(i + 1u),
				     instrument->attack_ticks));
	}
	if (instrument->attack_ticks == 0u) {
		step(&compiler, instrument->peak_level);
	}
	for (i = 0u; i < instrument->decay_ticks; ++i) {
		step(&compiler, ramp(instrument->peak_level,
				     instrument->sustain_level,
				     (uint16_t)(i + 1u),
				     instrument->decay_ticks));
	}
	compiler.run += gate_ticks;
	for (i = 0u; i < instrument->release_ticks; ++i) {
		step(&compiler, ramp(instrument->sustain_level, 0u,
				     (uint16_t)(i + 1u),
				     instrument->release_ticks));
	}
	step(&compiler, 0u);      /* the guaranteed final silence      */
	emit(&compiler, SND_P_END);
	return compiler.full != 0u ? -KOS_ENOSPC : 0;
}

static int compile_release(uint8_t voice, uint8_t from_level,
			   uint8_t release_ticks)
{
	struct compiler compiler;
	uint8_t *program = snd_programs[voice];
	unsigned int i;

	compiler.cursor = program;
	compiler.limit = program + SND_PROGRAM_BYTES;
	compiler.last_atten = 0xFFu;
	compiler.run = 0u;
	compiler.full = 0u;
	for (i = 0u; i < release_ticks; ++i) {
		step(&compiler, ramp(from_level, 0u, (uint16_t)(i + 1u),
				     release_ticks));
	}
	step(&compiler, 0u);
	emit(&compiler, SND_P_END);
	return compiler.full != 0u ? -KOS_ENOSPC : 0;
}

/* --- the manual API ---------------------------------------------------- */

static int tone_voice_usable(snd_voice_t voice)
{
	uint8_t chip;

	if (snd_handle == 0) {
		return -KOS_ENODEV;
	}
	if (voice >= SND_VOICE_COUNT || (voice & 3u) == 3u) {
		return -KOS_EINVAL;
	}
	if ((snd_reserved_voices & (uint16_t)(1u << voice)) != 0u) {
		return -KOS_EBUSY;
	}
	/* Noise rate 3 couples the chip's tone 2: a tone-2 note while
	 * that noise is configured is refused (and the reverse). */
	if ((voice & 3u) == 2u) {
		chip = voice >> 2;
		if ((snd_desired_noise[chip] & 3u) == 3u) {
			return -KOS_EBUSY;
		}
	}
	return 0;
}

int snd_define_instrument(uint8_t id,
			  const struct snd_instrument_simple *instrument)
{
	if (id >= SND_INSTRUMENTS || instrument ==
	    (const struct snd_instrument_simple *)0) {
		return -KOS_EINVAL;
	}
	if ((instrument->kind != SND_INSTR_TONE &&
	     instrument->kind != SND_INSTR_NOISE) ||
	    instrument->noise_rate > 3u ||
	    instrument->noise_mode > SND_NOISE_WHITE ||
	    instrument->sustain_level > instrument->peak_level) {
		return -KOS_EINVAL;
	}
	snd_instruments[id] = *instrument;
	snd_instrument_defined[id] = 1u;
	return 0;
}

static int note_start(snd_voice_t voice, snd_note_t note,
		      uint16_t gate_ticks, uint8_t instrument)
{
	const struct snd_instrument_simple *definition;
	int result;

	result = tone_voice_usable(voice);
	if (result < 0) {
		return result;
	}
	if (note >= SND_NOTE_COUNT || instrument >= SND_INSTRUMENTS ||
	    snd_instrument_defined[instrument] == 0u) {
		return -KOS_EINVAL;
	}
	definition = &snd_instruments[instrument];
	if (definition->kind != SND_INSTR_TONE) {
		return -KOS_EINVAL;
	}
	/* Idle the voice during the compile so the engine never reads a
	 * half-written program. */
	snd_voice_clear(voice);
	result = compile_note(voice, note, gate_ticks, definition);
	if (result < 0) {
		return result;
	}
	snd_releasing[voice] = 0u;
	snd_voice_instrument[voice] = instrument;
	snd_voice_install(voice);
	return 0;
}

int snd_note_on(snd_voice_t voice, snd_note_t note, uint8_t instrument)
{
	if (instrument >= SND_INSTRUMENTS ||
	    snd_instrument_defined[instrument] == 0u) {
		return -KOS_EINVAL;
	}
	return note_start(voice, note, snd_instruments[instrument].gate_ticks,
			  instrument);
}

int snd_note_for(snd_voice_t voice, snd_note_t note,
		 uint16_t duration_ticks, uint8_t instrument)
{
	return note_start(voice, note, duration_ticks, instrument);
}

int snd_note_off(snd_voice_t voice)
{
	uint8_t level;
	uint8_t instrument;
	int result;

	result = tone_voice_usable(voice);
	if (result < 0) {
		return result;
	}
	if (snd_voice_active[voice] == 0u || snd_releasing[voice] != 0u) {
		return -KOS_EINVAL;
	}
	level = snd_voice_level[voice];
	instrument = snd_voice_instrument[voice];
	/* Idle the voice during the compile, exactly as note_start does;
	 * the level was read first. */
	snd_voice_clear(voice);
	result = compile_release(voice, level,
				 snd_instruments[instrument].release_ticks);
	if (result < 0) {
		return result;
	}
	snd_releasing[voice] = 1u;
	snd_voice_install(voice);
	return 0;
}

int snd_voice_silence(snd_voice_t voice)
{
	if (voice >= SND_VOICE_COUNT) {
		return -KOS_EINVAL;
	}
	if (snd_handle == 0) {
		return -KOS_ENODEV;
	}
	snd_releasing[voice] = 0u;
	snd_voice_clear(voice);
	return 0;
}

int snd_set_noise(snd_voice_t noise_voice, uint8_t mode, uint8_t rate)
{
	uint8_t chip;
	uint8_t tone2;

	if (snd_handle == 0) {
		return -KOS_ENODEV;
	}
	if ((noise_voice & 3u) != 3u || noise_voice >= SND_VOICE_COUNT ||
	    mode > SND_NOISE_WHITE || rate > 3u) {
		return -KOS_EINVAL;
	}
	if ((snd_reserved_voices & (uint16_t)(1u << noise_voice)) != 0u) {
		return -KOS_EBUSY;
	}
	chip = noise_voice >> 2;
	tone2 = (uint8_t)(chip << 2) + 2u;
	if (rate == 3u &&
	    (snd_voice_active[tone2] != 0u ||
	     (snd_reserved_voices & (uint16_t)(1u << tone2)) != 0u)) {
		return -KOS_EBUSY;
	}
	snd_noise_apply(noise_voice, (uint8_t)((rate & 3u) |
					       (mode != 0u ? 4u : 0u)));
	return 0;
}
