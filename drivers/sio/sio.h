/* Internal interface of the SIO0/B driver implementation (drivers/sio/).
 *
 * sio.c owns the bounded polling operations and the private state;
 * sio_boot.c owns the one-time 115200 8N1 programming sequence;
 * sio_proto.c owns the serial-protocol dispatch (abi/kos/protocol.h).
 * The image is packed as the `sio` resident driver.
 * Not a kernel ABI.
 */

#ifndef KONAOS_DRIVERS_SIO_H
#define KONAOS_DRIVERS_SIO_H

#include "driver.h"
#include "kos/types.h"

/* Boot segment: the channel reset and the documented 115200 8N1
 * programming sequence; called once, through the mode-10 boot gate. */
void sio0b_hw_init(void);

/* Hot segment (sio_io.asm): the channel's setup after its reset - WR4,
 * WR3, WR5 with RTS asserted, WR1 - which the boot runs and the end of
 * an exclusive hold rewrites; and that restore, the setup followed by a
 * bounded drain of the receiver. */
void sio0b_setup(void);
void sio0b_restore(void);

/* Hot segment: the endpoint dispatch and the init entry the loader calls
 * with the driver bootstrap table. */
int16_t sio_driver_dispatch(struct kos_ipc_frame *frame);
uint8_t sio_driver_init(const struct driver_boot *boot);

int sio0b_putc(void *state, char ch);
int sio0b_write(void *state, const char *buf, uint16_t len);
int sio0b_poll(void *state);
int sio0b_getc(void *state);

/* Driver-private state (DRIVER_IO data of the image).  `exclusive` is
 * 1 while a program holds the port; `dropped` counts the bytes PUTC and
 * WRITE answered without sending during the latest hold, the hold's
 * evidence rather than a protocol answer. */
struct sio0b_state {
	uint16_t dropped;
	uint8_t exclusive;
};

extern struct sio0b_state sio0b_state;

#endif /* KONAOS_DRIVERS_SIO_H */
