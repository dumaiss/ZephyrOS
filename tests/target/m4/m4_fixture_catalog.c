/* ZephyrOS M4 target-test fixture catalog (test artifact).
 *
 * Defines the stage-07 loader-fixture catalog linked only into acceptance
 * images.  The production boot catalog (kernel/module/embedded.c) carries
 * only the real boot module; the byte blobs for the fixtures are
 * generated at build time into $(BUILD)/m4_fixture_blobs.inc by
 * tools/zmodpack.py.
 */

#include "zephyros/module.h"

#include "m4_selftest.h"

#include "m4_fixture_blobs.inc"

extern const uint8_t z4_fixture_probe[];
extern const uint8_t z4_fixture_fail[];
extern const uint8_t z4_fixture_filler[];

const struct zmod_catalog_entry zmod_fixture_catalog[] = {
	{ ZOS_M4ST_CATALOG_PROBE, z4_fixture_probe,
	  Z4_FIXTURE_PROBE_SIZE },
	{ ZOS_M4ST_CATALOG_FAIL, z4_fixture_fail,
	  Z4_FIXTURE_FAIL_SIZE },
	{ ZOS_M4ST_CATALOG_FILLER, z4_fixture_filler,
	  Z4_FIXTURE_FILLER_SIZE },
	{ (const char *)0, (const uint8_t *)0, 0 }
};
