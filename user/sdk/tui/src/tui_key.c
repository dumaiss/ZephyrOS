/* libtui VT100 key decoder.
 *
 * The console transport hands the process VT100 bytes.  This member turns
 * the exact controller vocabulary into tui_key_t values, keeps a bounded
 * eight-byte sequence buffer and an eight-byte replay queue for unknown or
 * malformed sequences, and uses only console_read()/idle() for input.
 *
 * A standalone ESC is returned after one immediate non-blocking look-ahead:
 * if no continuation byte is already pending, ESC is delivered at once and
 * the parser is NORMAL.  There is no timer, sleep or configurable timeout.
 *
 * The parser's static state is reset lazily against a generation counter in
 * the shared TUI state.  tui_init() advances the generation; the next decoder
 * call clears every parser, ready-key, sequence and replay field.  The
 * lifecycle therefore does not reference this archive member, and a
 * screen-only program still links no decoder code or data.
 */

#include "tui_internal.h"
#include "kos/console.h"
#include "kos.h"

#define TUI_KEY_STATE_NORMAL 0u
#define TUI_KEY_STATE_CSI 1u
#define TUI_KEY_STATE_SS3 2u

#define TUI_KEY_SEQUENCE_MAX 8u

static uint8_t tui_key_state;
static uint8_t tui_key_sequence[TUI_KEY_SEQUENCE_MAX];
static uint8_t tui_key_length;
static uint8_t tui_key_replay_buffer[TUI_KEY_SEQUENCE_MAX];
static uint8_t tui_key_replay_length;
static uint8_t tui_key_replay_at;
static tui_key_t tui_key_ready;
static uint8_t tui_key_ready_valid;
static uint16_t tui_key_generation;

static int tui_key_read(uint8_t *out)
{
	int result;

	if (tui_key_replay_at < tui_key_replay_length) {
		*out = tui_key_replay_buffer[tui_key_replay_at];
		++tui_key_replay_at;
		return 1;
	}
	result = console_read((char *)out, 1u);
	if (result < 0) {
		return result;
	}
	return result;
}

static void tui_key_replay(const uint8_t *bytes, uint8_t length)
{
	uint8_t i;

	tui_key_replay_length = length;
	tui_key_replay_at = 0u;
	for (i = 0u; i < length; ++i) {
		tui_key_replay_buffer[i] = bytes[i];
	}
}

static void tui_key_reset(void)
{
	uint8_t i;

	tui_key_state = TUI_KEY_STATE_NORMAL;
	tui_key_length = 0u;
	tui_key_replay_length = 0u;
	tui_key_replay_at = 0u;
	tui_key_ready = TUI_KEY_NONE;
	tui_key_ready_valid = 0u;
	for (i = 0u; i < TUI_KEY_SEQUENCE_MAX; ++i) {
		tui_key_sequence[i] = 0u;
		tui_key_replay_buffer[i] = 0u;
	}
}

static void tui_key_sync(void)
{
	if (tui_key_generation != tui_state.key_generation) {
		tui_key_generation = tui_state.key_generation;
		tui_key_reset();
	}
}

static void tui_key_reject(void)
{
	tui_key_state = TUI_KEY_STATE_NORMAL;
	tui_key_ready = TUI_KEY_ESC;
	tui_key_ready_valid = 1u;
	tui_key_replay(tui_key_sequence, tui_key_length);
	tui_key_length = 0u;
}

static void tui_key_deliver(tui_key_t key)
{
	tui_key_state = TUI_KEY_STATE_NORMAL;
	tui_key_length = 0u;
	tui_key_ready = key;
	tui_key_ready_valid = 1u;
}

static tui_key_t tui_key_csi_letter(uint8_t ch)
{
	switch (ch) {
	case 'A':
		return TUI_KEY_UP;
	case 'B':
		return TUI_KEY_DOWN;
	case 'C':
		return TUI_KEY_RIGHT;
	case 'D':
		return TUI_KEY_LEFT;
	case 'H':
		return TUI_KEY_HOME;
	case 'F':
		return TUI_KEY_END;
	default:
		return TUI_KEY_NONE;
	}
}

static tui_key_t tui_key_csi_number(uint16_t value)
{
	switch (value) {
	case 2u:
		return TUI_KEY_INSERT;
	case 3u:
		return TUI_KEY_DELETE;
	case 5u:
		return TUI_KEY_PGUP;
	case 6u:
		return TUI_KEY_PGDN;
	case 15u:
		return TUI_KEY_F5;
	case 17u:
		return TUI_KEY_F6;
	case 18u:
		return TUI_KEY_F7;
	case 19u:
		return TUI_KEY_F8;
	case 20u:
		return TUI_KEY_F9;
	case 21u:
		return TUI_KEY_F10;
	case 23u:
		return TUI_KEY_F11;
	case 24u:
		return TUI_KEY_F12;
	default:
		return TUI_KEY_NONE;
	}
}

static tui_key_t tui_key_ss3_letter(uint8_t ch)
{
	switch (ch) {
	case 'P':
		return TUI_KEY_F1;
	case 'Q':
		return TUI_KEY_F2;
	case 'R':
		return TUI_KEY_F3;
	case 'S':
		return TUI_KEY_F4;
	default:
		return TUI_KEY_NONE;
	}
}

static tui_key_t tui_key_csi_number_complete(void)
{
	uint16_t value = 0u;
	uint8_t i;

	for (i = 1u; (uint8_t)(i + 1u) < tui_key_length; ++i) {
		if (tui_key_sequence[i] < (uint8_t)'0' ||
		    tui_key_sequence[i] > (uint8_t)'9') {
			return TUI_KEY_NONE;
		}
		value = (uint16_t)((value << 3) + (value << 1) +
				   (uint16_t)(tui_key_sequence[i] -
					      (uint8_t)'0'));
	}
	return tui_key_csi_number(value);
}

/* Decode until one key is ready, input is exhausted (0), or the console
 * reports an error (negative). */
static int tui_key_decode(void)
{
	uint8_t ch;
	uint8_t next;
	tui_key_t key;
	int result;

	for (;;) {
		if (tui_key_ready_valid != 0u) {
			return 1;
		}
		result = tui_key_read(&ch);
		if (result <= 0) {
			return result;
		}
		switch (tui_key_state) {
		case TUI_KEY_STATE_NORMAL:
			if (ch == 0u) {
				continue;
			}
			if (ch == 0x1Bu) {
				/* Immediate look-ahead: a lone ESC is a complete
				 * key and never leaves the parser waiting. */
				result = tui_key_read(&next);
				if (result < 0) {
					return result;
				}
				if (result == 0) {
					tui_key_deliver(TUI_KEY_ESC);
					return 1;
				}
				if (next == (uint8_t)'[') {
					tui_key_sequence[0] = next;
					tui_key_length = 1u;
					tui_key_state = TUI_KEY_STATE_CSI;
					continue;
				}
				if (next == (uint8_t)'O') {
					tui_key_sequence[0] = next;
					tui_key_length = 1u;
					tui_key_state = TUI_KEY_STATE_SS3;
					continue;
				}
				tui_key_state = TUI_KEY_STATE_NORMAL;
				tui_key_ready = TUI_KEY_ESC;
				tui_key_ready_valid = 1u;
				tui_key_replay_buffer[0] = next;
				tui_key_replay_length = 1u;
				tui_key_replay_at = 0u;
				return 1;
			}
			tui_key_ready = (tui_key_t)ch;
			tui_key_ready_valid = 1u;
			return 1;

		case TUI_KEY_STATE_CSI:
			if (tui_key_length >= TUI_KEY_SEQUENCE_MAX) {
				tui_key_reject();
				return 1;
			}
			tui_key_sequence[tui_key_length] = ch;
			++tui_key_length;
			if (ch == 0u) {
				tui_key_reject();
				return 1;
			}
			if (tui_key_length == 2u) {
				key = tui_key_csi_letter(ch);
				if (key != TUI_KEY_NONE) {
					tui_key_deliver(key);
					return 1;
				}
			}
			if (ch == (uint8_t)'~') {
				key = tui_key_csi_number_complete();
				if (key == TUI_KEY_NONE) {
					tui_key_reject();
				} else {
					tui_key_deliver(key);
				}
				return 1;
			}
			if (ch >= (uint8_t)'0' && ch <= (uint8_t)'9') {
				if (tui_key_length >= TUI_KEY_SEQUENCE_MAX) {
					tui_key_reject();
					return 1;
				}
				continue;
			}
			tui_key_reject();
			return 1;

		default: /* TUI_KEY_STATE_SS3 */
			if (tui_key_length >= TUI_KEY_SEQUENCE_MAX) {
				tui_key_reject();
				return 1;
			}
			tui_key_sequence[tui_key_length] = ch;
			++tui_key_length;
			if (tui_key_length == 2u) {
				key = tui_key_ss3_letter(ch);
				if (key != TUI_KEY_NONE) {
					tui_key_deliver(key);
					return 1;
				}
			}
			tui_key_reject();
			return 1;
		}
	}
}

int tui_key_pending(void)
{
	int result;

	if (tui_state.initialized == 0u) {
		return -KOS_EINVAL;
	}
	tui_key_sync();
	result = tui_key_decode();
	if (result > 0) {
		return 1;
	}
	return result;
}

tui_key_t tui_pollkey(void)
{
	tui_key_t key;

	if (tui_state.initialized == 0u) {
		return TUI_KEY_NONE;
	}
	tui_key_sync();
	if (tui_key_decode() <= 0) {
		return TUI_KEY_NONE;
	}
	key = tui_key_ready;
	tui_key_ready = TUI_KEY_NONE;
	tui_key_ready_valid = 0u;
	return key;
}

tui_key_t tui_getkey(void)
{
	tui_key_t key;

	if (tui_state.initialized == 0u) {
		return TUI_KEY_NONE;
	}
	tui_key_sync();
	for (;;) {
		key = tui_pollkey();
		if (key != TUI_KEY_NONE) {
			return key;
		}
		idle();
	}
}
