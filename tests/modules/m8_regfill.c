/* ZephyrOS M8 arena-exhaustion fixture (test artifact).
 *
 * Registers no devices and claims no IRQs.  Its BSS-only image is larger
 * than the free production driver arena that remains after the SIO, IOC
 * and HID residents, so a fault-injection ROMFS manifest can load it in
 * place of `v9958.zmod` and drive the loader's allocation-failure path
 * (ZMOD_ENOSPC) deterministically.  The module image itself is tiny, so
 * the exhaustion is purely an allocation failure, exactly like a real
 * oversized module arriving when the arena is already nearly full.
 *
 * The init is never reached when the allocation fails; it exists only
 * to keep the container a structurally valid driver module.
 */

#include "zephyros/driver.h"

/* 36 granules (18432 bytes) of BSS plus the small image: more granules
 * than the whole derived production arena declares, so the allocation
 * can never succeed even if the earlier residents shrink in a future
 * geometry. */
uint8_t m8_regfill_pad[0x4800];

int m8_regfill_init(const struct zdriver_api *api)
{
	(void)api;
	return ZDRIVER_OK;
}
