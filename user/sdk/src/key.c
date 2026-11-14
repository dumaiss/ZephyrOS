/* KonaOS SDK: the keyboard key decoder (kos/key.h).
 *
 * The console hands the process VT100 bytes.  This member turns the
 * controller's vocabulary into key codes, keeps a bounded eight-byte
 * sequence buffer and an eight-byte replay queue for unknown or malformed
 * sequences, and reads input only through console_read().
 *
 * A standalone ESC is returned after one immediate non-blocking
 * look-ahead: if no continuation byte is already pending, ESC is delivered
 * at once and the parser is NORMAL.  There is no timer, sleep or
 * configurable timeout.
 */

#include "kos.h"
#include "kos/console.h"
#include "kos/key.h"

#define KEY_STATE_NORMAL 0u
#define KEY_STATE_CSI 1u
#define KEY_STATE_SS3 2u

#define KEY_SEQUENCE_MAX 8u

static uint8_t key_state;
static uint8_t key_sequence[KEY_SEQUENCE_MAX];
static uint8_t key_length;
static uint8_t key_replay_buffer[KEY_SEQUENCE_MAX];
static uint8_t key_replay_length;
static uint8_t key_replay_at;
static key_code_t key_ready;
static uint8_t key_ready_valid;

static int key_read(uint8_t *out)
{
	if (key_replay_at < key_replay_length) {
		*out = key_replay_buffer[key_replay_at];
		++key_replay_at;
		return 1;
	}
	return console_read((char *)out, 1u);
}

static void key_replay(const uint8_t *bytes, uint8_t length)
{
	uint8_t i;

	key_replay_length = length;
	key_replay_at = 0u;
	for (i = 0u; i < length; ++i) {
		key_replay_buffer[i] = bytes[i];
	}
}

/* The buffers' bytes beyond their lengths are never read: zeroing the
 * lengths and the state is the whole reset. */
void key_reset(void)
{
	key_state = KEY_STATE_NORMAL;
	key_length = 0u;
	key_replay_length = 0u;
	key_replay_at = 0u;
	key_ready = KEY_NONE;
	key_ready_valid = 0u;
}

static void key_reject(void)
{
	key_state = KEY_STATE_NORMAL;
	key_ready = KEY_ESC;
	key_ready_valid = 1u;
	key_replay(key_sequence, key_length);
	key_length = 0u;
}

static void key_deliver(key_code_t key)
{
	key_state = KEY_STATE_NORMAL;
	key_length = 0u;
	key_ready = key;
	key_ready_valid = 1u;
}

static key_code_t key_csi_letter(uint8_t ch)
{
	switch (ch) {
	case 'A':
		return KEY_UP;
	case 'B':
		return KEY_DOWN;
	case 'C':
		return KEY_RIGHT;
	case 'D':
		return KEY_LEFT;
	case 'H':
		return KEY_HOME;
	case 'F':
		return KEY_END;
	default:
		return KEY_NONE;
	}
}

static key_code_t key_csi_number(uint16_t value)
{
	switch (value) {
	case 2u:
		return KEY_INSERT;
	case 3u:
		return KEY_DELETE;
	case 5u:
		return KEY_PGUP;
	case 6u:
		return KEY_PGDN;
	case 15u:
		return KEY_F5;
	case 17u:
		return KEY_F6;
	case 18u:
		return KEY_F7;
	case 19u:
		return KEY_F8;
	case 20u:
		return KEY_F9;
	case 21u:
		return KEY_F10;
	case 23u:
		return KEY_F11;
	case 24u:
		return KEY_F12;
	default:
		return KEY_NONE;
	}
}

static key_code_t key_ss3_letter(uint8_t ch)
{
	switch (ch) {
	case 'P':
		return KEY_F1;
	case 'Q':
		return KEY_F2;
	case 'R':
		return KEY_F3;
	case 'S':
		return KEY_F4;
	default:
		return KEY_NONE;
	}
}

static key_code_t key_csi_number_complete(void)
{
	uint16_t value = 0u;
	uint8_t i;

	for (i = 1u; (uint8_t)(i + 1u) < key_length; ++i) {
		if (key_sequence[i] < (uint8_t)'0' ||
		    key_sequence[i] > (uint8_t)'9') {
			return KEY_NONE;
		}
		value = (uint16_t)((value << 3) + (value << 1) +
				   (uint16_t)(key_sequence[i] -
					      (uint8_t)'0'));
	}
	return key_csi_number(value);
}

/* Decode until one key is ready, input is exhausted (0), or the console
 * reports an error (negative). */
static int key_decode(void)
{
	uint8_t ch;
	uint8_t next;
	key_code_t key;
	int result;

	for (;;) {
		if (key_ready_valid != 0u) {
			return 1;
		}
		result = key_read(&ch);
		if (result <= 0) {
			return result;
		}
		switch (key_state) {
		case KEY_STATE_NORMAL:
			if (ch == 0u) {
				continue;
			}
			if (ch == 0x1Bu) {
				/* Immediate look-ahead: a lone ESC is a complete
				 * key and never leaves the parser waiting. */
				result = key_read(&next);
				if (result < 0) {
					return result;
				}
				if (result == 0) {
					key_deliver(KEY_ESC);
					return 1;
				}
				if (next == (uint8_t)'[') {
					key_sequence[0] = next;
					key_length = 1u;
					key_state = KEY_STATE_CSI;
					continue;
				}
				if (next == (uint8_t)'O') {
					key_sequence[0] = next;
					key_length = 1u;
					key_state = KEY_STATE_SS3;
					continue;
				}
				/* ESC, then the byte as itself. */
				key_deliver(KEY_ESC);
				key_replay(&next, 1u);
				return 1;
			}
			key_ready = (key_code_t)ch;
			key_ready_valid = 1u;
			return 1;

		case KEY_STATE_CSI:
			if (key_length >= KEY_SEQUENCE_MAX) {
				key_reject();
				return 1;
			}
			key_sequence[key_length] = ch;
			++key_length;
			if (ch == 0u) {
				key_reject();
				return 1;
			}
			if (key_length == 2u) {
				key = key_csi_letter(ch);
				if (key != KEY_NONE) {
					key_deliver(key);
					return 1;
				}
			}
			if (ch == (uint8_t)'~') {
				key = key_csi_number_complete();
				if (key == KEY_NONE) {
					key_reject();
				} else {
					key_deliver(key);
				}
				return 1;
			}
			if (ch >= (uint8_t)'0' && ch <= (uint8_t)'9') {
				if (key_length >= KEY_SEQUENCE_MAX) {
					key_reject();
					return 1;
				}
				continue;
			}
			key_reject();
			return 1;

		default: /* KEY_STATE_SS3 */
			if (key_length >= KEY_SEQUENCE_MAX) {
				key_reject();
				return 1;
			}
			key_sequence[key_length] = ch;
			++key_length;
			if (key_length == 2u) {
				key = key_ss3_letter(ch);
				if (key != KEY_NONE) {
					key_deliver(key);
					return 1;
				}
			}
			key_reject();
			return 1;
		}
	}
}

int key_pending(void)
{
	int result = key_decode();

	return result > 0 ? 1 : result;
}

key_code_t key_poll(void)
{
	key_code_t key;

	if (key_decode() <= 0) {
		return KEY_NONE;
	}
	key = key_ready;
	key_ready = KEY_NONE;
	key_ready_valid = 0u;
	return key;
}
