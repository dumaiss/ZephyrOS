/* ZephyrOS M4 embedded boot module catalog.
 *
 * One null-terminated table is generated at build time (the byte blobs
 * come from tools/zmodpack.py; no handwritten byte arrays exist):
 *
 *   zmod_catalog   boot modules: exactly the packed SIO driver (or its
 *                  IRQ self-test / init-failure test variant in
 *                  purpose-built acceptance images).  The kernel boot
 *                  path loads these through module_catalog_load_all().
 *
 * The stage-07 loader-fixture catalog is a test artifact: it lives in
 * tests/target/m4/m4_fixture_catalog.c and is linked only into acceptance
 * images.
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
