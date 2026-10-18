#ifndef SNTRACKER_H
#define SNTRACKER_H

#include "kos.h"

#define ZTR_CHANNELS          16u
#define ZTR_NOTE_COUNT        96u
#define ZTR_MAX_FILE_BYTES    20480u
#define ZTR_HEADER_BYTES      64u
#define ZTR_INSTRUMENT_BYTES  16u
#define ZTR_PATTERN_BYTES     8u
#define ZTR_MACRO_SLOTS       5u

#define EVENT_NOTE            0x80u
#define EVENT_INSTRUMENT      0x40u
#define EVENT_VOLUME          0x20u
#define EVENT_EFFECT          0x10u
#define EVENT_OFF             0x08u
#define EVENT_RELEASE         0x04u
#define EVENT_LEGATO          0x02u
#define EVENT_MODIFIERS       0x01u

#define MOD_RESET             0x80u
#define MOD_ARP_SET           0x40u
#define MOD_ARP_CLEAR         0x20u
#define MOD_HAIRPIN_SET       0x10u
#define MOD_HAIRPIN_CLEAR     0x08u

#define EFFECT_ARPEGGIO       1u
#define EFFECT_VOL_SLIDE      2u
#define EFFECT_LEGATO         3u
#define EFFECT_VOL_DEC        4u
#define EFFECT_FAST_VOL       5u
#define EFFECT_STOP           6u

struct ztr_song {
	unsigned char tick_rate;
	unsigned char speed;
	unsigned int pattern_length;
	unsigned char order_count;
	unsigned int pattern_count;
	unsigned char instrument_count;
	const char *title;
	const char *author;
	const unsigned char *orders;
	const unsigned char *instruments;
	const unsigned char *pattern_directory;
	const unsigned char *note_table;
};

struct tracker_channel {
	unsigned char note;
	unsigned char instrument;
	unsigned char base_volume;
	unsigned char macro_volume;
	unsigned char effect;
	unsigned char effect_parameter;
	unsigned char flags;
	unsigned char dirty;
	int arpeggio_value;
	int pitch_value;
	unsigned char duty_value;
	unsigned char phase_value;
	unsigned char macro_timer[ZTR_MACRO_SLOTS];
	unsigned char macro_delay[ZTR_MACRO_SLOTS];
	unsigned int macro_position[ZTR_MACRO_SLOTS];
	const unsigned char *macro[ZTR_MACRO_SLOTS];
	const unsigned char *pattern_cursor;
	const unsigned char *pattern_end;
	unsigned char effect_phase;
	unsigned char arpeggio_flags;
	unsigned char notation_arpeggio;
	unsigned char notation_phase;
	signed char hairpin_rate;
	signed char hairpin_offset;
	unsigned char hairpin_ceiling;
	unsigned int last_period;
	unsigned char last_attenuation;
	unsigned char last_noise;
};

extern struct ztr_song ztr_song;
extern unsigned char ztr_buffer[ZTR_MAX_FILE_BYTES];
extern unsigned int ztr_size;
extern const char *ztr_error;

extern volatile unsigned char player_playing;
extern volatile unsigned char player_row_changed;
extern unsigned char player_order;
extern unsigned char player_row;
extern unsigned char player_minutes;
extern unsigned char player_seconds;
extern unsigned int player_psg_writes;
extern unsigned char player_psg_error;
extern struct tracker_channel player_channels[ZTR_CHANNELS];

int ztr_load(const char *path);
void player_start(void);
void player_tick(void);
void player_stop(void);
int psg_mute_all(void);
void psg_render_all(void);

unsigned int ztr_u16(const unsigned char *p);
int ztr_i16(const unsigned char *p);

#endif
