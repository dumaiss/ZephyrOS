#include "sntracker.h"
#include "diag.h"

#define PSG_FIRST_PORT        0xE0u
#define PSG_COUNT             4u

#define DIRTY_PITCH           0x01u
#define DIRTY_VOLUME          0x02u
#define DIRTY_NOISE           0x04u
#define DIRTY_PHASE           0x08u

#define CHANNEL_ACTIVE        0x01u
#define CHANNEL_NOTATION_ARP  0x08u

static int write_port(unsigned char port, unsigned char value)
{
	struct kos_diag_io_out_args request;
	int result;
	request.port = port;
	request.value = value;
	result = diag_io_out(&request);
	if (result < 0) {
		player_psg_error = 1u;
		player_playing = 0u;
	} else {
		++player_psg_writes;
	}
	return result;
}

int psg_mute_all(void)
{
	unsigned char chip;
	int result = 0;
	for (chip = 0u; chip < PSG_COUNT; ++chip) {
		unsigned char value = 0x9Fu;
		for (;;) {
			int current = write_port((unsigned char)(PSG_FIRST_PORT + chip), value);
			if (current < 0) result = current;
			if (value == 0xFFu) break;
			value = (unsigned char)(value + 0x20u);
		}
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
	unsigned char port = (unsigned char)(PSG_FIRST_PORT + (number >> 2));
	unsigned char note = effective_note(channel);
	unsigned int period = ztr_u16(ztr_song.note_table + (unsigned int)note * 2u);
	period = fine_pitch(period, channel->pitch_value);
	if (period != channel->last_period) {
		(void)write_port(port, (unsigned char)(0x80u | (voice << 5) |
							(period & 0x0Fu)));
		(void)write_port(port, (unsigned char)((period >> 4) & 0x3Fu));
		channel->last_period = period;
	}
	channel->dirty &= (unsigned char)~DIRTY_PITCH;
}

static void render_noise(unsigned char number, struct tracker_channel *channel)
{
	unsigned char port = (unsigned char)(PSG_FIRST_PORT + (number >> 2));
	unsigned char control = effective_note(channel) & 3u;
	if ((channel->duty_value & 1u) != 0u) control |= 4u;
	if (control != channel->last_noise ||
	    (channel->dirty & DIRTY_PHASE) != 0u) {
		(void)write_port(port, (unsigned char)(0xE0u | control));
		channel->last_noise = control;
	}
	channel->dirty &= (unsigned char)~(DIRTY_PITCH | DIRTY_NOISE | DIRTY_PHASE);
}

static void render_volume(unsigned char number, struct tracker_channel *channel)
{
	unsigned char loudness = 0u;
	unsigned char voice = number & 3u;
	unsigned char port = (unsigned char)(PSG_FIRST_PORT + (number >> 2));
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
		(void)write_port(port, (unsigned char)(0x90u | (voice << 5) |
							loudness));
		channel->last_attenuation = loudness;
	}
	channel->dirty &= (unsigned char)~DIRTY_VOLUME;
}

void psg_render_all(void)
{
	unsigned char number;
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
}
