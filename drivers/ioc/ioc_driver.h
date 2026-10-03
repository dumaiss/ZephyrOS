/* Internal interface of the IOC driver implementation (drivers/ioc/).
 *
 * ioc.c owns the command state, framing, CRC, and validation;
 * ioc_io.asm owns all SIO1/B port I/O and the timing-critical streaming
 * loops; ioc_module.c owns the zdevice descriptor and the module init
 * entry.  Not a kernel ABI.
 */

#ifndef ZEPHYROS_DRIVERS_IOC_DRIVER_H
#define ZEPHYROS_DRIVERS_IOC_DRIVER_H

#include "zephyros/ioc.h"

/* One-time SIO1/B (command) and SIO1/A (bulk) programming: channel reset,
 * WR1 interrupts/WAIT off, External Sync x1, FFh/AAh fill, RX disabled
 * (command) / enabled without Hunt (bulk), TX idle.  Never called during
 * an ordinary transaction. */
void ioc_hw_init(void);

/* Bounded CMD_LINK_SYNC bring-up.  Each attempt clears BOTH lane sync
 * flags and re-arms local Hunt so the MCU emits a fresh /SYNC edge;
 * success requires a CRC-verified LINK_SYNC reply with the matching
 * sequence.  Also the explicit whole-link recovery path.  Returns
 * ZIOC_OK or ZIOC_ETIMEDOUT. */
int ioc_link_bringup(void);

/* The class operation implementations (exposed for the descriptor and
 * for focused host tests). */
int ioc_command(void *state, const struct zioc_frame *request,
		struct zioc_frame *reply);
int ioc_bulk_read(void *state, void *dst, uint16_t length);
int ioc_bulk_write(void *state, const void *src, uint16_t length);
uint16_t ioc_status(void *state);

extern const struct zioc_ops ioc_ops;
extern struct ioc_state ioc_state;

#endif /* ZEPHYROS_DRIVERS_IOC_DRIVER_H */
