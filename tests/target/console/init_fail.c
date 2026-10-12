/* KonaOS required-driver init-failure acceptance driver (work package
 * 45, test artifact).
 *
 * The one job of this resident bank-6 image is to refuse: its init
 * returns a non-zero status, so the loader must fail the boot explicitly
 * on the early console instead of falling back (architecture 36.11,
 * section 37 "required-driver initialization failure is explicit
 * boot/configuration failure").  It is packed into its own failure store
 * beside `drivers.man`; nothing else in the tree loads it.
 *
 * The hot segment exists because every driver image needs one, and the
 * `_DRIVER_IO` byte gives the segment its declared BSS area.  There is
 * no endpoint, no publication and no dependency: the failure happens
 * before any of those could matter.
 */

#include "driver.h"

uint8_t init_fail_marker;

uint8_t init_fail_init(const struct driver_boot *boot)
{
	(void)boot;
	return 1u;
}
