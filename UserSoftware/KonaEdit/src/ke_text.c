#include "ke_text.h"

uint32_t ke_visual_advance(uint32_t column, uint8_t byte)
{
	if (byte == 9u) return (column + 8ul) & ~7ul;
	return column + 1ul;
}

ke_pos_t ke_line_start(struct ke_store *store, ke_pos_t position)
{
	uint8_t byte;

	if (position > ke_store_length(store)) position = ke_store_length(store);
	while (position != 0ul) {
		if (ke_store_byte(store, position - 1ul, &byte) != KE_STORE_OK ||
		    byte == 10u) break;
		--position;
	}
	return position;
}

ke_pos_t ke_line_end(struct ke_store *store, ke_pos_t position)
{
	uint8_t byte;
	ke_pos_t length = ke_store_length(store);

	while (position < length) {
		if (ke_store_byte(store, position, &byte) != KE_STORE_OK || byte == 10u)
			break;
		++position;
	}
	return position;
}

uint32_t ke_visual_column(struct ke_store *store, ke_pos_t position)
{
	ke_pos_t at = ke_line_start(store, position);
	uint32_t column = 0ul;
	uint8_t byte;

	while (at < position && ke_store_byte(store, at, &byte) == KE_STORE_OK) {
		column = ke_visual_advance(column, byte);
		++at;
	}
	return column;
}

ke_pos_t ke_position_for_column(struct ke_store *store, ke_pos_t line,
				uint32_t column)
{
	ke_pos_t at = line;
	ke_pos_t end = ke_line_end(store, line);
	uint32_t visual = 0ul;
	uint8_t byte;

	while (at < end && ke_store_byte(store, at, &byte) == KE_STORE_OK) {
		uint32_t next = ke_visual_advance(visual, byte);

		if (next > column) break;
		visual = next;
		++at;
	}
	return at;
}

static uint8_t fold_ascii(uint8_t byte)
{
	if (byte >= (uint8_t)'a' && byte <= (uint8_t)'z')
		return (uint8_t)(byte - ((uint8_t)'a' - (uint8_t)'A'));
	return byte;
}

int ke_find(struct ke_store *store, ke_pos_t from, const uint8_t *pattern,
	    uint16_t length, uint8_t match_case, ke_pos_t *found)
{
	ke_pos_t limit;
	ke_pos_t at;

	if (pattern == (const uint8_t *)0 || found == (ke_pos_t *)0 ||
	    length == 0u || from > ke_store_length(store) ||
	    (ke_pos_t)length > ke_store_length(store)) return 0;
	limit = ke_store_length(store) - length;
	for (at = from; at <= limit; ++at) {
		uint16_t i;
		uint8_t byte;

		for (i = 0u; i < length; ++i) {
			if (ke_store_byte(store, at + i, &byte) != KE_STORE_OK)
				return 0;
			if (match_case != 0u) {
				if (byte != pattern[i]) break;
			} else if (fold_ascii(byte) != fold_ascii(pattern[i])) {
				break;
			}
		}
		if (i == length) {
			*found = at;
			return 1;
		}
	}
	return 0;
}

int ke_normalize(struct ke_normalizer *state, const uint8_t *input,
		 uint16_t input_length, uint8_t *output, uint16_t output_size,
		 uint16_t *consumed, uint16_t *produced)
{
	uint16_t in = 0u;
	uint16_t out = 0u;

	if (state == (struct ke_normalizer *)0 || input == (const uint8_t *)0 ||
	    output == (uint8_t *)0 || consumed == (uint16_t *)0 ||
	    produced == (uint16_t *)0) return -1;
	while (in < input_length) {
		uint8_t byte = input[in];

		if (state->pending_cr != 0u) {
			if (out >= output_size) break;
			output[out++] = 10u;
			state->pending_cr = 0u;
			if (byte == 10u) {
				++in;
				continue;
			}
		}
		if (byte == 0x1au) {
			state->eof = 1u;
			++in;
			break;
		}
		if (byte == 0u) return -1;
		if (byte == 13u) {
			state->pending_cr = 1u;
			++in;
			continue;
		}
		if (out >= output_size) break;
		output[out++] = byte;
		++in;
	}
	*consumed = in;
	*produced = out;
	return 0;
}

int ke_normalize_finish(struct ke_normalizer *state, uint8_t *output,
			uint16_t output_size, uint16_t *produced)
{
	if (state == (struct ke_normalizer *)0 || output == (uint8_t *)0 ||
	    produced == (uint16_t *)0) return -1;
	*produced = 0u;
	if (state->pending_cr != 0u) {
		if (output_size == 0u) return -1;
		output[0] = 10u;
		*produced = 1u;
		state->pending_cr = 0u;
	}
	return 0;
}
