/* ZephyrOS M4 loader test fixture: relocatable driver probe.
 *
 * The same packed bytes are loaded twice by the M4 self-test.  The first
 * load lands on an even 512-byte granule, the second on an odd one; the
 * module chooses its device name from bit 9 of its own (relocated)
 * private-state address, so both loads initialize successfully while the
 * kernel can verify that the registered state/ops/name pointers lie
 * inside the allocation and that the state pointer equals
 * load_base + image_size exactly.
 *
 * The even-granule instance also claims IRQ_CTC0 through the driver API
 * with a handler inside its own image, proving that init-time claims from
 * a LOADING module are accepted with the module as owner.
 *
 * This fixture must not use stack-built descriptors or other constructs
 * that produce byte-fragmented relocations; the packer rejects those.
 */

#include "zephyros/device.h"
#include "zephyros/driver.h"
#include "zephyros/irq.h"

struct m4_probe_state {
	uint16_t magic;
};

/* First (and only) BSS object: its module-relative offset is 0, so the
 * registered state pointer must equal load_base + image_size. */
struct m4_probe_state m4_probe_state;

static int m4_probe_noop(void *state, char ch)
{
	(void)state;
	(void)ch;
	return ZDEV_OK;
}

static const struct zchar_ops m4_probe_ops = {
	m4_probe_noop,
	(int (*)(void *, const char *, uint16_t))0,
	(int (*)(void *))0,
	(int (*)(void *))0
};

static const struct zdevice_desc m4_probe_desc_a = {
	ZDEV_OTHER,
	0,
	"m4.probe.a",
	&m4_probe_ops,
	(const void *)0,
	&m4_probe_state
};

static const struct zdevice_desc m4_probe_desc_b = {
	ZDEV_OTHER,
	0,
	"m4.probe.b",
	&m4_probe_ops,
	(const void *)0,
	&m4_probe_state
};

static void m4_probe_irq(void *context)
{
	(void)context;
}

int m4_probe_init(const struct zdriver_api *api)
{
	/* Read the relocated state pointer out of the (word-relocated)
	 * descriptor rather than materializing `&m4_probe_state` in the
	 * instruction stream: the size optimizer otherwise narrows the
	 * address to a single byte, which is a byte-fragmented relocation
	 * the ZMOD format deliberately does not carry.  A volatile read
	 * keeps the full 16-bit value in the descriptor's `.dw` word. */
	uint16_t self =
		*(const volatile uint16_t *)&m4_probe_desc_a.state;

	/* BSS must have been zeroed by the loader before init runs. */
	if (m4_probe_state.magic != 0) {
		return ZDRIVER_FAIL;
	}
	m4_probe_state.magic = 0x4D31u;

	if (((self >> 9) & 1u) != 0) {
		return api->device_register(&m4_probe_desc_b,
					    (zdev_handle_t *)0);
	}
	if (api->device_register(&m4_probe_desc_a,
				 (zdev_handle_t *)0) != ZDRIVER_OK) {
		return ZDRIVER_FAIL;
	}
	if (api->irq_claim(IRQ_CTC0, m4_probe_irq, (void *)0) != IRQ_OK) {
		return ZDRIVER_FAIL;
	}
	return ZDRIVER_OK;
}
