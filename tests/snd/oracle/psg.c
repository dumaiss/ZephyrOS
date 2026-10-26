#include "sntracker.h"
#include "kos/sound.h"

#define PSG_BATCH_WRITES      48u

#define DIRTY_PITCH           0x01u
#define DIRTY_VOLUME          0x02u
#define DIRTY_NOISE           0x04u
#define DIRTY_PHASE           0x08u

#define CHANNEL_ACTIVE        0x01u
#define CHANNEL_NOTATION_ARP  0x08u

static struct ksound_write psg_batch[PSG_BATCH_WRITES];
static unsigned char psg_batch_count;
static int sound_handle;

int psg_open(void)
{
	int result = sound_open(1u);

	if (result < 0)
		return result;
	sound_handle = result;
	return 0;
}

void psg_close(void)
{
	if (sound_handle > 0) {
		(void)sound_close(sound_handle);
		sound_handle = 0;
	}
}

static void queue_write(unsigned char target, unsigned char value)
{
	if (psg_batch_count >= PSG_BATCH_WRITES) {
		player_psg_error = 1u;
		player_playing = 0u;
		return;
	}
	psg_batch[psg_batch_count].target = target;
	psg_batch[psg_batch_count].value = value;
	++psg_batch_count;
}

static void flush_writes(void)
{
	int result;

	if (psg_batch_count == 0u || player_psg_error != 0u)
		return;
	result = sound_write(sound_handle, psg_batch, psg_batch_count);
	if (result != (int)psg_batch_count) {
		player_psg_error = 1u;
		player_playing = 0u;
		return;
	}
	player_psg_writes = (unsigned int)(player_psg_writes + psg_batch_count);
}

int psg_mute_all(void)
{
	int result;

	if (sound_handle <= 0)
		return -KOS_ENODEV;
	result = sound_reset(sound_handle);
	if (result < 0) {
		player_psg_error = 1u;
		player_playing = 0u;
	}
	return result;
}

static unsigned char effective_note(const struct tracker_channel *channel)
{
	int note;
	if ((channel->arpeggio_flags & 1u) != 0u)
		note = (signed char)(channel->arpeggio_value & 0xFF);
	else
		note = (int)channel->note + (signed char)(channel->arpeggio_value & 0xFF);
	if (note < 0) note = 0;
	if (channel->effect == EFFECT_ARPEGGIO) {
		if (channel->effect_phase == 1u)
			note += channel->effect_parameter >> 4;
		else if (channel->effect_phase == 2u)
			note += channel->effect_parameter & 0x0Fu;
	}
	if ((channel->flags & CHANNEL_NOTATION_ARP) != 0u) {
		if (channel->notation_phase == 1u)
			note += channel->notation_arpeggio >> 4;
		else if (channel->notation_phase == 2u)
			note += channel->notation_arpeggio & 0x0Fu;
	}
	if (note >= (int)ZTR_NOTE_COUNT) return ZTR_NOTE_COUNT - 1u;
	return (unsigned char)note;
}

static unsigned int fine_pitch(unsigned int period, int pitch)
{
	unsigned int magnitude;
	unsigned int delta;
	if (pitch > 255 || pitch < -255 || pitch == 0) return period;
	magnitude = pitch < 0 ? (unsigned int)-pitch : (unsigned int)pitch;
	if (magnitude > 63u) magnitude = 63u;
	delta = (unsigned int)((period * magnitude) >> 11);
	if (pitch > 0) return delta >= period ? 1u : (unsigned int)(period - delta);
	period = (unsigned int)(period + delta);
	return period > 1023u ? 1023u : period;
}

static void render_tone(unsigned char number, struct tracker_channel *channel)
{
	unsigned char voice = number & 3u;
	unsigned char target = number >> 2;
	unsigned char note = effective_note(channel);
	unsigned int period = ztr_u16(ztr_song.note_table + (unsigned int)note * 2u);
	period = fine_pitch(period, channel->pitch_value);
	if (period != channel->last_period) {
		queue_write(target, (unsigned char)(0x80u | (voice << 5) |
						(period & 0x0Fu)));
		queue_write(target, (unsigned char)((period >> 4) & 0x3Fu));
		channel->last_period = period;
	}
	channel->dirty &= (unsigned char)~DIRTY_PITCH;
}

static void render_noise(unsigned char number, struct tracker_channel *channel)
{
	unsigned char target = number >> 2;
	unsigned char control = effective_note(channel) & 3u;
	if ((channel->duty_value & 1u) != 0u) control |= 4u;
	if (control != channel->last_noise ||
	    (channel->dirty & DIRTY_PHASE) != 0u) {
		queue_write(target, (unsigned char)(0xE0u | control));
		channel->last_noise = control;
	}
	channel->dirty &= (unsigned char)~(DIRTY_PITCH | DIRTY_NOISE | DIRTY_PHASE);
}

static void render_volume(unsigned char number, struct tracker_channel *channel)
{
	unsigned char loudness = 0u;
	unsigned char voice = number & 3u;
	unsigned char target = number >> 2;
	if ((channel->flags & CHANNEL_ACTIVE) != 0u) {
		int adjusted;
		loudness = (unsigned char)(((unsigned int)channel->base_volume *
					    channel->macro_volume) / 15u);
		adjusted = (int)loudness + channel->hairpin_offset;
		if (channel->hairpin_offset > 0 && adjusted > channel->hairpin_ceiling)
			adjusted = channel->hairpin_ceiling;
		if (adjusted < 0) adjusted = 0;
		if (adjusted > 15) adjusted = 15;
		loudness = (unsigned char)adjusted;
	}
	loudness = (unsigned char)(15u - loudness);
	if (loudness != channel->last_attenuation) {
		queue_write(target, (unsigned char)(0x90u | (voice << 5) |
						loudness));
		channel->last_attenuation = loudness;
	}
	channel->dirty &= (unsigned char)~DIRTY_VOLUME;
}

void psg_render_all(void)
{
	unsigned char number;

	psg_batch_count = 0u;
	for (number = 0u; number < ZTR_CHANNELS && player_playing != 0u; ++number) {
		struct tracker_channel *channel = &player_channels[number];
		if ((channel->flags & CHANNEL_ACTIVE) == 0u) {
			channel->dirty &= (unsigned char)~(DIRTY_PITCH | DIRTY_NOISE |
							   DIRTY_PHASE);
		}
		if ((number & 3u) == 3u) {
			if ((channel->dirty & (DIRTY_PITCH | DIRTY_NOISE | DIRTY_PHASE)) != 0u)
				render_noise(number, channel);
		} else if ((channel->dirty & DIRTY_PITCH) != 0u) {
			render_tone(number, channel);
		}
		if ((channel->dirty & DIRTY_VOLUME) != 0u) render_volume(number, channel);
	}
	flush_writes();
}
