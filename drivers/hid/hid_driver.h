/* Internal interface of the HID driver implementation (drivers/hid/).
 *
 * hid.c owns the local ring, the fetch-on-doorbell engine, the
 * stuck-doorbell penalty and the resident-service protocol dispatch
 * (abi/zos/protocol.h).  The engine discovers the IOC service through the
 * bootstrap interface only -- never an imported symbol and never a
 * registry lookup by name -- and declares that dependency in its image
 * metadata.  The whole driver is hot (no boot segment): its bring-up is
 * dependency discovery, which needs the bootstrap interface.
 * Not a kernel ABI.
 */

#ifndef ZEPHYROS_DRIVERS_HID_DRIVER_H
#define ZEPHYROS_DRIVERS_HID_DRIVER_H

#include "zephyros/ipc.h"
#include "zephyros/ioc.h"
#include "zos/types.h"

/* Local translated-input ring: power-of-two size so the wraparound is a
 * mask, matching the proven MCU-side queue discipline. */
#define ZHID_RING_SIZE 16u

/* Console service calls to ignore the semantic doorbell after a fetch
 * that produced no data (the proven stuck-doorbell guard). */
#define ZHID_STUCK_PENALTY 64u

/* Bounded READ_TEXT staging: the service I/O block the drained bytes are
 * copied out from (SERVICE_IO, the one fast copy range). */
#define ZHID_READ_BATCH 24u

/* Manifest dependency order (services/manifests/hid.json): position 0 is
 * the IOC transport the host resolved and wired. */
#define HID_DEP_IOC 0u

struct zhid_state {
	uint8_t ioc_handle; /* the IOC endpoint, 0 = absent/unready  */
	uint8_t head;       /* ring produce index                    */
	uint8_t tail;       /* ring consume index                    */
	uint8_t count;      /* bytes currently buffered              */
	uint8_t penalty;    /* stuck-doorbell countdown              */
	uint8_t ring[ZHID_RING_SIZE];
	uint8_t read_buffer[ZHID_READ_BATCH];
	uint16_t read_buffer_address;
};

/* The class operation implementations (exposed for the dispatch). */
int hid_read_text(void *state, uint8_t *dst, uint16_t max_len);
int hid_poll(void *state);
int hid_getc(void *state);

/* Hot segment: the endpoint dispatch and the init entry the loader calls
 * with the service bootstrap table. */
int16_t hid_service_dispatch(struct zos_ipc_frame *frame);
uint8_t hid_service_init(const struct ipc_service_boot *boot);

extern struct zhid_state hid_state;

#endif /* ZEPHYROS_DRIVERS_HID_DRIVER_H */
