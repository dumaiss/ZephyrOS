/* ZephyrOS embedded boot-module catalog (acceptance images only).
 *
 * One null-terminated table is generated at build time (the byte blobs
 * come from tools/zmodpack.py; no handwritten byte arrays exist):
 *
 *   zmod_catalog   boot modules: the packed SIO driver or its IRQ
 *                  self-test / init-failure test variant.
 *
 * M5 moved the production boot path to the ROMFS recovery store
 * (romfs_load_zmod); this embedded catalog and its loader remain the
 * acceptance-image source for the M3/M4 milestone images, which preserve
 * their original embedded-catalog behavior.  Production images do not
 * link this file at all -- tools/mapcheck.py --production rejects
 * embedded boot blob/catalog symbols.
 *
 * The stage-07 loader-fixture catalog is a separate test artifact:
 * tests/target/m4/m4_fixture_catalog.c.
 *
 * The loader itself makes no filesystem assumptions and never inspects
 * the catalog names; they exist for diagnostics and for test code that
 * locates a specific entry.
 */

#include "zephyros/module.h"

#include "m4_catalog.inc"

/* The generated blob symbol always carries the boot module for this
 * image; the Makefile selects sio.zmod, sio-irq.zmod or sio-fail.zmod
 * when generating the assembly. */
extern const uint8_t z4_module_boot[];

#define ZOS_M4_BOOT_MODULE_NAME "sio"

const struct zmod_catalog_entry zmod_catalog[] = {
	{ ZOS_M4_BOOT_MODULE_NAME, z4_module_boot, Z4_MODULE_BOOT_SIZE },
	{ (const char *)0, (const uint8_t *)0, 0 }
};

uint8_t module_catalog_load_all(void)
{
	const struct zmod_catalog_entry *entry = zmod_catalog;
	uint8_t failed = 0;

	while (entry->name != (const char *)0) {
		if (module_load(entry->name, entry->blob, entry->size) < 0) {
			++failed;
		}
		++entry;
	}
	return failed;
}
