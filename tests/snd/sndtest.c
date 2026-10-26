/* libsnd's target outcome program (tests/snd): one mode per command-tail
 * word, replayed by tests/test_snd.py on the production image against
 * Python references built from the same rules.
 *
 *   sndtest tones     twelve tone voices, the divisors from the table
 *   sndtest adsr      one envelope tick by tick, then a release
 *   sndtest noise     modes and rates, the forced control write, the
 *                     tone-2 coupling refusals
 *   sndtest modes     the same chord in DIRECT and BATCH
 *   sndtest catchup   snd_tick(50): the cap and the overrun count
 *   sndtest close     silence + release, then an exit without snd_close
 *   sndtest timing    the budget workloads, marker-framed
 *   sndtest zvg FILE CHIP [batch] [step]  the ZVG player on the timer;
 *                                     `step`: unpaced, tick by tick
 *   sndtest zvgstall FILE CHIP        ZVG with a stalled reader
 *   sndtest zvgnotes FILE CHIP        manual notes against the ZVG chip
 *
 * Every tick the harness times is framed by the DAC marker (E4h, A5h),
 * which no song writes.
 */

#include "kos.h"
#include "kos/sound.h"
#include "kos/reader.h"
#include "kos/snd.h"
#include "kos/event.h"
#include "kos/timer.h"
#include "snd_private.h"

static struct snd_info info;

static void marker(void)
{
	sound_out(info.dac_port, 0xA5u);
}

static int open_card(void)
{
	if (snd_open() < 0) {
		printf("sndtest: open failed\r\n");
		return 1;
	}
	if (snd_get_info(&info) < 0) {
		printf("sndtest: info failed\r\n");
		return 1;
	}
	return 0;
}

/* The held chord: instant attack, full level, no release. */
static void define_chord(void)
{
	snd_instrument_simple_t chord = {
		.kind = SND_INSTR_TONE, .peak_level = 255, .gate_ticks = 200,
		.attack_ticks = 0, .decay_ticks = 0, .sustain_level = 255,
		.release_ticks = 0, .noise_mode = 0, .noise_rate = 0,
	};

	snd_define_instrument(0, &chord);
}

/* The twelve tone voices, chip-major (0-2, 4-6, 8-10, 12-14). */
static const snd_voice_t tone12[12] = {
	0u, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 10u, 12u, 13u, 14u,
};

static int mode_tones(void)
{
	static const snd_note_t notes[12] = {
		SND_C4, SND_E4, SND_G4, SND_C5, SND_E5, SND_G5,
		SND_C6, SND_E6, SND_G6, SND_C7, SND_E7, SND_G7,
	};
	unsigned char i;
	unsigned char ticks;

	if (open_card() != 0) return 1;
	define_chord();
	for (i = 0u; i < 12u; ++i) {
		if (snd_note_on(tone12[i], notes[i], 0u) < 0) {
			printf("sndtest: tones note %u refused\r\n", i);
			return 1;
		}
	}
	for (ticks = 0u; ticks < 3u; ++ticks) {
		marker();
		snd_tick(1u);
		marker();
	}
	snd_close();
	printf("sndtest: tones PASS\r\n");
	return 0;
}

/* The plucked note: attack 4, decay 3, sustain 120, release 3, peak
 * 200; tests/test_snd.py's reference reproduces this exact envelope. */
static void define_pluck(void)
{
	snd_instrument_simple_t pluck = {
		.kind = SND_INSTR_TONE, .peak_level = 200, .gate_ticks = 200,
		.attack_ticks = 4, .decay_ticks = 3, .sustain_level = 120,
		.release_ticks = 3, .noise_mode = 0, .noise_rate = 0,
	};

	snd_define_instrument(1, &pluck);
}

static int mode_adsr(void)
{
	unsigned char ticks;

	if (open_card() != 0) return 1;
	define_pluck();
	snd_note_on(0u, SND_C4, 1u);
	for (ticks = 0u; ticks < 10u; ++ticks) {
		marker();
		snd_tick(1u);
		marker();
	}
	snd_note_off(0u);
	for (ticks = 0u; ticks < 6u; ++ticks) {
		marker();
		snd_tick(1u);
		marker();
	}
	snd_close();
	printf("sndtest: adsr PASS\r\n");
	return 0;
}

static int mode_noise(void)
{
	int result;

	if (open_card() != 0) return 1;
	define_chord();
	/* Periodic noise, rate 1: the first tick writes the control byte
	 * (E4h) even though the shadow's initial state is unknown. */
	snd_set_noise(3u, SND_NOISE_PERIODIC, 1u);
	marker();
	snd_tick(1u);
	marker();
	/* The same value again: the force flag makes the second tick
	 * rewrite the control byte (the phase-reset rule). */
	snd_set_noise(3u, SND_NOISE_PERIODIC, 1u);
	marker();
	snd_tick(1u);
	marker();
	/* White noise, rate 3 needs the chip's tone 2: occupied -> busy. */
	snd_note_on(2u, SND_C4, 0u);
	result = snd_set_noise(3u, SND_NOISE_WHITE, 3u);
	printf("sndtest: noise busy %d\r\n", result);
	snd_voice_silence(2u);
	/* Tone 2 free: rate 3 is accepted and writes E7h. */
	result = snd_set_noise(3u, SND_NOISE_WHITE, 3u);
	printf("sndtest: noise rate3 %d\r\n", result);
	marker();
	snd_tick(1u);
	marker();
	/* The reverse coupling: a tone-2 note while rate 3 is active. */
	result = snd_note_on(2u, SND_C4, 0u);
	printf("sndtest: noise tone2 %d\r\n", result);
	snd_close();
	printf("sndtest: noise PASS\r\n");
	return 0;
}

static int mode_modes(void)
{
	unsigned char i;
	unsigned char ticks;

	if (open_card() != 0) return 1;
	define_chord();
	/* DIRECT first, then the same chord through BATCH. */
	for (i = 0u; i < 2u; ++i) {
		snd_voice_t voice;
		for (voice = 0u; voice < 3u; ++voice) {
			snd_note_on(voice, (snd_note_t)(SND_C4 + voice), 0u);
		}
		for (ticks = 0u; ticks < 3u; ++ticks) {
			marker();
			snd_tick(1u);
			marker();
		}
		snd_set_output(SND_OUTPUT_BATCH);
		for (voice = 0u; voice < 16u; ++voice) {
			snd_voice_silence(voice);
		}
		marker();
		snd_tick(1u);
		marker();
	}
	snd_close();
	printf("sndtest: modes PASS\r\n");
	return 0;
}

static int mode_catchup(void)
{
	if (open_card() != 0) return 1;
	marker();
	snd_tick(50u);
	marker();
	printf("sndtest: catchup overruns %u\r\n", snd_overruns);
	snd_close();
	printf("sndtest: catchup PASS\r\n");
	return 0;
}

static int mode_close(void)
{
	unsigned char voice;

	if (open_card() != 0) return 1;
	define_chord();
	for (voice = 0u; voice < 3u; ++voice) {
		snd_note_on(voice, (snd_note_t)(SND_C4 + voice), 0u);
	}
	marker();
	snd_tick(1u);
	marker();
	snd_close();
	printf("sndtest: close PASS\r\n");
	/* Then an exit without snd_close: the kernel's release resets the
	 * card, and the replay asserts the trailing silence. */
	if (open_card() != 0) return 1;
	snd_note_on(0u, SND_C4, 0u);
	marker();
	snd_tick(1u);
	marker();
	printf("sndtest: exit PASS\r\n");
	return 0;
}

extern void sndtest_put_primitives(void);

static int mode_primitive(void)
{
	if (open_card() != 0) return 1;
	/* Establish a valid silent shadow before the measured primitive calls. */
	snd_tick(0u);
	marker();
	sndtest_put_primitives();
	marker();
	snd_close();
	printf("sndtest: primitive PASS\r\n");
	return 0;
}

static int mode_stagger(int batch)
{
	snd_instrument_simple_t ramp = {
		.kind = SND_INSTR_TONE, .peak_level = 255,
		.gate_ticks = 20, .attack_ticks = 8, .decay_ticks = 7,
		.sustain_level = 96, .release_ticks = 8,
		.noise_mode = 0, .noise_rate = 0,
	};
	unsigned int tick;
	unsigned char i;

	if (open_card() != 0) return 1;
	if (batch != 0) snd_set_output(SND_OUTPUT_BATCH);
	snd_define_instrument(2u, &ramp);
	/* Reverse publication makes the trace distinguish a list walk from the
	 * descriptor engine's fixed logical-voice order. */
	for (i = 12u; i != 0u; --i) {
		snd_note_for(tone12[i - 1u], (snd_note_t)(SND_C4 + i - 1u),
			     (uint16_t)(20u + (unsigned int)i * 3u), 2u);
	}
	for (tick = 0u; tick < 200u; ++tick) {
		marker();
		snd_tick(1u);
		marker();
	}
	snd_close();
	printf("sndtest: stagger PASS\r\n");
	return 0;
}

static int mode_coalesce(void)
{
	snd_instrument_simple_t ramp = {
		.kind = SND_INSTR_TONE, .peak_level = 255,
		.gate_ticks = 20, .attack_ticks = 8, .decay_ticks = 0,
		.sustain_level = 255, .release_ticks = 0,
		.noise_mode = 0, .noise_rate = 0,
	};
	if (open_card() != 0) return 1;
	snd_define_instrument(2u, &ramp);
	/* Validate the initial shadow separately, then measure one coalesced call. */
	snd_tick(0u);
	snd_note_on(0u, SND_C4, 2u);
	marker();
	snd_tick(5u);
	marker();
	snd_close();
	printf("sndtest: coalesce PASS\r\n");
	return 0;
}

static int mode_timing(void)
{
	unsigned char i;
	unsigned char ticks;

	if (open_card() != 0) return 1;
	/* 1: nothing due (12 idle programs). */
	define_chord();
	for (i = 0u; i < 12u; ++i) {
		snd_note_on(tone12[i], (snd_note_t)(SND_C4 + i), 0u);
	}
	marker();
	snd_tick(1u);
	marker();
	/* The notes ended: 12 idle programs, several idle ticks. */
	for (ticks = 0u; ticks < 6u; ++ticks) {
		marker();
		snd_tick(1u);
		marker();
	}
	/* 2: twelve voices each changing attenuation (an 8-tick attack). */
	{
		snd_instrument_simple_t ramp = {
			.kind = SND_INSTR_TONE, .peak_level = 255,
			.gate_ticks = 200, .attack_ticks = 8, .decay_ticks = 0,
			.sustain_level = 255, .release_ticks = 0,
			.noise_mode = 0, .noise_rate = 0,
		};
		snd_define_instrument(2, &ramp);
	}
	for (i = 0u; i < 12u; ++i) {
		snd_note_on(tone12[i], (snd_note_t)(SND_C4 + i), 2u);
	}
	for (ticks = 0u; ticks < 8u; ++ticks) {
		marker();
		snd_tick(1u);
		marker();
	}
	/* 3: one changed byte per tick (a single voice's attack). */
	for (i = 0u; i < 16u; ++i) {
		snd_voice_silence((snd_voice_t)i);
	}
	snd_note_on(0u, SND_C4, 2u);
	for (ticks = 0u; ticks < 8u; ++ticks) {
		marker();
		snd_tick(1u);
		marker();
	}
	/* 4: a full refresh of the four chips (the shadow was invalidated
	 * by the BATCH flip below; here: close/reopen invalidates it). */
	snd_close();
	if (open_card() != 0) return 1;
	define_chord();
	snd_note_on(0u, SND_C4, 0u);
	snd_note_on(4u, SND_C4, 0u);
	snd_note_on(8u, SND_C4, 0u);
	snd_note_on(12u, SND_C4, 0u);
	marker();
	snd_tick(1u);
	marker();
	/* 5: the same through BATCH. */
	snd_set_output(SND_OUTPUT_BATCH);
	snd_voice_silence(0u);
	snd_voice_silence(4u);
	snd_voice_silence(8u);
	snd_voice_silence(12u);
	snd_note_on(0u, SND_C4, 0u);
	snd_note_on(4u, SND_C4, 0u);
	snd_note_on(8u, SND_C4, 0u);
	snd_note_on(12u, SND_C4, 0u);
	marker();
	snd_tick(1u);
	marker();
	/* 6: a snd_note_for compile, marker-framed. */
	snd_voice_silence(0u);
	marker();
	snd_note_for(0u, SND_A4, 12u, 0u);
	marker();
	snd_close();
	printf("sndtest: timing PASS\r\n");
	return 0;
}

/* ---- zvg: the pre-rendered stream player, one marker-framed tick ----
 * at a time so the harness can measure the wait-only tick budget and
 * the per-written-byte budget.  `zvgstall` wraps the file reader in a
 * fake that stalls at a chosen offset to prove the underrun contract:
 * counted underruns, a muted chip, no partial command, exact resumption.
 */

static unsigned int zvg_chip_from(const char *text)
{
	unsigned int chip = 0u;

	while (*text >= '0' && *text <= '9') {
		chip = chip * 10u + (unsigned int)(*text - '0');
		++text;
	}
	return chip;
}

struct zvg_stall {
	kos_reader_t inner;
	unsigned int at;
	unsigned int left;
	unsigned int served;
};

static int zvg_stall_read_at(void *context, uint32_t offset, void *dst,
			     uint16_t requested)
{
	struct zvg_stall *stall = (struct zvg_stall *)context;

	if (stall->left != 0u &&
	    (uint32_t)(offset + (uint32_t)requested) > stall->at) {
		--stall->left;
		++stall->served;
		return 0; /* deliver nothing: the ring starves */
	}
	return stall->inner.read_at(stall->inner.context, offset, dst,
				    requested);
}

static int zvg_play_loop(void)
{
	struct snd_zvg_status status;
	unsigned int ticks = 0u;
	unsigned int service_rc = 0u;

	for (;;) {
		snd_zvg_get_status(&status);
		if (status.state == SND_ZVG_DONE ||
		    status.state == SND_ZVG_ERROR || ticks > 30000u) {
			break;
		}
		marker();
		snd_tick(1u);
		marker();
		service_rc = snd_service();
		++ticks;
	}
	snd_zvg_get_status(&status);
	printf("sndtest: zvg PASS state %u ticks %u service %u writes %lu "
	       "underruns %u bytes %lu\r\n",
	       (unsigned int)status.state, ticks, service_rc,
	       status.writes, (unsigned int)status.underruns, status.bytes);
	if (status.state == SND_ZVG_ERROR) {
		printf("sndtest: zvg opcode %02x at offset %lu\r\n",
		       (unsigned int)snd_zvg_error_op, status.error_offset);
	}
	return 0;
}

/* The paced player: the timer at snd_timer_period, snd_tick(count) from
 * the handler, snd_service and idle() in the foreground; a key stops. */
static volatile unsigned char zvg_playing;
static volatile unsigned int zvg_events;

static void zvg_on_timer(unsigned int count, unsigned int missed)
{
	(void)missed;
	if (count != 0u && zvg_playing != 0u) {
		(void)snd_tick((uint16_t)count);
		zvg_events = (unsigned int)(zvg_events + count);
	}
}

static int zvg_play_timed(void)
{
	struct snd_zvg_status status;
	uint32_t period;
	int timer;
	int result;

	idle(); /* prime idle()'s timer probe before the events start */
	timer = timer_open();
	if (timer < 0) {
		printf("sndtest: zvg timer %d\r\n", timer);
		return 1;
	}
	result = snd_timer_period(&period);
	if (result == 0) {
		result = notify(timer, zvg_on_timer);
	}
	if (result == 0) {
		zvg_playing = 1u;
		result = timer_event(timer, (unsigned int)period);
	}
	if (result < 0) {
		zvg_playing = 0u;
		printf("sndtest: zvg timer %d\r\n", result);
		(void)notify(timer, (kos_event_handler_t)0);
		(void)unbind((unsigned char)timer);
		return 1;
	}
	while (zvg_playing != 0u) {
		(void)snd_service();
		(void)snd_zvg_get_status(&status);
		if (status.state != SND_ZVG_PLAYING &&
		    status.state != SND_ZVG_UNDERRUN) {
			break;
		}
		if (console_poll() > 0) {
			char key;

			(void)console_read(&key, 1u);
			break;
		}
		idle();
	}
	zvg_playing = 0u;
	(void)timer_event(timer, 0u);
	(void)notify(timer, (kos_event_handler_t)0);
	(void)unbind((unsigned char)timer);
	(void)snd_zvg_get_status(&status);
	printf("sndtest: zvg PASS state %u events %u ticks %lu writes %lu "
	       "underruns %u bytes %lu\r\n",
	       (unsigned int)status.state, zvg_events,
	       (unsigned long)status.ticks, status.writes,
	       (unsigned int)status.underruns, status.bytes);
	return 0;
}

static int mode_zvg(const char *path, unsigned int chip, unsigned int batch,
		    unsigned int stall, unsigned int step)
{
	kos_reader_t reader;
	kos_reader_t wrapped;
	kos_handle_source_t source;
	struct zvg_stall fake;
	unsigned int handle;
	int result;

	if (open_card() != 0) return 1;
	snd_set_output(batch ? SND_OUTPUT_BATCH : SND_OUTPUT_DIRECT);
	handle = open(path, KFS_O_READ);
	if ((int)handle < 0) {
		printf("sndtest: zvg open %d\r\n", (int)handle);
		snd_close();
		return 1;
	}
	source.handle = handle;
	result = kos_reader_from_handle(&reader, &source);
	if (result != 0) {
		printf("sndtest: zvg reader %d\r\n", result);
		snd_close();
		return 1;
	}
	if (stall != 0u) {
		fake.inner = reader;
		fake.at = 20u;   /* inside the first service chunk */
		fake.left = 2u;  /* two stalled calls */
		fake.served = 0u;
		wrapped.context = &fake;
		wrapped.read_at = zvg_stall_read_at;
		wrapped.size = reader.size;
		reader = wrapped;
	}
	result = snd_zvg_open(&reader, (uint8_t)chip);
	if (result != 0) {
		printf("sndtest: zvg refused %d\r\n", result);
		snd_close();
		return 1;
	}
	result = snd_zvg_prepare();
	if (result != 0) {
		printf("sndtest: zvg prepare %d\r\n", result);
		snd_close();
		return 1;
	}
	result = snd_zvg_play();
	if (result != 0) {
		printf("sndtest: zvg play %d\r\n", result);
		snd_close();
		return 1;
	}
	if (step != 0u || stall != 0u) {
		(void)zvg_play_loop();
	} else {
		(void)zvg_play_timed();
	}
	if (stall != 0u) {
		printf("sndtest: zvgstall served %u\r\n", fake.served);
	}
	snd_zvg_stop();
	snd_close();
	return 0;
}

static int mode_zvg_notes(const char *path, unsigned int chip)
{
	kos_reader_t reader;
	kos_handle_source_t source;
	unsigned int other;
	unsigned int handle;
	int result;

	if (open_card() != 0) return 1;
	handle = open(path, KFS_O_READ);
	if ((int)handle < 0) {
		printf("sndtest: zvgnotes open %d\r\n", (int)handle);
		snd_close();
		return 1;
	}
	source.handle = handle;
	result = kos_reader_from_handle(&reader, &source);
	if (result != 0) {
		printf("sndtest: zvgnotes reader %d\r\n", result);
		snd_close();
		return 1;
	}
	result = snd_zvg_open(&reader, (uint8_t)chip);
	if (result != 0) {
		printf("sndtest: zvgnotes refused %d\r\n", result);
		snd_close();
		return 1;
	}
	other = (chip + 1u) & 3u;
	define_chord();
	/* The ZVG chip's four voices are reserved; a free chip's are not. */
	result = snd_note_on((snd_voice_t)(chip * 4u), SND_C4, 0u);
	printf("sndtest: zvgnotes busy %d\r\n", result);
	result = snd_note_on((snd_voice_t)(other * 4u), SND_C4, 0u);
	printf("sndtest: zvgnotes other %d\r\n", result);
	snd_zvg_close();
	snd_close();
	return 0;
}

/* ---- ztr: the song player, one marker-framed tick at a time ----
 * (the harness splits the writes by the marker).  `ztrstall` wraps
 * the reader in the same fake as zvgstall: the queue drains, the
 * engine underruns muted, and service resumes it at the due row.
 * `ztrload` opens and drains the prepared queue in the foreground,
 * printing a per-order checksum; `ztrdump` prints the engine's channel
 * state after every tick, in the oracle's text (a debug export, only
 * here); `ztrpause` frames pause/resume/stop for the harness.
 */

struct ztr_stall {
	kos_reader_t inner;
	unsigned int at;
	unsigned int left;
	unsigned int served;
};

static int ztr_stall_read_at(void *context, uint32_t offset, void *dst,
			     uint16_t requested)
{
	struct ztr_stall *stall = (struct ztr_stall *)context;

	if (stall->left != 0u &&
	    (uint32_t)(offset + (uint32_t)requested) > stall->at) {
		--stall->left;
		++stall->served;
		return 0; /* deliver nothing: the queue drains */
	}
	return stall->inner.read_at(stall->inner.context, offset, dst,
				    requested);
}

static int ztr_open_source(const char *path, kos_reader_t *reader,
			   struct ztr_stall *stall)
{
	/* The handle adapter borrows this: it must outlive the reader,
	 * which lives until snd_ztr_close. */
	static kos_handle_source_t source;
	unsigned int handle;

	handle = open(path, KFS_O_READ);
	if ((int)handle < 0) {
		return (int)handle;
	}
	source.handle = handle;
	if (kos_reader_from_handle(reader, &source) != 0) {
		return -KOS_EIO;
	}
	if (stall != (struct ztr_stall *)0) {
		kos_reader_t wrapped;

		stall->inner = *reader;
		wrapped.context = stall;
		wrapped.read_at = ztr_stall_read_at;
		wrapped.size = reader->size;
		*reader = wrapped;
	}
	return 0;
}

static void ztr_play_loop(void)
{
	struct snd_ztr_status status;
	unsigned int ticks = 0u;

	for (;;) {
		(void)snd_ztr_get_status(&status);
		if (status.state == SND_ZTR_DONE ||
		    status.state == SND_ZTR_ERROR || ticks > 30000u) {
			break;
		}
		marker();
		(void)snd_tick(1u);
		marker();
		(void)snd_service();
		++ticks;
	}
	(void)snd_ztr_get_status(&status);
	printf("sndtest: ztr PASS state %u ticks %u underruns %u "
	       "order %u row %u\r\n",
	       (unsigned int)status.state, ticks,
	       (unsigned int)status.underruns,
	       (unsigned int)status.order, (unsigned int)status.row);
}

/* The stall mode: prepare, then stall every service call after the
 * queue has drained, so the engine underruns exactly once and resumes
 * at the due row when the source recovers. */
static int mode_ztr(const char *path, unsigned int batch, unsigned int step,
		    unsigned int stall, unsigned int pause_mode)
{
	kos_reader_t reader;
	struct ztr_stall fake;
	struct snd_ztr_status status;
	unsigned int ticks = 0u;
	int result;

	(void)step;
	if (open_card() != 0) return 1;
	snd_set_output(batch ? SND_OUTPUT_BATCH : SND_OUTPUT_DIRECT);
	fake.at = 0u;
	fake.left = 0u;
	fake.served = 0u;
	fake.inner.context = (void *)0;
	fake.inner.read_at = (kos_read_at_fn)0;
	fake.inner.size = 0u;
	result = ztr_open_source(path, &reader, stall ? &fake : 0);
	if (result < 0) {
		printf("sndtest: ztr open %d\r\n", result);
		snd_close();
		return 1;
	}
	result = snd_ztr_open(&reader);
	if (result != 0) {
		printf("sndtest: ztr refused %d\r\n", result);
		snd_close();
		return 1;
	}
	result = snd_ztr_prepare();
	if (result != 0 && result != SND_NEED_MORE) {
		printf("sndtest: ztr prepare %d\r\n", result);
		snd_ztr_close();
		snd_close();
		return 1;
	}
	/* The stall engages after the start window: the first reads the
	 * window itself needs are served, then two service calls fail. */
	if (stall != 0u) {
		fake.at = 120u;
		fake.left = 2u;
	}
	result = snd_ztr_play();
	if (result != 0) {
		printf("sndtest: ztr play %d\r\n", result);
		snd_ztr_close();
		snd_close();
		return 1;
	}
	if (pause_mode != 0u) {
		/* four framed ticks, pause, two silent framed ticks,
		 * resume, four framed ticks, stop */
		for (ticks = 0u; ticks < 4u; ++ticks) {
			marker();
			(void)snd_tick(1u);
			marker();
		}
		(void)snd_ztr_pause();
		for (ticks = 0u; ticks < 2u; ++ticks) {
			marker();
			(void)snd_tick(1u);
			marker();
		}
		(void)snd_ztr_resume();
		for (ticks = 0u; ticks < 4u; ++ticks) {
			marker();
			(void)snd_tick(1u);
			marker();
		}
		(void)snd_ztr_get_status(&status);
		printf("sndtest: ztr PASS state %u order %u row %u "
		       "tick %u\r\n",
		       (unsigned int)status.state,
		       (unsigned int)status.order,
		       (unsigned int)status.row,
		       (unsigned int)status.tick_in_row);
		snd_ztr_stop();
		snd_close();
		return 0;
	}
	ztr_play_loop();
	if (stall != 0u) {
		printf("sndtest: ztrstall served %u\r\n", fake.served);
	}
	snd_ztr_stop();
	snd_close();
	return 0;
}

static uint16_t ztr_checksum_byte(uint16_t sum, uint8_t byte)
{
	sum = (uint16_t)((sum << 1) | (sum >> 15));
	sum = (uint16_t)(sum + byte);
	return sum;
}

static int mode_ztrload(const char *path)
{
	kos_reader_t reader;
	int result;

	if (open_card() != 0) return 1;
	result = ztr_open_source(path, &reader, (struct ztr_stall *)0);
	if (result < 0) {
		printf("sndtest: ztrload open %d\r\n", result);
		snd_close();
		return 1;
	}
	result = snd_ztr_open(&reader);
	if (result != 0) {
		printf("sndtest: ztrload refused %d\r\n", result);
		snd_close();
		return 1;
	}
	result = snd_ztr_prepare();
	if (result != 0 && result != SND_NEED_MORE) {
		printf("sndtest: ztrload prepare %d\r\n", result);
		snd_ztr_close();
		snd_close();
		return 1;
	}
	/* Drain the queue in the foreground, service refilling it, and
	 * checksum every row record by order. */
	{
		uint16_t order = 0u;
		uint16_t sum = 0u;

		for (;;) {
			uint8_t head = snd_ztr_head;

			/* consume the resident rows */
			while (head != snd_ztr_tail) {
				uint16_t addr = snd_ztr_queue_addrs[head];
				const uint8_t *record =
					(const uint8_t *)addr;
				unsigned int byte;

				if (record[0] != (uint8_t)order) {
					printf("sndtest: ztrload order %u "
					       "checksum %04x\r\n",
					       (unsigned int)order,
					       (unsigned int)sum);
					order = record[0];
					sum = 0u;
				}
				for (byte = 0u; byte < SND_ZTR_ROW_BYTES;
				     ++byte) {
					sum = ztr_checksum_byte(sum,
								record[byte]);
				}
				++head;
				if (head == SND_ZTR_QUEUE_ROWS) {
					head = 0u;
				}
				snd_ztr_head = head;
			}
			/* produce the next rows; a pass that publishes
			 * nothing means the song is fully drained */
			{
				uint8_t before = snd_ztr_tail;

				(void)snd_service();
				if (snd_ztr_tail == before) {
					break;
				}
			}
		}
		printf("sndtest: ztrload order %u checksum %04x\r\n",
		       (unsigned int)order, (unsigned int)sum);
		printf("sndtest: ztrload PASS\r\n");
		snd_ztr_close();
		snd_close();
		return 0;
	}
}

static int mode_ztrdump(const char *path, unsigned int ticks)
{
	kos_reader_t reader;
	unsigned int tick;
	unsigned int channel;
	int result;

	if (open_card() != 0) return 1;
	result = ztr_open_source(path, &reader, (struct ztr_stall *)0);
	if (result < 0) {
		printf("sndtest: ztrdump open %d\r\n", result);
		snd_close();
		return 1;
	}
	result = snd_ztr_open(&reader);
	if (result != 0) {
		printf("sndtest: ztrdump refused %d\r\n", result);
		snd_close();
		return 1;
	}
	(void)snd_ztr_prepare();
	(void)snd_ztr_play();
	/* The debug export, single pass: the markers bracket each tick
	 * (the harness attributes the writes by them), the service keeps
	 * the queue resident, and the per-tick channel state is printed
	 * in the oracle's text (ztr_trace SONG TICKS STATE), so one boot
	 * diffs both the register writes and the state. */
	for (tick = 0u; tick < ticks; ++tick) {
		marker();
		(void)snd_tick(1u);
		marker();
		(void)snd_service();
		for (channel = 0u; channel < SND_ZTR_CHANNELS; ++channel) {
			uint8_t *core = (uint8_t *)
				(uint16_t)((uint16_t)snd_ztr_core_page << 8);
			uint8_t base = (uint8_t)(channel << 4);
			uint16_t pitch = (uint16_t)core[base + ZC_PITCH_LO] |
				((uint16_t)core[base + ZC_PITCH_HI] << 8);
			unsigned int slot;
			uint16_t position[SND_ZTR_MACRO_SLOTS];

			for (slot = 0u; slot < SND_ZTR_MACRO_SLOTS;
			     ++slot) {
				uint8_t *page = (uint8_t *)(uint16_t)
					((uint16_t)snd_ztr_macro_page[slot] << 8);

				position[slot] =
					(uint16_t)page[base + ZM_POS_LO] |
					((uint16_t)page[base + ZM_POS_HI] << 8);
			}
			printf("tick %u: ch%u n=%02x v=%u p=%04x f=%02x "
			       "m=%04x %04x %04x %04x %04x\r\n",
			       tick, channel,
			       (unsigned int)core[base + ZC_NOTE],
			       (unsigned int)core[base + ZC_MACRO_VOL],
			       (unsigned int)pitch,
			       (unsigned int)core[base + ZC_FLAGS],
			       (unsigned int)position[0],
			       (unsigned int)position[1],
			       (unsigned int)position[2],
			       (unsigned int)position[3],
			       (unsigned int)position[4]);
		}
	}
	printf("sndtest: ztrdump PASS\r\n");
	snd_ztr_stop();
	snd_close();
	return 0;
}

static int mode_ztr_notes(const char *path)
{
	kos_reader_t reader;
	unsigned int voice;
	int result;
	int in_mask;
	int out_mask;

	if (open_card() != 0) return 1;
	result = ztr_open_source(path, &reader, (struct ztr_stall *)0);
	if (result < 0) {
		printf("sndtest: ztrnotes open %d\r\n", result);
		snd_close();
		return 1;
	}
	result = snd_ztr_open(&reader);
	if (result != 0) {
		printf("sndtest: ztrnotes refused %d\r\n", result);
		snd_close();
		return 1;
	}
	define_chord();
	in_mask = -1;
	out_mask = -1;
	for (voice = 0u; voice < 16u; ++voice) {
		result = snd_note_on((snd_voice_t)voice, SND_C4, 0u);
		if ((snd_ztr_mask & (uint16_t)(1u << voice)) != 0u &&
		    in_mask == -1) {
			in_mask = result;
		}
		if ((snd_ztr_mask & (uint16_t)(1u << voice)) == 0u &&
		    out_mask == -1) {
			out_mask = result;
		}
	}
	printf("sndtest: ztrnotes busy %d\r\n", in_mask);
	printf("sndtest: ztrnotes other %d\r\n", out_mask);
	snd_ztr_close();
	snd_close();
	return 0;
}

static int same(const char *left, const char *right)
{
	while (*left == *right) {
		if (*left == '\0') return 1;
		++left;
		++right;
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argv == (char **)0 || argc < 1) {
		console_puts("sndtest: no mode\r\n");
		return 1;
	}
	if (same(argv[0], "zvg") != 0 || same(argv[0], "zvgstall") != 0) {
		unsigned int stall = same(argv[0], "zvgstall") != 0;
		unsigned int batch = 0u;
		unsigned int step = 0u;
		int i;

		if (argc < 3 || argc > 5 || (stall != 0u && argc != 3)) {
			console_puts("sndtest: usage: sndtest zvg FILE CHIP "
				     "[batch] [step]\r\n");
			return 1;
		}
		for (i = 3; i < argc; ++i) {
			if (same(argv[i], "batch") != 0) {
				batch = 1u;
			} else if (same(argv[i], "step") != 0) {
				step = 1u;
			} else {
				console_puts("sndtest: zvg: batch or step\r\n");
				return 1;
			}
		}
		return mode_zvg(argv[1], zvg_chip_from(argv[2]), batch,
				stall, step);
	}
	if (same(argv[0], "zvgnotes") != 0 && argc == 3) {
		return mode_zvg_notes(argv[1], zvg_chip_from(argv[2]));
	}
	if (same(argv[0], "ztr") != 0 || same(argv[0], "ztrstall") != 0) {
		unsigned int stall = same(argv[0], "ztrstall") != 0;
		unsigned int batch = 0u;
		unsigned int step = 0u;
		int i;

		if (argc < 2 || argc > 4) {
			console_puts("sndtest: usage: sndtest ztr FILE "
				     "[batch] [step]\r\n");
			return 1;
		}
		for (i = 2; i < argc; ++i) {
			if (same(argv[i], "batch") != 0) {
				batch = 1u;
			} else if (same(argv[i], "step") != 0) {
				step = 1u;
			} else {
				console_puts("sndtest: ztr: batch or step\r\n");
				return 1;
			}
		}
		return mode_ztr(argv[1], batch, step, stall, 0u);
	}
	if (same(argv[0], "ztrpause") != 0 && argc >= 2 && argc <= 3) {
		unsigned int batch = 0u;

		if (argc == 3 && same(argv[2], "batch") != 0) {
			batch = 1u;
		} else if (argc == 3) {
			return 1;
		}
		return mode_ztr(argv[1], batch, 1u, 0u, 1u);
	}
	if (same(argv[0], "ztrload") != 0 && argc == 2) {
		return mode_ztrload(argv[1]);
	}
	if (same(argv[0], "ztrdump") != 0 && argc == 3) {
		return mode_ztrdump(argv[1], zvg_chip_from(argv[2]));
	}
	if (same(argv[0], "ztrnotes") != 0 && argc == 2) {
		return mode_ztr_notes(argv[1]);
	}
	if (same(argv[0], "stagger") != 0 && argc <= 2) {
		if (argc == 2 && same(argv[1], "batch") == 0) return 1;
		return mode_stagger(argc == 2);
	}
	if (argc != 1) {
		console_puts("sndtest: usage: sndtest tones|adsr|noise|modes|"
			     "catchup|close|timing|primitive|stagger [batch]|zvg FILE CHIP [batch]\r\n");
		return 1;
	}
	if (same(argv[0], "tones") != 0) return mode_tones();
	if (same(argv[0], "adsr") != 0) return mode_adsr();
	if (same(argv[0], "noise") != 0) return mode_noise();
	if (same(argv[0], "modes") != 0) return mode_modes();
	if (same(argv[0], "catchup") != 0) return mode_catchup();
	if (same(argv[0], "coalesce") != 0) return mode_coalesce();
	if (same(argv[0], "close") != 0) return mode_close();
	if (same(argv[0], "primitive") != 0) return mode_primitive();
	if (same(argv[0], "timing") != 0) return mode_timing();
	console_puts("sndtest: unknown mode\r\n");
	return 1;
}
