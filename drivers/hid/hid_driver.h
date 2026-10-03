/* Internal interface of the HID driver implementation (drivers/hid/).
 *
 * hid.c owns the local ring, the fetch-on-doorbell engine, and the
 * stuck-doorbell penalty; hid_module.c owns the zdevice descriptor and
 * the module init entry.  The module discovers the IOC class through
 * the passed `zdriver_api` only -- no imported IOC-module symbol and no
 * registry lookup by name.  Not a kernel ABI.
 */

#ifndef ZEPHYROS_DRIVERS_HID_DRIVER_H
#define ZEPHYROS_DRIVERS_HID_DRIVER_H

#include "zephyros/device.h"
#include "zephyros/driver.h"
#include "zephyros/hid.h"
#include "zephyros/ioc.h"

/* Local translated-input ring: power-of-two size so the wraparound is a
 * mask, matching the proven MCU-side queue discipline. */
#define ZHID_RING_SIZE 16u

/* Console service calls to ignore the semantic doorbell after a fetch
 * that produced no data (the proven stuck-doorbell guard). */
#define ZHID_STUCK_PENALTY 64u

struct zhid_state {
	const struct zioc_ops *ioc; /* discovered IOC class operations */
	void *ioc_ctx;              /* opaque IOC instance state        */
	uint8_t head;               /* ring produce index               */
	uint8_t tail;               /* ring consume index               */
	uint8_t count;              /* bytes currently buffered         */
	uint8_t penalty;            /* stuck-doorbell countdown         */
	uint8_t ring[ZHID_RING_SIZE];
};

/* Discovery + state setup; used by the module init entry.  Verifies the
 * IOC dependency through device_find()/device_interface() only and
 * returns ZDRIVER_OK or ZDRIVER_FAIL.  Nothing is published here. */
int hid_driver_init(const struct zdriver_api *api);

/* The class/character operation implementations (exposed for the
 * descriptor table). */
int hid_read_text(void *state, uint8_t *dst, uint16_t max_len);
int hid_poll(void *state);
int hid_getc(void *state);

extern const struct zhid_ops hid_ops;
extern const struct zchar_ops hid_char_ops;
extern struct zhid_state hid_state;

#endif /* ZEPHYROS_DRIVERS_HID_DRIVER_H */
