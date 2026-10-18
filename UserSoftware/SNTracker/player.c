#include "sntracker.h"

#define DIRTY_PITCH          0x01u
#define DIRTY_VOLUME         0x02u
#define DIRTY_NOISE          0x04u
#define DIRTY_PHASE          0x08u

#define CHANNEL_ACTIVE       0x01u
#define CHANNEL_RELEASED     0x02u
#define CHANNEL_LEGATO       0x04u
#define CHANNEL_NOTATION_ARP 0x08u

struct tracker_channel player_channels[ZTR_CHANNELS];
volatile unsigned char player_playing;
volatile unsigned char player_row_changed;
unsigned char player_order;
unsigned char player_row;
unsigned char player_minutes;
unsigned char player_seconds;
unsigned int player_psg_writes;
unsigned char player_psg_error;

static unsigned char tick_in_row;
static unsigned char elapsed_subticks;

static void clear_notation(struct tracker_channel *channel)
{
	channel->flags &= (unsigned char)~CHANNEL_NOTATION_ARP;
	channel->notation_arpeggio = 0u;
	channel->notation_phase = 0u;
	channel->hairpin_rate = 0;
	channel->hairpin_offset = 0;
	channel->hairpin_ceiling = 0u;
	channel->dirty |= DIRTY_PITCH | DIRTY_VOLUME | DIRTY_NOISE;
}

static void bind_instrument(struct tracker_channel *channel,
			    unsigned char number)
{
	unsigned char i;
	unsigned char slot;
	const unsigned char *entry = (const unsigned char *)0;

	for (i = 0u; i < ztr_song.instrument_count; ++i) {
		const unsigned char *candidate = ztr_song.instruments + i * 16u;
		if (candidate[0] == number) {
			entry = candidate;
			break;
		}
	}
	for (slot = 0u; slot < ZTR_MACRO_SLOTS; ++slot) {
		unsigned int offset = entry == (const unsigned char *)0 ? 0u :
			ztr_u16(entry + 4u + slot * 2u);
		if (offset == 0u)
			channel->macro[slot] = (const unsigned char *)0;
		else
			channel->macro[slot] = (const unsigned char *)(ztr_buffer + offset);
	}
}

static void reset_macros(struct tracker_channel *channel)
{
	unsigned char slot;
	for (slot = 0u; slot < ZTR_MACRO_SLOTS; ++slot) {
		channel->macro_timer[slot] = 0u;
		channel->macro_delay[slot] = channel->macro[slot] ==
			(const unsigned char *)0 ? 0u : channel->macro[slot][4];
		channel->macro_position[slot] = 0u;
	}
}

static void release_macros(struct tracker_channel *channel)
{
	unsigned char slot;
	for (slot = 0u; slot < ZTR_MACRO_SLOTS; ++slot) {
		const unsigned char *macro = channel->macro[slot];
		if (macro != (const unsigned char *)0 && (macro[2] & 1u) != 0u) {
			unsigned int release = ztr_u16(macro + 8);
			if (release != 0xFFFFu) {
				channel->macro_position[slot] = release;
				channel->macro_timer[slot] = 0u;
				channel->macro_delay[slot] = 0u;
			}
		}
	}
}

static void note_on(struct tracker_channel *channel, unsigned char note)
{
	if ((channel->flags & (CHANNEL_ACTIVE | CHANNEL_LEGATO)) ==
	    (CHANNEL_ACTIVE | CHANNEL_LEGATO)) {
		channel->note = note;
		channel->dirty |= DIRTY_PITCH | DIRTY_NOISE;
		return;
	}
	channel->note = note;
	channel->flags = (channel->flags & CHANNEL_LEGATO) | CHANNEL_ACTIVE;
	channel->macro_volume = 15u;
	channel->arpeggio_value = 0;
	channel->pitch_value = 0;
	channel->duty_value = 0u;
	channel->phase_value = 0u;
	channel->arpeggio_flags = 0u;
	reset_macros(channel);
	channel->dirty |= DIRTY_PITCH | DIRTY_VOLUME | DIRTY_NOISE | DIRTY_PHASE;
}

static void process_modifiers(struct tracker_channel *channel,
			      unsigned char modifiers,
			      unsigned char arpeggio,
			      signed char hairpin_rate,
			      unsigned char hairpin_ceiling)
{
	if ((modifiers & MOD_RESET) != 0u) clear_notation(channel);
	if ((modifiers & MOD_ARP_CLEAR) != 0u) {
		channel->flags &= (unsigned char)~CHANNEL_NOTATION_ARP;
		channel->notation_phase = 0u;
		channel->dirty |= DIRTY_PITCH | DIRTY_NOISE;
	}
	if ((modifiers & MOD_ARP_SET) != 0u) {
		channel->notation_arpeggio = arpeggio;
		channel->notation_phase = 2u;
		channel->flags |= CHANNEL_NOTATION_ARP;
		channel->dirty |= DIRTY_PITCH | DIRTY_NOISE;
	}
	if ((modifiers & MOD_HAIRPIN_CLEAR) != 0u) channel->hairpin_rate = 0;
	if ((modifiers & MOD_HAIRPIN_SET) != 0u) {
		channel->hairpin_rate = hairpin_rate;
		channel->hairpin_ceiling = hairpin_ceiling;
	}
}

static void process_effect(struct tracker_channel *channel,
			   unsigned char effect, unsigned char parameter)
{
	if (effect == EFFECT_ARPEGGIO || effect == EFFECT_VOL_SLIDE ||
	    effect == EFFECT_FAST_VOL) {
		channel->effect = effect;
		channel->effect_parameter = parameter;
		channel->effect_phase = 0u;
		channel->dirty |= DIRTY_PITCH | DIRTY_NOISE;
	} else if (effect == EFFECT_LEGATO) {
		if (parameter != 0u) channel->flags |= CHANNEL_LEGATO;
		else channel->flags &= (unsigned char)~CHANNEL_LEGATO;
	} else if (effect == EFFECT_VOL_DEC) {
		unsigned char volume = parameter > channel->base_volume ? 0u :
			(unsigned char)(channel->base_volume - parameter);
		if (volume != channel->base_volume) {
			channel->base_volume = volume;
			channel->dirty |= DIRTY_VOLUME;
		}
	} else if (effect == EFFECT_STOP) {
		player_stop();
	}
}

static void decode_row(struct tracker_channel *channel)
{
	const unsigned char *p = channel->pattern_cursor;
	unsigned char mask;
	unsigned char note = 0xFFu;
	unsigned char instrument = 0xFFu;
	unsigned char volume = 0xFFu;
	unsigned char effect = 0u;
	unsigned char parameter = 0u;
	unsigned char modifiers = 0u;
	unsigned char arpeggio = 0u;
	signed char hairpin_rate = 0;
	unsigned char hairpin_ceiling = 0u;

	if (p == (const unsigned char *)0 || p >= channel->pattern_end ||
	    p[0] != player_row) return;
	++p;
	mask = *p++;
	if ((mask & EVENT_NOTE) != 0u) note = *p++;
	if ((mask & EVENT_INSTRUMENT) != 0u) instrument = *p++;
	if ((mask & EVENT_VOLUME) != 0u) volume = *p++;
	if ((mask & EVENT_EFFECT) != 0u) {
		effect = *p++;
		parameter = *p++;
	}
	if ((mask & EVENT_MODIFIERS) != 0u) {
		modifiers = *p++;
		if ((modifiers & MOD_ARP_SET) != 0u) arpeggio = *p++;
		if ((modifiers & MOD_HAIRPIN_SET) != 0u) {
			hairpin_rate = (signed char)*p++;
			hairpin_ceiling = *p++;
		}
	}
	channel->pattern_cursor = p;
	if ((mask & EVENT_MODIFIERS) != 0u)
		process_modifiers(channel, modifiers, arpeggio,
				  hairpin_rate, hairpin_ceiling);
	if (instrument != 0xFFu) {
		channel->instrument = instrument;
		bind_instrument(channel, instrument);
	}
	if (volume != 0xFFu && volume != channel->base_volume) {
		channel->base_volume = volume;
		channel->dirty |= DIRTY_VOLUME;
	}
	if (effect != 0u) process_effect(channel, effect, parameter);
	if ((mask & EVENT_OFF) != 0u) {
		clear_notation(channel);
		channel->flags &= CHANNEL_LEGATO;
		channel->dirty |= DIRTY_VOLUME;
		return;
	}
	if ((mask & EVENT_RELEASE) != 0u) {
		channel->flags |= CHANNEL_RELEASED;
		release_macros(channel);
		return;
	}
	if (note == 0xFFu) return;
	if ((mask & EVENT_LEGATO) != 0u &&
	    (channel->flags & CHANNEL_ACTIVE) != 0u) {
		channel->note = note;
		channel->dirty |= DIRTY_PITCH | DIRTY_NOISE;
	} else {
		note_on(channel, note);
	}
}

static void begin_row(void)
{
	unsigned char i;
	for (i = 0u; i < ZTR_CHANNELS; ++i) {
		if (player_channels[i].effect == EFFECT_ARPEGGIO)
			player_channels[i].dirty |= DIRTY_PITCH | DIRTY_NOISE;
		player_channels[i].effect = 0u;
		player_channels[i].effect_phase = 0u;
	}
	for (i = 0u; i < ZTR_CHANNELS && player_playing != 0u; ++i)
		decode_row(&player_channels[i]);
}

static void advance_notation(struct tracker_channel *channel)
{
	if ((channel->flags & CHANNEL_NOTATION_ARP) != 0u) {
		channel->notation_phase = (unsigned char)(channel->notation_phase + 1u);
		if (channel->notation_phase >= 3u) channel->notation_phase = 0u;
		channel->dirty |= DIRTY_PITCH | DIRTY_NOISE;
	}
	if (channel->hairpin_rate != 0) {
		int next = (int)channel->hairpin_offset + channel->hairpin_rate;
		if (next > 15) next = 15;
		if (next < -15) next = -15;
		if (next != channel->hairpin_offset) {
			channel->hairpin_offset = (signed char)next;
			channel->dirty |= DIRTY_VOLUME;
		}
	}
}

static void advance_effects(void)
{
	unsigned char i;
	for (i = 0u; i < ZTR_CHANNELS; ++i) {
		struct tracker_channel *channel = &player_channels[i];
		if (channel->effect == EFFECT_ARPEGGIO && tick_in_row != 0u) {
			channel->effect_phase = (unsigned char)(channel->effect_phase + 1u);
			if (channel->effect_phase >= 3u) channel->effect_phase = 0u;
			channel->dirty |= DIRTY_PITCH | DIRTY_NOISE;
		} else if ((channel->effect == EFFECT_VOL_SLIDE ||
			    channel->effect == EFFECT_FAST_VOL) && tick_in_row != 0u) {
			unsigned char amount;
			unsigned char volume;
			if ((channel->effect_parameter & 0xF0u) != 0u) {
				amount = (unsigned char)(channel->effect_parameter >> 4);
				if (channel->effect == EFFECT_FAST_VOL)
					amount = (unsigned char)(amount * 4u);
				volume = (unsigned char)(channel->base_volume + amount);
				if (volume < channel->base_volume || volume > 15u) volume = 15u;
			} else {
				amount = (unsigned char)(channel->effect_parameter & 0x0Fu);
				if (channel->effect == EFFECT_FAST_VOL)
					amount = (unsigned char)(amount * 4u);
				volume = amount > channel->base_volume ? 0u :
					(unsigned char)(channel->base_volume - amount);
			}
			if (volume != channel->base_volume) {
				channel->base_volume = volume;
				channel->dirty |= DIRTY_VOLUME;
			}
		}
		advance_notation(channel);
	}
}

static unsigned int bound_macro_position(struct tracker_channel *channel,
					 unsigned char slot,
					 unsigned int position)
{
	const unsigned char *macro = channel->macro[slot];
	unsigned int loop = ztr_u16(macro + 6);
	unsigned int release = ztr_u16(macro + 8);
	unsigned int count = ztr_u16(macro + 10);

	if ((channel->flags & CHANNEL_RELEASED) == 0u && release != 0xFFFFu &&
	    position >= release) {
		if (loop != 0xFFFFu) return loop;
		return release == 0u ? 0u : release - 1u;
	}
	if (position >= count) {
		if (release != 0xFFFFu) return count - 1u;
		return loop != 0xFFFFu ? loop : count - 1u;
	}
	return position;
}

static unsigned char advance_macro(struct tracker_channel *channel,
				   unsigned char slot, int *value,
				   unsigned char *value_flags)
{
	const unsigned char *macro = channel->macro[slot];
	unsigned int position;
	const unsigned char *item;

	if (macro == (const unsigned char *)0) return 0u;
	if (channel->macro_delay[slot] != 0u) {
		--channel->macro_delay[slot];
		return 0u;
	}
	if (channel->macro_timer[slot] != 0u) {
		--channel->macro_timer[slot];
		return 0u;
	}
	position = bound_macro_position(channel, slot,
					channel->macro_position[slot]);
	item = macro + 12u + position * 3u;
	*value_flags = item[0];
	*value = ztr_i16(item + 1);
	channel->macro_timer[slot] = macro[3] == 0u ? 0u :
		(unsigned char)(macro[3] - 1u);
	channel->macro_position[slot] = bound_macro_position(channel, slot,
							   position + 1u);
	return 1u;
}

static void advance_macros(void)
{
	unsigned char i;
	for (i = 0u; i < ZTR_CHANNELS; ++i) {
		struct tracker_channel *channel = &player_channels[i];
		unsigned char slot;
		if ((channel->flags & CHANNEL_ACTIVE) == 0u) continue;
		for (slot = 0u; slot < ZTR_MACRO_SLOTS; ++slot) {
			int value;
			unsigned char flags;
			if (advance_macro(channel, slot, &value, &flags) == 0u) continue;
			if (slot == 0u) {
				unsigned char volume = value < 0 ? 0u :
					(value > 15 ? 15u : (unsigned char)value);
				if (volume != channel->macro_volume) {
					channel->macro_volume = volume;
					channel->dirty |= DIRTY_VOLUME;
				}
			} else if (slot == 1u) {
				if (value != channel->pitch_value) {
					channel->pitch_value = value;
					channel->dirty |= DIRTY_PITCH;
				}
			} else if (slot == 2u) {
				if (value != channel->arpeggio_value ||
				    (flags & 1u) != channel->arpeggio_flags) {
					channel->arpeggio_value = value;
					channel->arpeggio_flags = flags & 1u;
					channel->dirty |= DIRTY_PITCH | DIRTY_NOISE;
				}
			} else if (slot == 3u) {
				unsigned char duty = (unsigned char)value & 1u;
				if (duty != channel->duty_value) {
					channel->duty_value = duty;
					channel->dirty |= DIRTY_NOISE;
				}
			} else {
				channel->phase_value = (unsigned char)value;
				if (channel->phase_value != 0u && (i & 3u) == 3u)
					channel->dirty |= DIRTY_PHASE;
			}
		}
	}
}

static void setup_order(void)
{
	unsigned char channel_number;
	const unsigned char *order = ztr_song.orders +
		(unsigned int)player_order * ZTR_CHANNELS;
	unsigned int i;

	for (channel_number = 0u; channel_number < ZTR_CHANNELS; ++channel_number) {
		struct tracker_channel *channel = &player_channels[channel_number];
		channel->pattern_cursor = (const unsigned char *)0;
		channel->pattern_end = (const unsigned char *)0;
	}
	/* Resolve all 16 channel selections in one directory pass.  Circuits has
	 * 398 entries and 32-row orders; the old channel-major search performed
	 * as many as 6,368 comparisons at every bar boundary. */
	for (i = 0u; i < ztr_song.pattern_count; ++i) {
		const unsigned char *entry = ztr_song.pattern_directory + i * 8u;
		unsigned char number = entry[0];
		struct tracker_channel *channel = &player_channels[number];
		if (channel->pattern_cursor == (const unsigned char *)0 &&
		    order[number] != 0xFFu && entry[1] == order[number]) {
			unsigned int offset = ztr_u16(entry + 2);
			channel->pattern_cursor = ztr_buffer + offset;
			channel->pattern_end = channel->pattern_cursor + ztr_u16(entry + 4);
		}
	}
}

static void advance_position(void)
{
	unsigned int next = (unsigned int)player_row + 1u;
	if (next < ztr_song.pattern_length) {
		player_row = (unsigned char)next;
		return;
	}
	player_row = 0u;
	++player_order;
	if (player_order >= ztr_song.order_count) {
		player_stop();
		return;
	}
	player_row_changed = 1u;
	setup_order();
}

void player_start(void)
{
	unsigned char i;
	for (i = 0u; i < ZTR_CHANNELS; ++i) {
		unsigned char slot;
		struct tracker_channel *channel = &player_channels[i];
		channel->note = 0xFFu;
		channel->instrument = 0xFFu;
		channel->base_volume = 15u;
		channel->macro_volume = 15u;
		channel->effect = 0u;
		channel->effect_parameter = 0u;
		channel->flags = 0u;
		channel->dirty = DIRTY_PITCH | DIRTY_VOLUME | DIRTY_NOISE | DIRTY_PHASE;
		channel->arpeggio_value = 0;
		channel->pitch_value = 0;
		channel->duty_value = 0u;
		channel->phase_value = 0u;
		channel->pattern_cursor = (const unsigned char *)0;
		channel->pattern_end = (const unsigned char *)0;
		channel->effect_phase = 0u;
		channel->arpeggio_flags = 0u;
		channel->notation_arpeggio = 0u;
		channel->notation_phase = 0u;
		channel->hairpin_rate = 0;
		channel->hairpin_offset = 0;
		channel->hairpin_ceiling = 0u;
		channel->last_period = 0u;
		channel->last_attenuation = 0xFFu;
		channel->last_noise = 0xFFu;
		for (slot = 0u; slot < ZTR_MACRO_SLOTS; ++slot) {
			channel->macro_timer[slot] = 0u;
			channel->macro_delay[slot] = 0u;
			channel->macro_position[slot] = 0u;
			channel->macro[slot] = (const unsigned char *)0;
		}
	}
	player_order = 0u;
	player_row = 0u;
	tick_in_row = 0u;
	elapsed_subticks = 0u;
	player_minutes = 0u;
	player_seconds = 0u;
	player_psg_writes = 0u;
	player_psg_error = 0u;
	player_row_changed = 1u;
	setup_order();
	player_playing = 1u;
	if (psg_mute_all() < 0) {
		player_playing = 0u;
	} else {
		for (i = 0u; i < ZTR_CHANNELS; ++i)
			player_channels[i].last_attenuation = 15u;
	}
}

void player_stop(void)
{
	player_playing = 0u;
}

void player_tick(void)
{
	if (player_playing == 0u) return;
	if (tick_in_row == 0u) begin_row();
	if (player_playing == 0u) return;
	advance_effects();
	advance_macros();
	psg_render_all();
	++elapsed_subticks;
	if (elapsed_subticks >= ztr_song.tick_rate) {
		elapsed_subticks = 0u;
		++player_seconds;
		if (player_seconds >= 60u) {
			player_seconds = 0u;
			++player_minutes;
			if (player_minutes >= 100u) player_minutes = 0u;
		}
	}
	++tick_in_row;
	if (tick_in_row >= ztr_song.speed) {
		tick_in_row = 0u;
		advance_position();
	}
}
