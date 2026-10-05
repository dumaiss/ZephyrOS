/* ZephyrOS M8 device registry contract (driver ABI v3).
 *
 * This header and driver.h define the internal kernel/driver ABI:
 * driver, device, class, and capability concepts are ZephyrOS-internal
 * and are not a user syscall ABI.  Structure layout is pinned to the
 * driver ABI version; M9 is ABI v3.
 *
 * Model (see docs/devices.md):
 *   driver      implementation code for a hardware family
 *   device      one registered hardware instance
 *   capability  operations a device can provide
 *   service     OS-level binding that selects devices for a role
 *
 * Handles are kernel-issued, valid for one boot only, and never 0.
 * Handle 0 is the invalid handle.  Clients select devices by class and
 * capability queries, never by driver or instance name.
 *
 * The current ABI separates the optional generic character interface
 * (`struct zchar_ops`) from a class-specific interface (`class_ops`,
 * type determined by `class_id`); a device may expose either or both.
 * Capabilities are authoritative: unsupported class operations fail
 * predictably instead of being inferred from the device name.  M8 also
 * refines the two M3 placeholder class names while keeping their
 * numeric slots: ZDEV_INPUT -> ZDEV_HID, ZDEV_TRANSPORT -> ZDEV_IOC.
 */

#ifndef ZEPHYROS_DEVICE_H
#define ZEPHYROS_DEVICE_H

#include "zos/types.h"
#include "zephyros/errno.h"

typedef uint8_t zdev_handle_t;
typedef uint32_t zdev_caps_t;

#define ZDEV_HANDLE_INVALID 0u
#define ZDEV_MAX_DEVICES    16u

/* Device classes (small fixed registry identifiers; stable slots). */
enum {
	ZDEV_SERIAL = 1,
	ZDEV_VIDEO,
	ZDEV_STORAGE,
	ZDEV_HID,
	ZDEV_IOC,
	ZDEV_OTHER
};

/* Class filter accepted by device_find(): match any class. */
#define ZDEV_CLASS_ANY 0u

/* Capability bits (32-bit mask).  Lookup requires every queried bit to
 * be present.  Generic character/console bits occupy 0-3; each device
 * class owns a documented bit range so new classes cannot collide. */
#define ZCAP_CHAR_IN     0x0001u
#define ZCAP_CHAR_OUT    0x0002u
#define ZCAP_CONSOLE_IN  0x0004u
#define ZCAP_CONSOLE_OUT 0x0008u

/* VIDEO class capabilities: bits 8-14. */
#define ZCAP_VIDEO_CTRL_WRITE 0x00000100u
#define ZCAP_VIDEO_DATA_WRITE 0x00000200u
#define ZCAP_VIDEO_DATA_BLOCK 0x00000400u
#define ZCAP_VIDEO_PALETTE    0x00000800u
#define ZCAP_VIDEO_INDIRECT   0x00001000u
#define ZCAP_VIDEO_PRESENT    0x00002000u
#define ZCAP_VIDEO_RESET      0x00004000u

/* HID class capabilities: bits 16-18. */
#define ZCAP_HID_TEXT       0x00010000u
#define ZCAP_HID_KEY_EVENTS 0x00020000u
#define ZCAP_HID_GAMEPAD    0x00040000u

/* IOC class capabilities: bits 24-27. */
#define ZCAP_IOC_COMMAND    0x01000000u
#define ZCAP_IOC_BULK_READ  0x02000000u
#define ZCAP_IOC_BULK_WRITE 0x04000000u
#define ZCAP_IOC_STATUS     0x08000000u

/* Internal status convention: 0 = success, negative = -ZOS_Exxx. */
#define ZDEV_OK        0
#define ZDEV_EINVAL    (-ZOS_EINVAL)
#define ZDEV_ENODEV    (-ZOS_ENODEV)
#define ZDEV_EBUSY     (-ZOS_EBUSY)
#define ZDEV_ENOENT    (-ZOS_ENOENT)
#define ZDEV_ENOSPC    (-ZOS_ENOSPC)
#define ZDEV_ENOTSUP   (-ZOS_ENOTSUP)
#define ZDEV_ETIMEDOUT (-ZOS_ETIMEDOUT)
#define ZDEV_EAGAIN    (-ZOS_EAGAIN)

/* Generic character-device operations (optional interface).
 * Drivers own the implementation and the instance state; the registry
 * only stores the pointers.
 *
 * Return conventions:
 *   putc   ZDEV_OK, or a negative error (ZDEV_ETIMEDOUT when the bounded
 *          TX-ready poll expired).
 *   write  number of bytes written (0..len), or a negative error when no
 *          byte could be written.
 *   poll   1 = input ready, 0 = no input, negative = error.
 *   getc   byte value 0..255, ZDEV_EAGAIN when no input is pending, or a
 *          negative error.
 */
struct zchar_ops {
	int (*putc)(void *state, char ch);
	int (*write)(void *state, const char *buf, uint16_t len);
	int (*poll)(void *state);
	int (*getc)(void *state);
};

/* Registration input.  The registry copies the metadata it needs for
 * integrity; name/char_ops/class_ops/state remain owned by the driver.
 * `char_ops` and `class_ops` are each optional, but at least one must be
 * present; `class_ops` points at a class-specific operations structure
 * whose type is determined by `class_id`. */
struct zdevice_desc {
	uint8_t class_id;
	zdev_caps_t capabilities;
	const char *name;
	const struct zchar_ops *char_ops;
	const void *class_ops;
	void *state;
};

/* Registry entry (read-only for clients). */
struct zdevice {
	zdev_handle_t handle;
	uint8_t class_id;
	zdev_caps_t capabilities;
	const char *name;
	const struct zchar_ops *char_ops;
	const void *class_ops;
	void *state;
	const void *driver;
};

#if defined(__SDCC)
/* The registry copies these structures field by field; pin the packed
 * ABI layout so a compiler/flag drift cannot silently break it. */
_Static_assert(sizeof(struct zchar_ops) == 8, "zchar_ops must be 8 bytes");
_Static_assert(__builtin_offsetof(struct zchar_ops, poll) == 4,
	       "zchar_ops poll offset");
_Static_assert(sizeof(struct zdevice_desc) == 13,
	       "zdevice_desc must be 13 packed bytes");
_Static_assert(__builtin_offsetof(struct zdevice_desc, capabilities) == 1,
	       "zdevice_desc capabilities offset");
_Static_assert(__builtin_offsetof(struct zdevice_desc, char_ops) == 7,
	       "zdevice_desc char_ops offset");
_Static_assert(__builtin_offsetof(struct zdevice_desc, class_ops) == 9,
	       "zdevice_desc class_ops offset");
_Static_assert(__builtin_offsetof(struct zdevice_desc, state) == 11,
	       "zdevice_desc state offset");
_Static_assert(sizeof(struct zdevice) == 16,
	       "zdevice must be 16 packed bytes");
_Static_assert(__builtin_offsetof(struct zdevice, capabilities) == 2,
	       "zdevice capabilities offset");
_Static_assert(__builtin_offsetof(struct zdevice, char_ops) == 8,
	       "zdevice char_ops offset");
_Static_assert(__builtin_offsetof(struct zdevice, class_ops) == 10,
	       "zdevice class_ops offset");
_Static_assert(__builtin_offsetof(struct zdevice, state) == 12,
	       "zdevice state offset");
_Static_assert(__builtin_offsetof(struct zdevice, driver) == 14,
	       "zdevice driver offset");
#endif /* __SDCC */

/* Read-only discovery.  Returns the first registered device at or after
 * class/capability cursor `after` (0 = start of the registry) whose class
 * matches `class_id` (ZDEV_CLASS_ANY = any class) and whose capability
 * mask contains every bit of `required_caps`.  A zero `required_caps`, an
 * invalid class, or an out-of-range cursor yields ZDEV_HANDLE_INVALID;
 * there is deliberately no registry mutation interface. */
zdev_handle_t device_find(uint8_t class_id, zdev_caps_t required_caps,
			  zdev_handle_t after);

/* Read-only class interface access.  Verifies the handle is live, the
 * device class equals `expected_class`, and the class interface is
 * present; on success copies the driver-owned class ops/state pointers.
 * Errors: ZDEV_EINVAL (null out pointers, invalid handle/class),
 * ZDEV_ENODEV (stale handle: slot no longer registered), ZDEV_ENOTSUP
 * (class mismatch or no class interface). */
int device_interface(zdev_handle_t handle, uint8_t expected_class,
		     const void **class_ops, void **state);

/* The registry lifecycle, the registration transaction (checkpoint,
 * rollback, owner), and the bank-7 registry storage are kernel-private
 * and live in kernel/device/registry.h.  A ZMOD module publishes a
 * device only through the loader's `zdriver_api.device_register`. */

#endif /* ZEPHYROS_DEVICE_H */
