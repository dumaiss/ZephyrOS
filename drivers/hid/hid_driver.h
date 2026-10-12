/* Internal interface of the HID driver implementation (drivers/hid/).
 *
 * hid.c owns the local ring, the fetch-on-doorbell engine, the
 * stuck-doorbell penalty and the resident-driver protocol dispatch
 * (abi/kos/protocol.h).  The engine discovers the IOC driver through the
 * bootstrap interface only -- never an imported symbol and never a
 * registry lookup by name -- and declares that dependency in its image
 * metadata.  The whole driver is hot (no boot segment): its bring-up is
 * dependency discovery, which needs the bootstrap interface.
 * Not a kernel ABI.
 */

#ifndef KONAOS_DRIVERS_HID_DRIVER_H
#define KONAOS_DRIVERS_HID_DRIVER_H

#include "driver.h"
#include "ioc.h"
#include "kos/types.h"

/* Local translated-input ring: power-of-two size so the wraparound is a
 * mask, matching the proven MCU-side queue discipline. */
#define KHID_RING_SIZE 16u

/* Console driver calls to ignore the semantic doorbell after a fetch
 * that produced no data (the proven stuck-doorbell guard). */
#define KHID_STUCK_PENALTY 64u

/* Bounded READ_TEXT staging: the driver I/O block the drained bytes are
 * copied out from (DRIVER_IO, the one fast copy range). */
#define KHID_READ_BATCH 24u

/* Manifest dependency order (drivers/hid/hid.json): position 0 is
 * the IOC transport the host resolved and wired. */
#define HID_DEP_IOC 0u

struct khid_state {
	uint8_t ioc_handle; /* the IOC endpoint, 0 = absent/unready  */
	uint8_t head;       /* ring produce index                    */
	uint8_t tail;       /* ring consume index                    */
	uint8_t count;      /* bytes currently buffered              */
	uint8_t penalty;    /* stuck-doorbell countdown              */
	uint8_t ring[KHID_RING_SIZE];
	uint8_t read_buffer[KHID_READ_BATCH];
	uint16_t read_buffer_address;
};

/* The class operation implementations (exposed for the dispatch). */
int hid_read_text(void *state, uint8_t *dst, uint16_t max_len);
int hid_poll(void *state);
int hid_getc(void *state);

/* Hot segment: the endpoint dispatch and the init entry the loader calls
 * with the driver bootstrap table. */
int16_t hid_driver_dispatch(struct kos_ipc_frame *frame);
uint8_t hid_driver_init(const struct driver_boot *boot);

extern struct khid_state hid_state;

#endif /* KONAOS_DRIVERS_HID_DRIVER_H */
