#ifndef SNTRACK_ZTRDOC_H
#define SNTRACK_ZTRDOC_H

#include "kos/reader.h"

#define ZTRDOC_CHANNELS 16u
#define ZTRDOC_MAX_ORDERS 136u
#define ZTRDOC_MAX_INSTRUMENTS 16u
#define ZTRDOC_MAX_PATTERNS 400u
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
	uint16_t events;
};

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
	uint8_t orders[ZTRDOC_MAX_ORDERS][ZTRDOC_CHANNELS];
	struct ztrdoc_pattern patterns[ZTRDOC_MAX_PATTERNS];
	uint16_t channel_start[ZTRDOC_CHANNELS + 1u];
	struct ztrdoc_instrument instruments[ZTRDOC_MAX_INSTRUMENTS];
	int8_t instrument_index[ZTRDOC_MAX_INSTRUMENTS];
};

/* Open, checking the whole file; scratch holds one stream at a time.
 * -KOS_ENOSPC: a stream or an order over the buffers; -KOS_ENOTSUP: an
 * instrument used on tone and noise channels; -KOS_EINVAL: malformed. */
int ztrdoc_open(struct ztrdoc *doc, const kos_reader_t *reader,
		uint8_t *scratch, uint16_t size);
/* The order's streams after a 16-entry start/end table; the used size. */
int ztrdoc_load_order(const struct ztrdoc *doc, uint16_t order,
		      uint8_t *buffer, uint16_t size);
/* One channel's events for the ZTRDOC_VIEW_ROWS rows from first_row of a
 * loaded order. */
int ztrdoc_decode_channel(const uint8_t *buffer, uint8_t channel,
			  uint16_t first_row,
			  struct ztrdoc_event out[ZTRDOC_VIEW_ROWS]);
int ztrdoc_find_pattern(const struct ztrdoc *doc, uint8_t channel,
			uint8_t number, struct ztrdoc_pattern *out);
const struct ztrdoc_instrument *ztrdoc_instrument(
	const struct ztrdoc *doc, uint8_t number);
uint8_t ztrdoc_noise_control(const struct ztrdoc *doc,
			     const struct ztrdoc_event *event,
			     uint8_t *shown);

#endif
