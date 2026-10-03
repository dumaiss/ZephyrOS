/* ZephyrOS SIO IRQ self-test ZMOD entry (test artifact).
 *
 * Module entry for the `sio-irq` acceptance image.  It registers the real
 * `sio0.b` device and then exercises the M2 IRQ executive from inside the
 * loaded module: it claims CTC0 through the module API with a relocated
 * in-image handler, counts repeated deliveries, and releases the source.
 *
 * The production module (drivers/sio/sio_module.c) contains none of this
 * logic; this entry is packed only into build/modules/sio-irq.zmod.
 */

#include "zephyros/driver.h"
#include "zephyros/platform.h"

#include "sio.h"

#define ZOS_SIO_IRQ_TARGET_HITS 3u

static const struct zdevice_desc sio0b_desc = {
	ZDEV_SERIAL,
	ZCAP_CHAR_IN | ZCAP_CHAR_OUT | ZCAP_CONSOLE_IN | ZCAP_CONSOLE_OUT,
	"sio0.b",
	&sio0b_ops,
	(const void *)0,
	&sio0b_state
};

/* Module-private interrupt-selftest state (BSS, relocated with the
 * module).  Non-static so the packer report can publish the offsets and
 * host replays can inspect them. */
volatile uint16_t sio_module_irq_hits;
volatile uint16_t sio_module_irq_state_addr;

/* Registered handler.  The M2 executive maps bank 7 and owns RETI; this
 * body only touches relocated module state and returns normally. */
void sio_module_irq_handler(void *context)
{
	(void)context;
	++sio_module_irq_hits;
}

/* Bounded interrupt wait.  The kernel starts CTC0 with the M2 lock held
 * before the module is loaded, so the first tick can only be dispatched
 * after this init has claimed the source (an unowned dispatch would
 * quiesce CTC0 and stop the timer).  Each loop iteration re-arms the CPU
 * because the M2 executive deliberately returns with maskable interrupts
 * disabled (no nesting).  The relocated handler must be reached
 * repeatedly and must update the relocated BSS counter, otherwise init
 * fails. */
static int sio_irq_exercise(const struct zdriver_api *api)
{
	uint16_t spins;

	sio_module_irq_hits = 0;
	sio_module_irq_state_addr = (uint16_t)&sio_module_irq_hits;

	if (api->irq_claim(IRQ_CTC0, sio_module_irq_handler, (void *)0) !=
	    IRQ_OK) {
		return -1;
	}
	/* One-owner rule: a second claim of the same source is refused. */
	if (api->irq_claim(IRQ_CTC0, sio_module_irq_handler, (void *)0) !=
	    IRQ_EBUSY) {
		return -1;
	}
	for (spins = 0; spins < 0x8000u &&
	     sio_module_irq_hits < ZOS_SIO_IRQ_TARGET_HITS; ++spins) {
		__asm__("ei");
	}
	__asm__("di");
	if (sio_module_irq_hits < ZOS_SIO_IRQ_TARGET_HITS) {
		return -1;
	}
	if (sio_module_irq_state_addr != (uint16_t)&sio_module_irq_hits) {
		return -1;
	}
	if (api->irq_release(IRQ_CTC0) != IRQ_OK) {
		return -1;
	}
	/* After release the executive must report the source unowned. */
	if (api->irq_release(IRQ_CTC0) != IRQ_ENOENT) {
		return -1;
	}
	return 0;
}

int sio_irq_module_init(const struct zdriver_api *api)
{
	sio0b_hw_init();

	if (api->device_register(&sio0b_desc, (zdev_handle_t *)0) !=
	    ZDRIVER_OK) {
		return ZDRIVER_FAIL;
	}
	if (sio_irq_exercise(api) != 0) {
		return ZDRIVER_FAIL;
	}
	return ZDRIVER_OK;
}
