/* Internal interface of the SIO0/B driver implementation (drivers/sio/).
 *
 * sio.c owns the bounded polling operations and the private state;
 * sio_boot.c owns the one-time 115200 8N1 programming sequence;
 * sio_proto.c owns the serial-protocol dispatch (abi/kos/protocol.h).
 * The image is packed as the `sio` resident driver (work package 41).
 * Not a kernel ABI.
 */

#ifndef KONAOS_DRIVERS_SIO_H
#define KONAOS_DRIVERS_SIO_H

#include "driver.h"
#include "kos/types.h"

/* Boot segment: re-establish the documented 115200 8N1 programming
 * sequence; called once, through the mode-10 boot gate. */
void sio0b_hw_init(void);

/* Hot segment: the endpoint dispatch and the init entry the loader calls
 * with the driver bootstrap table. */
int16_t sio_driver_dispatch(struct kos_ipc_frame *frame);
uint8_t sio_driver_init(const struct driver_boot *boot);

int sio0b_putc(void *state, char ch);
int sio0b_write(void *state, const char *buf, uint16_t len);
int sio0b_poll(void *state);
int sio0b_getc(void *state);

/* Driver-private state (DRIVER_IO data of the image). */
struct sio0b_state {
	uint16_t tx_timeouts;
	uint16_t rx_reads;
};

extern struct sio0b_state sio0b_state;

#endif /* KONAOS_DRIVERS_SIO_H */
