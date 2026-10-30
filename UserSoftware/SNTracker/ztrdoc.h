#ifndef SNTRACK_ZTRDOC_H
#define SNTRACK_ZTRDOC_H

#include "kos/reader.h"
#include "kos/snd.h"

#define ZTRDOC_CHANNELS 16u
#define ZTRDOC_MAX_INSTRUMENTS 16u
#define ZTRDOC_ORDER_BUFFER 1024u
#define ZTRDOC_ORDER_TABLE 64u
#define ZTRDOC_VIEW_ROWS 16u

#define ZTRDOC_INSTR_UNUSED 0u
#define ZTRDOC_INSTR_TONE   1u
#define ZTRDOC_INSTR_NOISE  2u

struct ztrdoc_event {
	uint8_t mask;
	uint8_t note;
	uint8_t instrument;
	uint8_t volume;
	uint8_t effect;
	uint8_t parameter;
	uint8_t modifier_flags;
	uint8_t modifier_xy;
	int8_t hairpin_rate;
	uint8_t hairpin_ceiling;
};

struct ztrdoc_instrument {
	uint8_t number;
	uint8_t type;
	char name[19];
	uint16_t arpeggio_offset;
	uint16_t noise_offset;
	int16_t arpeggio_first;
	int16_t noise_first;
	uint8_t arpeggio_flags;
	uint8_t arpeggio_delay;
	uint8_t noise_delay;
};

struct ztrdoc_pattern {
	uint8_t channel;
	uint8_t number;
	uint16_t offset;
	uint16_t length;
};

/* Order `n`'s 16 pattern numbers (FFh: the channel is unused). */
#define ZTRDOC_ORDER(doc, n) ((doc)->orders + ((uint16_t)(n) << 4))

struct ztrdoc {
	const kos_reader_t *reader;
	uint16_t rate;
	uint8_t speed;
	uint16_t rows;
	uint16_t order_count;
	uint16_t pattern_count;
	uint8_t instrument_count;
	uint32_t clock_hz;
	uint16_t strings_offset;
	uint16_t strings_size;
	uint16_t orders_offset;
	uint16_t instruments_offset;
	uint16_t macros_offset;
	uint16_t patterns_offset;
	uint16_t pattern_data_offset;
	uint16_t note_table_offset;
	char title[33];
	/* libsnd's resident tables (snd_ztr_tables): the orders and the
	 * 6-byte pattern directory, not copied. */
	const uint8_t *orders;
	const uint8_t *patterns;
	struct ztrdoc_instrument instruments[ZTRDOC_MAX_INSTRUMENTS];
	int8_t instrument_index[ZTRDOC_MAX_INSTRUMENTS];
};

/* Open over a song libsnd has loaded (its layout, capacities and CRC
 * already checked): the header's display values, the instruments, and
 * one pass over every stream that validates the events and types each
 * instrument; scratch holds a window of streams.  -KOS_ENOSPC: a stream
 * over the scratch; -KOS_ENOTSUP: an instrument used on tone and noise
 * channels; -KOS_EINVAL: malformed. */
int ztrdoc_open(struct ztrdoc *doc, const kos_reader_t *reader,
		const struct snd_ztr_tables *tables,
		uint8_t *scratch, uint16_t size);
/* The order's streams after a 16-entry start/end table; the used size. */
int ztrdoc_load_order(const struct ztrdoc *doc, uint16_t order,
		      uint8_t *buffer, uint16_t size);
/* Row-at-a-time decoding of a loaded order: ztrdoc_seek_rows sets each
 * channel's stream cursor to its first event at or after `first_row`;
 * ztrdoc_next_row then decodes row `row` (rows asked in ascending order)
 * into one event per channel, empty where the channel has none. */
int ztrdoc_seek_rows(const uint8_t *buffer, uint16_t first_row,
		     uint16_t at[ZTRDOC_CHANNELS]);
int ztrdoc_next_row(const uint8_t *buffer, uint16_t row,
		    uint16_t at[ZTRDOC_CHANNELS],
		    struct ztrdoc_event out[ZTRDOC_CHANNELS]);
const struct ztrdoc_instrument *ztrdoc_instrument(
	const struct ztrdoc *doc, uint8_t number);
uint8_t ztrdoc_noise_control(const struct ztrdoc *doc,
			     const struct ztrdoc_event *event,
			     uint8_t *shown);

#endif
