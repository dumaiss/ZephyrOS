/* KonaOS libsnd: the card lifecycle (user/sdk/snd).
 *
 * snd_open is the only SOUND IPC the library makes at open: one
 * exclusive bind of `/dev/sound`, the type check, one sound_info and
 * the clock check against the built-in A440 table (or the application's
 * own table).  Every later call works the ports the record reported.
 * The close - or the process's exit, whose handle release resets the
 * card - puts the card back to silence.
 */

#include "kos.h"

#include "kos/sound.h"
#include "snd_private.h"

#define SND_A440_CLOCK_HZ 3579545ul

int snd_handle;
uint8_t snd_output_mode;
uint16_t snd_manual_rate;
const uint16_t *snd_custom_table;
uint16_t snd_reserved_voices;
uint16_t snd_active_rate;
unsigned int snd_overruns;

int snd_open(void)
{
	struct ksound_info info;
	unsigned char type = 0u;
	int handle;

	if (snd_handle != 0) {
		return -KOS_EBUSY;
	}
	handle = bind("/dev/sound", KOS_BIND_EXCLUSIVE, &type);
	if (handle < 0) {
		return handle;
	}
	snd_handle = handle;
	if (type != KOS_TYPE_SOUND) {
		snd_close();
		return -KOS_EINVAL;
	}
	if (sound_info(snd_handle, &info) < 0) {
		snd_close();
		return -KOS_EIO;
	}
	if (info.version != KSOUND_INFO_VERSION ||
	    info.psg_count < SND_CHIP_COUNT ||
	    (info.flags & KSOUND_CAP_DIRECT_IO) == 0u) {
		snd_close();
		return -KOS_ENOTSUP;
	}
	if (info.psg_clock_hz != SND_A440_CLOCK_HZ &&
	    snd_custom_table == (const uint16_t *)0) {
		snd_close();
		return -KOS_ENOTSUP;
	}
	/* The ports and the clock reach the assembly through the mirrored
	 * bytes from here on. */
	snd_load_ports(&info);
	snd_voice_init();
	snd_shadow_invalidate();
	snd_output_mode = SND_OUTPUT_DIRECT;
	snd_reserved_voices = 0u;
	if (snd_manual_rate == 0u) {
		snd_manual_rate = 50u;
	}
	snd_active_rate = snd_manual_rate;
	return 0;
}

void snd_close(void)
{
	unsigned char i;

	if (snd_handle == 0) {
		return;
	}
	/* Silence every voice before the release (the release resets the
	 * card whatever was left, but the silence is the library's own
	 * contract). */
	for (i = 0u; i < SND_VOICE_COUNT; ++i) {
		snd_voice_clear(i);
	}
	/* Close has no later application tick: consume the pending per-voice
	 * silences through the same shadowed write-through path here. */
	snd_engine(0u);
	/* A lingering ZVG's state and reservation die with the session
	 * (the card reset mutes it). */
	snd_output_flush();
	(void)sound_close(snd_handle);
	snd_handle = 0;
}

int snd_get_info(struct snd_info *out)
{
	struct ksound_info card;
	unsigned char i;

	if (snd_handle == 0 || out == (struct snd_info *)0) {
		return -KOS_EINVAL;
	}
	if (sound_info(snd_handle, &card) < 0) {
		return -KOS_EIO;
	}
	out->version = 1u;
	out->psg_count = card.psg_count;
	out->caps = card.flags;
	out->output_mode = snd_output_mode;
	out->tick_rate = snd_active_rate;
	out->manual_rate = snd_manual_rate;
	for (i = 0u; i < SND_CHIP_COUNT; ++i) {
		out->psg_ports[i] = card.psg_ports[i];
	}
	out->dac_port = card.dac_port;
	out->psg_clock_hz = card.psg_clock_hz;
	return 0;
}

int snd_set_output(uint8_t mode)
{
	if (snd_handle == 0) {
		return -KOS_ENODEV;
	}
	if (mode != SND_OUTPUT_DIRECT && mode != SND_OUTPUT_BATCH) {
		return -KOS_EINVAL;
	}
	snd_output_mode = mode;
	return 0;
}

int snd_set_rate(uint16_t ticks_per_sec)
{
	if (ticks_per_sec == 0u || ticks_per_sec > SND_MAX_TICK_RATE) {
		return -KOS_EINVAL;
	}
	snd_manual_rate = ticks_per_sec;
	snd_active_rate = ticks_per_sec;
	return 0;
}

uint16_t snd_tick_rate(void)
{
	return snd_active_rate;
}

int snd_timer_period(uint32_t *counts)
{
	uint32_t rate;

	if (counts == (uint32_t *)0) {
		return -KOS_EINVAL;
	}
	rate = snd_tick_rate();
	if (rate == 0u) {
		return -KOS_EINVAL;
	}
	/* round(39,062.5 / rate): (390625 + 5*rate) / (10*rate), kept in
	 * 32-bit arithmetic without a long multiply. */
	{
		uint32_t dividend = 390625ul +
			(uint32_t)(uint16_t)(rate * 5u);
		uint32_t divisor = (uint32_t)(uint16_t)(rate * 10u);

		*counts = dividend / divisor;
	}
	return 0;
}

int snd_set_note_table(const uint16_t *divisors)
{
	if (divisors != (const uint16_t *)0 && snd_handle != 0) {
		/* The table is checked at open; a running session keeps the
		 * clock it opened with. */
		return -KOS_EBUSY;
	}
	snd_custom_table = divisors;
	return 0;
}
