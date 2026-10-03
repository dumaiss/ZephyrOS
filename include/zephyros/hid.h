/* ZephyrOS HID class contract (M8, driver ABI v2).
 *
 * The HID class is the native human-input abstraction.  It sits above
 * the IOC transport and never owns SIO1 framing or hardware status
 * pins; the doorbell that signals pending input is consumed through
 * the IOC class's semantic status, not by sampling `/CTSB` or RR0.
 * Class values and capability bits are fixed by docs/devices.md and
 * docs/architecture.md section 30.5.
 *
 * Capabilities are authoritative:
 *
 *   - ZCAP_HID_TEXT       translated terminal bytes through read_text
 *   - ZCAP_HID_KEY_EVENTS reserved in M8; the key-event shape below is
 *                         frozen so a later implementation can add the
 *                         operation without changing the class boundary
 *   - ZCAP_HID_GAMEPAD    reserved in M8; the gamepad shape below is
 *                         frozen for the same reason
 *
 * A keyboard/text instance also advertises ZCAP_CHAR_IN and
 * ZCAP_CONSOLE_IN and provides the generic character input operations
 * (`zchar_ops.getc`/`poll`) so the console service can bind it.
 *
 * Absent methods stay null and no caller may consult them: an instance
 * that does not advertise the matching capability must be reported as
 * ZDEV_ENOTSUP by any dispatcher, never inferred from the instance or
 * driver name.  M8 implements only ZCAP_HID_TEXT.
 *
 * read_text semantics (nonblocking):
 *   - returns 0 when nothing is available;
 *   - a positive count of bytes copied to `dst`, at most `max_len`;
 *   - a negative error (`ZDEV_EINVAL` for null/invalid arguments).
 *   Every byte is delivered exactly as the MCU translated it: NUL and
 *   control bytes are data, and multi-byte VT100 sequences arrive as
 *   their individual bytes in order, never rearranged or filtered.
 *
 * The companion character operations follow the generic conventions in
 * device.h: `getc` returns the oldest byte or ZDEV_EAGAIN, `poll`
 * returns 1 when a byte is available and 0 otherwise.
 */

#ifndef ZEPHYROS_HID_H
#define ZEPHYROS_HID_H

#include "zephyros/types.h"
#include "zephyros/errno.h"
#include "zephyros/device.h"

/* Reserved key-event shape for ZCAP_HID_KEY_EVENTS (not implemented in
 * M8).  `pressed` is nonzero for a press and zero for a release. */
struct zhid_key_event {
	uint8_t usage;     /* translated HID usage identifier       */
	uint8_t modifiers; /* modifier bitmap at event time         */
	uint8_t pressed;   /* nonzero = press, zero = release       */
	uint8_t reserved;  /* zero; keeps the shape stable          */
};

/* Reserved gamepad shape for ZCAP_HID_GAMEPAD (not implemented in M8).
 * Axis values are signed; the neutral center is zero. */
struct zhid_gamepad_state {
	uint8_t buttons;   /* button bitmap                          */
	uint8_t reserved;  /* zero; keeps the shape stable           */
	int8_t x;
	int8_t y;
};

/* Class operations, in the architecture's fixed order.  Either pointer
 * may be null; the matching capability bit states whether the operation
 * is available, and the others are never called. */
struct zhid_ops {
	int (*read_text)(void *state, uint8_t *dst, uint16_t max_len);
	int (*read_gamepad)(void *state, uint8_t index,
			    struct zhid_gamepad_state *out);
};

#if defined(__SDCC)
/* Pin the reserved shapes and the operation table layout so a
 * compiler/flag drift cannot silently break the class ABI. */
_Static_assert(sizeof(struct zhid_key_event) == 4,
	       "zhid_key_event must be 4 packed bytes");
_Static_assert(sizeof(struct zhid_gamepad_state) == 4,
	       "zhid_gamepad_state must be 4 packed bytes");
_Static_assert(sizeof(struct zhid_ops) == 4,
	       "zhid_ops must be 4 packed bytes");
_Static_assert(__builtin_offsetof(struct zhid_ops, read_gamepad) == 2,
	       "zhid_ops read_gamepad offset");
#endif /* __SDCC */

#endif /* ZEPHYROS_HID_H */
