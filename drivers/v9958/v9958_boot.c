/* KonaOS V9958 driver boot segment.
 *
 * Runs once through the mode-10 gate with Q = 6 (architecture 5.2): the
 * whole resident bank is mapped, the fixed core is not, and the driver
 * bootstrap interface is unavailable.  Nothing here executes after boot.
 *
 * The boot segment performs the initial display bring-up and records the
 * boot-once marker the acceptance replay reads back.  The run-time
 * display reset re-runs the same hot chain.
 */

#include "kos/protocol.h"

#include "v9958_driver.h"

volatile uint8_t v9958_boot_runs;

void v9958_boot_entry(void)
{
	v9958_boot_runs = 1u;
	(void)v9958_display_bringup();
}
