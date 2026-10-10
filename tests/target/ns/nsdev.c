/* ZephyrOS namespace acceptance: the `nsdev` synthetic publisher (test
 * artifact).
 *
 * One resident bank-6 service whose publications give the DEVICE provider
 * a second and third protocol type and a second grouping container
 * (tests/target/include/ns_test.h).  The image boots the real SIO and
 * console services beside it, so the table it installs is the production
 * one plus these two test-only names.
 *
 * The service is a test double: its jobs are to be reachable through a
 * bound handle - `call` must arrive at this dispatch with the caller's
 * frame - and to answer deterministically.  It keeps no state and needs
 * no dependency.
 */

#include "zephyros/errno.h"
#include "zephyros/ipc.h"

#include "ns_test.h"

/* Calls served, in the service's own SERVICE_IO area (bank 6's
 * 8000h-BFFFh, where a nucleus copy can reach it): the replay reads it
 * back to prove the bound-handle calls really arrived here. */
uint16_t nsdev_calls;

/* The endpoint dispatch entry the manifest names: a module-global symbol
 * so the packer can resolve its hot-image offset. */
int16_t nsdev_dispatch(struct zos_ipc_frame *frame)
{
	nsdev_calls = (uint16_t)(nsdev_calls + 1u);
	switch (frame->op) {
	case NS_OP_PING:
		/* Echo the caller's word back: the reply frame proves the
		 * call carried the caller's own payload here. */
		frame->payload[2] = frame->payload[0];
		frame->payload[3] = frame->payload[1];
		return (int16_t)NS_MAGIC;
	case NS_OP_WHOAMI:
		return (int16_t)NS_MAGIC;
	case NS_OP_DESC:
		return (int16_t)frame->desc;
	default:
		return -ZOS_EINVAL;
	}
}

uint8_t nsdev_init(const struct ipc_service_boot *boot)
{
	(void)boot;
	return 0u;
}
