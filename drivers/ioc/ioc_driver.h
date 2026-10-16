/* Internal interface of the IOC driver implementation (drivers/ioc/).
 *
 * ioc.c owns the command state, framing, CRC, and validation;
 * ioc_io.asm owns all SIO1/B port I/O and the timing-critical streaming
 * loops; ioc_boot.c owns the boot segment's one-time lane programming and
 * ioc_proto.c the resident-driver protocol dispatch.  Not a kernel
 * ABI.
 */

#ifndef KONAOS_DRIVERS_IOC_DRIVER_H
#define KONAOS_DRIVERS_IOC_DRIVER_H

#include "ioc.h"
#include "driver.h"
#include "kos/types.h"

/* One-time SIO1/B (command) and SIO1/A (bulk) programming: channel reset,
 * WR1 interrupts/WAIT off, External Sync x1, FFh/AAh fill, RX disabled
 * (command) / enabled without Hunt (bulk), TX idle.  Never called during
 * an ordinary transaction. */
void ioc_hw_init(void);

/* Bounded CMD_LINK_SYNC bring-up.  Each attempt clears BOTH lane sync
 * flags and re-arms local Hunt so the MCU emits a fresh /SYNC edge;
 * success requires a CRC-verified LINK_SYNC reply with the matching
 * sequence.  Also the explicit whole-link recovery path.  Returns
 * KIOC_OK or KIOC_ETIMEDOUT. */
int ioc_link_bringup(void);

/* One command request as the exchange reads it: the header fields and
 * where its DATA already is (the IPC frame's payload), so the DATA is
 * copied once, into the wire packet. */
struct ioc_request {
	uint8_t type;
	uint8_t status;
	uint8_t length;
	const uint8_t *data;
};

/* The class operation implementations (exposed for the descriptor and
 * for focused host tests).  The reply is written in place: the caller
 * hands over the frame it publishes. */
int ioc_command(void *state, const struct ioc_request *request,
		struct kioc_frame *reply);
int ioc_bulk_read(void *state, void *dst, uint16_t length);
/* The length of the MCU-to-Z80 bulk phase the last reply admitted, 0 when
 * none is waiting (KIOC_OP_COMMAND_READ runs it in the same call). */
uint16_t ioc_admitted_read(void *state);
int ioc_bulk_write(void *state, const void *src, uint16_t length);
uint16_t ioc_status(void *state);

/* Boot segment: one-time lane programming, run through the mode-10 gate. */
void ioc_boot_entry(void);

/* Hot segment: the endpoint dispatch and the init entry the loader calls
 * with the driver bootstrap table (which runs the one LINK_SYNC
 * bring-up and records the readiness bit). */
int16_t ioc_driver_dispatch(struct kos_ipc_frame *frame);
uint8_t ioc_driver_init(const struct driver_boot *boot);

extern struct ioc_state ioc_state;

#if KOS_IOC_PHASES
/* Read-path phase instrument (drivers/include/ioc_phases.h): the IOC
 * driver's own table and its address, which is published in the
 * KIOC_OP_COMMAND reply's payload[5..6] so the IOC-FAT driver can follow
 * it.  The bulk receive's three stamps are inline assembly in
 * ioc_io.asm, and their validity bits are set by ioc.c once a transfer
 * has validated.  Nothing here exists outside a measurement build. */
#include "kos/protocol.h"
extern struct kfile_phases ioc_phases;
#endif

#endif /* KONAOS_DRIVERS_IOC_DRIVER_H */
