/* ZephyrOS M3 device registry contract.
 *
 * This header and driver.h define the internal kernel/driver ABI:
 * driver, device, class, and capability concepts are ZephyrOS-internal
 * and are not a user syscall ABI.  Structure layout may evolve with the
 * driver ABI version.
 *
 * Model (see docs/devices.md):
 *   driver      implementation code for a hardware family
 *   device      one registered hardware instance
 *   capability  operations a device can provide
 *   service     OS-level binding that selects devices for a role
 *
 * Handles are kernel-issued, valid for one boot only, and never 0.
 * Handle 0 is the invalid handle.  Clients select devices by capability
 * queries, not by driver or instance name.
 */

#ifndef ZEPHYROS_DEVICE_H
#define ZEPHYROS_DEVICE_H

#include "zephyros/types.h"
#include "zephyros/errno.h"

typedef uint8_t zdev_handle_t;

#define ZDEV_HANDLE_INVALID 0u
#define ZDEV_MAX_DEVICES    16u

/* Device classes (small fixed registry identifiers). */
enum {
	ZDEV_SERIAL = 1,
	ZDEV_VIDEO,
	ZDEV_STORAGE,
	ZDEV_INPUT,
	ZDEV_TRANSPORT,
	ZDEV_OTHER
};

/* Capability bits.  Lookup requires all bits of a query to be present. */
#define ZCAP_CHAR_IN     0x0001u
#define ZCAP_CHAR_OUT    0x0002u
#define ZCAP_CONSOLE_IN  0x0004u
#define ZCAP_CONSOLE_OUT 0x0008u

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

/* Character-device operations (M3 has one shared operation table shape).
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
struct zdevice_ops {
	int (*putc)(void *state, char ch);
	int (*write)(void *state, const char *buf, uint16_t len);
	int (*poll)(void *state);
	int (*getc)(void *state);
};

/* Registration input.  The registry copies the metadata it needs for
 * integrity; name/ops/state remain owned by the driver. */
struct zdevice_desc {
	uint8_t class_id;
	uint16_t capabilities;
	const char *name;
	const struct zdevice_ops *ops;
	void *state;
};

/* Registry entry (read-only for clients). */
struct zdevice {
	zdev_handle_t handle;
	uint8_t class_id;
	uint16_t capabilities;
	const char *name;
	const struct zdevice_ops *ops;
	void *state;
	const void *driver;
};

/* Registry lifecycle. */
void device_registry_init(void);
int device_register(const struct zdevice_desc *desc, zdev_handle_t *out_handle);
const struct zdevice *device_get(zdev_handle_t handle);
zdev_handle_t device_find_capability(uint16_t required_caps,
				     zdev_handle_t after);

/* Registration transaction boundaries used around each driver init (M4
 * reuses the same operations).  A checkpoint records the current
 * registration generation; rollback removes exactly the entries created
 * after that checkpoint and returns how many were removed.  Entries
 * registered earlier are never touched. */
typedef uint16_t zdev_checkpoint_t;
zdev_checkpoint_t device_registry_checkpoint(void);
int device_registry_rollback(zdev_checkpoint_t checkpoint);

/* Owner token recorded in entries registered while a driver init runs. */
void device_registry_set_owner(const void *owner);
const void *device_registry_owner(void);

/* Registry storage: bank-7 kernel data.  The symbol is intentionally
 * global so tools/mapcheck.py can prove bank-7 placement. */
extern struct zdevice device_table[ZDEV_MAX_DEVICES];

#endif /* ZEPHYROS_DEVICE_H */
